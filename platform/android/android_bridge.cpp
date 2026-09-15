// android_bridge.cpp — Java/NDK glue kept in the Clash-Flux app library.
//
// HuxerUI owns the rendering Activity and application directories. Clash-Flux
// keeps a small URL bridge and receives state from the sing-box VPN service.
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
    try {
        stream::logApplication(error ? "error" : "info", message);
    } catch (...) {
        // Diagnostics must never interfere with the JNI callback itself.
    }
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
Java_dev_farna_clashflux_MainActivity_nativeAppLog(JNIEnv* environment,
                                                    jclass,
                                                    jint level,
                                                    jstring message) {
    if (environment == nullptr || message == nullptr) return;
    const char* value = environment->GetStringUTFChars(message, nullptr);
    if (value == nullptr) return;
    const char* name = level == 3 ? "error" : level == 2 ? "warning" :
                       level == 4 ? "debug" : "info";
    try {
        stream::logApplication(name, value);
    } catch (...) {
    }
    environment->ReleaseStringUTFChars(message, value);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeStartCore(JNIEnv*, jclass) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            try {
                auto& core = store::coreStore();
                core.init();
                // Android's real data plane is created only after the user
                // grants VpnService consent. Preparing a config here makes
                // the UI race that service and leaves the native state stuck
                // at Stopped, so startup only initializes the store/monitor.
                log_android("Android native store initialized; waiting for VPN consent");

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
    g_vpn_state.store(0);
    try {
        store::coreStore().setAndroidRuntimeState(0, "");
        store::coreStore().setSetting("core.tun_enabled", "false");
    } catch (...) {
        log_android("Failed to roll back VPN setting after declined consent", true);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeVpnStartFailed(JNIEnv* environment,
                                                            jclass,
                                                            jstring message) {
    g_vpn_state.store(3);
    std::string text = "Android VPN 启动失败";
    if (environment != nullptr && message != nullptr) {
        if (const char* value = environment->GetStringUTFChars(message, nullptr)) {
            text = value;
            environment->ReleaseStringUTFChars(message, value);
        }
    }
    log_android(text.c_str(), true);
    try {
        store::coreStore().stopAndroidApiStreams();
        store::coreStore().setAndroidRuntimeState(3, text);
        store::coreStore().setSetting("core.tun_enabled", "false");
    } catch (...) {
        log_android("Failed to persist Android VPN startup failure", true);
    }
}

// cfg::systemPrefersDark() 的 Android 后端（见 src/config.cppm）。
extern "C" bool clashflux_android_system_dark() noexcept {
    return g_system_dark.load();
}

// ---- sing-box VPN state ----------------------------------------------------

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
    try { store::coreStore().setAndroidRuntimeState(state, text); } catch (...) {}
    if (state == 2) {
        // Android libbox exposes its status API through the Java-side
        // CommandClient.  It does not host the desktop clash_api HTTP/WS
        // controller on 127.0.0.1:9097.  Starting CoreStreams here would
        // create three invalid IXWebSocket channels immediately after TUN
        // attach, and on some Android builds the process dies when those
        // channels time out.  Keep the native bridge limited to runtime
        // state; ClashVpnService owns the authoritative status stream.
        log_android("Android libbox 使用 CommandClient 状态通道，不启动桌面 clash_api WebSocket", false);
    }
    if (state == 0 || state == 3) {
        try {
            store::coreStore().stopAndroidApiStreams();
            store::coreStore().setSetting("core.tun_enabled", "false");
        } catch (...) {}
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_ClashVpnService_nativeVpnStats(JNIEnv*, jclass,
                                                        jlong upload_rate,
                                                        jlong download_rate,
                                                        jlong upload_total,
                                                        jlong download_total,
                                                        jint connections) {
    try {
        store::coreStore().setAndroidRuntimeStats(
            static_cast<std::int64_t>(upload_rate), static_cast<std::int64_t>(download_rate),
            static_cast<std::int64_t>(upload_total), static_cast<std::int64_t>(download_total),
            static_cast<int>(connections));
    } catch (...) {
        // A status callback can race the first native-store initialization.
    }
}

extern "C" int clashflux_android_vpn_state() noexcept { return g_vpn_state.load(); }

// 设置页 VPN 开关 → MainActivity.startVpn/stopVpn（consent 弹窗/前台服务）。
extern "C" void clashflux_android_start_vpn() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return;
    }
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
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return;
    }
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "stopVpn", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

// 后台保活由 Activity 统一串接通知权限、前台服务和电池优化豁免页面。
extern "C" void clashflux_android_request_background_keep_alive() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return;
    }
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "requestBackgroundKeepAlive", "()V")) {
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
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return;
    }
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "requestIgnoreBatteryOptimizations", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

