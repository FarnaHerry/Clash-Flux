#!/usr/bin/env bash
set -euo pipefail

version=""
release_dir="release"
pages_dir="pages"
expected_fingerprint=""

usage() {
  cat <<'EOF'
Usage:
  scripts/build-linux-repo.sh --version <ver> --expected-fingerprint <40-hex-fpr> [--release-dir <dir>] [--pages-dir <dir>]
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
  --version)
    version=${2:?--version requires a value}
    shift 2
    ;;
  --release-dir)
    release_dir=${2:?--release-dir requires a value}
    shift 2
    ;;
  --pages-dir)
    pages_dir=${2:?--pages-dir requires a value}
    shift 2
    ;;
  --expected-fingerprint)
    expected_fingerprint=${2:?--expected-fingerprint requires a value}
    shift 2
    ;;
  --help | -h)
    usage
    exit 0
    ;;
  *)
    echo "Unknown argument: $1" >&2
    exit 1
    ;;
  esac
done

version=${version#v}
[ -n "$version" ] || { echo "Missing --version" >&2; exit 1; }
[ -n "$expected_fingerprint" ] || { echo "Missing --expected-fingerprint" >&2; exit 1; }
[ -n "${CLASHFLUX_LINUX_GPG_PRIVATE_KEY:-}" ] || {
  echo "CLASHFLUX_LINUX_GPG_PRIVATE_KEY is required" >&2
  exit 1
}

for tool in gpg rpmsign rpm createrepo_c dpkg-scanpackages apt-ftparchive gzip awk; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "Required repository signing tool missing: $tool" >&2
    exit 1
  }
done

tmp_root=$(mktemp -d)
trap 'rm -rf "$tmp_root"' EXIT

export GNUPGHOME="$tmp_root/gnupg"
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"
printf 'allow-loopback-pinentry\n' >"$GNUPGHOME/gpg-agent.conf"
printf 'pinentry-mode loopback\n' >"$GNUPGHOME/gpg.conf"

pass_file="$tmp_root/passphrase"
umask 077
printf '%s' "${CLASHFLUX_LINUX_GPG_PASSPHRASE:-}" >"$pass_file"
chmod 600 "$pass_file"

printf '%s\n' "$CLASHFLUX_LINUX_GPG_PRIVATE_KEY" | gpg --batch --import
actual_fingerprint=$(gpg --list-secret-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}')
if [ "$actual_fingerprint" != "$expected_fingerprint" ]; then
  echo "GPG key fingerprint mismatch: expected $expected_fingerprint, got $actual_fingerprint" >&2
  exit 1
fi

gpg_sign_args=(--batch --yes --digest-algo SHA256)
if [ -n "${CLASHFLUX_LINUX_GPG_PASSPHRASE:-}" ]; then
  gpg_sign_args+=(--pinentry-mode loopback --passphrase-file "$pass_file")
fi

rm -rf "$pages_dir"
mkdir -p \
  "$pages_dir/apt/pool/main/c/clash-flux" \
  "$pages_dir/apt/dists/stable/main/binary-amd64" \
  "$pages_dir/apt/dists/stable/main/binary-arm64" \
  "$pages_dir/rpm/x86_64" \
  "$pages_dir/rpm/aarch64"

gpg --batch --yes --armor --export "$actual_fingerprint" >"$pages_dir/gpg.key"
gpg --batch --yes --export "$actual_fingerprint" >"$pages_dir/clash-flux.gpg"
test -s "$pages_dir/gpg.key"
test -s "$pages_dir/clash-flux.gpg"

if [ -f "platform/linux/clash-flux-repo.asc" ]; then
  cmp -s "$pages_dir/gpg.key" "platform/linux/clash-flux-repo.asc" || {
    echo "Exported GPG public key differs from platform/linux/clash-flux-repo.asc" >&2
    exit 1
  }
fi

rpmdb="$tmp_root/rpmdb"
mkdir -p "$rpmdb"
rpm --dbpath "$rpmdb" --initdb
rpm --dbpath "$rpmdb" --import "$pages_dir/gpg.key"

gpg_bin=$(command -v gpg)
if [ -n "${CLASHFLUX_LINUX_GPG_PASSPHRASE:-}" ]; then
  rpm_gpg_sign_cmd="$gpg_bin --batch --no-verbose --no-armor --pinentry-mode loopback --passphrase-file $pass_file --no-secmem-warning -u %{?_gpg_name} -sbo %{__signature_filename} --digest-algo sha256 %{__plaintext_filename}"
else
  rpm_gpg_sign_cmd="$gpg_bin --batch --no-verbose --no-armor --no-secmem-warning -u %{?_gpg_name} -sbo %{__signature_filename} --digest-algo sha256 %{__plaintext_filename}"
fi

