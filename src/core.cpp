// core.cpp — clashflux.core 实现单元。
//
// 进程后端按编译期分流：POSIX（Linux + macOS）走 posix_spawn + poll 读管道 +
// SIGTERM/SIGKILL；Windows 走 CreateProcessW + CreatePipe 重定向 + 监视线程读管
// 拆行 + TerminateProcess。两条路径共用输出队列与拆行逻辑，差异集中在
// spawn/monitorLoop/stop/running 四个点。
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
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>   // open, O_APPEND
#include <cerrno>   // errno, EINTR
#include <unistd.h>
extern char** environ;
#endif

module clashflux.core;

import std;
import clashflux.service;

namespace core {

// ---- TUN 打开门禁（见 core.cppm 注释）----
namespace {

#ifdef _WIN32
// 当前进程是否以管理员令牌运行。
bool tokenElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD ret = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &ret)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
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
    // root 服务托管的内核由服务侧（root）建 TUN：服务可用即视为可开。
    if (service::available()) return TunGate::Ok;
#endif
    return TunGate::Denied;
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

// ---- 配置合成 ----
namespace {

// 订阅 YAML 里由应用托管的顶层键：文本级剔除（顶层键 = 行首无缩进的 key:）。
// 逐行扫，仅剔行首无空白的 "key:" 行；命中后连同其缩进值块（profile: 这类
// 多行块）一起跳过——否则孤立的缩进子行会让合并结果直接不是合法 YAML。
// 键表必须与下方注入块严格一一对应：注入什么就剔什么。
bool isManagedKeyLine(std::string_view line) {
    if (line.empty() || line.front() == ' ' || line.front() == '\t' ||
        line.front() == '#') {
        return false;
    }
    static constexpr std::string_view kKeys[] = {
        "external-controller:", "secret:", "mixed-port:", "port:", "socks-port:",
        "allow-lan:", "mode:", "log-level:", "ipv6:",
        "unified-delay:", "tcp-concurrent:", "find-process-mode:",
        "global-client-fingerprint:", "profile:", "tun:",
    };
    for (const auto key : kKeys) {
        if (line.starts_with(key)) return true;
    }
    return false;
}

bool isIndentedContinuation(std::string_view line) {
    return !line.empty() && (line.front() == ' ' || line.front() == '\t');
}

} // namespace

std::string generateConfig(const std::string& profileYaml,
                           const std::string& controller,
                           const std::string& secret,
                           int mixedPort,
                           const std::string& mode,
                           bool allowLan,
                           const std::string& logLevel,
                           bool tunEnabled) {
    std::string out;
    // 注入块放头部。
    out += "# ---- Clash-Flux 托管块（手写改动会被覆盖）----\n";
    out += std::format("mixed-port: {}\n", mixedPort);
    out += std::format("allow-lan: {}\n", allowLan ? "true" : "false");
    out += std::format("mode: {}\n", mode);
    out += std::format("log-level: {}\n", logLevel);
    out += std::format("external-controller: {}\n", controller);
    if (!secret.empty()) out += std::format("secret: \"{}\"\n", secret);
    out += "ipv6: true\n";
    out += "unified-delay: true\n";
    out += "tcp-concurrent: true\n";
    out += "find-process-mode: 'off'\n";
    out += "global-client-fingerprint: chrome\n";
    out += "profile:\n  store-selected: true\n  store-fake-ip: true\n";
    if (tunEnabled) {
        // TUN 透明代理（需 root/CAP_NET_ADMIN，权限不足时 mihomo 只报错不退出）。
        out += "tun:\n"
               "  enable: true\n"
               "  stack: mixed\n"
               "  device: clash-flux\n"
               "  auto-route: true\n"
               "  auto-detect-interface: true\n"
               "  dns-hijack:\n    - any:53\n";
    }
    out += "# ---- 订阅内容 ----\n";

    // 剔除订阅里的托管顶层键（连同其缩进值块）后原样拼接。
    std::string_view rest{profileYaml};
    bool skippingBlock = false;
    while (!rest.empty()) {
        const auto pos = rest.find('\n');
        const std::string_view line = rest.substr(0, pos);
        if (skippingBlock && isIndentedContinuation(line)) {
            // 托管键的缩进值块：继续跳过（空行/注释行结束块）。
        } else {
            skippingBlock = false;
            if (isManagedKeyLine(line)) {
                skippingBlock = true;
            } else {
                out += line;
                out.push_back('\n');
            }
        }
        rest = (pos == std::string_view::npos) ? std::string_view{} : rest.substr(pos + 1);
    }
    return out;
}

// ---- detached spawn / killPid（接管与 CLI 驻留形态）----
namespace {

// 写 <workDir>/mihomo.pid（CoreProcess::start 与 spawnDetached 共用）。
void writePidFile(const std::filesystem::path& workDir, long pid) {
    std::ofstream out(workDir / "mihomo.pid", std::ios::trunc);
    out << pid << '\n';
}

} // namespace

void killPid(long pid) {
#ifdef _WIN32
    if (pid <= 0) return;
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    if (pid > 0) ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
}

bool spawnDetached(const std::filesystem::path& binary,
                   const std::filesystem::path& workDir,
                   const std::filesystem::path& configFile,
                   std::string& error) {
    if (binary.empty() || !std::filesystem::exists(binary)) {
        error = "未找到 mihomo 内核（engines/ 或 PATH）";
        return false;
    }
    const std::filesystem::path logPath = workDir / "mihomo.log";
#ifdef _WIN32
    // 日志重定向到文件，句柄可继承；DETACHED_PROCESS 不挂控制台。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(logPath.wstring().c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ, &sa, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        error = "无法打开内核日志文件";
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logHandle;
    si.hStdError = logHandle;
    si.hStdInput = nullptr;
    const std::wstring cmd = std::format(L"\"{}\" -d \"{}\" -f \"{}\"",
                                         binary.wstring(), workDir.wstring(),
                                         configFile.wstring());
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const std::wstring workDirW = workDir.wstring();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr,
                                   workDirW.c_str(), &si, &pi);
    CloseHandle(logHandle);
    if (!ok) {
        error = "mihomo 进程启动失败";
        return false;
    }
    writePidFile(workDir, static_cast<long>(pi.dwProcessId));
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
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
    std::vector<std::string> argsStorage{bin, "-d", dir, "-f", cfg};
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
        error = "mihomo 进程启动失败";
        return false;
    }
    writePidFile(workDir, static_cast<long>(pid));
    return true;
