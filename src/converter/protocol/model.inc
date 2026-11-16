// Private protocol values; no YAML/JSON nodes or application state are retained.
namespace protocol {

struct NodeOrigin {
    std::string sourceId;
    std::string name;
};

struct NodeDiagnostic {
    std::string field;
    FidelityNote note;
};

struct Diagnostics {
    NodeOrigin origin;
    std::vector<NodeDiagnostic> entries;

    void Note(Fidelity level, std::string field, std::string detail, std::string action = {}) {
        entries.push_back({std::move(field), {FidelityScope::Node, level, origin.name,
            std::move(detail), std::move(action), origin.sourceId}});
    }

    bool ValueError(std::string_view field, std::string_view reason) {
        Note(Fidelity::Unsupported, std::string(field),
            std::format("节点「{}」的 {} {}，已跳过", origin.name, field, reason),
            "修正该字段后重新导入；不会删掉异常成员后保留部分线路配置");
        return false;
    }

    bool DialError(std::string_view field, std::string_view reason) {
        Note(Fidelity::Unsupported, std::string(field),
            std::format("节点「{}」的 {} {}，已跳过", origin.name, field, reason));
        return false;
    }
};

struct PasswordAuth {
    std::optional<std::string> username;
    std::optional<std::string> password;
};

struct HttpOptions { PasswordAuth auth; };
struct Socks5Options { PasswordAuth auth; };
struct VmessOptions {
    std::string uuid;
    std::string security = "auto";
    std::optional<int> alterId;
    std::optional<std::string> packetEncoding;
    std::optional<bool> globalPadding;
    std::optional<bool> authenticatedLength;
    bool legacyShareTlsAutoCipher = false;
};
struct VlessOptions {
    std::string uuid;
    std::optional<std::string> flow;
    std::optional<std::string> packetEncoding;
};
struct TrojanOptions { std::optional<std::string> password; };

struct NoShadowsocksPlugin {};
enum class SimpleObfsMode { Http, Tls };
struct SimpleObfsPlugin {
    SimpleObfsMode mode = SimpleObfsMode::Http;
    std::optional<std::string> host;
};
struct V2RayWebSocketPlugin {
    std::optional<std::string> host;
    std::optional<std::string> path;
    bool tls = false;
    bool multiplex = false;
};
struct UdpOverTcpOptions {
    bool enabled = false;
    int version = 0; // The adapter must resolve the source's v1/v2 default.
};
struct ShadowsocksOptions {
    std::string method;
    std::optional<std::string> password;
    std::variant<NoShadowsocksPlugin, SimpleObfsPlugin, V2RayWebSocketPlugin> plugin;
    std::optional<UdpOverTcpOptions> udpOverTcp;
};

struct BandwidthOptions {
    std::optional<int> upMbps;
    std::optional<int> downMbps;
};
struct PortRange { std::uint16_t first = 0, last = 0; };
struct PortHopping {
    std::vector<PortRange> ports;
    std::uint64_t minimumSeconds = 0, maximumSeconds = 0;
};
struct NoHysteriaAuth {};
struct HysteriaBytesAuth { std::string base64; };
struct HysteriaTextAuth { std::string text; };
struct Hysteria1Options {
    BandwidthOptions bandwidth;
    std::variant<NoHysteriaAuth, HysteriaBytesAuth, HysteriaTextAuth> auth;
    std::optional<std::string> obfs;
    std::uint32_t streamReceiveWindow = 0, connectionReceiveWindow = 0;
    std::optional<bool> disableMtuDiscovery;
    std::optional<PortHopping> hopping;
};
struct NoHysteria2Obfs {};
struct SalamanderObfs { std::string password; };
struct GeckoObfs {
    std::string password;
    std::optional<std::uint16_t> minimumPacketSize, maximumPacketSize;
};
struct Hysteria2Options {
    BandwidthOptions bandwidth;
    std::optional<std::string> password;
    std::variant<NoHysteria2Obfs, SalamanderObfs, GeckoObfs> obfs;
    std::optional<PortHopping> hopping;
};
enum class TuicUdpRelayMode { Native, Quic };
struct TuicOptions {
    std::string uuid;
    std::optional<std::string> password, congestionControl;
    bool zeroRttHandshake = false;
    std::optional<TuicUdpRelayMode> udpRelayMode;
};
struct AnyTlsOptions {
    std::string password;
    std::optional<int> idleSessionCheckSeconds, idleSessionTimeoutSeconds, minimumIdleSessions;
    std::optional<std::string> clientMetadata;
};
struct SnellObfsOptions {
    enum class Mode { Http, Tls };
    Mode mode = Mode::Http;
    std::string host;
};
struct Snell4Options {
    std::string psk;
    bool reuse = false;
    std::optional<SnellObfsOptions> obfs;
};
struct SshPrivateKey {
    std::string pem;
    std::optional<std::string> passphrase;
};
struct SshOptions {
    std::string user;
    std::optional<std::string> password;
    std::optional<SshPrivateKey> privateKey;
    std::optional<std::vector<std::string>> hostKeys, hostKeyAlgorithms;
};

struct DirectTransport {};
struct WebSocketTransport {
    std::optional<std::string> path;
    std::optional<std::map<std::string, std::string>> headers;
    std::optional<int> maxEarlyData;
    std::optional<std::string> earlyDataHeader;
};
struct GrpcTransport { std::optional<std::string> serviceName; };
struct HttpTransport {
    std::optional<std::string> path;
    std::optional<std::vector<std::string>> hosts;
};
struct HttpUpgradeTransport {
    std::optional<std::string> path;
    std::optional<std::string> host;
};
using TransportOptions = std::variant<DirectTransport, WebSocketTransport, GrpcTransport, HttpTransport, HttpUpgradeTransport>;

struct RealityOptions {
    std::string publicKey;
    std::optional<std::string> shortId;
};

struct TlsOptions {
    std::optional<bool> declaredEnabled;
    bool enabled = false;
    std::optional<bool> verifyPeer;
    std::optional<std::string> serverName;
    std::optional<std::vector<std::string>> alpn;
    std::string fingerprint;
    bool inheritFingerprint = false;
    bool disableServerName = false;
    std::optional<RealityOptions> reality;
};

struct DialOptions {
    std::optional<std::string> detour;
    std::optional<std::string> interfaceName;
    std::optional<std::uint32_t> routingMark;
    std::optional<bool> tcpFastOpen;
    std::optional<bool> multiPathTcp;
};

struct SourceDefaults { std::string fingerprint; };

struct TargetCapabilities {
    bool bindInterface = false;
    bool routingMark = false;
};

struct NodeSpec {
    NodeOrigin origin;
    std::string server;
    std::optional<std::uint16_t> port;
    std::variant<HttpOptions, Socks5Options, VmessOptions, VlessOptions, TrojanOptions, ShadowsocksOptions,
        Hysteria1Options, Hysteria2Options, TuicOptions, AnyTlsOptions, Snell4Options, SshOptions> options;
    TransportOptions transport;
    TlsOptions tls;
    DialOptions dial;
    std::optional<bool> udp;
};

class ValidatedNode {
public:
    inline const NodeSpec& Spec() const noexcept { return node_; }
    inline bool TcpOnly() const noexcept { return tcpOnly_; }

private:
    NodeSpec node_;
    bool tcpOnly_ = false;

    explicit ValidatedNode(NodeSpec node, bool tcpOnly) : node_(std::move(node)), tcpOnly_(tcpOnly) {}
    friend std::optional<ValidatedNode> ValidateNode(NodeSpec, const TargetCapabilities&, Diagnostics&, const SourceDefaults&);
};

} // namespace protocol
