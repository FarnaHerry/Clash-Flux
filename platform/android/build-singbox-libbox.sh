#!/usr/bin/env bash
set -euo pipefail

# Corresponding source for the generated libbox.aar.  Never source native
# libraries from a third-party APK: build precisely the pinned upstream core.
readonly repo='https://github.com/SagerNet/sing-box.git'
readonly version="${SINGBOX_VERSION:-1.14.2}"
readonly revision="${SINGBOX_COMMIT:-af6e64c3b69e6132ebaee0e1a3d24e93903f6709}" # v1.14.2
readonly output="$(realpath -m "${1:?usage: build-singbox-libbox.sh OUTPUT_AAR}")"

command -v go >/dev/null || { echo 'Go is required.' >&2; exit 1; }
test -n "${ANDROID_NDK_HOME:-}" || { echo 'ANDROID_NDK_HOME is required.' >&2; exit 1; }
test -n "${JAVA_HOME:-}" || { echo 'JAVA_HOME (JDK 17) is required.' >&2; exit 1; }
java_version="$("$JAVA_HOME/bin/java" --version)"
[[ "$java_version" == *"openjdk 17"* ]] || {
    echo 'Upstream libbox requires OpenJDK 17; choose a matching JAVA_HOME.' >&2
    exit 1
}

readonly work="$(mktemp -d "${TMPDIR:-/tmp}/clash-flux-singbox.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$(dirname "$(realpath -m "$output")")"
# Fetch only the pinned commit: a full history clone can stall on large upstream
# repositories and is unnecessary for reproducible libbox builds.
git init -q "$work/source"
git -C "$work/source" remote add origin "$repo"
git -C "$work/source" -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=60 \
    fetch --depth=1 origin "refs/tags/v${version}:refs/tags/v${version}"
git -C "$work/source" checkout --detach "v${version}"
test "$(git -C "$work/source" rev-parse HEAD)" = "$revision"
test "$(git -C "$work/source" describe --tags --exact-match HEAD)" = "v${version}"
pushd "$work/source" >/dev/null
make lib_install
export PATH="$PATH:$(go env GOPATH)/bin"
go run ./cmd/internal/build_libbox -target android -platform android/arm64
test -s libbox.aar
mv libbox.aar "$output"
popd >/dev/null

# Gradle must identify cached AARs by source revision and content, not by their
# filename alone. This sidecar travels with CLASHFLUX_SINGBOX_AAR in CI/local builds.
readonly aar_sha256="$(sha256sum "$output" | cut -d ' ' -f 1)"
printf '{"version":"%s","revision":"%s","sha256":"%s","abi":"arm64-v8a"}\n' \
    "$version" "$revision" "$aar_sha256" > "$output.metadata.json.tmp"
mv "$output.metadata.json.tmp" "$output.metadata.json"
