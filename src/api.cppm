// api.cppm — clashflux.api：sing-box clash_api REST 客户端（接口模块）。
//
// 所有方法同步阻塞、线程安全（每次调用独立 curl easy handle），必须由 UI 经
// RunOnTaskThread 派到任务线程调用；UI 线程禁止直接调。curl 头只进实现单元。
//
// 端点约定（sing-box clash_api 与 mihomo 兼容的子集）：
//   GET    /version                     内核版本
//   GET    /configs                     运行配置快照（mode 等；字段非全量）
//   PATCH  /configs                     增量改运行配置（{"mode":"global"} 等；
//                                        sing-box 仅支持 mode 等少数字段）
//   GET    /proxies                     全部代理与策略组
//   PUT    /proxies/{group}             策略组切换节点（{"name": "..."}）
//   GET    /proxies/{name}/delay        内核代理延迟测速（?url=&timeout=）
//   GET    /connections                 连接快照（REST 轮询用；推送走 WS）
//   DELETE /connections/{id}            关闭单条连接
//   DELETE /connections                 关闭全部连接
// mihomo 专属端点（/group/{name}/delay 聚合测速、/providers/*、PUT /configs、
// /restart）已随 mihomo 移除；组测速由 groupDelay 内部 fan-out 实现。
export module clashflux.api;

import std;

namespace api {

export struct ApiResult {
    bool ok = false;
    long status = 0;          // HTTP 状态码；传输失败为 0
    std::string body;         // 响应体（错误时也可能有，错误是 JSON message）
    std::string error;        // 传输层错误描述（curl）；HTTP 错误时从 body 提取 message
    // 响应头（仅订阅下载填充；名字统一小写，重定向取最后一跳）。
    std::map<std::string, std::string> headers;
};

// REST 客户端：baseUrl 形如 "http://127.0.0.1:29097"，secret 为 Bearer token（可空）。
export class ClashApi {
public:
    ClashApi(std::string baseUrl, std::string secret);
    ~ClashApi();
    ClashApi(const ClashApi&) = delete;
    ClashApi& operator=(const ClashApi&) = delete;

    void setEndpoint(std::string baseUrl, std::string secret);
    const std::string& baseUrl() const;

    ApiResult version();
    ApiResult configs();
    ApiResult patchConfigs(const std::string& jsonBody);

    ApiResult proxies();
    ApiResult selectProxy(const std::string& group, const std::string& name);
    // 延迟测速：ok 时 body 是 {"delay":N}；超时/失败 ok=false。
    ApiResult proxyDelay(const std::string& name, const std::string& testUrl, int timeoutMs);

    ApiResult connections();
    ApiResult closeConnection(const std::string& id);
    ApiResult closeAllConnections();

    // 订阅下载选项：超时 / 证书校验 / 代理三态（指定代理 > 系统环境代理 >
    // 强制直连）。使用内核代理时由调用方拼 http://127.0.0.1:<mixedPort>。
    struct DownloadOptions {
        long timeoutSecs = 60;
        bool allowInvalidCert = false;
        std::string proxyUrl;        // 非空 = 经该代理（如 http://127.0.0.1:7899）
        bool allowProxyEnv = false;  // proxyUrl 为空时是否允许环境变量代理
    };

    // 订阅下载：GET url 落盘到 dest（覆盖写）。options 必传（模块接口下
    // 默认参数 + 花括号临时量的组合 GCC 不接受）。
    ApiResult downloadToFile(const std::string& url, const std::filesystem::path& dest,
                             const DownloadOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace api