#endif
}

// ---- CoreProcess ----
namespace {

constexpr std::size_t kMaxOutputLines = 4000;   // 内核日志队列上限（防爆内存）
constexpr auto kGracePeriod = std::chrono::seconds(2);

struct CoreProcessImpl {
#ifdef _WIN32
    HANDLE childProcess = nullptr;
    HANDLE readPipe = nullptr;
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

    ~CoreProcessImpl() { stop(); joinMonitor(); }

    void joinMonitor() {
        if (monitor.joinable()) monitor.join();
    }

    void stop() {
        if (!running.load()) return;
        stopRequested.store(true);
#ifdef _WIN32
        // Windows 没有 SIGTERM 语义（控制台进程只能靠 GenerateConsoleCtrlEvent
        // 且要求同控制台组，对 CREATE_NO_WINDOW 子进程不适用）：不给宽限，
        // 直接 TerminateProcess。POSIX 分支的 2s 宽限是等内核优雅退出，
        // Windows 上这一步不存在，差异仅此而已。
        if (childProcess) TerminateProcess(childProcess, 1);
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
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        const std::wstring cmd = std::format(L"\"{}\" -d \"{}\" -f \"{}\"",
                                             binary.wstring(), workDir.wstring(),
                                             configFile.wstring());
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        // 工作目录设为 mihomo 的 -d 目录：内核若解析相对路径（ui/、mmdb 在线
        // 下载回落等）与命令行参数行为一致。
        const std::wstring workDirW = workDir.wstring();
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr,
                                       workDirW.c_str(), &si, &pi);
        CloseHandle(writePipe);
        if (!ok) {
            CloseHandle(readPipe);
            readPipe = nullptr;
            return false;
        }
        childProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
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
        std::vector<std::string> argsStorage{bin, "-d", dir, "-f", cfg};
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
    }

    void pushLine(std::string line) {
        if (line.empty()) return;
        std::lock_guard lock(mutex);
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
            if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
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
        if (childProcess) {
            WaitForSingleObject(childProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(childProcess, &code);
            status = static_cast<int>(code);
            CloseHandle(childProcess);
            childProcess = nullptr;
        }
        if (readPipe) {
            CloseHandle(readPipe);
            readPipe = nullptr;
        }
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
    }
    impl_->stopRequested.store(false);
    impl_->exitCode.store(-1);
    impl_->lastError.clear();

    if (binary.empty() || !std::filesystem::exists(binary)) {
        impl_->lastError = "未找到 mihomo 内核（engines/ 或 PATH）";
        return false;
    }
    if (!impl_->spawn(binary, workDir, configFile)) {
        impl_->lastError = "mihomo 进程启动失败";
        return false;
    }
    // pidfile：GUI 附着 spawn 的内核也能被 CLI（另一进程）经 pidfile 接管/停止。
#ifdef _WIN32
    writePidFile(workDir, static_cast<long>(GetProcessId(impl_->childProcess)));
#else
    writePidFile(workDir, static_cast<long>(impl_->childPid));
#endif
    impl_->running.store(true);
    impl_->monitor = std::thread([this] { impl_->monitorLoop(); });
    return true;
}

void CoreProcess::stop() { impl_->stop(); }

bool CoreProcess::running() const {
#ifdef _WIN32
    // 以进程句柄为权威：WaitForSingleObject 超时 0 探测子进程是否还活着
    // （WAIT_TIMEOUT = 仍运行）。running 原子量只作快速短路——句柄只在
    // 监视线程收尾时关闭，此后原子量也已翻 false。
    if (!impl_->running.load()) return false;
    if (impl_->childProcess) {
        return WaitForSingleObject(impl_->childProcess, 0) == WAIT_TIMEOUT;
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

} // namespace core
