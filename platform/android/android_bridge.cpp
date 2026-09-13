// android_bridge.cpp — Java/NDK glue kept in the Clash-Flux app library.
//
// HuxerUI owns the rendering Activity and application directories. Clash-Flux
// keeps a small URL bridge and starts the bundled mihomo process after the
// Android shell has extracted it.
#include <jni.h>

#include <unistd.h>

#include <atomic>
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
// 「跟随系统」主题用：MainActivity 在每次 onCreate 时上报（系统深浅切换会
// 重建 Activity，uiMode 不在 configChanges 里），因此缓存总是新鲜的。
std::atomic<bool> g_system_dark{false};
// Owned by ClashVpnService, never inferred from a saved setting.
// 0=closed, 1=building, 2=attached, 3=failed.
std::atomic<int> g_vpn_state{0};

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
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            try {
                auto& core = store::coreStore();
                core.init();
                core.startCore(store::profilesStore().selectedYaml());
                const auto snapshot = core.snapshot();
                log_android(("Android embedded core startup state=" +
                             std::to_string(static_cast<int>(snapshot.state)) +
                             " error=" + snapshot.lastError)
                                .c_str(),
                            snapshot.state == core::CoreState::Failed);

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

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeSetSystemDark(JNIEnv*, jclass,
                                                          jboolean dark) {
    g_system_dark.store(dark == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeVpnStartCancelled(JNIEnv*, jclass) {
    // VpnService.prepare 的授权框被取消时，不能留下一个看似已启用的偏好。
    try {
        store::coreStore().setSetting("core.tun_enabled", "false");
    } catch (...) {
        log_android("Failed to roll back VPN setting after declined consent", true);
    }
}

// cfg::systemPrefersDark() 的 Android 后端（见 src/config.cppm）。
extern "C" bool clashflux_android_system_dark() noexcept {
    return g_system_dark.load();
}

// ---- Embedded core / VPN state --------------------------------------------

extern "C" bool clashflux_android_start_embedded_mihomo(const char* home) noexcept {
    if (home == nullptr) return false;
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return false;
    bool ok = false;
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "startEmbeddedCore", "(Ljava/lang/String;)Z")) {
        jstring path = environment->NewStringUTF(home);
        if (path != nullptr) {
            ok = environment->CallStaticBooleanMethod(g_activity_class, method, path) == JNI_TRUE;
            environment->DeleteLocalRef(path);
        }
    }
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        ok = false;
    }
    if (attached) g_vm->DetachCurrentThread();
    return ok;
}

extern "C" void clashflux_android_stop_embedded_mihomo() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment != nullptr && g_activity_class != nullptr) {
        if (jmethodID method = environment->GetStaticMethodID(
                g_activity_class, "stopEmbeddedCore", "()V")) {
            environment->CallStaticVoidMethod(g_activity_class, method);
            if (environment->ExceptionCheck()) environment->ExceptionClear();
        }
    }
    if (attached) g_vm->DetachCurrentThread();
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_ClashVpnService_nativeVpnState(JNIEnv* environment, jclass,
                                                        jint state, jstring message) {
    g_vpn_state.store(state);
    std::string text;
    if (message != nullptr) {
        if (const char* value = environment->GetStringUTFChars(message, nullptr)) {
            text = value;
            environment->ReleaseStringUTFChars(message, value);
        }
    }
    log_android(("VPN state=" + std::to_string(state) + " " + text).c_str(), state == 3);
    if (state == 0 || state == 3) {
        try { store::coreStore().setSetting("core.tun_enabled", "false"); } catch (...) {}
    }
}

extern "C" int clashflux_android_vpn_state() noexcept { return g_vpn_state.load(); }

// 设置页 VPN 开关 → MainActivity.startVpn/stopVpn（consent 弹窗/前台服务）。
extern "C" void clashflux_android_start_vpn() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return;
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "startVpn", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

extern "C" void clashflux_android_stop_vpn() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return;
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "stopVpn", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

// 已豁免时 Java 侧直接 return（不弹窗）。
extern "C" void clashflux_android_request_ignore_battery() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return;
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "requestIgnoreBatteryOptimizations", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
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
