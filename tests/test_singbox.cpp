// test_singbox.cpp — clashflux.singbox 编译器的最小单元测试。
//
// 覆盖：节点协议映射（ss/vmess/hysteria2/tuic/未知协议）、代理组降级与成员
// 过滤、规则直映射 + REJECT action + GEOIP/GEOSITE 规则集 + MATCH final、
// resolve 前置规则、三模式
// clash_mode 前置规则、原生 sing-box JSON 直通、空订阅最小配置。
// 断言风格与 test_vpn 一致（check 计数 + main）。
#include <cassert>
#include <cstdio>
#include <zstd.h>

import std;
import nlohmann.json;
import clashflux.singbox;
import clashflux.rule_provider_cache;
import clashflux.vpn;

namespace {

using nlohmann::json;

const std::string kFixture = R"yaml(
mixed-port: 7890
mode: rule
log-level: info
dns:
  ipv6: false
  # 节点解析独立于经代理查询的主 DNS，避免解析/代理依赖环。
  proxy-server-nameserver: [223.5.5.5]
  nameserver:
    - https://dns.example.com/dns-query#手动选择
    - 223.5.5.5
proxies:
  - name: "香港 01"
    type: ss
    server: hk.example.com
    port: 8388
    cipher: aes-256-gcm
    password: "pass1"
    udp: true
  - name: "美国 01"
    type: vmess
    server: us.example.com
    port: 443
    uuid: 11111111-2222-3333-4444-555555555555
    alterId: 0
    cipher: auto
    tls: true
    servername: us.example.com
    network: ws
    ws-opts:
      path: /vmess
      headers:
        Host: us.example.com
  - name: "日本 01"
    type: hysteria2
    server: jp.example.com
    port: 443
    password: "pass3"
    obfs: salamander
    obfs-password: "obfspass"
    up: "30 Mbps"
    down: "200 Mbps"
  - name: "德国 01"
    type: tuic
    server: de.example.com
    port: 8443
    uuid: 99999999-8888-7777-6666-555555555555
    password: "pass4"
    congestion-controller: bbr
    alpn: [h3]
  - name: "过期节点"
    type: ssr
    server: legacy.example.com
    port: 1000
    cipher: rc4-md5
    password: "x"
proxy-groups:
  - name: "自动选择"
    type: url-test
    proxies:
      - "香港 01"
      - "美国 01"
      - "日本 01"
      - REJECT
    url: https://www.gstatic.com/generate_204
    interval: 300
  - name: "手动选择"
    type: select
    proxies:
      - "自动选择"
      - DIRECT
rules:
  - DOMAIN,example.org,DIRECT
  - DOMAIN-SUFFIX,cn,GEOIPPLACEHOLDER
  - IP-CIDR,192.168.0.0/16,DIRECT,no-resolve
  - GEOSITE,CN,DIRECT
  - GEOIP,CN,手动选择
  - RULE-SET,cn,DIRECT
  - RULE-SET,cn-ip,DIRECT
  - DOMAIN,blocked.net,REJECT
  - RULE-SET,some-provider,手动选择
  - PROCESS-NAME,ssh,手动选择
  - PROCESS-PATH,/usr/bin/curl,DIRECT
  - PROCESS-PATH-REGEX,^/opt/.+,DIRECT
  - PROCESS-NAME-REGEX,^chr,DIRECT
  - MATCH,自动选择
)yaml";

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::println(stderr, "FAIL: {}", what);
        ++failures;
    }
}

singbox::CompileResult compileProxyValues(std::string_view fields,
                                         std::string_view root = {}) {
    singbox::CompileOptions options;
    options.profileYaml = std::string(root) + "\nproxies:\n  - name: values\n"
        "    server: example.test\n    port: 443\n" + std::string(fields) +
        "\nrules:\n  - MATCH,DIRECT\n";
    return singbox::compileConfig(options);
}

json valuesNode(const singbox::CompileResult& result) {
    if (result.json.empty()) return {};
    const auto config = json::parse(result.json);
    for (const auto& node : config["outbounds"])
        if (node.value("tag", "") == "values") return node;
    return {};
}

class RuleSetTestDirectory {
public:
    RuleSetTestDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                std::format("clashflux-test-ruleset-{:x}-{:x}", random(), random());
            if (!std::filesystem::create_directory(candidate)) continue;
            path = std::move(candidate);
            return;
        }
        throw std::runtime_error("cannot allocate exclusive rule-set test directory");
    }
    ~RuleSetTestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    RuleSetTestDirectory(const RuleSetTestDirectory&) = delete;
    RuleSetTestDirectory& operator=(const RuleSetTestDirectory&) = delete;

    std::filesystem::path path;
};

void testProxyValues() {
    // Subscription generators still emit these valid compatibility fields.
    // A field allowlist must distinguish inactive/duplicate settings from an
    // unsupported encrypted transport, rather than remove every such node.
    for (const char* encryption : {"none", "''"}) {
        const auto result = compileProxyValues(std::string("    type: vless\n    encryption: ") + encryption + "\n");
        const auto node = valuesNode(result);
        check(!node.empty() && !node.contains("encryption") && result.fidelity.empty(),
              "plain VLESS encryption compatibility values preserve node exactly");
    }
    for (const char* encryption : {"mlkem768x25519plus.native.1rtt.sample", "[none]", "null"}) {
        const auto result = compileProxyValues(std::string("    type: vless\n    encryption: ") + encryption + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              "unsupported VLESS encryption is not silently removed");
    }
    for (const char* type : {"vmess", "vless", "trojan"}) {
        const std::string base = std::format("    type: {}\n    network: ws\n", type);
        for (const char* fields : {
            "    ws-path: '/ exact path '\n    ws-headers: {Host: example.test, X-Test: ' exact value '}\n",
            "    ws-path: '/ exact path '\n    ws-headers: {Host: example.test, X-Test: ' exact value '}\n"
            "    ws-opts: {path: '/ exact path ', headers: {Host: example.test, X-Test: ' exact value '}}\n"}) {
            const auto result = compileProxyValues(base + fields);
            const auto node = valuesNode(result);
            check(!node.empty() && node["transport"]["path"] == "/ exact path " &&
                  node["transport"]["headers"]["Host"] == "example.test" &&
                  node["transport"]["headers"]["X-Test"] == " exact value " && result.fidelity.empty(),
                  "legacy WS fields and matching duplicate modern fields preserve node exactly");
        }
        const auto override = compileProxyValues(base +
            "    ws-path: /legacy\n    ws-headers: {Host: old.test, X-Legacy: legacy}\n"
            "    ws-opts: {path: /modern, headers: {Host: new.test}}\n");
        const auto node = valuesNode(override);
        check(!node.empty() && node["transport"]["path"] == "/modern" &&
              node["transport"]["headers"] == json{{"Host", "new.test"}} &&
              std::ranges::any_of(override.fidelity, [](const auto& note) {
                  return note.level == singbox::Fidelity::Approx;
              }), "conflicting modern WS options override legacy values with ledger");
        for (const char* fields : {"    ws-path: [/bad]\n", "    ws-headers: {Host: [bad]}\n",
                                   "    ws-headers: {Host: a, Host: b}\n",
                                   "    ws-headers: null\n"}) {
            const auto result = compileProxyValues(base + fields);
            check(valuesNode(result).empty() && !result.fidelity.empty(),
                  "malformed legacy WS values reject the whole node");
        }
    }
    // Literal credentials are opaque bytes, including whitespace and explicit
    // empty optional HTTP/SOCKS values. Never print them in fidelity messages.
    for (const auto& [type, extra] : std::vector<std::pair<std::string, std::string>>{
        {"ss", "    cipher: aes-256-gcm\n"}, {"trojan", ""},
        {"hysteria2", "    obfs: salamander\n    obfs-password: ' obfs secret '\n"},
        {"tuic", "    uuid: 11111111-2222-3333-4444-555555555555\n"},
        {"http", "    username: ' user name '\n"},
        {"socks5", "    username: ' user name '\n"}, {"anytls", "    udp: true\n"}}) {
        const auto result = compileProxyValues("    type: " + type + "\n" + extra +
            "    password: \" \\t sample \\n \"\n");
        const auto node = valuesNode(result);
        check(result.error.empty() && !node.empty() &&
              node.value("password", "") == " \t sample \n ",
              std::format("{} preserves literal credential bytes", type));
        if (type == "http" || type == "socks5")
            check(node.value("username", "") == " user name ", "username whitespace is literal");
        if (type == "hysteria2")
            check(node.value("obfs", json{}).value("password", "") == " obfs secret ",
                  "HY2 obfs credential whitespace is literal");
    }
    for (const char* type : {"http", "socks5"}) {
        const auto node = valuesNode(compileProxyValues(std::format(
            "    type: {}\n    username: ''\n    password: ''\n", type)));
        check(node.contains("username") && node["username"] == "" &&
              node.contains("password") && node["password"] == "",
              "explicit empty optional credentials are retained");
    }
    const auto snell = valuesNode(compileProxyValues(
        "    type: snell\n    version: 4\n    psk: ' psk secret '\n"));
    check(snell.value("psk", "") == " psk secret ", "Snell PSK whitespace remains literal");
    for (const char* fields : {
        "type: trojan\n    password: [sample]", "type: ss\n    cipher: aes-256-gcm\n    password: null",
        "type: http\n    username: {user: sample}", "type: socks5\n    password: [sample]",
        "type: hysteria2\n    obfs-password: {password: sample}",
        "type: anytls\n    password: [sample]", "type: snell\n    version: 4\n    psk: [sample]"}) {
        const auto result = compileProxyValues(std::string("    ") + fields + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              "malformed credentials reject the entire node and enter fidelity");
        check(std::ranges::none_of(result.fidelity, [](const auto& note) {
            return note.detail.find("sample") != std::string::npos;
        }), "fidelity does not echo credential values");
    }

    for (const char* fields : {
        "alpn: {protocol: h2}", "alpn: [h2, {protocol: http/1.1}]", "alpn: [h2, null]",
        "alpn: ['']", "alpn: null", "client-fingerprint: [firefox]",
        "client-fingerprint: unknown-browser", "client-fingerprint: ''", "sni: [example.test]",
        "reality-opts: {public-key: [sample], short-id: '12'}"}) {
        const auto result = compileProxyValues(std::string("    type: vless\n    tls: true\n    ") + fields + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              std::format("malformed TLS {} rejects whole node", fields));
    }
    for (const char* opts : {
        "{headers: {Host: [example.test]}}", "{headers: {Host: {value: example.test}}}",
        "{headers: {Host: null}}", "{headers: [example.test]}", "{path: [/a]}",
        "{max-early-data: lots}", "{max-early-data: -1}", "{early-data-header-name: [X-Test]}",
        "{headers: {X-Test: a, X-Test: b}}"}) {
        const auto result = compileProxyValues(std::string("    type: vmess\n    network: ws\n    ws-opts: ") + opts + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              "malformed WS values reject node atomically");
    }
    for (const char* opts : {"{host: [example.test, {host: other.test}]}",
                            "{path: [/a, {path: /b}]}", "{host: null}"}) {
        const auto result = compileProxyValues(std::string("    type: vmess\n    network: http\n    http-opts: ") + opts + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              "malformed HTTP transport lists cannot be partially accepted");
    }
    const auto wsResult = compileProxyValues(
        "    type: vmess\n    tls: true\n    alpn: [h2, http/1.1]\n    network: ws\n"
        "    ws-opts: {path: '/ exact path ', headers: {X-Test: ' exact value ', Host: example.test}}\n");
    const auto ws = valuesNode(wsResult);
    check(!ws.empty() && ws["tls"]["alpn"] == json::array({"h2", "http/1.1"}) &&
          ws["transport"]["path"] == "/ exact path " &&
          ws["transport"]["headers"]["X-Test"] == " exact value " && wsResult.fidelity.empty(),
          "valid ALPN, WS path and headers retain exact content and order");
    for (const char* alpn : {"h2", "[h2]", "[]"}) {
        const auto result = compileProxyValues(std::string("    type: trojan\n    alpn: ") + alpn + "\n");
        check(!valuesNode(result).empty() && result.fidelity.empty(),
              "scalar/list/empty-list ALPN remain supported");
    }
    const auto oversized = compileProxyValues("    type: trojan\n    alpn: '" + std::string(256, 'a') + "'\n");
    check(valuesNode(oversized).empty() && !oversized.fidelity.empty(), "oversized ALPN is rejected");
    for (const char* fields : {"network: grpc\n    grpc-service-name: [service]",
                              "network: grpc\n    grpc-opts: {grpc-service-name: {service: test}}",
                              "network: httpupgrade\n    httpupgrade-opts: {host: [example.test]}"}) {
        const auto result = compileProxyValues(std::string("    type: vmess\n    ") + fields + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(), "other transport scalar values are validated");
    }

    for (const char* fields : {"alpn: [h2]", "client-fingerprint: firefox",
                              "tls: false\n    sni: example.test", "tls: false\n    reality-opts: {public-key: sample}"}) {
        const auto result = compileProxyValues(std::string("    type: vless\n    ") + fields + "\n");
        check(valuesNode(result).empty() && !result.fidelity.empty(),
              "inactive TLS fields cannot disappear or silently enable TLS");
    }
    const auto inherited = valuesNode(compileProxyValues("    type: trojan\n",
                                     "global-client-fingerprint: firefox\n"));
    check(!inherited.empty() && inherited["tls"]["utls"]["fingerprint"] == "firefox",
          "node inherits its source global fingerprint");
    const auto explicitNode = valuesNode(compileProxyValues(
        "    type: trojan\n    client-fingerprint: safari\n", "global-client-fingerprint: firefox\n"));
    check(!explicitNode.empty() && explicitNode["tls"]["utls"]["fingerprint"] == "safari",
          "node fingerprint overrides source global fingerprint");
    for (const char* global : {"[firefox]", "{fingerprint: firefox}", "null", "''", "unknown-browser"}) {
        const auto result = compileProxyValues("    type: trojan\n",
            std::string("global-client-fingerprint: ") + global + "\n");
        check(!result.error.empty() && result.json.empty() && !result.fidelity.empty(),
              "invalid global fingerprint fails source and retains ledger");
    }
    const auto quic = valuesNode(compileProxyValues("    type: hysteria2\n",
                                "global-client-fingerprint: firefox\n"));
    check(!quic.empty() && !quic["tls"].contains("utls"), "global TCP fingerprint is not applied to QUIC");
    for (const char* fingerprint : {"random", "chrome_psk", "chrome_pq"}) {
        const auto result = compileProxyValues(std::string("    type: trojan\n    client-fingerprint: ") + fingerprint + "\n");
        check(!valuesNode(result).empty() && result.fidelity.size() == 1 &&
              result.fidelity.front().level == singbox::Fidelity::Approx,
              "random distributions and native folded aliases are explicitly approximate");
    }
    const auto mandatory = compileProxyValues("    type: anytls\n    password: sample\n    tls: false\n");
    check(valuesNode(mandatory).empty() && !mandatory.fidelity.empty(), "AnyTLS cannot disable mandatory TLS");
    auto failedMatchOptions = singbox::CompileOptions{};
    failedMatchOptions.profileYaml = "proxies:\n  - {name: invalid, type: trojan, alpn: {protocol: h2}, "
        "server: example.test, port: 443, password: sample}\nrules:\n  - MATCH,invalid\n";
    const auto failedMatch = singbox::compileConfig(failedMatchOptions);
    check(!failedMatch.error.empty() && failedMatch.json.empty() && !failedMatch.fidelity.empty(),
          "rejected value MATCH target fails and retains fidelity instead of direct fallback");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources;
    sources.mainConnectionId = "profile-1";
    sources.profileYaml = "global-client-fingerprint: firefox\nproxies:\n"
        "  - {name: same, type: trojan, server: example.test, port: 443, password: sample}\n"
        "rules:\n  - MATCH,same\n";
    sources.auxiliarySources = {{"profile-2", "secondary", "global-client-fingerprint: safari\nproxies:\n"
        "  - {name: same, type: trojan, server: example.test, port: 443, password: sample}\n"
        "rules:\n  - MATCH,same\n", true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test",
        .connectionId = "profile-2", .id = "fp-scope", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Node, .targetObject = "same"}};
    for (const char* secondaryFingerprint : {"safari", "chrome"}) {
        if (std::string_view(secondaryFingerprint) == "chrome")
            sources.auxiliarySources[0].content.erase(0, sources.auxiliarySources[0].content.find('\n') + 1);
        const auto result = singbox::compileConfig(sources);
        check(result.error.empty() && !result.json.empty(), "two sources with distinct global fingerprints compile");
        if (result.json.empty()) continue;
        const auto config = json::parse(result.json);
        for (const auto& object : result.sourceObjects) {
            if (object.objectId != "same") continue;
            const auto node = std::ranges::find_if(config["outbounds"], [&](const auto& out) {
                return out.value("tag", "") == object.tag;
            });
            check(node != config["outbounds"].end() &&
                  (*node)["tls"]["utls"]["fingerprint"] ==
                    (object.sourceId == "profile-1" ? "firefox" : secondaryFingerprint),
                  "global fingerprint never leaks between namespaced sources");
        }
    }
#endif
}

void testProxyUdpOptions() {
    const auto mapped = [](std::string_view type, std::string_view fields) {
        return compileProxyValues(std::format("    type: {}\n{}", type, fields));
    };
    for (const char* type : {"vmess", "vless"}) {
        const auto defaults = mapped(type, "");
        check(!valuesNode(defaults).empty() && !valuesNode(defaults).contains("packet_encoding") &&
              defaults.fidelity.empty(), "UDP encoding absent preserves protocol-specific native default");
        for (const char* encoding : {"packetaddr", "packet", "xudp", "''"}) {
            const auto result = mapped(type, std::format("    packet-encoding: {}\n", encoding));
            const std::string expected = std::string_view(encoding) == "packet" ? "packetaddr" :
                std::string_view(encoding) == "''" ? (std::string_view(type) == "vless" ? "xudp" : "") : encoding;
            check(!valuesNode(result).empty() && valuesNode(result)["packet_encoding"] == expected &&
                  result.fidelity.empty(), "UDP encoding aliases preserve Clash effective behavior");
        }
        for (const char* invalid : {"mystery", "[xudp]", "null"}) {
            const auto result = mapped(type, std::format("    packet-encoding: {}\n", invalid));
            check(valuesNode(result).empty() && !result.fidelity.empty(), "invalid UDP encoding rejects node with ledger");
        }
        for (const auto& [fields, expected] : std::vector<std::pair<std::string, std::string>>{
            {"    packet-addr: true\n", "packetaddr"},
            {"    xudp: true\n", "xudp"},
            {"    packet-addr: true\n    xudp: true\n", "xudp"},
            {"    packet-addr: false\n    xudp: false\n", std::string_view(type) == "vless" ? "xudp" : ""},
            {"    packet-encoding: packetaddr\n    xudp: true\n", std::string_view(type) == "vless" ? "packetaddr" : "xudp"},
            {"    packet-encoding: xudp\n    packet-addr: true\n", std::string_view(type) == "vless" ? "packetaddr" : "xudp"}}) {
            const auto result = mapped(type, fields);
            check(!valuesNode(result).empty() && valuesNode(result)["packet_encoding"] == expected &&
                  result.fidelity.empty(), "legacy UDP switches follow each Clash protocol's precedence");
        }
        for (const char* field : {"packet-addr", "xudp"}) {
            const auto result = mapped(type, std::format("    {}: [true]\n", field));
            check(valuesNode(result).empty() && !result.fidelity.empty(), "invalid legacy UDP boolean rejects node");
        }
    }
    for (const char* value : {"true", "false"}) {
        const auto result = mapped("vmess", std::format("    global-padding: {}\n    authenticated-length: {}\n", value, value));
        const auto node = valuesNode(result);
        check(!node.empty() && node["global_padding"] == (std::string_view(value) == "true") &&
              node["authenticated_length"] == (std::string_view(value) == "true") && result.fidelity.empty(),
              "VMess padding flags map exactly including explicit false");
    }
    for (const char* field : {"global-padding", "authenticated-length"}) {
        const auto result = mapped("vmess", std::format("    {}: invalid\n", field));
        check(valuesNode(result).empty() && !result.fidelity.empty(), "malformed VMess padding flag rejects node");
    }
    const auto hy2 = [&](std::string_view fields) { return mapped("hysteria2", "    password: sample\n" + std::string(fields)); };
    for (const char* type : {"salamander", "gecko"}) {
        const auto result = hy2(std::format("    obfs: {}\n    obfs-password: ' exact secret '\n", type));
        const auto node = valuesNode(result);
        check(!node.empty() && node["obfs"]["type"] == type && node["obfs"]["password"] == " exact secret " &&
              result.fidelity.empty(), "HY2 respects obfs type and literal credential");
    }
    const auto gecko = hy2("    obfs: gecko\n    obfs-password: sample\n    obfs-min-packet-size: 600\n    obfs-max-packet-size: 1400\n");
    check(!valuesNode(gecko).empty() && valuesNode(gecko)["obfs"]["min_packet_size"] == 600 &&
          valuesNode(gecko)["obfs"]["max_packet_size"] == 1400 && gecko.fidelity.empty(), "HY2 Gecko sizes map to fixed native schema");
    const auto geckoDefaults = hy2("    obfs: gecko\n    obfs-password: sample\n    obfs-min-packet-size: 0\n    obfs-max-packet-size: 0\n");
    check(!valuesNode(geckoDefaults).empty() && valuesNode(geckoDefaults)["obfs"]["min_packet_size"] == 0 &&
          valuesNode(geckoDefaults)["obfs"]["max_packet_size"] == 0 && geckoDefaults.fidelity.empty(),
          "HY2 Gecko explicit zero preserves native default sizes");
    for (const char* fields : {"    obfs: salamander\n", "    obfs: gecko\n    obfs-password: ''\n",
         "    obfs: unknown\n    obfs-password: sample\n", "    obfs: [gecko]\n",
         "    obfs: gecko\n    obfs-password: sample\n    obfs-max-packet-size: [1200]\n",
         "    obfs: gecko\n    obfs-password: sample\n    obfs-max-packet-size: 500\n",
         "    obfs: gecko\n    obfs-password: sample\n    obfs-min-packet-size: 1500\n",
         "    obfs: gecko\n    obfs-password: sample\n    obfs-max-packet-size: 2049\n",
         "    obfs: gecko\n    obfs-password: sample\n    obfs-min-packet-size: -1\n"}) {
        const auto result = hy2(fields);
        check(valuesNode(result).empty() && !result.fidelity.empty(), "invalid HY2 obfs never becomes plain/salamander connection");
    }
    const auto inactive = hy2("    obfs-password: sample\n");
    check(!valuesNode(inactive).empty() && !valuesNode(inactive).contains("obfs") && !inactive.fidelity.empty(),
          "password alone does not enable HY2 obfs and ignored setting is reported");
    for (const char* fields : {"    hop-interval: 30\n", "    obfs-min-packet-size: 600\n",
                              "    obfs: salamander\n    obfs-password: sample\n    obfs-max-packet-size: 1400\n"}) {
        const auto result = hy2(fields);
        check(!valuesNode(result).empty() && !valuesNode(result).contains("hop_interval") &&
              std::ranges::any_of(result.fidelity, [](const auto& note) {
                  return note.level == singbox::Fidelity::Approx;
              }), "inactive HY2 option is reported without silently enabling hopping or Gecko");
    }
    for (const char* hop : {"30", "15-30", "30-15"}) {
        const auto result = hy2(std::format("    ports: '20000-30000,40000'\n    hop-interval: {}\n", hop));
        const auto node = valuesNode(result);
        check(!node.empty() && node["server_ports"] == json{"20000:30000", "40000:40000"} &&
              !node.contains("server_port") &&
              node["hop_interval"] == (std::string_view(hop) == "30" ? "30s" : "15s") &&
              node["hop_interval_max"] == "30s" && result.fidelity.empty(), "HY2 port hopping and interval range map exactly");
    }
    const auto reversedPorts = hy2("    ports: 400-200\n");
    check(!valuesNode(reversedPorts).empty() && valuesNode(reversedPorts)["server_ports"] == json{"200:400"} &&
          reversedPorts.fidelity.empty(), "HY2 reversed ports normalize exactly as Clash");
    for (const char* ports : {"0", "65536", "100,,200", "100,", "[100, 200]", "null", "100:200"}) {
        const auto result = hy2(std::format("    ports: {}\n", ports));
        check(valuesNode(result).empty() && !result.fidelity.empty(), "HY2 malformed port list rejects atomically");
    }
    for (const char* hop : {"-1", "15-30-40", "1.5", "9223372037", "[30]", "null"}) {
        const auto result = hy2(std::format("    ports: '100-200'\n    hop-interval: {}\n", hop));
        check(valuesNode(result).empty() && !result.fidelity.empty(), "HY2 malformed or overflowing duration rejects node");
    }
    for (const char* hop : {"0", "''", "2"}) {
        const auto result = hy2(std::format("    ports: '100-200'\n    hop-interval: {}\n", hop));
        check(!valuesNode(result).empty() && valuesNode(result)["hop_interval"] ==
              (std::string_view(hop) == "2" ? "5s" : "30s") &&
              (std::string_view(hop) == "2" ? !result.fidelity.empty() : result.fidelity.empty()),
              "HY2 mirrors Clash default and minimum hop interval with clamp ledger");
    }
    for (const char* port : {"", "    port: 0\n"}) {
        singbox::CompileOptions options;
        options.profileYaml = "proxies:\n  - name: values\n    type: hysteria2\n    server: example.test\n"
            "    password: sample\n    ports: '100-200'\n" + std::string(port) + "rules:\n  - MATCH,DIRECT\n";
        const auto result = singbox::compileConfig(options);
        check(!valuesNode(result).empty() && !valuesNode(result).contains("server_port") && result.fidelity.empty(),
              "HY2 ports-only endpoint does not require unused single port");
        check(!valuesNode(result).empty() && valuesNode(result)["hop_interval"] == "30s" &&
              valuesNode(result)["hop_interval_max"] == "30s", "HY2 absent hop interval is explicitly the Clash default");
    }
}

