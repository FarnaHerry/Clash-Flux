#!/usr/bin/env bash
set -euo pipefail

# Build the exact sing-box revision used by Android and desktop as an Apple
# Libbox.xcframework. This produces engine slices only; the Xcode app and
# Packet Tunnel targets consume the framework in a later integration step.
readonly repo='https://github.com/SagerNet/sing-box.git'
readonly revision="${SINGBOX_COMMIT:-af6e64c3b69e6132ebaee0e1a3d24e93903f6709}" # v1.14.2
readonly output_arg="${1:?usage: build-singbox-libbox.sh OUTPUT_XCFRAMEWORK}"

for command in go git make xcodebuild xcrun ditto; do
    command -v "$command" >/dev/null || {
        echo "$command is required to build the Apple Libbox framework." >&2
        exit 1
    }
done

xcrun --sdk iphoneos --show-sdk-path >/dev/null
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null

output_parent="$(dirname "$output_arg")"
mkdir -p "$output_parent"
output_parent="$(cd "$output_parent" && pwd)"
readonly output="$output_parent/$(basename "$output_arg")"
if [[ -e "$output" ]]; then
    echo "Output already exists: $output" >&2
    exit 1
fi

readonly work="$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-singbox-apple.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

git clone --filter=blob:none "$repo" "$work/source"
git -C "$work/source" checkout --detach "$revision"
test "$(git -C "$work/source" rev-parse HEAD)" = "$revision"

pushd "$work/source" >/dev/null
make lib_install
export PATH="$(go env GOPATH)/bin:$PATH"
go run ./cmd/internal/build_libbox -target apple -platform ios,iossimulator
popd >/dev/null

framework="$(find "$work/source" -maxdepth 1 -type d -name '*.xcframework' -print -quit)"
if [[ -z "$framework" ]]; then
    echo 'sing-box did not produce an Apple XCFramework.' >&2
    exit 1
fi
ditto "$framework" "$output"
test -s "$output/Info.plist"
echo "Built pinned sing-box Libbox XCFramework: $output"
