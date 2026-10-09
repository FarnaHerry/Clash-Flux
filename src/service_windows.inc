// Windows module facade: SCM/pipe I/O stays in a plain TU; privileged hooks
// execute only in ServiceMain, without a Runtime, CoreStore or database.
namespace {
namespace native_service = clashflux::windows_service;
namespace protocol = clashflux::service_protocol;
vpn::compensation::RouteLease& serviceTunRoutes() {
    static vpn::compensation::RouteLease lease;
    return lease;
}
bool windowsOk(const std::string& command, std::string& error, unsigned timeout = 10000) {
    const auto reply = native_service::request(command, error, timeout);
    if (!reply) return false;
    if (*reply == "OK") return true;
    error = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
    return false;
}
std::string privilegedCommand(std::string_view command) {
    const auto fields = protocol::words(command);
    if (fields.empty()) return "ERR Invalid command";
    if (command == "PPTP_AVAILABLE") return pptp::PrivilegedPptpAvailable() ? "YES" : "NO";
    if (command == "TUN_RESET") { serviceTunRoutes().reset(); return "OK"; }
    if (fields[0] == "TUN_PREPARE" && fields.size() >= 3) {
        const auto tun = protocol::unhex(fields[1]);
        std::size_t count = 0;
        const auto [end, code] = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), count);
        if (!tun || tun->empty() || code != std::errc{} || end != fields[2].data() + fields[2].size() ||
            count > 64 || fields.size() < 3 + count || fields.size() > 3 + count + 4096)
            return "ERR Invalid TUN fields";
        std::vector<std::string> interfaces, exclusions;
        for (std::size_t i = 3; i < fields.size(); ++i) {
            auto field = protocol::unhex(fields[i]);
            if (!field || field->empty()) return "ERR Invalid TUN field";
            (i < 3 + count ? interfaces : exclusions).push_back(std::move(*field));
        }
        std::string error;
        auto next = vpn::compensation::PrepareWindowsTun(*tun, interfaces, exclusions, error);
        if (!next) return "ERR " + error;
        serviceTunRoutes() = std::move(next);
        return "OK";
    }
    if (fields.size() < 2) return "ERR Unknown command";
    const auto id = protocol::unhex(fields[1]);
    if (!id || id->empty() || id->size() > 128) return "ERR Invalid PPTP ID";
    if (fields[0] == "PPTP_STATUS" && fields.size() == 2)
        return pptp::PrivilegedPptpSessionAlive(*id) ? "CONNECTED" : "DISCONNECTED";
    if (fields[0] == "PPTP_STOP" && fields.size() == 2) {
        pptp::PrivilegedPptpDisconnect(*id); return "OK";
    }
    const bool start = fields[0] == "PPTP_START";
    if ((!start && fields[0] != "PPTP_ROUTES") || (start && fields.size() < 3))
        return "ERR Unknown command";
    std::vector<std::string> routes;
    for (std::size_t i = start ? 3 : 2; i < fields.size(); ++i) {
        auto route = protocol::unhex(fields[i]);
        if (!route || route->empty()) return "ERR Invalid PPTP route";
        routes.push_back(std::move(*route));
    }
    std::string error;
    if (!start) return pptp::PrivilegedPptpApplyRoutes(*id, routes, error) ? "OK" : "ERR " + error;
    const auto config = protocol::unhex(fields[2]);
    if (!config || config->empty()) return "ERR Invalid PPTP config";
    std::string interfaceName, gateway, transportAddress;
    if (!pptp::PrivilegedPptpConnect(*id, *config, routes, interfaceName, gateway, error, &transportAddress))
        return "ERR " + error;
    return "OK " + protocol::hex(interfaceName) + " " + protocol::hex(gateway) + " " + protocol::hex(transportAddress);
}
} // namespace

export std::filesystem::path privilegedWorkDir() { return native_service::storageDirectory(); }