void testShadowsocksPlugins() {
    const auto ss = [](std::string_view fields) {
        return compileProxyValues("    type: ss\n    cipher: aes-256-gcm\n    password: sample\n" + std::string(fields));
    };
    for (const char* mode : {"http", "tls"}) {
        const auto result = ss(std::format("    plugin: obfs\n    plugin-opts: {{mode: {}}}\n", mode));
        const auto node = valuesNode(result);
        check(!node.empty() && node["plugin"] == "obfs-local" &&
              node["plugin_opts"] == std::format("obfs={};obfs-host=bing.com", mode) && result.fidelity.empty(),
              "SS simple-obfs maps mode and explicit Clash default host");
    }
    const auto escaped = valuesNode(ss("    plugin: obfs\n    plugin-opts: {mode: http, host: 'example.test;tls=1\\suffix'}\n"));
    check(!escaped.empty() && escaped["plugin_opts"] == "obfs=http;obfs-host=example.test\\;tls\\=1\\\\suffix",
          "SIP003 values escape separators without injecting extra plugin options");
    for (const char* tls : {"true", "false"}) {
        for (const char* mux : {"true", "false"}) {
            const auto result = ss(std::format("    plugin: v2ray-plugin\n    plugin-opts: {{mode: websocket, tls: {}, mux: {}}}\n", tls, mux));
            const auto node = valuesNode(result);
            const auto expected = std::format("mode=websocket;host=bing.com;path=/;mux={}{}", std::string_view(mux) == "true" ? 1 : 0,
                                               std::string_view(tls) == "true" ? ";tls" : "");
            check(!node.empty() && node["plugin"] == "v2ray-plugin" && node["plugin_opts"] == expected &&
                  result.fidelity.empty(), "SS v2ray-plugin preserves TLS presence and mux boolean semantics");
        }
    }
    const auto defaults = ss("    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket}\n");
    check(!valuesNode(defaults).empty() && valuesNode(defaults)["plugin_opts"] ==
          "mode=websocket;host=bing.com;path=/;mux=1" && defaults.fidelity.empty(),
          "SS v2ray-plugin keeps Clash host/path/mux defaults");
    const auto explicitOptions = ss("    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, host: ws.example.test, path: '/p?q=a;b=c', headers: {}, skip-cert-verify: false, v2ray-http-upgrade: false, v2ray-http-upgrade-fast-open: false}\n");
    check(!valuesNode(explicitOptions).empty() && valuesNode(explicitOptions)["plugin_opts"] ==
          "mode=websocket;host=ws.example.test;path=/p?q\\=a\\;b\\=c;mux=1" && explicitOptions.fidelity.empty(),
          "SS v2ray-plugin keeps explicit strings and inactive compatibility defaults");
    for (const char* fields : {
        "    plugin: [obfs]\n", "    plugin: null\n", "    plugin: restls\n    plugin-opts: {mode: http}\n",
        "    plugin: obfs\n", "    plugin: obfs\n    plugin-opts: null\n", "    plugin: obfs\n    plugin-opts: [http]\n",
        "    plugin: obfs\n    plugin-opts: {mode: unknown}\n", "    plugin: obfs\n    plugin-opts: {mode: http, host: [bad]}\n",
        "    plugin: obfs\n    plugin-opts: {mode: http, host: ''}\n", "    plugin: obfs\n    plugin-opts: {mode: http, extra: true}\n",
        "    plugin: obfs\n    plugin-opts: {mode: http, mode: tls}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: quic}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, tls: maybe}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, mux: [false]}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, path: null}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, tls: true, skip-cert-verify: true}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, headers: {X-Test: sample}}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, headers: {Host: [bad]}}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, v2ray-http-upgrade: true}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, v2ray-http-upgrade-fast-open: true}\n",
        "    plugin: v2ray-plugin\n    plugin-opts: {mode: websocket, fingerprint: sample}\n"}) {
        const auto result = ss(fields);
        check(valuesNode(result).empty() && std::ranges::any_of(result.fidelity, [](const auto& note) {
            return note.level == singbox::Fidelity::Unsupported;
        }), "SS invalid or unmapped plugin options reject whole node with ledger");
    }
    for (const char* version : {"0", "1", "2"}) {
        const auto result = ss(std::format("    udp-over-tcp: true\n    udp-over-tcp-version: {}\n", version));
        check(!valuesNode(result).empty() && valuesNode(result)["udp_over_tcp"] ==
              json{{"enabled", true}, {"version", std::string_view(version) == "2" ? 2 : 1}} && result.fidelity.empty(),
              "SS UOT version zero/default is legacy v1 and v2 stays explicit");
    }
    const auto defaultUot = ss("    udp-over-tcp: true\n");
    check(!valuesNode(defaultUot).empty() && valuesNode(defaultUot)["udp_over_tcp"] == json{{"enabled", true}, {"version", 1}} &&
          defaultUot.fidelity.empty(), "SS UOT enabled without version retains Clash legacy v1 instead of native v2");
    const auto disabledUot = ss("    udp-over-tcp: false\n    udp-over-tcp-version: 2\n");
    check(!valuesNode(disabledUot).empty() && valuesNode(disabledUot)["udp_over_tcp"] == json{{"enabled", false}, {"version", 2}} &&
          disabledUot.fidelity.empty(), "SS UOT explicit disabled flag keeps version without enabling transport");
    for (const char* version : {"-1", "3", "257", "[1]", "null", "1.5"}) {
        const auto result = ss(std::format("    udp-over-tcp-version: {}\n", version));
        check(valuesNode(result).empty() && !result.fidelity.empty(), "SS invalid UOT version rejects without narrowing or defaulting");
    }
    singbox::CompileOptions target;
    target.profileYaml = "proxies:\n  - {name: invalid-plugin, type: ss, server: example.test, port: 443, cipher: aes-256-gcm, password: sample, plugin: v2ray-plugin, plugin-opts: {mode: websocket, headers: {X-Test: sample}}}\n"
        "rules:\n  - MATCH,invalid-plugin\n";
    const auto rejected = singbox::compileConfig(target);
    check(!rejected.error.empty() && !rejected.fidelity.empty(), "SS plugin rejection keeps fidelity when referenced MATCH fails");
}

void testLogicalRuleResources() {
    const auto compile = [](std::string_view rules, std::string_view providers = {}) {
        singbox::CompileOptions options;
        options.profileYaml = std::string(providers) + "\nrules:\n" + std::string(rules) + "  - MATCH,DIRECT\n";
        return singbox::compileConfig(options);
    };
    const auto routeRules = [](const auto& result) {
        return result.json.empty() ? json::array() : json::parse(result.json)["route"]["rules"];
    };
    const auto logicalRules = [&](const auto& result) {
        json found = json::array();
        for (const auto& rule : routeRules(result)) if (rule.value("type", "") == "logical") found.push_back(rule);
        return found;
    };
    const auto privateGeo = compile("  - AND,((NETWORK,TCP),(NOT,((GEOIP,private)))),REJECT-DROP\n");
    const auto privateRules = logicalRules(privateGeo);
    check(privateGeo.error.empty() && privateGeo.fidelity.empty() && privateRules.size() == 1 &&
          privateRules[0]["method"] == "drop" && privateRules[0]["rules"][1]["invert"] == true &&
          privateRules[0]["rules"][1]["rules"][0]["ip_is_private"] == true && privateGeo.ruleSetResources.empty(),
          "nested private GEO retains NOT and outer reject-drop without resources");
    const auto countries = compile("  - OR,((GEOIP,CN),(AND,((GEOSITE,CN),(NETWORK,TCP)))),DIRECT\n");
    const auto countryRules = logicalRules(countries);
    check(countries.error.empty() && countries.fidelity.empty() && countryRules.size() == 1 &&
          countryRules[0]["rules"][0]["rule_set"] == json{"geoip-cn"} &&
          countryRules[0]["rules"][1]["rules"][0]["rule_set"] == json{"geosite-cn"} &&
          countries.ruleSetResources.size() == 2, "nested GEOIP/GEOSITE create typed references and resource manifest");
    const std::string providers = "rule-providers:\n  precise:\n    type: inline\n    behavior: domain\n    payload: [example.test]\n";
    const auto inlineSet = compile("  - AND,((RULE-SET,precise),(DST-PORT,443)),DIRECT\n", providers);
    const auto inlineRules = logicalRules(inlineSet);
    check(inlineSet.error.empty() && inlineSet.fidelity.empty() && inlineRules.size() == 1 &&
          inlineRules[0]["rules"][0]["rule_set"] == json{"clash-provider-0"} &&
          !inlineRules[0]["rules"][0].contains("outbound"), "logical RULE-SET references exact inline provider with only outer action");
    const auto alias = compile("  - NOT,((RULE-SET,cn)),DIRECT\n");
    const auto aliasRules = logicalRules(alias);
    check(aliasRules.size() == 1 && aliasRules[0]["invert"] == true && alias.ruleSetResources.size() == 1 &&
          std::ranges::any_of(alias.fidelity, [](const auto& note) { return note.level == singbox::Fidelity::Approx; }),
          "nested China alias remains approximate and preserves NOT");
    if (!alias.json.empty()) check(json::parse(alias.json)["dns"].value("rules", json::array()).empty(),
          "negated China condition does not insert a global China DNS rule");
    const auto failedProvider = compile("  - AND,((RULE-SET,cn),(NETWORK,TCP)),DIRECT\n",
        "rule-providers:\n  cn: {type: http, behavior: domain, url: https://example.test/rules.yaml}\n");
    check(logicalRules(failedProvider).empty() && failedProvider.ruleSetResources.empty() &&
          !failedProvider.fidelity.empty(), "failed declared provider never falls back to same-named China alias in logical rule");
    for (const char* child : {"GEOIP,TELEGRAM", "GEOSITE,invalid!", "RULE-SET,missing", "UNKNOWN,x", "GEOIP,CN,bogus"}) {
        const auto failed = compile(std::format("  - AND,((GEOSITE,CN),({})),DIRECT\n", child));
        check(logicalRules(failed).empty() && failed.ruleSetResources.empty() &&
              !failed.fidelity.empty() && !failed.json.empty() && json::parse(failed.json)["route"].value("rule_set", json::array()).empty(),
              "one invalid logical child rejects entire tree and rolls back newly staged GEO resources");
    }
    const auto retained = compile("  - GEOIP,CN,DIRECT\n  - AND,((GEOSITE,CN),(UNKNOWN,x)),DIRECT\n  - GEOSITE,CN,DIRECT\n");
    check(logicalRules(retained).empty() && retained.ruleSetResources.size() == 2 &&
          routeRules(retained).size() >= 2, "failed logical rule preserves earlier resources and permits later valid GEO rule");
    const auto unknownTarget = compile("  - AND,((GEOSITE,CN),(NETWORK,TCP)),missing\n");
    check(logicalRules(unknownTarget).empty() && unknownTarget.ruleSetResources.empty() && !unknownTarget.fidelity.empty(),
          "invalid logical action target does not allocate unused rule-set resources");
    const auto headless = compile("  - RULE-SET,rejected,DIRECT\n",
        "rule-providers:\n  rejected:\n    type: inline\n    behavior: classical\n    payload: ['AND,((GEOSITE,CN),(NETWORK,TCP))']\n");
    check(headless.ruleSetResources.empty() && !headless.fidelity.empty() && !headless.json.empty() &&
          json::parse(headless.json)["route"].value("rule_set", json::array()).empty(),
          "headless provider continues rejecting GEO/RULE-SET references instead of writing route-only fields");
    const auto noResolve = compile("  - AND,((GEOIP,private),(NETWORK,TCP)),DIRECT,no-resolve\n");
    check(logicalRules(noResolve).size() == 1 && std::ranges::any_of(noResolve.fidelity, [](const auto& note) {
          return note.level == singbox::Fidelity::Approx; }), "logical no-resolve modifier retains existing approximate contract");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources;
    sources.mainConnectionId = "logical-main";
    sources.profileYaml = providers + "rules:\n  - AND,((RULE-SET,precise),(NOT,((GEOSITE,CN)))),DIRECT\n  - MATCH,DIRECT\n";
    sources.auxiliarySources = {{"logical-secondary", "secondary", providers + "rules:\n  - MATCH,DIRECT\n", true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test",
        .connectionId = "logical-secondary", .id = "logical-source", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Default}};
    const auto namespaced = singbox::compileConfig(sources);
    const auto scopedRules = logicalRules(namespaced);
    check(namespaced.error.empty() && scopedRules.size() == 1 && namespaced.participatingSources.size() == 2,
          "nested resource rule compiles with two desktop sources");
    if (scopedRules.size() == 1) {
        const auto config = json::parse(namespaced.json);
        const auto& providerRef = scopedRules[0]["rules"][0]["rule_set"][0];
        const auto& geoRef = scopedRules[0]["rules"][1]["rules"][0]["rule_set"][0];
        const auto hasTag = [&](const auto& tag) {
            return std::ranges::any_of(config["route"]["rule_set"], [&](const auto& set) { return set["tag"] == tag; });
        };
        check(providerRef != "clash-provider-0" && geoRef != "geosite-cn" && hasTag(providerRef) && hasTag(geoRef) &&
              namespaced.ruleSetResources.size() == 1 && namespaced.ruleSetResources[0].tag == "geosite-cn",
              "nested provider/GEO runtime tags are namespaced while owned cache identity stays canonical");
    }
#endif
}

void testHysteria1() {
    const auto hy1 = [](std::string_view fields = {}) {
        return compileProxyValues("    type: hysteria\n    up: 20 Mbps\n    down: 100\n" + std::string(fields));
    };
    const auto basic = hy1("    auth-str: ' sample secret '\n    obfs: ' obfs secret '\n");
    const auto node = valuesNode(basic);
    check(basic.error.empty() && !node.empty() && node.value("type", "") == "hysteria" &&
          node.value("auth_str", "") == " sample secret " && node.value("obfs", "") == " obfs secret " &&
          node.value("up_mbps", 0) == 20 && node.value("down_mbps", 0) == 100 &&
          node.value("stream_receive_window", 0) == 15728640 && node.value("connection_receive_window", 0) == 67108864 &&
          node.value("tls", json::object()).value("enabled", false) && !node["tls"].contains("utls") &&
          std::ranges::any_of(basic.fidelity, [](const auto& note) { return note.level == singbox::Fidelity::Approx; }),
          "HY1 keeps literal authentication/obfs, required QUIC TLS and bandwidth, reports initial-window difference");
    const auto exact = hy1("    auth: 'c2FtcGxl'\n    auth-str: ignored\n    protocol: udp\n    recv-window-conn: 1048576\n    recv-window: 4194304\n    disable-mtu-discovery: true\n    fast-open: false\n    udp: false\n");
    const auto exactNode = valuesNode(exact);
    check(!exactNode.empty() && exact.fidelity.empty() && exactNode.value("auth", "") == "c2FtcGxl" &&
          !exactNode.contains("auth_str") && exactNode.value("stream_receive_window", 0) == 1048576 &&
          exactNode.value("connection_receive_window", 0) == 4194304 && exactNode.value("disable_path_mtu_discovery", false) &&
          exactNode.value("network", "") == "tcp", "HY1 auth bytes take priority, window fields follow actual Clash axes, explicit UDP restriction survives");
    for (const char* fields : {"", "    auth: ''\n", "    auth-str: ''\n", "    protocol: ''\n", "    protocol: faketcp\n    obfs-protocol: udp\n"}) {
        check(!valuesNode(hy1(fields)).empty(), "HY1 anonymous authentication and active UDP transport defaults compile");
    }
    for (const char* fields : {"    ports: '443,5000-5002'\n", "    ports: '443'\n    hop-interval: 0\n", "    ports: '443'\n    hop-interval: 15\n"}) {
        const auto hopping = valuesNode(hy1(fields));
        check(!hopping.empty() && !hopping.contains("server_port") && hopping.contains("server_ports") &&
              hopping.value("hop_interval", "") == (std::string_view(fields).find("15") == std::string_view::npos ? "10s" : "15s") &&
              !hopping.contains("hop_interval_max"), "HY1 port hopping preserves its own 10-second default without HY2-only fields");
    }
    singbox::CompileOptions portsOnly;
    portsOnly.profileYaml = "proxies:\n  - {name: values, type: hysteria, server: example.test, ports: '5000-5001', up: 20, down: 100}\nrules:\n  - MATCH,DIRECT\n";
    check(!valuesNode(singbox::compileConfig(portsOnly)).empty(), "HY1 hopping does not require an unused single port");
    for (const char* fields : {"    protocol: faketcp\n", "    obfs-protocol: wechat-video\n", "    fast-open: true\n",
          "    auth: not_base64\n", "    auth: ' c2FtcGxl '\n", "    auth: 'YQ'\n", "    auth: 'YQ==='\n", "    auth: []\n", "    auth-str: {}\n", "    obfs: []\n",
          "    hop-interval: -1\n", "    ports: '443'\n    hop-interval: 4\n", "    hop-interval: '5-10'\n", "    ports: '443,bad'\n",
          "    recv-window: 1048576\n", "    recv-window-conn: 1048576\n", "    recv-window: -1\n", "    disable-mtu-discovery: perhaps\n",
          "    alpn: []\n", "    tls: false\n", "    client-fingerprint: chrome\n", "    fingerprint: sample\n", "    certificate: sample\n"}) {
        const auto failed = hy1(fields);
        check(valuesNode(failed).empty() && std::ranges::any_of(failed.fidelity, [](const auto& note) {
              return note.level == singbox::Fidelity::Unsupported; }), "HY1 unrepresentable transport, invalid credential/options and certificate constraints reject whole node");
    }
    for (const char* type : {"hysteria", "hysteria2"}) {
        const auto bytes = valuesNode(compileProxyValues(std::format("    type: {}\n    up: 3 MBps\n    down: 10 Mbps\n", type)));
        check(!bytes.empty() && bytes.value("up_mbps", 0) == 24, "Mbps and MBps are distinct and bytes convert without truncation");
        for (const char* fields : {"    up: auto\n", "    up: 30 mbps\n", "    up: 1.5 Mbps\n", "    up: 0\n", "    up: 268435456 MBps\n", "    up: []\n"}) {
            const auto failed = compileProxyValues(std::format("    type: {}\n    down: 10\n{}", type, fields));
            check(valuesNode(failed).empty() && !failed.fidelity.empty(), "HY1/HY2 bandwidth invalid units, values and overflow reject node");
        }
    }
    const auto absent = compileProxyValues("    type: hysteria\n    auth-str: sample\n");
    check(valuesNode(absent).empty() && !absent.fidelity.empty(), "HY1 requires both bandwidth directions instead of native failure at startup");
    for (const char* auth : {"YQ==", "YWI=", "YWJj", "AP8=", "YR==", "YQ==\\r\\n"}) {
        const auto encoded = valuesNode(hy1(std::format("    auth: \"{}\"\n", auth)));
        check(!encoded.empty() && encoded.contains("auth") && !encoded.contains("auth_str"),
              "HY1 standard Base64 accepts padding, binary bytes, Go-compatible padding bits and CR/LF");
    }
    for (const std::size_t size : {65535, 65536}) {
        const auto credential = hy1("    auth-str: '" + std::string(size, 's') + "'\n");
        check(valuesNode(credential).empty() == (size > 65535), "HY1 literal credential respects 16-bit wire length without truncation");
    }
    const auto rejectedBytes = hy1("    auth: '" + std::string(87384, 'A') + "'\n");
    check(valuesNode(rejectedBytes).empty() && !rejectedBytes.fidelity.empty(), "HY1 encoded credential enforces decoded wire length");
    singbox::CompileOptions invalidMatch;
    invalidMatch.profileYaml = "proxies:\n  - {name: rejected, type: hysteria, server: example.test, port: 443, up: 20, down: 100, protocol: faketcp}\nrules:\n  - MATCH,rejected\n";
    const auto failedMatch = singbox::compileConfig(invalidMatch);
    check(!failedMatch.error.empty() && failedMatch.json.empty() && !failedMatch.fidelity.empty(),
          "rejected HY1 target fails MATCH and keeps fidelity instead of becoming direct");
    const auto detached = hy1("    hop-interval: 15\n");
    check(!valuesNode(detached).contains("hop_interval") && std::ranges::any_of(detached.fidelity, [](const auto& note) {
        return note.detail.find("hop-interval") != std::string::npos;
    }), "HY1 interval without port hopping stays inactive with ledger entry");
}

