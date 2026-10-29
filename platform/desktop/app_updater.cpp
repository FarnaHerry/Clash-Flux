// Independent, unprivileged handoff. It verifies and retains the package before
// acknowledging readiness, waits for the GUI to exit, then invokes the native
// installer. It never opens a database or creates a HuxerUI Runtime.
#include "../../src/app_update_hash.h"
#include <charconv>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <shellapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using FileOwner = clashflux::update::FileOwner;

static std::uint64_t Number(const std::string& value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result == 0)
        throw std::runtime_error("Invalid update argument");
    return result;
}

static int Apply(const std::vector<fs::path>& arguments) {
    if (arguments.size() != 8 || arguments[1] != "--apply") return 2;
    const auto parentId = Number(arguments[2].string());
    const fs::path package = fs::absolute(arguments[3]);
    const auto hash = arguments[4].string();
    const auto size = Number(arguments[5].string());
    const auto kind = arguments[6].string();
    const fs::path application = arguments[7];
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos ||
        size > 512ULL * 1024 * 1024 || !application.is_absolute()) return 2;
    const auto directory = package.parent_path();
    bool ready = false;
    try {
#if defined(_WIN32)
        if (kind != "exe" || package.filename() != "package.exe" || parentId > MAXDWORD) return 2;
        clashflux::win32::UniqueHandle parent(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentId)));
        clashflux::win32::UniqueHandle lockedPackage(CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!parent.valid() || !lockedPackage.valid() ||
            !GetFileInformationByHandleEx(lockedPackage.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw std::runtime_error("Cannot retain the application or update package");
        clashflux::update::VerifyFile(package, size, hash);
#else
        if ((kind != "deb" && kind != "rpm") || package.filename() != "package." + kind ||
            parentId != static_cast<std::uint64_t>(::getppid())) return 2;
        // Seal a private snapshot before requesting root authorization. A file
        // in a user-writable cache must not change between hashing and dpkg/rpm.
        const int memory = static_cast<int>(::syscall(SYS_memfd_create, "clash-flux-update", MFD_ALLOW_SEALING));
        FileOwner snapshot(memory < 0 ? nullptr : ::fdopen(memory, "w+b"));
        if (!snapshot) {
            if (memory >= 0) ::close(memory);
            throw std::runtime_error("Cannot prepare immutable update snapshot");
        }
        FileOwner input(std::fopen(package.c_str(), "rb"));
        if (!input) throw std::runtime_error("Cannot open update package");
        std::array<unsigned char, 64 * 1024> buffer{};
        std::uint64_t copied = 0;
        for (;;) {
            const auto count = std::fread(buffer.data(), 1, buffer.size(), input.get());
            copied += count;
            if (copied > size || std::fwrite(buffer.data(), 1, count, snapshot.get()) != count)
                throw std::runtime_error("Cannot snapshot update package");
            if (count < buffer.size()) {
                if (std::ferror(input.get())) throw std::runtime_error("Cannot read update package");
                break;
            }
        }
        if (std::fflush(snapshot.get()) || std::fseek(snapshot.get(), 0, SEEK_SET) ||
            ::fcntl(memory, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
            throw std::runtime_error("Cannot seal update snapshot");
        clashflux::update::VerifyStream(snapshot.get(), size, hash);
        const std::string immutable = "/proc/" + std::to_string(::getpid()) + "/fd/" + std::to_string(memory);
#endif
        {
            std::ofstream output(directory / "ready.txt", std::ios::trunc);
            output << "ready\n";
            output.close();
            if (!output) throw std::runtime_error("Cannot acknowledge update readiness");
        }
        ready = true;
#if defined(_WIN32)
        if (WaitForSingleObject(parent.get(), 120000) != WAIT_OBJECT_0)
            throw std::runtime_error("Application did not exit; installation cancelled");
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.lpVerb = L"open";
        launch.lpFile = package.c_str();
        launch.lpDirectory = directory.c_str();
        launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) throw std::runtime_error("Installation cancelled or could not be started");
        clashflux::win32::UniqueHandle installer(launch.hProcess);
        if (!installer.valid() || WaitForSingleObject(installer.get(), INFINITE) != WAIT_OBJECT_0)
            throw std::runtime_error("Cannot wait for the Windows installer");
        DWORD code = 1;
        if (!GetExitCodeProcess(installer.get(), &code) || (code != 0 && code != 3010))
            throw std::runtime_error("Installation did not complete; reopen Clash-Flux to retry");
        lockedPackage.reset();
#else
        unsigned waited = 0;
        while (::getppid() == static_cast<pid_t>(parentId) && waited++ < 1200)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (::getppid() == static_cast<pid_t>(parentId))
            throw std::runtime_error("Application did not exit; installation cancelled");
        const pid_t installer = ::fork();
        if (installer == 0) {
            if (kind == "deb") ::execl("/usr/bin/pkexec", "pkexec", "/usr/bin/dpkg", "--install", immutable.c_str(), nullptr);
            else ::execl("/usr/bin/pkexec", "pkexec", "/usr/bin/rpm", "--upgrade", immutable.c_str(), nullptr);
            ::_exit(127);
        }
        int status = 0;
        if (installer < 0 || ::waitpid(installer, &status, 0) != installer ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("Installation cancelled or failed; reopen Clash-Flux to retry");
#endif
        std::ofstream(directory / "result.txt") << "Installation completed\n";
        fs::remove(package);
        fs::remove(directory / "ready.txt");
        return 0;
    } catch (const std::exception& error) {
        std::ofstream(directory / "result.txt") << error.what() << '\n';
        if (ready) std::ofstream(directory.parent_path() / "last-result.txt") << error.what() << '\n';
#if defined(_WIN32)
        if (ready) MessageBoxW(nullptr,
            L"Clash-Flux update did not complete. Reopen the application to retry.\nDetails: result.txt in the update cache.",
            L"Clash-Flux", MB_OK | MB_ICONERROR);
#else
        if (ready) {
            // Return to the ordinary user session; never relaunch the GUI as root.
            const auto child = ::fork();
            if (child == 0) { ::execl(application.c_str(), application.c_str(), nullptr); ::_exit(127); }
        }
#endif
        std::fprintf(stderr, "Clash-Flux update: %s\n", error.what());
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        std::vector<fs::path> arguments;
        for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
        return Apply(arguments);
    } catch (...) { return 2; }
}
