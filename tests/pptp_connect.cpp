// 开发用 PPTP 实连工具。
//
// 该工具不加入 CTest，也不把密码放在 argv 中：
//   ./build/test_pptp_connect /path/to/pptp.conf 10.20.0.0/16
// 连接成功后按回车断开并清理路由/PPP 进程。
// Linux 需先在 Clash-Flux 设置中安装 root 服务；本工具本身不再要求 sudo。
#include <iostream>

import std;
import clashflux.pptp;
import clashflux.vpn;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr,
                     "用法：{} <pptp.conf> [内网 CIDR ...]\n"
                     "配置文件应为 key=value，包含 server/username/password；"
                     "连接后按回车断开",
                     argc > 0 ? argv[0] : "test_pptp_connect");
        return 2;
    }

    std::ifstream configFile(argv[1], std::ios::binary);
    if (!configFile) {
        std::println(stderr, "无法读取 PPTP 配置文件：{}", argv[1]);
        return 2;
    }
    const std::string configText{std::istreambuf_iterator<char>(configFile),
                                 std::istreambuf_iterator<char>()};

    vpn::VpnConnection connection{
        .id = "local-pptp-test",
        .name = "本地 PPTP 测试",
        .kind = vpn::ConnectionKind::Pptp,
        .enginePreference = {vpn::EngineKind::SystemPptp},
        .nativeConfig = configText,
    };
    for (int index = 2; index < argc; ++index) {
        connection.internalRoutes.emplace_back(argv[index]);
    }

    vpn::VpnManager manager;
    manager.setAdapters({pptp::MakePptpAdapter()});
    manager.setConnections({std::move(connection)});

    std::string error;
    if (!manager.connect("local-pptp-test", error)) {
        std::println(stderr, "PPTP 连接失败：{}", error);
        return 1;
    }

    const vpn::VpnConnection* connected =
        manager.findConnection("local-pptp-test");
    std::println("PPTP 已连接：接口={} 网关={}，按回车断开",
                 connected != nullptr ? connected->interfaceName : "",
                 connected != nullptr ? connected->gateway : "");
    std::string line;
    std::getline(std::cin, line);
    manager.disconnect("local-pptp-test");
    std::println("PPTP 已断开，路由和临时凭据已清理");
    return 0;
}
