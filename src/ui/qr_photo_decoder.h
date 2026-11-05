#pragma once

#if defined(__ANDROID__)

#include <functional>
#include <memory>
#include <string>

#include <huxerui/platform_registry.h>
#include <huxerui/app.h>

namespace clashflux::ui {

class QrPhotoDecoder {
public:
    explicit QrPhotoDecoder(huxerui::PlatformChannel channel);
    ~QrPhotoDecoder();

    huxerui::PlatformRequestId Decode(
        huxerui::Bytes jpeg,
        std::function<void(huxerui::PlatformResult<std::string>)> completed) const;
    huxerui::Task<std::string> DecodeAsync(huxerui::Bytes image) const;
    huxerui::Task<std::string> DecodeAsync(huxerui::FileReference image) const;
    bool Cancel(huxerui::PlatformRequestId request) const;

private:
    huxerui::PlatformChannel channel_;
};

void InstallQrPhotoDecoder(huxerui::ApplicationContext& context);
std::shared_ptr<QrPhotoDecoder> OpenQrPhotoDecoder();

} // namespace clashflux::ui

#endif
