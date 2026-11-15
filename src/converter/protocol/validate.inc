namespace protocol {

bool UsesQuicTls(const NodeSpec& node) {
    return std::holds_alternative<Hysteria1Options>(node.options) ||
        std::holds_alternative<Hysteria2Options>(node.options) || std::holds_alternative<TuicOptions>(node.options);
}

bool HasHysteriaHopping(const NodeSpec& node) {
    if (const auto* hy = std::get_if<Hysteria1Options>(&node.options)) return hy->hopping && !hy->hopping->ports.empty();
    if (const auto* hy = std::get_if<Hysteria2Options>(&node.options)) return hy->hopping && !hy->hopping->ports.empty();
    return false;
}

bool FingerprintSupported(std::string_view value) {
    static constexpr std::string_view fingerprints[]{
        "chrome", "firefox", "edge", "safari", "360", "qq", "ios", "android",
        "random", "randomized", "chrome_psk", "chrome_psk_shuffle",
        "chrome_padding_psk_shuffle", "chrome_pq", "chrome_pq_psk"};
    return std::ranges::find(fingerprints, value) != std::end(fingerprints);
}

bool ValidateFingerprint(std::string_view value, Diagnostics& diagnostics) {
    if (!FingerprintSupported(value))
        return diagnostics.ValueError("client-fingerprint", "不是固定内核支持的指纹");
    // A format adapter may have checked this before a later parsing failure.
    if (std::ranges::any_of(diagnostics.entries, [](const auto& entry) {
            return entry.field == "client-fingerprint" && entry.note.level == Fidelity::Approx;
        })) return true;
    if (value.starts_with("chrome_"))
        diagnostics.Note(Fidelity::Approx, "client-fingerprint",
            std::format("节点「{}」的指纹别名在固定内核中按普通 chrome 处理", diagnostics.origin.name),
            "固定 1.14.2 不保留这些 PSK/PQ 别名差异；需要区别时请更换配置");
    else if (value == "random")
        diagnostics.Note(Fidelity::Approx, "client-fingerprint",
            std::format("节点「{}」使用内核 random 指纹，其抽样分布与 Clash 不保证一致", diagnostics.origin.name));
    return true;
}

bool ValidateInterface(const DialOptions& dial, const TargetCapabilities& target, Diagnostics& diagnostics) {
    if (dial.interfaceName && !dial.interfaceName->empty() && !target.bindInterface && !dial.detour)
        return diagnostics.DialError("interface-name", "在当前手机平台没有原生等价");
    return true;
}

bool ValidateRoutingMark(const DialOptions& dial, const TargetCapabilities& target, Diagnostics& diagnostics) {
    if (dial.routingMark.value_or(0) != 0 && !target.routingMark && !dial.detour)
        return diagnostics.DialError("routing-mark", "仅支持 Linux 桌面");
    return true;
}

bool ValidateTls(const NodeSpec& node, Diagnostics& diagnostics) {
    const auto& tls = node.tls;
    if ((std::holds_alternative<TrojanOptions>(node.options) || std::holds_alternative<AnyTlsOptions>(node.options) ||
            UsesQuicTls(node)) && !tls.enabled)
        return diagnostics.ValueError("tls", "不能关闭该协议必需的 TLS");
    if (!tls.enabled) {
        if (tls.declaredEnabled.value_or(false))
            return diagnostics.ValueError("tls", "包含冲突的启用状态");
        bool valid = true;
        for (const auto& [field, present] : {
                std::pair{"sni", tls.serverName.has_value()}, {"skip-cert-verify", tls.verifyPeer.has_value()},
                {"alpn", tls.alpn.has_value()}, {"client-fingerprint", !tls.fingerprint.empty()},
                {"reality-opts", tls.reality.has_value()}, {"disable-sni", tls.disableServerName}})
            if (present) valid = diagnostics.ValueError(field, "不能在未启用 TLS 时应用") && valid;
        return valid;
    }
    if (std::holds_alternative<Socks5Options>(node.options))
        return diagnostics.ValueError("tls", "不适用于当前 SOCKS5 映射");
    if (std::holds_alternative<ShadowsocksOptions>(node.options))
        return diagnostics.ValueError("tls", "不适用于当前 Shadowsocks 映射");
    if (std::holds_alternative<SshOptions>(node.options) || std::holds_alternative<Snell4Options>(node.options))
        return diagnostics.ValueError("tls", "不适用于当前 SSH/Snell 映射");
    if (tls.declaredEnabled == false)
        return diagnostics.ValueError("tls", "包含冲突的启用状态");
    if (tls.alpn) {
        for (std::size_t index = 0; index < tls.alpn->size(); ++index) {
            const auto& value = (*tls.alpn)[index];
            if (value.empty()) return diagnostics.ValueError(std::format("alpn[{}]", index), "不能为空");
            if (value.size() > 255) return diagnostics.ValueError("alpn", "协议名长度必须为 1–255 字节");
        }
    }
    if (UsesQuicTls(node) && (!tls.fingerprint.empty() || tls.inheritFingerprint || tls.reality))
        return diagnostics.ValueError("client-fingerprint/reality-opts", "不适用于该协议的 QUIC TLS");
    if (!UsesQuicTls(node) && !FingerprintSupported(tls.fingerprint))
        return diagnostics.ValueError("client-fingerprint", "不是固定内核支持的指纹");
    if (tls.reality && tls.reality->publicKey.empty())
        return diagnostics.ValueError("reality-opts.public-key", "不能为空");
    if (tls.disableServerName && !std::holds_alternative<TuicOptions>(node.options))
        return diagnostics.ValueError("disable-sni", "仅适用于当前 TUIC 映射");
    if (std::holds_alternative<Hysteria1Options>(node.options) && tls.alpn && tls.alpn->empty())
        return diagnostics.ValueError("alpn", "原生 Hysteria 将空列表恢复为协议默认，不能保留显式空 ALPN");
    return true;
}

bool ValidateTransport(const NodeSpec& node, Diagnostics& diagnostics) {
    if ((UsesQuicTls(node) || std::holds_alternative<AnyTlsOptions>(node.options) ||
            std::holds_alternative<Snell4Options>(node.options) || std::holds_alternative<SshOptions>(node.options)) &&
        !std::holds_alternative<DirectTransport>(node.transport))
        return diagnostics.ValueError("network", "不适用于该协议的原生传输");
    if (std::holds_alternative<ShadowsocksOptions>(node.options) &&
        !std::holds_alternative<DirectTransport>(node.transport))
        return diagnostics.ValueError("network", "不适用于当前 Shadowsocks 映射");
    if ((std::holds_alternative<HttpOptions>(node.options) || std::holds_alternative<Socks5Options>(node.options)) &&
        !std::holds_alternative<DirectTransport>(node.transport))
        return diagnostics.ValueError("network", "不适用于当前 HTTP/SOCKS5 映射");
    if (const auto* ws = std::get_if<WebSocketTransport>(&node.transport)) {
        if (ws->maxEarlyData && *ws->maxEarlyData < 0)
            return diagnostics.ValueError("ws-opts.max-early-data", "必须为非负整数");
        if (ws->headers && ws->headers->contains(""))
            return diagnostics.ValueError("ws-opts.headers 的键", "不能为空");
    }
    return true;
}

bool ValidateShadowsocks(const ShadowsocksOptions& options, Diagnostics& diagnostics) {
    if (const auto* obfs = std::get_if<SimpleObfsPlugin>(&options.plugin)) {
        if (obfs->host && obfs->host->empty()) return diagnostics.ValueError("plugin-opts.host", "不能为空");
        if (obfs->mode != SimpleObfsMode::Http && obfs->mode != SimpleObfsMode::Tls)
            return diagnostics.ValueError("plugin-opts.mode", "simple-obfs 仅支持 http/tls");
    } else if (const auto* ws = std::get_if<V2RayWebSocketPlugin>(&options.plugin)) {
        if (ws->host && ws->host->empty()) return diagnostics.ValueError("plugin-opts.host", "不能为空");
    }
    if (options.udpOverTcp && options.udpOverTcp->version != 1 && options.udpOverTcp->version != 2)
        return diagnostics.ValueError("udp-over-tcp-version", "仅支持已归一化的 UOT v1 或 v2");
    return true;
}

std::optional<std::size_t> HysteriaBase64Size(std::string_view encoded) {
    std::string text;
    for (char c : encoded) if (c != '\r' && c != '\n') text.push_back(c);
    if (text.size() % 4 != 0) return std::nullopt;
    if (text.empty()) return 0;
    const std::size_t padding = text.ends_with("==") ? 2 : text.ends_with("=") ? 1 : 0;
    for (std::size_t i = 0; i < text.size() - padding; ++i) {
        const unsigned char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) return std::nullopt;
    }
    return text.size() / 4 * 3 - padding;
}

