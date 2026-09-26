// clashflux.instance：桌面单实例与唤醒通道。
//
// GUI 与 CLI 都经过这里：抢到锁的进程是 owner，负责自己的运行时并服务转发来的
// CLI 命令；抢不到的进程 GUI 只唤醒已有窗口，CLI 则把命令行转发给 owner 执行
// （见 clashflux.cli_ipc）。若发现旧实例处于 closing（正在退出）状态但未完全
// 死亡，则判定为异常卡死，新实例将主动终止旧残留进程并接管单实例锁。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <windows.h>
#include "win32_raii.h"
#else
#include <cerrno>
#include <csignal>
#include <cstdio>
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

struct InstanceInfo {
    long pid = 0;
    std::string state; // "running", "closing"
    long long timestamp = 0;
};

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

InstanceInfo readInstanceInfo(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    std::ifstream file(path);
    if (!file.is_open()) return {};
    InstanceInfo info;
    if (file >> info.pid) {
        if (!(file >> info.state)) {
            info.state = "running";
        }
        file >> info.timestamp;
    }
    return info;
}

void writeInstanceInfo(const std::filesystem::path& path, long pid, std::string_view state) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    file << pid << " " << state << " " << now << "\n";
}

void terminateOldProcess(long pid) {
    if (pid <= 0) return;
    clashflux::win32::UniqueHandle process{OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))};
    if (!process.valid()) return;
    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 1000);
}

#else

int& lockFile() {
    static int fd = -1;
    return fd;
}

long& currentPid() {
    static long pid = 0;
    return pid;
}

volatile std::sig_atomic_t activationRequested = 0;

void onActivationSignal(int) { activationRequested = 1; }

InstanceInfo readInstanceInfo(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};

    char buffer[128]{};
    const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (count <= 0) return {};

    buffer[count] = '\0';
    std::string text(buffer, static_cast<std::size_t>(count));
    std::istringstream iss(text);
    InstanceInfo info;
    if (iss >> info.pid) {
        if (!(iss >> info.state)) {
            info.state = "running";
        }
        iss >> info.timestamp;
    }
    return info;
}

void writeLockState(int fd, long pid, std::string_view state) {
    if (fd < 0) return;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const std::string content = std::format("{} {} {}\n", pid, state, now);
    if (::lseek(fd, 0, SEEK_SET) == 0 && ::ftruncate(fd, 0) == 0) {
        ::write(fd, content.data(), content.size());
        ::fsync(fd);
    }
}

void terminateOldProcess(long pid) {
    if (pid <= 0) return;
    const auto pidVal = static_cast<pid_t>(pid);
    if (::kill(pidVal, 0) == 0) {
        ::kill(pidVal, SIGTERM);
    }
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (::kill(pidVal, 0) != 0) return;
    }
    ::kill(pidVal, SIGKILL);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (::kill(pidVal, 0) != 0) return;
    }
}

void notifyExistingProcess(long pid) {
    if (pid <= 0) return;
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

export void markClosing() {
#ifdef _WIN32
    const std::filesystem::path path =
        cfg::dataDir() / "clash-flux.instance.lock";
    writeInstanceInfo(path, static_cast<long>(GetCurrentProcessId()), "closing");
#else
    const int fd = lockFile();
    if (fd >= 0) {
        writeLockState(fd, currentPid(), "closing");
    }
#endif
}

export void markRunning() {
#ifdef _WIN32
    const std::filesystem::path path =
        cfg::dataDir() / "clash-flux.instance.lock";
    writeInstanceInfo(path, static_cast<long>(GetCurrentProcessId()), "running");
#else
    const int fd = lockFile();
    if (fd >= 0) {
        writeLockState(fd, currentPid(), "running");
    }
#endif
}

/// 抢单实例锁。返回 true = 本进程成为 owner（继续启动/执行命令）；
/// 返回 false = 已有实例在运行。
/// @param activate false 时只探测/转发，不唤醒已有窗口（CLI 命令用）。
export bool acquireOrActivate(bool activate = true) {
#ifdef _WIN32
    const std::filesystem::path path =
        cfg::dataDir() / "clash-flux.instance.lock";
    clashflux::win32::UniqueHandle mutex{CreateMutexW(nullptr, TRUE, kMutexName)};
    if (!mutex.valid()) {
        // 无法判断实例状态时继续启动，避免把应用变成不可启动；正常安装
        // 环境下 CreateMutexW 不会失败。
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const InstanceInfo info = readInstanceInfo(path);
        if (info.pid > 0 && info.state == "closing") {
            // 上一个进程处于关闭状态但未完全退出（异常卡死）
            std::println(stderr, "clash-flux: 发现旧实例 (PID {}) 处于未完成的关闭状态，正在执行清理并接管...", info.pid);
            terminateOldProcess(info.pid);
            mutex.reset();
            for (int i = 0; i < 20; ++i) {
                clashflux::win32::UniqueHandle candidate{
                    CreateMutexW(nullptr, TRUE, kMutexName)};
                if (candidate.valid() && GetLastError() != ERROR_ALREADY_EXISTS) {
                    // 所有权交给进程级单例（活到进程结束）。
                    instanceMutex() = candidate.release();
                    activationEvent() = CreateEventW(nullptr, FALSE, FALSE, kEventName);
                    writeInstanceInfo(path, static_cast<long>(GetCurrentProcessId()), "running");
                    return true;
                }
                // candidate 在本轮结束时自动 CloseHandle；重试期间不再有悬空句柄。
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (activate) {
            clashflux::win32::UniqueHandle event{
                OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName)};
            if (event.valid()) SetEvent(event.get());
            activateExistingWindow();
        }
        return false;
    }
    instanceMutex() = mutex.release();
    activationEvent() = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    writeInstanceInfo(path, static_cast<long>(GetCurrentProcessId()), "running");
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
        if (!busy) {
            ::close(fd);
            return true;
        }

        const InstanceInfo info = readInstanceInfo(path);
        if (info.pid > 0 && info.state == "closing") {
            // 上一个进程处于关闭状态但未完全退出（异常卡死）。
            // 本新进程主动执行清理关闭，接管单实例锁。
            std::println(stderr, "clash-flux: 发现旧实例 (PID {}) 处于未完成的关闭状态，正在执行清理并接管...", info.pid);
            terminateOldProcess(info.pid);

            // 稍候等待内核释放锁
            for (int i = 0; i < 20; ++i) {
                if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                    currentPid() = static_cast<long>(::getpid());
                    writeLockState(fd, currentPid(), "running");
                    lockFile() = fd;
                    installSignalHandler();
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        if (info.pid > 0 && activate) {
            notifyExistingProcess(info.pid);
        }
        ::close(fd);
        return false;
    }

    currentPid() = static_cast<long>(::getpid());
    writeLockState(fd, currentPid(), "running");
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
