// Exercise the production model without YAML, Context, or host-platform macros.
namespace protocol_model_tests {
using singbox::Fidelity;
using singbox::FidelityNote;
using singbox::FidelityScope;
#include "../src/converter/protocol/model.inc"
#include "../src/converter/protocol/validate.inc"
#include "../src/converter/protocol/singbox.inc"

#include "protocol_model_p4_cases.inc"

void Run() {
    RunP4();
    {
        protocol::NodeSpec uri;
        uri.origin = {"p5-source", "inherited"}; uri.server = "example.test"; uri.port = 443;
        uri.options = protocol::VmessOptions{"11111111-2222-3333-4444-555555555555"};
        uri.tls.enabled = true; uri.tls.inheritFingerprint = true;
        for (const auto* fingerprint : {"firefox", "random", "chrome_psk"}) {
            protocol::Diagnostics notes{uri.origin, {}};
            const auto validated = protocol::ValidateNode(uri, {true, true}, notes, {fingerprint});
            check(bool(validated) && notes.entries.size() == (std::string_view(fingerprint) == "firefox" ? 0U : 1U),
                "P5 model resolves source fingerprints and records approximate distributions once");
            if (validated) check(protocol::EmitSingBox(*validated)["tls"]["utls"]["fingerprint"] == fingerprint &&
                !validated->Spec().tls.inheritFingerprint, "validated nodes retain the resolved source value");
        }
        uri.tls.inheritFingerprint = false; uri.tls.fingerprint = "chrome_psk";
        protocol::Diagnostics staged{uri.origin, {}};
        check(protocol::ValidateFingerprint(uri.tls.fingerprint, staged) &&
            bool(protocol::ValidateNode(uri, {true, true}, staged, {"firefox"})) && staged.entries.size() == 1,
            "staged Clash and final semantic validation share one fingerprint note");
        uri.tls.fingerprint = "unsupported";
        check(!protocol::ValidateNode(uri, {true, true}, staged, {"firefox"}) && staged.entries.size() == 2 &&
            staged.entries.back().note.level == singbox::Fidelity::Unsupported,
            "an earlier approximate note cannot hide a later unsupported fingerprint");
        uri.options = protocol::Hysteria2Options{}; uri.tls.fingerprint.clear();
        protocol::Diagnostics quic{uri.origin, {}};
        const auto validated = protocol::ValidateNode(uri, {true, true}, quic, {"random"});
        check(bool(validated) && quic.entries.empty() && !protocol::EmitSingBox(*validated)["tls"].contains("utls"),
            "QUIC ignores source TCP fingerprints without adding a TCP approximation");
        uri.tls.inheritFingerprint = true;
        protocol::Diagnostics invalidQuic{uri.origin, {}};
        check(!protocol::ValidateNode(uri, {true, true}, invalidQuic, {"firefox"}),
            "a QUIC model cannot request inherited TCP uTLS");
    }
    static_assert(!std::is_constructible_v<protocol::ValidatedNode, protocol::NodeSpec, bool>);
    protocol::NodeSpec input;
    input.origin = {"source-model", "model-node"};
    input.server = "example.test";
    input.port = 443;
    input.options = protocol::HttpOptions{{std::string{}, std::string{" exact secret "}}};
    input.dial.tcpFastOpen = false;
    input.dial.multiPathTcp = false;
    input.dial.routingMark = 0;
    for (const auto target : {protocol::TargetCapabilities{true, true}, {true, false}, {false, false}}) {
        protocol::Diagnostics diagnostics{input.origin, {}};
        auto result = protocol::ValidateNode(input, target, diagnostics);
        check(bool(result) && diagnostics.entries.empty(), "typed model accepts zero/false dial options on every target");
        if (result) {
            const auto output = protocol::EmitSingBox(*result);
            check(output["username"] == "" && output["password"] == " exact secret " &&
                output["routing_mark"] == 0 && output["tcp_fast_open"] == false &&
                output["tcp_multi_path"] == false, "typed emission preserves optional empty/zero/false values without YAML");
        }
        auto bound = input;
        bound.dial.interfaceName = "interface-test";
        protocol::Diagnostics interfaceNotes{input.origin, {}};
        check(bool(protocol::ValidateNode(bound, target, interfaceNotes)) == target.bindInterface,
            "typed model uses target interface capability instead of the build host");
        auto marked = input;
        marked.dial.routingMark = 9;
        protocol::Diagnostics markNotes{input.origin, {}};
        check(bool(protocol::ValidateNode(marked, target, markNotes)) == target.routingMark,
            "typed model limits nonzero marks to the Linux desktop capability");
        bound.dial.routingMark = 9;
        bound.dial.detour = "next-node";
        protocol::Diagnostics chained{input.origin, {}};
        auto detour = protocol::ValidateNode(bound, target, chained);
        check(bool(detour) && chained.entries.size() == 1 &&
            chained.entries[0].note.level == singbox::Fidelity::Approx &&
            chained.entries[0].note.sourceId == "source-model" && chained.entries[0].field == "dialer-proxy",
            "detour retains physical options and reports one source-aware approximation on every target");
        if (detour) {
            const auto output = protocol::EmitSingBox(*detour);
            check(output["detour"] == "next-node" && output["bind_interface"] == "interface-test" &&
                output["routing_mark"] == 9, "typed detour emission does not silently drop physical options");
        }
    }
    auto socks = input;
    socks.options = protocol::Socks5Options{{std::nullopt, std::string{" exact secret "}}};
    for (const auto udp : {std::optional<bool>{}, {false}, {true}}) {
        socks.udp = udp;
        protocol::Diagnostics diagnostics{socks.origin, {}};
        const auto result = protocol::ValidateNode(socks, {true, true}, diagnostics);
        check(bool(result), "SOCKS typed UDP intent validates independently of input syntax");
        if (result) {
            const auto output = protocol::EmitSingBox(*result);
            check(output.contains("network") == (udp && !*udp) && !output.contains("username"),
                "SOCKS UDP absence/false/true and absent username remain distinct");
        }
    }
    socks.tls.enabled = true;
    protocol::Diagnostics rejected{socks.origin, {}};
    check(!protocol::ValidateNode(socks, {true, true}, rejected) && rejected.entries.size() == 1,
        "unvalidated SOCKS TLS cannot reach native emission");
    auto emptyServer = input;
    emptyServer.server.clear();
    protocol::Diagnostics addressNotes{input.origin, {}};
    check(!protocol::ValidateNode(emptyServer, {true, true}, addressNotes),
        "typed node still validates its own address without the Clash gate");
    auto secure = input;
    secure.tls.enabled = true;
    secure.tls.fingerprint = "firefox";
    secure.tls.alpn = std::vector<std::string>{"h2", "http/1.1"};
    secure.tls.reality = protocol::RealityOptions{"fixture-key", std::string{}};
    protocol::Diagnostics secureNotes{input.origin, {}};
    const auto accepted = protocol::ValidateNode(secure, {true, true}, secureNotes);
    check(bool(accepted), "typed TLS can be constructed without a Clash adapter");
    if (accepted) {
        const auto output = protocol::EmitSingBox(*accepted);
        check(output["tls"]["alpn"] == json::array({"h2", "http/1.1"}) &&
            output["tls"]["reality"]["short_id"] == "", "typed TLS preserves ALPN order and an explicit empty short ID");
    }
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto bad = secure;
        if (invalid == 0) bad.tls.alpn = std::vector<std::string>{"h2", ""};
        if (invalid == 1) bad.tls.fingerprint = "unsupported";
        if (invalid == 2) bad.tls.reality->publicKey.clear();
        if (invalid == 3) bad.tls.declaredEnabled = false;
        if (invalid == 4) bad.tls.enabled = false;
        protocol::Diagnostics notes{input.origin, {}};
        check(!protocol::ValidateNode(bad, {true, true}, notes) && !notes.entries.empty(),
            "typed validation rejects malformed TLS independently of YAML parsing");
    }
    auto vmess = input;
    protocol::VmessOptions vmessOptions;
    vmessOptions.uuid = "11111111-2222-3333-4444-555555555555";
    vmessOptions.alterId = 0;
    vmessOptions.packetEncoding = "";
    vmessOptions.globalPadding = false;
    vmessOptions.authenticatedLength = false;
    vmess.options = vmessOptions;
    vmess.transport = protocol::WebSocketTransport{std::string{},
        std::map<std::string, std::string>{{"Host", " exact host "}}, 0, std::string{}};
    protocol::Diagnostics vmessNotes{vmess.origin, {}};
    const auto vmessResult = protocol::ValidateNode(vmess, {false, false}, vmessNotes);
    check(bool(vmessResult), "VMess is constructible directly without Clash or URI parsing");
    if (vmessResult) {
        const auto output = protocol::EmitSingBox(*vmessResult);
        check(output["type"] == "vmess" && output["alter_id"] == 0 && output["packet_encoding"] == "" &&
            output["global_padding"] == false && output["authenticated_length"] == false &&
            output["transport"]["path"] == "" && output["transport"]["max_early_data"] == 0 &&
            output["transport"]["early_data_header_name"] == "" && output["transport"]["headers"]["Host"] == " exact host ",
            "VMess model retains false, zero, empty transport values and exact header bytes");
    }
    auto vless = vmess;
    vless.options = protocol::VlessOptions{vmessOptions.uuid, std::string{"xtls-rprx-vision"}, std::string{"xudp"}};
    vless.transport = protocol::HttpTransport{std::string{"/%2F?a=1"}, std::vector<std::string>{"b.test", "a.test"}};
    protocol::Diagnostics vlessNotes{vless.origin, {}};
    const auto vlessResult = protocol::ValidateNode(vless, {false, false}, vlessNotes);
    check(bool(vlessResult), "VLESS accepts a transport through the neutral model");
    if (vlessResult) {
        const auto output = protocol::EmitSingBox(*vlessResult);
        check(output["type"] == "vless" && output["flow"] == "xtls-rprx-vision" &&
            output["packet_encoding"] == "xudp" && output["transport"]["path"] == "/%2F?a=1" &&
            output["transport"]["host"] == json::array({"b.test", "a.test"}) && !output.contains("alter_id"),
            "VLESS preserves host order and path bytes with its own protocol fields");
    }
    auto trojan = secure;
    trojan.options = protocol::TrojanOptions{std::string{" exact trojan secret "}};
    trojan.transport = protocol::GrpcTransport{std::string{" exact service "}};
    protocol::Diagnostics trojanNotes{trojan.origin, {}};
    const auto trojanResult = protocol::ValidateNode(trojan, {false, false}, trojanNotes);
    check(bool(trojanResult), "Trojan accepts required TLS and a neutral gRPC transport");
    if (trojanResult) {
        const auto output = protocol::EmitSingBox(*trojanResult);
        check(output["type"] == "trojan" && output["password"] == " exact trojan secret " &&
            output["transport"]["service_name"] == " exact service " && !output.contains("uuid"),
            "Trojan credentials and service names retain exact bytes");
    }
    trojan.tls = {};
    protocol::Diagnostics trojanPlain{trojan.origin, {}};
    check(!protocol::ValidateNode(trojan, {true, true}, trojanPlain), "typed Trojan cannot disable required TLS");
    auto invalidTransport = input;
    invalidTransport.transport = vmess.transport;
    protocol::Diagnostics invalidTransportNotes{input.origin, {}};
    check(!protocol::ValidateNode(invalidTransport, {true, true}, invalidTransportNotes),
        "typed HTTP cannot emit an unsupported V2Ray transport");
    std::get<protocol::WebSocketTransport>(vmess.transport).maxEarlyData = -1;
    protocol::Diagnostics invalidEarly{vmess.origin, {}};
    check(!protocol::ValidateNode(vmess, {true, true}, invalidEarly), "typed WebSocket rejects negative early data");

