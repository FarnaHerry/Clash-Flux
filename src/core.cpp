// core.cpp — clashflux.core 实现单元。
//
// 进程后端按编译期分流：POSIX（Linux + macOS）走 posix_spawn，Windows 走
// CreateProcess。Android 的 sing-box libbox 由 Java VpnService 持有，
// 此处仅保留跨平台的轻量状态接口。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteExW（WIN32_LEAN_AND_MEAN 不含 shellapi）
#include "win32_raii.h"
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#if !defined(__ANDROID__)
#include <spawn.h>
#endif
#include <poll.h>
#include <fcntl.h>   // open, O_APPEND
#include <cerrno>   // errno, EINTR
#include <cstring>  // strerror
#include <unistd.h>
extern char** environ;
#endif

#if defined(__ANDROID__)
#include <android/log.h>
extern "C" void clashflux_android_open_url(const char* url) noexcept;
#endif

module clashflux.core;

import std;
import clashflux.service;
import clashflux.singbox;

namespace core {

// ---- TUN 打开门禁（见 core.cppm 注释）----
namespace {

#ifdef _WIN32
// 当前进程是否以管理员令牌运行。
bool tokenElevated() {
    BOOL elevated = FALSE;
    HANDLE raw = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        clashflux::win32::UniqueHandle token{raw};
        TOKEN_ELEVATION elevation{};
        DWORD ret = 0;
        if (GetTokenInformation(token.get(), TokenElevation, &elevation,
                                sizeof(elevation), &ret)) {
            elevated = elevation.TokenIsElevated;
        }
    }
    return elevated != FALSE;
}

// 以管理员重新启动自身（runas → UAC）。用户取消 / 失败返回 false。
bool relaunchElevated() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}
#endif

} // namespace

TunGate tunGate() {
#ifdef _WIN32
    if (tokenElevated()) return TunGate::Ok;
    return relaunchElevated() ? TunGate::Elevated : TunGate::Denied;
#else
    if (::geteuid() == 0) return TunGate::Ok;
#ifdef __linux__
    // root 服务托管的内核由服务侧（root）建 TUN。若已安装但版本落后，
    // 握手会请求 root 服务自行升级；没有服务时仍交给 UI 引导安装。
    std::string serviceError;
    if (service::ensureCompatible(serviceError)) return TunGate::Ok;
#endif
    return TunGate::Denied;
#endif
}

void openInBrowser(const std::string& url) {
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1,
                                         nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wurl(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
#elif defined(__ANDROID__)
    clashflux_android_open_url(url.c_str());
#else
    // fork + exec 不经 shell：URL 里的 & 等字符无注入面。
    const char* opener =
#ifdef __APPLE__
        "open";
#else
        "xdg-open";
#endif
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
#endif
}

const char* stateName(CoreState s) {
    switch (s) {
        case CoreState::Stopped: return "已停止";
        case CoreState::Starting: return "启动中";
        case CoreState::Running: return "运行中";
        case CoreState::Failed: return "启动失败";
    }
    return "未知";
}

// ---- 配置合成（经 clashflux.singbox 编译器）----

singbox::CompileResult generateConfig(singbox::CompileOptions options) {
#if defined(__ANDROID__)
    // Android 的 TUN 由 VpnService 建立：只有请求 TUN 时才生成 tun inbound，
    // 普通内核常驻/测速使用同一个 libbox 服务但不创建系统 VPN 接口。
    // IPv6 是否启用由持久化设置传入；VpnService 会按 libbox 返回的 IPv6
    // 地址与路由配置接口。严格路由仍关闭，避免截断系统级分流。
    if (options.tunInbound) options.tunStrictRoute = false;
#endif
    return singbox::compileConfig(options);
}

// ---- detached spawn / killPid（接管与 CLI 驻留形态）----
namespace {

// 写 <workDir>/core.pid（CoreProcess::start 与 spawnDetached 共用）。
void writePidFile(const std::filesystem::path& workDir, long pid) {
    std::ofstream out(workDir / "core.pid", std::ios::trunc);
    out << pid << '\n';
}

} // namespace

void killPid(long pid) {
#ifdef _WIN32
    if (pid <= 0) return;
    clashflux::win32::UniqueHandle process{
        OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))};
    if (process.valid()) TerminateProcess(process.get(), 1);
#else
    if (pid > 0) ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
}

bool pidAlive(long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    clashflux::win32::UniqueHandle process{OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid))};
    return process.valid();
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    // EPERM：进程存在但属其他用户（root 服务托管的内核），同样视为存活。
    return errno == EPERM;
#endif
}

