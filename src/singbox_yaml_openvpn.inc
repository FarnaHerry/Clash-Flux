// singbox_yaml_openvpn.inc — YAML 基础取值与 OpenVPN endpoint 编译
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

// ---- YAML 取值助手（缺键/类型不符一律安全回落）----------------------------

std::string trimCopy(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return s;
}

std::string ytext(const YAML::Node& map, const char* key) {
    if (!map) return {};
    const YAML::Node value = map[key];
    if (!value || !value.IsScalar()) return {};
    return trimCopy(value.Scalar());
}

std::optional<int> yint(const YAML::Node& map, const char* key) {
    const std::string text = ytext(map, key);
    if (text.empty()) return std::nullopt;
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return parsed;
}

bool ybool(const YAML::Node& map, const char* key) {
    const std::string text = lowerCopy(ytext(map, key));
    return text == "true" || text == "yes" || text == "1";
}

std::vector<std::string> ylist(const YAML::Node& map, const char* key) {
    std::vector<std::string> out;
    if (!map) return out;
    const YAML::Node value = map[key];
    if (!value || !value.IsScalar()) {
        if (value && value.IsSequence()) {
            for (const YAML::Node& item : value) {
                if (item.IsScalar()) out.push_back(trimCopy(item.Scalar()));
            }
        }
        return out;
    }
    out.push_back(trimCopy(value.Scalar()));
    return out;
}

// The profile editor deliberately keeps the original .ovpn text so users can
// re-export it.  sing-box consumes structured JSON instead, therefore the
// boundary compiler understands the portable OpenVPN directives here and
// rejects file/script directives instead of silently changing their meaning.
struct OpenVpnEndpointRemote {
    std::string host;
    int port = 1194;
    std::string network = "udp";
};

struct OpenVpnEndpointInput {
    std::vector<OpenVpnEndpointRemote> remotes;
    std::map<std::string, std::string> options;
    std::map<std::string, std::string> inlineBlocks;
    std::string network = "udp";
};

std::vector<std::string> openVpnWords(std::string_view input) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < input.size()) {
        while (index < input.size() &&
               std::isspace(static_cast<unsigned char>(input[index]))) ++index;
        if (index == input.size()) break;
        std::string word;
        if (input[index] == '\'' || input[index] == '"') {
            const char quote = input[index++];
            while (index < input.size() && input[index] != quote) {
                word.push_back(input[index++]);
            }
            if (index < input.size()) ++index;
        } else {
            while (index < input.size() &&
                   !std::isspace(static_cast<unsigned char>(input[index]))) {
                word.push_back(input[index++]);
            }
        }
        if (!word.empty()) words.push_back(std::move(word));
    }
    return words;
}

std::string openVpnNetwork(std::string value) {
    value = lowerCopy(trimCopy(value));
    if (value.starts_with("tcp")) return "tcp";
    return "udp";
}

std::optional<OpenVpnEndpointInput> parseOpenVpnEndpointInput(
    std::string_view text, std::string& error) {
    OpenVpnEndpointInput input;
    std::istringstream stream{std::string(text)};
    std::string line;
    std::string activeBlock;
    std::string blockContent;
    while (std::getline(stream, line)) {
        const std::string value = trimCopy(line);
        if (!activeBlock.empty()) {
            if (value == "</" + activeBlock + ">") {
                input.inlineBlocks[activeBlock] = std::move(blockContent);
                blockContent.clear();
                activeBlock.clear();
            } else {
                blockContent += line;
                blockContent.push_back('\n');
            }
            continue;
        }
        if (value.empty() || value.starts_with('#') || value.starts_with(';')) continue;
        if (value.starts_with('<') && value.ends_with('>')) {
            if (value == "<connection>" || value == "</connection>") continue;
            if (value.starts_with("</")) {
                error = "OpenVPN inline 配置块不匹配";
                return std::nullopt;
            }
            activeBlock = value.substr(1, value.size() - 2);
            blockContent.clear();
            continue;
        }

        auto words = openVpnWords(value);
        if (words.empty()) continue;
        std::string key = words.front();
        if (key.starts_with("--")) key.erase(0, 2);
        const std::size_t firstArgument = value.find_first_of(" \t");
        const std::string rest = firstArgument == std::string::npos
            ? std::string{}
            : trimCopy(std::string_view(value).substr(firstArgument));
        if (key == "remote") {
            if (words.size() < 2) {
                error = "OpenVPN remote 缺少服务器地址";
                return std::nullopt;
            }
            OpenVpnEndpointRemote remote{.host = words[1],
                                         .network = input.network};
            if (remote.host.size() >= 2 && remote.host.front() == '[' &&
                remote.host.back() == ']') {
                remote.host = remote.host.substr(1, remote.host.size() - 2);
            }
            if (words.size() >= 3) {
                int port = 0;
                const auto [ptr, ec] = std::from_chars(
                    words[2].data(), words[2].data() + words[2].size(), port);
                if (ec != std::errc() || ptr != words[2].data() + words[2].size() ||
                    port < 1 || port > 65535) {
                    error = "OpenVPN remote 端口无效";
                    return std::nullopt;
                }
                remote.port = port;
            }
            if (words.size() >= 4) remote.network = openVpnNetwork(words[3]);
            input.remotes.push_back(std::move(remote));
        } else if (key == "proto") {
            input.network = openVpnNetwork(words.size() >= 2 ? words[1] : "udp");
            input.options[key] = rest;
            for (auto& remote : input.remotes) {
                if (remote.network == "udp") remote.network = input.network;
            }
        } else {
            input.options[key] = rest;
        }
    }
    if (!activeBlock.empty()) {
        error = "OpenVPN inline 配置块未结束";
        return std::nullopt;
    }
    if (input.remotes.empty()) {
        error = "OpenVPN 配置缺少 remote 服务器";
        return std::nullopt;
    }
    return input;
}

