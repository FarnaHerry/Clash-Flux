// stream.cpp — clashflux.stream 实现单元（IXWebSocket）。
//
// 三条通道共享一个实现：IX 回调只写内部槽位（持锁），UI 泵 drain。
// 日志队列上限 2000 行（UI 跟不上时丢最旧的，保住内存）。
module;

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

module clashflux.stream;

import std;
import nlohmann.json;
import clashflux.utils;

namespace stream {
namespace {

constexpr std::size_t kMaxLogLines = 2000;

std::string withToken(const std::string& url, const std::string& secret) {
    if (secret.empty()) return url;
    return appendQuery(url, {{"token", secret}});
}

struct Channel {
    ix::WebSocket socket;
    std::atomic<bool> open{false};

    void start(const std::string& url) {
        socket.disableAutomaticReconnection();
        socket.setUrl(url);
        socket.setHandshakeTimeout(5);
        socket.start();
    }

    void stop() {
        open.store(false);
        socket.stop();
    }
};

} // namespace

struct CoreStreams::Impl {
    Channel logs;
    Channel traffic;
    Channel connections;

    std::mutex mutex;
    std::vector<LogLine> logLines;
    TrafficPoint latestTraffic;
    bool trafficDirty = false;
    std::string latestConnections;
    bool connectionsDirty = false;

    ~Impl() { stopAll(); }

    void stopAll() {
        logs.stop();
        traffic.stop();
        connections.stop();
    }

    void pushLog(const std::string& text) {
        LogLine line;
        line.at = nowUnix();
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object()) {
            line.level = j.value("type", "info");
            line.payload = j.value("payload", "");
        } else {
            line.level = "info";
            line.payload = text;
        }
        std::lock_guard lock(mutex);
        if (logLines.size() >= kMaxLogLines) {
            logLines.erase(logLines.begin(),
                           logLines.begin() +
                               static_cast<std::ptrdiff_t>(logLines.size() - kMaxLogLines + 1));
        }
        logLines.push_back(std::move(line));
    }

    void pushTraffic(const std::string& text) {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return;
        std::lock_guard lock(mutex);
        latestTraffic.up = j.value("up", std::int64_t{0});
        latestTraffic.down = j.value("down", std::int64_t{0});
        latestTraffic.at = nowUnix();
        trafficDirty = true;
    }

    void pushConnections(const std::string& text) {
        std::lock_guard lock(mutex);
        latestConnections = text;
        connectionsDirty = true;
    }
};

CoreStreams::CoreStreams() : impl_(std::make_unique<Impl>()) {
    static std::once_flag flag;
    std::call_once(flag, [] { ix::initNetSystem(); });
}

CoreStreams::~CoreStreams() = default;

void CoreStreams::start(const std::string& wsBase, const std::string& secret,
                        const std::string& logLevel) {
    impl_->stopAll();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->logLines.clear();
        impl_->trafficDirty = false;
        impl_->connectionsDirty = false;
    }

    Impl* impl = impl_.get();
    impl_->logs.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->logs.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushLog(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->logs.open.store(false);
                    break;
                default: break;
            }
        });
    impl_->traffic.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->traffic.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushTraffic(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->traffic.open.store(false);
                    break;
                default: break;
            }
        });
    impl_->connections.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->connections.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushConnections(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->connections.open.store(false);
                    break;
                default: break;
            }
        });

    impl_->logs.start(withToken(appendQuery(wsBase + "/logs", {{"level", logLevel}}),
                                secret));
    impl_->traffic.start(withToken(wsBase + "/traffic", secret));
    impl_->connections.start(withToken(wsBase + "/connections", secret));
}

void CoreStreams::stop() { impl_->stopAll(); }

bool CoreStreams::logsOpen() const { return impl_->logs.open.load(); }
bool CoreStreams::trafficOpen() const { return impl_->traffic.open.load(); }
bool CoreStreams::connectionsOpen() const { return impl_->connections.open.load(); }

std::vector<LogLine> CoreStreams::drainLogs() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->logLines, {});
}

bool CoreStreams::takeTraffic(TrafficPoint& out) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->trafficDirty) return false;
    out = impl_->latestTraffic;
    impl_->trafficDirty = false;
    return true;
}

bool CoreStreams::takeConnections(std::string& out) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->connectionsDirty) return false;
    out = std::move(impl_->latestConnections);
    impl_->connectionsDirty = false;
    return true;
}

} // namespace stream