void testSsh() {
    const auto ssh = [](std::string_view fields = {}) {
        return compileProxyValues("    type: ssh\n    username: ' test user '\n" + std::string(fields));
    };
    const auto password = ssh("    password: ' sample password '\n    udp: false\n");
    const auto node = valuesNode(password);
    check(password.error.empty() && !node.empty() && password.fidelity.empty() &&
          node.value("type", "") == "ssh" && node.value("user", "") == " test user " &&
          node.value("password", "") == " sample password " && !node.contains("username") &&
          !node.contains("network") && !node.contains("tls"),
          "SSH maps literal user/password without unsupported TLS or network fields");
    const std::string key = "    private-key: |\n      -----BEGIN OPENSSH PRIVATE KEY-----\n      test-only-key\n      -----END OPENSSH PRIVATE KEY-----\n";
    const auto keyed = ssh(key + "    private-key-passphrase: ' test passphrase '\n");
    const auto keyedNode = valuesNode(keyed);
    check(!keyedNode.empty() && keyed.fidelity.empty() &&
          keyedNode.value("private_key", "") == "-----BEGIN OPENSSH PRIVATE KEY-----\ntest-only-key\n-----END OPENSSH PRIVATE KEY-----\n" &&
          keyedNode.value("private_key_passphrase", "") == " test passphrase " && !keyedNode.contains("private_key_path"),
          "SSH inline key and encrypted-key passphrase remain literal; native check validates cryptographic syntax");
    const auto dual = ssh(key + "    password: sample\n");
    check(!valuesNode(dual).empty() && std::ranges::any_of(dual.fidelity, [](const auto& note) {
        return note.level == singbox::Fidelity::Approx && note.detail.find("认证顺序") != std::string::npos;
    }), "SSH dual authentication reports native password-before-key order");
    const auto pins = ssh("    host-key: ['ssh-ed25519 test-only', 'ssh-rsa test-only']\n"
                          "    host-key-algorithms: [ssh-ed25519, rsa-sha2-512]\n");
    const auto pinnedNode = valuesNode(pins);
    check(!pinnedNode.empty() && pins.fidelity.empty() &&
          pinnedNode.value("host_key", json::array()) == json::array({"ssh-ed25519 test-only", "ssh-rsa test-only"}) &&
          pinnedNode.value("host_key_algorithms", json::array()) == json::array({"ssh-ed25519", "rsa-sha2-512"}),
          "SSH host pins and algorithm preference lists preserve every member and order");
    const auto unpinned = ssh("    private-key: ''\n    password: ''\n    host-key: []\n");
    check(!valuesNode(unpinned).empty() && unpinned.fidelity.empty(), "SSH optional empty credentials/pins keep source defaults");
    const auto scalarPin = valuesNode(ssh("    host-key: 'ssh-ed25519 test-only'\n    host-key-algorithms: ssh-ed25519\n"));
    check(!scalarPin.empty() && scalarPin.value("host_key", json::array()) == json::array({"ssh-ed25519 test-only"}) &&
          scalarPin.value("host_key_algorithms", json::array()) == json::array({"ssh-ed25519"}), "SSH scalar pin/algorithm keeps single-member compatibility");
    for (const char* algorithm : {"ssh-rsa", "ssh-dss", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
          "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "rsa-sha2-256", "rsa-sha2-512",
          "ssh-rsa-cert-v01@openssh.com", "ssh-dss-cert-v01@openssh.com", "ecdsa-sha2-nistp256-cert-v01@openssh.com",
          "ecdsa-sha2-nistp384-cert-v01@openssh.com", "ecdsa-sha2-nistp521-cert-v01@openssh.com", "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com",
          "ssh-ed25519-cert-v01@openssh.com", "sk-ssh-ed25519-cert-v01@openssh.com", "rsa-sha2-256-cert-v01@openssh.com", "rsa-sha2-512-cert-v01@openssh.com"}) {
        check(!valuesNode(ssh(std::format("    host-key-algorithms: ['{}']\n", algorithm))).empty(),
              "SSH accepts fixed native host-key algorithms including certificates and security keys");
    }
    const auto orphan = ssh("    private-key-passphrase: 'inactive secret'\n");
    check(!valuesNode(orphan).contains("private_key_passphrase") && std::ranges::any_of(orphan.fidelity, [](const auto& note) {
        return note.level == singbox::Fidelity::Approx && note.detail.find("private-key-passphrase") != std::string::npos &&
               note.detail.find("inactive secret") == std::string::npos;
    }), "SSH inactive passphrase is reported without leaking its value");
    const auto udp = ssh("    udp: true\n");
    check(!valuesNode(udp).empty() && !valuesNode(udp).contains("network") && !udp.fidelity.empty(),
          "SSH requested UDP reports TCP-only native capability without invented fields");
    for (const char* fields : {"    private-key: ./id_ed25519\n", "    private-key: []\n",
          "    private-key-passphrase: {}\n", "    password: []\n", "    host-key: [valid, {}]\n", "    host-key: ['']\n",
          "    host-key-algorithms: {}\n", "    host-key-algorithms: [ssh-ed25519, null]\n", "    host-key-algorithms: []\n",
          "    host-key-algorithms: [ssh-ed25519, unknown-algorithm]\n",
          "    tls: true\n", "    network: tcp\n", "    client-fingerprint: chrome\n", "    cipher: aes128-ctr\n", "    udp: perhaps\n"}) {
        const auto failed = ssh(fields);
        check(valuesNode(failed).empty() && std::ranges::any_of(failed.fidelity, [](const auto& note) {
            return note.level == singbox::Fidelity::Unsupported;
        }), "SSH paths, invalid list members and unrepresentable fields reject the whole node");
    }
    for (const char* user : {"", "    username: ''\n", "    username: null\n", "    username: []\n"}) {
        const auto failed = compileProxyValues(std::string("    type: ssh\n") + user);
        check(valuesNode(failed).empty() && !failed.fidelity.empty(), "SSH missing/empty user never silently becomes root");
    }
    for (const char* target : {"missing", "values"}) {
        const auto failed = ssh(std::format("    dialer-proxy: {}\n", target));
        check(!failed.error.empty() && failed.json.empty() && !failed.fidelity.empty(),
              "SSH missing or cyclic detour fails compilation instead of direct fallback");
    }
    singbox::CompileOptions failedMatch;
    failedMatch.profileYaml = "proxies:\n  - {name: rejected, type: ssh, server: example.test, port: 22, username: test, private-key: ./id}\nrules:\n  - MATCH,rejected\n";
    const auto rejected = singbox::compileConfig(failedMatch);
    check(!rejected.error.empty() && rejected.json.empty() && !rejected.fidelity.empty(),
          "SSH rejected target keeps failure ledger and cannot turn MATCH into direct");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources;
    const auto content = [](std::string_view user) {
        return std::format("proxies:\n  - {{name: same, type: ssh, server: example.test, port: 22, username: {}, dialer-proxy: base}}\n"
            "  - {{name: base, type: ssh, server: example.test, port: 22, username: {}}}\nrules:\n  - MATCH,same\n", user, user);
    };
    sources.mainConnectionId = "profile-1";
    sources.profileYaml = content("main-user");
    sources.auxiliarySources = {{"profile-2", "secondary", content("secondary-user"), true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test",
        .connectionId = "profile-2", .id = "ssh-scope", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Node, .targetObject = "same"}};
    const auto namespaced = singbox::compileConfig(sources);
    check(namespaced.error.empty() && !namespaced.json.empty(), "SSH namespaced sources compile with their detour closure");
    if (!namespaced.json.empty()) {
        const auto config = json::parse(namespaced.json);
        int verified = 0;
        for (const auto& object : namespaced.sourceObjects) {
            if (object.objectId != "same") continue;
            const auto node = std::ranges::find_if(config["outbounds"], [&](const auto& out) { return out.value("tag", "") == object.tag; });
            const auto base = std::ranges::find_if(namespaced.sourceObjects, [&](const auto& value) {
                return value.sourceId == object.sourceId && value.objectId == "base";
            });
            check(node != config["outbounds"].end() && base != namespaced.sourceObjects.end() &&
                  node->value("user", "") == (object.sourceId == "profile-1" ? "main-user" : "secondary-user") &&
                  node->value("detour", "") == base->tag, "SSH credentials and detours stay inside their own source namespace");
            ++verified;
        }
        check(verified == 2, "both namespaced SSH targets remain available");
    }
#endif
}

void testFileRuleProviders() {
    const RuleSetTestDirectory directory;
    const RuleSetTestDirectory outside;
    std::filesystem::create_directory(directory.path / "rules");
    const auto write = [&](const std::string& name, const std::string& content) {
        std::ofstream file(directory.path / name, std::ios::binary | std::ios::trunc);
        file << content;
        check(file.good(), "file provider fixture written");
    };
    const auto compile = [&](const std::string& path, const std::string& behavior = "domain",
                             const std::string& format = "yaml", const std::string& extra = "") {
        singbox::CompileOptions options;
        options.ruleProviderDir = directory.path.string();
        options.profileYaml = std::format(
            "rule-providers:\n  cn:\n    type: file\n    path: '{}'\n    behavior: {}\n    format: {}\n{}"
            "rules:\n  - AND,((RULE-SET,cn),(NETWORK,TCP)),DIRECT\n  - MATCH,DIRECT\n",
            path, behavior, format, extra);
        return singbox::compileConfig(options);
    };
    const auto sets = [](const auto& result) {
        return result.json.empty() ? json::array() :
            json::parse(result.json)["route"].value("rule_set", json::array());
    };
    const auto routeRules = [](const auto& result) {
        return result.json.empty() ? json::array() :
            json::parse(result.json)["route"].value("rules", json::array());
    };
    write("rules/domain.yaml", "payload:\n  - example.test\n  - '+.suffix.test'\n");
    const auto domain = compile("rules/domain.yaml");
    const auto domainSets = sets(domain);
    check(domain.error.empty() && domainSets.size() == 1 &&
          domainSets[0]["rules"][0]["domain"] == json{"example.test"} &&
          domainSets[0]["rules"][1]["domain_suffix"] == json{"suffix.test"} &&
          domain.ruleSetResources.empty() && std::ranges::any_of(routeRules(domain), [](const auto& rule) {
              return rule.value("type", "") == "logical" && rule["rules"][0]["rule_set"] == json{"clash-provider-0"};
          }),
          "file YAML provider keeps all conditions and logical reference without GEO ownership");
    check(domain.fidelity.size() == 1 && domain.fidelity[0].level == singbox::Fidelity::Approx &&
          domain.fidelity[0].subject == "rule-providers.cn",
          "file snapshot lifecycle is explicitly approximate rather than pretending to watch");
    write("rules/ip.txt", "# comment\r\n // premium comment\r\n\r\n192.0.2.0/24\r\n2001:db8::/32");
    const auto ip = compile("rules/ip.txt", "ipcidr", "text");
    check(ip.error.empty() && sets(ip).size() == 1 && sets(ip)[0]["rules"].size() == 2 &&
          sets(ip)[0]["rules"][1]["ip_cidr"] == json{"2001:db8::/32"},
          "text file accepts comments, CRLF, IPv6 and last line without newline");
    write("rules/classical.txt", "AND,((DOMAIN-SUFFIX,example.test),(DST-PORT,443))\nNOT,((NETWORK,UDP))\n");
    const auto classical = compile("rules/classical.txt", "classical", "text");
    check(classical.error.empty() && sets(classical)[0]["rules"].size() == 2 &&
          sets(classical)[0]["rules"][1]["invert"] == true,
          "classical file uses atomic headless logical converter");
    const std::string modifiers = "IP-CIDR,192.0.2.28/32,no-resolve\n"
        "IP-CIDR6,2001:db8::/32,no-resolve\n"
        "AND,((IP-CIDR,198.51.100.0/24,no-resolve),(NETWORK,TCP))\n";
    write("rules/modifiers.txt", modifiers);
    const auto modified = compile("rules/modifiers.txt", "classical", "text");
    check(modified.error.empty() && sets(modified).size() == 1 &&
          sets(modified)[0]["rules"].size() == 3 &&
          sets(modified)[0]["rules"][0]["ip_cidr"] == json{"192.0.2.28/32"} &&
          sets(modified)[0]["rules"][1]["ip_cidr"] == json{"2001:db8::/32"} &&
          sets(modified)[0]["rules"][2]["rules"][0]["ip_cidr"] == json{"198.51.100.0/24"} &&
          std::ranges::count_if(modified.fidelity, [](const auto& note) {
              return note.scope == singbox::FidelityScope::Rule && note.level == singbox::Fidelity::Approx &&
                     note.subject.find("no-resolve") != std::string::npos;
          }) == 3,
          "classical provider preserves IPv4/IPv6 and nested no-resolve matches with approximation ledger");
    singbox::CompileOptions inlineModifiers;
    inlineModifiers.profileYaml = "rule-providers:\n  sample:\n    type: inline\n    behavior: classical\n"
        "    payload: ['IP-CIDR,192.0.2.28/32,no-resolve']\nrules: ['RULE-SET,sample,DIRECT','MATCH,DIRECT']\n";
    const auto inlineModified = singbox::compileConfig(inlineModifiers);
    check(inlineModified.error.empty() && sets(inlineModified).size() == 1 &&
          sets(inlineModified)[0]["rules"][0]["ip_cidr"] == json{"192.0.2.28/32"},
          "inline classical provider shares no-resolve conversion");
    auto httpModifiers = inlineModifiers;
    httpModifiers.profileYaml = "rule-providers:\n  sample: {type: http, behavior: classical, format: text, url: https://example.test/rules}\n"
        "rules: ['RULE-SET,sample,DIRECT','MATCH,DIRECT']\n";
    const auto discovered = singbox::compileConfig(httpModifiers);
    check(discovered.httpRuleProviders.size() == 1, "HTTP classical modifier fixture discovers resource");
    if (!discovered.httpRuleProviders.empty()) {
        const auto& resource = discovered.httpRuleProviders[0];
        std::string reason;
        check(singbox::ValidateHttpRuleProvider(resource, modifiers, reason),
              "HTTP staging validator accepts complete classical no-resolve payload");
        httpModifiers.ruleProviderContents[resource.cacheKey] = modifiers;
        const auto httpModified = singbox::compileConfig(httpModifiers);
        check(httpModified.error.empty() && sets(httpModified).size() == 1 &&
              sets(httpModified)[0]["rules"].size() == 3,
              "HTTP candidate compiles all staged no-resolve entries");
        check(!singbox::ValidateHttpRuleProvider(resource,
              "DOMAIN,valid.test\nIP-CIDR,192.0.2.0/24,unknown\n", reason) &&
              reason.find("payload[1]") != std::string::npos,
              "HTTP unknown modifier rejects whole payload with position");
    }
    write("rules/domain.yaml", "payload: [updated.test]\n");
    const auto updated = compile("rules/domain.yaml");
    check(updated.error.empty() && sets(updated)[0]["rules"][0]["domain"] == json{"updated.test"} &&
          domainSets[0]["rules"][0]["domain"] == json{"example.test"},
          "recompilation reads new file while previous candidate snapshot stays intact");
    const auto rejected = [&](const auto& result, std::string_view message) {
        check(!result.error.empty() && result.json.empty() && !result.fidelity.empty() &&
              result.ruleSetResources.empty() && std::ranges::any_of(result.fidelity, [](const auto& note) {
                  return note.level == singbox::Fidelity::Unsupported;
              }), message);
    };
    rejected(compile("missing.yaml"), "missing declared cn file fails candidate without substituting China alias");
    rejected(compile("../outside.yaml"), "file parent traversal rejected");
    rejected(compile((directory.path / "rules/domain.yaml").string()), "absolute provider path rejected");
    rejected(compile("rules"), "directory provider rejected");
    rejected(compile("rules/domain.yaml", "domain", "mrs"), "file MRS format rejected");
    for (const auto* format : {"{bad: yaml}", "[yaml]", "null", "''", "'  '"})
        rejected(compile("rules/domain.yaml", "domain", format), "file explicit malformed format cannot become default YAML");
    rejected(compile("rules/domain.yaml", "domain", "yaml", "    url: https://example.test/rules\n"),
             "file unmapped field rejects entire candidate");
    rejected(compile("rules/domain.yaml", "domain", "yaml", "    path: missing.yaml\n"),
             "duplicate file field cannot silently pick a path");
    for (const bool fileFirst : {false, true}) {
        singbox::CompileOptions duplicate;
        duplicate.ruleProviderDir = directory.path.string();
        const std::string file = "  cn: {type: file, path: rules/domain.yaml, behavior: domain}\n";
        const std::string inlineSet = "  cn: {type: inline, behavior: domain, payload: [inline.test]}\n";
        duplicate.profileYaml = "rule-providers:\n" + (fileFirst ? file + inlineSet : inlineSet + file) +
            "rules:\n  - RULE-SET,cn,DIRECT\n  - MATCH,DIRECT\n";
        rejected(singbox::compileConfig(duplicate), "duplicate file/inline name fails candidate regardless of declaration order");
    }
    write("rules/invalid.yaml", "payload: [valid.test, 'part*.unsupported.test']\n");
    const auto invalid = compile("rules/invalid.yaml");
    rejected(invalid, "invalid payload child never commits valid prefix");
    check(invalid.error.find("payload[1]") != std::string::npos, "file failure carries payload position");
    std::ifstream retained(directory.path / "rules/invalid.yaml");
    check(std::string(std::istreambuf_iterator<char>(retained), {}) ==
          "payload: [valid.test, 'part*.unsupported.test']\n", "failed compilation does not modify source file");
    for (const auto& content : {"payload: [valid.test]\nunknown: x\n", "payload: [a.test]\npayload: [b.test]\n",
                               "payload: [a.test]\n---\npayload: [b.test]\n", "payload: [\n", "payload: scalar\n"}) {
        write("rules/bad.yaml", content);
        rejected(compile("rules/bad.yaml"), "file schema/duplicate/multiple-document/parse failures reject candidate");
    }
    write("rules/invalid.txt", "DOMAIN,valid.test\nGEOIP,CN\n");
    rejected(compile("rules/invalid.txt", "classical", "text"), "file HeadlessRule rejects route-only GEO fields atomically");
    for (const auto* invalidModifier : {"unknown", "no-resolve,unknown", "no-resolve,", "no-resolve,no-resolve"}) {
        write("rules/modifiers.txt", std::format("DOMAIN,valid.test\nIP-CIDR,192.0.2.0/24,{}\n", invalidModifier));
        const auto rejectedModifier = compile("rules/modifiers.txt", "classical", "text");
        rejected(rejectedModifier, "unknown/extra classical modifiers reject entire file candidate");
        check(rejectedModifier.error.find("payload[1]") != std::string::npos,
              "classical modifier rejection retains failing item position");
    }
    write("rules/nul.txt", std::string("valid.test\n\0bad", 15));
    rejected(compile("rules/nul.txt", "domain", "text"), "binary NUL file rejected");
    write("rules/large.txt", std::string(8 * 1024 * 1024 + 1, 'x'));
    rejected(compile("rules/large.txt", "domain", "text"), "file read bounded to 8 MiB");
#ifndef _WIN32
    // Windows fixture creation may require privileges; native Unix runners
    // cover both outside-root and allowed inside-root symlink resolution.
    std::ofstream(outside.path / "external.yaml") << "payload: [outside.test]\n";
    std::filesystem::create_symlink(outside.path / "external.yaml", directory.path / "escape.yaml");
    rejected(compile("escape.yaml"), "file symlink escape rejected");
    std::filesystem::create_symlink(directory.path / "rules/domain.yaml", directory.path / "inside.yaml");
    check(compile("inside.yaml").error.empty(), "inside-root symlink accepted");
#endif
    singbox::CompileOptions unset;
    unset.profileYaml = "rule-providers:\n  cn: {type: file, path: rules/domain.yaml, behavior: domain}\n";
    rejected(singbox::compileConfig(unset), "no implicit cwd fallback when file root absent");
}