for arch in x86_64 aarch64; do
  rpm_file="$release_dir/clash-flux-${version}-linux-${arch}.rpm"
  deb_file="$release_dir/clash-flux-${version}-linux-${arch}.deb"
  test -s "$rpm_file"
  test -s "$deb_file"

  rpmsign --addsign \
    --define '_signature gpg' \
    --define "_gpg_name $actual_fingerprint" \
    --define "__gpg $gpg_bin" \
    --define "__gpg_sign_cmd $rpm_gpg_sign_cmd" \
    "$rpm_file"

  check_out=$(rpm --dbpath "$rpmdb" --checksig "$rpm_file")
  printf '%s\n' "$check_out"
  printf '%s\n' "$check_out" | grep -q "signatures OK" || {
    echo "Signed RPM signature verification failed for $rpm_file" >&2
    exit 1
  }

  cp "$rpm_file" "$pages_dir/rpm/$arch/"
  createrepo_c --quiet "$pages_dir/rpm/$arch"
  gpg "${gpg_sign_args[@]}" --detach-sign --armor \
    -o "$pages_dir/rpm/$arch/repodata/repomd.xml.asc" \
    "$pages_dir/rpm/$arch/repodata/repomd.xml"
  gpg --batch --verify \
    "$pages_dir/rpm/$arch/repodata/repomd.xml.asc" \
    "$pages_dir/rpm/$arch/repodata/repomd.xml"

  deb_arch="amd64"
  if [ "$arch" = "aarch64" ]; then
    deb_arch="arm64"
  fi
  cp "$deb_file" "$pages_dir/apt/pool/main/c/clash-flux/clash-flux_${version}_${deb_arch}.deb"
done

(
  cd "$pages_dir/apt"
  dpkg-scanpackages --arch amd64 --multiversion pool/main >dists/stable/main/binary-amd64/Packages
  dpkg-scanpackages --arch arm64 --multiversion pool/main >dists/stable/main/binary-arm64/Packages
  grep -q '^Package: clash-flux$' dists/stable/main/binary-amd64/Packages || {
    echo "APT amd64 Packages missing clash-flux entry" >&2
    exit 1
  }
  grep -q '^Package: clash-flux$' dists/stable/main/binary-arm64/Packages || {
    echo "APT arm64 Packages missing clash-flux entry" >&2
    exit 1
  }
  gzip -k -9 -f dists/stable/main/binary-amd64/Packages
  gzip -k -9 -f dists/stable/main/binary-arm64/Packages
)

(
  cd "$pages_dir/apt/dists/stable"
  apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="Clash-Flux" \
    -o APT::FTPArchive::Release::Label="Clash-Flux" \
    -o APT::FTPArchive::Release::Suite="stable" \
    -o APT::FTPArchive::Release::Codename="stable" \
    -o APT::FTPArchive::Release::Architectures="amd64 arm64" \
    -o APT::FTPArchive::Release::Components="main" \
    release . >Release
  gpg "${gpg_sign_args[@]}" -abs -o Release.gpg Release
  gpg "${gpg_sign_args[@]}" --clearsign -o InRelease Release
  gpg --batch --verify Release.gpg Release
  gpg --batch --verify InRelease
)

if [ -f "resources/images/clash_flux_logo.png" ]; then
  cp "resources/images/clash_flux_logo.png" "$pages_dir/logo.png"
elif [ -f "assets/icon.png" ]; then
  cp "assets/icon.png" "$pages_dir/logo.png"
fi

