#include "qr_photo_decoder.h"

#if defined(__ANDROID__)

#include <huxerui/android/platform_registry.h>
#include <huxerui/huxerui.h>

#include <utility>

namespace clashflux::ui {
namespace {

constexpr char kQrPhotoDecoderModule[] = "clashflux/QrPhotoDecoder";

} // namespace

QrPhotoDecoder::QrPhotoDecoder(huxerui::PlatformChannel channel)
    : channel_(std::move(channel)) {}

huxerui::PlatformRequestId QrPhotoDecoder::Decode(
    huxerui::Bytes jpeg,
    std::function<void(huxerui::PlatformResult<std::string>)> completed) const {
    return channel_.Invoke<std::string>("decode", jpeg, std::move(completed));
}

bool QrPhotoDecoder::Cancel(huxerui::PlatformRequestId request) const {
    return channel_.Cancel(request);
}

void InstallQrPhotoDecoder(huxerui::RootContext& root) {
    huxerui::android::JavaPlatformModuleFactory<
        std::shared_ptr<QrPhotoDecoder>> factory;
    factory.class_name = "dev.farna.clashflux.QrPhotoDecoderModule";
    factory.create = [](huxerui::PlatformChannel channel) {
        return std::make_shared<QrPhotoDecoder>(std::move(channel));
    };
    root.RegisterPlatformModule<std::shared_ptr<QrPhotoDecoder>>(
        kQrPhotoDecoderModule, std::move(factory));
    root.Provide(root.OpenPlatformModule<std::shared_ptr<QrPhotoDecoder>>(
        kQrPhotoDecoderModule));
}

} // namespace clashflux::ui

#endif
