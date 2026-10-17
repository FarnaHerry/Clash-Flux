// Real SCM + named-pipe + Job Object regression; needs an elevated Windows runner.
// Uses an isolated service and a fake core; never changes proxy/TUN/routes/database.
#include "../src/windows_service.h"
#include "../src/service_protocol.h"
#include "../src/win32_raii.h"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <future>
#include <stdexcept>
#include <thread>
#include <chrono>
using namespace clashflux;
using win32::UniqueHandle;
using win32::UniqueServiceHandle;
namespace ws = windows_service;
#ifndef CLASHFLUX_TEST_SERVICE_VERSION
#define CLASHFLUX_TEST_SERVICE_VERSION "fixture-v1"
#endif
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
void protectFixture(const std::filesystem::path& path, bool userWritable = false) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    const wchar_t* sddl = userWritable ? L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;BU)"
                                     : L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &raw, nullptr), "fixture ACL");
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor(raw, &LocalFree);
    PACL acl = nullptr; PSID owner = nullptr; BOOL present, defaulted;
    check(GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted), "fixture DACL");
    check(GetSecurityDescriptorOwner(raw, &owner, &defaulted), "fixture owner");
    check(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, acl, nullptr) == ERROR_SUCCESS, "protect fixture source");
}
UniqueHandle ownerToken() {
    HANDLE tokenRaw = nullptr;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &tokenRaw), "owner token");
    const UniqueHandle token{tokenRaw};
    HANDLE restrictedRaw = nullptr;
    BYTE adminSid[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(adminSid);
    check(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &size), "owner admin SID");
    SID_AND_ATTRIBUTES disabled{adminSid, 0};
    check(CreateRestrictedToken(token.get(), DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0, nullptr, &restrictedRaw), "owner restricted token");
    return UniqueHandle{restrictedRaw};
}
#ifdef CLASHFLUX_TEST_SERVICE_ENGINE
int main(int argc, char** argv) {
    if (argc != 6 || std::string(argv[1]) != "run" || std::string(argv[2]) != "-c" || std::string(argv[4]) != "-D") return 3;
    // A stable live child whose lifetime can only end via the production Job.
    Sleep(INFINITE); return 0;
}
#else
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "service" && std::string(argv[2]) == "apply-upgrade") {
        std::uintptr_t handle = 0;
        const std::string value(argv[3]);
        const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), handle);
        if (code != std::errc{} || end != value.data() + value.size()) return 2;
        std::string error;
        const int result = ws::applyUpgrade(handle, error, optionsFor(executable().parent_path().parent_path()));
        if (result) std::cerr << error << '\n';
        return result;
    }
    if (argc == 2 && (std::string(argv[1]) == "client-upgrade" || std::string(argv[1]) == "client-reject")) {
        try {
            const auto token = ownerToken();
            check(ImpersonateLoggedOnUser(token.get()), "ordinary owner impersonation");
            struct Revert { ~Revert() { RevertToSelf(); } } revert;
            std::string error;
            const bool ok = ws::ensureCompatible("fixture-v2", error, optionsFor(executable().parent_path().parent_path()));
            if (std::string(argv[1]) == "client-reject") return !ok && error.find("writable") != std::string::npos ? 0 : 1;
            if (!ok) std::cerr << error << '\n';
            return ok ? 0 : 1;
        } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
    }
    if (argc == 3 && std::string(argv[1]) == "service" && std::string(argv[2]) == "run") {
        const auto root = executable().parent_path().parent_path();
        return ws::run(CLASHFLUX_TEST_SERVICE_VERSION, {
            .command = [root](std::string_view command) -> std::string {
                if (command == "PPTP_AVAILABLE") return "YES";
                if (command.starts_with("PPTP_STATUS ")) return "DISCONNECTED";
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
        check(!ws::request("PPTP_AVAILABLE", error, 500, options), "PPTP capability before installation");
        check(ws::install(ws::currentUserSid(), error, options) == 0, "install: " + error);
        check(ws::installed(options), "SCM registration");
        check(ws::query("fixture-v1", options).error.empty(), "VERSION handshake");
        // CI runner 的 SCM fixture 偶发在 2s 预算内没起好管道（2026-10-17 连续
        // 三轮挂在不同的传输检查点上）：传输失败重试，版本门禁断言本身不放松。
        auto mismatch = ws::query("fixture-v2", options);
        for (int attempt = 0; attempt < 5 && !mismatch.reachable; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            mismatch = ws::query("fixture-v2", options);
        }
        check(mismatch.reachable && !mismatch.error.empty(), "version mismatch must fail");
        check(ws::request("STATUS", error, 2000, options) == "STOPPED", "initial status");
        check(ws::request("PPTP_AVAILABLE", error, 2000, options) == "YES", "PPTP capability after installation");
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
        check(ws::request("PPTP_AVAILABLE", error, 2000, options) == "YES", "PPTP capability during a long operation");
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
        // SCM 重启后服务管道就绪需要时间：轮询等待而不是单次 2s 查询定生死。
        bool pptpAfterRestart = false;
        for (int attempt = 0; attempt < 20 && !pptpAfterRestart; ++attempt) {
            std::string restartError;
            pptpAfterRestart = ws::request("PPTP_AVAILABLE", restartError, 2000, options) == "YES";
            if (!pptpAfterRestart) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        check(pptpAfterRestart, "PPTP capability after SCM restart");
        service.reset();
#ifdef CLASHFLUX_TEST_SERVICE_V2_PATH
        // An ordinary writable build path cannot be used for SYSTEM self-update.
        const auto rejected = ws::request("UPGRADE", error, 30000, options);
        check(rejected && rejected->starts_with("ERR "), "untrusted client source rejected");
        check(ws::query("fixture-v1", options).error.empty(), "source rejection preserves old service");
        check(ws::applyUpgrade(0, error, options) != 0, "helper cannot run as an administrator GUI");
        const auto clientDir = root / L"client";
        std::filesystem::create_directory(clientDir); protectFixture(clientDir);
        std::filesystem::create_directory(clientDir / L"engines"); protectFixture(clientDir / L"engines");
        const auto clientExe = clientDir / L"clash-flux.exe";
        std::filesystem::copy_file(CLASHFLUX_TEST_SERVICE_V2_PATH, clientExe); protectFixture(clientExe);
        std::filesystem::copy_file(sourceEngine, clientDir / L"engines" / L"sing-box.exe");
        protectFixture(clientDir / L"engines" / L"sing-box.exe");
        const auto runClient = [&](const wchar_t* action) {
            auto command = L"\"" + clientExe.wstring() + L"\" " + action;
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            check(CreateProcessW(clientExe.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, clientDir.c_str(), &startup, &process), "trusted update client");
            const UniqueHandle child{process.hProcess}, thread{process.hThread};
            check(WaitForSingleObject(child.get(), 100000) == WAIT_OBJECT_0, "update client completed");
            DWORD code = 1; check(GetExitCodeProcess(child.get(), &code) && !code, "update client result");
        };
        const auto writableDll = clientDir / L"untrusted.dll";
        std::ofstream(writableDll) << "untrusted fixture"; protectFixture(writableDll, true);
        runClient(L"client-reject");
        check(ws::query("fixture-v1", options).error.empty(), "writable DLL rejection preserves old service");
        std::filesystem::remove(writableDll);
        // The service launches the v1 protected helper as SYSTEM, switches to
        // v2, restarts SCM, and preserves the original installation user SID.
        runClient(L"client-upgrade");
        check(ws::query("fixture-v2", options).error.empty(), "self-update activated v2");
        runClient(L"client-upgrade"); // Same version does not launch a helper again.
        check(ws::request("PPTP_AVAILABLE", error, 2000, options) == "YES", "capability after self-update");
#endif
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
