#include "../src/network_settings.h"
#include "../src/pac_server.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <iostream>
import std;
import clashflux.sysproxy;

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--pac-server") {
        std::string error;
        const auto script = clashflux::network::PacScript("127.0.0.1", 19099, ".example.test\n*.internal.test\nEXACT.TEST");
        assert(!script.empty());
        assert(clashflux::network::PacScript("bad'host", 1, "").empty());
        assert(clashflux::network::PacScript("127.0.0.1", 1, "x';bad").empty());
        clashflux::network::PacServer server;
        const int port = std::stoi(argv[2]);
        assert(server.Start(port, script, "fixture", error));
        clashflux::network::PacServer duplicate;
        assert(!duplicate.Start(port, script, "another", error));
        std::cout << server.url() << std::endl;
        std::string command;
        std::getline(std::cin, command);
        return 0;
    }
    // This binary is run only with fake desktop utilities and an isolated HOME.
    if (argc != 2 || !std::getenv("CLASHFLUX_PROXY_TEST_STATE")) return 2;
    std::string error;
    const auto original = sysproxy::capture(error);
    assert(original && original->desktop == argv[1]);
    assert(sysproxy::enable("127.0.0.1", 19099, error));
    assert(sysproxy::setBypass({"localhost", "*.example.test", "192.0.2.1"}, error));
    const auto applied = sysproxy::capture(error);
    assert(applied && *applied != *original);
    assert(sysproxy::enabled() && sysproxy::currentProxy() == "127.0.0.1:19099");
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    assert(sysproxy::enablePac("https://trusted.test/proxy.pac", error));
    const auto pac = sysproxy::capture(error);
    assert(pac && *pac != *original);
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    assert(!sysproxy::enablePac("https://bad.test/\nunsafe", error));
    assert(sysproxy::capture(error) == original);
    setenv("CLASHFLUX_PROXY_FAIL_KEY", std::string(argv[1]) == "gnome" ? "mode" : "ProxyType", 1);
    assert(!sysproxy::enablePac("https://trusted.test/fail.pac", error));
    unsetenv("CLASHFLUX_PROXY_FAIL_KEY");
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    if (original->desktop == "kde") {
        auto legacy = *original;
        std::erase_if(legacy.values, [](const auto& value) { return value.key == "Proxy Config Script"; });
        assert(clashflux::network::ProxySnapshotMatches(legacy, *original));
        assert(sysproxy::restore(legacy, error));
    }
    assert(sysproxy::enable("127.0.0.1", 19102, error, false));
    const auto httpOnly = sysproxy::capture(error);
    assert(httpOnly);
    for (const auto& field : httpOnly->values) {
        if (field.key == "socksProxy") assert(field.value.empty());
        if (field.scope == ".socks" && field.key == "port") assert(field.value == "0");
    }
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    auto invalid = *original;
    invalid.values[0].key = "mode; touch /tmp/must-not-run";
    assert(!sysproxy::restore(invalid, error));
    assert(sysproxy::capture(error) == original);
    setenv("CLASHFLUX_PROXY_FAIL_KEY", std::string(argv[1]) == "gnome" ? "https.port" : "httpsProxy", 1);
    assert(!sysproxy::enable("127.0.0.1", 19100, error));
    unsetenv("CLASHFLUX_PROXY_FAIL_KEY");
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    assert(sysproxy::enable("::1", 19101, error));
    assert(sysproxy::setBypass({}, error));
    assert(sysproxy::restore(*original, error));
    assert(sysproxy::capture(error) == original);
    std::cout << original->desktop << ": capture, target, bypass, partial failure, restore and invalid snapshot passed\n";
}