void testDnsWildcardPolicies() {
    const std::vector<std::pair<std::string, int>> policies = {
        {"+.example.test", 10}, {".example.test", 11}, {"*.example.test", 12},
        {"fixed.example.test", 13}, {"*.deep.example.test", 14}, {".deep.example.test", 15},
        {"left.*.example.test", 16}, {"*.a.example.test", 17}, {".a.example.test", 18},
        {"deep.*.test", 19}, {".a.test", 20}, {"*.*.example.test", 21}, {"*", 22},
        {"+.scope.*.test", 23}, {".tail.*.test", 24},
    };
    const std::vector<std::pair<std::string, int>> cases = {
        {"example.test", 10}, {"child.example.test", 12}, {"fixed.example.test", 13},
        {"child.fixed.example.test", 21}, {"a.deep.example.test", 14}, {"x.a.deep.example.test", 15},
        {"deep.example.test", 12}, {"left.a.example.test", 17}, {"left.b.example.test", 16},
        {"other.b.example.test", 21}, {"other.a.example.test", 17}, {"x.other.a.example.test", 18},
        {"deep.a.test", 20}, {"localhost", 22}, {"x.localhost", 1},
        {"notexample.test", 1}, {"example.test.evil", 1}, {"UPPER.EXAMPLE.TEST.", 12},
        {"scope.b.test", 23}, {"x.scope.b.test", 23}, {"scope.b.c.test", 1},
        {"tail.b.test", 1}, {"x.tail.b.test", 24}, {"x.y.tail.b.test", 24}, {"x.tail.b.c.test", 1},
        {"_node.example.test", 12}, {"left._node.example.test", 16}, {"_service", 22},
        {"a..example.test", 1},
    };
    const auto fixture = [&](bool dedicated, bool reverse) {
        auto ordered = policies;
        if (reverse) std::reverse(ordered.begin() + 2, ordered.end()); // Preserve intentional +./. overwrite order.
        std::string input = "dns:\n  nameserver: [192.0.2.1]\n  nameserver-policy:\n";
        for (const auto& [pattern, index] : ordered)
            input += std::format("    '{}': 192.0.2.{}\n", pattern, index);
        if (dedicated) {
            input += "  proxy-server-nameserver: [198.51.100.1]\n  proxy-server-nameserver-policy:\n";
            for (const auto& [pattern, index] : ordered)
                input += std::format("    '{}': 198.51.100.{}\n", pattern, index);
        }
        input += "proxies:\n";
        for (std::size_t i = 0; i < cases.size(); ++i)
            input += std::format("  - {{name: node-{}, type: socks5, server: '{}', port: 1080}}\n", i, cases[i].first);
        return input + "rules: ['MATCH,DIRECT']\n";
    };
    for (const auto& [dedicated, reverse] : std::vector<std::pair<bool, bool>>{
            {false, false}, {true, false}, {false, true}, {true, true}}) {
        singbox::CompileOptions options;
        options.profileYaml = fixture(dedicated, reverse);
        const auto result = singbox::compileConfig(options);
        check(result.error.empty() && std::ranges::none_of(result.fidelity, [](const auto& note) {
            return note.level != singbox::Fidelity::Exact;
        }), "whole-label wildcard DNS policies preserve semantics without approximation");
        const auto config = json::parse(result.json);
        std::map<std::string, std::string> servers;
        for (const auto& server : config["dns"]["servers"]) servers.emplace(server["tag"], server.value("server", ""));
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto node = std::ranges::find_if(config["outbounds"], [&](const auto& item) {
                return item.value("tag", "") == std::format("node-{}", i);
            });
            check(node != config["outbounds"].end() &&
                  servers.at((*node)["domain_resolver"]["server"]) ==
                      std::format("{}{}", dedicated ? "198.51.100." : "192.0.2.", cases[i].second),
                  std::format("DNS trie priority and boundaries for node {} (dedicated={})", cases[i].first, dedicated));
        }
        check(std::ranges::all_of(config["dns"]["rules"], [&](const auto& rule) {
            return servers.at(rule["server"]).starts_with("192.0.2.") &&
                   !rule.contains("labels") && !rule.contains("priority");
        }), "node-specific wildcard policy never leaks into ordinary DNS rules or emits private matcher metadata");
    }
    for (const auto* invalid : {"part*.test", "*part.test", "part+word.test", "part.+.test", "**.test",
            "..test", "+.", "foo..test", "foo..", "foo$.test", "geosite:cn", "rule-set:cn", "中文.test", "a,b.test"}) {
        singbox::CompileOptions options;
        options.profileYaml = std::format("dns:\n  nameserver: [192.0.2.1]\n  nameserver-policy:\n"
            "    '{}': 192.0.2.2\nproxies:\n  - {{name: kept, type: socks5, server: a.test, port: 1080}}\n"
            "rules: ['MATCH,DIRECT']\n", invalid);
        const auto result = singbox::compileConfig(options);
        const auto config = json::parse(result.json);
        check(result.error.empty() && config["dns"]["servers"].size() == 2 &&
              config["dns"].value("rules", json::array()).empty() &&
              std::ranges::any_of(result.fidelity, [](const auto& note) {
                  return note.scope == singbox::FidelityScope::Dns && note.level == singbox::Fidelity::Unsupported;
              }), "invalid/unmapped DNS patterns reject entire entry before creating resolver or widening match");
    }
    singbox::CompileOptions overwrite;
    overwrite.profileYaml = R"yaml(dns:
  nameserver: [192.0.2.1]
  nameserver-policy:
    '.overwritten.test': 192.0.2.2
    '+.overwritten.test': 192.0.2.3
    'OVERWRITTEN.test': 192.0.2.4
proxies:
  - {name: root, type: socks5, server: overwritten.test, port: 1080}
  - {name: child, type: socks5, server: child.overwritten.test, port: 1080}
rules: ['MATCH,DIRECT']
)yaml";
    const auto overwritten = singbox::compileConfig(overwrite);
    const auto config = json::parse(overwritten.json);
    check(overwritten.error.empty() && config["dns"]["rules"].size() == 2 &&
          config["outbounds"][1]["domain_resolver"]["server"] == "dns-3" &&
          config["outbounds"][2]["domain_resolver"]["server"] == "dns-2" &&
          std::ranges::all_of(overwritten.fidelity, [](const auto& note) { return note.level == singbox::Fidelity::Exact; }),
          "later +. replaces root/subdomain independently, later exact replaces only root, canonical overlap recorded exact");
    singbox::CompileOptions cycle;
    cycle.profileYaml = "dns:\n  nameserver: [192.0.2.1]\n  nameserver-policy:\n"
        "    '*.nodes.test': 'https://192.0.2.2#node'\nproxies:\n"
        "  - {name: node, type: socks5, server: a.nodes.test, port: 1080}\nrules: ['MATCH,DIRECT']\n";
    const auto cyclic = singbox::compileConfig(cycle);
    check(!cyclic.error.empty() && cyclic.json.empty() && !cyclic.fidelity.empty(),
          "wildcard-selected node resolver participates in DNS/detour cycle validation without direct fallback");
}

void testDnsCertificateParameters() {
    const auto compile = [](const std::string& address) {
        singbox::CompileOptions options;
        options.profileYaml = "dns:\n  nameserver: ['" + address +
            "']\nrules:\n  - MATCH,DIRECT\n";
        return singbox::compileConfig(options);
    };
    for (const auto* scheme : {"tls", "https", "quic"}) {
        for (const auto* flag : {"true", "false"}) {
            const auto result = compile(std::string(scheme) + "://dns.example.test#skip-cert-verify=" + flag);
            check(result.error.empty() && result.fidelity.empty(), "DNS TLS certificate flag maps without semantic downgrade");
            const auto servers = json::parse(result.json)["dns"]["servers"];
            check(servers.size() == 2 && servers[1]["tls"]["enabled"] == true &&
                  servers[1]["tls"]["insecure"] == (std::string_view(flag) == "true") &&
                  !servers[1]["tls"].contains("server_name") && servers[1]["server"] == "dns.example.test",
                  "explicit flag affects only endpoint verification, not server identity/SNI");
        }
        const auto secure = json::parse(compile(std::string(scheme) + "://dns.example.test").json);
        check(!secure["dns"]["servers"][1].contains("tls"), "omitted DNS certificate flag preserves kernel trust defaults");
    }
    const auto combined = compile("https://127.0.0.1:9443/custom-query#DIRECT&skip-cert-verify=true&h3=true");
    const auto server = json::parse(combined.json)["dns"]["servers"][1];
    check(combined.fidelity.empty() && server["type"] == "h3" && server["detour"] == "DIRECT" &&
          server["server_port"] == 9443 && server["path"] == "/custom-query" && server["tls"]["insecure"] == true,
          "TLS flag composes with H3, explicit port/path and compiled detour");
    check(json::parse(compile("https://127.0.0.1#h3=false&skip-cert-verify=false").json)
              ["dns"]["servers"][1]["type"] == "https", "explicit h3 false remains ordinary verified DoH");
    for (const auto* address : {
        "tls://127.0.0.1#skip-cert-verify=yes", "https://127.0.0.1#skip-cert-verify=TRUE",
        "quic://127.0.0.1#skip-cert-verify=1", "https://127.0.0.1#skip-cert-verify=",
        "https://127.0.0.1#skip-cert-verify=true&skip-cert-verify=false",
        "tls://127.0.0.1#skip-cert-verify=false&skip-cert-verify=false",
        "https://127.0.0.1#skip-cert-verify=true&name-cert-verify=other.test",
        "https://127.0.0.1#skip-cert-verify=true&ecs=192.0.2.1/24",
        "https://127.0.0.1#skip-cert-verify=true&",
        "https://127.0.0.1# skip-cert-verify=true", "https://127.0.0.1#skip-cert-verify=true &DIRECT",
        "udp://127.0.0.1#skip-cert-verify=true", "tcp://127.0.0.1#skip-cert-verify=false",
        "tls://127.0.0.1#h3=false", "quic://127.0.0.1#h3=true",
        "https://127.0.0.1#skip-cert-verify=true&missing-outbound"}) {
        const auto rejected = compile(address);
        check(rejected.error.empty() && json::parse(rejected.json)["dns"]["servers"].size() == 1 &&
              std::ranges::any_of(rejected.fidelity, [](const auto& note) {
                  return note.scope == singbox::FidelityScope::Dns && note.level == singbox::Fidelity::Unsupported;
              }), "invalid/duplicate/unknown/misplaced DNS parameter rejects entire server and records loss");
    }
    singbox::CompileOptions scoped;
    scoped.mainConnectionId = "certificate-source";
    scoped.profileYaml = R"yaml(dns:
  default-nameserver: ['tls://192.0.2.1#skip-cert-verify=false']
  nameserver: ['https://resolver.test/custom-query#skip-cert-verify=true']
  nameserver-policy: {exact.test: 'tls://192.0.2.2#skip-cert-verify=false'}
  proxy-server-nameserver: ['quic://192.0.2.3#skip-cert-verify=true']
proxies:
  - {name: node, type: socks5, server: node.test, port: 1080}
rules:
  - MATCH,DIRECT
)yaml";
    const auto scopedResult = singbox::compileConfig(scoped);
    check(scopedResult.error.empty() && scopedResult.fidelity.empty(), "TLS flags compose with DNS bootstrap/policy/node resolvers and source namespace");
    const auto config = json::parse(scopedResult.json);
    const auto& servers = config["dns"]["servers"];
    check(servers.size() == 5 && servers[1]["tls"]["insecure"] == false &&
          servers[2]["tls"]["insecure"] == true && servers[3]["tls"]["insecure"] == false &&
          servers[4]["tls"]["insecure"] == true && !servers[0].contains("tls"),
          "certificate relaxation remains per endpoint, not inherited by bootstrap/local/other servers");
    check(servers[2]["domain_resolver"]["server"] == servers[1]["tag"] &&
          config["dns"]["rules"][0]["server"] == servers[3]["tag"],
          "namespace preserves bootstrap and policy references with TLS options");
}

void testClashHosts() {
    singbox::CompileOptions base;
    base.profileYaml = R"yaml(
hosts:
  Node.Test.: [192.0.2.1, '2001:db8::1']
  resolver.test: 192.0.2.53
  only6.test: '2001:db8::6'
dns:
  enable: true
  ipv6: true
  use-hosts: true
  nameserver: [https://resolver.test/dns-query]
  proxy-server-nameserver: [9.9.9.9]
  nameserver-policy: {node.test: 8.8.8.8}
proxies:
  - {name: mapped, type: trojan, server: node.test, port: 443, password: test-only}
  - {name: ordinary, type: socks5, server: ordinary.test, port: 1080}
rules: ["MATCH,DIRECT"]
)yaml";
    const auto result = singbox::compileConfig(base);
    check(result.error.empty() && !result.json.empty(), "exact hosts fixture compiles");
    const auto config = json::parse(result.json);
    const auto& dnsRules = config["dns"]["rules"];
    const auto hostsServer = std::ranges::find_if(config["dns"]["servers"], [](const auto& server) {
        return server.value("type", "") == "hosts";
    });
    check(hostsServer != config["dns"]["servers"].end() &&
          (*hostsServer)["predefined"]["node.test"] == json::array({"192.0.2.1", "2001:db8::1"}) &&
          !hostsServer->contains("path"), "hosts preserves full IP list and canonical exact names, no external file paths");
    check(dnsRules[0]["server"] == "clash-hosts" && dnsRules[0]["query_type"] == json::array({"A", "AAAA"}) &&
          dnsRules[0]["rewrite_ttl"] == 10 && dnsRules[1]["server"] != "clash-hosts",
          "hosts A/AAAA reply precedes nameserver policy, preserving normal non-address query routing");
    for (const auto& server : config["dns"]["servers"])
        if (server.value("server", "") == "resolver.test") check(server["domain_resolver"]["server"] == "clash-hosts",
            "mapped DNS endpoint uses hosts instead of recursive bootstrap");
    for (const auto& out : config["outbounds"]) {
        if (out.value("tag", "") == "mapped") check(out["domain_resolver"]["server"] == "clash-hosts" &&
            out["server"] == "node.test" && out["tls"]["enabled"] == true,
            "node uses hosts ahead of proxy resolver while retaining original server/TLS identity");
        if (out.value("tag", "") == "ordinary") check(out["domain_resolver"]["server"] != "clash-hosts",
            "unmapped node keeps its declared DNS resolver");
    }
    check(config["route"]["rules"][2]["server"] == "clash-hosts" &&
          config["route"]["rules"][3]["action"] == "resolve" && config["route"]["rules"][3]["invert"] == true,
          "main hosts resolution is a pre-action before normal resolve and product policy rules");
    check(std::ranges::any_of(result.fidelity, [](const auto& note) {
        return note.scope == singbox::FidelityScope::Dns && note.subject == "hosts" && note.level == singbox::Fidelity::Approx;
    }), "hosts native cache/selection differences enter fidelity ledger");
    for (const auto flag : {"use-hosts: false", "use-hosts: no", "use-hosts: 0", "enable: false"}) {
        auto disabled = base;
        const std::string field = std::string_view(flag).starts_with("enable") ? "enable: true" : "use-hosts: true";
        disabled.profileYaml.replace(disabled.profileYaml.find(field), field.size(), flag);
        const auto noReply = json::parse(singbox::compileConfig(disabled).json);
        check(std::ranges::none_of(noReply["dns"].value("rules", json::array()), [](const auto& rule) {
            return rule.value("server", "") == "clash-hosts";
        }) && noReply["route"]["rules"][2]["server"] == "clash-hosts",
            "dns use-hosts/enable false disables replies, not global hosts connection resolution");
    }
    for (const char* bad : {"'*.wild.test': 192.0.2.2", "'+.wild.test': 192.0.2.2",
        "bad.test: alias.test", "bad.test: lan", "bad.test: []", "bad.test: null",
        "bad.test: [192.0.2.2, invalid]", "bad.test: [192.0.2.2, {nested: value}]",
        "bad.test: 192.0.2.0/24", "bad.test: '::ffff:192.0.2.2'", "bad.test: '192.0.2.999'",
        "bad.test: 'fe80::1%eth0'", "' bad.test': 192.0.2.2"}) {
        auto invalid = base;
        invalid.profileYaml.insert(invalid.profileYaml.find("dns:"), std::string("  ") + bad + "\n");
        const auto rejected = singbox::compileConfig(invalid);
        const auto parsed = json::parse(rejected.json);
        for (const auto& server : parsed["dns"]["servers"]) if (server.value("type", "") == "hosts")
            check(server["predefined"].size() == 3 && !server["predefined"].contains("bad.test"),
                "unsupported hosts entry rejects entire IP list without discarding independent valid mappings");
        check(std::ranges::any_of(rejected.fidelity, [](const auto& note) { return note.level == singbox::Fidelity::Unsupported; }),
              "every failed hosts entry is explicitly accounted");
    }
    auto duplicate = base;
    duplicate.profileYaml.insert(duplicate.profileYaml.find("dns:"), "  node.test: 192.0.2.9\n");
    const auto repeated = json::parse(singbox::compileConfig(duplicate).json);
    for (const auto& server : repeated["dns"]["servers"]) if (server.value("type", "") == "hosts")
        check(!server["predefined"].contains("node.test"), "canonical duplicate hosts names all rejected, never last-wins");
    auto malformedFlag = base;
    malformedFlag.profileYaml.replace(malformedFlag.profileYaml.find("use-hosts: true"), 15, "use-hosts: [true]");
    const auto flagResult = singbox::compileConfig(malformedFlag);
    check(std::ranges::any_of(flagResult.fidelity, [](const auto& note) {
        return note.subject == "dns.use-hosts" && note.level == singbox::Fidelity::Unsupported;
    }), "malformed use-hosts flag explicitly rejected without guessing true");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    auto sources = base;
    sources.mainConnectionId = "main";
    sources.auxiliarySources = {{"secondary", "secondary", base.profileYaml}};
    vpn::RouteRule rule;
    rule.id = "hosts-secondary"; rule.connectionId = "secondary";
    rule.targetKind = vpn::TargetKind::Node; rule.targetObject = "mapped";
    rule.match = vpn::MatchKind::ExactDomain; rule.pattern = "selected.test";
    sources.globalRules = {rule};
    const auto combined = singbox::compileConfig(sources);
    check(combined.error.empty(), "secondary source with hosts resolver compiles with isolated DNS dependency");
    const auto multi = json::parse(combined.json);
    int hostsCount = 0;
    for (const auto& server : multi["dns"]["servers"]) hostsCount += server.value("type", "") == "hosts";
    check(hostsCount == 2 && std::ranges::none_of(multi["dns"]["rules"], [](const auto& item) {
        return item.value("server", "").starts_with("cf_7365636f6e64617279_d_");
    }), "secondary hosts DNS dependency retained without injecting secondary DNS response policy");
    check(multi["route"]["rules"][2]["server"] == multi["dns"]["rules"][0]["server"],
          "main hosts pre-action follows actual namespaced DNS tag after orchestration");
#endif
}

std::string mrsFrame(std::string_view decoded) {
    std::string compressed(ZSTD_compressBound(decoded.size()), '\0');
    const auto size = ZSTD_compress(compressed.data(), compressed.size(), decoded.data(), decoded.size(), 1);
    check(!ZSTD_isError(size), "MRS fixture compression succeeds");
    compressed.resize(size); return compressed;
}

