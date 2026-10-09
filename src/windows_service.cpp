#include "windows_service.h"
#ifdef _WIN32
#include "win32_raii.h"
#include "service_protocol.h"
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace clashflux::windows_service {
namespace {
using clashflux::win32::UniqueHandle;
using clashflux::win32::UniqueServiceHandle;
using LocalMemory = std::unique_ptr<void, decltype(&LocalFree)>;
using namespace service_protocol;
std::string winError(std::string_view operation, DWORD code = GetLastError()) {
    return std::string(operation) + " (Win32 " + std::to_string(code) + ")";
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 service field");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-16 service field");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring quote(std::wstring_view value) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); out += L'"'; return out;
}
std::filesystem::path selfExe() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size == buffer.size()) throw std::runtime_error("Cannot locate executable");
    buffer.resize(size); return buffer;
}
std::filesystem::path directory(const Options& options) {
    if (!options.directory.empty()) return options.directory;
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw)))
        throw std::runtime_error("Cannot locate Program Files");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(raw, &CoTaskMemFree);
    return std::filesystem::path(raw) / L"Clash-Flux-Service";
}
LocalMemory descriptor(const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                               &raw, nullptr))
        throw std::runtime_error(winError("Security descriptor"));
    return LocalMemory(raw, &LocalFree);
}
std::string tokenSid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) throw std::runtime_error(winError("TokenUser size"));
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size))
        throw std::runtime_error(winError("TokenUser"));
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &raw))
        throw std::runtime_error(winError("SID"));
    const LocalMemory owner(raw, &LocalFree);
    return utf8(raw);
}
bool validSid(std::string_view value) {
    PSID raw = nullptr;
    if (!ConvertStringSidToSidW(wide(value).c_str(), &raw)) return false;
    const LocalMemory owner(raw, &LocalFree);
    // One concrete user, never Everyone/Authenticated Users/Builtin groups.
    return IsValidSid(raw) && (value.starts_with("S-1-5-21-") || value.starts_with("S-1-12-1-"));
}
UniqueServiceHandle openService(const Options& options, DWORD rights) {
    UniqueServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.valid()) return {};
    const auto handle = OpenServiceW(manager.get(), options.name.c_str(), rights);
    const DWORD error = GetLastError();
    manager.reset();
    SetLastError(error); // Closing the manager must not erase OpenService's error.
    return UniqueServiceHandle{handle};
}
bool status(SC_HANDLE service, SERVICE_STATUS_PROCESS& out) {
    DWORD size = 0;
    return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&out), sizeof(out), &size) != FALSE;
}
bool waitState(SC_HANDLE service, DWORD state, unsigned timeoutMs, std::string& error) {
    const auto end = GetTickCount64() + timeoutMs;
    do {
        SERVICE_STATUS_PROCESS info{};
        if (!status(service, info)) { error = winError("QueryServiceStatus"); return false; }
        if (info.dwCurrentState == state) return true;
        if (state == SERVICE_RUNNING && info.dwCurrentState == SERVICE_STOPPED) {
            error = winError("Service stopped during startup", info.dwWin32ExitCode); return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (GetTickCount64() < end);
    error = "Windows service state transition timed out"; return false;
}
bool startService(SC_HANDLE service, std::string& error) {
    SERVICE_STATUS_PROCESS info{};
    if (!status(service, info)) { error = winError("QueryServiceStatus"); return false; }
    if (info.dwCurrentState == SERVICE_RUNNING) return true;
    if (info.dwCurrentState == SERVICE_STOP_PENDING && !waitState(service, SERVICE_STOPPED, 30000, error)) return false;
    if (info.dwCurrentState != SERVICE_START_PENDING &&
        !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = winError("StartService"); return false;
    }
    return waitState(service, SERVICE_RUNNING, 30000, error);
}
bool stopService(SC_HANDLE service, std::string& error) {
    SERVICE_STATUS_PROCESS info{};
    if (!status(service, info)) { error = winError("QueryServiceStatus"); return false; }
    if (info.dwCurrentState == SERVICE_STOPPED) return true;
    const UniqueHandle process{info.dwProcessId ? OpenProcess(SYNCHRONIZE, FALSE, info.dwProcessId) : nullptr};
    SERVICE_STATUS ignored{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE && info.dwCurrentState != SERVICE_STOP_PENDING) {
        error = winError("ControlService STOP"); return false;
    }
    if (!waitState(service, SERVICE_STOPPED, 30000, error)) return false;
    if (process.valid() && WaitForSingleObject(process.get(), 30000) != WAIT_OBJECT_0) {
        error = "Service process did not release its payload"; return false;
    }
    return true;
}
// Every pending I/O is cancelled and drained before its OVERLAPPED/buffer dies.
bool completeIo(HANDLE handle, OVERLAPPED& operation, BOOL immediate,
                DWORD initialError, DWORD& count, unsigned timeoutMs, HANDLE stop = nullptr, HANDLE child = nullptr) {
    if (!immediate && initialError != ERROR_IO_PENDING) return false;
    if (!immediate) {
        HANDLE events[] = {operation.hEvent, stop, child};
        const DWORD wait = WaitForMultipleObjects(child ? 3 : stop ? 2 : 1, events, FALSE, timeoutMs);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(handle, &operation);
            GetOverlappedResult(handle, &operation, &count, TRUE);
            SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
            return false;
        }
    }
    return GetOverlappedResult(handle, &operation, &count, FALSE) != FALSE;
}
bool transfer(HANDLE handle, char* data, DWORD size, bool write, DWORD& count,
              unsigned timeoutMs, HANDLE stop = nullptr) {
    const UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) return false;
    OVERLAPPED operation{}; operation.hEvent = event.get();
    const BOOL immediate = write ? WriteFile(handle, data, size, nullptr, &operation)
                                 : ReadFile(handle, data, size, nullptr, &operation);
    const DWORD initialError = immediate ? ERROR_SUCCESS : GetLastError();
    return completeIo(handle, operation, immediate, initialError, count, timeoutMs, stop);
}
bool readLine(HANDLE pipe, std::string& line, std::size_t limit, unsigned timeoutMs,
              HANDLE stop = nullptr) {
    const auto deadline = GetTickCount64() + timeoutMs;
    char buffer[1024];
    while (line.size() <= limit) {
        if (GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        DWORD count = 0;
        if (!transfer(pipe, buffer, sizeof(buffer), false, count,
                      static_cast<unsigned>(deadline - GetTickCount64()), stop) || !count) return false;
        line.append(buffer, count);
        const auto newline = line.find('\n');
        if (newline != std::string::npos)
            return newline == line.size() - 1 && newline <= limit &&
                   line.find('\0') == std::string::npos && line.find('\r') == std::string::npos
                   ? (line.pop_back(), true) : false;
    }
    SetLastError(ERROR_BUFFER_OVERFLOW); return false;
}
bool writeLine(HANDLE pipe, std::string line, unsigned timeoutMs, HANDLE stop = nullptr) {
    if (line.size() > 4096 || line.find_first_of("\r\n") != std::string::npos) line = "ERR Invalid reply";
    line += '\n';
    DWORD count = 0;
    return transfer(pipe, line.data(), static_cast<DWORD>(line.size()), true, count, timeoutMs, stop) && count == line.size();
}
void secureDirectory(const std::filesystem::path& path) {
    // Reject junctions at every level before doing privileged file writes.
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Service path contains a reparse point");
        if (current == current.root_path()) break;
    }
    auto security = descriptor(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw std::runtime_error(winError("Create service directory"));
    BOOL present = FALSE, defaulted = FALSE; PACL acl = nullptr;
    GetSecurityDescriptorDacl(security.get(), &present, &acl, &defaulted);
    PSID owner = nullptr;
    GetSecurityDescriptorOwner(security.get(), &owner, &defaulted);
    const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, acl, nullptr);
    if (code != ERROR_SUCCESS) throw std::runtime_error(winError("Protect service directory", code));
}
// Serialize elevated install/uninstall across GUI and CLI processes. The lock
// grants no ordinary-user access and uses a distinct identity for test fixtures.
class ManagementLock {
public:
    explicit ManagementLock(const Options& options) {
        auto security = descriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
        const auto name = L"Global\\" + options.name + L".Management";
        mutex_.reset(CreateMutexW(&attributes, FALSE, name.c_str()));
        if (!mutex_.valid()) throw std::runtime_error(winError("Service management lock"));
        const auto result = WaitForSingleObject(mutex_.get(), 30000);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
            throw std::runtime_error("Another service management command is running");
    }
    ~ManagementLock() { ReleaseMutex(mutex_.get()); }
private:
    UniqueHandle mutex_;
};
struct Server {
    Options options;
    Hooks hooks;
    std::string version;
    std::string ownerSid;
    UniqueHandle stop;
    UniqueHandle queryReady;
    std::atomic<bool> queryFailed{false};
    std::mutex engineMutex;
    UniqueHandle engine;
    UniqueHandle job;
    SERVICE_STATUS_HANDLE statusHandle = nullptr; // SCM-owned, not CloseHandle.
    std::mutex statusMutex;
    DWORD checkpoint = 0;
    void report(DWORD state, DWORD error = NO_ERROR) {
        std::lock_guard lock(statusMutex);
        SERVICE_STATUS info{};
        info.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        info.dwCurrentState = state;
        info.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        info.dwWin32ExitCode = error;
        info.dwCheckPoint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? ++checkpoint : 0;
        info.dwWaitHint = info.dwCheckPoint ? 90000 : 0;
        SetServiceStatus(statusHandle, &info);
    }
    bool running() {
        std::lock_guard lock(engineMutex);
        return engine.valid() && WaitForSingleObject(engine.get(), 0) == WAIT_TIMEOUT;
    }
    std::string coreStatus() {
        std::lock_guard lock(engineMutex);
        return engine.valid() && WaitForSingleObject(engine.get(), 0) == WAIT_TIMEOUT
            ? "RUNNING " + std::to_string(GetProcessId(engine.get())) : "STOPPED";
    }
    void stopEngine() {
        if (hooks.resetTun) hooks.resetTun(); // Release routes before capture device.
        if (job.valid()) TerminateJobObject(job.get(), 0);
        if (engine.valid()) WaitForSingleObject(engine.get(), 2000);
        { std::lock_guard lock(engineMutex); engine.reset(); }
        job.reset();
    }
    ~Server() { try { stopEngine(); if (hooks.shutdown) hooks.shutdown(); } catch (...) {} }
    bool startEngine(std::string_view field, HANDLE pipe, std::string& error) {
        const auto decoded = unhex(field);
        if (!decoded) { error = "Invalid configuration path"; return false; }
        const std::filesystem::path config{wide(*decoded)};
        if (!config.is_absolute() || config.extension() != L".json" ||
            !config.root_name().wstring().ends_with(L":") ||
            std::ranges::any_of(config, [](const auto& part) { return part == L".."; })) {
            error = "Configuration must be a local absolute JSON path"; return false;
        }
        // Check read access as the requesting user, never use SYSTEM to read a
        // configuration the authenticated client itself cannot open.
        if (!ImpersonateNamedPipeClient(pipe)) { error = winError("Impersonate client"); return false; }
        struct Revert { ~Revert() { RevertToSelf(); } } revert;
        UniqueHandle input{CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        LARGE_INTEGER size{};
        if (!input.valid() || GetFileType(input.get()) != FILE_TYPE_DISK ||
            !GetFileSizeEx(input.get(), &size) || size.QuadPart <= 0 || size.QuadPart > 32 * 1024 * 1024) {
            error = "Configuration is unreadable or too large"; return false;
        }
        // Take an owning snapshot while impersonating. The privileged core never
        // reopens the caller's replaceable config path after access validation.
        std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD count = 0;
        if (!ReadFile(input.get(), content.data(), static_cast<DWORD>(content.size()), &count, nullptr) || count != content.size()) {
            error = "Cannot snapshot configuration"; return false;
        }
        input.reset();
        if (!RevertToSelf()) { error = winError("RevertToSelf"); return false; }
        const auto root = directory(options);
        const auto snapshot = root / L"runtime.json";
        { std::ofstream out(snapshot, std::ios::binary | std::ios::trunc);
          out.write(content.data(), static_cast<std::streamsize>(content.size())); out.close();
          if (!out) { error = "Cannot write service configuration snapshot"; return false; } }
        stopEngine();
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.valid() || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = winError("Create core job"); return false;
        }
        const auto binary = root / L"bin" / L"engines" / L"sing-box.exe";
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        const UniqueHandle log{CreateFileW((root / L"sing-box.service.log").c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!log.valid()) { error = winError("Open core log"); return false; }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log.get(); startup.hStdError = log.get(); startup.hStdInput = nullptr;
        auto command = quote(binary.wstring()) + L" run -c " + quote(snapshot.wstring()) + L" -D " + quote(config.parent_path().wstring());
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, root.c_str(), &startup, &process)) {
            error = winError("Create core process"); return false;
        }
        { std::lock_guard lock(engineMutex); engine.reset(process.hProcess); }
        const UniqueHandle thread{process.hThread};
        if (!AssignProcessToJobObject(job.get(), engine.get()) || ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            error = winError("Assign core job"); TerminateProcess(engine.get(), 1); stopEngine(); return false;
        }
        if (WaitForSingleObject(engine.get(), 100) == WAIT_OBJECT_0) {
            error = "Core exited during startup; inspect sing-box.service.log"; stopEngine(); return false;
        }
        return true;
    }
    bool authorized(HANDLE pipe) {
        if (!ImpersonateNamedPipeClient(pipe)) return false;
        struct Revert { ~Revert() { RevertToSelf(); } } revert;
        HANDLE raw = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) return false;
        const UniqueHandle token{raw};
        const auto sid = tokenSid(token.get());
        if (sid == ownerSid || sid == "S-1-5-18") return true;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID adminRaw = nullptr;
        if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminRaw)) return false;
        const std::unique_ptr<void, decltype(&FreeSid)> admin(adminRaw, &FreeSid);
        BOOL member = FALSE;
        return CheckTokenMembership(token.get(), admin.get(), &member) && member;
    }
    void loop(bool queries = false) {
        const auto pipeName = options.pipe + (queries ? L".Query" : L"");
        const auto security = descriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0012019b;;;" + wide(ownerSid) + L")");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
        bool first = true;
        while (WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT) {
            UniqueHandle pipe{CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 2000, &attributes)};
            if (!pipe.valid()) throw std::runtime_error(winError("Create service pipe"));
            if (first) {
                if (queries) SetEvent(queryReady.get());
                else report(SERVICE_RUNNING);
                first = false;
            }
            const UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!event.valid()) throw std::runtime_error(winError("Pipe event"));
            OVERLAPPED operation{}; operation.hEvent = event.get();
            const BOOL connected = ConnectNamedPipe(pipe.get(), &operation);
            const DWORD initial = connected ? ERROR_SUCCESS : GetLastError();
            DWORD count = 0;
            if (initial != ERROR_PIPE_CONNECTED &&
                !completeIo(pipe.get(), operation, connected, initial, count, INFINITE, stop.get(), queries ? nullptr : engine.get())) {
                if (!queries && engine.valid() && !running()) stopEngine();
                continue;
            }
            std::string command;
            if (!readLine(pipe.get(), command, kMaxCommand, 5000, stop.get())) continue;
            std::string reply;
            try {
                if (!authorized(pipe.get())) reply = "ERR Unauthorized service client";
                else if (command == "VERSION") reply = "clash-flux-service 1 app " + version;
                else if (command == "STATUS") reply = coreStatus();
                else if (queries) {
                    if (hooks.command && (command == "PPTP_AVAILABLE" || command.starts_with("PPTP_STATUS ")))
                        reply = hooks.command(command);
                    else reply = "ERR Read-only query pipe";
                }
                else if (command == "STOP") { stopEngine(); reply = "OK"; }
                else if (command.starts_with("START ")) {
                    std::string error;
                    reply = startEngine(command.substr(6), pipe.get(), error) ? "OK" : "ERR " + error;
                } else if (hooks.command) reply = hooks.command(command);
                else reply = "ERR Unknown service command";
            } catch (const std::exception& error) { reply = "ERR " + std::string(error.what()); }
            // Do not FlushFileBuffers: an unresponsive client must not prevent
            // SCM stop. The client acknowledges receiving the complete reply.
            if (writeLine(pipe.get(), reply, 2000, stop.get())) {
                char ack = 0; DWORD received = 0;
                transfer(pipe.get(), &ack, 1, false, received, 2000, stop.get());
            }
            DisconnectNamedPipe(pipe.get());
            if (!queries && !running() && engine.valid()) stopEngine();
        }
    }
};
Server* activeServer = nullptr; // Owned by run(); dispatcher joins ServiceMain.
DWORD WINAPI control(DWORD code, DWORD, void*, void* context) {
    auto& server = *static_cast<Server*>(context);
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        server.report(SERVICE_STOP_PENDING);
        SetEvent(server.stop.get());
    } else if (code == SERVICE_CONTROL_INTERROGATE) {
        // SCM already owns the last reported status; do not overwrite pending.
    } else return ERROR_CALL_NOT_IMPLEMENTED;
    return NO_ERROR;
}
void WINAPI serviceMain(DWORD, wchar_t**) {
    auto& server = *activeServer;
    server.statusHandle = RegisterServiceCtrlHandlerExW(server.options.name.c_str(), control, &server);
    if (!server.statusHandle) return;
    server.report(SERVICE_START_PENDING);
    DWORD code = NO_ERROR;
    std::thread queryWorker;
    try {
        std::ifstream owner(directory(server.options) / L"owner.sid");
        std::getline(owner, server.ownerSid);
        if (!owner || !validSid(server.ownerSid)) throw std::runtime_error("Missing service owner SID");
        queryWorker = std::thread([&server] {
            try { server.loop(true); }
            catch (...) { server.queryFailed.store(true); SetEvent(server.stop.get()); }
        });
        HANDLE ready[] = {server.queryReady.get(), server.stop.get()};
        if (WaitForMultipleObjects(2, ready, FALSE, 20000) != WAIT_OBJECT_0 || server.queryFailed.load())
            throw std::runtime_error("Cannot start service query endpoint");
        server.loop();
        if (server.queryFailed.load()) code = ERROR_SERVICE_SPECIFIC_ERROR;
    } catch (...) { code = ERROR_SERVICE_SPECIFIC_ERROR; }
    SetEvent(server.stop.get());
    if (queryWorker.joinable()) queryWorker.join();
    server.report(SERVICE_STOP_PENDING);
    try { server.stopEngine(); if (server.hooks.shutdown) server.hooks.shutdown(); }
    catch (...) { code = ERROR_SERVICE_SPECIFIC_ERROR; }
    server.report(SERVICE_STOPPED, code);
}
} // namespace

