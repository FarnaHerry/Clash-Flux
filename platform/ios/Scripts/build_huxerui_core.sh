#!/usr/bin/env bash
set -euo pipefail

: "${CLASHFLUX_PROJECT_ROOT:?Xcode must provide CLASHFLUX_PROJECT_ROOT}"
: "${HUXERUI_CORE_BUILD_DIR:?Xcode must provide HUXERUI_CORE_BUILD_DIR}"
: "${SDKROOT:?Xcode must provide SDKROOT}"

llvm_prefix="${CLASHFLUX_LLVM_PREFIX:-$(brew --prefix llvm)}"
module_dir="$llvm_prefix/share/libc++/v1"
modules_json="$HUXERUI_CORE_BUILD_DIR/libc++.modules.json"

# 核心库必须与 Xcode 侧 app/extension 使用同一套 libc++：全部采用 iOS SDK
# 的头文件（-nostdinc++ 屏蔽 brew llvm 自带的 libc++ 头），否则 brew 的
# 新版 ABI 运行时符号（__atomic_monitor_global 等）在 OS libc++ 里不存在，
# app 链接时成批未定义。std 模块包装仍用 brew 的 std.cppm，但其中的
# #include 全部按名字解析到 SDK 头。
sdk_cxx_include="$SDKROOT/usr/include/c++/v1"
test -d "$sdk_cxx_include" || {
    echo "Missing SDK libc++ headers: $sdk_cxx_include" >&2
    exit 1
}

mkdir -p "$HUXERUI_CORE_BUILD_DIR"
for module in std.cppm std.compat.cppm; do
    test -f "$module_dir/$module" || {
        echo "Missing libc++ module source: $module_dir/$module" >&2
        exit 1
    }
done
python3 - "$module_dir" "$modules_json" "$sdk_cxx_include" <<'PY'
import json
import sys

module_dir, output, sdk_include = sys.argv[1:]
modules = []
for name in ("std", "std.compat"):
    modules.append({
        "logical-name": name,
        "source-path": f"{module_dir}/{name}.cppm",
        "is-std-library": True,
        "local-arguments": {
            "system-include-directories": [sdk_include],
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
    -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem $sdk_cxx_include -D_LIBCPP_DISABLE_AVAILABILITY -D_LIBCPP_ENABLE_EXPERIMENTAL" \
    -DCMAKE_OBJCXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem $sdk_cxx_include -D_LIBCPP_DISABLE_AVAILABILITY -D_LIBCPP_ENABLE_EXPERIMENTAL" \
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
