#include "app_update_package.h"

#if defined(_WIN32) || defined(__linux__)
#if !defined(__ANDROID__)
#include "../app_update_hash.h"
#endif
#include <chrono>
#include <cerrno>
#include <fstream>
#include <random>
#include <thread>
#if defined(_WIN32)
#include "../win32_raii.h"
#elif !defined(__ANDROID__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace clashflux::ui {

std::string ReadAppUpdateNotice(const std::filesystem::path& cache) {
    const auto file = cache / "app-updates" / "last-result.txt";
    std::ifstream input(file, std::ios::binary);
    std::array<char, 1024> buffer{};
    input.read(buffer.data(), buffer.size());
    const std::string message(buffer.data(), static_cast<std::size_t>(input.gcount()));
    input.close();
    if (!message.empty()) {
        std::error_code error;
        std::filesystem::remove(file, error);
    }
    return message;
}

AppUpdateTarget DetectAppUpdateTarget() {
#if defined(__ANDROID__) && defined(__aarch64__)
    return AppUpdateTarget::AndroidArm64;
#elif defined(_WIN32) && defined(_M_X64)
    return AppUpdateTarget::WindowsX64;
#elif defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
    if (::access("/usr/bin/pkexec", X_OK) != 0) return AppUpdateTarget::Manual;
    // Select the host package database, not whichever conversion tool happens
    // to be installed. Arch/other distributions retain the manual release link.
    const bool deb = std::filesystem::exists("/var/lib/dpkg/status") && ::access("/usr/bin/dpkg", X_OK) == 0;
    const bool rpm = (std::filesystem::exists("/var/lib/rpm") ||
        std::filesystem::exists("/usr/lib/sysimage/rpm")) && ::access("/usr/bin/rpm", X_OK) == 0;
#if defined(__aarch64__)
    return deb ? AppUpdateTarget::LinuxArm64Deb : rpm ? AppUpdateTarget::LinuxArm64Rpm : AppUpdateTarget::Manual;
#else
    return deb ? AppUpdateTarget::LinuxX64Deb : rpm ? AppUpdateTarget::LinuxX64Rpm : AppUpdateTarget::Manual;
#endif
#else
    return AppUpdateTarget::Manual;
#endif
}

std::shared_ptr<AppUpdatePackage> AppUpdatePackage::Prepare(const std::filesystem::path& cache,
                                                          wire::ReleaseAsset asset) {
    auto result = std::make_shared<AppUpdatePackage>();
    const auto root = cache / "app-updates";
    std::filesystem::create_directories(root);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto directory = root / ("pending-" + std::to_string(std::random_device{}()) +
            "-" + std::to_string(std::random_device{}()));
        std::error_code error;
        if (!std::filesystem::create_directory(directory, error)) {
            if (error && error != std::errc::file_exists) break;
            continue;
        }
        result->directory_ = directory;
#if defined(__ANDROID__)
        result->path_ = directory / "package.part.apk";
#else
        result->path_ = directory / "package.part";
#endif
        result->asset_ = std::move(asset);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        result->output_.open(result->path_, std::ios::binary | std::ios::trunc);
        if (!result->output_) throw std::runtime_error("Cannot write update download");
        return result;
    }
    throw std::runtime_error("Cannot create an exclusive update directory");
}

AppUpdatePackage::~AppUpdatePackage() {
    if (handedOff_ || directory_.empty()) return;
    output_.close();
    std::error_code error;
    // Known files only; never recursively clean a cache or user data directory.
    for (const auto* name : {"package.part", "package.part.apk", "package.exe", "package.deb", "package.rpm", "package.apk", "updater.exe", "ready.txt"})
        std::filesystem::remove(directory_ / name, error);
    std::filesystem::remove(directory_, error);
}

void AppUpdatePackage::Write(std::span<const std::byte> bytes) {
    if (bytes.size() > asset_.size - written_) throw std::runtime_error("Update exceeds the declared size");
    output_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output_) throw std::runtime_error("Cannot save update download");
    written_ += bytes.size();
}

