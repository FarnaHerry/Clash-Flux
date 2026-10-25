#pragma once
#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <net/if.h>
#include <optional>
#include <signal.h>
#include <sys/wait.h>

namespace clashflux::pptp_health {
// Only a reaped child or ESRCH proves process exit. ECHILD with a live PID
// cannot establish ownership; permission errors and interrupted reads are unknown.
inline std::optional<bool> ChildEvidence(pid_t pid) {
    if (pid <= 0) return false;
    int status = 0;
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) return false;
    if (result == 0) return true;
    if (errno == ECHILD && ::kill(pid, 0) < 0 && errno == ESRCH) return false;
    return {};
}

inline std::optional<bool> InterfaceEvidence(const char* name, unsigned expected) {
    if (!name || !*name || !expected) return {};
    errno = 0;
    const auto index = ::if_nametoindex(name);
    if (index) return index == expected;
    if (errno == ENXIO || errno == ENODEV) return false;
    return {};
}

inline std::optional<bool> SessionEvidence(std::optional<bool> process,
                                         std::optional<bool> interface) {
    if ((process && !*process) || (interface && !*interface)) return false;
    if (process && interface) return true;
    return {};
}
} // namespace clashflux::pptp_health
#endif
