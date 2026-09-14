// app_instance.cppm — clashflux.instance：桌面 GUI 单实例与唤醒通道。
//
// CLI 命令不经过这里；只有无参数启动 GUI 时由各平台 main 获取实例锁。
// 第二次启动只发送一次唤醒信号并退出，不会再创建第二个内核/托盘。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#endif

export module clashflux.instance;

import std;
import clashflux.config;

namespace clashflux::instance {
namespace {

#ifdef _WIN32

HANDLE& instanceMutex() {
    static HANDLE handle = nullptr;
    return handle;
}

HANDLE& activationEvent() {
    static HANDLE handle = nullptr;
    return handle;
}

constexpr wchar_t kMutexName[] = L"Local\\ClashFlux.Singleton";
constexpr wchar_t kEventName[] = L"Local\\ClashFlux.Activate";

void activateExistingWindow() {
    // 立刻尝试一次，覆盖已有窗口已经创建完成的常规场景。事件通道负责
    // 覆盖窗口尚在启动、此时 FindWindow 尚未可见的竞态。
    if (HWND window = FindWindowW(nullptr, L"Clash-Flux"); window != nullptr) {
        ShowWindow(window, SW_RESTORE);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
}

#else

int& lockFile() {
    static int fd = -1;
    return fd;
}

volatile std::sig_atomic_t activationRequested = 0;

void onActivationSignal(int) { activationRequested = 1; }

void notifyExistingProcess(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    char pidText[32]{};
    const ssize_t count = ::read(fd, pidText, sizeof(pidText) - 1);
    ::close(fd);
    if (count <= 0) return;

    pidText[count] = '\0';
    char* end = nullptr;
    const long pid = std::strtol(pidText, &end, 10);
    if (end == pidText || pid <= 0) return;
    ::kill(static_cast<pid_t>(pid), SIGUSR1);
}

bool installSignalHandler() {
    struct sigaction action {};
    action.sa_handler = &onActivationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return sigaction(SIGUSR1, &action, nullptr) == 0;
}

#endif

} // namespace

export bool acquireOrActivate() {
#ifdef _WIN32
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex == nullptr) {
        // 无法判断实例状态时继续启动，避免把应用变成不可启动；正常安装
        // 环境下 CreateMutexW 不会失败。
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName);
            event != nullptr) {
            SetEvent(event);
            CloseHandle(event);
        }
        activateExistingWindow();
        CloseHandle(mutex);
        return false;
    }
    instanceMutex() = mutex;
    activationEvent() = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    return true;
#else
    const std::filesystem::path path =
        cfg::dataDir() / "clash-flux.instance.lock";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK || errno == EAGAIN;
        if (busy) notifyExistingProcess(path);
        ::close(fd);
        return !busy;
    }

    const std::string pid = std::to_string(static_cast<long>(::getpid()));
    if (::ftruncate(fd, 0) == 0) {
        ::write(fd, pid.data(), pid.size());
        ::fsync(fd);
    }
    lockFile() = fd;
    if (!installSignalHandler()) {
        // 锁仍然有效；只是不支持从第二次启动唤醒，避免放弃单实例保证。
    }
    return true;
#endif
}

export bool consumeActivation() {
#ifdef _WIN32
    return activationEvent() != nullptr &&
           WaitForSingleObject(activationEvent(), 0) == WAIT_OBJECT_0;
#else
    if (activationRequested == 0) return false;
    activationRequested = 0;
    return true;
#endif
}

} // namespace clashflux::instance
