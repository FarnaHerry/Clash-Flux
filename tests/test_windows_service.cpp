// Real SCM + named-pipe + Job Object regression; needs an elevated Windows runner.
// Uses an isolated service and a fake core; never changes proxy/TUN/routes/database.
#include "../src/windows_service.h"
#include "../src/service_protocol.h"
#include "../src/win32_raii.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#include <chrono>
using namespace clashflux;
using win32::UniqueHandle;
using win32::UniqueServiceHandle;
namespace ws = windows_service;
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::filesystem::path executable() {
    wchar_t buffer[32768];
    check(GetModuleFileNameW(nullptr, buffer, 32768) != 0, "executable path");
    return buffer;
}
ws::Options optionsFor(const std::filesystem::path& root) {
    ws::Options options;
    options.directory = root; options.name = root.filename().wstring();
    options.pipe = L"\\\\.\\pipe\\" + options.name;
    return options;
}
#ifdef CLASHFLUX_TEST_SERVICE_ENGINE
int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) != "run" || std::string(argv[2]) != "-c" || std::string(argv[4]) != "-D") return 3;
    // A stable live child whose lifetime can only end via the production Job.
    Sleep(INFINITE); return 0;
}
#else
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "service" && std::string(argv[2]) == "run") {
        const auto root = executable().parent_path().parent_path();
        return ws::run("fixture-v1", {
            .command = [root](std::string_view command) -> std::string {
                if (command == "TEST_DELAY") {
                    std::ofstream(root / L"delay-started.txt") << "started";
                    Sleep(3000); return "OK";
                }
                if (command == "TEST_THROW") throw std::runtime_error("fixture exception");
                return "ERR Unknown command";
            },
            .resetTun = [root] { std::ofstream(root / L"route-cleanup.txt") << "released"; },
            .shutdown = [root] { std::ofstream(root / L"session-cleanup.txt") << "disconnected"; },
        }, optionsFor(root));
    }
    try {
        PWSTR raw = nullptr;
        check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &raw)), "Program Files");
        const auto root = std::filesystem::path(raw) / (L"ClashFluxServiceTest-" + std::to_wstring(GetCurrentProcessId()));
        CoTaskMemFree(raw);
        const auto options = optionsFor(root);
        struct Cleanup {
            ws::Options options;
            ~Cleanup() { std::string error; ws::uninstall(error, options); }
        } cleanup{options};
        std::string error;
        check(!ws::installed(options), "fixture service must not already exist");
        check(ws::install(ws::currentUserSid(), error, options) == 0, "install: " + error);
        check(ws::installed(options), "SCM registration");
        check(ws::query("fixture-v1", options).error.empty(), "VERSION handshake");
        const auto mismatch = ws::query("fixture-v2", options);
        check(mismatch.reachable && !mismatch.error.empty(), "version mismatch must fail");
        check(ws::request("STATUS", error, 2000, options) == "STOPPED", "initial status");
        // A pipe with the right protocol but the wrong server PID is rejected
        // before any control command is sent to it.
        auto spoofOptions = options; spoofOptions.pipe += L".Spoof";
        UniqueHandle spoof{CreateNamedPipeW(spoofOptions.pipe.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 2000, nullptr)};
        check(spoof.valid(), "spoof fixture pipe");
        DWORD leaked = 0;
        std::thread impersonator([&] {
            if (ConnectNamedPipe(spoof.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                char buffer[32]; ReadFile(spoof.get(), buffer, sizeof(buffer), &leaked, nullptr);
            }
        });
        const auto spoofReply = ws::request("STOP", error, 2000, spoofOptions);
        impersonator.join();
        check(!spoofReply && leaked == 0, "SCM PID binding rejects spoofed pipe without leaking command");

        check(ws::request("TEST_THROW", error, 2000, options) == "ERR fixture exception", "exception response");
        check(ws::query("fixture-v1", options).error.empty(), "server survives callback exception");
        auto slowOperation = std::async(std::launch::async, [&] {
            std::string failure;
            return ws::request("TEST_DELAY", failure, 10000, options);
        });
        const auto delayDeadline = GetTickCount64() + 5000;
        while (!std::filesystem::exists(root / L"delay-started.txt") && GetTickCount64() < delayDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(std::filesystem::exists(root / L"delay-started.txt"), "long command actually started");
        check(ws::query("fixture-v1", options).error.empty(), "VERSION stays responsive during a long operation");
        check(ws::request("STATUS", error, 2000, options) == "STOPPED", "STATUS stays responsive during a long operation");
        check(slowOperation.get() == "OK", "long operation result");

        check(!ws::request("VERSION\nSTOP", error, 2000, options), "reject command injection");
        check(!ws::request(std::string(service_protocol::kMaxCommand + 1, 'x'), error, 2000, options), "bound commands");
        const auto invalidStart = ws::request("START 00", error, 2000, options);
        check(invalidStart && invalidStart->starts_with("ERR "), "reject encoded NUL");
        const auto configRoot = std::filesystem::temp_directory_path() / (L"ClashFluxConfig-路径-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(configRoot);
        struct ConfigCleanup {
            std::filesystem::path path;
            ~ConfigCleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        } configCleanup{configRoot};
        const auto config = configRoot / L"config.json";
        std::ofstream(config) << "{\"log\":{\"level\":\"info\"}}";
        const auto encoded = config.u8string();
        const auto command = "START " + service_protocol::hex(std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
        // Drop the administrator group on this thread. The installation user's
        // explicit DACL must suffice for querying, starting and pipe control.
        HANDLE tokenRaw = nullptr; check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &tokenRaw), "token");
        UniqueHandle token{tokenRaw};
        HANDLE restrictedRaw = nullptr;
        BYTE adminSid[SECURITY_MAX_SID_SIZE]; DWORD sidSize = sizeof(adminSid);
        check(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidSize), "admin SID");
        SID_AND_ATTRIBUTES disabled{adminSid, 0};
        check(CreateRestrictedToken(token.get(), DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0, nullptr, &restrictedRaw), "restricted token");
        UniqueHandle restricted{restrictedRaw};
        check(ImpersonateLoggedOnUser(restricted.get()), "impersonation");
        const auto ordinaryReply = ws::request(command, error, 10000, options);
        RevertToSelf();
        check(ordinaryReply == "OK", "ordinary owner START: " + error);
        const auto firstStatus = ws::request("STATUS", error, 2000, options);
        check(firstStatus && firstStatus->starts_with("RUNNING "), "running child PID");
        const DWORD childPid = std::stoul(firstStatus->substr(8));
        UniqueHandle child{OpenProcess(SYNCHRONIZE, FALSE, childPid)};
        check(child.valid(), "child handle");
        // A staging failure must leave the installed service and live child intact.
        const auto sourceEngine = executable().parent_path() / L"engines" / L"sing-box.exe";
        const auto hiddenEngine = executable().parent_path() / L"engines" / L"sing-box.staging-test";
        std::filesystem::rename(sourceEngine, hiddenEngine);
        struct RestoreEngine {
            std::filesystem::path original, hidden;
            ~RestoreEngine() { std::error_code ec;
                if (std::filesystem::exists(hidden)) std::filesystem::rename(hidden, original, ec); }
        } restoreEngine{sourceEngine, hiddenEngine};
        const int failedUpdate = ws::install(ws::currentUserSid(), error, options);
        std::filesystem::rename(hiddenEngine, sourceEngine);
        check(failedUpdate != 0, "missing engine rejects service update");
        check(ws::request("STATUS", error, 2000, options) == firstStatus, "staging failure preserves running core");

        check(ws::request("STOP", error, 10000, options) == "OK", "core STOP");
        check(WaitForSingleObject(child.get(), 3000) == WAIT_OBJECT_0, "core job terminated");
        check(std::filesystem::exists(root / L"route-cleanup.txt"), "service owns route cleanup");
        check(ws::request("STOP", error, 10000, options) == "OK", "idempotent STOP");
        check(ws::request(command, error, 10000, options) == "OK", "restart core");
        const auto restarted = ws::request("STATUS", error, 2000, options);
        check(restarted && restarted->starts_with("RUNNING "), "restarted status");
        UniqueHandle newChild{OpenProcess(SYNCHRONIZE, FALSE, std::stoul(restarted->substr(8)))};
        check(newChild.valid(), "restarted core handle");
        auto service = UniqueServiceHandle{OpenServiceW(
            UniqueServiceHandle{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)}.get(),
            options.name.c_str(), SERVICE_QUERY_STATUS | SERVICE_STOP)};
        check(service.valid(), "SCM handle");
        // Force the service process to die. Job Object cleanup must terminate
        // the core even when ServiceMain cannot run its graceful cleanup.
        SERVICE_STATUS_PROCESS status{}; DWORD bytes = 0;
        check(QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes), "service PID");
        UniqueHandle daemon{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, status.dwProcessId)};
        check(daemon.valid() && TerminateProcess(daemon.get(), 1), "force daemon exit");
        check(WaitForSingleObject(newChild.get(), 5000) == WAIT_OBJECT_0, "daemon crash kills job child");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        check(ws::ensureCompatible("fixture-v1", error, options), "SCM restart/reconnect: " + error);
        SERVICE_STATUS stopped{};
        check(ControlService(service.get(), SERVICE_CONTROL_STOP, &stopped), "SCM graceful stop");
        const auto deadline = GetTickCount64() + 5000;
        do {
            check(QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                  reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes), "graceful stop status");
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (GetTickCount64() < deadline);
        check(status.dwCurrentState == SERVICE_STOPPED &&
              std::filesystem::exists(root / L"session-cleanup.txt"), "SCM stop releases session hooks");
        check(ws::ensureCompatible("fixture-v1", error, options), "owner restarts stopped service");
        service.reset();
        // Leave a silent pipe client connected: SCM stop must cancel pending IO.
        UniqueHandle silent{CreateFileW(options.pipe.c_str(), GENERIC_READ | FILE_WRITE_DATA,
            0, nullptr, OPEN_EXISTING, 0, nullptr)};
        check(silent.valid(), "silent client");
        check(ws::uninstall(error, options) == 0, "uninstall with stalled client: " + error);
        check(!ws::installed(options), "SCM deletion");
        check(!std::filesystem::exists(root), "dedicated payload removed");
        check(std::filesystem::exists(config), "user configuration preserved");
        check(ws::uninstall(error, options) == 0, "idempotent uninstall");
        std::cout << "Windows SCM/pipe/owner SID/job crash cleanup/stop/uninstall passed\n";
        return 0;
    } catch (const std::exception& exception) {
        RevertToSelf(); std::cerr << exception.what() << '\n'; return 1;
    }
}
#endif
