// service_stub.inc — 非 Linux 平台服务 stub
// 由 src/service.cppm 在同一模块单元中包含。

#if defined(__ANDROID__) || defined(CLASHFLUX_ANDROID_LEGACY)
#define CLASHFLUX_SERVICE_EXPORT inline
#else
#define CLASHFLUX_SERVICE_EXPORT export inline
#endif

CLASHFLUX_SERVICE_EXPORT bool available() { return false; }
CLASHFLUX_SERVICE_EXPORT bool reachable() { return false; }
CLASHFLUX_SERVICE_EXPORT ServiceInfo query() {
    ServiceInfo info;
    info.error = "服务模式仅支持 Linux";
    return info;
}
CLASHFLUX_SERVICE_EXPORT bool ensureCompatible(std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT bool installed() { return false; }
CLASHFLUX_SERVICE_EXPORT bool startCore(const std::filesystem::path&, std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT bool stopCore(std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT bool coreRunning() { return false; }
CLASHFLUX_SERVICE_EXPORT bool pptpAvailable() { return false; }
CLASHFLUX_SERVICE_EXPORT bool pptpSessionAlive(std::string_view) { return false; }
CLASHFLUX_SERVICE_EXPORT bool startPptp(std::string_view, std::string_view,
                      std::span<const std::string>, PptpSessionInfo&, std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT bool applyPptpRoutes(std::string_view, std::span<const std::string>,
                            std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT bool stopPptp(std::string_view, std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
CLASHFLUX_SERVICE_EXPORT int install() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}
CLASHFLUX_SERVICE_EXPORT int uninstall() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}
CLASHFLUX_SERVICE_EXPORT int run() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}

#undef CLASHFLUX_SERVICE_EXPORT
