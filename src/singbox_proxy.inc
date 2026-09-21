// singbox_proxy.inc — 代理节点、传输层与代理组转换
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

void appendTls(Context& ctx, const YAML::Node& item, nlohmann::json& out) {
    const std::string sni = [] (const YAML::Node& node) {
        std::string value = ytext(node, "servername");
        if (value.empty()) value = ytext(node, "sni");
        return value;
    }(item);
    const bool skipVerify = ybool(item, "skip-cert-verify");
    if (!ybool(item, "tls") && sni.empty() && !skipVerify) return;
    nlohmann::json tls = {{"enabled", true}};
    if (!sni.empty()) tls["server_name"] = sni;
    if (skipVerify) tls["insecure"] = true;
    if (const std::vector<std::string> alpn = ylist(item, "alpn"); !alpn.empty()) {
        tls["alpn"] = alpn;
    }
    // mihomo 全局 uTLS 指纹的历史行为：无节点级指纹时按 chrome 处理。
    std::string fingerprint = ytext(item, "client-fingerprint");
    if (fingerprint.empty()) fingerprint = "chrome";
    tls["utls"] = {{"enabled", true}, {"fingerprint", fingerprint}};
    if (const YAML::Node reality = item["reality-opts"]; reality && reality.IsMap()) {
        nlohmann::json realityOut = {{"enabled", true}};
        if (const std::string publicKey = ytext(reality, "public-key"); !publicKey.empty()) {
            realityOut["public_key"] = publicKey;
        }
        if (const std::string shortId = ytext(reality, "short-id"); !shortId.empty()) {
            realityOut["short_id"] = shortId;
        }
        tls["reality"] = realityOut;
    }
    out["tls"] = std::move(tls);
}

