#pragma once

#include <memory>
#include <optional>
#include <string>

#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

// 通用「拖入或点击」文件选区，定义在 common.cpp。FileDropTarget 按扩展名对
// 外部拖入做整批校验（要么全收要么拒绝），读取与导入由调用方决定；点击走
// FilePicker——键盘、触摸与不支持拖入的宿主都依赖这个等价入口。title 与
// filter.name 在组合期解析；选中后回写 FileReference，由调用方保留访问权。
huxerui::View FileDropZone(
    huxerui::ImageVariant icon, huxerui::StringVariant title,
    huxerui::State<std::optional<huxerui::FileReference>> picked_file, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    huxerui::FilePickerFilter filter, std::string key = "file-drop-zone");

} // namespace clashflux::ui