void testMrsRuleProviders() {
    const auto integer = [](std::string& body, std::uint64_t value) {
        for (unsigned i = 8; i-- > 0;) body.push_back(static_cast<char>(value >> (i * 8)));
    };
    const auto header = [&](unsigned behavior) {
        std::string body = "MRS"; body.push_back(1); body.push_back(behavior);
        integer(body, 3); integer(body, 0); body.push_back(1); return body;
    };
    // Independent BFS trie fixture builder, with explicit exact, subdomain-only,
    // root+subdomain and single-label wildcard paths.
    struct Node { bool leaf = false; std::map<char, unsigned> children; };
    std::vector<std::string> keys{"example.test", "+.only.test", "root.test", "+.root.test", "*.wild.test"};
    for (unsigned i = 0; i < 4096; ++i) keys.push_back("entry" + std::to_string(i) + ".bulk.test");
    std::vector<Node> tree(1);
    for (std::string key : keys) {
        std::reverse(key.begin(), key.end()); unsigned node = 0;
        for (const char c : key) {
            auto found = tree[node].children.find(c);
            if (found == tree[node].children.end()) {
                const unsigned next = tree.size(); tree.emplace_back();
                tree[node].children[c] = next; node = next;
            } else node = found->second;
        }
        tree[node].leaf = true;
    }
    std::vector<std::uint64_t> leaves, edges;
    const auto set = [](auto& words, std::size_t index) {
        if (words.size() <= index / 64) words.resize(index / 64 + 1);
        words[index / 64] |= std::uint64_t{1} << (index % 64);
    };
    std::vector<unsigned> queue{0}; std::size_t position = 0;
    std::string labels;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto node = queue[i]; if (tree[node].leaf) set(leaves, i);
        for (const auto& [c, child] : tree[node].children) { labels.push_back(c); queue.push_back(child); ++position; }
        set(edges, position++);
    }
    std::string domain = header(0);
    for (const auto& words : {leaves, edges}) { integer(domain, words.size()); for (const auto word : words) integer(domain, word); }
    integer(domain, labels.size()); domain += labels;
    singbox::CompileOptions base;
    base.profileYaml = "rule-providers:\n  test: {type: http, behavior: domain, format: mrs, url: https://example.test/test.mrs}\n"
        "rules: [\"RULE-SET,test,DIRECT\", \"MATCH,DIRECT\"]";
    const auto resource = singbox::compileConfig(base).httpRuleProviders[0];
    const auto compressed = mrsFrame(domain);
    std::string error;
    check(singbox::ValidateHttpRuleProvider(resource, compressed, error), "MRS domain dictionary validates");
    base.ruleProviderContents[resource.cacheKey] = compressed;
    const auto result = singbox::compileConfig(base);
    check(result.error.empty() && !result.json.empty(), "MRS HTTP domain lowers complete trie");
    if (!result.json.empty()) {
        const auto rules = json::parse(result.json)["route"]["rule_set"][0]["rules"];
        check(rules.size() == 2 && rules.dump().find("domain_regex") != std::string::npos &&
              rules.dump().find("only") != std::string::npos && rules.dump().find("wild") != std::string::npos,
              "MRS compacts exact and strict suffix/wildcard conditions into separate OR branches");
        const auto& exact = rules[0]["domain"];
        const auto exactValues = exact.get<std::vector<std::string>>();
        const std::set<std::string> exactSet(exactValues.begin(), exactValues.end());
        check(exact.size() == 4098 && std::ranges::all_of(keys, [&](const auto& key) {
            return !key.ends_with(".bulk.test") || exactSet.contains(key);
        }), "large MRS compaction preserves every exact entry and keeps native branch count bounded");
        check(std::ranges::find(exact, "only.test") == exact.end() &&
              std::ranges::find(exact, "root.test") != exact.end(), "subdomain-only never gains a root match");
    }
    auto badMagic = domain; badMagic[0] = 'X';
    auto badVersion = domain; badVersion[3] = 2;
    auto badBehavior = domain; badBehavior[4] = 1;
    auto badExtra = domain; badExtra[20] = 1;
    for (const auto& invalid : {mrsFrame(badMagic), mrsFrame(badVersion), mrsFrame(badBehavior),
                               mrsFrame(badExtra), mrsFrame(domain + "extra"),
                               compressed.substr(0, compressed.size() - 1), compressed + "extra",
                               mrsFrame(std::string(33 * 1024 * 1024, 'x'))})
        check(!singbox::ValidateHttpRuleProvider(resource, invalid, error),
              "MRS wrong header/behavior/extensions, truncation, trailing data and expansion bomb reject whole body");
    std::string ip = header(1); integer(ip, 2);
    for (unsigned end : {1u, 6u}) {
        ip.append(10, '\0'); ip.append(2, static_cast<char>(255));
        for (unsigned b : {192u, 0u, 2u, end}) ip.push_back(b);
    }
    // Go netip sorts IPv4 before IPv6, even when IPv6 bytes (::) sort lower.
    const std::array<unsigned char,16> v6{};
    for (unsigned end : {0u, 3u}) { auto a = v6; a.back() = end; for (auto b : a) ip.push_back(b); }
    auto ipOptions = base;
    ipOptions.ruleProviderContents.clear();
    ipOptions.profileYaml = "rule-providers:\n  test: {type: http, behavior: ipcidr, format: mrs, url: https://example.test/ip.mrs}\n"
        "rules: [\"RULE-SET,test,DIRECT\", \"MATCH,DIRECT\"]";
    const auto ipResource = singbox::compileConfig(ipOptions).httpRuleProviders[0];
    check(singbox::ValidateHttpRuleProvider(ipResource, mrsFrame(ip), error), "MRS IPv4-mapped and IPv6 ranges validate");
    ipOptions.ruleProviderContents[ipResource.cacheKey] = mrsFrame(ip);
    const auto ipResult = singbox::compileConfig(ipOptions);
    check(ipResult.error.empty() && !ipResult.json.empty(), "MRS ranges compile without invented range fields");
    if (!ipResult.json.empty()) {
        const auto cidrs = json::parse(ipResult.json)["route"]["rule_set"][0]["rules"][0]["ip_cidr"];
        check(cidrs.size() == 5 && cidrs[0] == "192.0.2.1/32" && cidrs[1] == "192.0.2.2/31" &&
              cidrs[2] == "192.0.2.4/31" && cidrs[3] == "192.0.2.6/32",
              "range decomposition retains boundaries instead of widening to containing CIDR");
    }
    auto reversed = ip; reversed[37] = 7;
    check(!singbox::ValidateHttpRuleProvider(ipResource, mrsFrame(reversed), error), "reversed MRS IP range rejects whole body");
    const RuleSetTestDirectory directory;
    std::ofstream(directory.path / "test.mrs", std::ios::binary) << compressed;
    auto file = base; file.ruleProviderContents.clear(); file.ruleProviderDir = directory.path.string();
    file.profileYaml = "rule-providers:\n  test: {type: file, behavior: domain, format: mrs, path: test.mrs}\n"
        "rules: [\"RULE-SET,test,DIRECT\", \"MATCH,DIRECT\"]";
    check(singbox::compileConfig(file).error.empty(), "MRS file shares strict decoder and safe read-only root");
}

void testAsnRuleProviders() {
    const RuleSetTestDirectory directory;
    singbox::CompileOptions base;
    base.ruleProviderCacheDir = (directory.path / "cache").string();
    base.profileYaml = "rule-providers:\n  ai: {type: http, behavior: classical, url: https://example.test/ai.yaml}\n"
        "rules: [\"RULE-SET,ai,DIRECT\", \"MATCH,DIRECT\"]\n";
    const std::string body = "payload: [\"IP-ASN,14061,no-resolve\", \"NOT,((SRC-IP-ASN,14061))\"]\n";
    const std::string snapshot = R"({"asn":14061,"prefixes":{"ipv4":["192.0.2.0/24"],"ipv6":["2001:db8::/32"]}})";
    int fetches = 0, checks = 0, commits = 0;
    bool rejectCheck = false, badAsn = false;
    singbox::HttpRuleProviderResource asnResource;
    rule_provider_cache::Operations operations;
    operations.fetch = [&](const auto& resource, const auto& path, std::string&) {
        ++fetches;
        if (resource.asn) asnResource = resource;
        std::ofstream output(path);
        output << (resource.asn ? (badAsn ? "{}" : snapshot) : body);
        output.close(); return output.good();
    };
    operations.check = [&](const auto&, const auto& result, std::string& error) {
        ++checks;
        check(commits == 0, "ASN and provider replacements wait for complete candidate check");
        if (rejectCheck) { error = "kernel rejects fixture"; return false; }
        return result.error.empty() && !result.json.empty();
    };
    operations.commit = [&](const auto& source, const auto& destination, std::string&) {
        ++commits;
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        return true;
    };
    auto candidate = base;
    singbox::CompileResult result;
    std::string error;
    check(rule_provider_cache::Prepare(candidate, operations, result, error) &&
          fetches == 2 && checks == 1 && commits == 2 && candidate.ruleProviderContents.size() == 2,
          "HTTP classical payload discovers and prepares one shared ASN snapshot in another round");
    if (!result.json.empty()) {
        const auto config = json::parse(result.json);
        const auto& rules = config["route"]["rule_set"][0]["rules"];
        check(rules.size() == 2 && rules[0]["ip_cidr"] == json::array({"192.0.2.0/24", "2001:db8::/32"}) &&
              rules[1]["invert"] == true && rules[1]["rules"][0].contains("source_ip_cidr"),
              "ASN preserves both families and nested NOT/source match without invented fields");
        check(std::count_if(result.fidelity.begin(), result.fidelity.end(), [](const auto& note) {
                  return note.level == singbox::Fidelity::Approx && note.scope == singbox::FidelityScope::Rule;
              }) >= 3, "ASN snapshot and no-resolve differences enter fidelity ledger");
    }
    for (const auto* invalid : {
            R"({"asn":14062,"prefixes":{"ipv4":["192.0.2.0/24"],"ipv6":[]}})",
            R"({"asn":14061,"prefixes":{"ipv4":["192.0.2.0/24","bad"],"ipv6":[]}})",
            R"({"asn":14061,"prefixes":{"ipv4":[],"ipv6":["192.0.2.0/24"]}})",
            R"({"asn":14061,"prefixes":{"ipv4":[],"ipv6":[]}})",
            R"({"asn":14061,"prefixes":{"ipv4":["192.0.2.0/24"]}})"}) {
        check(!singbox::ValidateHttpRuleProvider(asnResource, invalid, error),
              "ASN wrong identity, malformed member, family mismatch or empty/incomplete union rejects whole snapshot");
    }
    singbox::CompileOptions direct = candidate;
    direct.profileYaml = "rules: [\"IP-ASN,14061,DIRECT,no-resolve\", \"SRC-IP-ASN,14061,DIRECT\", \"MATCH,DIRECT\"]";
    const auto directResult = singbox::compileConfig(direct);
    check(directResult.error.empty() && !directResult.json.empty(), "top-level ASN uses same prepared snapshot");
    direct.ruleProviderContents.clear(); direct.ruleProviderCacheDir.clear();
    const auto missing = singbox::compileConfig(direct);
    check(!missing.error.empty() && missing.json.empty() && missing.httpRuleProviders.size() == 1,
          "missing ASN cannot publish a config with silently omitted rules");
    auto inlineOptions = candidate;
    inlineOptions.profileYaml = "rule-providers:\n  ai: {type: inline, behavior: classical, payload: [\"IP-ASN,14061,no-resolve\"]}\n"
        "rules: [\"RULE-SET,ai,DIRECT\", \"MATCH,DIRECT\"]";
    check(singbox::compileConfig(inlineOptions).error.empty(), "inline ASN expands to headless ip_cidr");
    for (const auto* invalid : {"IP-ASN,0", "IP-ASN,-1", "IP-ASN,4294967296", "IP-ASN,14061,unknown",
                               "IP-ASN,14061,no-resolve,extra", "IP-ASN,AS14061"}) {
        const auto provider = singbox::compileConfig(base).httpRuleProviders[0];
        check(!singbox::ValidateHttpRuleProvider(provider, "payload: [\"" + std::string(invalid) + "\"]", error),
              "HTTP syntax validation rejects invalid ASN/modifiers before staging dependencies");
    }
    // Existing provider+ASN files survive a rejected replacement. Use expired
    // valid caches so both replacement downloads stage before kernel refusal.
    std::map<std::filesystem::path, std::string> oldImages;
    for (const auto& resource : result.httpRuleProviders) {
        const auto file = std::filesystem::path(base.ruleProviderCacheDir) / (resource.cacheKey + ".cache");
        std::ifstream input(file); oldImages[file] = std::string(std::istreambuf_iterator<char>(input), {});
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
    }
    auto refresh = base;
    refresh.profileYaml = "rule-providers:\n  ai: {type: http, behavior: classical, url: https://example.test/ai.yaml, interval: 1}\n"
        "rules: [\"RULE-SET,ai,DIRECT\", \"MATCH,DIRECT\"]";
    commits = checks = 0; rejectCheck = true;
    check(!rule_provider_cache::Prepare(refresh, operations, result, error) && commits == 0 && checks == 1 &&
          refresh.ruleProviderContents.empty(), "rejected full ASN candidate never publishes snapshots or replaces caches");
    for (const auto& [file, image] : oldImages) {
        std::ifstream input(file);
        check(std::string(std::istreambuf_iterator<char>(input), {}) == image, "rejected ASN refresh retains each old cache byte");
    }
    auto fresh = base; fresh.ruleProviderCacheDir = (directory.path / "bad-cache").string();
    badAsn = true; rejectCheck = false; commits = checks = 0;
    check(!rule_provider_cache::Prepare(fresh, operations, result, error) && commits == 0 && checks == 0 &&
          fresh.ruleProviderContents.empty(), "invalid ASN download rejects staged classical body before any commit");
}

void testHttpRuleProviders() {
    const RuleSetTestDirectory directory;
    const auto cache = directory.path / "cache";
    std::filesystem::create_directory(cache);
    singbox::CompileOptions base;
    base.ruleProviderDir = directory.path.string();
    base.ruleProviderCacheDir = cache.string();
    base.mainConnectionId = "http-main";
    base.profileYaml = "rule-providers:\n  cn: {type: http, behavior: domain, url: https://example.test/rules.yaml, interval: 1}\n"
        "rules:\n  - AND,((RULE-SET,cn),(NETWORK,TCP)),DIRECT\n  - MATCH,DIRECT\n";
    const auto missing = singbox::compileConfig(base);
    check(!missing.error.empty() && missing.json.empty() && missing.httpRuleProviders.size() == 1 &&
          !missing.fidelity.empty() && missing.ruleSetResources.empty(),
          "HTTP discovery returns owned manifest on cache miss without CN alias substitution/network IO");
    const auto resource = missing.httpRuleProviders[0];
    const auto file = cache / (resource.cacheKey + ".cache");
    std::string body = "payload: [first.test]\n", reason;
    int fetched = 0, committed = 0, checked = 0;
    bool offline = false, rejectNative = false;
    rule_provider_cache::Operations operations;
    operations.fetch = [&](const auto&, const auto& path, std::string& error) {
        ++fetched;
        if (offline) { error = "test offline"; return false; }
        std::ofstream output(path, std::ios::binary);
        output << body; output.close(); return output.good();
    };
    operations.check = [&](const auto&, const auto& result, std::string& error) {
        ++checked;
        if (rejectNative) { error = "test kernel rejects"; return false; }
        return result.error.empty() && !result.json.empty();
    };
    operations.commit = [&](const auto& source, const auto& destination, std::string&) {
        ++committed;
        // Fake replace adapter; production uses api::CommitFile on all OSes.
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        return true;
    };
    auto first = base;
    singbox::CompileResult prepared;
    check(rule_provider_cache::Prepare(first, operations, prepared, reason) && fetched == 1 &&
          committed == 1 && checked == 1 && prepared.fidelity.size() == 1 &&
          prepared.fidelity[0].level == singbox::Fidelity::Approx,
          "HTTP first load converts/checks before cache commit and reports snapshot approximation");
    check(singbox::ReadHttpRuleProviderCache(resource, cache, reason) == body &&
          first.ruleProviderContents[resource.cacheKey] == body,
          "HTTP committed cache and applied candidate contain same complete raw snapshot");
    const auto original = *singbox::ReadRuleProviderText(file, 9 * 1024 * 1024, reason);
    const auto expire = [&] {
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(10));
    };
    expire(); offline = true;
    auto cached = base;
    check(rule_provider_cache::Prepare(cached, operations, prepared, reason) &&
          cached.ruleProviderContents[resource.cacheKey] == body && committed == 1 &&
          std::ranges::any_of(prepared.warnings, [](const auto& warning) { return warning.find("沿用") != std::string::npos; }),
          "stale offline HTTP source keeps verified cache with runtime warning, no destructive repair");
    offline = false; body = "payload: [valid.test, 'part*.unsupported.test']\n";
    auto bad = base;
    check(rule_provider_cache::Prepare(bad, operations, prepared, reason) && committed == 1 &&
          *singbox::ReadRuleProviderText(file, 9 * 1024 * 1024, reason) == original,
          "invalid refreshed payload never replaces good cache or commits a valid prefix");
    body = "payload: [second.test]\n"; rejectNative = true;
    auto rejected = base;
    check(!rule_provider_cache::Prepare(rejected, operations, prepared, reason) && committed == 1 &&
          rejected.ruleProviderContents.empty() &&
          *singbox::ReadRuleProviderText(file, 9 * 1024 * 1024, reason) == original,
          "complete candidate/kernel rejection preserves old cache and caller options");
    rejectNative = false;
    auto second = base;
    check(rule_provider_cache::Prepare(second, operations, prepared, reason) && committed == 2,
          "valid refreshed HTTP snapshot can replace cache after complete candidate check");
    const int fetches = fetched;
    check(rule_provider_cache::Prepare(first, operations, prepared, reason) && fetched == fetches &&
          first.ruleProviderContents[resource.cacheKey] == "payload: [first.test]\n",
          "pinned prior plan remains old snapshot after global cache update, without redownload on rollback");
    rejectNative = true;
    const auto pinnedContents = first.ruleProviderContents;
    const int previousChecks = checked;
    check(!rule_provider_cache::Prepare(first, operations, prepared, reason) &&
          checked == previousChecks + 1 && fetched == fetches && committed == 2 &&
          first.ruleProviderContents == pinnedContents,
          "pinned snapshot fast path still checks complete candidate and preserves options/cache on rejection");
    rejectNative = false;
    for (const char* extra : {"header: {X-Test: sample}", "header: {X-Test: [one, two]}",
                             "header: {X-Test: []}", "header: {X-Test: [null]}",
                             "header: {X-Test: [sample], x-test: [duplicate]}",
                             "header: {Host: [other.test]}", "header: {Accept-Encoding: [gzip]}",
                             "header: {X-Test: [' leading ']}", "header: {X-Test: [中文]}",
                             "header: {'Bad Name': [sample]}", "header: {X-Test: [\"bad\\r\\nInjected: yes\"]}",
                             "proxy: selected", "format: unknown", "format: {bad: yaml}",
                             "format: [yaml]", "format: null", "format: ''", "format: '  '",
                             "interval: -1", "interval: 1.5", "size-limit: 8388609", "size-limit: invalid",
                             "path: '../escape'", "url: file:///tmp/rules", "url: https://sample:pass@example.test/rules"}) {
        auto unsupported = base;
        unsupported.profileYaml = "rule-providers:\n  cn:\n    type: http\n    behavior: domain\n";
        if (!std::string_view(extra).starts_with("url:")) unsupported.profileYaml += "    url: https://example.test/rules\n";
        unsupported.profileYaml += std::string("    ") + extra + "\nrules:\n  - RULE-SET,cn,DIRECT\n  - MATCH,DIRECT\n";
        const auto result = singbox::compileConfig(unsupported);
        check(!result.error.empty() && result.json.empty() && result.httpRuleProviders.empty() && !result.fidelity.empty(),
              "HTTP unsupported/invalid download semantics fail declaration before resource enqueue");
    }
    auto changedSource = base;
    changedSource.mainConnectionId = "other-source";
    const auto different = singbox::compileConfig(changedSource);
    check(different.httpRuleProviders[0].cacheKey != resource.cacheKey,
          "HTTP cache identity isolates stable source IDs rather than runtime tag/preview directory");
    auto limited = base;
    limited.ruleProviderContents.clear();
    limited.profileYaml = "rule-providers:\n  tiny: {type: http, behavior: domain, format: text, url: https://example.test/tiny, size-limit: 4, proxy: DIRECT}\n"
        "rules:\n  - RULE-SET,tiny,DIRECT\n  - MATCH,DIRECT\n";
    body = "valid.test\n";
    check(!rule_provider_cache::Prepare(limited, operations, prepared, reason) && committed == 2 &&
          limited.ruleProviderContents.empty(), "HTTP body over declared size-limit rejects without truncation or cache commit");
    auto multiple = base;
    multiple.profileYaml = "rule-providers:\n  good: {type: http, behavior: domain, url: https://example.test/good}\n"
        "  bad: {type: http, behavior: ipcidr, url: https://example.test/bad}\n"
        "rules:\n  - RULE-SET,good,DIRECT\n  - RULE-SET,bad,DIRECT\n  - MATCH,DIRECT\n";
    body = "payload: [valid.test]\n";
    check(!rule_provider_cache::Prepare(multiple, operations, prepared, reason) && committed == 2 &&
          multiple.ruleProviderContents.empty(), "one invalid HTTP provider prevents all staged candidate cache commits");
    auto seed = base;
    seed.profileYaml = "rule-providers:\n  seed: {type: http, behavior: domain, url: https://example.test/seed, path: seed.yaml}\n"
        "rules:\n  - RULE-SET,seed,DIRECT\n  - MATCH,DIRECT\n";
    std::ofstream(directory.path / "seed.yaml", std::ios::binary) << "payload: [seed.test]\n";
    const int beforeSeed = fetched;
    check(rule_provider_cache::Prepare(seed, operations, prepared, reason) && fetched == beforeSeed && committed == 3,
          "HTTP interval=0 uses valid read-only seed offline and stores app-owned cache");
    check(*singbox::ReadRuleProviderText(directory.path / "seed.yaml", 100, reason) == "payload: [seed.test]\n",
          "HTTP seed path is never rewritten as download cache");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "clash-flux-rule-provider-v2\nother identity\npayload: [wrong.test]\n";
    auto mismatch = base;
    check(!rule_provider_cache::Prepare(mismatch, operations, prepared, reason) && fetched == beforeSeed && committed == 3,
          "cache identity/future version mismatch refuses reuse and overwrite");
    auto exceptionOptions = base;
    exceptionOptions.mainConnectionId = "throwing-fetch";
    auto exceptions = operations;
    exceptions.fetch = [](const auto&, const auto&, std::string&) -> bool {
        throw std::runtime_error("test transfer exception");
    };
    check(!rule_provider_cache::Prepare(exceptionOptions, exceptions, prepared, reason) &&
          !reason.empty() && exceptionOptions.ruleProviderContents.empty() && committed == 3,
          "HTTP transfer exceptions return failure without escaping task/coroutine or touching cache/options");
    check(std::ranges::none_of(std::filesystem::directory_iterator(cache), [](const auto& entry) {
        return entry.path().filename().string().starts_with(".stage-");
    }), "HTTP exclusive stage directories cleaned on every success/failure path");

    auto withHeaders = base;
    withHeaders.profileYaml = "rule-providers:\n  cn: {type: http, behavior: domain, url: https://example.test/rules.yaml, "
        "header: {User-Agent: [provider-test/1], Authorization: [Bearer test-only], X-Token: [sample]}}\n"
        "rules:\n  - RULE-SET,cn,DIRECT\n  - MATCH,DIRECT\n";
    const auto headerManifest = singbox::compileConfig(withHeaders);
    check(headerManifest.httpRuleProviders.size() == 1, "valid single-value header map enters resource manifest");
    const auto headerResource = headerManifest.httpRuleProviders[0];
    check(headerResource.headers.at("authorization") == "Bearer test-only" &&
          headerResource.cacheKey != resource.cacheKey,
          "header values retained exactly and cache identity isolated from no-header downloads");
    auto headerOperations = operations;
    headerOperations.fetch = [&](const auto& request, const auto& path, std::string&) {
        check(request.headers == headerResource.headers, "task fetch receives exact compiler-owned headers");
        std::ofstream output(path, std::ios::binary); output << "payload: [authorized.test]\n"; output.close();
        return output.good();
    };
    check(rule_provider_cache::Prepare(withHeaders, headerOperations, prepared, reason) &&
          prepared.fidelity.size() == 2 && prepared.json.find("Bearer test-only") == std::string::npos,
          "header snapshot checked/committed without credentials in native config, redirect difference is accounted");
    auto otherHeader = withHeaders;
    auto& yaml = otherHeader.profileYaml;
    yaml.replace(yaml.find("Bearer test-only"), std::string("Bearer test-only").size(), "Bearer another-test");
    const auto otherManifest = singbox::compileConfig(otherHeader);
    check(!otherManifest.error.empty() && otherManifest.httpRuleProviders.size() == 1 &&
          otherManifest.httpRuleProviders[0].cacheKey != headerResource.cacheKey &&
          otherManifest.error.find("Bearer") == std::string::npos,
          "changed credentials cannot reuse old raw pins/cache and never appear in failure diagnostics");
    const auto authorizedCache = cache / (headerResource.cacheKey + ".cache");
    const auto authorizedImage = *singbox::ReadRuleProviderText(authorizedCache, 9 * 1024 * 1024, reason);
    const auto previousPins = otherHeader.ruleProviderContents;
    offline = true;
    check(!rule_provider_cache::Prepare(otherHeader, operations, prepared, reason) &&
          otherHeader.ruleProviderContents == previousPins &&
          *singbox::ReadRuleProviderText(authorizedCache, 9 * 1024 * 1024, reason) == authorizedImage &&
          !std::filesystem::exists(cache / (otherManifest.httpRuleProviders[0].cacheKey + ".cache")),
          "failed changed-credential download keeps old cache/options without using another header identity as fallback");
    offline = false;
    const int beforeHeaderRollback = fetched;
    check(rule_provider_cache::Prepare(withHeaders, operations, prepared, reason) &&
          fetched == beforeHeaderRollback && withHeaders.ruleProviderContents.at(headerResource.cacheKey) ==
          "payload: [authorized.test]\n", "previous authorized plan rolls back using its pinned raw snapshot without fetch");
    auto noHeader = base;
    noHeader.profileYaml.insert(noHeader.profileYaml.find("interval: 1"), "header: {}, ");
    check(singbox::compileConfig(noHeader).httpRuleProviders[0].cacheKey == resource.cacheKey,
          "empty header map preserves prior cache identity for upgrade compatibility");
    auto caseHeader = withHeaders;
    caseHeader.profileYaml.replace(caseHeader.profileYaml.find("User-Agent"), 10, "user-agent");
    check(singbox::compileConfig(caseHeader).httpRuleProviders[0].cacheKey == headerResource.cacheKey,
          "case-insensitive header names produce stable identities");
    auto oversized = base;
    oversized.profileYaml = "rule-providers:\n  cn: {type: http, behavior: domain, url: https://example.test/rules.yaml, header: {X-Token: ['" +
        std::string(16385, 's') + "']}}\nrules: [MATCH,DIRECT]\n";
    const auto tooLarge = singbox::compileConfig(oversized);
    check(!tooLarge.error.empty() && tooLarge.httpRuleProviders.empty() && tooLarge.error.size() < 1000,
          "oversized headers reject declaration without exposing raw header values");
}