    auto ss = input;
    protocol::ShadowsocksOptions ssOptions;
    ssOptions.method = "aes-256-gcm";
    ssOptions.password = " exact SS secret +%40;=\\tail 😀 ";
    ss.options = ssOptions;
    protocol::Diagnostics plainSsNotes{ss.origin, {}};
    const auto plainSs = protocol::ValidateNode(ss, {true, true}, plainSsNotes);
    check(bool(plainSs) && plainSsNotes.entries.empty(), "plain Shadowsocks constructs without a format adapter");
    if (plainSs) {
        const auto output = protocol::EmitSingBox(*plainSs);
        check(output["type"] == "shadowsocks" && output["method"] == "aes-256-gcm" &&
            output["password"] == *ssOptions.password && !output.contains("plugin") &&
            !output.contains("udp_over_tcp") && !output.contains("tls"),
            "SS credentials retain exact bytes and plain nodes do not acquire plugin, UOT or TLS defaults");
    }
    for (const auto password : {std::optional<std::string>{}, {""}}) {
        auto optional = ss;
        std::get<protocol::ShadowsocksOptions>(optional.options).password = password;
        protocol::Diagnostics notes{optional.origin, {}};
        const auto validated = protocol::ValidateNode(optional, {true, true}, notes);
        check(bool(validated), "SS preserves the preexisting optional credential boundary before core validation");
        if (validated) {
            const auto output = protocol::EmitSingBox(*validated);
            check(output.contains("password") == password.has_value() && (!password || output["password"] == ""),
                "SS absent and explicit empty passwords remain distinct");
        }
    }
    for (const auto mode : {protocol::SimpleObfsMode::Http, protocol::SimpleObfsMode::Tls}) {
        auto obfs = ss;
        std::get<protocol::ShadowsocksOptions>(obfs.options).plugin =
            protocol::SimpleObfsPlugin{mode, " edge.test;tls=1\\suffix "};
        protocol::Diagnostics notes{obfs.origin, {}};
        const auto validated = protocol::ValidateNode(obfs, {true, true}, notes);
        check(bool(validated), "typed simple-obfs accepts its exact host bytes");
        if (validated) {
            const auto output = protocol::EmitSingBox(*validated);
            const auto prefix = mode == protocol::SimpleObfsMode::Http ? "obfs=http" : "obfs=tls";
            check(output["plugin"] == "obfs-local" && output["plugin_opts"] ==
                std::string(prefix) + ";obfs-host= edge.test\\;tls\\=1\\\\suffix " && !output.contains("tls"),
                "simple-obfs escapes once and TLS disguise does not enable node certificate TLS");
        }
    }
    for (const bool tls : {false, true}) {
        for (const bool mux : {false, true}) {
            auto ws = ss;
            auto& options = std::get<protocol::ShadowsocksOptions>(ws.options);
            options.plugin = protocol::V2RayWebSocketPlugin{" edge.test ", "/%2F?a=+;k=v\\tail", tls, mux};
            options.udpOverTcp = protocol::UdpOverTcpOptions{false, 2};
            protocol::Diagnostics notes{ws.origin, {}};
            const auto validated = protocol::ValidateNode(ws, {false, false}, notes);
            check(bool(validated), "typed SS plugin and disabled versioned UOT validate on mobile capabilities");
            if (validated) {
                const auto output = protocol::EmitSingBox(*validated);
                check(output["plugin"] == "v2ray-plugin" && output["plugin_opts"] ==
                    std::string("mode=websocket;host= edge.test ;path=/%2F?a\\=+\\;k\\=v\\\\tail;mux=") +
                        (mux ? "1" : "0") + (tls ? ";tls" : "") &&
                    output["udp_over_tcp"] == json{{"enabled", false}, {"version", 2}},
                    "SS SIP003 output retains percent/plus bytes, escapes delimiters once and only emits enabled TLS keys");
            }
        }
    }
    for (int invalid = 0; invalid < 6; ++invalid) {
        auto bad = ss;
        auto& options = std::get<protocol::ShadowsocksOptions>(bad.options);
        if (invalid == 0) options.plugin = protocol::SimpleObfsPlugin{protocol::SimpleObfsMode::Http, ""};
        if (invalid == 1) options.plugin = protocol::SimpleObfsPlugin{static_cast<protocol::SimpleObfsMode>(9), "edge.test"};
        if (invalid == 2) options.plugin = protocol::V2RayWebSocketPlugin{""};
        if (invalid == 3) options.udpOverTcp = protocol::UdpOverTcpOptions{false, 0};
        if (invalid == 4) bad.tls = secure.tls;
        if (invalid == 5) bad.transport = protocol::GrpcTransport{};
        protocol::Diagnostics notes{bad.origin, {}};
        check(!protocol::ValidateNode(bad, {true, true}, notes) && !notes.entries.empty(),
            "invalid SS plugin/UOT/outer TLS/transport cannot reach native emission through the model");
    }
}
} // namespace protocol_model_tests
