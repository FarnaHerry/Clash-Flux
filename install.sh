#!/bin/sh

set -eu

repository_url="https://github.com/FarnaHerry/Clash-Flux"
repo_url=${CLASHFLUX_REPO_URL:-"https://farnaherry.github.io/Clash-Flux"}
system_root=${CLASHFLUX_SYSTEM_ROOT:-""}

expected_gpg_fingerprint="5D58F92EC9A683FE42638DBF89A86E971F6EFD47"
expected_gpg_ascii_sha256="ac5622d1116c33db608a2ab3d8a00bbbdd6ed9fc82fc04563479ec9d449fe06b"
expected_gpg_bin_sha256="06643b297a55915352e1ca7cdae5f6e9b8d29ec2a084c8d6e223ce1073444ae6"

version=""
prefix=""
explicit_prefix=false
archive=""
expected_sha256=""
assume_yes=false
uninstall=false

temporary_directory=""
staging_directory=""
backup_directory=""
dmg_mount_point=""

usage() {
  cat <<'EOF'
Usage:
  install.sh [--version <version>] [--prefix <path>] [--archive <path>] [--sha256 <hex>] [--yes]
  install.sh --uninstall [--prefix <path>] [--yes]

Examples:
  curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh
  curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh -s -- --uninstall
EOF
}

log() {
  printf 'Clash-Flux installer: %s\n' "$1"
}

fail() {
  printf 'Clash-Flux installer: %s\n' "$1" >&2
  exit 1
}

cleanup() {
  if [ -n "$dmg_mount_point" ] && command -v hdiutil >/dev/null 2>&1; then
    hdiutil detach "$dmg_mount_point" -quiet >/dev/null 2>&1 ||
      hdiutil detach "$dmg_mount_point" -force -quiet >/dev/null 2>&1 || true
  fi
  if [ -n "$staging_directory" ] && [ -e "$staging_directory" ]; then
    rm -rf "$staging_directory"
  fi
  if [ -n "$backup_directory" ] && [ -e "$backup_directory" ]; then
    if [ -n "${swap_target:-}" ] && [ ! -e "$swap_target" ]; then
      mv "$backup_directory" "$swap_target" ||
        printf 'Clash-Flux installer: previous installation remains at %s\n' "$backup_directory" >&2
    else
      rm -rf "$backup_directory"
    fi
  fi
  if [ -n "$temporary_directory" ] && [ -d "$temporary_directory" ]; then
    rm -rf "$temporary_directory"
  fi
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

require_value() {
  [ "$#" -ge 2 ] || fail "$1 requires a value"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
  --version)
    require_value "$@"
    version=$2
    shift 2
    ;;
  --prefix)
    require_value "$@"
    prefix=$2
    explicit_prefix=true
    shift 2
    ;;
  --archive)
    require_value "$@"
    archive=$2
    shift 2
    ;;
  --sha256)
    require_value "$@"
    expected_sha256=$2
    shift 2
    ;;
  --yes | -y)
    assume_yes=true
    shift
    ;;
  --uninstall)
    uninstall=true
    shift
    ;;
  --help | -h)
    usage
    exit 0
    ;;
  *)
    fail "unknown argument: $1"
    ;;
  esac
done

if [ "$uninstall" = true ] && { [ -n "$version" ] || [ -n "$archive" ] || [ -n "$expected_sha256" ]; }; then
  fail "--uninstall cannot be combined with --version, --archive, or --sha256"
fi
if [ -n "$version" ] && [ -n "$archive" ]; then
  fail "--version and --archive cannot be combined"
fi

case "$(uname -s)" in
Linux)
  if [ -n "${TERMUX_VERSION:-}" ] || { [ -n "${ANDROID_ROOT:-}" ] && [ -x /system/bin/getprop ]; }; then
    fail "Termux/Android CLI is not supported; install the Android APK release"
  fi
  host_system="linux"
  ;;
Darwin)
  host_system="macos"
  ;;
*)
  fail "unsupported operating system: $(uname -s)"
  ;;
esac

case "$(uname -m)" in
x86_64 | amd64)
  [ "$host_system" != "macos" ] || fail "macOS releases currently support Apple Silicon (arm64) only"
  host_architecture="x86_64"
  dpkg_architecture="amd64"
  ;;
arm64 | aarch64)
  if [ "$host_system" = "macos" ]; then
    host_architecture="arm64"
  else
    host_architecture="aarch64"
  fi
  dpkg_architecture="arm64"
  ;;
*)
  fail "unsupported architecture: $(uname -m)"
  ;;
esac

[ -n "${HOME:-}" ] || fail "HOME is not configured"