void AppUpdatePackage::CloseDownload() {
    if (output_.is_open()) output_.close();
    if (!output_) throw std::runtime_error("Cannot close update download");
}

void AppUpdatePackage::Commit() {
    CloseDownload();
#if defined(__ANDROID__)
    if (!androidVerified_) throw std::runtime_error("APK has not passed platform verification");
#else
    update::VerifyFile(path_, asset_.size, std::string_view(asset_.digest).substr(7));
#endif
    const auto extension = std::filesystem::path(asset_.name).extension();
    if (extension != ".exe" && extension != ".deb" && extension != ".rpm" && extension != ".apk")
        throw std::runtime_error("Unsupported update package type");
    const auto final = directory_ / ("package" + extension.string());
    std::filesystem::rename(path_, final);
    path_ = final;
}

#if defined(_WIN32)
static std::wstring QuoteUpdateArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
#endif

void AppUpdatePackage::InstallDesktopUpdate(const std::filesystem::path& executableDirectory) {
#if defined(__ANDROID__)
    static_cast<void>(executableDirectory);
    throw std::runtime_error("Android installation is owned by the system package installer");
#else
    update::VerifyFile(path_, asset_.size, std::string_view(asset_.digest).substr(7));
    const auto ready = directory_ / "ready.txt";
    std::filesystem::remove(ready);
    std::filesystem::remove(directory_.parent_path() / "last-result.txt");
#if defined(_WIN32)
    const auto helper = directory_ / "updater.exe";
    std::filesystem::copy_file(executableDirectory / "clash-flux-updater.exe", helper,
        std::filesystem::copy_options::overwrite_existing);
    const auto hash = std::filesystem::path(asset_.digest.substr(7)).wstring();
    std::wstring command = QuoteUpdateArgument(helper.wstring()) + L" --apply " +
        std::to_wstring(GetCurrentProcessId()) + L" " + QuoteUpdateArgument(path_.wstring()) + L" " + hash +
        L" " + std::to_wstring(asset_.size) + L" exe " +
        QuoteUpdateArgument((executableDirectory / "clash-flux.exe").wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    win32::UniqueHandle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null.valid()) throw std::runtime_error("Cannot prepare update helper output");
    startup.hStdInput = startup.hStdOutput = startup.hStdError = null.get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, directory_.c_str(), &startup, &process))
        throw std::runtime_error("Cannot start update helper");
    win32::UniqueHandle processOwner(process.hProcess), threadOwner(process.hThread);
#else
    const auto helper = executableDirectory / "clash-flux-updater";
    std::vector<std::string> arguments{helper.string(), "--apply", std::to_string(::getpid()), path_.string(),
        asset_.digest.substr(7), std::to_string(asset_.size), path_.extension() == ".deb" ? "deb" : "rpm",
        (executableDirectory / "clash-flux").string()};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    if (::posix_spawn(&child, helper.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        throw std::runtime_error("Cannot start update helper");
    int status = 0;
    pid_t completed;
    do { completed = ::waitpid(child, &status, 0); } while (completed < 0 && errno == EINTR);
    if (completed == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        handedOff_ = true;
        return;
    }
    std::ifstream result(directory_ / "result.txt");
    std::array<char, 8192> message{};
    result.read(message.data(), message.size());
    throw std::runtime_error(result.gcount() > 0
        ? std::string(message.data(), static_cast<std::size_t>(result.gcount()))
        : "Linux update helper failed; the application remains open");
#endif
#if defined(_WIN32)
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        std::ifstream input(ready);
        std::string line;
        if (std::getline(input, line) && line == "ready") {
            handedOff_ = true;
            return;
        }
        if (WaitForSingleObject(processOwner.get(), 0) != WAIT_TIMEOUT) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    TerminateProcess(processOwner.get(), 1);
    WaitForSingleObject(processOwner.get(), 5000);
    throw std::runtime_error("Update helper failed to prepare; the application remains open");
#endif
#endif
}

} // namespace clashflux::ui
#endif
