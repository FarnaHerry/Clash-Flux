// stream.cppm — clashflux.stream：clash_api 推送流（接口模块）。
//
// 三条 WebSocket 通道（clash_api 的推送端点）：
//   /logs?level=<level>     内核日志（{"type":..,"payload":..} 逐条推）
//   /traffic                每秒流量帧（{"up":N,"down":N}，字节/秒）
//   /connections            连接快照（全量 JSON，约每秒一帧）
// IXWebSocket 自管内部线程，应用侧不拥有线程：回调（IX 线程）只把事件推入
// 互斥保护的队列/最新值槽，UI 经 PollWhile 泵按节拍 drain（见 store 层）。
// 鉴权用 ?token=<secret> 查询参数（clash 系内核对 WS 的通用鉴权方式）。
export module clashflux.stream;

import std;

namespace stream {

export struct LogLine {
    std::string level;     // info / warning / error / debug / silent（原文 type）
    std::string payload;
    std::int64_t at = 0;   // Unix 秒
};

// 内核日志与应用自身的诊断日志分别持久化；Android 的 libbox CommandClient
// 与桌面 clash_api 推送都写入同一条内核日志流。history 用于页面重进恢复，
// drain 只消费增量队列。
export void logCore(std::string level, std::string payload) noexcept;
export std::vector<LogLine> coreLogHistory();
export std::vector<LogLine> drainCoreLogs();
export void clearCoreLogs();

export void logApplication(std::string level, std::string payload) noexcept;
export std::vector<LogLine> applicationLogHistory();
export std::vector<LogLine> drainApplicationLogs();
export void clearApplicationLogs();

export struct TrafficPoint {
    std::int64_t up = 0;    // 上传速率，字节/秒
    std::int64_t down = 0;  // 下载速率，字节/秒
    std::int64_t at = 0;    // Unix 秒

    bool operator==(const TrafficPoint&) const = default;
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
    // 取最新连接快照（原始 JSON 文本）；消费新帧标记但保留快照缓存，
    // 无新帧返回 false。
    bool takeConnections(std::string& out);
    // 读取最近一次连接快照但不消费。页面切换/重新挂载后仍能立即显示
    // 当前连接，而不必等待下一帧 WebSocket 推送。
    bool readConnections(std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stream