confirm_destructive() {
  prompt=$1
  printf '%s\n' "$prompt"
  if [ "$assume_yes" = true ]; then
    return
  fi
  if [ ! -r /dev/tty ]; then
    fail "confirmation requires an interactive terminal or --yes"
  fi
  printf 'Continue? [y/N] ' >/dev/tty
  answer=""
  IFS= read -r answer </dev/tty || true
  case "$answer" in
  y | Y | yes | YES | Yes) ;;
  *) fail "cancelled" ;;
  esac
}

run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    if sudo -n true >/dev/null 2>&1; then
      sudo "$@"
    elif [ -r /dev/tty ]; then
      sudo "$@" </dev/tty
    else
      fail "root privileges or an interactive terminal for sudo are required to run: $*"
    fi
  elif command -v doas >/dev/null 2>&1; then
    if [ -r /dev/tty ]; then
      doas "$@" </dev/tty
    else
      fail "root privileges or an interactive terminal for doas are required to run: $*"
    fi
  else
    fail "root privileges, sudo, or doas are required to install system packages"
  fi
}

to_abs_path() {
  case "$1" in
  /*) printf '%s\n' "$1" ;;
  *) printf '%s/%s\n' "$(pwd -P)" "$1" ;;
  esac
}

validate_safe_prefix() {
  candidate=$1
  case "/$candidate/" in
  *"/../"* | *"/./"*) fail "--prefix must not contain . or .. path components" ;;
  esac
  case "$candidate" in
  "" | "/" | "$HOME" | "$HOME/") fail "unsafe installation prefix: $candidate" ;;
  esac
  [ ! -L "$candidate" ] || fail "installation prefix must not be a symbolic link: $candidate"
}

normalize_prefix() {
  candidate=$(to_abs_path "$1")
  validate_safe_prefix "$candidate"
  prefix_parent=$(dirname "$candidate")
  prefix_name=$(basename "$candidate")
  mkdir -p "$prefix_parent"
  prefix_parent=$(cd "$prefix_parent" && pwd -P)
  candidate="$prefix_parent/$prefix_name"
  validate_safe_prefix "$candidate"
  printf '%s\n' "$candidate"
}

normalize_sha256() {
  LC_ALL=C printf '%s' "$1" | tr 'A-F' 'a-f'
}

is_valid_sha256() {
  LC_ALL=C awk -v h="$1" 'BEGIN { exit !(length(h) == 64 && h ~ /^[0-9a-fA-F]+$/) }'
}

compute_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  else
    fail "no SHA-256 tool found (expected sha256sum, shasum, or openssl)"
  fi
}

extract_sha256_from_sums() {
  sums_file=$1
  target_name=$2
  [ -f "$sums_file" ] || return 1
  found=$(LC_ALL=C awk -v name="$target_name" '
    {
      hash = $1
      file = $2
      sub(/^\*/, "", file)
      sub(/^.*\//, "", file)
      if (file == name && length(hash) == 64 && hash ~ /^[0-9a-fA-F]+$/) {
        print hash
        exit 0
      }
    }
  ' "$sums_file")
  [ -n "$found" ] || return 1
  printf '%s\n' "$found"
}