// 去掉 CSI 转义序列（ESC [ 参数… 终止字节），只留可读文本。
std::string stripAnsi(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) != 0x1b) {
            out.push_back(text[i]);
            continue;
        }
        ++i;
        if (i < text.size() && text[i] == '[') {
            while (i + 1 < text.size() &&
                   !(text[i + 1] >= '@' && text[i + 1] <= '~')) {
                ++i;
            }
            ++i;  // 跳过终止字节
        }
    }
    return out;
}

bool spawnDetached(const std::filesystem::path& binary,
                   const std::filesystem::path& workDir,
                   const std::filesystem::path& configFile,
                   std::string& error) {
    if (binary.empty() || !std::filesystem::exists(binary)) {
        error = "未找到 sing-box 内核（engines/ 或 PATH）";
        return false;
    }
    const std::filesystem::path logPath = workDir / "core.log";
#ifdef _WIN32
    // 日志重定向到文件，句柄可继承；DETACHED_PROCESS 不挂控制台。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    clashflux::win32::UniqueHandle logHandle{CreateFileW(
        logPath.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!logHandle.valid()) {
        error = "无法打开内核日志文件";
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logHandle.get();
    si.hStdError = logHandle.get();
    si.hStdInput = nullptr;
    const std::wstring cmd = std::format(L"\"{}\" run -c \"{}\" -D \"{}\"",
                                         binary.wstring(), configFile.wstring(),
                                         workDir.wstring());
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const std::wstring workDirW = workDir.wstring();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr,
                                   workDirW.c_str(), &si, &pi);
    if (!ok) {
        error = "sing-box 进程启动失败";
        return false;
    }
    clashflux::win32::UniqueHandle process{pi.hProcess};
    clashflux::win32::UniqueHandle thread{pi.hThread};
    writePidFile(workDir, static_cast<long>(pi.dwProcessId));
    return true;
#else
#if defined(__ANDROID__)
    const int logFd = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd < 0) {
        error = "无法打开内核日志文件";
        return false;
    }
    const std::string bin = binary.string();
    const std::string dir = workDir.string();
    const std::string cfg = configFile.string();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::dup2(logFd, STDOUT_FILENO);
        ::dup2(logFd, STDERR_FILENO);
        ::close(logFd);
        ::execl(bin.c_str(), bin.c_str(), "run", "-c", cfg.c_str(), "-D",
                dir.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(logFd);
    if (pid < 0) {
        error = "Android sing-box 内核进程启动失败";
        return false;
    }
    writePidFile(workDir, static_cast<long>(pid));
    return true;
#else
    // setsid 脱离会话：CLI 退出后内核驻留；stdout/stderr 追加进日志文件
    // （若仍接管道，CLI 退出后内核写日志会吃 SIGPIPE 被杀）。
    const int logFd = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd < 0) {
        error = "无法打开内核日志文件";
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, logFd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, logFd, STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    std::string bin = binary.string();
    std::string dir = workDir.string();
    std::string cfg = configFile.string();
    std::vector<std::string> argsStorage{bin, "run", "-c", cfg, "-D", dir};
    std::vector<char*> argv;
    for (auto& a : argsStorage) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, bin.c_str(), &actions, &attr, argv.data(),
                                environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(logFd);
    if (rc != 0) {
        error = "sing-box 进程启动失败";
        return false;
    }
    writePidFile(workDir, static_cast<long>(pid));
    return true;
#endif
#endif
}

// ---- CoreProcess ----
namespace {

constexpr std::size_t kMaxOutputLines = 4000;   // 内核日志队列上限（防爆内存）
constexpr std::size_t kTailLines = 6;           // 诊断尾部缓冲行数
constexpr std::size_t kTailLineChars = 120;     // 尾部单行截断长度
constexpr auto kGracePeriod = std::chrono::seconds(2);

struct CoreProcessImpl {
#ifdef _WIN32
    // 子进程与读管道由本对象拥有；monitorLoop 收尾时 reset()，其余线程只通过
    // get()/valid() 读取，不再手写 CloseHandle。
    clashflux::win32::UniqueHandle childProcess;
    clashflux::win32::UniqueHandle readPipe;
#else
    pid_t childPid = -1;
    int readFd = -1;
#endif
    std::thread monitor;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<int> exitCode{-1};
    std::string lastError;
    std::mutex mutex;
    std::vector<std::string> output;
    std::vector<std::string> tail;  // 最近输出的非破坏性副本（诊断用）

    ~CoreProcessImpl() { stop(); joinMonitor(); }

    void joinMonitor() {
        if (monitor.joinable()) monitor.join();
    }

