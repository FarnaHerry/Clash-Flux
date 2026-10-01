import Foundation
import Libbox
import Network
import NetworkExtension

final class ClashFluxPlatformInterface: NSObject, LibboxPlatformInterfaceProtocol {
    private weak var provider: NEPacketTunnelProvider?
    private var networkSettings: NEPacketTunnelNetworkSettings?
    private var pathMonitor: NWPathMonitor?

    func setProvider(_ provider: NEPacketTunnelProvider) {
        self.provider = provider
    }

    func openTun(
        _ options: LibboxTunOptionsProtocol?,
        ret0_: UnsafeMutablePointer<Int32>?
    ) throws {
        guard let provider, let options, let ret0_ else {
            throw ClashFluxTunnelError("打开 TUN 时缺少 Packet Tunnel 参数")
        }

        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: "127.0.0.1")
        if options.getAutoRoute() {
            settings.mtu = NSNumber(value: options.getMTU())

            let dnsMode = options.getDNSMode()?.value ?? LibboxDNSModeDisabled
            if dnsMode != LibboxDNSModeDisabled {
                let iterator = try options.getDNSServerAddress()
                var servers: [String] = []
                while iterator.hasNext() {
                    servers.append(iterator.next())
                }
                if !servers.isEmpty {
                    let dnsSettings = NEDNSSettings(servers: servers)
                    dnsSettings.matchDomains = [""]
                    dnsSettings.matchDomainsNoSearch = true
                    settings.dnsSettings = dnsSettings
                }
            }

            let ipv4Addresses = collect(options.getInet4Address())
            if !ipv4Addresses.isEmpty {
                let ipv4 = NEIPv4Settings(
                    addresses: ipv4Addresses.map { $0.address() },
                    subnetMasks: ipv4Addresses.map { $0.mask() }
                )
                let included = collect(options.getInet4RouteAddress())
                ipv4.includedRoutes = included.isEmpty
                    ? [NEIPv4Route.default()]
                    : included.map {
                        NEIPv4Route(
                            destinationAddress: $0.address(),
                            subnetMask: $0.mask()
                        )
                    }
                ipv4.excludedRoutes = collect(options.getInet4RouteExcludeAddress()).map {
                    NEIPv4Route(
                        destinationAddress: $0.address(),
                        subnetMask: $0.mask()
                    )
                }
                settings.ipv4Settings = ipv4
            }

            let ipv6Addresses = collect(options.getInet6Address())
            if !ipv6Addresses.isEmpty {
                let ipv6 = NEIPv6Settings(
                    addresses: ipv6Addresses.map { $0.address() },
                    networkPrefixLengths: ipv6Addresses.map {
                        NSNumber(value: $0.prefix())
                    }
                )
                let included = collect(options.getInet6RouteAddress())
                ipv6.includedRoutes = included.isEmpty
                    ? [NEIPv6Route.default()]
                    : included.map {
                        NEIPv6Route(
                            destinationAddress: $0.address(),
                            networkPrefixLength: NSNumber(value: $0.prefix())
                        )
                    }
                ipv6.excludedRoutes = collect(options.getInet6RouteExcludeAddress()).map {
                    NEIPv6Route(
                        destinationAddress: $0.address(),
                        networkPrefixLength: NSNumber(value: $0.prefix())
                    )
                }
                settings.ipv6Settings = ipv6
            }
        }

        if options.isHTTPProxyEnabled() {
            let proxy = NEProxySettings()
            let server = NEProxyServer(
                address: options.getHTTPProxyServer(),
                port: Int(options.getHTTPProxyServerPort())
            )
            proxy.httpServer = server
            proxy.httpsServer = server
            proxy.httpEnabled = true
            proxy.httpsEnabled = true
            proxy.exceptionList = collect(options.getHTTPProxyBypassDomain())
            proxy.matchDomains = collect(options.getHTTPProxyMatchDomain())
            settings.proxySettings = proxy
        }

