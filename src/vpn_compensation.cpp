// Temporary Linux policy routes. The main table is never changed.
module;

#if defined(__linux__) && !defined(__ANDROID__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
extern char** environ;
#endif

module clashflux.vpn_compensation;

import std;
import nlohmann.json;

namespace vpn::compensation {
namespace {

constexpr std::string_view RouteProtocol = "186";
constexpr std::string_view ReservationMetric = "4278190000";

std::optional<std::uint32_t> parseIpv4(std::string_view value) {
    std::uint32_t result = 0;
    std::size_t begin = 0;
    for (int part = 0; part != 4; ++part) {
        const auto dot = value.find('.', begin);
        const auto end = dot == std::string_view::npos ? value.size() : dot;
        if (end == begin || (end - begin > 1 && value[begin] == '0')) return {};
        unsigned int octet = 0;
        const auto [ptr, ec] = std::from_chars(value.data() + begin,
                                              value.data() + end, octet);
        if (ec != std::errc{} || ptr != value.data() + end || octet > 255 ||
            (part < 3 && dot == std::string_view::npos) ||
            (part == 3 && dot != std::string_view::npos)) return {};
        result = (result << 8) | octet;
        begin = end + 1;
    }
    return result;
}

std::string formatIpv4(std::uint32_t value) {
    return std::format("{}.{}.{}.{}", value >> 24, (value >> 16) & 255,
                       (value >> 8) & 255, value & 255);
}

bool usableTransport(std::string_view value) {
    const auto address = parseIpv4(value);
    return address && (*address >> 24) != 0 && (*address >> 24) != 127 &&
           (*address >> 28) < 14 && !IsSyntheticIpv4(value);
}

bool validInterface(std::string_view value) {
    return !value.empty() && value.size() < 16 && value != "." && value != ".." &&
           value.front() != '-' && std::ranges::all_of(value, [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
           });
}

bool validHost(std::string_view value) {
    if (value.empty() || value.size() > 253) return false;
    if (value.back() == '.') value.remove_suffix(1);
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('.', begin);
        const auto label = value.substr(begin, end == std::string_view::npos
                                                   ? value.size() - begin : end - begin);
        if (label.empty() || label.size() > 63 || label.front() == '-' ||
            label.back() == '-' || !std::ranges::all_of(label, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-';
            })) return false;
        if (end == std::string_view::npos) return true;
        begin = end + 1;
    }
    return false;
}

std::string targetKey(const RouteTarget& target) {
    return std::format("{}|{}|{}|{}|{}", static_cast<int>(target.purpose),
                       target.destination, target.interfaceName, target.gateway,
                       target.source);
}

bool normalizeTarget(RouteTarget& target, std::string& error) {
    if (!validInterface(target.interfaceName)) {
        error = "补偿路由接口名无效（Linux 接口名最长 15 字节）";
        return false;
    }
    if ((!target.gateway.empty() && !usableTransport(target.gateway)) ||
        (!target.source.empty() && !usableTransport(target.source))) {
        error = "补偿路由网关或源地址必须是有效 IPv4 地址";
        return false;
    }
    if (target.purpose == RoutePurpose::BoundInterface) {
        if (!target.destination.empty() && target.destination != "0.0.0.0/0") {
            error = "绑定接口补偿规则必须使用默认路由";
            return false;
        }
        target.destination = "0.0.0.0/0";
    } else if (target.purpose == RoutePurpose::Transport ||
               target.purpose == RoutePurpose::NativeDestination) {
        const auto normalized = NormalizeIpv4Cidr(target.destination);
        if (!normalized) {
            error = "原生 VPN 补偿仅支持非默认 IPv4/CIDR 路由";
            return false;
        }
        target.destination = *normalized;
        if (target.purpose == RoutePurpose::Transport &&
            (!target.destination.ends_with("/32") ||
             !usableTransport(target.destination.substr(0, target.destination.size() - 3)))) {
            error = "VPN 服务器绕行必须使用真实 IPv4 主机地址，不能使用 fake-IP";
            return false;
        }
    } else {
        error = "未知补偿路由用途";
        return false;
    }
    return true;
}

