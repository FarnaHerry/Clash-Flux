#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ras.h>
#include <raserror.h>
#include <optional>

namespace clashflux::pptp_health {
inline std::optional<bool> RasEvidence(DWORD error, RASCONNSTATE state,
                                       std::optional<bool> interfaceMatches) {
    if (error == ERROR_INVALID_HANDLE || error == ERROR_NO_CONNECTION) return false;
    if (error != ERROR_SUCCESS) return {};
    if (state == RASCS_Disconnected) return false;
    if (state != RASCS_Connected) return {};
    return interfaceMatches;
}
}
#endif
