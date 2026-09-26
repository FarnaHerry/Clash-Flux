// cli_ipc.cppm — 单实例下的 CLI 命令转发（跨平台，文件 + 启动泵轮询）。
//
// 单实例模型：GUI 与 CLI 都先抢实例锁；抢到的进程成为 owner，执行命令并在
// 启动泵里顺带服务转发请求；没抢到的进程把命令行写进 <dataDir>/cli-requests/
// 并轮询响应文件，拿到输出与退出码后打印并退出。
//
// 这样任何时刻只有一个 Runtime / 一个持久化缓存 / 一个托盘，CLI 改的设置由
// 运行中的实例直接写进它的缓存并落库，不存在跨进程缓存不一致。
module;

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

export module clashflux.cli_ipc;

import std;
import clashflux.config;
import clashflux.cli;

namespace clashflux::cli_ipc {

namespace {

// 响应文件结尾固定 8 字节 footer：int32 退出码 + uint32 magic。
// 输出文本写在 footer 之前，客户端按尾部 8 字节解析。
constexpr std::uint32_t kMagic = 0x43464C31U;  // 'CFL1'
constexpr auto kClientTimeout = std::chrono::seconds{60};

std::filesystem::path requestDir() {
    const std::filesystem::path dir = cfg::dataDir() / "cli-requests";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir;
}

void WriteArgs(const std::filesystem::path& path,
               const std::vector<std::string>& args) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto count = static_cast<std::uint32_t>(args.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const std::string& arg : args) {
        const auto length = static_cast<std::uint32_t>(arg.size());
        out.write(reinterpret_cast<const char*>(&length), sizeof(length));
        out.write(arg.data(), static_cast<std::streamsize>(arg.size()));
    }
}

bool ReadArgs(const std::filesystem::path& path,
              std::vector<std::string>& args) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || count > 4096) return false;
    args.clear();
    args.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t length = 0;
        in.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!in || length > (1U << 20)) return false;
        std::string arg(length, '\0');
        in.read(arg.data(), static_cast<std::streamsize>(length));
        if (!in) return false;
        args.push_back(std::move(arg));
    }
    return true;
}

std::filesystem::path RequestPath(long pid) {
    return requestDir() / std::format("{}.req", pid);
}

std::filesystem::path ResponsePath(long pid) {
    return requestDir() / std::format("{}.res", pid);
}

long CurrentPid() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// ---- stdout/stderr 重定向（把 cli::run 的输出写进响应文件）-------------------
struct OutputRedirect {
    int savedOut = -1;
    int savedErr = -1;
    int target = -1;

    explicit OutputRedirect(const std::filesystem::path& path) {
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        target = ::_open(path.string().c_str(),
                         _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (target < 0) return;
        savedOut = ::_dup(::_fileno(stdout));
        savedErr = ::_dup(::_fileno(stderr));
        ::_dup2(target, ::_fileno(stdout));
        ::_dup2(target, ::_fileno(stderr));
#else
        target = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (target < 0) return;
        savedOut = ::dup(STDOUT_FILENO);
        savedErr = ::dup(STDERR_FILENO);
        ::dup2(target, STDOUT_FILENO);
        ::dup2(target, STDERR_FILENO);
#endif
    }

    ~OutputRedirect() { Restore(); }

    OutputRedirect(const OutputRedirect&) = delete;
    OutputRedirect& operator=(const OutputRedirect&) = delete;

    void Restore() {
        if (target < 0) return;
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        if (savedOut >= 0) ::_dup2(savedOut, ::_fileno(stdout));
        if (savedErr >= 0) ::_dup2(savedErr, ::_fileno(stderr));
        if (savedOut >= 0) ::_close(savedOut);
        if (savedErr >= 0) ::_close(savedErr);
        ::_close(target);
#else
        if (savedOut >= 0) ::dup2(savedOut, STDOUT_FILENO);
        if (savedErr >= 0) ::dup2(savedErr, STDERR_FILENO);
        if (savedOut >= 0) ::close(savedOut);
        if (savedErr >= 0) ::close(savedErr);
        ::close(target);
#endif
        savedOut = savedErr = target = -1;
    }
};

void WriteFooter(const std::filesystem::path& path, int code) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    const auto exitCode = static_cast<std::int32_t>(code);
    out.write(reinterpret_cast<const char*>(&exitCode), sizeof(exitCode));
    out.write(reinterpret_cast<const char*>(&kMagic), sizeof(kMagic));
}

} // namespace

/// owner 进程：清理上轮残留并确保请求目录存在。应用启动任务里调用一次。
export void startCommandServer() {
    const std::filesystem::path dir = requestDir();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".req") || name.ends_with(".res")) {
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }
}

/// owner 进程：服务所有待处理请求。在启动泵里调用（命令会阻塞，调用方应放到
/// 任务线程）。返回本次服务的请求数。
export int servePendingCommands() {
    const std::filesystem::path dir = requestDir();
    std::error_code error;
    std::vector<std::filesystem::path> requests;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".req")) requests.push_back(entry.path());
    }
    std::ranges::sort(requests);
    int served = 0;
    for (const std::filesystem::path& request : requests) {
        std::vector<std::string> args;
        if (!ReadArgs(request, args)) {
            std::error_code removeError;
            std::filesystem::remove(request, removeError);
            continue;
        }
        // 文件名 <pid>.req → 响应写回 <pid>.res。
        const std::filesystem::path response =
            request.parent_path() / (request.stem().string() + ".res");
        {
            OutputRedirect redirect(response);
            const int code = cli::run(args);
            redirect.Restore();
            WriteFooter(response, code);
        }
        std::error_code removeError;
        std::filesystem::remove(request, removeError);
        ++served;
    }
    return served;
}

/// 非 owner 进程：把命令转发给运行中的实例，拿到输出与退出码。
/// 失败（owner 未起来 / 超时）返回 false。
export bool tryForwardCommand(const std::vector<std::string>& args, int& code) {
    const long pid = CurrentPid();
    const std::filesystem::path request = RequestPath(pid);
    const std::filesystem::path response = ResponsePath(pid);
    std::error_code error;
    std::filesystem::remove(response, error);
    WriteArgs(request, args);
    if (!std::filesystem::exists(request, error)) return false;

    const auto deadline = std::chrono::steady_clock::now() + kClientTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(response, error)) {
            // 响应先落文本、后追加 8 字节 footer；等 footer 到齐再读。
            const auto size = std::filesystem::file_size(response, error);
            if (!error && size >= 8) {
                std::ifstream in(response, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                if (content.size() < 8) break;
                std::int32_t exitCode = 1;
                std::uint32_t magic = 0;
                std::memcpy(&exitCode, content.data() + content.size() - 8, 4);
                std::memcpy(&magic, content.data() + content.size() - 4, 4);
                if (magic != kMagic) {
                    // 输出还在写：稍后重试。
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    continue;
                }
                std::string output = content.substr(0, content.size() - 8);
                if (!output.empty()) {
                    std::fwrite(output.data(), 1, output.size(), stdout);
                    std::fflush(stdout);
                }
                code = exitCode;
                std::filesystem::remove(request, error);
                std::filesystem::remove(response, error);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    std::filesystem::remove(request, error);
    std::filesystem::remove(response, error);
    return false;
}

} // namespace clashflux::cli_ipc