int tableNumber(const nlohmann::json& value) {
    if (value.is_number_integer()) return value.get<int>();
    if (!value.is_string()) return -1;
    const auto name = value.get<std::string>();
    int number = -1;
    const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), number);
    return ec == std::errc{} && ptr == name.data() + name.size() ? number : -1;
}

bool parseOccupiedTables(std::string_view output, std::set<int>& tables,
                         std::string& error) {
    const auto entries = nlohmann::json::parse(output, nullptr, false);
    if (!entries.is_array()) {
        error = "读取系统路由失败：ip 未返回 JSON 数组";
        return false;
    }
    for (const auto& entry : entries) {
        if (!entry.is_object()) {
            error = "读取系统路由失败：ip 返回无效路由条目";
            return false;
        }
        if (entry.contains("table")) tables.insert(tableNumber(entry["table"]));
    }
    return true;
}

std::vector<std::string> reservationCommand(int table, std::string_view action) {
    return {"ip", "-4", "route", std::string(action), "unreachable", "default",
            "table", std::to_string(table), "metric", std::string(ReservationMetric),
            "proto", std::string(RouteProtocol)};
}

std::vector<std::string> routeCommand(const RouteTarget& target, int table,
                                       std::string_view action) {
    std::vector<std::string> args{"ip", "-4", "route", std::string(action),
                                   target.destination, "table", std::to_string(table)};
    if (!target.gateway.empty()) args.insert(args.end(), {"via", target.gateway});
    args.insert(args.end(), {"dev", target.interfaceName});
    // A fresh private table has no connected gateway route; the gateway was
    // already validated against a route on a physical interface in main.
    if (!target.gateway.empty()) args.push_back("onlink");
    if (!target.source.empty()) args.insert(args.end(), {"src", target.source});
    args.insert(args.end(), {"proto", std::string(RouteProtocol)});
    return args;
}

std::vector<std::string> ruleCommand(const RouteTarget& target, int table,
                                      std::string_view action) {
    const int priority = target.purpose == RoutePurpose::Transport ? TransportPriority :
                         target.purpose == RoutePurpose::BoundInterface ? BoundInterfacePriority :
                                                                         NativeFallbackPriority;
    std::vector<std::string> args{"ip", "-4", "rule", std::string(action),
                                   "pref", std::to_string(priority)};
    if (target.purpose == RoutePurpose::BoundInterface)
        args.insert(args.end(), {"oif", target.interfaceName});
    else args.insert(args.end(), {"to", target.destination});
    args.insert(args.end(), {"lookup", std::to_string(table),
                            "protocol", std::string(RouteProtocol)});
    return args;
}

} // namespace

std::optional<std::string> NormalizeIpv4Cidr(std::string_view value, bool allowDefault) {
    const auto slash = value.find('/');
    const auto address = parseIpv4(value.substr(0, slash));
    if (!address) return {};
    unsigned int prefix = 32;
    if (slash != std::string_view::npos) {
        const auto [ptr, ec] = std::from_chars(value.data() + slash + 1,
                                              value.data() + value.size(), prefix);
        if (ec != std::errc{} || ptr != value.data() + value.size() || prefix > 32) return {};
    }
    if (prefix == 0 && !allowDefault) return {};
    const std::uint32_t mask = prefix == 0 ? 0 : (~std::uint32_t{0} << (32 - prefix));
    return std::format("{}/{}", formatIpv4(*address & mask), prefix);
}

bool IsSyntheticIpv4(std::string_view address) {
    const auto value = parseIpv4(address);
    return value && (*value & 0xfffe0000U) == 0xc6120000U; // sing-box 198.18.0.0/15
}