void testGroupProxyExpansion() {
    const std::string nodes = R"YAML(proxies:
  - {name: z-node, type: socks5, server: 192.0.2.1, port: 1080}
  - {name: a-node, type: socks5, server: 192.0.2.2, port: 1080}
)YAML";
    const auto compile = [&](const std::string& fields, std::string source = {}) {
        singbox::CompileOptions options;
        options.profileYaml = (source.empty() ? nodes : source) +
            "proxy-groups:\n  - name: all\n    type: select\n" + fields +
            "  - {name: other, type: select, proxies: [DIRECT]}\nrules: ['MATCH,all']\n";
        return singbox::compileConfig(options);
    };
    const auto group = [](const auto& result) {
        if (result.json.empty()) return json::object();
        const auto config = json::parse(result.json);
        for (const auto& outbound : config["outbounds"])
            if (outbound.value("tag", "") == "all") return outbound;
        return json::object();
    };
    const auto exact = [](const auto& result) {
        return std::ranges::any_of(result.fidelity, [](const auto& note) {
            return note.scope == singbox::FidelityScope::Group && note.subject == "all" &&
                   note.level == singbox::Fidelity::Exact;
        });
    };
    for (const auto* value : {"true", "yes", "1"}) {
        const auto result = compile(std::format("    include-all-proxies: {}\n", value));
        check(result.error.empty() && group(result)["outbounds"] == json::array({"a-node", "z-node"}) && exact(result),
              "include-all-proxies sorts only source nodes, excluding other groups and managed DIRECT");
    }
    const auto explicitFirst = compile("    include-all-proxies: true\n    proxies: [other, z-node]\n"
                                       "    default-selected: a-node\n    filter: ''\n    use: []\n");
    check(explicitFirst.error.empty() && group(explicitFirst)["outbounds"] ==
          json::array({"other", "z-node", "a-node", "z-node"}) &&
          group(explicitFirst)["default"] == "a-node", "explicit members precede expansion; repeated members and selector default survive");
    for (const auto* value : {"false", "no", "0"}) {
        const auto result = compile(std::format("    include-all-proxies: {}\n    proxies: [DIRECT]\n", value));
        check(result.error.empty() && group(result)["outbounds"] == json::array({"DIRECT"}) && !exact(result),
              "false expansion flag preserves explicit members without a false expansion ledger");
    }
    for (const auto* field : {"include-all", "include-all-proxies", "include-all-providers"})
        for (const auto* invalid : {"null", "{}", "[]", "''", "maybe"}) {
            const auto result = compile(std::format("    {}: {}\n    proxies: [DIRECT]\n", field, invalid));
            check(!result.error.empty() && result.json.empty() && !result.fidelity.empty(),
                  "invalid expansion booleans reject candidate with retained fidelity");
        }
    for (const auto* fields : {
        "    filter: 'a'\n", "    exclude-filter: 'z'\n", "    exclude-type: Socks5\n",
        "    filter: null\n", "    exclude-filter: []\n", "    exclude-type: {}\n",
        "    filter: ' '\n", "    use: [remote]\n", "    use: null\n",
        "    include-all: true\n", "    include-all-providers: true\n",
        "    proxies: DIRECT\n", "    proxies: [DIRECT, null]\n", "    proxies: [DIRECT, {}]\n",
        "    proxies: ['']\n", "    include-all-proxies: false\n", "    filter: ''\n    filter: a\n"}) {
        const auto result = compile(std::string("    include-all-proxies: true\n") + fields);
        check(!result.error.empty() && result.json.empty() && std::ranges::any_of(result.fidelity, [](const auto& note) {
                  return note.level == singbox::Fidelity::Unsupported && note.subject == "all";
              }), "partial expansion, unsupported filters and malformed/duplicate members fail atomically");
    }
    const auto partial = compile("    include-all-proxies: true\n", nodes +
        "  - {name: rejected, type: unsupported, server: 192.0.2.3, port: 1080}\n");
    check(partial.error.empty() && group(partial)["outbounds"] == json::array({"a-node", "z-node"}) &&
          std::ranges::any_of(partial.fidelity, [](const auto& note) {
              return note.scope == singbox::FidelityScope::Group && note.subject == "all" &&
                     note.level == singbox::Fidelity::Approx;
          }), "rejected source nodes remain recorded in expanded group fidelity");
    for (const auto* members : {"", "    proxies: [REJECT]\n", "    proxies: [missing]\n"}) {
        const auto result = compile(std::string("    include-all-proxies: true\n") + members, "proxies: []\n");
        check(!result.error.empty() && result.json.empty(), "empty expansion never silently falls back to DIRECT");
    }
    const auto cycle = compile("    include-all-proxies: true\n",
        "proxies:\n  - {name: chain, type: socks5, server: 192.0.2.1, port: 1080, dialer-proxy: all}\n");
    check(!cycle.error.empty() && cycle.json.empty(), "expanded group membership participates in detour cycle validation");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources;
    sources.mainConnectionId = "expansion-main";
    sources.profileYaml = nodes + "proxy-groups:\n  - {name: all, type: select, include-all-proxies: true}\nrules: ['MATCH,all']\n";
    sources.auxiliarySources = {{"expansion-secondary", "secondary",
        "proxies:\n  - {name: a-node, type: socks5, server: 192.0.2.3, port: 1080}\n"
        "  - {name: secondary-only, type: socks5, server: 192.0.2.4, port: 1080}\n"
        "proxy-groups:\n  - {name: all, type: select, include-all-proxies: true}\nrules: ['MATCH,all']\n", true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test",
        .connectionId = "expansion-secondary", .id = "expansion-source", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Group, .targetObject = "all"}};
    const auto scoped = singbox::compileConfig(sources);
    check(scoped.error.empty() && !scoped.json.empty(), "expanded groups compile in independent main and secondary sources");
    if (!scoped.json.empty()) {
        const auto config = json::parse(scoped.json);
        int verified = 0;
        for (const auto& object : scoped.sourceObjects) {
            if (object.objectId != "all" || object.kind != vpn::TargetKind::Group) continue;
            const auto found = std::ranges::find_if(config["outbounds"], [&](const auto& value) {
                return value.value("tag", "") == object.tag;
            });
            check(found != config["outbounds"].end() && (*found)["outbounds"].size() == 2,
                  "each source exports its own two expanded nodes");
            if (found == config["outbounds"].end()) continue;
            for (const auto& member : (*found)["outbounds"])
                check(std::ranges::any_of(scoped.sourceObjects, [&](const auto& value) {
                    return value.sourceId == object.sourceId && value.kind == vpn::TargetKind::Node && value.tag == member.get<std::string>();
                }), "expanded member references stay within their source namespace");
            ++verified;
        }
        check(verified == 2, "both source groups retain their expansion after namespace rewriting");
    }
#endif
    for (const bool speedOnly : {false, true}) {
        singbox::CompileOptions options;
        options.speedTestOnly = speedOnly;
        options.profileYaml = nodes +
            "proxy-groups:\n  - {name: all, type: url-test, include-all-proxies: true, interval: 60}\nrules: ['MATCH,all']\n";
        const auto result = singbox::compileConfig(options);
        check(result.error.empty() && group(result)["outbounds"] == json::array({"a-node", "z-node"}) &&
              group(result).value("type", "") == (speedOnly ? "selector" : "urltest"),
              "expanded URLTest members survive ordinary and speed-only compilation");
    }
}

} // namespace

// Inactive-source inspection uses the same export compiler as orchestration,
// without building/applying a plan or staging provider resources.
void testSourceObjectDirectory() {
    singbox::CompileOptions options;
    options.mainConnectionId = "profile-17";
    options.mainSourceName = "inactive";
    options.profileYaml = R"yaml(
proxies:
  - {name: A, type: socks5, server: 127.0.0.1, port: 1080}
  - {name: B, type: socks5, server: 127.0.0.1, port: 1081}
  - {name: rejected, type: unknown, server: bad.test, port: 443}
proxy-groups:
  - {name: Pick, type: select, include-all-proxies: true}
rule-providers:
  external: {type: http, behavior: domain, url: 'https://invalid.test/provider', path: absent.yaml}
rules: ['RULE-SET,external,Pick', 'MATCH,Pick']
)yaml";
    auto missingIdentity = options;
    missingIdentity.mainConnectionId.clear();
    check(!singbox::inspectClashSourceObjects(missingIdentity).error.empty(), "object inventory requires a stable source ID");
    const auto inventory = singbox::inspectClashSourceObjects(options);
    check(inventory.error.empty(), "inactive source object inspection does not require provider download");
    check(inventory.json.empty() && inventory.httpRuleProviders.empty() && inventory.ruleSetResources.empty(),
          "object inspection is not a runnable plan or a resource-staging request");
    const auto contains = [&](const auto& result, vpn::TargetKind kind, const std::string& name) {
        return std::ranges::any_of(result.sourceObjects, [&](const auto& item) {
            return item.sourceId == options.mainConnectionId && item.kind == kind && item.objectId == name && item.tag != name;
        });
    };
    check(contains(inventory, vpn::TargetKind::Node, "A") && contains(inventory, vpn::TargetKind::Node, "B") &&
          contains(inventory, vpn::TargetKind::Group, "Pick"), "directory includes compiled nodes and expanded groups with original names");
    check(!contains(inventory, vpn::TargetKind::Node, "rejected") && !inventory.fidelity.empty(),
          "rejected nodes remain absent and inspection retains fidelity notes");
    options.mainConnectionId = "profile-18";
    const auto second = singbox::inspectClashSourceObjects(options);
    check(contains(second, vpn::TargetKind::Group, "Pick") &&
          second.sourceObjects.back().tag != inventory.sourceObjects.back().tag,
          "same object name in another source has a distinct identity");
    for (const auto* invalid : {"", "{\"outbounds\": []}", "proxies: [", "proxy-groups: [{name: loop, type: select, proxies: [loop]}]",
                              "proxy-groups: [{name: empty, type: select, include-all-proxies: true}]", "rules: ['MATCH,missing']"}) {
        options.profileYaml = invalid;
        const auto result = singbox::inspectClashSourceObjects(options);
        check(!result.error.empty() && result.sourceObjects.empty(), "unreadable or invalid sources never publish a partial object directory");
    }
}

