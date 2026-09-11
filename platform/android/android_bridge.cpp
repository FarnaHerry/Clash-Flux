// android_bridge.cpp — Java/NDK glue kept in the Clash-Flux app library.
//
// HuxerUI owns the rendering Activity and application directories. Clash-Flux
// keeps a small URL bridge and starts the bundled mihomo process after the
// Android shell has extracted it.
#include <jni.h>

#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "clashflux_android_legacy.h"

namespace {

std::mutex g_mutex;
JavaVM* g_vm = nullptr;
jclass g_activity_class = nullptr;
std::once_flag g_core_start_once;

void log_android(const char* message, bool error = false) noexcept {
#if defined(__ANDROID__)
    __android_log_print(error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "ClashFlux",
                        "%s", message);
#else
    static_cast<void>(message);
    static_cast<void>(error);
#endif
}

JNIEnv* current_environment(bool& attached) noexcept {
    attached = false;
    if (g_vm == nullptr) return nullptr;

    JNIEnv* environment = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) ==
        JNI_OK) {
        return environment;
    }
    if (g_vm->AttachCurrentThread(&environment, nullptr) != JNI_OK) {
        return nullptr;
    }
    attached = true;
    return environment;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeInit(JNIEnv* environment,
                                                  jclass activity_class,
                                                  jstring files_directory,
                                                  jstring native_library_directory) {
    if (environment == nullptr || activity_class == nullptr) {
        return;
    }

    if (files_directory != nullptr) {
        const char* value = environment->GetStringUTFChars(files_directory, nullptr);
        if (value != nullptr) {
            cfg::setAndroidDataDir(value);
            environment->ReleaseStringUTFChars(files_directory, value);
        }
    }
    if (native_library_directory != nullptr) {
        const char* value = environment->GetStringUTFChars(native_library_directory, nullptr);
        if (value != nullptr) {
            cfg::setAndroidNativeLibraryDir(value);
            environment->ReleaseStringUTFChars(native_library_directory, value);
        }
    }

    {
        std::lock_guard lock(g_mutex);
        if (g_vm == nullptr) {
            environment->GetJavaVM(&g_vm);
            g_activity_class =
                static_cast<jclass>(environment->NewGlobalRef(activity_class));
        }
    }

    log_android("Native bridge initialized with Android files directory");
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeStartCore(JNIEnv*, jclass) {
    std::call_once(g_core_start_once, [] {
        std::thread([] {
            try {
                auto& core = store::coreStore();
                core.init();
                const std::filesystem::path binary = cfg::mihomoBinary();
                const std::string path = binary.empty() ? "<missing>" : binary.string();
                log_android(("Android mihomo path: " + path).c_str(), binary.empty());
                if (!binary.empty()) {
                    core.startCore(store::profilesStore().selectedYaml());
                    const auto snapshot = core.snapshot();
                    log_android(("Android core startup state=" +
                                 std::to_string(static_cast<int>(snapshot.state)) +
                                 " error=" + snapshot.lastError)
                                    .c_str(),
                                snapshot.state == core::CoreState::Failed);
                }

                // Keep the process state fresh even when the first HuxerUI
                // frame is delayed. UI pages independently poll snapshots and
                // will observe the same state once they compose.
                for (;;) {
                    core.checkAlive();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            } catch (const std::exception& error) {
                log_android(("Android core initialization failed: " +
                             std::string{error.what()})
                                .c_str(),
                            true);
            } catch (...) {
                log_android("Android core initialization failed: unknown exception", true);
            }
        }).detach();
    });
}

extern "C" void clashflux_android_open_url(const char* url) noexcept {
    if (url == nullptr || *url == '\0') return;

    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return;

    jmethodID method = environment->GetStaticMethodID(
        g_activity_class, "openUrl", "(Ljava/lang/String;)V");
    if (method != nullptr) {
        jstring value = environment->NewStringUTF(url);
        if (value != nullptr) {
            environment->CallStaticVoidMethod(g_activity_class, method, value);
            environment->DeleteLocalRef(value);
        }
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
}
