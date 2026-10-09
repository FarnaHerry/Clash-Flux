// stream.cppm — clashflux.stream：clash_api 推送流（接口模块）。
//
// 三条 WebSocket 通道（clash_api 的推送端点）：
//   /logs?level=<level>     内核日志（{"type":..,"payload":..} 逐条推）
//   /traffic                每秒流量帧（{"up":N,"down":N}，字节/秒）
//   /connections            连接快照（全量 JSON，约每秒一帧）
// IXWebSocket 自管内部线程，应用侧不拥有线程：回调（IX 线程）只把事件推入
// 互斥保护的队列/最新值槽；写入后经 addStreamUpdateObserver 通知 UI 线程，
// UI 收到通知再 drain（不再按节拍轮询）。
// 鉴权用 ?token=<secret> 查询参数（clash 系内核对 WS 的通用鉴权方式）。
export module clashflux.stream;

import std;

namespace stream {

export struct LogLine {
    std::string level;     // info / warning / error / debug / silent（原文 type）
    std::string payload;
    std::int64_t at = 0;   // Unix 秒
};

// 内核、应用诊断与 Android libbox 共用 core/logs.log、历史与增量队列。
// 两个写入口保留调用方语义，UI 不再区分来源；旧分离日志不迁移、不读取。
export void logCore(std::string level, std::string payload) noexcept;
export void logApplication(std::string level, std::string payload) noexcept;
export std::vector<LogLine> logHistory();
export std::vector<LogLine> drainLogs();
export void clearLogs();

export struct TrafficPoint {
    std::int64_t up = 0;    // 上传速率，字节/秒
    std::int64_t down = 0;  // 下载速率，字节/秒
    std::int64_t at = 0;    // Unix 秒

    bool operator==(const TrafficPoint&) const = default;
};

// 推送流里发生变化的那一路。观察者据此只 drain 自己关心的队列。
export enum class StreamKind {
    Logs,
    Traffic,
    Connections,
};

export class CoreStreams {
public:
    CoreStreams();
    ~CoreStreams();

    CoreStreams(const CoreStreams&) = delete;
    CoreStreams& operator=(const CoreStreams&) = delete;

    // 立即返回（握手在 IX 线程异步进行）；重复调用先 stop 旧连接。
    void start(const std::string& wsBase, const std::string& secret,
               const std::string& logLevel);
    void stop();

    bool logsOpen() const;
    bool trafficOpen() const;
    bool connectionsOpen() const;

    // UI 线程泵：取走内核日志增量。历史由 stream 模块持久化，重启连接不会清空。
    std::vector<LogLine> drainLogs();
    // 取最新流量帧；无新帧返回 false（槽位取走后清空）。
    bool takeTraffic(TrafficPoint& out);
    // 取最新有效连接总量。解码在 WebSocket 工作线程完成，首页只读整数；
    // 消费标记不影响连接页的原文快照。非法帧不会替换有效总量。
    bool takeConnectionTotals(std::int64_t& upload, std::int64_t& download);
    // 读取最近一次连接快照但不消费。页面切换/重新挂载后仍能立即显示
    // 当前连接，而不必等待下一帧 WebSocket 推送。
    bool readConnections(std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- 变化通知（推送线程 → UI 线程）----------------------------------------
// IX 线程（或任意调用 logCore/logApplication 的线程）在写入数据后通知观察者；
// 观察者通常在 UI 层把回调 Post 回 UI 线程，处理完再 Acknowledge(kind)。
// 同一路在同一时刻最多只有一个未确认通知（内部合并），所以推送再密也只是一个
// 待处理回调，不会淹没 UI。观察者在任意线程被调用：不得直接触碰 State/View。
export using StreamUpdateObserver = std::function<void(StreamKind)>;
export [[nodiscard]] std::uint64_t addStreamUpdateObserver(StreamUpdateObserver observer);
export void removeStreamUpdateObserver(std::uint64_t id);
// 观察者处理完该路数据后调用，允许下一次通知。
export void acknowledgeStreamUpdate(StreamKind kind);

} // namespace stream