// Clash 把传输层放在 network + *-opts；sing-box 用 typed transport 对象。
bool appendTransport(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                     const std::string& name) {
    const std::string network = lowerCopy(ytext(item, "network"));
    if (network.empty() || network == "tcp") return true;
    if (network == "ws") {
        // sing-box 的类型名是 "ws"（"websocket" 会直接解码失败）。
        nlohmann::json transport = {{"type", "ws"}};
        if (const YAML::Node opts = item["ws-opts"]; opts && opts.IsMap()) {
            if (const std::string path = ytext(opts, "path"); !path.empty()) {
                transport["path"] = path;
            }
            if (const YAML::Node headers = opts["headers"]; headers && headers.IsMap()) {
                nlohmann::json mapped = nlohmann::json::object();
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    const YAML::Node key = it->first;
                    const YAML::Node value = it->second;
                    if (key.IsScalar() && value.IsScalar()) {
                        mapped[key.Scalar()] = trimCopy(value.Scalar());
                    }
                }
                if (!mapped.empty()) transport["headers"] = std::move(mapped);
            }
            if (const auto earlyData = yint(opts, "max-early-data"); earlyData) {
                transport["max_early_data"] = *earlyData;
            }
            if (const std::string earlyHeader = ytext(opts, "early-data-header-name");
                !earlyHeader.empty()) {
                transport["early_data_header_name"] = earlyHeader;
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "grpc") {
        nlohmann::json transport = {{"type", "grpc"}};
        std::string service = ytext(item, "grpc-service-name");
        if (const YAML::Node opts = item["grpc-opts"]; opts && opts.IsMap()) {
            const std::string nested = ytext(opts, "grpc-service-name");
            if (!nested.empty()) service = nested;
        }
        if (!service.empty()) transport["service_name"] = service;
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "h2" || network == "http") {
        nlohmann::json transport = {{"type", "http"}};
        const YAML::Node opts = network == "h2" ? item["h2-opts"] : item["http-opts"];
        if (opts && opts.IsMap()) {
            if (const std::vector<std::string> paths = ylist(opts, "path"); !paths.empty()) {
                transport["path"] = paths.front();
            }
            if (const YAML::Node host = opts["host"]; host && host.IsSequence()) {
                std::vector<std::string> hosts;
                for (const YAML::Node& entry : host) {
                    if (entry.IsScalar()) hosts.push_back(trimCopy(entry.Scalar()));
                }
                if (!hosts.empty()) transport["host"] = std::move(hosts);
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "httpupgrade") {
        nlohmann::json transport = {{"type", "httpupgrade"}};
        if (const YAML::Node opts = item["httpupgrade-opts"]; opts && opts.IsMap()) {
            if (const std::string path = ytext(opts, "path"); !path.empty()) {
                transport["path"] = path;
            }
            if (const std::string host = ytext(opts, "host"); !host.empty()) {
                transport["host"] = host;
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    ctx.warn(std::format("节点「{}」的传输层 network:{} 暂不支持，已跳过", name, network));
    return false;
}

// ---- 节点转换 ---------------------------------------------------------------

std::optional<nlohmann::json> convertProxy(Context& ctx, const YAML::Node& item) {
    const std::string name = ytext(item, "name");
    const std::string type = lowerCopy(ytext(item, "type"));
    if (name.empty() || type.empty()) {
        ctx.warn("订阅包含无名称或类型的节点，已跳过");
        return std::nullopt;
    }
    if (ctx.tagKnown(name)) {
        ctx.warn(std::format("订阅包含重名节点「{}」，仅保留首个", name));
        return std::nullopt;
    }
    const std::string server = ytext(item, "server");
    const auto port = yint(item, "port");
    if (server.empty() || !port || *port <= 0 || *port > 65535) {
        ctx.warn(std::format("节点「{}」缺少有效的 server/port，已跳过", name));
        return std::nullopt;
    }

    nlohmann::json out = {{"tag", name}};
    std::string singboxType = type;
    if (type == "ss") singboxType = "shadowsocks";
    if (type == "hy2") singboxType = "hysteria2";
    out["type"] = singboxType;
    out["server"] = server;
    out["server_port"] = *port;

    if (singboxType == "shadowsocks") {
        out["method"] = ytext(item, "cipher");
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (!ytext(item, "plugin").empty()) {
            ctx.warn(std::format("节点「{}」使用 SS 插件（obfs/v2ray-plugin），暂不支持，已跳过", name));
            return std::nullopt;
        }
        if (ybool(item, "udp-over-tcp")) out["udp_over_tcp"] = true;
    } else if (singboxType == "vmess") {
        out["uuid"] = ytext(item, "uuid");
        std::string security = ytext(item, "cipher");
        if (security.empty()) security = "auto";
        out["security"] = lowerCopy(security);
        if (const auto alterId = yint(item, "alterId"); alterId) {
            out["alter_id"] = *alterId;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "vless") {
        out["uuid"] = ytext(item, "uuid");
        if (const std::string flow = ytext(item, "flow"); !flow.empty()) {
            out["flow"] = flow;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "trojan") {
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "hysteria2") {
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        // 带宽字段 "30 Mbps" / "30" 之类取前导数字；无效则省略（内核自适应）。
        const auto leadingNumber = [](std::string_view text) -> std::optional<int> {
            // 拷贝到稳定存储：trimCopy 返回的临时 string 赋回 string_view 会
            // 悬垂（libstdc++ 的 SSO 缓冲恰好能读到，libc++ 下即出错）。
            const std::string trimmed = trimCopy(text);
            std::string_view digits{trimmed};
            std::size_t end = 0;
            while (end < digits.size() && digits[end] >= '0' && digits[end] <= '9') {
                ++end;
            }
            if (end == 0) return std::nullopt;
            int parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(digits.data(), digits.data() + end, parsed);
            if (ec != std::errc()) return std::nullopt;
            return parsed;
        };
        if (const auto up = leadingNumber(ytext(item, "up")); up) out["up_mbps"] = *up;
        if (const auto down = leadingNumber(ytext(item, "down")); down) out["down_mbps"] = *down;
        if (const std::string obfsPassword = ytext(item, "obfs-password"); !obfsPassword.empty()) {
            out["obfs"] = {{"type", "salamander"}, {"password", obfsPassword}};
        } else if (const std::string obfs = ytext(item, "obfs");
                   !obfs.empty() && lowerCopy(obfs) != "none") {
            ctx.warn(std::format("节点「{}」的 obfs 配置缺少 obfs-password，已忽略", name));
        }
        if (const std::string ports = ytext(item, "ports"); !ports.empty()) {
            // Clash "20000-30000,40000" → sing-box server_ports ["20000:30000","40000"]。
            nlohmann::json serverPorts = nlohmann::json::array();
            std::string_view rest{ports};
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                std::string piece = trimCopy(rest.substr(0, comma));
                if (!piece.empty()) {
                    const auto dash = piece.find('-');
                    if (dash != std::string::npos) piece.replace(dash, 1, ":");
                    serverPorts.push_back(std::move(piece));
                }
                rest = comma == std::string_view::npos
                           ? std::string_view{}
                           : rest.substr(comma + 1);
            }
            if (!serverPorts.empty()) out["server_ports"] = std::move(serverPorts);
        }
        appendTls(ctx, item, out);
    } else if (singboxType == "tuic") {
        out["uuid"] = ytext(item, "uuid");
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (const std::string congestion =
                ytext(item, "congestion-controller"); !congestion.empty()) {
            out["congestion_control"] = congestion;
        }
        if (ybool(item, "reduce-rtt")) out["zero_rtt_handshake"] = true;
        if (ybool(item, "disable-sni")) out["disable_sni"] = true;
        if (const std::vector<std::string> alpn = ylist(item, "alpn"); !alpn.empty()) {
            nlohmann::json tls = {{"enabled", true}};
            if (const std::string sni = ytext(item, "sni"); !sni.empty()) {
                tls["server_name"] = sni;
            }
            if (ybool(item, "skip-cert-verify")) tls["insecure"] = true;
            tls["alpn"] = alpn;
            out["tls"] = std::move(tls);
        } else {
            appendTls(ctx, item, out);
        }
    } else if (singboxType == "http" || singboxType == "socks5") {
        if (singboxType == "socks5") out["type"] = "socks";
        if (const std::string username = ytext(item, "username"); !username.empty()) {
            out["username"] = username;
        }
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        appendTls(ctx, item, out);
    } else {
        ctx.warn(std::format("节点「{}」的协议 {} 暂不支持，已跳过", name, type));
        return std::nullopt;
    }
    return out;
}

// ---- 代理组转换 --------------------------------------------------------------

std::optional<nlohmann::json> convertGroup(Context& ctx, const YAML::Node& item,
                                           const std::string& name,
                                           const std::vector<std::string>& members) {
    const std::string clashType = lowerCopy(ytext(item, "type"));
    nlohmann::json out = {{"tag", name}};
    if (clashType == "select") {
        out["type"] = "selector";
    } else if (clashType == "url-test" || clashType == "fallback" ||
               clashType == "load-balance") {
        if (clashType != "url-test") {
            ctx.warn(std::format("代理组「{}」类型 {} 在 sing-box 无对应语义，降级为 urltest",
                                 name, clashType));
        }
        if (ctx.opt.speedTestOnly) {
            // A speed-only libbox instance performs explicit outbound tests
            // from Android.  Keeping imported URLTest groups here would make
            // sing-box start its own automatic sweep at the same time, which
            // doubles the connections and mixes its configured URL with the
            // CommandClient default URL.  Keep the group as a selector so the
            // page still exposes the same members without a second sweep.
            out["type"] = "selector";
        } else {
            out["type"] = "urltest";
            std::string url = ytext(item, "url");
            if (url.empty()) url = "https://www.gstatic.com/generate_204";
            out["url"] = url;
            if (const auto interval = yint(item, "interval"); interval && *interval > 0) {
                out["interval"] = std::format("{}s", *interval);
            }
        }
    } else {
        ctx.warn(std::format("代理组「{}」类型 {} 暂不支持，已跳过", name, clashType));
        return std::nullopt;
    }
    // 成员必须是已知的节点/组/DIRECT；REJECT 等特殊目标在 1.14 无对应 outbound，
    // 过滤并提示（sing-box 对未知 outbound tag 直接拒绝启动）。
    nlohmann::json filtered = nlohmann::json::array();
    for (const std::string& member : members) {
        if (member == "REJECT" || member == "REJECT-DROP" || member == "PASS") {
            ctx.warn(std::format("代理组「{}」的成员 {} 在 sing-box 无对应 outbound，已移除",
                                 name, member));
            continue;
        }
        if (member != "DIRECT" && !ctx.tagKnown(member)) {
            ctx.warn(std::format("代理组「{}」引用了未知的成员「{}」，已移除", name, member));
            continue;
        }
        filtered.push_back(member);
    }
    if (filtered.empty()) filtered.push_back("DIRECT");
    out["outbounds"] = std::move(filtered);
    return out;
}

// ---- 规则转换 -----------------------------------------------------------------