bool ValidateBandwidth(const BandwidthOptions& value, bool required, Diagnostics& diagnostics) {
    for (const auto& [field, rate] : {std::pair{"up", value.upMbps}, {"down", value.downMbps}})
        if ((required && !rate) || (rate && *rate <= 0))
            return diagnostics.ValueError(field, "必须为正整数 Mbps");
    return true;
}

bool ValidateHopping(const std::optional<PortHopping>& hopping, bool range, Diagnostics& diagnostics) {
    if (!hopping) return true;
    if (hopping->ports.empty()) return diagnostics.ValueError("ports", "不能为空");
    for (const auto& port : hopping->ports)
        if (port.first == 0 || port.last < port.first) return diagnostics.ValueError("ports", "必须为有效升序端口范围");
    if (hopping->minimumSeconds < 5 || hopping->maximumSeconds < hopping->minimumSeconds ||
        hopping->maximumSeconds > 9223372036ULL || (!range && hopping->maximumSeconds != hopping->minimumSeconds))
        return diagnostics.ValueError("hop-interval", "必须为当前协议支持的有效秒数或范围");
    return true;
}

bool SshHostAlgorithmSupported(std::string_view value) {
    static constexpr std::string_view algorithms[]{
        "ssh-rsa", "ssh-dss", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
        "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "rsa-sha2-256", "rsa-sha2-512",
        "ssh-rsa-cert-v01@openssh.com", "ssh-dss-cert-v01@openssh.com", "ecdsa-sha2-nistp256-cert-v01@openssh.com",
        "ecdsa-sha2-nistp384-cert-v01@openssh.com", "ecdsa-sha2-nistp521-cert-v01@openssh.com",
        "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com", "ssh-ed25519-cert-v01@openssh.com",
        "sk-ssh-ed25519-cert-v01@openssh.com", "rsa-sha2-256-cert-v01@openssh.com", "rsa-sha2-512-cert-v01@openssh.com"};
    return std::ranges::find(algorithms, value) != std::end(algorithms);
}

