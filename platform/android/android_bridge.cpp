// android_bridge.cpp — Java/NDK glue kept in the Clash-Flux app library.
//
// HuxerUI owns the rendering Activity and application directories. Clash-Flux
// only keeps a small bridge for opening subscription home pages.
#include <jni.h>

#include <mutex>
#include <string>

#include "clashflux_android_legacy.h"

namespace {

std::mutex g_mutex;
JavaVM* g_vm = nullptr;
jclass g_activity_class = nullptr;

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
                                                  jclass activity_class) {
    if (environment == nullptr || activity_class == nullptr) {
        return;
    }

    std::lock_guard lock(g_mutex);
    if (g_vm == nullptr) {
        environment->GetJavaVM(&g_vm);
        g_activity_class = static_cast<jclass>(environment->NewGlobalRef(activity_class));
    }
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