extract_sha256_from_release_json() {
  json_file=$1
  target_name=$2
  found=$(LC_ALL=C awk -v name="$target_name" '
    BEGIN { RS = "," }
    {
      line = $0
      if (line ~ /"name"[[:space:]]*:/) {
        val = line
        sub(/^.*"name"[[:space:]]*:[[:space:]]*"/, "", val)
        sub(/".*$/, "", val)
        current_name = val
      }
      if (line ~ /"digest"[[:space:]]*:[[:space:]]*"sha256:/ && current_name == name) {
        dig = line
        sub(/^.*"digest"[[:space:]]*:[[:space:]]*"sha256:/, "", dig)
        sub(/".*$/, "", dig)
        if (length(dig) == 64 && dig ~ /^[0-9a-fA-F]+$/) {
          print dig
          exit 0
        }
      }
    }
  ' "$json_file")
  [ -n "$found" ] || return 1
  printf '%s\n' "$found"
}

verify_sha256() {
  file_path=$1
  expected=$2
  label=$3
  expected=$(normalize_sha256 "$expected")
  is_valid_sha256 "$expected" || fail "invalid SHA-256 digest for $label: $expected"
  actual=$(normalize_sha256 "$(compute_sha256 "$file_path")")
  [ "$actual" = "$expected" ] || fail "SHA-256 mismatch for $label (expected $expected, got $actual)"
  log "Verified SHA-256 for $label"
}

verify_repo_gpg_key() {
  key_file=$1
  expected_sha=$2
  label=$3
  verify_sha256 "$key_file" "$expected_sha" "$label"
  if command -v gpg >/dev/null 2>&1; then
    actual_fpr=$(gpg --batch --show-keys --with-colons "$key_file" 2>/dev/null |
      awk -F: '$1=="fpr"{print toupper($10); exit}' || true)
    [ "$actual_fpr" = "$expected_gpg_fingerprint" ] ||
      fail "GPG key fingerprint mismatch for $label (expected $expected_gpg_fingerprint, got ${actual_fpr:-none})"
    log "Verified GPG fingerprint ($expected_gpg_fingerprint) for $label"
  fi
}

detect_linux_pkg_manager() {
  if command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then
    printf '%s\n' "apt"
  elif command -v dnf >/dev/null 2>&1 && command -v rpm >/dev/null 2>&1; then
    printf '%s\n' "dnf"
  elif command -v zypper >/dev/null 2>&1 && command -v rpm >/dev/null 2>&1; then
    printf '%s\n' "zypper"
  elif command -v yum >/dev/null 2>&1 && command -v rpm >/dev/null 2>&1; then
    printf '%s\n' "yum"
  else
    printf '%s\n' "unsupported"
  fi
}

check_linux_launcher_path() {
  expected_bin="$system_root/usr/bin/clash-flux"
  resolved_bin=$(command -v clash-flux 2>/dev/null || true)
  if [ -z "$resolved_bin" ]; then
    log "Note: /usr/bin is not in PATH; run Clash-Flux via /usr/bin/clash-flux"
    return
  fi
  if [ -d "$(dirname "$expected_bin")" ]; then
    expected_dir=$(cd "$(dirname "$expected_bin")" && pwd -P)
    expected_bin_real="$expected_dir/clash-flux"
  else
    expected_bin_real="$expected_bin"
  fi
  resolved_dir=$(cd "$(dirname "$resolved_bin")" 2>/dev/null && pwd -P || true)
  resolved_bin_real="$resolved_dir/$(basename "$resolved_bin")"
  if [ "$resolved_bin_real" != "$expected_bin_real" ]; then
    log "Warning: 'clash-flux' in PATH currently resolves to $resolved_bin instead of $expected_bin (shadowed by another installation)."
    log "If you have a leftover user-local binary (e.g. ~/.local/bin/clash-flux), remove it so the system package takes precedence."
  fi
}

if [ "$host_system" = "linux" ]; then
  [ -z "$archive" ] && [ -z "$expected_sha256" ] ||
    fail "--archive and --sha256 are only used for macOS .dmg installation"
  [ "$explicit_prefix" = false ] ||
    fail "--prefix is not supported on Linux (system packages install to /opt/clash-flux)"

  pkg_manager=$(detect_linux_pkg_manager)
  if [ "$pkg_manager" = "unsupported" ]; then
    fail "no supported Linux package manager found (expected apt, dnf, zypper, or yum). For Arch/other distros, download clash-flux-<version>-linux-$host_architecture.tar.gz from $repository_url/releases and extract to /opt/clash-flux so the systemd service can verify root ownership."
  fi

  apt_keyring="$system_root/usr/share/keyrings/clash-flux.gpg"
  apt_sources="$system_root/etc/apt/sources.list.d/clash-flux.sources"
  yum_repo="$system_root/etc/yum.repos.d/clash-flux.repo"
  zypp_repo="$system_root/etc/zypp/repos.d/clash-flux.repo"

  if [ "$uninstall" = true ]; then
    if [ "$pkg_manager" = "apt" ]; then
      confirm_destructive "Uninstall Clash-Flux and remove its APT repository configuration; user configuration and subscriptions will be preserved."
      run_privileged apt-get remove -y clash-flux
      run_privileged rm -f "$apt_sources" "$apt_keyring"
    else
      confirm_destructive "Uninstall Clash-Flux and remove its RPM repository configuration; user configuration and subscriptions will be preserved."
      case "$pkg_manager" in
      dnf) run_privileged dnf remove -y clash-flux ;;
      zypper) run_privileged zypper --non-interactive remove clash-flux ;;
      yum) run_privileged yum remove -y clash-flux ;;
      esac
      run_privileged rm -f "$yum_repo" "$zypp_repo"
    fi
    log "Clash-Flux removed cleanly (user data preserved)"
    exit 0
  fi

  command -v curl >/dev/null 2>&1 || fail "curl is required"

  if [ -n "$version" ]; then
    version=${version#v}
    case "$version" in
    "" | *[!0-9A-Za-z._-]*) fail "invalid version: $version" ;;
    esac
  fi

  temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-install.XXXXXX")

  if [ "$pkg_manager" = "apt" ]; then
    tmp_keyring="$temporary_directory/clash-flux.gpg"
    tmp_sources="$temporary_directory/clash-flux.sources"
    log "Configuring signed APT repository ($repo_url/apt)"
    curl -fsSL "$repo_url/clash-flux.gpg" -o "$tmp_keyring" ||
      fail "failed to download APT GPG keyring from $repo_url/clash-flux.gpg"
    verify_repo_gpg_key "$tmp_keyring" "$expected_gpg_bin_sha256" "clash-flux.gpg"

    cat >"$tmp_sources" <<EOF
Types: deb
URIs: $repo_url/apt
Suites: stable
Components: main
Architectures: $dpkg_architecture
Signed-By: /usr/share/keyrings/clash-flux.gpg
EOF

    run_privileged mkdir -p "$(dirname "$apt_keyring")" "$(dirname "$apt_sources")"
    run_privileged cp "$tmp_keyring" "$apt_keyring"
    run_privileged chmod 0644 "$apt_keyring"
    run_privileged cp "$tmp_sources" "$apt_sources"
    run_privileged chmod 0644 "$apt_sources"

    run_privileged apt-get update
    if [ -n "$version" ]; then
      run_privileged apt-get install -y "clash-flux=$version"
    else
      run_privileged apt-get install -y clash-flux
    fi
    log "Clash-Flux installed via APT (/opt/clash-flux); future updates arrive automatically via 'sudo apt upgrade'."
  else
    tmp_key="$temporary_directory/gpg.key"
    tmp_repo="$temporary_directory/clash-flux.repo"
    log "Configuring signed RPM repository ($repo_url/rpm/\$basearch)"
    curl -fsSL "$repo_url/gpg.key" -o "$tmp_key" ||
      fail "failed to download RPM GPG key from $repo_url/gpg.key"
    verify_repo_gpg_key "$tmp_key" "$expected_gpg_ascii_sha256" "gpg.key"

    run_privileged rpm --import "$tmp_key"

    if [ "$pkg_manager" = "zypper" ]; then
      target_repo="$zypp_repo"
      cat >"$tmp_repo" <<EOF
[clash-flux]
name=Clash-Flux
baseurl=$repo_url/rpm/\$basearch
enabled=1
autorefresh=1
gpgcheck=1
repo_gpgcheck=0
gpgkey=$repo_url/gpg.key
EOF
    else
      target_repo="$yum_repo"
      cat >"$tmp_repo" <<EOF
[clash-flux]
name=Clash-Flux
baseurl=$repo_url/rpm/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=0
gpgkey=$repo_url/gpg.key
EOF
    fi

    run_privileged mkdir -p "$(dirname "$target_repo")"
    run_privileged cp "$tmp_repo" "$target_repo"
    run_privileged chmod 0644 "$target_repo"

    pkg_spec="clash-flux"
    if [ -n "$version" ]; then
      pkg_spec="clash-flux-$version"
    fi
    case "$pkg_manager" in
    dnf)
      run_privileged dnf -y --refresh install "$pkg_spec"
      ;;
    zypper)
      run_privileged zypper --non-interactive --gpg-auto-import-keys refresh clash-flux
      run_privileged zypper --non-interactive install "$pkg_spec"
      ;;
    yum)
      run_privileged yum makecache -y
      run_privileged yum install -y "$pkg_spec"
      ;;
    esac
    log "Clash-Flux installed via $pkg_manager (/opt/clash-flux); future updates arrive automatically via system upgrades."
  fi

  check_linux_launcher_path
  log "To enable TUN/PPTP mode, install the systemd service from Settings or run: pkexec /opt/clash-flux/clash-flux service install"
  exit 0
