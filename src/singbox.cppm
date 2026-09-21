// singbox.cppm — clashflux.singbox：Clash 订阅 YAML → sing-box JSON 编译器。
//
// sing-box 不认识 Clash YAML；全平台（桌面 spawn 进程 / Android libbox）共用
// 本编译器把订阅（或手写配置、原生 sing-box JSON）合成为 sing-box 运行时
// 配置。clash_mode 取小写 rule/global/direct（经 clash_api mode_list 自定义，
// 与 UI 硬编码字符串一致）。保真度第一期：基础协议/组/规则 + GEOIP/GEOSITE
// → .srs 规则集；不支持的条目（RULE-SET、小众协议等）以警告返回而不是静默
// 丢弃，调用方呈现给用户。
export module clashflux.singbox;

import std;
import clashflux.vpn;

namespace singbox {

export struct CompileResult {
    std::string json;                    // sing-box 配置 JSON；失败为空
    std::string error;                   // 致命错误（YAML 解析失败等）
    std::vector<std::string> warnings;   // 保真度降级报告（不阻断启动）
};

// 原生引擎的运行时快照。未连接的声明也必须传入，使规则保持拒绝而不回落
// 主出口；internalRoutes 是隐式规则，不加入 TUN 排除地址。OpenVPN 的
// nativeConfig 是 .ovpn 原文，编译器会把它翻译为 sing-box endpoint；PPTP
// 则只使用 interfaceName/gateway 走系统接口补偿。
export struct NativeConnection {
    std::string id;
    std::string interfaceName;
    std::vector<std::string> internalRoutes;
    bool connected = false;
    vpn::ConnectionKind kind = vpn::ConnectionKind::Pptp;
    std::string transportAddress;
    std::string nativeConfig;
};

export struct CompileOptions {
    std::string profileYaml;                     // 订阅/手写配置原文（Clash YAML、
                                                 // 原生 sing-box JSON 或空）
    std::string controller = "127.0.0.1:29097";  // clash_api external_controller
    std::string secret;                          // clash_api secret（可空）
    int mixedPort = 7899;
    std::string mode = "rule";                   // rule / global / direct
    bool allowLan = false;
    std::string logLevel = "info";               // silent/error/warning/info/debug
    bool tunInbound = false;                     // 生成 TUN inbound（Android 恒真；
                                                 // 桌面 = tunEnabled，需 root/CAP_NET_ADMIN）
    bool speedTestOnly = false;                  // 仅测速时关闭 URLTest 自动巡检
    bool tunStrictRoute = true;                  // Android VpnService 侧保持 false（严格
                                                 // 路由会截断系统级分流）
    bool ipv6 = true;                            // DNS 与 TUN 是否允许 IPv6
    std::string ruleSetDir;                      // 非空时 GEOIP/GEOSITE .srs 本地命中即以
                                                 // local rule_set 生成（core_store
                                                 // 预取缓存目录；未命中回落 remote）
    std::string mainConnectionId;                // 本次加载的 sing-box profile 连接 ID
    std::vector<vpn::RouteRule> globalRules;      // 优先于模式和订阅的全局连接规则
    std::vector<NativeConnection> nativeConnections;
    std::vector<std::string> tunExcludeAddresses; // 原生 VPN 服务器传输地址（IP/CIDR）
};

// 编译 options.profileYaml 为 sing-box 配置 JSON。可为空（最小可用配置）、
// Clash YAML，或以 "{" 开头的原生 sing-box JSON（直通并合并托管设置）。
export CompileResult compileConfig(const CompileOptions& options);

} // namespace singbox
