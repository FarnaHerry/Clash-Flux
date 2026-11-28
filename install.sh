#!/bin/sh

set -eu

repository_url="https://github.com/FarnaHerry/Clash-Flux"
api_url="https://api.github.com/repos/FarnaHerry/Clash-Flux"

version=""
format="auto"
format_explicit=false
prefix=""
explicit_prefix=false
archive=""
expected_sha256=""
assume_yes=false
uninstall=false

temporary_directory=""
staging_directory=""
backup_directory=""
lock_directory=""
dmg_mount_point=""

usage() {
  cat <<'EOF'
Usage:
  install.sh [--version <version>] [--format auto|deb|rpm|tar|dmg] [--portable] [--prefix <path>] [--archive <path>] [--sha256 <hex>] [--yes]
  install.sh --uninstall [--format auto|deb|rpm|tar|dmg] [--portable] [--prefix <path>] [--yes]

Examples:
  curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh
  curl -fsSL https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.sh | sh -s -- --portable
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
  if [ -n "$lock_directory" ]; then
    rmdir "$lock_directory" 2>/dev/null || true
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
  --format)
    require_value "$@"
    format=$2
    format_explicit=true
    shift 2
    ;;
  --portable)
    format="tar"
    format_explicit=true
    shift
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

case "$format" in
auto | deb | rpm | tar | dmg) ;;
*) fail "unsupported format: $format (expected auto, deb, rpm, tar, or dmg)" ;;
esac

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
  ;;
arm64 | aarch64)
  if [ "$host_system" = "macos" ]; then
    host_architecture="arm64"
  else
    host_architecture="aarch64"
  fi
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

detect_linux_format() {
  if [ -f /var/lib/dpkg/status ] && command -v dpkg >/dev/null 2>&1; then
    printf '%s\n' "deb"
  elif { [ -d /var/lib/rpm ] || [ -d /usr/lib/sysimage/rpm ]; } && command -v rpm >/dev/null 2>&1; then
    printf '%s\n' "rpm"
  else
    printf '%s\n' "tar"
  fi
}

has_privilege_escalation() {
  [ "$(id -u)" -eq 0 ] || command -v sudo >/dev/null 2>&1 || command -v doas >/dev/null 2>&1
}

run_privileged() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  elif command -v doas >/dev/null 2>&1; then
    doas "$@"
  else
    fail "root privileges, sudo, or doas are required to run: $*"
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

