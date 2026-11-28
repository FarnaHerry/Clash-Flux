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

  cp "$deb_file" "$pages_dir/apt/pool/main/c/clash-flux/"
done

(
  cd "$pages_dir/apt"
  dpkg-scanpackages --arch amd64 --multiversion pool/main >dists/stable/main/binary-amd64/Packages
  dpkg-scanpackages --arch arm64 --multiversion pool/main >dists/stable/main/binary-arm64/Packages
  test -s dists/stable/main/binary-amd64/Packages
  test -s dists/stable/main/binary-arm64/Packages
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

cat >"$pages_dir/index.html" <<EOF
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Clash-Flux Linux Package Repository</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; max-width: 760px; margin: 40px auto; padding: 0 20px; line-height: 1.6; }
    code, pre { background: #f4f4f6; padding: 2px 6px; border-radius: 6px; }
    pre { padding: 14px; overflow-x: auto; }
  </style>
</head>
<body>
  <h1>Clash-Flux Linux Package Repository</h1>
  <p>Latest signed release: <strong>v${version}</strong></p>
  <h2>Quick Install (Linux &amp; macOS)</h2>
  <pre><code>curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh</code></pre>
  <h2>Repository Endpoints</h2>
  <ul>
    <li>APT repository: <code>https://farnaherry.github.io/Clash-Flux/apt</code> (suite: <code>stable</code>, component: <code>main</code>)</li>
    <li>RPM repository: <code>https://farnaherry.github.io/Clash-Flux/rpm/\$basearch</code> (<code>x86_64</code>, <code>aarch64</code>)</li>
    <li>GPG public key (ASCII / RPM): <a href="gpg.key">gpg.key</a></li>
    <li>GPG keyring (APT binary): <a href="clash-flux.gpg">clash-flux.gpg</a></li>
    <li>Fingerprint: <code>${actual_fingerprint}</code></li>
  </ul>
</body>
</html>
EOF
