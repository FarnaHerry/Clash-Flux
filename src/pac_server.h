#pragma once
#include "network_settings.h"
#include <ixwebsocket/IXSocketServer.h>
#include <ixwebsocket/IXSocket.h>
#include <atomic>
#include <chrono>
#include <ixwebsocket/IXNetSystem.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace clashflux::network {

// Splits the request on '\n' but keeps the trailing '\r' of the wire format,
// matching std::getline on a '\n' delimiter. Deliberately avoids <sstream>:
// this header lives in a module global fragment, and MSVC instantiates
// std::getline in importer TUs where <istream> may be incomplete (C2079).
inline bool NextLine(std::string_view& rest, std::string_view& line) {
    if (rest.empty()) return false;
    const auto end = rest.find('\n');
    line = rest.substr(0, end);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
    return true;
}

// All JS strings come from validated IPs, port numbers and a domain alphabet.
// No subscription rules or user-supplied script are evaluated by the app.
inline std::string PacScript(const std::string& host, int port, const std::string& directDomains) {
    Advanced validation;
    validation.pac_direct_domains = directDomains;
    if (!IP(host) || port < 1 || port > 65535 || !ValidateAdvanced(validation).empty()) return {};
    std::string script = "function FindProxyForURL(url, host) {\n  host = host.toLowerCase();\n"
        "  if (host === 'localhost' || host === '127.0.0.1' || host === '::1' || host === '[::1]') return 'DIRECT';\n";
    for (auto domain : Lines(directDomains)) {
        std::transform(domain.begin(), domain.end(), domain.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
        });
        if (domain.starts_with('.')) {
            script += "  if (host === '" + domain.substr(1) + "' || dnsDomainIs(host, '" + domain + "')) return 'DIRECT';\n";
        } else script += "  if (shExpMatch(host, '" + domain + "')) return 'DIRECT';\n";
    }
    const auto authority = host.find(':') == std::string::npos ? host : "[" + host + "]";
    return script + "  return 'PROXY " + authority + ":" + std::to_string(port) + "';\n}\n";
}

// Application/session owned, loopback only. Destruction joins IX's workers;
// callers create/release on the lifecycle task thread, never during composition.
class PacServer {
    class Listener final : public ix::SocketServer {
        std::string path_, script_, host_;
        std::atomic<std::size_t> active_{0};
        std::size_t getConnectedClientsCount() override { return active_.load(); }
        void handleConnection(std::unique_ptr<ix::Socket> socket,
                              std::shared_ptr<ix::ConnectionState> state) override {
            const auto previous = active_.fetch_add(1);
            struct Completion {
                std::atomic<std::size_t>& active;
                std::shared_ptr<ix::ConnectionState> state;
                ~Completion() { --active; state->setTerminated(); }
            } completion{active_, std::move(state)};
            if (previous >= 8) return;
            try {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                const auto cancelled = [deadline] { return std::chrono::steady_clock::now() >= deadline; };
                std::string request;
                char byte = 0;
                while (request.size() < 8192 && !request.ends_with("\r\n\r\n")) {
                    if (!socket->readByte(&byte, cancelled)) return;
                    request += byte;
                }
                bool ok = request.ends_with("\r\n\r\n");
                const std::string http11 = "GET " + path_ + " HTTP/1.1\r";
                const std::string http10 = "GET " + path_ + " HTTP/1.0\r";
                std::string_view rest(request), line;
                ok = ok && NextLine(rest, line) && (line == http11 || line == http10);
                bool host = false;
                while (NextLine(rest, line) && line != "\r") {
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) { ok = false; continue; }
                    std::string key(line.substr(0, colon));
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
                    });
                    if (key == "host") {
                        const auto values = Lines(line.substr(colon + 1));
                        ok = ok && !host && values.size() == 1 && values[0] == host_;
                        host = true;
                    }
                    // GET bodies/upgrades are not part of the PAC endpoint.
                    if (key == "content-length" || key == "transfer-encoding" || key == "upgrade") ok = false;
                }
                ok = ok && host;
                const auto body = ok ? script_ : std::string{};
                socket->writeBytes(std::string("HTTP/1.1 ") + (ok ? "200 OK" : "404 Not Found") +
                    "\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nCache-Control: no-store\r\n"
                    "X-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body, cancelled);
            } catch (...) { /* A malformed local request must not terminate the application. */ }
        }
    public:
        Listener(int port, std::string path, std::string script)
            : SocketServer(port, "127.0.0.1", 8, 8), path_(std::move(path)), script_(std::move(script)),
              host_("127.0.0.1:" + std::to_string(port)) {}
        ~Listener() override { stop(); }
    };
    std::unique_ptr<Listener> server_;
    std::string url_;
public:
    ~PacServer() { if (server_) server_->stop(); }
    PacServer() = default;
    PacServer(const PacServer&) = delete;
    PacServer& operator=(const PacServer&) = delete;
    const std::string& url() const { return url_; }
    bool Start(int port, std::string script, std::string token, std::string& error) {
        static std::once_flag init;
        std::call_once(init, [] { ix::initNetSystem(); });
        if (script.empty() || token.empty() || token.size() > 128 ||
            token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-=") != std::string::npos) {
            error = "PAC 脚本或路径无效"; return false;
        }
        if (server_) { error = "PAC 服务已启动"; return false; }
        const std::string path = "/" + token + "/proxy.pac";
        auto next = std::make_unique<Listener>(port, path, std::move(script));
        const auto [ok, detail] = next->listen();
        if (!ok) { error = "PAC 回环端口监听失败：" + detail; return false; }
        next->start();
        url_ = "http://127.0.0.1:" + std::to_string(port) + path;
        server_ = std::move(next);
        return true;
    }
};
} // namespace clashflux::network