int main(int argc, char** argv) {
    // Test-only compiler entry for isolated native runtime fixtures; no app
    // Runtime, user database, network downloads or persistent paths involved.
    if (argc == 5 && std::string_view(argv[1]) == "--prepare-config") {
        const std::filesystem::path root(argv[3]);
        std::ifstream input(argv[2]);
        singbox::CompileOptions options;
        options.profileYaml.assign(std::istreambuf_iterator<char>(input), {});
        options.ruleProviderCacheDir = (root / "cache").string();
        options.ruleProviderDir = root.string();
        options.ruleSetDir = (root / "geo").string();
        rule_provider_cache::Operations operations;
        operations.fetch = [&](const auto& resource, const auto& target, std::string& error) {
            std::error_code ec;
            std::filesystem::copy_file(root / (resource.cacheKey + ".download"), target,
                                      std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) error = "missing offline fixture";
            return !ec;
        };
        operations.check = [](const auto&, const auto& result, std::string&) {
            return result.error.empty() && !result.json.empty();
        };
        operations.commit = [](const auto& source, const auto& target, std::string& error) {
            std::error_code ec; std::filesystem::rename(source, target, ec);
            error = ec ? ec.message() : ""; return !ec;
        };
        singbox::CompileResult result; std::string error;
        const bool ok = rule_provider_cache::Prepare(options, operations, result, error);
        json manifest = json::array();
        for (const auto& resource : result.httpRuleProviders)
            manifest.push_back({{"key", resource.cacheKey}, {"url", resource.url},
                                {"headers", resource.headers}, {"max_bytes", resource.maxBytes}});
        std::ofstream(root / "manifest.json") << manifest.dump();
        if (!ok) { std::println(stderr, "{}", error); return 1; }
        std::ofstream output(argv[4]); output << result.json; output.close();
        std::println("prepared providers={}, fidelity={}", result.httpRuleProviders.size(), result.fidelity.size());
        return output ? 0 : 1;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--compile-config") {
        std::ifstream input(argv[2], std::ios::binary);
        singbox::CompileOptions options;
        options.mainConnectionId = "runtime-main";
        options.profileYaml.assign(std::istreambuf_iterator<char>(input), {});
        const auto result = singbox::compileConfig(options);
        if (!input || !result.error.empty() || result.json.empty()) return 1;
        std::ofstream output(argv[3]); output << result.json; output.close();
        return output ? 0 : 1;
    }
    testProxyValues();
    testGroupProxyExpansion();
    testSourceObjectDirectory();
    testProxyUdpOptions();
    testShadowsocksPlugins();
    testLogicalRuleResources();
    testFileRuleProviders();
    testHttpRuleProviders();
    testAsnRuleProviders();
    testMrsRuleProviders();
    for (const auto* format : {"{bad: yaml}", "[yaml]", "null", "''", "'  '"}) {
        singbox::CompileOptions options;
        options.profileYaml = std::format(
            "rule-providers:\n  sample: {{type: inline, behavior: domain, format: {}, payload: [valid.test]}}\n"
            "rules: ['RULE-SET,sample,DIRECT', 'MATCH,DIRECT']\n", format);
        const auto result = singbox::compileConfig(options);
        check(std::ranges::any_of(result.fidelity, [](const auto& note) {
            return note.level == singbox::Fidelity::Unsupported && note.subject == "rule-providers.sample";
        }), "inline explicit malformed format is rejected with ledger rather than defaulted");
        if (!result.json.empty()) check(json::parse(result.json)["route"].value("rule_set", json::array()).empty(),
                                       "inline malformed format emits no usable provider");
    }
    testDnsCertificateParameters();
    testDnsWildcardPolicies();
    testClashHosts();
    testHysteria1();
    testSsh();
    // A plain node must never replace an unsupported certificate constraint or
    // combination transport without a fidelity entry (legacy protocol paths).
    for (const auto& [type, credentials] : std::vector<std::pair<std::string, std::string>>{
        {"trojan", "    password: sample\n"},
        {"vless", "    uuid: 11111111-2222-3333-4444-555555555555\n"},
        {"vmess", "    uuid: 11111111-2222-3333-4444-555555555555\n"},
        {"ss", "    cipher: aes-256-gcm\n    password: sample\n"},
        {"hysteria2", "    password: sample\n"},
        {"tuic", "    uuid: 11111111-2222-3333-4444-555555555555\n    password: sample\n"},
        {"http", "    username: sample\n"}, {"socks5", "    username: sample\n"}}) {
        singbox::CompileOptions legacy;
        legacy.profileYaml = "proxies:\n  - name: guarded\n    type: " + type +
            "\n    server: 127.0.0.1\n    port: 443\n" + credentials +
            "    fingerprint: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
            "    shadow-tls-opts: {version: 3, password: sample}\n"
            "rules:\n  - MATCH,DIRECT\n";
        const auto guarded = singbox::compileConfig(legacy);
        check(guarded.error.empty(), "unsupported legacy node does not invalidate unrelated DIRECT rules");
        check(guarded.fidelity.size() == 2 && guarded.warnings.size() == 2,
              std::format("{} certificate and combination fields both enter ledger", type));
        if (!guarded.json.empty()) {
            const auto config = json::parse(guarded.json);
            check(std::ranges::none_of(config["outbounds"], [](const auto& out) {
                return out.value("tag", "") == "guarded";
            }), "unsupported combination is rejected as a whole node");
        }
        legacy.profileYaml.replace(legacy.profileYaml.find("  - MATCH,DIRECT"),
                                  std::string::npos, "  - MATCH,guarded\n");
        const auto failed = singbox::compileConfig(legacy);
        check(!failed.error.empty() && failed.json.empty() && failed.fidelity.size() >= 2,
              "missing guarded MATCH target fails and preserves fidelity");
    }
    {
        singbox::CompileOptions nested;
        nested.profileYaml = "proxies:\n  - {name: ws, type: vless, server: 127.0.0.1, port: 443, "
            "uuid: 11111111-2222-3333-4444-555555555555, network: ws, "
            "ws-opts: {path: /a, v2ray-http-upgrade: true}}\nrules:\n  - MATCH,DIRECT\n";
        const auto guarded = singbox::compileConfig(nested);
        check(guarded.fidelity.size() == 1 && guarded.fidelity.front().level == singbox::Fidelity::Unsupported,
              "unknown nested transport field rejects node and enters ledger");
    }
    // ---- 订阅编译 -----------------------------------------------------------
    for (const char* type : {"ss", "vmess", "vless", "trojan", "hysteria2", "tuic", "socks5"}) {
        singbox::CompileOptions udp;
        udp.profileYaml = std::format(
            "proxies:\n  - {{name: tcp-only, type: {}, server: 127.0.0.1, port: 443, udp: false}}\n"
            "rules:\n  - MATCH,DIRECT\n", type);
        const auto result = singbox::compileConfig(udp);
        check(result.error.empty() && !result.json.empty(), "explicit UDP disable compiles");
        if (!result.json.empty()) {
            const auto config = json::parse(result.json);
            const auto found = std::ranges::find_if(config["outbounds"], [](const auto& out) {
                return out.value("tag", "") == "tcp-only";
            });
            check(found != config["outbounds"].end() && found->value("network", "") == "tcp",
                  std::format("{} explicit udp:false becomes native TCP restriction", type));
        }
    }
    for (const char* field : {"udp: perhaps", "tls: []", "up: auto", "up: 0", "down: 30.5 Mbps", "up: 30 Kbps"}) {
        singbox::CompileOptions invalid;
        invalid.profileYaml = std::format(
            "proxies:\n  - {{name: invalid, type: hysteria2, server: 127.0.0.1, port: 443, {}}}\n"
            "rules:\n  - MATCH,DIRECT\n", field);
        const auto result = singbox::compileConfig(invalid);
        check(!result.fidelity.empty() && result.fidelity.front().level == singbox::Fidelity::Unsupported,
              std::format("{} cannot silently become default", field));
    }
    {
        singbox::CompileOptions paths;
        paths.profileYaml = "proxies:\n  - {name: http-paths, type: vless, server: 127.0.0.1, port: 443, "
            "uuid: 11111111-2222-3333-4444-555555555555, network: http, "
            "http-opts: {path: [/first, /second], host: [example.test]}}\nrules:\n  - MATCH,DIRECT\n";
        const auto result = singbox::compileConfig(paths);
        check(result.fidelity.size() == 1 && result.fidelity.front().level == singbox::Fidelity::Approx,
              "HTTP multi-path reduction enters fidelity ledger");
    }
    for (const char* type : {"trojan", "hysteria2", "tuic"}) {
        singbox::CompileOptions tls;
        tls.profileYaml = std::format(
            "proxies:\n  - {{name: secure, type: {}, server: example.test, port: 443, "
            "password: sample, uuid: 11111111-2222-3333-4444-555555555555}}\nrules:\n  - MATCH,DIRECT\n", type);
        // Only TUIC has a UUID field.
        if (std::string_view(type) != "tuic") {
            const auto begin = tls.profileYaml.find(", uuid:");
            tls.profileYaml.erase(begin, tls.profileYaml.find('}', begin) - begin);
        }
        const auto result = singbox::compileConfig(tls);
        check(!result.json.empty(), "implicit TLS node compiles");
        if (!result.json.empty()) {
            const auto config = json::parse(result.json);
            const auto found = std::ranges::find_if(config["outbounds"], [](const auto& out) {
                return out.value("tag", "") == "secure";
            });
            check(found != config["outbounds"].end() && (*found)["tls"].value("enabled", false),
                  std::format("{} enables its protocol TLS without optional subscription fields", type));
            if (found != config["outbounds"].end() && std::string_view(type) != "trojan")
                check(!(*found)["tls"].contains("utls"), "QUIC TLS does not inject TCP uTLS");
        }
    }
    for (const char* proxy : {
        "{name: unsupported, type: socks5, server: 127.0.0.1, port: 1080, tls: true}",
        "{name: unsupported, type: hysteria2, server: example.test, port: 443, client-fingerprint: chrome}",
        "{name: unsupported, type: tuic, server: example.test, port: 443, reality-opts: {public-key: x}}"}) {
        singbox::CompileOptions invalid;
        invalid.profileYaml = std::string("proxies:\n  - ") + proxy + "\nrules:\n  - MATCH,DIRECT\n";
        const auto result = singbox::compileConfig(invalid);
        check(!result.fidelity.empty() && result.fidelity.front().level == singbox::Fidelity::Unsupported,
              "protocol-specific unsupported TLS fields reject node and enter ledger");
    }
    singbox::CompileOptions options;
    options.controller = "127.0.0.1:9097";
    options.secret = "s3cret";
    options.mixedPort = 7899;
    options.mode = "rule";
    options.allowLan = false;
    options.logLevel = "warning";
    options.tunInbound = false;
    options.profileYaml = kFixture;
    const auto result = singbox::compileConfig(options);
    check(result.error.empty(), std::format("编译无致命错误（实际: {}）", result.error));
    check(!result.json.empty(), "产物 JSON 非空");
    if (result.json.empty()) return 1;
    const json config = json::parse(result.json);

    // 骨架：clash_api / 日志级别映射 / 混合入站。
    check(config["experimental"]["clash_api"]["external_controller"] ==
              "127.0.0.1:9097",
          "clash_api external_controller");
    check(config["experimental"]["clash_api"]["secret"] == "s3cret",
          "clash_api secret");
    check(config["experimental"]["clash_api"]["default_mode"] == "rule",
          "clash_api default_mode");
    check(config["experimental"]["clash_api"].contains("mode_list") == false,
          "clash_api 不携带 mode_list（内核自动推导）");
    check(config["log"]["level"] == "warn", "日志级别 warning→warn");
    bool hasMixed = false;
    for (const auto& inbound : config["inbounds"]) {
        if (inbound["type"] == "mixed" && inbound["listen_port"] == 7899) {
            hasMixed = inbound["listen"] == "127.0.0.1";
        }
    }
    check(hasMixed, "mixed 入站 127.0.0.1:7899");
    check(!config.contains("inbounds") ||
              std::none_of(config["inbounds"].begin(), config["inbounds"].end(),
                           [](const json& i) { return i.value("type", "") == "tun"; }),
          "未启用 TUN 时无 tun inbound");

    // 节点映射。
    const json& outbounds = config["outbounds"];
    auto findOutbound = [&](std::string_view tag) -> const json {
        for (const auto& out : outbounds) {
            if (out.value("tag", "") == tag) return out;
        }
        return json{};
    };
    const json hk = findOutbound("香港 01");
    check(hk.value("type", "") == "shadowsocks", "ss → shadowsocks");
    check(hk.value("method", "") == "aes-256-gcm", "cipher → method");
    check(hk.value("server_port", 0) == 8388, "port → server_port");
    check(!hk.contains("udp"), "udp 字段不透传（sing-box 严格字段校验）");
    const json us = findOutbound("美国 01");
    check(us.value("security", "") == "auto", "vmess cipher → security");
    check(us.value("transport", json{})["type"] == "ws", "vmess ws transport");
    check(us.value("transport", json{})["headers"]["Host"] == "us.example.com",
          "ws-opts.headers → transport.headers");
    check(us.value("tls", json{})["server_name"] == "us.example.com", "vmess tls sni");
    const json jp = findOutbound("日本 01");
    check(jp.value("obfs", json{})["type"] == "salamander", "hysteria2 obfs 映射");
    check(jp.value("up_mbps", 0) == 30 && jp.value("down_mbps", 0) == 200,
          "hysteria2 带宽映射");
    const json de = findOutbound("德国 01");
    check(de.value("congestion_control", "") == "bbr", "tuic congestion_control");
    check(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) { return w.find("ssr") != std::string::npos; }),
          "未知协议 ssr 产生警告");
    // 同一批降级既进 warnings（自由文本）也进 fidelity（结构化账本）：UI 按
    // fidelity 渲染，不再解析文本（见 docs/singbox-layers-and-fidelity.md §2）。
    check(std::any_of(result.fidelity.begin(), result.fidelity.end(),
                      [](const singbox::FidelityNote& n) {
                          return n.level == singbox::Fidelity::Unsupported &&
                                 n.subject == "过期节点" && !n.action.empty();
                      }),
          "未知协议进账本（Unsupported + subject + 建议动作）");
    check(std::any_of(result.fidelity.begin(), result.fidelity.end(),
                      [](const singbox::FidelityNote& n) {
                          return n.scope == singbox::FidelityScope::Group &&
                                 n.level == singbox::Fidelity::Approx &&
                                 n.subject == "自动选择" &&
                                 n.detail.find("REJECT") != std::string::npos;
                      }),
          "REJECT 成员移除进账本（组级 Approx，subject = 组名）");
    check(std::all_of(result.fidelity.begin(), result.fidelity.end(),
                      [&result](const singbox::FidelityNote& n) {
                          return std::find(result.warnings.begin(),
                                           result.warnings.end(),
                                           n.detail) != result.warnings.end();
                      }),
          "每条 fidelity 都能在 warnings 里找到自由文本投影");
    const std::string mainSummary = singbox::FidelitySummary(result.fidelity);
    check(mainSummary.find("跳过 1 个节点") != std::string::npos,
          "摘要统计被跳过的节点数");
    check(mainSummary.find("降级 1 个组") != std::string::npos,
          "摘要统计被降级的组数（REJECT 成员移除）");

    // 组映射：url-test → urltest，REJECT 成员被过滤。
    const json autoGroup = findOutbound("自动选择");
    check(autoGroup.value("type", "") == "urltest", "url-test → urltest");
    check(autoGroup.value("interval", "") == "300s", "interval 秒 → 时长");
    const json members = autoGroup.value("outbounds", json{});
    check(std::find(members.begin(), members.end(), "REJECT") == members.end(),
          "REJECT 成员被移除");
    check(std::find(members.begin(), members.end(), "香港 01") != members.end(),
          "节点成员保留");
    // select 组本身不该有降级条目。这里断言结构化账本而不是 warnings 文本：
    // 「手动选择」会作为别的降级条目的目标名出现在 detail 里（如
    // `PROCESS-NAME,ssh,手动选择`），子串匹配会误判。
    check(std::none_of(
              result.fidelity.begin(), result.fidelity.end(),
              [](const singbox::FidelityNote& n) {
                  return n.scope == singbox::FidelityScope::Group &&
                         n.subject == "手动选择";
              }),
          "select 组无降级条目");

    // 路由：final = MATCH 目标；clash_mode 前置；resolve 使 GEOIP 可匹配域名；
    // REJECT → action；GEOIP/GEOSITE → rule_set。
    check(config["route"]["final"] == "自动选择", "MATCH 目标成为 final");
    const json& dnsServers = config["dns"]["servers"];
    check(config["dns"]["final"] == "dns-0", "Clash nameserver 成为 DNS final");
    check(dnsServers.size() == 3, "Clash nameserver 转换为 typed DNS servers");
    if (dnsServers.size() >= 3) {
        check(dnsServers[1].value("type", "") == "https" &&
                  dnsServers[1].value("server", "") == "dns.example.com" &&
                  dnsServers[1].value("detour", "") == "手动选择" &&
                  dnsServers[1].value("domain_resolver", json{})["server"] == "local",
              "DoH nameserver 保留 detour 与 domain_resolver");
        check(dnsServers[2].value("type", "") == "udp" &&
                  dnsServers[2].value("server", "") == "223.5.5.5",
              "纯 IP nameserver 转换为 UDP DNS server");
    }
    const json& rules = config["route"]["rules"];
    check(rules[0].value("action", "") == "sniff", "首条规则为 sniff action");
    check(rules[1].value("action", "") == "hijack-dns", "DNS 劫持规则");
    check(rules[2].value("action", "") == "resolve", "DNS 劫持后前置 resolve action");
    check(rules[3].value("clash_mode", "") == "direct" &&
              rules[3].value("outbound", "") == "DIRECT",
          "direct 模式前置规则");
    check(rules[4].value("clash_mode", "") == "global", "global 模式前置规则");
    bool sawReject = false;
    bool sawGeoip = false;
    bool sawGeosite = false;
    int geoipCnRuleCount = 0;
    int geositeCnRuleCount = 0;
    bool sawCidr = false;
    for (const auto& rule : rules) {
        if (rule.value("action", "") == "reject" &&
            rule.value("domain", json{}) == json{"blocked.net"}) {
            sawReject = true;
        }
        if (rule.contains("rule_set") &&
            rule["rule_set"] == json{"geoip-cn"}) {
            sawGeoip = true;
            ++geoipCnRuleCount;
        }
        if (rule.contains("rule_set") &&
            rule["rule_set"] == json{"geosite-cn"}) {
            sawGeosite = true;
            ++geositeCnRuleCount;
        }
        if (rule.value("ip_cidr", json{}) == json{"192.168.0.0/16"}) {
            sawCidr = true;
        }
    }
    check(sawReject, "REJECT 目标 → action:reject");
    check(sawGeoip, "GEOIP CN → rule_set");
    check(sawGeosite, "GEOSITE CN → rule_set");
    check(geoipCnRuleCount == 2, "RULE-SET cn-ip → geoip-cn rule_set");
    check(geositeCnRuleCount == 2, "RULE-SET cn → geosite-cn rule_set");
    check(sawCidr, "IP-CIDR 直映射");
    bool sawRemoteGeoip = false;
    bool sawRemoteGeosite = false;
    for (const auto& ruleSet : config["route"]["rule_set"]) {
        const std::string url = ruleSet.value("url", "");
        if (ruleSet.value("tag", "") == "geoip-cn") {
            sawRemoteGeoip = url.find("meta-rules-dat/sing/geo/geoip/cn.srs") !=
                             std::string::npos;
        }
        if (ruleSet.value("tag", "") == "geosite-cn") {
            sawRemoteGeosite = url.find("meta-rules-dat/sing/geo/geosite/cn.srs") !=
                                std::string::npos;
        }
    }
    check(sawRemoteGeoip, "geoip-cn 远程 .srs 规则集");
    check(sawRemoteGeosite, "geosite-cn 远程 .srs 规则集");
    check(config["dns"]["rules"].is_array() &&
              config["dns"]["rules"][0]["rule_set"] == json{"geosite-cn"} &&
              config["dns"]["rules"][0]["server"] == "local",
          "GEOSITE CN 直连规则使用本地 DNS");
    check(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) {
                          return w.find("RULE-SET") != std::string::npos;
                      }),
          "RULE-SET 产生警告");

    // 进程匹配：桌面上一等映射到 sing-box 的 process_name / process_path /
    // process_path_regex（Android 侧 process_* 不可用，编译器按平台分支记不支持，
    // 本测试是桌面目标，走不到那条分支）。
    const auto findRule = [&config](std::string_view field,
                                    const json& expected) -> bool {
        for (const auto& rule : config["route"]["rules"]) {
            if (rule.contains(field) && rule[field] == expected) return true;
        }
        return false;
    };
    check(findRule("process_name", json{"ssh"}),
          "PROCESS-NAME → process_name");
    check(findRule("process_path", json{"/usr/bin/curl"}),
          "PROCESS-PATH → process_path");
    check(findRule("process_path_regex", json{"^/opt/.+"}),
          "PROCESS-PATH-REGEX → process_path_regex");
    check(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) {
                          return w.find("PROCESS-NAME-REGEX") != std::string::npos;
                      }),
          "PROCESS-NAME-REGEX 无对应字段，进账本");
    check(std::none_of(result.fidelity.begin(), result.fidelity.end(),
                       [](const singbox::FidelityNote& n) {
                           return n.scope == singbox::FidelityScope::Rule &&
                                  n.subject == "PROCESS-NAME";
                       }),
          "PROCESS-NAME 已映射，不再出现在账本里");

    // ---- TUN 注入（Android 形态：ipv6 关闭 + 宽松路由）-----------------------
    singbox::CompileOptions androidOptions = options;
    androidOptions.tunInbound = true;
    androidOptions.ipv6 = false;
    androidOptions.tunStrictRoute = false;
    const auto androidResult = singbox::compileConfig(androidOptions);
    const json androidConfig = json::parse(androidResult.json);
    bool sawTun = false;
    for (const auto& inbound : androidConfig["inbounds"]) {
        if (inbound.value("type", "") == "tun") {
            sawTun = inbound.value("strict_route", true) == false &&
                     inbound["address"] == json{"172.19.0.1/30"};
        }
    }
    check(sawTun, "Android tun inbound 形态");
    check(androidConfig["dns"]["strategy"] == "ipv4_only", "Android IPv4-only DNS 策略");
    check(androidConfig["route"].value("auto_detect_interface", false),
          "tun 启用时 auto_detect_interface");

    // ---- 原生 sing-box JSON 直通 ---------------------------------------------
    singbox::CompileOptions nativeOptions = options;
    nativeOptions.profileYaml =
        R"({"outbounds":[{"type":"shadowsocks","tag":"n1","server":"s","server_port":1,"method":"aes-128-gcm","password":"p"}]})";
    const auto native = singbox::compileConfig(nativeOptions);
    check(native.error.empty(), "原生 JSON 直通无错误");
    const json nativeConfig = json::parse(native.json);
    check(nativeConfig["outbounds"][0]["tag"] == "n1", "原生 outbounds 保留");
    check(!nativeConfig["inbounds"].empty(), "原生配置补齐 inbounds");
    check(nativeConfig["route"]["final"] == "n1", "原生未指定 final 时保持首出站语义");

    // ---- 主 VPN + 原生补偿：匹配优先级和离线拒绝 -------------------------------
    singbox::CompileOptions compensation;
    compensation.profileYaml = R"yaml(
proxies:
  - {name: node, type: socks5, server: 127.0.0.1, port: 1080}
proxy-groups:
  - {name: chosen, type: select, proxies: [node]}
rules:
  - DOMAIN-SUFFIX,corp.example,DIRECT
  - MATCH,chosen
)yaml";
    compensation.tunInbound = true;
    compensation.mainConnectionId = "profile:main";
    compensation.nativeConnections = {
        {.id = "profile:pptp", .interfaceName = "ppp7", .internalRoutes = {"10.0.0.0/8"}, .connected = true},
        {.id = "profile:offline", .internalRoutes = {"10.42.0.0/16"}},
    };
    compensation.globalRules = {
        {.match = vpn::MatchKind::DomainSuffix, .pattern = "corp.example", .connectionId = "profile:pptp", .priority = 10, .order = 1},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "PUBLIC.CORP.EXAMPLE.", .connectionId = "profile:main", .priority = 10},
        {.match = vpn::MatchKind::ExactIp, .pattern = "10.42.0.9", .connectionId = "profile:main", .priority = 20},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "offline.example", .connectionId = "profile:offline", .priority = 9},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "other-core.example", .connectionId = "profile:other-core", .priority = 8},
        {.match = vpn::MatchKind::ExactIp, .pattern = "2001:db8::1", .connectionId = "profile:main", .priority = 7},
    };
    compensation.tunExcludeAddresses = {"198.51.100.7", "198.51.100.7/32"};
#if (!defined(__linux__) && !defined(_WIN32)) || defined(__ANDROID__)
    check(!singbox::compileConfig(compensation).error.empty(),
          "缺少补偿后端的平台必须拒绝原生 VPN 与 TUN 共存");
    compensation.tunInbound = false;
#endif
    auto compensationResult = singbox::compileConfig(compensation);
    check(compensationResult.error.empty(), "主 VPN 补偿配置编译成功");
    const auto compensationConfig = json::parse(compensationResult.json);
    const auto& compensationRules = compensationConfig["route"]["rules"];
    const std::string mainOutbound = compensationConfig["route"]["final"];
    check(compensationRules[0]["action"] == "sniff" &&
              compensationRules[1]["action"] == "hijack-dns" &&
              compensationRules[2]["action"] == "resolve",
          "补偿规则位于 sniff/DNS/resolve 之后");
    check(compensationRules[3]["ip_cidr"] == json{"10.42.0.9/32"} &&
              compensationRules[3]["outbound"] == mainOutbound, "高优先级全局规则可覆盖原生内网");
    check(compensationRules[4]["domain"] == json{"public.corp.example"} &&
              compensationRules[4]["outbound"] == mainOutbound, "同优先级按显式 order 排序，主 VPN 使用隔离后的订阅 final");
    const std::string nativeTag = compensationRules[5]["outbound"];
    bool hasBoundNative = false;
    for (const auto& outbound : compensationConfig["outbounds"]) {
        if (outbound.value("tag", "") == nativeTag) {
            hasBoundNative = outbound.value("type", "") == "direct" &&
                             outbound.value("bind_interface", "") == "ppp7";
        }
    }
    check(hasBoundNative, "原生 VPN 域名规则使用绑定 ppp 接口的 direct outbound");
    check(compensationRules[6]["action"] == "reject" &&
              compensationRules[7]["action"] == "reject" &&
              compensationRules[8]["ip_cidr"] == json{"2001:db8::1/128"},
          "离线目标按默认策略阻断，IPv6 精确 IP 全局规则保留");
    check(compensationRules[9]["ip_cidr"] == json{"10.42.0.0/16"} &&
              compensationRules[9]["action"] == "reject" &&
              compensationRules[10]["ip_cidr"] == json{"10.0.0.0/8"},
          "内部 CIDR 同层按最长前缀排序，离线内部网段阻断");
    check(compensationRules[11]["clash_mode"] == "direct" && compensationRules[12]["clash_mode"] == "global",
          "主 VPN 三模式不能绕过全局连接选择");
    for (const auto& inbound : compensationConfig["inbounds"]) {
        if (inbound.value("type", "") != "tun") continue;
        const auto& exclusions = inbound["route_exclude_address"];
        check(std::count(exclusions.begin(), exclusions.end(), json("198.51.100.7/32")) == 1,
              "服务器传输地址转换 CIDR 并去重");
        check(std::find(exclusions.begin(), exclusions.end(), json("10.0.0.0/8")) == exclusions.end(),
              "原生数据 CIDR 不绕过 TUN 的全局规则");
#if defined(__linux__) && !defined(__ANDROID__)
        check(inbound["iproute2_rule_index"] == 9000 && inbound["auto_redirect"] == false,
              "Linux TUN 使用约定的补偿规则优先级并关闭 auto_redirect");
#endif
    }
    #ifdef _WIN32
    check(compensationResult.warnings.size() == 2, "不可用连接各报告一次警告");
    for (const auto& inbound : compensationConfig["inbounds"]) {
        if (inbound.value("type", "") != "tun") continue;
        check(inbound["strict_route"] == true && inbound["interface_name"] == "ClashFlux",
              "Windows PPTP 共存保留 strict_route 并使用确定的 TUN 别名");
    }
    auto unsupportedNative = compensation;
    unsupportedNative.nativeConnections[0].kind = vpn::ConnectionKind::WireGuard;
    check(!singbox::compileConfig(unsupportedNative).error.empty(), "Windows 未实现的原生 VPN 共存不得被误开放");
    unsupportedNative.tunInbound = false;
    check(singbox::compileConfig(unsupportedNative).error.empty(), "Windows 未实现的原生 VPN 独立运行不受 TUN 限制");
    auto customCapture = compensation;
    customCapture.profileYaml = R"({"inbounds":[{"type":"tun","route_address":["10.0.0.0/8"]}]})";
    check(!singbox::compileConfig(customCapture).error.empty(), "Windows PPTP 共存拒绝不完整的 TUN 接管范围");
#else
    check(compensationResult.warnings.size() == 2, "不可用连接各报告一次警告");
