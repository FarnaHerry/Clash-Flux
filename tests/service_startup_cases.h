#pragma once
#include "../src/service_startup.h"
#include <stdexcept>
#include <vector>

inline void testServiceStartup() {
    struct Probe {
        bool reachable = false;
        bool matches = false;
        bool compatible() const { return reachable && matches; }
    };
    struct Fixture {
        bool installed = true;
        Probe probe{true, false};
        bool upgradeWorks = true;
        std::vector<std::string> calls;
        clashflux::service_startup::Result run() {
            return clashflux::service_startup::synchronize(
                [this] { calls.push_back("installed"); return installed; },
                [this] { calls.push_back("query"); return probe; },
                [this](std::string& error) {
                    calls.push_back("ensure");
                    if (upgradeWorks) probe = {true, true};
                    if (probe.compatible()) { error.clear(); return true; }
                    error = "update or connection failure";
                    return false;
                });
        }
    };
    const auto check = [](bool ok) {
        if (!ok) throw std::runtime_error("startup service regression");
    };
    Fixture absent;
    absent.installed = false;
    const auto absentResult = absent.run();
    check(!absentResult.installed && absentResult.ready && absent.calls == std::vector<std::string>{"installed"});
    Fixture current;
    current.probe = {true, true};
    check(current.run().ready && current.calls == std::vector<std::string>{"installed", "query"});
    Fixture windows;
    const auto updated = windows.run();
    check(updated.ready && updated.updated && updated.error.empty());
    check(windows.calls == std::vector<std::string>{"installed", "query", "ensure"});
    windows.calls.clear();
    check(windows.run().ready && windows.calls == std::vector<std::string>{"installed", "query"});
    Fixture cancelled;
    cancelled.upgradeWorks = false;
    const auto cancelledResult = cancelled.run();
    check(!cancelledResult.ready && !cancelledResult.updated && cancelledResult.error == "update or connection failure");
    check(cancelled.calls.back() == "ensure" && cancelled.calls.size() == 3);
    Fixture broken;
    broken.probe = {false, false};
    broken.upgradeWorks = false;
    const auto brokenResult = broken.run();
    check(!brokenResult.ready && brokenResult.error == "update or connection failure");
    check(broken.calls == std::vector<std::string>{"installed", "query", "ensure"});
    Fixture linuxFixture;
    const auto linuxResult = linuxFixture.run();
    check(linuxResult.ready && linuxResult.updated && linuxFixture.calls == std::vector<std::string>{"installed", "query", "ensure"});
    Fixture oldLinux;
    oldLinux.upgradeWorks = false;
    check(!oldLinux.run().ready && oldLinux.calls == std::vector<std::string>{"installed", "query", "ensure"});
    // A stopped installed daemon can become reachable without reinstalling it.
    Fixture stopped;
    stopped.probe = {false, false};
    const auto started = stopped.run();
    check(started.ready && !started.updated && stopped.calls == std::vector<std::string>{"installed", "query", "ensure"});
}
