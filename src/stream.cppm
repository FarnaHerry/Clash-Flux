// stream.cppm — clashflux.stream：mihomo 推送流（接口模块）。
//
// 三条 WebSocket 通道（mihomo external-controller 的推送端点）：
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

    // UI 线程泵：取走累计日志行（一次取空）。
    std::vector<LogLine> drainLogs();
    // 取最新流量帧；无新帧返回 false（槽位取走后清空）。
    bool takeTraffic(TrafficPoint& out);
    // 取最新连接快照（原始 JSON 文本）；无新帧返回 false。
    bool takeConnections(std::string& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stream
