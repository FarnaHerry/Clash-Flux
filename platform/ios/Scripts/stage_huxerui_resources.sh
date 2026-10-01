#!/usr/bin/env bash
set -euo pipefail

source="$HUXERUI_CORE_BUILD_DIR/huxerui-ios/clash-flux/resources/package"
destination="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/HuxerUI"
test -s "$source/huxerui/resources.bin"
cmake -E remove_directory "$destination"
cmake -E make_directory "$destination"
cmake -E copy_directory "$source" "$destination"
