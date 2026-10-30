#pragma once
#include "../src/service_startup.h"
#include <stdexcept>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

inline void testServiceChildReplacement() {
    const auto check = [](bool ok) {
        if (!ok) throw std::runtime_error("service child replacement regression");
    };
    int child = 41, spawns = 0;
    std::string error;
    auto spawn = [&](std::string&) { ++spawns; return 42; };
    check(!clashflux::service_startup::restartChild(child,
        [](int&, std::string& failure) { failure = "stop denied"; return false; }, spawn, error));
    check(child == 41 && spawns == 0 && error == "stop denied");
    // A positive PID cannot be overwritten even if a backend reports success
    // without releasing its previous child identity.
    check(!clashflux::service_startup::restartChild(child,
        [](int&, std::string&) { return true; }, spawn, error));
    check(child == 41 && spawns == 0 && !error.empty());
    check(clashflux::service_startup::restartChild(child,
        [](int& pid, std::string&) { pid = 0; return true; }, spawn, error));
    check(child == 42 && spawns == 1 && error.empty());
    check(!clashflux::service_startup::restartChild(child,
        [](int& pid, std::string&) { pid = 0; return true; },
        [](std::string& failure) { failure = "spawn denied"; return -1; }, error));
    check(child == 0 && error == "spawn denied");
    check(clashflux::service_startup::restartChild(child,
        [](int&, std::string&) -> bool { throw std::runtime_error("absent child must not be stopped"); },
        spawn, error));
    check(child == 42 && spawns == 2 && error.empty());

#if defined(__linux__) && !defined(__ANDROID__)
    // Own real children, never a system service or another process's PID.
    struct ChildOwner {
        pid_t pid = 0;
        ~ChildOwner() {
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
    } owner;
    const auto forkChild = [](std::string&) {
        const auto pid = ::fork();
        if (pid == 0) { for (;;) ::pause(); }
        return pid;
    };
    const auto stopChild = [](pid_t& pid, std::string& failure) {
        if (::kill(pid, SIGKILL) != 0) { failure = "kill failed"; return false; }
        pid_t result;
        do { result = ::waitpid(pid, nullptr, 0); } while (result < 0 && errno == EINTR);
        if (result != pid) { failure = "wait failed"; return false; }
        pid = 0;
        return true;
    };
    check(clashflux::service_startup::restartChild(owner.pid, stopChild, forkChild, error));
    const auto previous = owner.pid;
    bool calledSpawn = false;
    check(!clashflux::service_startup::restartChild(owner.pid,
        [](pid_t&, std::string& failure) { failure = "injected stop failure"; return false; },
        [&](std::string& message) { calledSpawn = true; return forkChild(message); }, error));
    check(!calledSpawn && owner.pid == previous && ::kill(previous, 0) == 0);
    check(clashflux::service_startup::restartChild(owner.pid, stopChild,
        [&](std::string& message) {
            check(owner.pid == 0); // prior child was reaped before replacement
            return forkChild(message);
        }, error));
    check(owner.pid > 0 && owner.pid != previous && error.empty());
    check(stopChild(owner.pid, error));
#endif
}

inline void testServiceStartup() {
    testServiceChildReplacement();
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