bool ValidateRemainingProtocol(const NodeSpec& node, Diagnostics& diagnostics) {
    if (const auto* hy = std::get_if<Hysteria1Options>(&node.options)) {
        if (!ValidateBandwidth(hy->bandwidth, true, diagnostics) || !ValidateHopping(hy->hopping, false, diagnostics)) return false;
        if (hy->streamReceiveWindow == 0 || hy->streamReceiveWindow > 2147483647U ||
            hy->connectionReceiveWindow == 0 || hy->connectionReceiveWindow > 2147483647U)
            return diagnostics.ValueError("recv-window/recv-window-conn", "必须为有效非零窗口");
        if (const auto* bytes = std::get_if<HysteriaBytesAuth>(&hy->auth)) {
            const auto size = HysteriaBase64Size(bytes->base64);
            if (!size || *size > 65535) return diagnostics.ValueError("auth", "必须为标准 Base64，解码后不能超过 65535 字节");
        }
        if (const auto* text = std::get_if<HysteriaTextAuth>(&hy->auth); text && text->text.size() > 65535)
            return diagnostics.ValueError("auth-str", "不能超过 65535 字节");
    } else if (const auto* hy = std::get_if<Hysteria2Options>(&node.options)) {
        if (!ValidateBandwidth(hy->bandwidth, false, diagnostics) || !ValidateHopping(hy->hopping, true, diagnostics)) return false;
        if (const auto* obfs = std::get_if<SalamanderObfs>(&hy->obfs); obfs && obfs->password.empty())
            return diagnostics.ValueError("obfs-password", "启用混淆时不能为空");
        if (const auto* obfs = std::get_if<GeckoObfs>(&hy->obfs)) {
            const auto min = obfs->minimumPacketSize.value_or(0), max = obfs->maximumPacketSize.value_or(0);
            if (obfs->password.empty()) return diagnostics.ValueError("obfs-password", "启用混淆时不能为空");
            if (min > 2048 || max > 2048 || (min == 0 ? 512 : min) > (max == 0 ? 1200 : max))
                return diagnostics.ValueError("obfs-min/max-packet-size", "有效最小值不能大于最大值或超过 2048");
        }
    } else if (const auto* any = std::get_if<AnyTlsOptions>(&node.options)) {
        if (any->password.empty()) return diagnostics.ValueError("password", "不能为空");
        for (const auto& value : {any->idleSessionCheckSeconds, any->idleSessionTimeoutSeconds, any->minimumIdleSessions})
            if (value && *value < 0) return diagnostics.ValueError("idle-session", "必须为非负整数");
    } else if (const auto* snell = std::get_if<Snell4Options>(&node.options)) {
        if (snell->psk.empty()) return diagnostics.ValueError("psk", "不能为空");
        if (snell->obfs && (snell->obfs->host.empty() ||
            (snell->obfs->mode != SnellObfsOptions::Mode::Http && snell->obfs->mode != SnellObfsOptions::Mode::Tls)))
            return diagnostics.ValueError("obfs-opts", "必须为有效模式与非空 host");
    } else if (const auto* ssh = std::get_if<SshOptions>(&node.options)) {
        if (ssh->user.empty()) return diagnostics.ValueError("username", "必须显式指定非空用户名");
        if (ssh->privateKey && (ssh->privateKey->pem.find("-----BEGIN ") == std::string::npos ||
                ssh->privateKey->pem.find("PRIVATE KEY-----") == std::string::npos))
            return diagnostics.ValueError("private-key", "仅支持内联 PEM 私钥，文件路径尚未接入");
        for (const auto* values : {&ssh->hostKeys, &ssh->hostKeyAlgorithms})
            if (*values && std::ranges::any_of(**values, [](const auto& value) { return value.empty(); }))
                return diagnostics.ValueError("host-key/host-key-algorithms", "成员不能为空");
        if (ssh->hostKeyAlgorithms) {
            if (ssh->hostKeyAlgorithms->empty()) return diagnostics.ValueError("host-key-algorithms", "显式空算法列表尚未支持");
            for (std::size_t i = 0; i < ssh->hostKeyAlgorithms->size(); ++i)
                if (!SshHostAlgorithmSupported((*ssh->hostKeyAlgorithms)[i]))
                    return diagnostics.ValueError(std::format("host-key-algorithms[{}]", i), "不是固定内核支持的主机公钥算法");
        }
    }
    return true;
}