is_portable_install() {
  [ -d "$1" ] && [ -x "$1/clash-flux" ]
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

if [ "$host_system" = "macos" ]; then
  if [ "$format" = "auto" ]; then
    format="dmg"
  elif [ "$format" != "dmg" ]; then
    fail "format '$format' is not supported on macOS"
  fi
else
  if [ "$format" = "dmg" ]; then
    fail "format 'dmg' is only supported on macOS"
  fi
  if [ "$format" = "auto" ]; then
    format=$(detect_linux_format)
    if [ "$uninstall" = false ] && { [ "$format" = "deb" ] || [ "$format" = "rpm" ]; } && ! has_privilege_escalation; then
      log "No root privileges or sudo/doas available; falling back to portable .tar.gz install"
      format="tar"
    fi
  fi
fi

if [ "$explicit_prefix" = false ]; then
  if [ "$host_system" = "macos" ]; then
    if [ -w /Applications ] || [ "$(id -u)" -eq 0 ]; then
      prefix="/Applications"
    else
      prefix="$HOME/Applications"
    fi
  else
    prefix="$HOME/.local/share/Clash-Flux"
  fi
fi

if [ "$uninstall" = true ]; then
  case "$format" in
  deb)
    confirm_destructive "Uninstall Clash-Flux (.deb package); user configuration and subscriptions will be preserved."
    if command -v apt-get >/dev/null 2>&1; then
      run_privileged apt-get remove -y clash-flux
    else
      run_privileged dpkg -r clash-flux
    fi
    log "Clash-Flux .deb package removed"
    ;;
  rpm)
    confirm_destructive "Uninstall Clash-Flux (.rpm package); user configuration and subscriptions will be preserved."
    if command -v dnf >/dev/null 2>&1; then
      run_privileged dnf remove -y clash-flux
    elif command -v zypper >/dev/null 2>&1; then
      run_privileged zypper --non-interactive remove clash-flux
    elif command -v yum >/dev/null 2>&1; then
      run_privileged yum remove -y clash-flux
    else
      run_privileged rpm -e clash-flux
    fi
    log "Clash-Flux .rpm package removed"
    ;;
  tar)
    prefix=$(normalize_prefix "$prefix")
    is_portable_install "$prefix" || fail "refusing to uninstall: Clash-Flux not found at $prefix"
    confirm_destructive "Uninstall portable Clash-Flux from $prefix; user configuration and subscriptions will be preserved."
    mkdir "$prefix.clash-flux-lock" 2>/dev/null ||
      fail "another installer is modifying $prefix"
    lock_directory="$prefix.clash-flux-lock"
    rm -rf "$prefix"
    bin_link="$HOME/.local/bin/clash-flux"
    if [ -L "$bin_link" ]; then
      link_target=$(readlink "$bin_link" 2>/dev/null || true)
      if [ "$link_target" = "$prefix/clash-flux" ]; then
        rm -f "$bin_link"
      fi
    fi
    data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
    rm -f "$data_home/applications/clash-flux.desktop"
    rm -f "$data_home/icons/hicolor/256x256/apps/clash-flux.png"
    log "Portable Clash-Flux removed from $prefix"
    ;;
  dmg)
    prefix=$(normalize_prefix "$prefix")
    app_bundle="$prefix/Clash-Flux.app"
    [ -d "$app_bundle" ] || fail "Clash-Flux.app not found at $app_bundle"
    confirm_destructive "Uninstall $app_bundle; user data will be preserved."
    rm -rf "$app_bundle"
    log "Removed $app_bundle"
    ;;
  esac
  exit 0
fi

command -v curl >/dev/null 2>&1 || fail "curl is required"

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
  case "$format" in
  deb) asset_name="clash-flux-$version-linux-$host_architecture.deb" ;;
  rpm) asset_name="clash-flux-$version-linux-$host_architecture.rpm" ;;
  tar) asset_name="clash-flux-$version-linux-$host_architecture.tar.gz" ;;
  dmg) asset_name="clash-flux-$version-macos-$host_architecture.dmg" ;;
  esac
  asset_url="$repository_url/releases/download/$release_tag/$asset_name"
else
  archive=$(to_abs_path "$archive")
  [ -f "$archive" ] || fail "archive does not exist: $archive"
  asset_name=$(basename "$archive")
  if [ "$format_explicit" = false ]; then
    case "$asset_name" in
    *.deb) format="deb" ;;
    *.rpm) format="rpm" ;;
    *.tar.gz | *.tgz) format="tar" ;;
    *.dmg) format="dmg" ;;
    *) fail "cannot infer package format from archive name: $asset_name" ;;
    esac
  fi
  case "$format:$asset_name" in
  deb:*.deb | rpm:*.rpm | tar:*.tar.gz | tar:*.tgz | dmg:*.dmg) ;;
  *) fail "archive $asset_name does not match format $format" ;;
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

case "$format" in
deb)
  has_privilege_escalation || fail "root privileges, sudo, or doas are required to install .deb packages"
  log "Installing $asset_name via system package manager"
  if command -v apt-get >/dev/null 2>&1; then
    run_privileged apt-get install -y "$package_file"
  else
    run_privileged dpkg -i "$package_file"
  fi
  log "Clash-Flux installed successfully ($asset_name)"
  ;;
rpm)
  has_privilege_escalation || fail "root privileges, sudo, or doas are required to install .rpm packages"
  log "Installing $asset_name via system package manager"
  if command -v dnf >/dev/null 2>&1; then
    run_privileged dnf install -y "$package_file"
  elif command -v zypper >/dev/null 2>&1; then
    run_privileged zypper --non-interactive install --allow-unsigned-rpm "$package_file"
  elif command -v yum >/dev/null 2>&1; then
    run_privileged yum install -y "$package_file"
  else
    run_privileged rpm -Uvh "$package_file"
  fi
  log "Clash-Flux installed successfully ($asset_name)"
  ;;
