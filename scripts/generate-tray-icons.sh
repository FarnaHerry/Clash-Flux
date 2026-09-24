#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
palette="$repo_root/design-assets/tray-logo-palette.png"
output_dir="$repo_root/resources/images"

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick 7 (magick) is required to regenerate tray icons." >&2
    exit 1
fi

if [[ ! -f "$palette" ]]; then
    echo "Tray logo palette not found: $palette" >&2
    exit 1
fi

generate_icon() {
    local name="$1"
    local x="$2"
    local y="$3"

    for size in 32 64 96; do
        local inset=$((size / 16))
        local content_size=$((size - inset * 2))
        local suffix=""
        if (( size == 64 )); then suffix="@2x"; fi
        if (( size == 96 )); then suffix="@3x"; fi

        magick "$palette" \
            -crop "256x256+$x+$y" +repage \
            -alpha set -fuzz 2% -fill none -draw 'color 0,0 floodfill' \
            -trim +repage \
            -resize "${content_size}x${content_size}" \
            -gravity center -background none -extent "${size}x${size}" \
            -depth 8 -strip -define png:color-type=6 \
            "$output_dir/${name}${suffix}.png"
    done
}

mkdir -p "$output_dir"
# Palette is a 6x4 grid of 256 px cells. Keep a high-contrast navy default,
# teal TUN, and purple system-proxy mark for readability in small tray sizes.
generate_icon tray_default 0 0
generate_icon tray_tun 768 0
generate_icon tray_system_proxy 0 256
