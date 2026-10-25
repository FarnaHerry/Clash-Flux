#pragma once

namespace clashflux::runtime_compile {
#if defined(__ANDROID__)
inline constexpr bool kAndroid = true;
#else
inline constexpr bool kAndroid = false;
#endif

// Apply runtime platform policy before the very first compiler invocation,
// including dependency discovery and candidate checks.
template<class Options>
inline void Normalize(Options& options, bool android = kAndroid) {
    if (android && options.tunInbound) options.tunStrictRoute = false;
}

inline bool GeoInputsChanged(bool hasResources, bool directoryChanged,
                             bool android = kAndroid) {
    // Android GEO is packaged by Gradle; its prefetch does not change files.
    return directoryChanged || (!android && hasResources);
}
} // namespace clashflux::runtime_compile