fi

# ---- macOS (.dmg) ------------------------------------------------------------
if [ "$explicit_prefix" = false ]; then
  if [ -w /Applications ] || [ "$(id -u)" -eq 0 ]; then
    prefix="/Applications"
  else
    prefix="$HOME/Applications"
  fi
fi

if [ "$uninstall" = true ]; then
  prefix=$(normalize_prefix "$prefix")
  app_bundle="$prefix/Clash-Flux.app"
  [ -d "$app_bundle" ] || fail "Clash-Flux.app not found at $app_bundle"
  confirm_destructive "Uninstall $app_bundle; user data will be preserved."
  rm -rf "$app_bundle"
  log "Removed $app_bundle"
  exit 0
fi

command -v curl >/dev/null 2>&1 || fail "curl is required"

api_url="https://api.github.com/repos/FarnaHerry/Clash-Flux"
release_tag=""
if [ -z "$archive" ]; then
  if [ -z "$version" ]; then
    latest_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$repository_url/releases/latest") ||
      fail "failed to resolve the latest Clash-Flux release"
    release_tag=${latest_url##*/}
    case "$release_tag" in
    v*) version=${release_tag#v} ;;
    *) fail "latest Clash-Flux release returned an invalid tag: $release_tag" ;;
    esac
  else
    version=${version#v}
    release_tag="v$version"
  fi
  case "$version" in
  "" | *[!0-9A-Za-z._-]*) fail "invalid version: $version" ;;
  esac
  asset_name="clash-flux-$version-macos-$host_architecture.dmg"
  asset_url="$repository_url/releases/download/$release_tag/$asset_name"