std::filesystem::path storageDirectory() { return directory({}); }

std::string currentUserSid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error(winError("OpenProcessToken"));
    const UniqueHandle token{raw}; return tokenSid(token.get());
}
bool installed(const Options& options) { return openService(options, SERVICE_QUERY_STATUS).valid(); }
std::optional<std::string> request(std::string_view command, std::string& error,
                                  unsigned timeoutMs, const Options& options) {
    error.clear();
    if (command.empty() || command.size() > kMaxCommand || command.find_first_of("\r\n") != std::string_view::npos || command.find('\0') != std::string_view::npos) {
        error = "Invalid service command"; return {};
    }
    const bool queries = command == "VERSION" || command == "STATUS" || command == "PPTP_AVAILABLE" || command.starts_with("PPTP_STATUS ");
    const auto pipeName = options.pipe + (queries ? L".Query" : L"");
    if (!WaitNamedPipeW(pipeName.c_str(), timeoutMs)) { error = winError("Service pipe unavailable"); return {}; }
    const UniqueHandle pipe{CreateFileW(pipeName.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr)};
    if (!pipe.valid()) { error = winError("Connect service pipe"); return {}; }
    // Bind IPC to the actual SCM-owned process, rejecting a spoofed pipe server.
    const auto service = openService(options, SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS info{}; ULONG serverPid = 0;
    if (!service.valid() || !status(service.get(), info) || info.dwCurrentState != SERVICE_RUNNING ||
        !GetNamedPipeServerProcessId(pipe.get(), &serverPid) || serverPid != info.dwProcessId) {
        error = "Pipe server is not the registered Windows service"; return {};
    }
    auto line = std::string(command) + '\n'; DWORD written = 0;
    if (!transfer(pipe.get(), line.data(), static_cast<DWORD>(line.size()), true, written, timeoutMs) || written != line.size()) {
        error = winError("Write service command"); return {};
    }
    std::string reply;
    if (!readLine(pipe.get(), reply, 4096, timeoutMs)) { error = winError("Read service reply"); return {}; }
    char ack = '\n'; transfer(pipe.get(), &ack, 1, true, written, 2000);
    return reply;
}
Info query(std::string_view version, const Options& options) {
    Info info;
    const auto reply = request("VERSION", info.error, 2000, options);
    if (!reply) return info;
    const auto fields = words(*reply);
    if (fields.size() != 4 || fields[0] != "clash-flux-service" || fields[2] != "app") {
        info.error = "Invalid Windows service version reply"; return info;
    }
    info.reachable = true; info.protocol = fields[1]; info.version = fields[3];
    if (info.protocol != "1" || info.version != version)
        info.error = "Windows service version differs; reinstall the service from Settings";
    return info;
}
bool ensureCompatible(std::string_view version, std::string& error, const Options& options) {
    auto info = query(version, options);
    if (!info.reachable) {
        const auto service = openService(options, SERVICE_QUERY_STATUS | SERVICE_START);
        if (!service.valid()) { error = "Windows service is not installed; install it from Settings"; return false; }
        if (!startService(service.get(), error)) return false;
        info = query(version, options);
    }
    error = info.error; return info.reachable && error.empty();
}
int install(std::string_view requestedSid, std::string& error, const Options& options) {
    error.clear();
    try {
        const ManagementLock lock(options);
        const std::string sid = requestedSid.empty() ? currentUserSid() : std::string(requestedSid);
        if (!validSid(sid)) throw std::runtime_error("Invalid installation owner SID");
        const UniqueServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
        if (!manager.valid()) throw std::runtime_error(winError("Install requires administrator authorization"));
        auto service = openService(options, SERVICE_ALL_ACCESS);
        if (!service.valid() && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) throw std::runtime_error(winError("Open service"));
        const auto root = directory(options);
        secureDirectory(root);
        const auto staging = root / L"staging";
        if (std::filesystem::exists(staging)) std::filesystem::remove_all(staging);
        secureDirectory(staging);
        const auto source = selfExe();
        const auto sourceDir = source.parent_path();
        std::filesystem::copy_file(source, staging / L"clash-flux.exe");
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir))
            if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
                std::filesystem::copy_file(entry.path(), staging / entry.path().filename());
        secureDirectory(staging / L"engines");
        std::filesystem::copy_file(sourceDir / L"engines" / L"sing-box.exe", staging / L"engines" / L"sing-box.exe");
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir / L"engines"))
            if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
                std::filesystem::copy_file(entry.path(), staging / L"engines" / entry.path().filename());
        // CopyFile may carry source permissions; explicitly protect every staged
        // executable/DLL and its owner before SCM can load it as LocalSystem.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(staging))
            secureDirectory(entry.path());
        std::string previousSid;
        { std::ifstream owner(root / L"owner.sid"); std::getline(owner, previousSid); }
        if (service.valid() && !stopService(service.get(), error)) return 1;
        const auto payload = root / L"bin", previous = root / L"previous";
        if (std::filesystem::exists(previous)) std::filesystem::remove_all(previous);
        const bool hadPayload = std::filesystem::exists(payload);
        if (hadPayload) std::filesystem::rename(payload, previous);
        try {
            std::filesystem::rename(staging, payload);
            const auto command = quote((payload / L"clash-flux.exe").wstring()) + L" service run";
            if (!service.valid()) {
                service = UniqueServiceHandle{CreateServiceW(manager.get(), options.name.c_str(), L"Clash-Flux Network Service",
                    SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                    command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr)};
                if (!service.valid()) throw std::runtime_error(winError("CreateService"));
            } else if (!ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr, nullptr))
                throw std::runtime_error(winError("ChangeServiceConfig"));
            // Normal owner may query/start; changing configuration or stopping
            // the service itself remains an administrator-only SCM operation.
            auto security = descriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;LCRP;;;" + wide(sid) + L")");
            if (!SetServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, security.get()))
                throw std::runtime_error(winError("Service DACL"));
            { std::ofstream owner(root / L"owner.sid", std::ios::trunc); owner << sid << '\n'; owner.close();
              if (!owner) throw std::runtime_error("Cannot record installation owner"); }
            SC_ACTION actions[] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
            SERVICE_FAILURE_ACTIONSW failures{}; failures.dwResetPeriod = 86400;
            failures.cActions = 3; failures.lpsaActions = actions;
            if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failures))
                throw std::runtime_error(winError("Service recovery"));
            SERVICE_FAILURE_ACTIONS_FLAG failureFlag{TRUE};
            if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag))
                throw std::runtime_error(winError("Service failure recovery flag"));
            if (!startService(service.get(), error)) throw std::runtime_error(error);
        } catch (...) {
            if (service.valid()) { std::string ignored; stopService(service.get(), ignored); }
            std::filesystem::remove_all(payload);
            if (hadPayload) {
                std::filesystem::rename(previous, payload);
                if (validSid(previousSid)) {
                    std::ofstream(root / L"owner.sid", std::ios::trunc) << previousSid << '\n';
                    auto security = descriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;LCRP;;;" + wide(previousSid) + L")");
                    SetServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, security.get());
                }
                std::string ignored; startService(service.get(), ignored);
            }
            else if (service.valid()) DeleteService(service.get());
            throw;
        }
        if (hadPayload) std::filesystem::remove_all(previous);
        return 0;
    } catch (const std::exception& exception) { error = exception.what(); return 1; }
}
int uninstall(std::string& error, const Options& options) {
    error.clear();
    try {
        const ManagementLock lock(options);
        auto service = openService(options, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
        if (!service.valid()) {
            if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return 0;
            error = winError("Uninstall requires administrator authorization"); return 1;
        }
        if (!stopService(service.get(), error)) return 1;
        if (!DeleteService(service.get())) { error = winError("DeleteService"); return 1; }
        service.reset();
        // Dedicated service payload only; never the GUI's data/database/profiles.
        std::filesystem::remove_all(directory(options));
        return 0;
    } catch (const std::exception& exception) { error = exception.what(); return 1; }
}
bool manageElevated(bool remove, std::string& error) {
    try {
        const auto exe = selfExe();
        const auto args = remove ? L"service uninstall" : L"service install --owner-sid " + wide(currentUserSid());
        SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas"; info.lpFile = exe.c_str(); info.lpParameters = args.c_str(); info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) { error = winError("Service authorization cancelled or failed"); return false; }
        const UniqueHandle process{info.hProcess};
        if (!process.valid() || WaitForSingleObject(process.get(), 60000) != WAIT_OBJECT_0) {
            error = "Service management is still running; check service status"; return false;
        }
        DWORD code = 1;
        if (!GetExitCodeProcess(process.get(), &code) || code != 0) { error = "Windows service management failed"; return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
int run(std::string_view version, Hooks hooks, const Options& options) {
    Server server; server.options = options; server.version = version; server.hooks = std::move(hooks);
    server.stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    server.queryReady.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!server.stop.valid() || !server.queryReady.valid()) return 1;
    activeServer = &server;
    SERVICE_TABLE_ENTRYW table[] = {{server.options.name.data(), serviceMain}, {nullptr, nullptr}};
    const BOOL success = StartServiceCtrlDispatcherW(table);
    activeServer = nullptr;
    return success ? 0 : 1;
}
} // namespace clashflux::windows_service
#endif
