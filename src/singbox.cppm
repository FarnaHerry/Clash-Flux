// singbox.cppm — clashflux.singbox：Clash 订阅 YAML → sing-box JSON 编译器。
//
// sing-box 不认识 Clash YAML；全平台（桌面 spawn 进程 / Android libbox）共用
// 本编译器把订阅（或手写配置、原生 sing-box JSON）合成为 sing-box 运行时
// 配置。clash_mode 取小写 rule/global/direct（经 clash_api mode_list 自定义，
// 与 UI 硬编码字符串一致）。保真度第一期：基础协议/组/规则 + GEOIP → 在线
// .srs 规则集；不支持的条目（RULE-SET、小众协议等）以警告返回而不是静默
// 丢弃，调用方呈现给用户。
export module clashflux.singbox;

import std;

namespace singbox {

export struct CompileResult {
    std::string json;                    // sing-box 配置 JSON；失败为空
    std::string error;                   // 致命错误（YAML 解析失败等）
    std::vector<std::string> warnings;   // 保真度降级报告（不阻断启动）
};

export struct CompileOptions {
    std::string profileYaml;                     // 订阅/手写配置原文（Clash YAML、
                                                 // 原生 sing-box JSON 或空）
    std::string controller = "127.0.0.1:9097";  // clash_api external_controller
    std::string secret;                          // clash_api secret（可空）
    int mixedPort = 7899;
    std::string mode = "rule";                   // rule / global / direct
    bool allowLan = false;
    std::string logLevel = "info";               // silent/error/warning/info/debug
    bool tunInbound = false;                     // 生成 TUN inbound（Android 恒真；
                                                 // 桌面 = tunEnabled，需 root/CAP_NET_ADMIN）
    bool tunStrictRoute = true;                  // Android VpnService 侧保持 false（严格
                                                 // 路由会截断系统级分流）
    bool ipv6 = true;                            // Android VpnService 目前只建 IPv4 TUN
    std::string ruleSetDir;                      // 非空时 GEOIP .srs 本地命中即以
                                                 // local rule_set 生成（core_store
                                                 // 预取缓存目录；未命中回落 remote）
};

// 编译 options.profileYaml 为 sing-box 配置 JSON。可为空（最小可用配置）、
// Clash YAML，或以 "{" 开头的原生 sing-box JSON（直通并合并托管设置）。
export CompileResult compileConfig(const CompileOptions& options);

} // namespace singbox
