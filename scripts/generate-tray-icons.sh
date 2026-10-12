#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
logo="$repo_root/assets/tray/logo.svg"
output_dir="$repo_root/resources/images"

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick 7 (magick) is required to regenerate tray icons." >&2
    exit 1
fi

if [[ ! -f "$logo" ]]; then
    echo "Vector tray logo not found: $logo" >&2
    exit 1
fi

generate_icon() {
    local name="$1"
    local logo_color="$2"

    for size in 32 64 96; do
        local suffix=""
        if (( size == 64 )); then suffix="@2x"; fi
        if (( size == 96 )); then suffix="@3x"; fi

        # Rasterize the tightly framed vector separately at every DPI size.
        # Do not resize a small PNG or retain the application logo's padding.
        sed -e "s/#253851/$logo_color/g" \
            -e "s/width=\"128\" height=\"128\"/width=\"$size\" height=\"$size\"/" \
            "$logo" > "$temporary_dir/mark.svg"
        magick -background none "$temporary_dir/mark.svg" \
            -depth 8 -strip -define png:color-type=6 \
            "$output_dir/${name}${suffix}.png"
    done
}

mkdir -p "$output_dir"
temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
# Keep the entire background transparent. The vector's narrow white keyline
# makes the colored cat-in-box mark readable on light and dark desktop trays.
generate_icon tray_default '#253851'
generate_icon tray_tun '#0BAA9E'
generate_icon tray_system_proxy '#7451C2'