export ServiceInfo query() {
    const auto native = native_service::query(CLASHFLUX_VERSION);
    return {.reachable = native.reachable, .protocolVersion = native.protocol,
            .applicationVersion = native.version, .error = native.error};
}
export bool available() { return query().compatible(); }
export bool reachable() { return query().reachable; }
export bool installed() { return native_service::installed(); }
export bool ensureCompatible(std::string& error) {
    return native_service::ensureCompatible(CLASHFLUX_VERSION, error);
}
export bool manageElevated(bool remove, std::string& error) {
    return native_service::manageElevated(remove, error);
}
export int installForUser(std::string_view sid) {
    std::string error;
    const int code = native_service::install(sid, error);
    if (code) std::println(stderr, "{}", error);
    return code;
}
export int install() { return installForUser({}); }
export int uninstall() {
    std::string error;
    const int code = native_service::uninstall(error);
    if (code) std::println(stderr, "{}", error);
    return code;
}
export int run() {
    return native_service::run(CLASHFLUX_VERSION, {
        .command = privilegedCommand,
        .resetTun = [] { serviceTunRoutes().reset(); },
        .shutdown = [] { pptp::PrivilegedPptpShutdown(); },
    });
}
export bool startCore(const std::filesystem::path& config, std::string& error) {
    if (!ensureCompatible(error)) return false;
    const auto value = config.u8string();
    return windowsOk("START " + protocol::hex(std::string(reinterpret_cast<const char*>(value.data()), value.size())), error);
}
export bool stopCore(std::string& error) { return windowsOk("STOP", error); }
export bool coreRunning() {
    std::string error;
    const auto reply = native_service::request("STATUS", error);
    return reply && reply->starts_with("RUNNING ");
}
export bool prepareWindowsTun(std::string_view tun, std::span<const std::string> interfaces,
                              std::span<const std::string> exclusions, std::string& error) {
    std::string command = "TUN_PREPARE " + protocol::hex(tun) + " " + std::to_string(interfaces.size());
    for (const auto& value : interfaces) command += " " + protocol::hex(value);
    for (const auto& value : exclusions) command += " " + protocol::hex(value);
    return windowsOk(command, error);
}
export bool pptpAvailable() {
    std::string error;
    const auto reply = native_service::request("PPTP_AVAILABLE", error);
    return reply && *reply == "YES";
}
export bool pptpSessionAlive(std::string_view id) {
    std::string error;
    const auto reply = native_service::request("PPTP_STATUS " + protocol::hex(id), error);
    return reply && *reply == "CONNECTED";
}
export bool startPptp(std::string_view id, std::string_view config,
                      std::span<const std::string> routes, PptpSessionInfo& session,
                      std::string& error) {
    if (!ensureCompatible(error)) return false;
    std::string command = "PPTP_START " + protocol::hex(id) + " " + protocol::hex(config);
    for (const auto& route : routes) command += " " + protocol::hex(route);
    const auto reply = native_service::request(command, error, 90000);
    if (!reply) return false;
    const auto fields = protocol::words(*reply);
    if (fields.size() != 4 || fields[0] != "OK") {
        error = reply->starts_with("ERR ") ? reply->substr(4) : "Invalid PPTP reply"; return false;
    }
    const auto iface = protocol::unhex(fields[1]), gateway = protocol::unhex(fields[2]), transport = protocol::unhex(fields[3]);
    if (!iface || !gateway || !transport) { error = "Invalid PPTP session"; return false; }
    session = {*iface, *gateway, *transport}; return true;
}
export bool applyPptpRoutes(std::string_view id, std::span<const std::string> routes, std::string& error) {
    std::string command = "PPTP_ROUTES " + protocol::hex(id);
    for (const auto& route : routes) command += " " + protocol::hex(route);
    return windowsOk(command, error);
}
export bool stopPptp(std::string_view id, std::string& error) {
    return windowsOk("PPTP_STOP " + protocol::hex(id), error);
}
