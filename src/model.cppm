// model.cppm — 跨层共享的领域数据结构（无依赖，供 db / persistence / store 共用）。
//
// 单独成模块是为了打破 db ↔ persistence 的模块环：持久化服务需要 Profile
// 类型，而 db.cpp（clashflux.db 的实现单元）又要转发到持久化服务。
export module clashflux.model;

import std;

namespace model {

export struct Profile {
    std::int64_t id = 0;         // 0 = 未保存过
    std::string name;
    std::string url;             // 订阅 URL；本地导入为空
    std::string file;            // profiles/ 下的 YAML 文件名（<id>.yaml）
    bool selected = false;       // 当前启用
    std::int64_t updatedAt = 0;  // 上次成功拉取/导入时间（Unix 秒）
    std::string error;           // 上次拉取错误（成功清空）
    // ---- 订阅选项（订阅弹窗编辑；下载行为在更新时生效）----
    std::string type = "remote";    // remote/local 或原生连接类型（如 pptp/openvpn）
    std::string description;        // 描述
    int timeoutSecs = 60;           // HTTP 请求超时（秒；<=0 回落 60）
    int intervalMins = 0;           // 更新间隔（分钟；0 = 不自动更新）
    bool autoUpdate = false;        // 允许自动更新（需 intervalMins > 0）
    bool useSystemProxy = false;    // 使用系统代理更新（环境变量代理）
    bool useCoreProxy = false;      // 使用内核代理更新（127.0.0.1:mixedPort）
    bool allowInvalidCert = false;  // 允许无效证书（危险）
    // ---- 原生连接订阅（PPTP/OpenVPN/WireGuard 等；由 type 决定解释）----
    std::string nativeConfig;
    std::string nativeRoutes;
    // ---- 订阅响应头解析（更新时从 subscription-userinfo / profile-web-page-url
    // 提取；本地导入恒空/0）----
    std::string homepage;           // 订阅提供方首页（右键「首页」跳转）
    std::int64_t usedBytes = 0;     // 已用流量（upload + download）
    std::int64_t totalBytes = 0;    // 总流量（0 = 未知，卡片不显示流量条）

    bool operator==(const Profile&) const = default;  // State 变更检测
};

} // namespace model
