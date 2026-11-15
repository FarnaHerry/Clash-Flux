namespace protocol {

std::string Sip003Escape(std::string_view value) {
    std::string escaped;
    for (const char ch : value) {
        if (ch == '\\' || ch == ';' || ch == '=') escaped += '\\';
        escaped += ch;
    }
    return escaped;
}

void EmitBandwidth(const BandwidthOptions& value, nlohmann::json& out) {
    if (value.upMbps) out["up_mbps"] = *value.upMbps;
    if (value.downMbps) out["down_mbps"] = *value.downMbps;
}

void EmitHopping(const std::optional<PortHopping>& value, bool range, nlohmann::json& out) {
    if (!value) return;
    auto ports = nlohmann::json::array();
    for (const auto& port : value->ports) ports.push_back(std::format("{}:{}", port.first, port.last));
    out.erase("server_port");
    out["server_ports"] = std::move(ports);
    out["hop_interval"] = std::format("{}s", value->minimumSeconds);
    if (range) out["hop_interval_max"] = std::format("{}s", value->maximumSeconds);
}

nlohmann::json EmitSingBox(const ValidatedNode& validated) {
    const auto& node = validated.Spec();
    nlohmann::json out{{"tag", node.origin.name}, {"server", node.server}};
    if (node.port) out["server_port"] = *node.port;
    std::visit([&](const auto& options) {
        using Options = std::decay_t<decltype(options)>;
        if constexpr (std::is_same_v<Options, HttpOptions> || std::is_same_v<Options, Socks5Options>) {
            out["type"] = std::is_same_v<Options, HttpOptions> ? "http" : "socks";
            if (options.auth.username) out["username"] = *options.auth.username;
            if (options.auth.password) out["password"] = *options.auth.password;
        } else if constexpr (std::is_same_v<Options, VmessOptions>) {
            out["type"] = "vmess";
            out["uuid"] = options.uuid;
            out["security"] = options.security;
            if (options.alterId) out["alter_id"] = *options.alterId;
            if (options.packetEncoding) out["packet_encoding"] = *options.packetEncoding;
            if (options.globalPadding) out["global_padding"] = *options.globalPadding;
            if (options.authenticatedLength) out["authenticated_length"] = *options.authenticatedLength;
        } else if constexpr (std::is_same_v<Options, VlessOptions>) {
            out["type"] = "vless";
            out["uuid"] = options.uuid;
            if (options.flow) out["flow"] = *options.flow;
            if (options.packetEncoding) out["packet_encoding"] = *options.packetEncoding;
        } else if constexpr (std::is_same_v<Options, TrojanOptions>) {
            out["type"] = "trojan";
            if (options.password) out["password"] = *options.password;
        } else if constexpr (std::is_same_v<Options, ShadowsocksOptions>) {
            out["type"] = "shadowsocks";
            out["method"] = options.method;
            if (options.password) out["password"] = *options.password;
            std::visit([&](const auto& plugin) {
                using Plugin = std::decay_t<decltype(plugin)>;
                if constexpr (std::is_same_v<Plugin, SimpleObfsPlugin>) {
                    out["plugin"] = "obfs-local";
                    out["plugin_opts"] = std::string("obfs=") +
                        (plugin.mode == SimpleObfsMode::Http ? "http" : "tls") +
                        (plugin.host ? ";obfs-host=" + Sip003Escape(*plugin.host) : "");
                } else if constexpr (std::is_same_v<Plugin, V2RayWebSocketPlugin>) {
                    out["plugin"] = "v2ray-plugin";
                    out["plugin_opts"] = std::string("mode=websocket") +
                        (plugin.host ? ";host=" + Sip003Escape(*plugin.host) : "") +
                        (plugin.path ? ";path=" + Sip003Escape(*plugin.path) : "") +
                        (plugin.multiplex ? ";mux=1" : ";mux=0") + (plugin.tls ? ";tls" : "");
                }
            }, options.plugin);
            if (options.udpOverTcp)
                out["udp_over_tcp"] = {{"enabled", options.udpOverTcp->enabled}, {"version", options.udpOverTcp->version}};
        } else if constexpr (std::is_same_v<Options, Hysteria1Options>) {
            out["type"] = "hysteria";
            EmitBandwidth(options.bandwidth, out);
            if (const auto* bytes = std::get_if<HysteriaBytesAuth>(&options.auth)) out["auth"] = bytes->base64;
            if (const auto* text = std::get_if<HysteriaTextAuth>(&options.auth)) out["auth_str"] = text->text;
            if (options.obfs) out["obfs"] = *options.obfs;
            if (options.disableMtuDiscovery) out["disable_path_mtu_discovery"] = *options.disableMtuDiscovery;
            out["stream_receive_window"] = options.streamReceiveWindow;
            out["connection_receive_window"] = options.connectionReceiveWindow;
            EmitHopping(options.hopping, false, out);
        } else if constexpr (std::is_same_v<Options, Hysteria2Options>) {
            out["type"] = "hysteria2";
            if (options.password) out["password"] = *options.password;
            EmitBandwidth(options.bandwidth, out);
            if (const auto* obfs = std::get_if<SalamanderObfs>(&options.obfs))
                out["obfs"] = {{"type", "salamander"}, {"password", obfs->password}};
            if (const auto* obfs = std::get_if<GeckoObfs>(&options.obfs)) {
                out["obfs"] = {{"type", "gecko"}, {"password", obfs->password}};
                if (obfs->minimumPacketSize) out["obfs"]["min_packet_size"] = *obfs->minimumPacketSize;
                if (obfs->maximumPacketSize) out["obfs"]["max_packet_size"] = *obfs->maximumPacketSize;
            }
            EmitHopping(options.hopping, true, out);
        } else if constexpr (std::is_same_v<Options, TuicOptions>) {
            out["type"] = "tuic";
            out["uuid"] = options.uuid;
            if (options.password) out["password"] = *options.password;
            if (options.congestionControl) out["congestion_control"] = *options.congestionControl;
            if (options.zeroRttHandshake) out["zero_rtt_handshake"] = true;
            if (options.udpRelayMode) out["udp_relay_mode"] = *options.udpRelayMode == TuicUdpRelayMode::Quic ? "quic" : "native";
        } else if constexpr (std::is_same_v<Options, AnyTlsOptions>) {
            out["type"] = "anytls";
            out["password"] = options.password;
            if (options.idleSessionCheckSeconds) out["idle_session_check_interval"] = std::format("{}s", *options.idleSessionCheckSeconds);
            if (options.idleSessionTimeoutSeconds) out["idle_session_timeout"] = std::format("{}s", *options.idleSessionTimeoutSeconds);
            if (options.minimumIdleSessions) out["min_idle_session"] = *options.minimumIdleSessions;
            if (options.clientMetadata) out["client_metadata"] = *options.clientMetadata;
        } else if constexpr (std::is_same_v<Options, Snell4Options>) {
            out["type"] = "snell";
            out["version"] = 4;
            out["psk"] = options.psk;
            out["reuse"] = options.reuse;
            if (options.obfs) {
                out["obfs_mode"] = options.obfs->mode == SnellObfsOptions::Mode::Http ? "http" : "tls";
                out["obfs_host"] = options.obfs->host;
            }
        } else if constexpr (std::is_same_v<Options, SshOptions>) {
            out["type"] = "ssh";
            out["user"] = options.user;
            if (options.password) out["password"] = *options.password;
            if (options.privateKey) {
                out["private_key"] = options.privateKey->pem;
                if (options.privateKey->passphrase) out["private_key_passphrase"] = *options.privateKey->passphrase;
            }
            if (options.hostKeys) out["host_key"] = *options.hostKeys;
            if (options.hostKeyAlgorithms) out["host_key_algorithms"] = *options.hostKeyAlgorithms;
        }
    }, node.options);
    std::visit([&](const auto& transport) {
        using Transport = std::decay_t<decltype(transport)>;
        if constexpr (!std::is_same_v<Transport, DirectTransport>) {
            nlohmann::json value = nlohmann::json::object();
            if constexpr (std::is_same_v<Transport, WebSocketTransport>) {
                value["type"] = "ws";
                if (transport.path) value["path"] = *transport.path;
                if (transport.headers) value["headers"] = *transport.headers;
                if (transport.maxEarlyData) value["max_early_data"] = *transport.maxEarlyData;
                if (transport.earlyDataHeader) value["early_data_header_name"] = *transport.earlyDataHeader;
            } else if constexpr (std::is_same_v<Transport, GrpcTransport>) {
                value["type"] = "grpc";
                if (transport.serviceName) value["service_name"] = *transport.serviceName;
            } else if constexpr (std::is_same_v<Transport, HttpTransport>) {
                value["type"] = "http";
                if (transport.path) value["path"] = *transport.path;
                if (transport.hosts) value["host"] = *transport.hosts;
            } else if constexpr (std::is_same_v<Transport, HttpUpgradeTransport>) {
                value["type"] = "httpupgrade";
                if (transport.path) value["path"] = *transport.path;
                if (transport.host) value["host"] = *transport.host;
            }
            out["transport"] = std::move(value);
        }
    }, node.transport);
    if (node.tls.enabled) {
        nlohmann::json tls{{"enabled", true}};
        if (!UsesQuicTls(node)) tls["utls"] = {{"enabled", true}, {"fingerprint", node.tls.fingerprint}};
        if (node.tls.serverName && !node.tls.serverName->empty()) tls["server_name"] = *node.tls.serverName;
        if (!node.tls.verifyPeer.value_or(true)) tls["insecure"] = true;
        if (node.tls.alpn) tls["alpn"] = *node.tls.alpn;
        if (node.tls.disableServerName) tls["disable_sni"] = true;
        if (node.tls.reality) {
            tls["reality"] = {{"enabled", true}, {"public_key", node.tls.reality->publicKey}};
            if (node.tls.reality->shortId) tls["reality"]["short_id"] = *node.tls.reality->shortId;
        }
        out["tls"] = std::move(tls);
    }
    if (node.dial.detour) out["detour"] = *node.dial.detour;
    if (node.dial.interfaceName && !node.dial.interfaceName->empty()) out["bind_interface"] = *node.dial.interfaceName;
    if (node.dial.routingMark) out["routing_mark"] = *node.dial.routingMark;
    if (node.dial.tcpFastOpen) out["tcp_fast_open"] = *node.dial.tcpFastOpen;
    if (node.dial.multiPathTcp) out["tcp_multi_path"] = *node.dial.multiPathTcp;
    if (validated.TcpOnly()) out["network"] = "tcp";
    return out;
}

} // namespace protocol
