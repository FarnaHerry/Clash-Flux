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
import nlohmann.json;
import clashflux.vpn;

namespace singbox {

// 保真度级别（见 docs/singbox-layers-and-fidelity.md §2）：翻译层对订阅里每个条目
// （协议 / 组 / 规则 / 字段）的映射结果。Exact 不产生条目，因此只出现后两级。
export enum class Fidelity {
    Exact,        // 1:1 映射
    Approx,       // 语义近似，或被忽略的字段（fallback → urltest、lazy 被丢弃…）
    Unsupported,  // 跳过 / 拒绝（未知协议、REJECT 成员、不支持的规则类型…）
};

// 条目作用的对象类型：汇总摘要（FidelitySummary）与页面级渲染按它分类，
// 不要靠解析 detail 文本。
export enum class FidelityScope {
    Node,
    Group,
    Rule,
    Dns,
    Field,
};

// 结构化保真度条目：subject 是订阅里出现的名字（节点 / 组名 / 规则类型 / 字段），
// detail 可直接展示给用户，action 是可选处置建议。UI 据此决定角标 / 灰显 / 原因，
// 不要再靠解析 warnings 文本。
export struct FidelityNote {
    FidelityScope scope = FidelityScope::Field;
    Fidelity level = Fidelity::Approx;
    std::string subject;
    std::string detail;
    std::string action;
    std::string sourceId;

    bool operator==(const FidelityNote&) const = default;
};

// 仅编译器生成的 GEO 资源进入应用预取；原生 JSON 的 rule_set 仍由内核管理。
export struct RuleSetResource {
    std::string tag;
    std::string url;
};

export struct SourceObject {
    std::string sourceId;
    std::string sourceName;
    vpn::TargetKind kind = vpn::TargetKind::Node;
    std::string objectId; // 来源内原名，删除/改名不自动改绑
    std::string tag;
    bool operator==(const SourceObject&) const = default;
};
export struct ProfileSource {
    std::string id;
    std::string name;
    std::string content;
    bool available = true;
};

export struct CompileResult {
    std::string json;                    // sing-box 配置 JSON；失败为空
    std::string error;                   // 致命错误（YAML 解析失败等）
    // 自由文本投影（历史消费方与内核启动诊断沿用；每条 = fidelity[i].detail）
    std::vector<std::string> warnings;
    // 结构化保真度账本（不阻断启动）
    std::vector<FidelityNote> fidelity;
    std::vector<RuleSetResource> ruleSetResources;
    std::vector<SourceObject> sourceObjects;
    std::vector<std::string> participatingSources;
    std::uint64_t planRevision = 0;
};

// 把账本汇总成一行提示（"跳过 3 个节点 · 降级 1 个组"）；无降级返回空串。
// 只报计数、不报明细——明细由设置页「配置保真度」呈现（见
// docs/singbox-layers-and-fidelity.md §2）。
export std::string FidelitySummary(const std::vector<FidelityNote>& notes);

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
    bool tunInbound = false;                     // 生成 TUN inbound（Android/桌面
                                                 // 均按当前 TUN 状态决定；Android
                                                 // 由 VpnService 提供 fd）
    bool speedTestOnly = false;                  // 仅测速时关闭 URLTest 自动巡检
    bool tunStrictRoute = true;                  // Android VpnService 侧保持 false（严格
                                                 // 路由会截断系统级分流）
    bool ipv6 = true;                            // DNS 与 TUN 是否允许 IPv6
    std::string ruleSetDir;                      // 非空时 GEOIP/GEOSITE .srs 本地命中即以
                                                 // local rule_set 生成（core_store
                                                 // 预取缓存目录；未命中回落 remote）
    std::string mainSourceName;
    std::vector<ProfileSource> auxiliarySources; // 桌面：只由启用的显式规则引用
    std::uint64_t planRevision = 0;
    std::string mainConnectionId;                // 本次加载的 sing-box profile 连接 ID
    std::vector<vpn::RouteRule> globalRules;      // 优先于模式和订阅的全局连接规则
    std::vector<NativeConnection> nativeConnections;
    std::vector<std::string> tunExcludeAddresses; // 原生 VPN 服务器传输地址（IP/CIDR）
};

