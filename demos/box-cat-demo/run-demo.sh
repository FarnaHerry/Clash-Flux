#!/usr/bin/env bash
set -euo pipefail

demo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$demo_dir/../.." && pwd)"
demo_cmake_args=(-DHUXERUI_HOME="$repo_dir/third_party/huxerui")
demo_tools_dir="$repo_dir/third_party/huxerui-tools"
if [[ -x "$demo_tools_dir/linux/$(uname -m)/hcg" && -x "$demo_tools_dir/linux/$(uname -m)/hrc" ]]; then
  demo_cmake_args+=(-DHUXERUI_HOST_TOOL_ROOT="$demo_tools_dir")
else
  demo_cmake_args+=(-U HUXERUI_HOST_TOOL_ROOT)
fi
cmake -S "$demo_dir" -B "$demo_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug "${demo_cmake_args[@]}"
demo_target=box_cat_demo
if [[ "${1:-}" == --restart-preview ]]; then
  demo_target=box_cat_restart_demo
  shift
fi
cmake --build "$demo_dir/build" --target "$demo_target" --parallel 2
exec "$demo_dir/build/$demo_target" "$@"
