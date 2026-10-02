// test_singbox.cpp — clashflux.singbox 编译器的最小单元测试。
//
// 覆盖：节点协议映射（ss/vmess/hysteria2/tuic/未知协议）、代理组降级与成员
// 过滤、规则直映射 + REJECT action + GEOIP/GEOSITE 规则集 + MATCH final、
// resolve 前置规则、三模式
// clash_mode 前置规则、原生 sing-box JSON 直通、空订阅最小配置。
// 断言风格与 test_vpn 一致（check 计数 + main）。
#include <cassert>
#include <cstdio>

import std;
import nlohmann.json;
import clashflux.singbox;
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

} // namespace

int main(int argc, char** argv) {
    testProxyValues();
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
        const std::string dir = "/tmp/clashflux-test-ruleset";
        const auto geoipPath = std::filesystem::path(dir) / "geoip-cn.srs";
        const auto geositePath = std::filesystem::path(dir) / "geosite-cn.srs";
        std::filesystem::create_directories(dir);
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
        realOptions.profileYaml = std::string(std::istreambuf_iterator<char>(input),
                                              std::istreambuf_iterator<char>());
        const auto realResult = singbox::compileConfig(realOptions);
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
