#!/usr/bin/env bash
set -euo pipefail

: "${CLASHFLUX_PROJECT_ROOT:?Xcode must provide CLASHFLUX_PROJECT_ROOT}"
: "${HUXERUI_CORE_BUILD_DIR:?Xcode must provide HUXERUI_CORE_BUILD_DIR}"
: "${SDKROOT:?Xcode must provide SDKROOT}"

llvm_prefix="${CLASHFLUX_LLVM_PREFIX:-$(brew --prefix llvm)}"
module_dir="$llvm_prefix/share/libc++/v1"
modules_json="$HUXERUI_CORE_BUILD_DIR/libc++.modules.json"
mkdir -p "$HUXERUI_CORE_BUILD_DIR"
for module in std.cppm std.compat.cppm; do
    test -f "$module_dir/$module" || {
        echo "Missing libc++ module source: $module_dir/$module" >&2
        exit 1
    }
done
python3 - "$module_dir" "$modules_json" <<'PY'
import json
import sys

module_dir, output = sys.argv[1:]
modules = []
for name in ("std", "std.compat"):
    modules.append({
        "logical-name": name,
        "source-path": f"{module_dir}/{name}.cppm",
        "is-std-library": True,
        "local-arguments": {
            "system-include-directories": [module_dir],
        },
    })
with open(output, "w", encoding="utf-8") as stream:
    json.dump({"version": 1, "revision": 1, "modules": modules}, stream, indent=1)
PY

cmake_architectures="$(printf '%s' "$ARCHS" | tr ' ' ';')"
cmake -S "$CLASHFLUX_PROJECT_ROOT" -B "$HUXERUI_CORE_BUILD_DIR" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DCMAKE_OSX_ARCHITECTURES="$cmake_architectures" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
    -DCMAKE_C_COMPILER="$llvm_prefix/bin/clang" \
    -DCMAKE_CXX_COMPILER="$llvm_prefix/bin/clang++" \
    -DCMAKE_OBJCXX_COMPILER="$llvm_prefix/bin/clang++" \
    -DCMAKE_CXX_FLAGS='-stdlib=libc++ -D_LIBCPP_DISABLE_AVAILABILITY' \
    -DCMAKE_OBJCXX_FLAGS='-stdlib=libc++ -D_LIBCPP_DISABLE_AVAILABILITY' \
    -DCMAKE_CXX_STDLIB_MODULES_JSON="$modules_json" \
    -DHUXERUI_BUILD_SHARED=OFF \
    -DHUXERUI_BUILD_STATIC=ON \
    -DHUXERUI_BUILD_TESTS=OFF \
    -DHUXERUI_BUILD_EXAMPLES=OFF \
    -DHUXERUI_BUILD_CLI=OFF \
    -DCLASHFLUX_BUILD_TESTS=OFF \
    -DCLASHFLUX_BUNDLE_SINGBOX=OFF

cmake --build "$HUXERUI_CORE_BUILD_DIR" \
    --target clash-flux_huxerui_ios_core --parallel
test -s "$HUXERUI_CORE_BUILD_DIR/huxerui-ios/clash-flux/libclash-flux_huxerui.a"
test -s "$HUXERUI_LINK_OPTIONS_FILE"