cat >"$pages_dir/index.html" <<EOF
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <title>Clash-Flux — 全平台 sing-box 代理客户端 &amp; Linux 签名软件源</title>
  <meta name="description" content="Clash-Flux 是基于 C++23 与 HuxerUI 构建的跨平台 sing-box 代理客户端，支持 Clash YAML 与原生 JSON、桌面订阅编排、系统服务托管与签名 Linux 软件源。">
  <link rel="icon" type="image/png" href="logo.png">
  <style>
    :root {
      color-scheme: light dark;
      --bg-light: #efedf0;
      --bg-dark: #131315;
      --surface-light: #fcf8f9;
      --surface-dark: #252628;
      --surface-alt-light: #f5f2f4;
      --surface-alt-dark: #1c1c1f;
      --text-light: #202126;
      --text-dark: #eeeff3;
      --muted-light: #5c606a;
      --muted-dark: #babac6;
      --border-light: #dcd7dd;
      --border-dark: #37383b;
      --accent-light: #1a66c9;
      --accent-dark: #8ab6ff;
      --accent-text-light: #ffffff;
      --accent-text-dark: #061427;
      --code-bg-light: #e7e4e9;
      --code-bg-dark: #1b1c1f;

      --bg: var(--bg-light);
      --surface: var(--surface-light);
      --surface-alt: var(--surface-alt-light);
      --text: var(--text-light);
      --muted: var(--muted-light);
      --border: var(--border-light);
      --accent: var(--accent-light);
      --accent-text: var(--accent-text-light);
      --code-bg: var(--code-bg-light);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg: var(--bg-dark);
        --surface: var(--surface-dark);
        --surface-alt: var(--surface-alt-dark);
        --text: var(--text-dark);
        --muted: var(--muted-dark);
        --border: var(--border-dark);
        --accent: var(--accent-dark);
        --accent-text: var(--accent-text-dark);
        --code-bg: var(--code-bg-dark);
      }
    }

    @supports (color: light-dark(white, black)) {
      :root {
        --bg: light-dark(var(--bg-light), var(--bg-dark));
        --surface: light-dark(var(--surface-light), var(--surface-dark));
        --surface-alt: light-dark(var(--surface-alt-light), var(--surface-alt-dark));
        --text: light-dark(var(--text-light), var(--text-dark));
        --muted: light-dark(var(--muted-light), var(--muted-dark));
        --border: light-dark(var(--border-light), var(--border-dark));
        --accent: light-dark(var(--accent-light), var(--accent-dark));
        --accent-text: light-dark(var(--accent-text-light), var(--accent-text-dark));
        --code-bg: light-dark(var(--code-bg-light), var(--code-bg-dark));
      }
    }

    :root {
      accent-color: var(--accent);
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      font-family: system-ui, -apple-system, "Segoe UI", Roboto, "Noto Sans SC", sans-serif;
      background: var(--bg);
      color: var(--text);
      line-height: 1.65;
    }

    .container {
      max-width: 920px;
      margin: 0 auto;
      padding: 40px 20px 64px;
    }

    .hero {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 24px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 28px;
      margin-bottom: 24px;
    }

    .hero-logo {
      width: 88px;
      height: 88px;
      border-radius: 18px;
      object-fit: contain;
      background: #ffffff;
      padding: 6px;
      flex-shrink: 0;
    }

    .hero-body { flex: 1 1 320px; }

    .badge-row {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      align-items: center;
      margin-bottom: 8px;
    }

    .badge {
      display: inline-block;
      padding: 2px 10px;
      border-radius: 999px;
      font-size: 0.82rem;
      font-weight: 600;
      background: var(--accent);
      color: var(--accent-text);
    }

    .badge-subtle {
      background: var(--code-bg);
      color: var(--muted);
    }

    h1 {
      margin: 0 0 6px;
      font-size: clamp(1.6rem, 3vw, 2.1rem);
      line-height: 1.25;
    }

    h2 {
      margin: 0 0 12px;
      font-size: 1.2rem;
    }

    .lead {
      margin: 0 0 16px;
      color: var(--muted);
    }

    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 8px 16px;
      border-radius: 8px;
      border: 1px solid var(--border);
      background: var(--surface-alt);
      color: var(--text);
      text-decoration: none;
      font-size: 0.92rem;
      font-weight: 600;
      cursor: pointer;
    }

    .btn-primary {
      background: var(--accent);
      color: var(--accent-text);
      border-color: transparent;
    }

    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 24px;
      margin-bottom: 20px;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 14px;
    }

    .feature {
      background: var(--surface-alt);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
    }

    .feature h3 {
      margin: 0 0 6px;
      font-size: 1rem;
    }

    .feature p {
      margin: 0;
      color: var(--muted);
      font-size: 0.92rem;
    }

    .cmd-box {
      position: relative;
      margin: 10px 0 18px;
    }

    pre {
      margin: 0;
      padding: 14px 86px 14px 16px;
      border-radius: 10px;
      background: var(--code-bg);
      border: 1px solid var(--border);
      overflow-x: auto;
      font-size: 0.88rem;
    }

    code {
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 0.9em;
    }

    .copy-btn {
      position: absolute;
      top: 9px;
      right: 10px;
      padding: 4px 10px;
      border-radius: 6px;
      border: 1px solid var(--border);
      background: var(--surface);
      color: var(--text);
      font-size: 0.8rem;
      cursor: pointer;
    }

    ul.meta {
      margin: 0;
      padding-left: 20px;
    }

    ul.meta li + li {
      margin-top: 8px;
    }

    a { color: var(--accent); }
    .muted { color: var(--muted); font-size: 0.92rem; }
  </style>