void NoteAnyTlsUdp(const NodeSpec& node, Diagnostics& diagnostics) {
    if (node.udp != false) return;
    // Staged parsing calls this before dial options; direct models use it during validation.
    if (std::ranges::any_of(diagnostics.entries, [](const auto& value) {
            return value.field == "udp" && value.note.level == Fidelity::Approx;
        })) return;
    diagnostics.Note(Fidelity::Approx, "udp",
        std::format("AnyTLS 节点「{}」未启用 UDP，但 sing-box AnyTLS 出站固定支持 TCP/UDP", node.origin.name),
        "sing-box 没有该出站的 network 限制；如需禁止 UDP，请在路由中显式拒绝");
}

std::optional<ValidatedNode> ValidateNode(NodeSpec node, const TargetCapabilities& target, Diagnostics& diagnostics,
        const SourceDefaults& defaults = {}) {
    if (node.tls.enabled && !UsesQuicTls(node)) {
        if (node.tls.inheritFingerprint) {
            node.tls.fingerprint = defaults.fingerprint;
            node.tls.inheritFingerprint = false;
        }
        if (!ValidateFingerprint(node.tls.fingerprint, diagnostics)) return std::nullopt;
    }
    if (node.server.empty() || (node.port && *node.port == 0) || (!node.port && !HasHysteriaHopping(node))) {
        diagnostics.ValueError("server/port", "必须为有效的服务器和端口");
        return std::nullopt;
    }
    if (!ValidateTransport(node, diagnostics) || !ValidateTls(node, diagnostics)) return std::nullopt;
    if (const auto* ss = std::get_if<ShadowsocksOptions>(&node.options); ss && !ValidateShadowsocks(*ss, diagnostics))
        return std::nullopt;
    if (!ValidateRemainingProtocol(node, diagnostics)) return std::nullopt;
    const bool anytls = std::holds_alternative<AnyTlsOptions>(node.options);
    if (anytls) NoteAnyTlsUdp(node, diagnostics);
    if (node.dial.detour && node.dial.detour->empty()) {
        diagnostics.DialError("dialer-proxy", "必须为非空出站名");
        return std::nullopt;
    }
    if (!ValidateInterface(node.dial, target, diagnostics) ||
        !ValidateRoutingMark(node.dial, target, diagnostics)) return std::nullopt;
    const bool active = (node.dial.interfaceName && !node.dial.interfaceName->empty()) ||
        node.dial.routingMark.value_or(0) != 0 || node.dial.tcpFastOpen.value_or(false) ||
        node.dial.multiPathTcp.value_or(false);
    if (node.dial.detour && active)
        diagnostics.Note(Fidelity::Approx, "dialer-proxy",
            std::format("节点「{}」使用 dialer-proxy，内核会忽略本节点的接口、路由标记及 TCP 拨号选项", node.origin.name),
            "物理网络选项需要放到代理链最终拨号的出站；不会自动迁移或继承这些字段");
    else if (node.dial.multiPathTcp.value_or(false))
        diagnostics.Note(Fidelity::Approx, "mptcp",
            std::format("节点「{}」已启用原生 TCP Multi Path；1.14.2 普通 IPv6 拨号路径未启用此选项", node.origin.name),
            "实际多路径还依赖操作系统与对端；不能承诺所有地址族都启用 MPTCP");
    const bool http = std::holds_alternative<HttpOptions>(node.options);
    const bool ssh = std::holds_alternative<SshOptions>(node.options);
    if ((http || ssh) && node.udp.value_or(false))
        diagnostics.Note(Fidelity::Approx, "udp",
            std::format("{} 节点「{}」启用了 UDP，但原生出站仅转发 TCP", ssh ? "ssh" : "http", node.origin.name));
    const bool tcpOnly = !http && !ssh && !anytls && node.udp && !*node.udp;
    return ValidatedNode(std::move(node), tcpOnly);
}

} // namespace protocol