// 编译 options.profileYaml 为 sing-box 配置 JSON。可为空（最小可用配置）、
// Clash YAML，或以 "{" 开头的原生 sing-box JSON（直通并合并托管设置）。
export CompileResult compileConfig(const CompileOptions& options);

// sing-box 二进制规则集（.srs）以 "SRS" 魔数开头。0 字节、被写入错误内容
// （例如把 HTTP 错误页落盘）或非规则集内容的缓存文件会让内核在启动期直接
// FATAL（parse rule-set: read rule: unexpected EOF），因此缓存命中与预取
// 替换前都必须先校验；仅校验文件头，不替代内核完整解析。
export bool RuleSetCacheValid(const std::filesystem::path& path);
// 返回应用自有 GEO tag 的固定下载来源；非法 tag 返回空，不能拼成路径。
export std::string BuiltinRuleSetUrl(std::string_view tag);

// 把编译产物拍成代理页使用的 /proxies 形状快照：
//   * 组出站（selector/urltest）→ {type, now, all, selectable}
//   * 其余出站 → {type, udp}（代理页卡片第二行的协议/UDP 元数据；内核没跑时的
//     "订阅预览"没有真实 /proxies，这里是唯一数据源）
// savedSelection(tag) 返回该组持久化的选中项（空串 = 没有）；命中成员时写回
// `now`，并同步落到 `config` 里该 selector 的 `default`（写盘配置与 UI 一致）。
// config 为空或形态不对时返回只有空 proxies 对象的快照。
export inline std::string BuildProxySnapshot(
    nlohmann::json& config,
    const std::function<std::string(const std::string&)>& savedSelection = {}) {
    nlohmann::json proxies = nlohmann::json::object();
    if (!config.is_object() || !config.contains("outbounds") ||
        !config["outbounds"].is_array()) {
        return nlohmann::json{{"proxies", proxies}}.dump();
    }
    // 先补每个节点自己的条目：代理页卡片第二行的协议/UDP 读的就是它。只写组
    // 条目时（旧行为）parseProxies 在 all 里找不到节点对象，预览态永远是空的。
    for (const auto& outbound : config["outbounds"]) {
        const std::string type = outbound.value("type", "");
        if (type == "selector" || type == "urltest") continue;  // 组条目下面生成
        const std::string tag = outbound.value("tag", "");
        if (tag.empty() || type.empty()) continue;
        // 只写真实存在的类型；UDP 能力是服务端策略、客户端快照里没有，
        // 不在这里臆测（只有内核 /proxies 真给了 udp 才由 UI 显示）。
        proxies[tag] = {{"type", type}};
    }
    for (auto& outbound : config["outbounds"]) {
        const std::string type = outbound.value("type", "");
        if (type != "selector" && type != "urltest") continue;
        const std::string group = outbound.value("tag", "");
        if (group.empty()) continue;
        const auto members =
            outbound.value("outbounds", nlohmann::json::array());
        std::string current = outbound.value("default", "");
        const std::string saved =
            savedSelection ? savedSelection(group) : std::string{};
        if (type == "selector" && !saved.empty()) {
            for (const auto& member : members) {
                if (member == saved) {
                    outbound["default"] = saved;
                    current = saved;
                    break;
                }
            }
        }
        if (current.empty() && !members.empty()) {
            current = members.front().get<std::string>();
        }
        proxies[group] = {{"type", type}, {"now", current}, {"all", members},
                          {"selectable", type == "selector"}};
    }
    return nlohmann::json{{"proxies", proxies}}.dump();
}

} // namespace singbox