tar)
  prefix=$(normalize_prefix "$prefix")
  if [ -e "$prefix" ] && ! is_portable_install "$prefix"; then
    fail "installation prefix exists and is not a portable Clash-Flux directory: $prefix"
  fi

  tar_listing="$temporary_directory/tar.list"
  tar -tzf "$package_file" >"$tar_listing" || fail "unable to read archive: $asset_name"
  if awk '
    $0 ~ /^\// || $0 ~ /(^|\/)\.\.(\/|$)/ { bad = 1 }
    END { exit bad ? 0 : 1 }
  ' "$tar_listing"; then
    fail "archive contains an unsafe absolute or traversal path"
  fi

  extract_dir="$temporary_directory/extract"
  mkdir -p "$extract_dir"
  tar -xzf "$package_file" -C "$extract_dir" || fail "failed to extract $asset_name"

  payload_dir=""
  if [ -f "$extract_dir/clash-flux" ]; then
    payload_dir="$extract_dir"
  else
    for candidate in "$extract_dir"/*; do
      if [ -d "$candidate" ] && [ -f "$candidate/clash-flux" ]; then
        [ -z "$payload_dir" ] || fail "archive contains multiple payload directories"
        payload_dir=$candidate
      fi
    done
  fi
  [ -n "$payload_dir" ] || fail "archive does not contain clash-flux executable"

  rm -f "$payload_dir"/*.deb "$payload_dir"/*.rpm
  chmod +x "$payload_dir/clash-flux"
  if [ -f "$payload_dir/clash-flux-updater" ]; then
    chmod +x "$payload_dir/clash-flux-updater"
  fi
  if [ -d "$payload_dir/engines" ]; then
    for engine in "$payload_dir/engines"/*; do
      [ -f "$engine" ] && chmod +x "$engine" || true
    done
  fi

  parent_dir=$(dirname "$prefix")
  mkdir -p "$parent_dir"
  mkdir "$prefix.clash-flux-lock" 2>/dev/null ||
    fail "another installer is modifying $prefix"
  lock_directory="$prefix.clash-flux-lock"

  staging_directory=$(mktemp -d "$parent_dir/.clash-flux-stage.XXXXXX")
  rmdir "$staging_directory"
  mv "$payload_dir" "$staging_directory"

  swap_target="$prefix"
  if [ -e "$prefix" ]; then
    backup_directory="$parent_dir/.clash-flux-backup.$$"
    rm -rf "$backup_directory"
    mv "$prefix" "$backup_directory"
  fi

  mv "$staging_directory" "$prefix"
  staging_directory=""
  if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
    rm -rf "$backup_directory"
    backup_directory=""
  fi
  swap_target=""

  bin_dir="$HOME/.local/bin"
  mkdir -p "$bin_dir"
  ln -sf "$prefix/clash-flux" "$bin_dir/clash-flux"

  data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
  icon_name="clash-flux"
  if [ -f "$prefix/assets/icon.png" ]; then
    icon_dir="$data_home/icons/hicolor/256x256/apps"
    mkdir -p "$icon_dir"
    cp "$prefix/assets/icon.png" "$icon_dir/clash-flux.png"
  fi
  desktop_dir="$data_home/applications"
  mkdir -p "$desktop_dir"
  cat >"$desktop_dir/clash-flux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Clash-Flux
Comment=Cross-platform sing-box proxy client
Exec="$prefix/clash-flux" %u
Icon=$icon_name
Categories=Network;
Terminal=false
MimeType=x-scheme-handler/clash;x-scheme-handler/sing-box;
StartupWMClass=clash-flux
EOF

  log "Installed portable Clash-Flux to $prefix"
  log "Launcher symlink: $bin_dir/clash-flux"
  ;;
dmg)
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
  ;;
esac
