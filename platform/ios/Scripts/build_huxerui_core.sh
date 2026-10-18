#!/usr/bin/env bash
set -euo pipefail

: "${CLASHFLUX_PROJECT_ROOT:?Xcode must provide CLASHFLUX_PROJECT_ROOT}"
: "${HUXERUI_CORE_BUILD_DIR:?Xcode must provide HUXERUI_CORE_BUILD_DIR}"
: "${SDKROOT:?Xcode must provide SDKROOT}"

llvm_prefix="${CLASHFLUX_LLVM_PREFIX:-$(brew --prefix llvm@19)}"
module_dir="$llvm_prefix/share/libc++/v1"
modules_json="$HUXERUI_CORE_BUILD_DIR/libc++.modules.json"

# 核心库必须与 Xcode 侧 app/extension 使用同一套 libc++ ABI：头文件统一
# 采用 iOS SDK 的（-nostdinc++ 屏蔽工具链自带的头），工具链钉在 LLVM 19
# 与 SDK/OS dylib 同世代；std 模块包装用工具链自带的 std.cppm（与 SDK 头
# 同版本家族，不会导出 SDK 缺失的头），其中的 #include 按名字解析到 SDK 头。
sdk_cxx_include="$SDKROOT/usr/include/c++/v1"
test -d "$sdk_cxx_include" || {
    echo "Missing SDK libc++ headers: $sdk_cxx_include" >&2
    exit 1
}
for module in std.cppm std.compat.cppm; do
    test -f "$module_dir/$module" || {
        echo "Missing std module source: $module_dir/$module" >&2
        exit 1
    }
done

mkdir -p "$HUXERUI_CORE_BUILD_DIR"
python3 - "$module_dir" "$sdk_cxx_include" "$modules_json" <<'PY'
import json
import sys

module_dir, sdk_include, output = sys.argv[1:]
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