</head>
<body>
  <main class="container">
    <section class="hero">
      <img class="hero-logo" src="logo.png" alt="Clash-Flux Logo" width="88" height="88" fetchpriority="high">
      <div class="hero-body">
        <div class="badge-row">
          <span class="badge">v${version}</span>
          <span class="badge badge-subtle">C++23 · HuxerUI · sing-box</span>
          <span class="badge badge-subtle">Linux / Windows / macOS / Android</span>
        </div>
        <h1>Clash-Flux</h1>
        <p class="lead">使用 C++23 与 HuxerUI 构建的全平台 <code>sing-box</code> 代理客户端，兼具 Clash 订阅兼容、原生 JSON 直通、桌面多订阅规则编排与轻量低占用体验。</p>
        <div class="actions">
          <a class="btn btn-primary" href="https://github.com/FarnaHerry/Clash-Flux/releases/tag/v${version}">下载 v${version}</a>
          <a class="btn" href="https://github.com/FarnaHerry/Clash-Flux">GitHub 仓库</a>
          <a class="btn" href="https://github.com/FarnaHerry/Clash-Flux/blob/main/README.md">中文文档</a>
        </div>
      </div>
    </section>

    <section class="card">
      <h2>核心特性</h2>
      <div class="grid">
        <div class="feature">
          <h3>Clash YAML 编译 &amp; 原生直通</h3>
          <p>支持 Clash 订阅（YAML/节点链接）、sing-box 原生 JSON 与 OpenVPN，自带完整转换器与保真度账本，显式记账不静默丢规则。</p>
        </div>
        <div class="feature">
          <h3>独立特权网络服务</h3>
          <p>Linux systemd / Windows SCM 托管 sing-box、TUN 与系统 PPTP，系统目录安装后随软件更新自动静默自升级服务，无需反复弹窗。</p>
        </div>
        <div class="feature">
          <h3>多订阅编排与规则追溯</h3>
          <p>支持桌面跨订阅选择策略目标、全局与个人规则覆写、连接规则追溯，以及网站出口测试与 QiFi/QRS 多帧二维码分享。</p>
        </div>
        <div class="feature">
          <h3>现代原生界面与主题</h3>
          <p>HuxerUI 原生渲染，支持深浅模式、Gemini 多色渐变、OKLCH 自适应自定义配色，以及应用内版本检查与完整性校验。</p>
        </div>
      </div>
    </section>

    <section class="card">
      <h2>一键安装</h2>
      <p class="muted"><strong>Linux / macOS</strong>（Linux 自动校验公钥指纹、配置本签名 APT/RPM 软件源并装入 <code>/opt/clash-flux</code>；macOS 自动安装 <code>.dmg</code> 至 <code>/Applications</code>）：</p>
      <div class="cmd-box">
        <pre><code id="cmd-unix">curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh</code></pre>
        <button class="copy-btn" type="button" data-copy="cmd-unix">复制</button>
      </div>
      <p class="muted"><strong>Windows PowerShell</strong>（校验 <code>SHA-256</code> 后默认运行官方 <code>.exe</code> 安装包）：</p>
      <div class="cmd-box">
        <pre><code id="cmd-win">irm https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.ps1 | iex</code></pre>
        <button class="copy-btn" type="button" data-copy="cmd-win">复制</button>
      </div>
    </section>

    <section class="card">
      <h2>Linux 签名软件源信息</h2>
      <ul class="meta">
        <li>APT 仓库：<code>https://farnaherry.github.io/Clash-Flux/apt</code>（suite: <code>stable</code>，component: <code>main</code>，支持 <code>amd64</code> / <code>arm64</code>）</li>
        <li>RPM 仓库：<code>https://farnaherry.github.io/Clash-Flux/rpm/\$basearch</code>（支持 <code>x86_64</code> / <code>aarch64</code>）</li>
        <li>GPG 公钥（ASCII / RPM）：<a href="gpg.key">gpg.key</a> · <a href="clash-flux.gpg">clash-flux.gpg</a>（APT keyring）</li>
        <li>公钥指纹：<code>${actual_fingerprint}</code></li>
      </ul>
      <p class="muted" style="margin-top: 12px;">Debian / Ubuntu 后续可直接通过 <code>sudo apt update &amp;&amp; sudo apt upgrade</code> 升级；Fedora / RHEL / openSUSE 可直接通过 <code>sudo dnf upgrade</code> 或 <code>sudo zypper up</code> 升级。Arch 等其他发行版可在 <a href="https://github.com/FarnaHerry/Clash-Flux/releases">Releases</a> 下载 <code>.tar.gz</code> 并解压至 <code>/opt/clash-flux</code>。</p>
    </section>
  </main>
  <script>
    document.querySelectorAll('.copy-btn').forEach(function (button) {
      button.addEventListener('click', function () {
        var target = document.getElementById(button.getAttribute('data-copy'));
        if (!target || !navigator.clipboard) return;
        navigator.clipboard.writeText(target.textContent).then(function () {
          var original = button.textContent;
          button.textContent = '已复制';
          setTimeout(function () { button.textContent = original; }, 1500);
        });
      });
    });
  </script>
</body>
</html>
EOF
