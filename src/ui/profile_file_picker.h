#pragma once

#include <exception>
#include <memory>
#include <optional>

#include "ui.h"

namespace clashflux::ui {

// 只在组合期解析本地化资源；交给事件/协程的是拥有型普通 DTO。
inline huxerui::FilePickerFilter ProfileConfigFileFilter() {
    return {.name = huxerui::UseString(Localized("sing-box / Clash 配置")),
            .extensions = {"json", "yaml", "yml"}};
}

inline huxerui::Task<std::optional<huxerui::FileReference>> PickProfileConfigFile(
    std::shared_ptr<huxerui::FilePicker> picker, huxerui::FilePickerFilter filter,
    huxerui::ToastHandle toast) {
    if (!picker) {
        toast.Show(Localized("文件选择器不可用"));
        co_return std::nullopt;
    }
    try {
        co_return co_await picker->OpenFileAsync(std::move(filter));
    } catch (const std::exception& error) {
        toast.Show(LocalizedFormat("选择文件失败：{}", error.what()));
        co_return std::nullopt;
    }
}

} // namespace clashflux::ui