std::optional<PhysicalRoute> SelectPhysicalRoute(std::string_view address,
                                                 std::span<const PhysicalRoute> routes) {
    const auto target = parseIpv4(address);
    if (!target || !usableTransport(address)) return {};
    std::optional<PhysicalRoute> selected;
    unsigned int selectedPrefix = 0;
    for (const auto& route : routes) {
        if (!route.physical || !validInterface(route.interfaceName) ||
            (!route.gateway.empty() && !usableTransport(route.gateway)) ||
            (!route.source.empty() && !usableTransport(route.source))) continue;
        const auto normalized = NormalizeIpv4Cidr(route.destination == "default"
                                                     ? "0.0.0.0/0" : route.destination, true);
        if (!normalized) continue;
        const auto slash = normalized->find('/');
        const auto network = parseIpv4(std::string_view(*normalized).substr(0, slash));
        unsigned int prefix = 0;
        std::from_chars(normalized->data() + slash + 1,
                        normalized->data() + normalized->size(), prefix);
        const std::uint32_t mask = prefix == 0 ? 0 : (~std::uint32_t{0} << (32 - prefix));
        if ((*target & mask) != *network) continue;
        if (!selected || prefix > selectedPrefix ||
            (prefix == selectedPrefix && route.metric < selected->metric)) {
            selected = route;
            selectedPrefix = prefix;
        }
    }
    return selected;
}

struct RouteRegistry::Impl : std::enable_shared_from_this<Impl> {
    struct Entry {
        std::shared_ptr<Impl> owner;
        RouteTarget target;
        std::string key;
        int table = 0;
        bool reserved = false;
        bool routed = false;
        bool ruled = false;
        ~Entry() {
            if (owner && (reserved || routed || ruled)) owner->release(*this);
        }
    };

    explicit Impl(CommandRunner executor) : runner(std::move(executor)) {}
    CommandRunner runner;
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<Entry>> entries;
    std::set<int> ownedTables;

    CommandResult run(const std::vector<std::string>& args) noexcept {
        try {
            if (!runner) return {-1, "未提供路由命令执行器"};
            return runner(args);
        } catch (const std::exception& exception) {
            return {-1, std::string("路由命令异常：") + exception.what()};
        } catch (...) {
            return {-1, "路由命令异常"};
        }
    }

    void removeObjects(Entry& entry) noexcept {
        // No flush or broad deletion: failures never authorize deleting an
        // object whose creation did not complete successfully.
        if (entry.ruled) run(ruleCommand(entry.target, entry.table, "del"));
        if (entry.routed) run(routeCommand(entry.target, entry.table, "del"));
        if (entry.reserved) run(reservationCommand(entry.table, "del"));
        ownedTables.erase(entry.table);
        entry.ruled = entry.routed = entry.reserved = false;
    }

    void release(Entry& entry) noexcept {
        const std::lock_guard lock(mutex);
        removeObjects(entry);
        const auto found = entries.find(entry.key);
        // The previous final owner may be awaiting this mutex while acquire()
        // installs a replacement with the same key in another table.
        if (found != entries.end() && found->second.expired()) entries.erase(found);
    }
};

RouteRegistry::RouteRegistry(CommandRunner runner)
    : impl_(std::make_shared<Impl>(std::move(runner))) {}
RouteRegistry::~RouteRegistry() = default;

RouteLease RouteRegistry::acquire(RouteTarget target, std::string& error) {
    error.clear();
    if (!normalizeTarget(target, error)) return {};
    const auto key = targetKey(target);
    const std::lock_guard lock(impl_->mutex);
    if (const auto found = impl_->entries.find(key); found != impl_->entries.end()) {
        if (auto existing = found->second.lock()) return existing;
    }
    std::set<int> occupied = impl_->ownedTables;
    for (const std::vector<std::string>& command : {
             std::vector<std::string>{"ip", "-j", "-N", "-4", "rule", "show"},
             std::vector<std::string>{"ip", "-j", "-N", "-4", "route", "show", "table", "all"}}) {
        const auto result = impl_->run(command);
        if (result.exitCode != 0) {
            error = "无法检查临时路由表占用：" + result.output;
            return {};
        }
        if (!parseOccupiedTables(result.output, occupied, error)) return {};
    }
    auto entry = std::make_shared<Impl::Entry>();
    entry->owner = impl_;
    entry->target = std::move(target);
    entry->key = key;
    for (int table = FirstTable; table <= LastTable; ++table) {
        if (occupied.contains(table)) continue;
        const auto result = impl_->run(reservationCommand(table, "add"));
        if (result.exitCode == 0) {
            entry->table = table;
            entry->reserved = true;
            impl_->ownedTables.insert(table);
            break;
        }
        // route add uses NLM_F_EXCL, making this sentinel an atomic reservation
        // against another process using this same managed table range.
        if (result.output.find("File exists") != std::string::npos) continue;
        error = "创建临时路由表失败（需要 root/CAP_NET_ADMIN）：" + result.output;
        return {};
    }
    if (!entry->reserved) {
        error = "临时路由表 52000–52999 已全部占用";
        return {};
    }
    auto result = impl_->run(routeCommand(entry->target, entry->table, "add"));
    if (result.exitCode == 0) {
        entry->routed = true;
        result = impl_->run(ruleCommand(entry->target, entry->table, "add"));
        entry->ruled = result.exitCode == 0;
    }
    if (result.exitCode != 0) {
        error = "安装临时补偿路由失败：" + result.output;
        impl_->removeObjects(*entry);
        return {};
    }
    impl_->entries[key] = entry;
    return entry;
}

