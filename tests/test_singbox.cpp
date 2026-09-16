// test_singbox.cpp — clashflux.singbox 编译器的最小单元测试。
//
// 覆盖：节点协议映射（ss/vmess/hysteria2/tuic/未知协议）、代理组降级与成员
// 过滤、规则直映射 + REJECT action + GEOIP 规则集 + MATCH final、三模式
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
  - GEOIP,CN,手动选择
  - DOMAIN,blocked.net,REJECT
  - RULE-SET,some-provider,手动选择
  - PROCESS-NAME,ssh,手动选择
  - MATCH,自动选择
)yaml";

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::println(stderr, "FAIL: {}", what);
        ++failures;
    }
}

} // namespace

int main() {
    // ---- 订阅编译 -----------------------------------------------------------
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

    // 组映射：url-test → urltest，REJECT 成员被过滤。
    const json autoGroup = findOutbound("自动选择");
    check(autoGroup.value("type", "") == "urltest", "url-test → urltest");
    check(autoGroup.value("interval", "") == "300s", "interval 秒 → 时长");
    const json members = autoGroup.value("outbounds", json{});
    check(std::find(members.begin(), members.end(), "REJECT") == members.end(),
          "REJECT 成员被移除");
    check(std::find(members.begin(), members.end(), "香港 01") != members.end(),
          "节点成员保留");
    check(std::none_of(
              result.warnings.begin(), result.warnings.end(),
              [](const std::string& w) { return w.find("手动选择") != std::string::npos; }),
          "select 组无降级警告");

    // 路由：final = MATCH 目标；clash_mode 前置；REJECT → action；GEOIP → rule_set。
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
    check(rules[2].value("clash_mode", "") == "direct" &&
              rules[2].value("outbound", "") == "DIRECT",
          "direct 模式前置规则");
    check(rules[3].value("clash_mode", "") == "global", "global 模式前置规则");
    bool sawReject = false;
    bool sawGeoip = false;
    bool sawCidr = false;
    for (const auto& rule : rules) {
        if (rule.value("action", "") == "reject" &&
            rule.value("domain", json{}) == json{"blocked.net"}) {
            sawReject = true;
        }
        if (rule.contains("rule_set") &&
            rule["rule_set"] == json{"geoip-cn"}) {
            sawGeoip = true;
        }
        if (rule.value("ip_cidr", json{}) == json{"192.168.0.0/16"}) {
            sawCidr = true;
        }
    }
    check(sawReject, "REJECT 目标 → action:reject");
    check(sawGeoip, "GEOIP CN → rule_set");
    check(sawCidr, "IP-CIDR 直映射");
    check(config["route"]["rule_set"].is_array() &&
              config["route"]["rule_set"][0]["tag"] == "geoip-cn" &&
              config["route"]["rule_set"][0]["url"].get<std::string>().find(
                  "meta-rules-dat/sing/geo/geoip/cn.srs") != std::string::npos,
          "geoip-cn 远程 .srs 规则集");
    check(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) {
                          return w.find("RULE-SET") != std::string::npos;
                      }),
          "RULE-SET 产生警告");
    check(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) {
                          return w.find("PROCESS-NAME") != std::string::npos;
                      }),
          "不支持的规则类型产生警告");

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
        {.match = vpn::MatchKind::DomainSuffix, .pattern = "corp.example", .connectionId = "profile:pptp", .priority = 10},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "PUBLIC.CORP.EXAMPLE.", .connectionId = "profile:main", .priority = 10},
        {.match = vpn::MatchKind::ExactIp, .pattern = "10.42.0.9", .connectionId = "profile:main", .priority = 20},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "offline.example", .connectionId = "profile:offline", .priority = 9},
        {.match = vpn::MatchKind::ExactDomain, .pattern = "other-core.example", .connectionId = "profile:other-core", .priority = 8},
        {.match = vpn::MatchKind::ExactIp, .pattern = "2001:db8::1", .connectionId = "profile:main", .priority = 7},
    };
    compensation.tunExcludeAddresses = {"198.51.100.7", "198.51.100.7/32"};
#if !defined(__linux__) || defined(__ANDROID__)
    check(!singbox::compileConfig(compensation).error.empty(),
          "缺少补偿后端的平台必须拒绝原生 VPN 与 TUN 共存");
    compensation.tunInbound = false;