else
  archive=$(to_abs_path "$archive")
  [ -f "$archive" ] || fail "archive does not exist: $archive"
  asset_name=$(basename "$archive")
  case "$asset_name" in
  *.dmg) ;;
  *) fail "archive $asset_name is not a macOS .dmg installer" ;;
  esac
  asset_url="$archive"
fi

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-install.XXXXXX")
package_file="$archive"

if [ -z "$package_file" ]; then
  package_file="$temporary_directory/$asset_name"
  log "Downloading $asset_name ($release_tag)"
  curl -fsSL "$asset_url" -o "$package_file" || fail "failed to download $asset_url"

  if [ -z "$expected_sha256" ]; then
    sums_file="$temporary_directory/SHA256SUMS.txt"
    if curl -fsSL "$repository_url/releases/download/$release_tag/SHA256SUMS.txt" -o "$sums_file" 2>/dev/null; then
      expected_sha256=$(extract_sha256_from_sums "$sums_file" "$asset_name" || true)
    fi
  fi
  if [ -z "$expected_sha256" ]; then
    release_json="$temporary_directory/release.json"
    if curl -fsSL "$api_url/releases/tags/$release_tag" -o "$release_json" 2>/dev/null; then
      expected_sha256=$(extract_sha256_from_release_json "$release_json" "$asset_name" || true)
    fi
  fi
else
  if [ -z "$expected_sha256" ]; then
    if [ -f "$package_file.sha256" ]; then
      expected_sha256=$(awk '{print $1; exit}' "$package_file.sha256")
    else
      archive_dir=$(dirname "$package_file")
      if [ -f "$archive_dir/SHA256SUMS.txt" ]; then
        expected_sha256=$(extract_sha256_from_sums "$archive_dir/SHA256SUMS.txt" "$asset_name" || true)
      fi
    fi
  fi
fi

[ -n "$expected_sha256" ] || fail "unable to resolve expected SHA-256 digest for $asset_name (provide --sha256 or SHA256SUMS.txt)"
verify_sha256 "$package_file" "$expected_sha256" "$asset_name"

prefix=$(normalize_prefix "$prefix")
mkdir -p "$prefix"
dmg_mount_point="$temporary_directory/mnt"
mkdir -p "$dmg_mount_point"
hdiutil attach -nobrowse -readonly -mountpoint "$dmg_mount_point" "$package_file" >/dev/null ||
  fail "failed to mount $asset_name"

source_app=""
if [ -d "$dmg_mount_point/Clash-Flux.app" ]; then
  source_app="$dmg_mount_point/Clash-Flux.app"
else
  for candidate in "$dmg_mount_point"/*.app; do
    if [ -d "$candidate" ]; then
      source_app=$candidate
      break
    fi
  done
fi
[ -n "$source_app" ] || fail "no .app bundle found in $asset_name"

app_name=$(basename "$source_app")
target_app="$prefix/$app_name"
staging_directory="$prefix/.Clash-Flux.staging.$$"
rm -rf "$staging_directory"
if command -v ditto >/dev/null 2>&1; then
  ditto "$source_app" "$staging_directory"
else
  cp -R "$source_app" "$staging_directory"
fi

hdiutil detach "$dmg_mount_point" -quiet >/dev/null 2>&1 || true
dmg_mount_point=""

swap_target="$target_app"
if [ -e "$target_app" ]; then
  backup_directory="$prefix/.Clash-Flux.backup.$$"
  rm -rf "$backup_directory"
  mv "$target_app" "$backup_directory"
fi

mv "$staging_directory" "$target_app"
staging_directory=""
if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
  rm -rf "$backup_directory"
  backup_directory=""
fi
swap_target=""

log "Installed Clash-Flux to $target_app"