#endif
    auto transport = compensation;
    transport.nativeConnections[0].transportAddress = "203.0.113.7";
    const auto transportConfig = json::parse(singbox::compileConfig(transport).json);
    for (const auto& inbound : transportConfig["inbounds"]) {
        if (inbound.value("type", "") != "tun") continue;
        const auto& excluded = inbound["route_exclude_address"];
        check(std::find(excluded.begin(), excluded.end(), json("203.0.113.7/32")) != excluded.end(),
              "运行时实际拨号地址自动排除 TUN");
    }
    transport.nativeConnections[0].transportAddress = "vpn.example";
    check(!singbox::compileConfig(transport).error.empty(), "拒绝未解析的会话服务器地址");

    compensation.nativeConnections[0].connected = false;
    const auto disconnected = json::parse(singbox::compileConfig(compensation).json);
    check(disconnected["route"]["rules"][5]["action"] == "reject" &&
              disconnected["route"]["rules"][8]["ip_cidr"] == json{"2001:db8::1/128"} &&
              disconnected["route"]["rules"][10]["action"] == "reject" &&
              disconnected["route"]["rules"][11]["clash_mode"] == "direct",
          "断开后保留匹配条件并阻断，不静默回落主订阅");
    compensation.nativeConnections[0].connected = true;
    compensation.nativeConnections[0].interfaceName.clear();
    const auto missingInterface = json::parse(singbox::compileConfig(compensation).json);
    check(missingInterface["route"]["rules"] == disconnected["route"]["rules"],
          "连接无接口时同样执行目标不可用策略");
    compensation.nativeConnections[0].connected = false;

    // ---- sing-box OpenVPN endpoint：多条连接并行、规则直达 endpoint ---------
    const std::string openVpnConfig =
        "client\n"
        "remote first.vpn.example 443 tcp-client\n"
        "remote second.vpn.example 1194 udp\n"
        "remote-random\n"
        "<ca>\n"
        "-----BEGIN CERTIFICATE-----\n"
        "test-ca\n"
        "-----END CERTIFICATE-----\n"
        "</ca>\n"
        "<auth-user-pass>\n"
        "alice\n"
        "secret\n"
        "</auth-user-pass>\n"
        "remote-cert-tls server\n";
    singbox::CompileOptions openVpnOptions;
    openVpnOptions.nativeConnections = {
        {.id = "profile:openvpn-a", .internalRoutes = {"10.20.0.0/16"},
         .connected = true, .kind = vpn::ConnectionKind::OpenVpn,
         .nativeConfig = openVpnConfig},
        {.id = "profile:openvpn-b", .internalRoutes = {"10.21.0.0/16"},
         .connected = true, .kind = vpn::ConnectionKind::OpenVpn,
         .nativeConfig = openVpnConfig},
    };
    openVpnOptions.globalRules = {
        {.match = vpn::MatchKind::Ipv4Cidr, .pattern = "10.20.0.0/16",
         .connectionId = "profile:openvpn-a", .priority = 10},
        {.match = vpn::MatchKind::Ipv4Cidr, .pattern = "10.21.0.0/16",
         .connectionId = "profile:openvpn-b", .priority = 10},
    };
    const auto openVpnResult = singbox::compileConfig(openVpnOptions);
    check(openVpnResult.error.empty(), "多个 OpenVPN endpoint 编译成功");
    if (openVpnResult.error.empty()) {
        const json openVpnJson = json::parse(openVpnResult.json);
        check(openVpnJson["endpoints"].size() == 2,
              "多个 OpenVPN profile 生成多个并行 endpoint");
        check(openVpnJson["endpoints"][0].value("type", "") == "openvpn-client" &&
                  openVpnJson["endpoints"][1].value("type", "") == "openvpn-client",
              "OpenVPN profile 使用 sing-box openvpn-client endpoint");
        check(openVpnJson["endpoints"][0].value("system", true) == false,
              "OpenVPN endpoint 默认使用 sing-box 内部网络栈");
        const std::set<std::string> endpointTags = {
            openVpnJson["endpoints"][0].value("tag", ""),
            openVpnJson["endpoints"][1].value("tag", ""),
        };
        check(endpointTags.size() == 2, "并行 OpenVPN endpoint tag 独立");
        bool sawEndpointRoute = false;
        for (const auto& rule : openVpnJson["route"]["rules"]) {
            if (rule.value("ip_cidr", json::array()) == json{"10.20.0.0/16"} ||
                rule.value("ip_cidr", json::array()) == json{"10.21.0.0/16"}) {
                sawEndpointRoute = endpointTags.contains(rule.value("outbound", ""));
            }
        }
        check(sawEndpointRoute, "全局网段规则直接指向 OpenVPN endpoint");
    }
    for (const char* delimiter : {"\n", "\r\n"}) {
        auto literalOpenVpn = openVpnOptions;
        auto& config = literalOpenVpn.nativeConnections[0].nativeConfig;
        const auto start = config.find("<auth-user-pass>\n") + std::string("<auth-user-pass>\n").size();
        const auto end = config.find("</auth-user-pass>", start);
        config.replace(start, end - start, std::string(" \t user name ") + delimiter +
                       " 'quoted password' \t " + delimiter);
        const auto compiled = singbox::compileConfig(literalOpenVpn);
        check(compiled.error.empty() && !compiled.json.empty(), "literal OpenVPN credentials compile");
        if (!compiled.json.empty()) {
            const auto endpoint = json::parse(compiled.json)["endpoints"][0];
            check(endpoint["username"] == " \t user name " &&
                  endpoint["password"] == " 'quoted password' \t ",
                  "OpenVPN reads credential lines literally, removing only CRLF delimiters");
        }
    }
    for (const char* credentials : {"user secret\n", "user\n\n", "\nsecret\n", ""}) {
        auto malformed = openVpnOptions;
        auto& config = malformed.nativeConnections[0].nativeConfig;
        const auto start = config.find("<auth-user-pass>\n") + std::string("<auth-user-pass>\n").size();
        const auto end = config.find("</auth-user-pass>", start);
        config.replace(start, end - start, credentials);
        const auto failed = singbox::compileConfig(malformed);
        check(!failed.error.empty() && failed.json.empty() &&
              std::ranges::any_of(failed.fidelity, [](const auto& note) {
                  return note.sourceId == "profile:openvpn-a" && note.level == singbox::Fidelity::Unsupported;
              }), "missing/interactive OpenVPN credentials fail with source fidelity");
    }

    auto catchAll = compensation;
    catchAll.globalRules = {{vpn::MatchKind::Any, "", "profile:pptp", 100}};
    const auto paused = json::parse(singbox::compileConfig(catchAll).json);
    check(paused["route"]["rules"][3]["action"] == "reject",
          "显式全部规则的离线目标执行默认阻断策略");
    catchAll.tunInbound = false;
    catchAll.nativeConnections[0].connected = true;
    catchAll.nativeConnections[0].interfaceName = "ppp7";
    catchAll.globalRules = {{vpn::MatchKind::ExactDomain, "https://Portal.Example:443/path?q=1", "profile:pptp", 100}};
    const auto urlConfig = json::parse(singbox::compileConfig(catchAll).json);
    check(urlConfig["route"]["rules"][3]["domain"] == json{"portal.example"},
          "URL 只生成目标域名匹配，不能变成全部流量");
    catchAll.globalRules[0].match = vpn::MatchKind::Any;
    check(!singbox::compileConfig(catchAll).error.empty(), "全部规则不允许携带被忽略的匹配内容");

    // 原生 JSON 的额外排除必须合并，TUN 开关和单一所有权同样生效。
    auto rawCompensation = compensation;
    rawCompensation.tunInbound = true;
    rawCompensation.profileYaml = R"({"inbounds":[{"type":"tun","tag":"user-tun","address":["172.20.0.1/30"],"auto_redirect":true,"route_exclude_address":["203.0.113.0/24"]}],"outbounds":[{"type":"direct","tag":"local"}],"route":{"final":"local","rules":[{"domain":["user.example"],"outbound":"local"}]}})";
    const auto rawResult = singbox::compileConfig(rawCompensation);
    check(rawResult.error.empty(), "原生 JSON 应用主 VPN 补偿");
    const auto rawConfig = json::parse(rawResult.json);
    const auto& rawExclude = rawConfig["inbounds"][0]["route_exclude_address"];
    check(std::find(rawExclude.begin(), rawExclude.end(), json("203.0.113.0/24")) != rawExclude.end() &&
              std::find(rawExclude.begin(), rawExclude.end(), json("198.51.100.7/32")) != rawExclude.end(),
          "用户和动态服务器排除地址均保留");
    check(rawConfig["route"]["rules"][3]["outbound"] == "local" &&
              rawConfig["route"]["rules"].back()["domain"] == json{"user.example"}, "原生 JSON 全局规则优先，订阅规则保留");
    rawCompensation.tunInbound = false;
    const auto withoutTun = json::parse(singbox::compileConfig(rawCompensation).json);
    check(std::none_of(withoutTun["inbounds"].begin(), withoutTun["inbounds"].end(), [](const auto& inbound) {
        return inbound.value("type", "") == "tun";
    }), "关闭主 TUN 会移除原生配置自带的 TUN");
    rawCompensation.tunInbound = true;
    rawCompensation.profileYaml = R"({"inbounds":[{"type":"tun"},{"type":"tun"}]})";
    check(!singbox::compileConfig(rawCompensation).error.empty(), "多个托管 TUN 必须报错");

    auto invalidCompensation = compensation;
    invalidCompensation.nativeConnections[0].internalRoutes = {"2001:db8::/64"};
    check(!singbox::compileConfig(invalidCompensation).error.empty(), "暂不支持的原生 IPv6 内网不静默丢弃");
    invalidCompensation = compensation;
    invalidCompensation.globalRules[0] = {.match = vpn::MatchKind::ExactIp, .pattern = "2001:db8::1", .connectionId = "profile:pptp"};
    check(!singbox::compileConfig(invalidCompensation).error.empty(), "原生 IPv6 全局规则明确报错");
    invalidCompensation = compensation;
    invalidCompensation.globalRules[0] = {.match = vpn::MatchKind::Ipv4Cidr, .pattern = "10.0.0.0/33", .connectionId = "profile:pptp"};
    check(!singbox::compileConfig(invalidCompensation).error.empty(), "错误的全局 CIDR 阻断启动");
    invalidCompensation = compensation;
    invalidCompensation.tunExcludeAddresses = {"vpn.example"};
    check(!singbox::compileConfig(invalidCompensation).error.empty(), "传输服务器必须预解析，不允许把域名当 CIDR");

    // ---- 本地规则集命中与坏缓存拒绝 -------------------------------------------
    {
        const RuleSetTestDirectory testDirectory;
        const std::string dir = testDirectory.path.string();
        const auto geoipPath = std::filesystem::path(dir) / "geoip-cn.srs";
        const auto geositePath = std::filesystem::path(dir) / "geosite-cn.srs";
        // .srs 头部包含魔数、版本与合法 zlib 头；这里只验证筛查边界，不解压。
        { std::ofstream out(geoipPath, std::ios::binary); out << "SRS\x02\x78\x9c" << "stub-stub"; }
        { std::ofstream out(geositePath, std::ios::binary); out << "SRS\x02\x78\x9c" << "stub-stub"; }
        singbox::CompileOptions localOptions = options;
        localOptions.ruleSetDir = dir;
        check(singbox::RuleSetCacheValid(geoipPath), "SRS 头的规则集缓存有效");
        const auto localResult = singbox::compileConfig(localOptions);
        const json localConfig = json::parse(localResult.json);
        bool sawLocalGeoip = false;
        bool sawLocalGeosite = false;
        for (const auto& rs : localConfig["route"]["rule_set"]) {
            if (rs.value("tag", "") == "geoip-cn") {
                sawLocalGeoip = rs.value("type", "") == "local" &&
                                rs.value("path", "").find("geoip-cn.srs") != std::string::npos;
            }
            if (rs.value("tag", "") == "geosite-cn") {
                sawLocalGeosite = rs.value("type", "") == "local" &&
                                   rs.value("path", "").find("geosite-cn.srs") != std::string::npos;
            }
        }
        check(sawLocalGeoip, "ruleSetDir 命中时 GEOIP 以 local rule_set 生成");
        check(sawLocalGeosite, "ruleSetDir 命中时 GEOSITE 以 local rule_set 生成");

        // 坏缓存（0 字节、错误内容）会让内核启动期 FATAL：必须拒绝并
        // 回落 remote，而不是把坏文件写进 local rule_set。
        { std::ofstream out(geoipPath, std::ios::binary | std::ios::trunc); out << "<html>404</html>"; }
        { std::ofstream out(geositePath, std::ios::binary | std::ios::trunc); }
        check(!singbox::RuleSetCacheValid(geoipPath), "非 SRS 内容的规则集缓存被拒绝");
        check(!singbox::RuleSetCacheValid(geositePath), "0 字节规则集缓存被拒绝");
        check(!singbox::RuleSetCacheValid(std::filesystem::path(dir) / "missing.srs"),
              "缺失的规则集缓存被拒绝");
        const auto rejectedResult = singbox::compileConfig(localOptions);
        const json rejectedConfig = json::parse(rejectedResult.json);
        bool geoipRemote = false;
        bool geositeRemote = false;
        for (const auto& rs : rejectedConfig["route"]["rule_set"]) {
            if (rs.value("tag", "") == "geoip-cn") {
                geoipRemote = rs.value("type", "") == "remote";
            }
            if (rs.value("tag", "") == "geosite-cn") {
                geositeRemote = rs.value("type", "") == "remote";
            }
        }
        check(geoipRemote && geositeRemote, "坏缓存回落为 remote rule_set");
        check(std::filesystem::exists(geoipPath) &&
                  std::filesystem::exists(geositePath),
              "纯编译保留坏缓存文件，替换由下载边界负责");
    }

    // ---- 空订阅最小配置 -------------------------------------------------------
    options.profileYaml.clear();
    const auto empty = singbox::compileConfig(options);
    check(empty.error.empty(), "空订阅编译无错误");
    const json emptyConfig = json::parse(empty.json);
    check(emptyConfig["route"]["final"] == "DIRECT", "空订阅 final = DIRECT");
    check(emptyConfig["outbounds"].size() == 1, "空订阅仅 DIRECT outbound");

    // 可选真实订阅回归：命令行传入 YAML、本地规则集目录和
    // 可选输出 JSON。测试不固化订阅密钥，但能用用户实际订阅
    // 验证 RULE-SET,cn/cn-ip 没有再落入 MATCH 兜底。
    if (argc >= 3) {
        std::ifstream input(argv[1], std::ios::binary);
        check(input.good(), "真实订阅测试文件可读");
        singbox::CompileOptions realOptions;
        realOptions.mode = "rule";
        realOptions.tunInbound = true;
        realOptions.ruleSetDir = argv[2];
        realOptions.ruleProviderDir = std::filesystem::absolute(argv[1]).parent_path().string();
        const RuleSetTestDirectory httpCache;
        realOptions.ruleProviderCacheDir = httpCache.path.string();
        realOptions.profileYaml = std::string(std::istreambuf_iterator<char>(input),
                                              std::istreambuf_iterator<char>());
        auto realResult = singbox::compileConfig(realOptions);
        if (!realResult.httpRuleProviders.empty()) {
            // Public offline HTTP fixtures seed content from companion files.
            // No user cache is written and no fixture ever makes network calls.
            rule_provider_cache::Operations operations;
            operations.fetch = [](const auto&, const auto&, std::string& error) {
                error = "fixture network disabled"; return false;
            };
            operations.check = [](const auto&, const auto& compiled, std::string&) {
                return compiled.error.empty() && !compiled.json.empty();
            };
            operations.commit = [](const auto& temp, const auto& dest, std::string& error) {
                std::error_code ec;
                std::filesystem::rename(temp, dest, ec);
                error = ec ? ec.message() : ""; return !ec;
            };
            std::string error;
            check(rule_provider_cache::Prepare(realOptions, operations, realResult, error),
                  "HTTP fixture prepares read-only seeds into isolated temporary cache");
        }
        check(realResult.error.empty(),
              std::format("真实订阅编译无错（实际: {}）", realResult.error));
        if (!realResult.json.empty()) {
            const json realConfig = json::parse(realResult.json);
            const auto& realRules = realConfig["route"]["rules"];
            const bool hasCnDomain = std::ranges::any_of(realRules, [](const auto& rule) {
                return rule.value("rule_set", json::array()) == json{"geosite-cn"} &&
                       rule.value("outbound", "") == "DIRECT";
            });
            const bool hasCnIp = std::ranges::any_of(realRules, [](const auto& rule) {
                return rule.value("rule_set", json::array()) == json{"geoip-cn"} &&
                       rule.value("outbound", "") == "DIRECT";
            });
            check(hasCnDomain, "真实订阅 RULE-SET,cn 保留为国内域名直连");
            check(hasCnIp, "真实订阅 RULE-SET,cn-ip 保留为国内 IP 直连");
            if (argc >= 4) {
                std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
                output << realResult.json;
                check(output.good(), "真实订阅 sing-box JSON 可写");
            }
        }
    }

    // ---- 代理页快照（BuildProxySnapshot）-----------------------------------
    // 组条目 + 每个节点的 type/udp；savedSelection 同时落到快照 now 与 config
    // 的 selector default（写盘配置必须和 UI 一致）。
    {
        nlohmann::json config = {
            {"outbounds",
             nlohmann::json::array(
                 {{{"type", "selector"},
                   {"tag", "节点选择"},
                   {"outbounds", nlohmann::json::array({"vmess-01", "http-01"})},
                   {"default", "vmess-01"}},
                  {{"type", "vmess"}, {"tag", "vmess-01"}},
                  {{"type", "http"}, {"tag", "http-01"}},
                  {{"type", "direct"}, {"tag", "DIRECT"}}})}};
        const std::string snapshot = singbox::BuildProxySnapshot(
            config, [](const std::string& group) {
                return group == "节点选择" ? std::string{"http-01"}
                                          : std::string{};
            });
        const auto proxies = nlohmann::json::parse(snapshot, nullptr, false);
        check(proxies.is_object() && proxies.contains("proxies"),
              "代理快照顶层含 proxies");
        const auto& table = proxies["proxies"];
        check(table.contains("节点选择") &&
                  table["节点选择"].value("all", nlohmann::json::array())
                          .size() == 2 &&
                  table["节点选择"].value("selectable", false) &&
                  table["节点选择"].value("now", "") == "http-01",
              "组条目保留 all 成员并按保存值更新 now");
        check(table.contains("vmess-01") &&
                  table["vmess-01"].value("type", "") == "vmess",
              "节点条目带真实协议类型");
        check(!table["vmess-01"].contains("udp") &&
                  !table["DIRECT"].value("type", "").empty(),
              "预览快照不臆测 udp（只有内核真给了才显示）");
        check(config["outbounds"][0].value("default", "") == "http-01",
              "保存的选中项落到 config 的 selector default");
        // 形态不对的 config 不能抛异常。
        nlohmann::json broken = nlohmann::json::array();
        const auto empty = nlohmann::json::parse(
            singbox::BuildProxySnapshot(broken), nullptr, false);
        check(empty.is_object() && empty["proxies"].empty(),
              "非对象 config 返回空 proxies 快照");
    }

    // ---- 保真度账本：fallback / load-balance → urltest（Approx）-------------
    {
        singbox::CompileOptions fidelityOptions;
        fidelityOptions.profileYaml = R"yaml(
proxies:
  - name: "A"
    type: ss
    server: a.example.com
    port: 443
    cipher: aes-256-gcm
    password: "x"
proxy-groups:
  - name: "故障转移"
    type: fallback
    proxies: [A]
  - name: "负载均衡"
    type: load-balance
    proxies: [A]
rules:
  - MATCH,故障转移
)yaml";
        const auto groupResult = singbox::compileConfig(fidelityOptions);
        const auto notedAs = [&groupResult](singbox::Fidelity level,
                                           std::string_view subject) {
            return std::any_of(
                groupResult.fidelity.begin(), groupResult.fidelity.end(),
                [&](const singbox::FidelityNote& n) {
                    return n.level == level && n.subject == subject;
                });
        };
        check(notedAs(singbox::Fidelity::Approx, "故障转移"),
              "fallback 进账本 Approx（subject = 组名）");
        check(notedAs(singbox::Fidelity::Approx, "负载均衡"),
              "load-balance 进账本 Approx（subject = 组名）");
        check(singbox::FidelitySummary(groupResult.fidelity) == "降级 2 个组",
              "摘要按 scope + 级别汇总（降级 N 个组）");
        check(singbox::FidelitySummary({}).empty(), "空账本摘要为空串");
        check(std::all_of(groupResult.fidelity.begin(), groupResult.fidelity.end(),
                          [&groupResult](const singbox::FidelityNote& n) {
                              return std::find(groupResult.warnings.begin(),
                                               groupResult.warnings.end(),
                                               n.detail) !=
                                     groupResult.warnings.end();
                          }),
              "降级组的 fidelity 都有对应的 warnings 投影");
    }

    // ---- 账本也覆盖「条目消失」类降级（输入非法 / 平台限制）------------------
    {
        singbox::CompileOptions degradeOptions;
        degradeOptions.profileYaml = R"yaml(
proxies:
  - name: "重复"
    type: ss
    server: a.example.com
    port: 443
    cipher: aes-256-gcm
    password: "x"
  - name: "重复"
    type: ss
    server: b.example.com
    port: 443
    cipher: aes-256-gcm
    password: "y"
rules:
  - GEOIP,TELEGRAM,DIRECT
  - MATCH,DIRECT
)yaml";
        const auto degradeResult = singbox::compileConfig(degradeOptions);
        const auto noted = [&degradeResult](singbox::FidelityScope scope,
                                            std::string_view subject) {
            return std::any_of(
                degradeResult.fidelity.begin(), degradeResult.fidelity.end(),
                [&](const singbox::FidelityNote& n) {
                    return n.scope == scope && n.subject == subject;
                });
        };
        check(noted(singbox::FidelityScope::Node, "重复"),
              "重名节点进账本（Node / 未支持）");
        check(noted(singbox::FidelityScope::Rule, "GEOIP,telegram"),
              "非法 GEOIP 代码进账本（Rule / 未支持）");
        const std::string summary = singbox::FidelitySummary(degradeResult.fidelity);
        check(summary.find("跳过 1 个节点") != std::string::npos &&
                  summary.find("忽略 1 条规则") != std::string::npos,
              "输入非法导致的条目消失同样计入摘要");
    }

    if (failures != 0) {
        std::println(stderr, "test_singbox: {} 项断言失败", failures);
        return 1;
    }
    std::println("test_singbox: ok");
    return 0;
}
