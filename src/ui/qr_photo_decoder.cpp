#include "qr_photo_decoder.h"

#if defined(__ANDROID__)

#include <huxerui/android/platform_registry.h>
#include <huxerui/huxerui.h>

#include <utility>

namespace clashflux::ui {
namespace {

constexpr char kQrPhotoDecoderModule[] = "clashflux/QrPhotoDecoder";

huxerui::Task<std::string> DecodeQrImage(huxerui::PlatformChannel channel,
    std::string method, huxerui::PlatformPayload image) {
    co_return co_await huxerui::AwaitCallback<std::string>(
        [channel, method = std::move(method), image = std::move(image)](auto completion) {
            const auto request = channel.Invoke<std::string>(method, image, [completion](auto result) {
                if (const auto* error = std::get_if<huxerui::PlatformError>(&result))
                    completion.Fail(std::make_exception_ptr(std::runtime_error(error->message)));
                else completion.Complete(std::get<std::string>(std::move(result)));
            });
            return [channel, request] { channel.Cancel(request); };
        });
}

} // namespace

QrPhotoDecoder::QrPhotoDecoder(huxerui::PlatformChannel channel)
    : channel_(std::move(channel)) {}

QrPhotoDecoder::~QrPhotoDecoder() { channel_.Close(); }

huxerui::PlatformRequestId QrPhotoDecoder::Decode(
    huxerui::Bytes jpeg,
    std::function<void(huxerui::PlatformResult<std::string>)> completed) const {
    return channel_.Invoke<std::string>("decode", jpeg, std::move(completed));
}

bool QrPhotoDecoder::Cancel(huxerui::PlatformRequestId request) const {
    return channel_.Cancel(request);
}

huxerui::Task<std::string> QrPhotoDecoder::DecodeAsync(huxerui::Bytes image) const {
    return DecodeQrImage(channel_, "decode", std::move(image));
}

huxerui::Task<std::string> QrPhotoDecoder::DecodeAsync(huxerui::FileReference image) const {
    return DecodeQrImage(channel_, "decodeFile", std::move(image));
}

void InstallQrPhotoDecoder(huxerui::ApplicationContext& context) {
    huxerui::android::JavaPlatformModuleFactory<
        std::shared_ptr<QrPhotoDecoder>> factory;
    factory.class_name = "dev.farna.clashflux.QrPhotoDecoderModule";
    factory.create = [](huxerui::PlatformChannel channel) {
        return std::make_shared<QrPhotoDecoder>(std::move(channel));
    };
    context.RegisterPlatformModule<std::shared_ptr<QrPhotoDecoder>>(
        kQrPhotoDecoderModule, std::move(factory));
}

std::shared_ptr<QrPhotoDecoder> OpenQrPhotoDecoder() {
    return huxerui::OpenPlatformModule<std::shared_ptr<QrPhotoDecoder>>(
        kQrPhotoDecoderModule);
}

} // namespace clashflux::ui

#endif