        try setNetworkSettings(settings, provider: provider)
        networkSettings = settings
        let descriptor = LibboxGetTunnelFileDescriptor()
        guard descriptor >= 0 else {
            throw ClashFluxTunnelError("Network Extension 未提供 TUN 文件描述符")
        }
        ret0_.pointee = descriptor
    }

    private func setNetworkSettings(
        _ settings: NEPacketTunnelNetworkSettings?,
        provider: NEPacketTunnelProvider
    ) throws {
        let semaphore = DispatchSemaphore(value: 0)
        var resultError: Error?
        provider.setTunnelNetworkSettings(settings) { error in
            resultError = error
            semaphore.signal()
        }
        guard semaphore.wait(timeout: .now() + 20) == .success else {
            throw ClashFluxTunnelError("等待 iOS 網絡設置超时")
        }
        if let resultError { throw resultError }
    }

    private func collect(
        _ iterator: LibboxRoutePrefixIteratorProtocol?
    ) -> [LibboxRoutePrefix] {
        guard let iterator else { return [] }
        var values: [LibboxRoutePrefix] = []
        while iterator.hasNext() {
            if let value = iterator.next() { values.append(value) }
        }
        return values
    }

    private func collect(_ iterator: LibboxStringIteratorProtocol?) -> [String] {
        guard let iterator else { return [] }
        var values: [String] = []
        while iterator.hasNext() { values.append(iterator.next()) }
        return values
    }

    func usePlatformAutoDetectInterfaceControl() -> Bool { false }
    func autoDetectControl(_ fd: Int32) throws {}
    func useProcFS() -> Bool { false }

    func findConnectionOwner(
        _ ipProtocol: Int32,
        sourceAddress: String?,
        sourcePort: Int32,
        destinationAddress: String?,
        destinationPort: Int32
    ) throws -> LibboxConnectionOwner {
        throw ClashFluxTunnelError("iOS 不提供连接进程归属查询")
    }

    func startDefaultInterfaceMonitor(
        _ listener: LibboxInterfaceUpdateListenerProtocol?
    ) throws {
        guard let listener else { return }
        let monitor = NWPathMonitor()
        pathMonitor = monitor
        monitor.pathUpdateHandler = { path in
            guard let interface = path.availableInterfaces.first else {
                listener.updateDefaultInterface(
                    "",
                    interfaceIndex: -1,
                    isExpensive: false,
                    isConstrained: false
                )
                return
            }
            listener.updateDefaultInterface(
                interface.name,
                interfaceIndex: Int32(interface.index),
                isExpensive: path.isExpensive,
                isConstrained: path.isConstrained
            )
        }
        monitor.start(queue: DispatchQueue.global(qos: .utility))
    }

    func closeDefaultInterfaceMonitor(
        _ listener: LibboxInterfaceUpdateListenerProtocol?
    ) throws {
        pathMonitor?.cancel()
        pathMonitor = nil
    }

    func getInterfaces() throws -> LibboxNetworkInterfaceIteratorProtocol {
        let interfaces = pathMonitor?.currentPath.availableInterfaces.map { item in
            let value = LibboxNetworkInterface()
            value.name = item.name
            value.index = Int32(item.index)
            switch item.type {
            case .wifi: value.type = LibboxInterfaceTypeWIFI
            case .cellular: value.type = LibboxInterfaceTypeCellular
            case .wiredEthernet: value.type = LibboxInterfaceTypeEthernet
            default: value.type = LibboxInterfaceTypeOther
            }
            return value
        } ?? []
        return ClashFluxNetworkInterfaceIterator(interfaces)
    }

    func underNetworkExtension() -> Bool { true }
    func includeAllNetworks() -> Bool { false }
    func readWIFIState() -> LibboxWIFIState? { nil }

    func clearDNSCache() {
        guard let provider, let networkSettings else { return }
        DispatchQueue.global(qos: .utility).async {
            try? self.setNetworkSettings(nil, provider: provider)
            try? self.setNetworkSettings(networkSettings, provider: provider)
        }
    }

    func send(_ notification: LibboxNotification?) throws {}
    func cancelNotification(_ identifier: String?, typeID: Int32) throws {}
    func startNeighborMonitor(_ listener: LibboxNeighborUpdateListenerProtocol?) throws {}
    func closeNeighborMonitor(_ listener: LibboxNeighborUpdateListenerProtocol?) throws {}
    func registerMyInterface(_ name: String?) {}
    func localDNSTransport() -> LibboxLocalDNSTransportProtocol? { nil }
    func usePlatformShell() -> Bool { false }

    func checkPlatformShell() throws {
        throw ClashFluxTunnelError("iOS Packet Tunnel 不提供平台 Shell")
    }

    func openShellSession(
        _ user: LibboxPlatformUser?,
        command: String?,
        environ: LibboxStringIteratorProtocol?,
        term: String?,
        rows: Int32,
        cols: Int32
    ) throws -> any LibboxShellSessionProtocol {
        throw ClashFluxTunnelError("iOS Packet Tunnel 不提供平台 Shell")
    }

    func lookupUser(_ username: String?) throws -> LibboxPlatformUser {
        throw ClashFluxTunnelError("iOS Packet Tunnel 不提供系统用户查询")
    }

    func lookupSFTPServer(_ error: NSErrorPointer) -> String { "" }
    func readSystemSSHHostKey(_ error: NSErrorPointer) -> String { "" }
    func tailscaleHostname() -> String { "" }
    func usePlatformBridge() -> Bool { false }

    func createBridge(
        _ options: LibboxBridgeOptions?
    ) throws -> any LibboxBridgeSessionProtocol {
        throw ClashFluxTunnelError("iOS Packet Tunnel 不支持平台 Bridge")
    }
}

private final class ClashFluxNetworkInterfaceIterator: NSObject,
    LibboxNetworkInterfaceIteratorProtocol
{
    private let values: [LibboxNetworkInterface]
    private var index = 0

    init(_ values: [LibboxNetworkInterface]) {
        self.values = values
    }

    func hasNext() -> Bool { index < values.count }

    func next() -> LibboxNetworkInterface? {
        guard index < values.count else { return nil }
        defer { index += 1 }
        return values[index]
    }
}