    void stop() {
        if (!running.load()) return;
        stopRequested.store(true);
#if defined(__ANDROID__)
        running.store(false);
        exitCode.store(0);
#elif defined(_WIN32)
        // Windows 没有 SIGTERM 语义（控制台进程只能靠 GenerateConsoleCtrlEvent
        // 且要求同控制台组，对 CREATE_NO_WINDOW 子进程不适用）：不给宽限，
        // 直接 TerminateProcess。POSIX 分支的 2s 宽限是等内核优雅退出，
        // Windows 上这一步不存在，差异仅此而已。
        if (childProcess.valid()) TerminateProcess(childProcess.get(), 1);
#else
        if (childPid > 0) ::kill(childPid, SIGTERM);
#endif
    }

    bool spawn(const std::filesystem::path& binary,
               const std::filesystem::path& workDir,
               const std::filesystem::path& configFile) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readRaw = nullptr;
        HANDLE writeRaw = nullptr;
        if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) return false;
        readPipe.reset(readRaw);
        // 子进程需要继承写端，故保持到 CreateProcessW 之后再随作用域释放。
        clashflux::win32::UniqueHandle writePipe{writeRaw};
        SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe.get();
        si.hStdError = writePipe.get();
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        const std::wstring cmd = std::format(L"\"{}\" run -c \"{}\" -D \"{}\"",
                                             binary.wstring(), configFile.wstring(),
                                             workDir.wstring());
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        // 工作目录设为 -D 目录：内核的相对路径（cache.db、在线规则集缓存等）
        // 与命令行参数行为一致。
        const std::wstring workDirW = workDir.wstring();
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr,
                                       workDirW.c_str(), &si, &pi);
        if (!ok) {
            readPipe.reset();
            return false;
        }
        childProcess.reset(pi.hProcess);
        clashflux::win32::UniqueHandle thread{pi.hThread};
        return true;
#else
#if defined(__ANDROID__)
        // Android CoreProcess::start is implemented through the Java
        // C-shared lifecycle. Never fall back to child-process execution.
        static_cast<void>(binary);
        static_cast<void>(workDir);
        static_cast<void>(configFile);
        return false;
#else
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);

        std::string bin = binary.string();
        std::string dir = workDir.string();
        std::string cfg = configFile.string();
        std::vector<std::string> argsStorage{bin, "run", "-c", cfg, "-D", dir};
        std::vector<char*> argv;
        for (auto& a : argsStorage) argv.push_back(a.data());
        argv.push_back(nullptr);

        const int rc = posix_spawnp(&childPid, bin.c_str(), &actions, nullptr,
                                    argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[1]);
        if (rc != 0) {
            ::close(pipefd[0]);
            childPid = -1;
            return false;
        }
        readFd = pipefd[0];
        return true;
#endif
#endif
    }

    void pushLine(std::string line) {
        if (line.empty()) return;
        // sing-box 给 FATAL/INFO 行加 ANSI 颜色；诊断文本直接进 UI 错误提示，
        // 必须去掉转义序列（否则用户看到 "[31mFATAL[0m"）。
        line = stripAnsi(std::move(line));
        if (line.empty()) return;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "ClashFlux", "core: %s", line.c_str());
#endif
        std::lock_guard lock(mutex);
        // 尾部缓冲先于上限检查：output 达到 kMaxOutputLines 后不再增长，
        // 但诊断尾部必须始终记录最新的行（崩溃原因常在缓冲饱和后出现）。
        tail.push_back(line);
        if (tail.size() > kTailLines) tail.erase(tail.begin());
        if (output.size() >= kMaxOutputLines) return;
        output.push_back(std::move(line));
    }

    void splitLines(std::string& pending, const char* buf, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!pending.empty()) pushLine(std::move(pending));
                pending.clear();
            } else {
                pending.push_back(c);
            }
        }
    }

    void monitorLoop() {
        std::string pending;
        auto killDeadline = std::chrono::steady_clock::time_point::max();

        for (;;) {
#ifdef _WIN32
            char buf[4096];
            DWORD n = 0;
            if (!ReadFile(readPipe.get(), buf, sizeof(buf), &n, nullptr) || n == 0) break;
            splitLines(pending, buf, n);
#else
            pollfd pfd{readFd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr == 0) {
                if (stopRequested.load() && childPid > 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (killDeadline == std::chrono::steady_clock::time_point::max()) {
                        killDeadline = now + kGracePeriod;
                    } else if (now >= killDeadline) {
                        ::kill(childPid, SIGKILL);
                        killDeadline = std::chrono::steady_clock::time_point::max();
                    }
                }
                continue;
            }
            if (pr < 0) break;
            if (pfd.revents & (POLLHUP | POLLERR)) {
                char buf[4096];
                const ssize_t n = ::read(readFd, buf, sizeof(buf));
                if (n > 0) splitLines(pending, buf, static_cast<size_t>(n));
                break;
            }
            if (pfd.revents & POLLIN) {
                char buf[4096];
                const ssize_t n = ::read(readFd, buf, sizeof(buf));
                if (n <= 0) break;
                splitLines(pending, buf, static_cast<size_t>(n));
            }
#endif
        }

        if (!pending.empty()) pushLine(std::move(pending));

        // 收尾：等退出码，清理句柄。
        int status = 0;
#ifdef _WIN32
        if (childProcess.valid()) {
            WaitForSingleObject(childProcess.get(), INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(childProcess.get(), &code);
            status = static_cast<int>(code);
            childProcess.reset();
        }
        readPipe.reset();
#else
        if (childPid > 0) {
            while (::waitpid(childPid, &status, 0) < 0 && errno == EINTR) {}
            childPid = -1;
        }
        if (readFd >= 0) {
            ::close(readFd);
            readFd = -1;
        }
#endif
#if defined(__ANDROID__)
        const int loggedExitCode =
#ifdef _WIN32
            status;
#else
            WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
        __android_log_print(ANDROID_LOG_INFO, "ClashFlux",
                            "core process exited: %d", loggedExitCode);
#endif
        if (stopRequested.load()) {
            exitCode.store(0);
        } else {
#ifdef _WIN32
            exitCode.store(status);
#else
            exitCode.store(WIFEXITED(status) ? WEXITSTATUS(status) : 128);
#endif
        }
        running.store(false);
    }
};

} // namespace

