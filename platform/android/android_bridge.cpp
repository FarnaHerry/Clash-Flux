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
std::once_flag g_core_start_once;
// 「跟随系统」主题用：MainActivity 在每次 onCreate 时上报（系统深浅切换会
// 重建 Activity，uiMode 不在 configChanges 里），因此缓存总是新鲜的。
std::atomic<bool> g_system_dark{false};
// VpnService establish() 交给本进程的 TUN fd（-1 = 无 VPN）。mihomo 子进程
// spawn 时把它 dup2 到 fd 3（kAndroidTunFd，与 ClashVpnService 的常量一致），
// 配置注入 tun.file-descriptor: 3 由内核接管。
std::atomic<int> g_vpn_tun_fd{-1};

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

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_MainActivity_nativeSetSystemDark(JNIEnv*, jclass,
                                                          jboolean dark) {
    g_system_dark.store(dark == JNI_TRUE);
}

// cfg::systemPrefersDark() 的 Android 后端（见 src/config.cppm）。
extern "C" bool clashflux_android_system_dark() noexcept {
    return g_system_dark.load();
}

// ---- VPN TUN（ClashVpnService ↔ 内核生命周期）------------------------------

namespace {

// VPN 建立/撤销后重启内核：等自动启动离开 Starting（避免与 startCore 的
// 重入闸互怼），再 stop+start 重新合成带/不带 tun 的配置。
void restartCoreForVpn(bool tunUp) {
    std::thread([tunUp] {
        try {
            auto& core = store::coreStore();
            for (int i = 0; i < 70; ++i) {
                if (core.snapshot().state != core::CoreState::Starting) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            core.stopCore();
            if (!tunUp) {
                // 系统侧撤销（快捷开关/被其他 VPN 接管）：设置同步回落。
                core.setSetting("core.tun_enabled", "false");
            }
            core.startCore(store::profilesStore().selectedYaml());
        } catch (const std::exception& error) {
            log_android(("VPN core restart failed: " + std::string{error.what()})
                            .c_str(),
                        true);
        } catch (...) {
            log_android("VPN core restart failed: unknown exception", true);
        }
    }).detach();
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_ClashVpnService_nativeTunEstablished(JNIEnv*, jclass,
                                                              jint fd) {
    g_vpn_tun_fd.store(fd);
    log_android("VPN TUN established; restarting core with tun fd");
    restartCoreForVpn(true);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_farna_clashflux_ClashVpnService_nativeTunRevoked(JNIEnv*, jclass) {
    if (const int fd = g_vpn_tun_fd.exchange(-1); fd >= 0) {
        ::close(fd);
    }
    log_android("VPN TUN revoked; restarting core without tun fd");
    restartCoreForVpn(false);
}

extern "C" int clashflux_android_vpn_tun_fd() noexcept {
    return g_vpn_tun_fd.load();
}

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