bool ReplaceNativeRoutes(RouteRegistry& registry, std::string_view interfaceName,
                           std::span<const std::string> routes,
                           std::vector<RouteLease>& leases, std::string& error) {
    error.clear();
    if (!validInterface(interfaceName)) {
        error = "原生 VPN 补偿路由接口名无效";
        return false;
    }
    std::vector<RouteLease> replacement;
    std::set<std::string> unique;
    for (const auto& route : routes) {
        const auto normalized = NormalizeIpv4Cidr(route);
        if (!normalized) {
            error = "原生 VPN 内网路由必须使用非默认 IPv4/CIDR";
            return false;
        }
        unique.insert(*normalized);
    }
    for (const auto& route : unique) {
        auto lease = registry.acquire({RoutePurpose::NativeDestination, route,
                                        std::string(interfaceName), {}, {}}, error);
        if (!lease) return false;
        replacement.push_back(std::move(lease));
    }
    leases.swap(replacement);
    return true;
}

namespace {

#if defined(__linux__) && !defined(__ANDROID__)

CommandResult runIp(const std::vector<std::string>& args) {
    if (args.empty() || args.front() != "ip") return {-1, "无效路由命令"};
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0) return {-1, std::strerror(errno)};
    posix_spawn_file_actions_t actions;
    int status = ::posix_spawn_file_actions_init(&actions);
    if (status != 0) {
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        return {-1, std::strerror(status)};
    }
    ::posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    ::posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    // Keep EEXIST recognizable regardless of the daemon's language settings.
    std::vector<std::string> environment;
    for (char** item = environ; item && *item; ++item) {
        if (!std::string_view(*item).starts_with("LC_ALL=")) environment.emplace_back(*item);
    }
    environment.emplace_back("LC_ALL=C");
    std::vector<char*> envp;
    for (auto& item : environment) envp.push_back(item.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = ::posix_spawnp(&pid, "ip", &actions, nullptr, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(descriptors[1]);
    if (spawned != 0) {
        ::close(descriptors[0]);
        return {-1, std::string("启动 ip 失败：") + std::strerror(spawned)};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string output;
    bool timedOut = false;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { timedOut = true; break; }
        pollfd fd{descriptors[0], POLLIN, 0};
        const int polled = ::poll(&fd, 1, static_cast<int>(remaining));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) { timedOut = true; break; }
        char buffer[4096];
        const auto count = ::read(descriptors[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (output.size() + static_cast<std::size_t>(count) > 4 * 1024 * 1024) {
            timedOut = true;
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptors[0]);
    if (timedOut) ::kill(pid, SIGKILL);
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (timedOut) return {-1, "ip 命令超时或输出过大"};
    return {waited > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(output)};
}

RouteRegistry& systemRegistry() {
    static RouteRegistry registry(runIp);
    return registry;
}

bool physicalInterface(std::string_view name, int depth = 0) {
    if (!validInterface(name) || name == "lo" || depth > 4) return false;
    const auto directory = std::filesystem::path("/sys/class/net") / name;
    std::error_code ec;
    if (std::filesystem::exists(directory / "device", ec)) return true;
    // VLANs, bonds and bridges backed by a physical port are valid uplinks.
    // Pure virtual interfaces (TUN, PPP, WireGuard, veth) have no such lower.
    for (std::filesystem::directory_iterator it(directory, ec); !ec && it != std::default_sentinel; it.increment(ec)) {
        const auto file = it->path().filename().string();
        if (file.starts_with("lower_") && physicalInterface(file.substr(6), depth + 1)) return true;
    }
    ec.clear();
    for (std::filesystem::directory_iterator it(directory / "brif", ec); !ec && it != std::default_sentinel; it.increment(ec)) {
        if (physicalInterface(it->path().filename().string(), depth + 1)) return true;
    }
    return false;
}

std::optional<std::vector<PhysicalRoute>> physicalRoutes(std::string& error) {
    // Never use `route get`: after TUN starts it selects the TUN's policy table.
    const auto result = runIp({"ip", "-j", "-N", "-4", "route", "show", "table", "main"});
    if (result.exitCode != 0) {
        error = "读取物理网卡路由失败：" + result.output;
        return {};
    }
    const auto entries = nlohmann::json::parse(result.output, nullptr, false);
    if (!entries.is_array()) { error = "ip 未返回有效的物理网卡路由"; return {}; }
    std::vector<PhysicalRoute> routes;
    try {
        for (const auto& entry : entries) {
            if (!entry.is_object() || entry.value("type", "unicast") != "unicast") continue;
            const auto name = entry.value("dev", "");
            if (!physicalInterface(name)) continue;
            if (entry.contains("flags") && entry["flags"].is_array() &&
                std::ranges::any_of(entry["flags"], [](const auto& flag) { return flag == "linkdown"; })) continue;
            routes.push_back({entry.value("dst", "default"), name, entry.value("gateway", ""),
                              entry.value("prefsrc", ""), entry.value("metric", 0U), true});
        }
    } catch (const std::exception&) { error = "ip 物理网卡路由字段无效"; return {}; }
    return routes;
}

RouteLease acquirePhysical(std::string_view address, std::span<const PhysicalRoute> routes,
                            std::string& error) {
    const auto route = SelectPhysicalRoute(address, routes);
    if (!route) {
        error = "找不到 VPN 服务器/DNS 的物理网卡 IPv4 路由；未使用 TUN/PPP 默认路由";
        return {};
    }
    return systemRegistry().acquire({RoutePurpose::Transport, std::string(address),
                                     route->interfaceName, route->gateway, route->source}, error);
}

std::vector<std::string> configuredDns() {
    std::vector<std::string> servers;
    for (const auto* path : {"/etc/resolv.conf", "/run/systemd/resolve/resolv.conf",
                             "/run/NetworkManager/no-stub-resolv.conf"}) {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            std::string kind, address;
            fields >> kind >> address;
            if (kind == "nameserver" && usableTransport(address) &&
                std::ranges::find(servers, address) == servers.end()) servers.push_back(address);
        }
        // Prefer the actual configured server list; other files are only a
        // way to discover the upstream hidden behind a loopback DNS stub.
        if (!servers.empty()) break;
    }
    return servers;
}

std::string lowerHost(std::string_view host) {
    std::string value(host);
    if (!value.empty() && value.back() == '.') value.pop_back();
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

std::vector<std::string> hostsFileAddresses(std::string_view host) {
    const auto desired = lowerHost(host);
    std::ifstream input("/etc/hosts");
    std::string line;
    std::vector<std::string> addresses;
    while (std::getline(input, line)) {
        line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        std::istringstream fields(line);
        std::string address, name;
        fields >> address;
        while (fields >> name) {
            if (lowerHost(name) == desired && usableTransport(address)) {
                addresses.push_back(address);
                break;
            }
        }
    }
    return addresses;
}

std::uint16_t dnsWord(std::span<const unsigned char> bytes, std::size_t position) {
    return static_cast<std::uint16_t>((bytes[position] << 8) | bytes[position + 1]);
}

bool dnsName(std::span<const unsigned char> packet, std::size_t& position,
             std::string& name) {
    std::size_t cursor = position;
    bool jumped = false;
    name.clear();
    for (unsigned int steps = 0; steps < 128 && cursor < packet.size(); ++steps) {
        const unsigned char length = packet[cursor++];
        if ((length & 0xc0) == 0xc0) {
            if (cursor >= packet.size()) return false;
            const auto target = static_cast<std::size_t>(((length & 63) << 8) | packet[cursor++]);
            if (!jumped) position = cursor;
            jumped = true;
            cursor = target;
        } else if (length == 0) {
            if (!jumped) position = cursor;
            return true;
        } else {
            if ((length & 0xc0) != 0 || cursor + length > packet.size()) return false;
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char*>(packet.data() + cursor), length);
            if (name.size() > 253) return false;
            cursor += length;
        }
    }
    return false;
}

std::vector<std::string> queryDns(std::string_view host, std::string_view server,
                                  const PhysicalRoute& route, std::string& error) {
    std::vector<unsigned char> request(12);
    const auto id = static_cast<std::uint16_t>(std::random_device{}());
    request[0] = id >> 8;
    request[1] = id & 255;
    request[2] = 1; // recursion desired
    request[5] = 1;
    const auto name = lowerHost(host);
    std::size_t begin = 0;
    while (begin < name.size()) {
        const auto end = name.find('.', begin);
        const auto label = std::string_view(name).substr(begin, end == std::string::npos
                                                                 ? name.size() - begin : end - begin);
        request.push_back(static_cast<unsigned char>(label.size()));
        request.insert(request.end(), label.begin(), label.end());
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    request.insert(request.end(), {0, 0, 1, 0, 1}); // A / IN
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { error = "创建 VPN DNS 套接字失败"; return {}; }
    struct SocketGuard { int fd; ~SocketGuard() { ::close(fd); } } guard{fd};
    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, route.interfaceName.c_str(),
                     static_cast<socklen_t>(route.interfaceName.size() + 1)) != 0) {
        error = "VPN DNS 绑定物理网卡失败";
        return {};
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(53);
    ::inet_pton(AF_INET, std::string(server).c_str(), &peer.sin_addr);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0 ||
        ::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) {
        error = "通过物理网卡发送 VPN DNS 查询失败";
        return {};
    }
    pollfd pollFd{fd, POLLIN, 0};
    int polled;
    do { polled = ::poll(&pollFd, 1, 2000); } while (polled < 0 && errno == EINTR);
    if (polled <= 0) { error = "通过物理网卡解析 VPN 服务器超时"; return {}; }
    std::array<unsigned char, 4096> buffer;
    const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received < 12) { error = "VPN DNS 返回无效应答"; return {}; }
    const std::span<const unsigned char> packet(buffer.data(), static_cast<std::size_t>(received));
    if (dnsWord(packet, 0) != id || (packet[2] & 0xfe) != 0x80 ||
        (packet[3] & 15) != 0 || dnsWord(packet, 4) != 1) {
        error = "VPN DNS 查询失败或应答被截断";
        return {};
    }
    std::size_t position = 12;
    std::string question;
    if (!dnsName(packet, position, question) || lowerHost(question) != name ||
        position + 4 > packet.size() || dnsWord(packet, position) != 1 ||
        dnsWord(packet, position + 2) != 1) { error = "VPN DNS 问题字段不匹配"; return {}; }
    position += 4;
    struct Answer { std::string owner; std::string value; bool cname; };
    std::vector<Answer> answers;
    const auto count = dnsWord(packet, 6);
    for (unsigned int i = 0; i < count; ++i) {
        std::string owner;
        if (!dnsName(packet, position, owner) || position + 10 > packet.size()) {
            error = "VPN DNS 应答长度无效";
            return {};
        }
        const auto type = dnsWord(packet, position);
        const auto klass = dnsWord(packet, position + 2);
        const auto length = dnsWord(packet, position + 8);
        position += 10;
        if (position + length > packet.size()) { error = "VPN DNS 应答数据不完整"; return {}; }
        if (klass == 1 && type == 1 && length == 4) {
            const auto address = (std::uint32_t{packet[position]} << 24) |
                                 (std::uint32_t{packet[position + 1]} << 16) |
                                 (std::uint32_t{packet[position + 2]} << 8) |
                                  std::uint32_t{packet[position + 3]};
            answers.push_back({lowerHost(owner), formatIpv4(address), false});
        } else if (klass == 1 && type == 5) {
            auto cursor = position;
            std::string alias;
            if (!dnsName(packet, cursor, alias) || cursor != position + length) {
                error = "VPN DNS 别名数据无效";
                return {};
            }
            answers.push_back({lowerHost(owner), lowerHost(alias), true});
        }
        position += length;
    }
    std::set<std::string> names{name};
    for (unsigned int pass = 0; pass < 16; ++pass) {
        bool changed = false;
        for (const auto& answer : answers)
            if (answer.cname && names.contains(answer.owner)) changed |= names.insert(answer.value).second;
        if (!changed) break;
    }
    std::vector<std::string> addresses;
    for (const auto& answer : answers) {
        if (!answer.cname && names.contains(answer.owner) && usableTransport(answer.value) &&
            std::ranges::find(addresses, answer.value) == addresses.end()) addresses.push_back(answer.value);
    }
    if (addresses.empty()) error = "VPN DNS 未返回真实 IPv4 地址（不接受 fake-IP 或仅 IPv6 应答）";
    return addresses;
}

#endif

} // namespace