std::vector<std::string> splitOpenVpnList(std::string value) {
    for (char& character : value) {
        if (character == ':') character = ',';
    }
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(',', begin);
        const std::string item = trimCopy(std::string_view(value).substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!item.empty()) result.push_back(item);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string openVpnDuration(std::string value) {
    value = trimCopy(value);
    if (value.empty()) return {};
    if (std::ranges::all_of(value, [](unsigned char c) { return std::isdigit(c); })) {
        return value + "s";
    }
    return value;
}

std::optional<nlohmann::json> compileOpenVpnEndpoint(
    const singbox::NativeConnection& native, std::string tag, std::string& error) {
    const auto parsed = parseOpenVpnEndpointInput(native.nativeConfig, error);
    if (!parsed) return std::nullopt;
    const auto& input = *parsed;
    nlohmann::json endpoint = {
        {"type", "openvpn-client"}, {"tag", std::move(tag)},
        {"network", input.network}, {"system", false},
    };
    if (input.remotes.size() == 1) {
        endpoint["server"] = input.remotes.front().host;
        endpoint["server_port"] = input.remotes.front().port;
        endpoint["network"] = input.remotes.front().network;
    } else {
        endpoint["servers"] = nlohmann::json::array();
        for (const auto& remote : input.remotes) {
            endpoint["servers"].push_back({
                {"server", remote.host}, {"server_port", remote.port},
                {"network", remote.network}});
        }
    }
    if (input.options.contains("remote-random")) endpoint["remote_random"] = true;

    const auto block = [&](std::string_view name) -> std::string {
        if (const auto it = input.inlineBlocks.find(std::string(name));
            it != input.inlineBlocks.end()) return it->second;
        return {};
    };
    const auto option = [&](std::string_view name) -> std::string {
        if (const auto it = input.options.find(std::string(name));
            it != input.options.end()) return it->second;
        return {};
    };
    if (!option("ca").empty() && block("ca").empty()) {
        error = "OpenVPN 外部 ca 文件路径不能交给 sing-box，请使用 inline <ca> 配置";
        return std::nullopt;
    }
    if (!option("cert").empty() && block("cert").empty()) {
        error = "OpenVPN 外部 cert 文件路径不能交给 sing-box，请使用 inline <cert> 配置";
        return std::nullopt;
    }
    if (!option("key").empty() && block("key").empty()) {
        error = "OpenVPN 外部 key 文件路径不能交给 sing-box，请使用 inline <key> 配置";
        return std::nullopt;
    }
    for (const std::string_view directive : {"tls-auth", "tls-crypt", "tls-crypt-v2"}) {
        if (!option(directive).empty() && block(directive).empty()) {
            error = std::format("OpenVPN 外部 {} 文件路径不能交给 sing-box，请使用 inline 配置",
                                directive);
            return std::nullopt;
        }
    }
    if (!option("secret").empty() && block("secret").empty()) {
        error = "OpenVPN 外部 secret 文件路径不能交给 sing-box，请使用 inline <secret> 配置";
        return std::nullopt;
    }

    const std::string staticKey = block("secret");
    if (!staticKey.empty()) {
        endpoint["mode"] = "static_key";
        endpoint["static_key"] = nlohmann::json::array({staticKey});
        if (!option("key-direction").empty()) endpoint["key_direction"] = option("key-direction");
    } else {
        endpoint["mode"] = "tls";
        nlohmann::json tls = nlohmann::json::object();
        const std::string ca = block("ca");
        if (!ca.empty()) tls["certificate"] = nlohmann::json::array({ca});
        const std::string cert = block("cert");
        const std::string key = block("key");
        if (cert.empty() != key.empty()) {
            error = "OpenVPN inline <cert> 与 <key> 必须同时存在";
            return std::nullopt;
        }
        if (!cert.empty()) {
            tls["client_certificate"] = nlohmann::json::array({cert});
            tls["client_key"] = nlohmann::json::array({key});
        }
        const std::string fingerprint = option("peer-fingerprint");
        if (!fingerprint.empty()) tls["peer_fingerprint"] = splitOpenVpnList(fingerprint);
        if (const std::string remoteTls = option("remote-cert-tls"); !remoteTls.empty()) {
            tls["remote_certificate_tls"] = remoteTls == "client" ? "client" : "server";
        }
        if (const auto verify = openVpnWords(option("verify-x509-name")); !verify.empty()) {
            tls["server_name"] = verify.front();
            if (verify.size() >= 2) tls["server_name_type"] = verify[1];
        }
        if (!option("tls-version-min").empty()) tls["version_min"] = option("tls-version-min");
        if (!option("tls-version-max").empty()) tls["version_max"] = option("tls-version-max");
        if (!option("tls-cipher").empty()) tls["cipher"] = option("tls-cipher");
        const std::string tlsAuth = block("tls-auth");
        const std::string tlsCrypt = block("tls-crypt");
        const std::string tlsCryptV2 = block("tls-crypt-v2");
        if (!tlsAuth.empty() || !tlsCrypt.empty() || !tlsCryptV2.empty()) {
            nlohmann::json control;
            control["type"] = !tlsAuth.empty() ? "tls_auth"
                : !tlsCrypt.empty() ? "tls_crypt" : "tls_crypt_v2";
            const std::string keyContent = !tlsAuth.empty() ? tlsAuth
                : !tlsCrypt.empty() ? tlsCrypt : tlsCryptV2;
            control["key"] = nlohmann::json::array({keyContent});
            if (!option("key-direction").empty()) control["direction"] = option("key-direction");
            tls["control_wrap"] = std::move(control);
        }
        if (tls.empty() && input.inlineBlocks.contains("ca") == false &&
            option("peer-fingerprint").empty()) {
            error = "OpenVPN TLS 配置缺少 inline <ca> 或 peer-fingerprint";
            return std::nullopt;
        }
        endpoint["tls"] = std::move(tls);
        if (!option("key-direction").empty()) endpoint["key_direction"] = option("key-direction");
        if (const std::string authUserPass = block("auth-user-pass"); !authUserPass.empty()) {
            const auto credentials = openVpnWords(authUserPass);
            if (credentials.size() >= 2) {
                endpoint["username"] = credentials[0];
                endpoint["password"] = credentials[1];
            } else {
                std::vector<std::string> values;
                std::istringstream credentialLines(authUserPass);
                std::string credentialLine;
                while (std::getline(credentialLines, credentialLine)) {
                    values.push_back(trimCopy(credentialLine));
                }
                if (values.size() >= 2) {
                    endpoint["username"] = values[0];
                    endpoint["password"] = values[1];
                }
            }
        }
        if (!option("auth-user-pass").empty() && block("auth-user-pass").empty()) {
            error = "OpenVPN 外部 auth-user-pass 文件不能交给 sing-box，请使用 inline <auth-user-pass> 配置";
            return std::nullopt;
        }
    }
    if (!option("data-ciphers").empty()) endpoint["data_ciphers"] = splitOpenVpnList(option("data-ciphers"));
    if (!option("data-ciphers-fallback").empty()) endpoint["data_ciphers_fallback"] = option("data-ciphers-fallback");
    if (!option("auth").empty()) endpoint["auth"] = option("auth");
    if (!option("cipher").empty() && staticKey.empty()) endpoint["data_ciphers_fallback"] = option("cipher");
    if (!option("comp-lzo").empty()) endpoint["compression_lzo"] = option("comp-lzo");
    if (!option("compress").empty()) endpoint["compression"] = openVpnWords(option("compress")).front();
    if (!option("allow-compression").empty()) endpoint["allow_compression"] = option("allow-compression");
    if (!option("topology").empty()) endpoint["topology"] = option("topology");
    if (!option("mssfix").empty()) endpoint["mss_fix"] = std::stoi(openVpnWords(option("mssfix")).front());
    if (!option("fragment").empty()) endpoint["fragment"] = std::stoi(openVpnWords(option("fragment")).front());
    if (!option("ping").empty()) endpoint["ping_interval"] = openVpnDuration(option("ping"));
    if (!option("ping-restart").empty()) endpoint["ping_restart"] = openVpnDuration(option("ping-restart"));
    if (!option("reneg-sec").empty()) endpoint["renegotiate_interval"] = openVpnDuration(option("reneg-sec"));
    if (!option("explicit-exit-notify").empty()) endpoint["explicit_exit_notify"] = std::stoi(option("explicit-exit-notify"));
    if (!native.internalRoutes.empty()) endpoint["routes"] = native.internalRoutes;
    return endpoint;
}

// ---- 编译上下文 -----------------------------------------------------------
