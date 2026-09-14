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

namespace {

using nlohmann::json;

const std::string kFixture = R"yaml(
mixed-port: 7890
mode: rule
log-level: info
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
    check(us.value("transport", json{})["type"] == "websocket", "vmess ws transport");
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
              config["route"]["rule_set"][0]["url"].get<std::string>().find("geoip-cn.srs") !=
                  std::string::npos,
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
