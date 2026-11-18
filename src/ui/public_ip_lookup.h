#pragma once

#include <array>
#include <string_view>

namespace clashflux::ui {

// Public egress echo services shared by the Home network card and website
// location correlation. Both return an IP and an ISO country code decoded by
// wire::DecodePublicIpLookup; the foreign service is tried first, then the
// domestic one, so either split-routing side usually has a reachable echo.
inline constexpr std::array<std::string_view, 2> kPublicIpLookupUrls{
    "https://api.country.is",
    "https://api.mir6.com/api/ip_json",
};

} // namespace clashflux::ui
