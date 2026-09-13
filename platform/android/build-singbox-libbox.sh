#!/usr/bin/env bash
set -euo pipefail

# Corresponding source for the generated libbox.aar.  Never source native
# libraries from a third-party APK: build precisely the pinned upstream core.
readonly repo='https://github.com/SagerNet/sing-box.git'
readonly revision="${SINGBOX_COMMIT:-0b8995879f29a9b98ee027bc17b75e101445b238}" # v1.14.0
readonly output="${1:?usage: build-singbox-libbox.sh OUTPUT_AAR}"

command -v go >/dev/null || { echo 'Go is required.' >&2; exit 1; }
test -n "${ANDROID_NDK_HOME:-}" || { echo 'ANDROID_NDK_HOME is required.' >&2; exit 1; }
test -n "${JAVA_HOME:-}" || { echo 'JAVA_HOME (JDK 17) is required.' >&2; exit 1; }

readonly work="$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-singbox.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$(dirname "$(realpath -m "$output")")"
git clone --filter=blob:none "$repo" "$work/source"
git -C "$work/source" checkout --detach "$revision"
test "$(git -C "$work/source" rev-parse HEAD)" = "$revision"
pushd "$work/source" >/dev/null
make lib_install
export PATH="$PATH:$(go env GOPATH)/bin"
go run ./cmd/internal/build_libbox -target android -platform android/arm64
test -s libbox.aar
mv libbox.aar "$output"
popd >/dev/null
