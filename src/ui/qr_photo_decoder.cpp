#include "qr_photo_decoder.h"

#if defined(__ANDROID__)

#include <android/bitmap.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/huxerui.h>
#include <huxerui/qrcode/codec.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace {

class LockedQrBitmap final {
public:
    LockedQrBitmap(JNIEnv* env, jobject bitmap) : env_(env), bitmap_(bitmap) {
        if (AndroidBitmap_getInfo(env_, bitmap_, &info_) != ANDROID_BITMAP_RESULT_SUCCESS ||
            info_.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
            info_.width == 0 || info_.height == 0 ||
            info_.width > 1600 || info_.height > 1600 ||
            info_.stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            info_.stride < info_.width * 4) {
            throw std::runtime_error("Invalid QR bitmap");
        }
        if (AndroidBitmap_lockPixels(env_, bitmap_, &pixels_) != ANDROID_BITMAP_RESULT_SUCCESS) {
            throw std::runtime_error("Unable to read QR bitmap pixels");
        }
    }
    LockedQrBitmap(const LockedQrBitmap&) = delete;
    LockedQrBitmap& operator=(const LockedQrBitmap&) = delete;
    ~LockedQrBitmap() { AndroidBitmap_unlockPixels(env_, bitmap_); }

    huxerui::qrcode::ImageView View() const {
        return {
            {static_cast<const std::uint8_t*>(pixels_),
             static_cast<std::size_t>(info_.stride) * info_.height},
            static_cast<int>(info_.width),
            static_cast<int>(info_.height),
            huxerui::qrcode::PixelFormat::RGBA8,
            static_cast<int>(info_.stride)
        };
    }

private:
    JNIEnv* env_;
    jobject bitmap_;
    AndroidBitmapInfo info_{};
    void* pixels_ = nullptr;
};

} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_dev_farna_clashflux_QrPhotoDecoderModule_decodePixels(JNIEnv* env, jclass, jobject bitmap) {
    namespace qr = huxerui::qrcode;
    try {
        const LockedQrBitmap pixels{env, bitmap};
        const auto decoded = qr::Decode(pixels.View(), {.formats = {qr::BarcodeFormat::QRCode}});
        const auto size = decoded ? static_cast<jsize>(decoded->text.size()) : 0;
        const auto bytes = env->NewByteArray(size);
        if (bytes && size != 0) {
            env->SetByteArrayRegion(bytes, 0, size,
                reinterpret_cast<const jbyte*>(decoded->text.data()));
        }
        return bytes;
    } catch (const std::exception&) {
        if (!env->ExceptionCheck()) {
            const auto error = env->FindClass("java/lang/IllegalStateException");
            if (error) env->ThrowNew(error, "Unable to decode QR bitmap");
        }
        return nullptr;
    }
}

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