extern "C" void clashflux_android_open_battery_settings() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return;
    }
    if (jmethodID method = environment->GetStaticMethodID(
            g_activity_class, "openBatteryOptimizationSettings", "()V")) {
        environment->CallStaticVoidMethod(g_activity_class, method);
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

extern "C" bool clashflux_android_is_ignoring_battery() noexcept {
    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) return false;
    const jmethodID method = environment->GetStaticMethodID(
        g_activity_class, "isIgnoringBatteryOptimizations", "()Z");
    if (method == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    const jboolean result =
        environment->CallStaticBooleanMethod(g_activity_class, method);
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    if (attached) g_vm->DetachCurrentThread();
    return result == JNI_TRUE;
}

extern "C" const char* clashflux_android_proxy_groups() noexcept {
    thread_local std::string result;
    result.clear();

    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return result.c_str();
    }
    const jmethodID method = environment->GetStaticMethodID(
        g_activity_class, "proxyGroups", "()Ljava/lang/String;");
    if (method != nullptr) {
        auto* value = static_cast<jstring>(
            environment->CallStaticObjectMethod(g_activity_class, method));
        if (!environment->ExceptionCheck() && value != nullptr) {
            if (const char* chars = environment->GetStringUTFChars(value, nullptr)) {
                result = chars;
                environment->ReleaseStringUTFChars(value, chars);
            }
            environment->DeleteLocalRef(value);
        } else if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
        }
    }
    if (attached) g_vm->DetachCurrentThread();
    return result.c_str();
}

extern "C" bool clashflux_android_set_clash_mode(const char* mode) noexcept {
    if (mode == nullptr || *mode == '\0') return false;

    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    const jmethodID method = environment->GetStaticMethodID(
        g_activity_class, "setClashMode", "(Ljava/lang/String;)Z");
    if (method == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    jstring modeValue = environment->NewStringUTF(mode);
    if (modeValue == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    const jboolean ok = environment->CallStaticBooleanMethod(
        g_activity_class, method, modeValue);
    environment->DeleteLocalRef(modeValue);
    const bool failed = environment->ExceptionCheck();
    if (failed) environment->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
    return !failed && ok == JNI_TRUE;
}

extern "C" bool clashflux_android_select_outbound(const char* group,
                                                    const char* name) noexcept {
    if (group == nullptr || name == nullptr || *group == '\0' || *name == '\0') {
        return false;
    }

    std::lock_guard lock(g_mutex);
    bool attached = false;
    JNIEnv* environment = current_environment(attached);
    if (environment == nullptr || g_activity_class == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    const jmethodID method = environment->GetStaticMethodID(
        g_activity_class, "selectOutbound", "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (method == nullptr) {
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    jstring groupValue = environment->NewStringUTF(group);
    jstring nameValue = environment->NewStringUTF(name);
    if (groupValue == nullptr || nameValue == nullptr) {
        if (groupValue != nullptr) environment->DeleteLocalRef(groupValue);
        if (nameValue != nullptr) environment->DeleteLocalRef(nameValue);
        if (attached) g_vm->DetachCurrentThread();
        return false;
    }
    const jboolean ok = environment->CallStaticBooleanMethod(
        g_activity_class, method, groupValue, nameValue);
    environment->DeleteLocalRef(groupValue);
    environment->DeleteLocalRef(nameValue);
    const bool failed = environment->ExceptionCheck();
    if (failed) environment->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
    return !failed && ok == JNI_TRUE;
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