#endif
    auto compensationResult = singbox::compileConfig(compensation);
    check(compensationResult.error.empty(), "主 VPN 补偿配置编译成功");
    const auto compensationConfig = json::parse(compensationResult.json);
    const auto& compensationRules = compensationConfig["route"]["rules"];
    check(compensationRules[0]["action"] == "sniff" && compensationRules[1]["action"] == "hijack-dns",
          "补偿规则位于 sniff/DNS 之后");
    check(compensationRules[2]["ip_cidr"] == json{"10.42.0.9/32"} &&
              compensationRules[2]["outbound"] == "chosen", "高优先级全局规则可覆盖原生内网");
    check(compensationRules[3]["domain"] == json{"public.corp.example"} &&
              compensationRules[3]["outbound"] == "chosen", "同优先级精确域名先于后缀，主 VPN 选择订阅 final");
    const std::string nativeTag = compensationRules[4]["outbound"];
    bool hasBoundNative = false;
    for (const auto& outbound : compensationConfig["outbounds"]) {
        if (outbound.value("tag", "") == nativeTag) {
            hasBoundNative = outbound.value("type", "") == "direct" &&
                             outbound.value("bind_interface", "") == "ppp7";
        }
    }
    check(hasBoundNative, "原生 VPN 域名规则使用绑定 ppp 接口的 direct outbound");
    check(compensationRules[5]["action"] == "reject" && compensationRules[6]["action"] == "reject",
          "离线原生连接和非主 sing-box 目标拒绝，不能泄露到主出口");
    check(compensationRules[7]["ip_cidr"] == json{"2001:db8::1/128"}, "IPv6 精确 IP 全局规则保留");
    check(compensationRules[8]["ip_cidr"] == json{"10.42.0.0/16"} &&
              compensationRules[8]["action"] == "reject" &&
              compensationRules[9]["ip_cidr"] == json{"10.0.0.0/8"}, "内部 CIDR 按最长前缀且在全局之后匹配");
    check(compensationRules[10]["clash_mode"] == "direct" && compensationRules[11]["clash_mode"] == "global",
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
    check(compensationResult.warnings.size() == 2, "不可用连接各报告一次警告");

    compensation.nativeConnections[0].connected = false;
    const auto disconnected = json::parse(singbox::compileConfig(compensation).json);
    check(disconnected["route"]["rules"][4]["action"] == "reject" &&
              disconnected["route"]["rules"][9]["action"] == "reject", "断开后的重新编译撤销接口出口并保留拒绝规则");
    compensation.nativeConnections[0].connected = true;
    compensation.nativeConnections[0].interfaceName.clear();
    const auto missingInterface = json::parse(singbox::compileConfig(compensation).json);
    check(missingInterface["route"]["rules"][4]["action"] == "reject", "连接状态成功但无接口不能回落直连");
    compensation.nativeConnections[0].connected = false;

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
    check(rawConfig["route"]["rules"][2]["outbound"] == "local" &&
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

    // ---- 本地规则集命中 -------------------------------------------------------
    {
        std::filesystem::create_directories("/tmp/clashflux-test-ruleset");
        { std::ofstream out("/tmp/clashflux-test-ruleset/geoip-cn.srs", std::ios::binary); out << "stub"; }
        singbox::CompileOptions localOptions = options;
        localOptions.ruleSetDir = "/tmp/clashflux-test-ruleset";
        const auto localResult = singbox::compileConfig(localOptions);
        const json localConfig = json::parse(localResult.json);
        bool sawLocal = false;
        for (const auto& rs : localConfig["route"]["rule_set"]) {
            if (rs.value("tag", "") == "geoip-cn") {
                sawLocal = rs.value("type", "") == "local" &&
                           rs.value("path", "").find("geoip-cn.srs") != std::string::npos;
            }
        }
        check(sawLocal, "ruleSetDir 命中时 GEOIP 以 local rule_set 生成");
    }

    // ---- 空订阅最小配置 -------------------------------------------------------
    options.profileYaml.clear();
    const auto empty = singbox::compileConfig(options);
    check(empty.error.empty(), "空订阅编译无错误");
    const json emptyConfig = json::parse(empty.json);
    check(emptyConfig["route"]["final"] == "DIRECT", "空订阅 final = DIRECT");
    check(emptyConfig["outbounds"].size() == 1, "空订阅仅 DIRECT outbound");

    if (failures != 0) {
        std::println(stderr, "test_singbox: {} 项断言失败", failures);
        return 1;
    }
    std::println("test_singbox: ok");
    return 0;
}
