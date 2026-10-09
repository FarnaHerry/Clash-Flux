// Native Windows SCM/IPC backend. Plain TU: no UI, modules or persistence.
#pragma once
#ifdef _WIN32
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace clashflux::windows_service {
struct Options {
    std::wstring name = L"ClashFluxService";
    std::wstring pipe = L"\\\\.\\pipe\\ClashFlux.Service.v1";
    std::filesystem::path directory; // Empty selects Program Files/Clash-Flux-Service.
};
struct Hooks {
    std::function<std::string(std::string_view)> command;
    std::function<void()> resetTun;
    std::function<void()> shutdown;
};
struct Info {
    bool reachable = false;
    std::string protocol;
    std::string version;
    std::string error;
};
std::string currentUserSid();
std::filesystem::path storageDirectory();
bool installed(const Options& options = {});
Info query(std::string_view version, const Options& options = {});
bool ensureCompatible(std::string_view version, std::string& error,
                      const Options& options = {});
std::optional<std::string> request(std::string_view command, std::string& error,
                                 unsigned timeoutMs = 2000, const Options& options = {});
// install/uninstall require elevation. manageElevated elevates only this command.
int install(std::string_view ownerSid, std::string& error, const Options& options = {});
int uninstall(std::string& error, const Options& options = {});
bool manageElevated(bool remove, std::string& error);
int run(std::string_view version, Hooks hooks, const Options& options = {});
} // namespace clashflux::windows_service
#endif