std::optional<TransportLease> PrepareTransport(std::string_view host, std::string& error) {
    error.clear();
    if (host.find(':') != std::string_view::npos) {
        error = "原生 VPN 服务器补偿目前仅支持 IPv4；暂不支持 IPv6 服务器";
        return {};
    }
    const bool numeric = !host.empty() && std::ranges::all_of(host, [](unsigned char c) {
        return (c >= '0' && c <= '9') || c == '.';
    });
    if ((numeric && !usableTransport(host)) || (!numeric && !validHost(host))) {
        error = "原生 VPN 服务器必须是有效域名或真实 IPv4 地址（不接受 fake-IP）";
        return {};
    }
#if defined(__linux__) && !defined(__ANDROID__)
    const auto routes = physicalRoutes(error);
    if (!routes) return {};
    std::vector<std::string> addresses;
    if (numeric) addresses.emplace_back(host);
    else {
        addresses = hostsFileAddresses(host);
        if (addresses.empty()) {
            const auto servers = configuredDns();
            if (servers.empty()) {
                error = "无法发现物理网卡的 IPv4 DNS 上游；请配置真实 DNS 或使用 VPN 服务器 IPv4 地址";
                return {};
            }
            for (const auto& server : servers) {
                const auto route = SelectPhysicalRoute(server, *routes);
                if (!route) continue;
                auto dnsLease = acquirePhysical(server, *routes, error);
                if (!dnsLease) continue;
                addresses = queryDns(host, server, *route, error);
                if (!addresses.empty()) break;
            }
        }
    }
    for (const auto& address : addresses) {
        if (auto lease = acquirePhysical(address, *routes, error))
            return TransportLease{address, std::move(lease)};
    }
    if (error.empty()) error = "无法通过物理网卡解析或绕行 VPN 服务器 IPv4 地址";
#else
    error = "当前平台尚未实现原生 VPN 临时补偿路由；仅 Linux 支持";
#endif
    return {};
}

RouteLease PrepareBoundInterface(std::string_view interfaceName, std::string& error) {
#if defined(__linux__) && !defined(__ANDROID__)
    return systemRegistry().acquire({RoutePurpose::BoundInterface, {},
                                     std::string(interfaceName), {}, {}}, error);
#else
    error = "当前平台尚未实现原生 VPN 接口补偿路由；仅 Linux 支持";
    return {};
#endif
}

bool ReplaceNativeRoutes(std::string_view interfaceName,
                           std::span<const std::string> routes,
                           std::vector<RouteLease>& leases, std::string& error) {
#if defined(__linux__) && !defined(__ANDROID__)
    return ReplaceNativeRoutes(systemRegistry(), interfaceName, routes, leases, error);
#else
    error = "当前平台尚未实现原生 VPN 内网补偿路由；仅 Linux 支持";
    return false;
#endif
}

} // namespace vpn::compensation
