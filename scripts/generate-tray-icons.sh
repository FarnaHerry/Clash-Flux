#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
logo="$repo_root/resources/images/clash_flux_logo.png"
output_dir="$repo_root/resources/images"

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick 7 (magick) is required to regenerate tray icons." >&2
    exit 1
fi

if [[ ! -f "$logo" ]]; then
    echo "Application logo not found: $logo" >&2
    exit 1
fi

generate_icon() {
    local name="$1"
    local logo_color="$2"

    for size in 32 64 96; do
        local logo_size=$((size * 7 / 8))
        local outline_width=$((size / 32))
        local suffix=""
        if (( size == 64 )); then suffix="@2x"; fi
        if (( size == 96 )); then suffix="@3x"; fi

        magick "$logo" -filter Lanczos -resize "${logo_size}x${logo_size}" \
            -alpha extract -morphology Dilate "Disk:$outline_width" \
            "$temporary_dir/outline-mask.png"
        magick -size "${logo_size}x${logo_size}" xc:white \
            "$temporary_dir/outline-mask.png" -compose CopyOpacity -composite \
            "$temporary_dir/outline.png"
        magick "$logo" -filter Lanczos -resize "${logo_size}x${logo_size}" \
            -fill "$logo_color" -colorize 100 "$temporary_dir/mark.png"
        magick -size "${size}x${size}" xc:none \
            "$temporary_dir/outline.png" -gravity center -composite \
            "$temporary_dir/mark.png" -gravity center -composite \
            -depth 8 -strip -define png:color-type=6 \
            "$output_dir/${name}${suffix}.png"
    done
}

mkdir -p "$output_dir"
temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
# Keep the entire background transparent. A narrow white keyline makes the
# colored cat-in-box mark readable on both light and dark desktop trays.
generate_icon tray_default '#253851'
generate_icon tray_tun '#0BAA9E'
generate_icon tray_system_proxy '#7451C2'
