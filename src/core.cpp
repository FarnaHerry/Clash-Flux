// core.cpp — clashflux.core 实现单元。
//
// POSIX 走 posix_spawn + poll 读管道；Windows 走 CreateProcess（里程碑 1 先实现
// POSIX，Windows 分支留 TerminateProcess 骨架）。
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
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <cerrno>   // errno, EINTR
#include <unistd.h>
extern char** environ;
#endif

module clashflux.core;

import std;

namespace core {

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
// 逐行扫，仅剔行首无空白的 "key:" 单行；多行值（如 tun: 下的缩进块）不受影响
// —— 剔除只针对我们注入的单行标量键。
bool isManagedKeyLine(std::string_view line) {
    if (line.empty() || line.front() == ' ' || line.front() == '\t' ||
        line.front() == '#') {
        return false;
    }
    static constexpr std::string_view kKeys[] = {
        "external-controller:", "secret:", "mixed-port:", "port:", "socks-port:",
        "allow-lan:", "mode:", "log-level:", "ipv6:",
    };
    for (const auto key : kKeys) {
        if (line.starts_with(key)) return true;
    }
    return false;
}

} // namespace

std::string generateConfig(const std::string& profileYaml,
                           const std::string& controller,
                           const std::string& secret,
                           int mixedPort,
                           const std::string& mode,
                           bool allowLan,
                           const std::string& logLevel) {
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
    out += "# ---- 订阅内容 ----\n";

    // 剔除订阅里的托管顶层键后原样拼接。
    std::string_view rest{profileYaml};
    while (!rest.empty()) {
        const auto pos = rest.find('\n');
        const std::string_view line = rest.substr(0, pos);
        if (!isManagedKeyLine(line)) {
            out += line;
            out.push_back('\n');
        }
        rest = (pos == std::string_view::npos) ? std::string_view{} : rest.substr(pos + 1);
    }
    return out;
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
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
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
    impl_->running.store(true);
    impl_->monitor = std::thread([this] { impl_->monitorLoop(); });
    return true;
}

void CoreProcess::stop() { impl_->stop(); }
bool CoreProcess::running() const { return impl_->running.load(); }
int CoreProcess::exitCode() const { return impl_->exitCode.load(); }
std::string CoreProcess::lastError() const { return impl_->lastError; }

std::vector<std::string> CoreProcess::drainOutput() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->output, {});
}

} // namespace core
