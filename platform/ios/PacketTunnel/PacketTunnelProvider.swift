import Foundation
import Libbox
import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider,
    LibboxCommandServerHandlerProtocol
{
    private let platformInterface = ClashFluxPlatformInterface()
    private var commandServer: LibboxCommandServer?
    private var configFile: URL?

    override init() {
        super.init()
        platformInterface.setProvider(self)
    }

    override func startTunnel(
        options: [String: NSObject]?,
        completionHandler: @escaping (Error?) -> Void
    ) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else {
                completionHandler(ClashFluxTunnelError("Packet Tunnel provider 已释放"))
                return
            }
            do {
                try self.startLibbox()
                completionHandler(nil)
            } catch {
                self.stopLibbox()
                completionHandler(error)
            }
        }
    }

    override func stopTunnel(
        with reason: NEProviderStopReason,
        completionHandler: @escaping () -> Void
    ) {
        stopLibbox()
        completionHandler()
    }

    private func startLibbox() throws {
        guard let tunnelProtocol = protocolConfiguration as? NETunnelProviderProtocol,
              let providerConfiguration = tunnelProtocol.providerConfiguration,
              let groupIdentifier = providerConfiguration["appGroupIdentifier"] as? String,
              let relativePath = providerConfiguration["configRelativePath"] as? String,
              let groupURL = FileManager.default.containerURL(
                forSecurityApplicationGroupIdentifier: groupIdentifier
              )
        else {
            throw ClashFluxTunnelError("Packet Tunnel 缺少 App Group 配置")
        }

        let baseURL = groupURL.standardizedFileURL
        let configURL = baseURL.appendingPathComponent(relativePath).standardizedFileURL
        guard configURL.path.hasPrefix(baseURL.path + "/"),
              FileManager.default.fileExists(atPath: configURL.path)
        else {
            throw ClashFluxTunnelError("找不到 App Group 中的 sing-box 配置")
        }
        let configContent = try String(contentsOf: configURL, encoding: .utf8)
        configFile = configURL

        let workingURL = baseURL.appendingPathComponent("core", isDirectory: true)
        let temporaryURL = workingURL.appendingPathComponent("temp", isDirectory: true)
        try FileManager.default.createDirectory(
            at: workingURL,
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(
            at: temporaryURL,
            withIntermediateDirectories: true
        )

        let setup = LibboxSetupOptions()
        setup.basePath = baseURL.path
        setup.workingPath = workingURL.path
        setup.tempPath = temporaryURL.path
        setup.logMaxLines = 3000
        setup.crashReportSource = "NetworkExtension"
        setup.appVersion = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
        setup.appMarketingVersion = Bundle.main.infoDictionary?[
            "CFBundleShortVersionString"
        ] as? String ?? "0"
        setup.oomKillerEnabled = true

        var setupError: NSError?
        LibboxSetup(setup, &setupError)
        if let setupError {
            throw setupError
        }

        var serverError: NSError?
        guard let server = LibboxNewCommandServer(
            self,
            platformInterface,
            &serverError
        ) else {
            throw serverError ?? ClashFluxTunnelError("创建 sing-box 命令服务失败")
        }
        try server.start()
        try server.startOrReloadService(
            configContent,
            options: LibboxOverrideOptions()
        )
        commandServer = server
    }

    private func stopLibbox() {
        guard let server = commandServer else { return }
        try? server.closeService()
        server.close()
        commandServer = nil
    }

    func serviceStop() throws {
        try commandServer?.closeService()
    }

    func serviceReload() throws {
        guard let configFile else {
            throw ClashFluxTunnelError("Packet Tunnel 尚未载入配置")
        }
        let configContent = try String(contentsOf: configFile, encoding: .utf8)
        try commandServer?.startOrReloadService(
            configContent,
            options: LibboxOverrideOptions()
        )
    }

    func getSystemProxyStatus() throws -> LibboxSystemProxyStatus {
        let status = LibboxSystemProxyStatus()
        status.available = false
        status.enabled = false
        return status
    }

    func setSystemProxyEnabled(_ enabled: Bool) throws {
        if enabled {
            throw ClashFluxTunnelError("iOS Packet Tunnel 不支持独立的系统代理开关")
        }
    }

    func triggerNativeCrash() throws {
        throw ClashFluxTunnelError("Packet Tunnel 调试崩溃入口不可用")
    }

    func writeDebugMessage(_ message: String?) {
        guard let message else { return }
        NSLog("Clash-Flux Libbox: %@", message)
    }

    func connectSSHAgent(_ ret0_: UnsafeMutablePointer<Int32>?) throws {
        ret0_?.pointee = -1
        throw ClashFluxTunnelError("iOS Packet Tunnel 不支持 SSH agent 转发")
    }
}

struct ClashFluxTunnelError: LocalizedError {
    let message: String

    init(_ message: String) {
        self.message = message
    }

    var errorDescription: String? { message }
}