struct CoreProcess::Impl : CoreProcessImpl {};

CoreProcess::CoreProcess() : impl_(std::make_unique<Impl>()) {}
CoreProcess::~CoreProcess() = default;

bool CoreProcess::start(const std::filesystem::path& binary,
                        const std::filesystem::path& workDir,
                        const std::filesystem::path& configFile) {
    if (impl_->running.load()) impl_->stop();
    impl_->joinMonitor();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->output.clear();
        impl_->tail.clear();
    }
    impl_->stopRequested.store(false);
    impl_->exitCode.store(-1);
    impl_->lastError.clear();

#if defined(__ANDROID__)
    // Android's engine is sing-box libbox, owned by ClashVpnService rather
    // than a native child process.  CoreProcess remains a lightweight state
    // holder so the desktop orchestration interface stays platform-neutral.
    static_cast<void>(binary);
    static_cast<void>(workDir);
    static_cast<void>(configFile);
    impl_->running.store(true);
    return true;
#else
    if (binary.empty() || !std::filesystem::exists(binary)) {
        impl_->lastError = "未找到 sing-box 内核（engines/ 或 PATH）";
        return false;
    }
    if (!impl_->spawn(binary, workDir, configFile)) {
        impl_->lastError = "sing-box 进程启动失败";
        return false;
    }
    // pidfile：GUI 附着 spawn 的内核也能被 CLI（另一进程）经 pidfile 接管/停止。
#ifdef _WIN32
    writePidFile(workDir, static_cast<long>(GetProcessId(impl_->childProcess.get())));
#else
    writePidFile(workDir, static_cast<long>(impl_->childPid));
#endif
    impl_->running.store(true);
    impl_->monitor = std::thread([this] { impl_->monitorLoop(); });
    return true;
#endif
}

void CoreProcess::stop() { impl_->stop(); }

bool CoreProcess::running() const {
#ifdef _WIN32
    // 以进程句柄为权威：WaitForSingleObject 超时 0 探测子进程是否还活着
    // （WAIT_TIMEOUT = 仍运行）。running 原子量只作快速短路——句柄只在
    // 监视线程收尾时关闭，此后原子量也已翻 false。
    if (!impl_->running.load()) return false;
    if (impl_->childProcess.valid()) {
        return WaitForSingleObject(impl_->childProcess.get(), 0) == WAIT_TIMEOUT;
    }
    return true;
#else
    return impl_->running.load();
#endif
}
int CoreProcess::exitCode() const { return impl_->exitCode.load(); }
std::string CoreProcess::lastError() const { return impl_->lastError; }

std::vector<std::string> CoreProcess::drainOutput() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->output, {});
}

std::string CoreProcess::recentTail() const {
    std::lock_guard lock(impl_->mutex);
    // 取最后 3 行、单行截断，紧凑单行拼接，便于直接附进 lastError。
    std::string joined;
    const std::size_t begin = impl_->tail.size() > 3 ? impl_->tail.size() - 3 : 0;
    for (std::size_t i = begin; i < impl_->tail.size(); ++i) {
        std::string line = impl_->tail[i].substr(0, kTailLineChars);
        if (!joined.empty()) joined += " / ";
        joined += line;
    }
    return joined;
}

} // namespace core
