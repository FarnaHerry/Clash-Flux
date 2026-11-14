void testProviderResourceManager() {
    using namespace provider_resources;
    using namespace std::chrono_literals;
    const auto waitFor = [](const auto& predicate) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        return predicate();
    };
    Request request;
    request.identity = "rule:source-1:full-identity";
    request.url = "https://example.test/resource";
    request.headers = {{"authorization", "test-only-secret"}};
    for (const bool throwing : {false, true}) {
        Manager manager;
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        std::atomic_uint calls = 0;
        const auto load = [&](const Request&) {
            ++calls;
            std::unique_lock lock(mutex);
            const bool released = changed.wait_for(lock, 3s, [&] { return release; });
            if (throwing) throw std::runtime_error("injected transport exception");
            Response response;
            response.ok = released;
            response.status = 200;
            response.body = "payload: [shared.test]";
            return response;
        };
        Outcome owner;
        std::stop_source ownerStop;
        std::jthread first([&] { owner = manager.fetch(request, load, ownerStop.get_token()); });
        check(waitFor([&] { return calls == 1; }), "shared provider owner entered transport");
        std::array<Outcome, 3> followers;
        std::vector<std::jthread> workers;
        for (unsigned i = 0; i < followers.size(); ++i)
            workers.emplace_back([&, i] {
                auto otherSource = request; otherSource.identity += ":source-" + std::to_string(i);
                followers[i] = manager.fetch(otherSource, load);
            });
        std::stop_source stop;
        Outcome cancelled;
        std::jthread waiter([&] { cancelled = manager.fetch(request, load, stop.get_token()); });
        check(waitFor([&] { return manager.snapshot().joins == 4; }),
              "identical identities and request policies join the existing provider request");
        stop.request_stop();
        ownerStop.request_stop();
        waiter.join();
        check(cancelled.cancelled && calls == 1 && manager.snapshot().active == 1,
              "cancelling a follower leaves the owner's shared IO alive");
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all();
        first.join();
        workers.clear();
        check(calls == 1 && owner.cancelled && owner.response && owner.response->ok == !throwing &&
              manager.snapshot().inFlight == 0 && manager.snapshot().active == 0,
              "success and exceptions both complete and retire the single in-flight request");
        for (const auto& follower : followers)
            check(follower.joined && follower.response == owner.response && !follower.cancelled,
                  "current consumers share the same owning immutable response");
        const auto retry = manager.fetch(request, load);
        check(calls == 2 && !retry.joined && retry.response != owner.response,
              "later force refresh or failure retry never reuses a completed flight");
    }
    {
        Manager manager;
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        std::atomic_uint entered = 0;
        Outcome owner;
        std::jthread first([&] {
            owner = manager.fetch(request, [&](const Request&) {
                ++entered;
                std::unique_lock lock(mutex);
                Response response;
                response.ok = changed.wait_for(lock, 3s, [&] { return release; });
                return response;
            });
        });
        check(waitFor([&] { return entered == 1; }), "identity isolation owner is in flight");
        std::vector<Request> distinct;
        auto add = [&](auto change) { auto other = request; change(other); distinct.push_back(std::move(other)); };
        add([](auto& r) { r.url += "/different"; });
        add([](auto& r) { r.headers["authorization"] = "another-test-secret"; });
        add([](auto& r) { r.headers["if-none-match"] = "test-etag"; });
        add([](auto& r) { r.acceptNotModified = true; });
        add([](auto& r) { r.maxBytes /= 2; });
        add([](auto& r) { ++r.timeoutSecs; });
        add([](auto& r) { ++r.attempts; });
        add([](auto& r) { r.allowProxyEnv = true; });
        add([](auto& r) { r.proxyUrl = "http://127.0.0.1:1"; });
        unsigned loads = 0;
        for (const auto& other : distinct) {
            const auto result = manager.fetch(other, [&](const Request&) { ++loads; Response r; r.ok = true; return r; });
            check(result.response && result.response->ok && !result.joined,
                  "URL, credentials, validators, size, retry and proxy policies isolate IO");
        }
        check(loads == distinct.size() && manager.snapshot().joins == 0,
              "full request equality is required rather than filename/hash equality");
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all(); first.join();
    }
    {
        Manager manager(1, 3);
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        std::atomic_uint cancelledLoads = 0, survivingLoads = 0;
        auto blockerRequest = request; blockerRequest.url += "/blocker";
        std::jthread blocker([&] {
            manager.fetch(blockerRequest, [&](const Request&) {
                std::unique_lock lock(mutex);
                Response response;
                response.ok = changed.wait_for(lock, 3s, [&] { return release; });
                return response;
            });
        });
        check(waitFor([&] { return manager.snapshot().active == 1; }), "blocker occupies the transport permit");
        std::stop_source stop;
        Outcome cancelled, survivor;
        std::jthread queued([&] {
            cancelled = manager.fetch(request, [&](const Request&) { ++cancelledLoads; return Response{}; }, stop.get_token());
        });
        check(waitFor([&] { return manager.snapshot().inFlight == 2; }), "cancellable owner is queued");
        std::jthread follower([&] {
            survivor = manager.fetch(request, [&](const Request&) {
                ++survivingLoads;
                Response response; response.ok = true; response.body = "surviving candidate";
                return response;
            });
        });
        check(waitFor([&] { return manager.snapshot().joins == 1; }), "surviving candidate joined the queued request");
        stop.request_stop(); queued.join();
        check(cancelled.cancelled && cancelledLoads == 0,
              "queued cancellation returns without IO or waiting for a transport permit");
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all(); blocker.join(); follower.join();
        check(survivor.response && survivor.response->ok && survivingLoads == 1 &&
              manager.snapshot().inFlight == 0,
              "survivor takes over an abandoned queued request using its own staging lifetime");
    }
    {
        Manager manager(2, 3);
        std::mutex mutex;
        std::condition_variable changed;
        bool release = false;
        std::atomic_uint entered = 0, maximum = 0, active = 0;
        const auto load = [&](const Request&) {
            ++entered;
            const auto count = ++active;
            auto observed = maximum.load();
            while (observed < count && !maximum.compare_exchange_weak(observed, count)) {}
            std::unique_lock lock(mutex);
            Response response;
            response.ok = changed.wait_for(lock, 3s, [&] { return release; });
            --active;
            return response;
        };
        std::array<Outcome, 3> results;
        std::vector<std::jthread> workers;
        for (unsigned i = 0; i < 2; ++i) workers.emplace_back([&, i] {
            auto other = request; other.url += "/" + std::to_string(i);
            results[i] = manager.fetch(other, load);
        });
        check(waitFor([&] { return entered == 2; }), "manager respects two active transport permits");
        workers.emplace_back([&] { auto other = request; other.url += "/queued"; results[2] = manager.fetch(other, load); });
        check(waitFor([&] { return manager.snapshot().inFlight == 3; }), "third resource waits in bounded queue");
        auto excess = request; excess.url += "/overflow";
        const auto rejected = manager.fetch(excess, load);
        check(rejected.response && !rejected.response->ok && !rejected.response->error.empty() && entered == 2,
              "full request queue fails without performing more IO");
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all(); workers.clear();
        check(entered == 3 && maximum == 2 && results[2].queueMicros > 0 &&
              manager.snapshot().inFlight == 0, "queued provider starts only after a permit is released");
        auto small = request; small.maxBytes = 1;
        const auto oversized = manager.fetch(small, [](const Request&) { Response r; r.ok = true; r.body = "xx"; return r; });
        check(oversized.response && !oversized.response->ok && oversized.response->body.empty(),
              "manager never shares a body larger than its request bound");
        std::stop_source stop; stop.request_stop();
        const auto cancelled = manager.fetch(request, load, stop.get_token());
        check(cancelled.cancelled && entered == 3 && manager.snapshot().inFlight == 0,
              "already-cancelled work starts no IO and creates no flight");
    }
}

void testProviderSharedCandidates() {
    using namespace std::chrono_literals;
    const RuleSetTestDirectory directory;
    provider_resources::Manager manager;
    std::mutex mutex;
    std::condition_variable changed;
    bool releaseProxy = false, releaseRule = false;
    std::atomic_uint proxyLoads = 0, ruleLoads = 0;
    std::array<singbox::CompileOptions, 2> options;
    std::array<singbox::CompileResult, 2> results;
    std::array<std::string, 2> errors;
    std::array<bool, 2> ready{};
    std::array<unsigned, 2> checks{}, commits{};
    for (unsigned i = 0; i < options.size(); ++i) {
        auto& candidate = options[i];
        candidate.mainConnectionId = "profile-shared";
        candidate.ruleProviderCacheDir = (directory.path / std::to_string(i) / "rules").string();
        candidate.proxyProviderCacheDir = (directory.path / std::to_string(i) / "proxies").string();
        candidate.profileYaml =
            "proxy-providers:\n  nodes: {type: http, url: https://example.test/nodes}\n"
            "proxy-groups:\n  - {name: Proxy, type: select, use: [nodes]}\n"
            "rule-providers:\n  sites: {type: http, behavior: domain, url: https://example.test/sites}\n"
            "rules: ['RULE-SET,sites,Proxy', 'MATCH,Proxy']\n";
    }
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < options.size(); ++i) workers.emplace_back([&, i] {
        const auto fetch = [&](bool proxy, const auto& resource, const auto& file, std::string& error) {
            provider_resources::Request request;
            request.identity = std::string(proxy ? "proxy:" : "rule:") + resource.identity;
            request.url = resource.url;
            request.maxBytes = resource.maxBytes;
            const auto outcome = manager.fetch(request, [&](const auto&) {
                if (proxy) ++proxyLoads; else ++ruleLoads;
                std::unique_lock lock(mutex);
                provider_resources::Response response;
                response.ok = changed.wait_for(lock, 3s, [&] { return proxy ? releaseProxy : releaseRule; });
                response.status = 200;
                response.body = proxy
                    ? "proxies: [{name: node, type: socks5, server: 127.0.0.1, port: 9}]\n"
                    : "payload: [shared.test]\n";
                return response;
            });
            if (!outcome.response || !outcome.response->ok) { error = "shared transport failed"; return false; }
            std::ofstream output(file, std::ios::binary);
            output << outcome.response->body; output.close();
            return output.good();
        };
        rule_provider_cache::Operations operations;
        operations.fetch = [&](const auto& resource, const auto& file, std::string& error) {
            return fetch(false, resource, file, error);
        };
        operations.fetchProxy = [&](const auto& resource, const auto& file, std::string& error) {
            return fetch(true, resource, file, error);
        };
        operations.check = [&](const auto&, const auto&, std::string& error) {
            ++checks[i];
            if (i == 0) { error = "reject this candidate only"; return false; }
            return true;
        };
        operations.commit = [&](const auto& from, const auto& to, std::string&) {
            ++commits[i];
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
            return true;
        };
        ready[i] = rule_provider_cache::Prepare(options[i], operations, results[i], errors[i]);
    });
    const auto awaitJoins = [&](std::uint64_t count) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (manager.snapshot().joins < count && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        return manager.snapshot().joins >= count;
    };
    check(awaitJoins(1), "two independent candidates share proxy transport before either is checked");
    { std::lock_guard lock(mutex); releaseProxy = true; }
    changed.notify_all();
    check(awaitJoins(2), "two independent candidates share subsequently discovered rule transport");
    { std::lock_guard lock(mutex); releaseRule = true; }
    changed.notify_all(); workers.clear();
    check(proxyLoads == 1 && ruleLoads == 1 && !ready[0] && ready[1] &&
          checks[0] == 1 && checks[1] == 1 && commits[0] == 0 && commits[1] == 2,
          "shared IO never shares candidate approval or lets a rejected candidate commit cache files");
    check(options[0].ruleProviderContents.empty() && options[0].proxyProviderContents.empty() &&
          options[1].ruleProviderContents.size() == 1 && options[1].proxyProviderContents.size() == 1,
          "only the approved candidate publishes immutable provider snapshots");
    check(std::ranges::none_of(std::filesystem::recursive_directory_iterator(directory.path), [](const auto& entry) {
        return entry.path().filename().string().starts_with(".stage-");
    }), "both candidates retire their private staging after success or rejection");
}

void testProxyConditionalRefresh() {
    const RuleSetTestDirectory directory;
    singbox::CompileOptions options;
    options.proxyProviderCacheDir = (directory.path / "proxy-conditional").string();
    options.profileYaml = "proxy-providers:\n  remote: {type: http, url: https://example.test/nodes, interval: 1}\n"
        "proxy-groups: [{name: Proxy, type: select, use: [remote]}]\nrules: ['MATCH,Proxy']\n";
    const auto resource = singbox::compileConfig(options).httpProxyProviders.front();
    const auto file = std::filesystem::path(options.proxyProviderCacheDir) / (resource.cacheKey + ".cache");
    std::filesystem::create_directories(file.parent_path());
    const std::string body = "proxies: [{name: remote-a, type: socks5, server: 127.0.0.1, port: 9}]\n";
    const singbox::HttpCacheValidators original{"\"proxy-v1\"", "Wed, 21 Oct 2026 07:28:00 GMT"};
    std::ofstream(file, std::ios::binary) << singbox::HttpProxyProviderCacheImage(resource, body, original);
    options.forceProviderRefreshSources = {resource.sourceId};
    unsigned fetched = 0, checked = 0, commits = 0;
    bool reject = false;
    rule_provider_cache::Operations operations;
    operations.revalidateProxy = [&](const auto&, const auto&, const auto& validators) {
        ++fetched;
        check(validators == original, "proxy validators belong to the complete verified body");
        rule_provider_cache::FetchResult response;
        response.ok = true; response.notModified = true; response.status = 304;
        return response;
    };
    operations.check = [&](const auto&, const auto&, std::string& reason) {
        ++checked; if (reject) reason = "reject proxy candidate"; return !reject;
    };
    operations.commit = [&](const auto& from, const auto& to, std::string&) {
        ++commits; std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing); return true;
    };
    singbox::CompileResult result;
    std::string error;
    check(rule_provider_cache::Prepare(options, operations, result, error) && fetched == 1 && checked == 1 && commits == 1,
          "proxy 304 avoids a full body while preserving candidate check and atomic commit");
    singbox::HttpCacheValidators recovered;
    check(singbox::ReadHttpProxyProviderCache(resource, options.proxyProviderCacheDir, error, &recovered) == body && recovered == original,
          "proxy v2 cache round-trips body and validators without changing node semantics");
    const auto image = singbox::ReadRuleProviderText(file, 8192, error);
    reject = true;
    auto candidate = options;
    check(!rule_provider_cache::Prepare(candidate, operations, result, error) && commits == 1 &&
          singbox::ReadRuleProviderText(file, 8192, error) == image,
          "rejected proxy 304 candidate retains the previously accepted cache");
    reject = false;
    operations.publish = [](const std::function<bool()>&, std::string& reason) {
        reason = "source changed after check"; return false;
    };
    candidate = options;
    check(!rule_provider_cache::Prepare(candidate, operations, result, error) && commits == 1 &&
          singbox::ReadRuleProviderText(file, 8192, error) == image,
          "publication guard rejects a stale prepared candidate before any cache replacement");
}

void testSourceParseReuse() {
    singbox::CompileOptions source;
    source.profileYaml = "# isolated source parse cache\nproxies: [{name: original, type: socks5, server: 127.0.0.1, port: 9}]\n"
        "proxy-groups: [{name: Proxy, type: select, proxies: [original, DIRECT]}]\nrules: ['MATCH,Proxy']\n";
    const auto initial = singbox::compileConfig(source);
    auto changedMode = source; changedMode.mode = "global";
    const auto warm = singbox::compileConfig(changedMode);
    auto coldMode = changedMode; coldMode.profileYaml += "# distinct bytes, identical semantics\n";
    const auto cold = singbox::compileConfig(coldMode);
    check(initial.error.empty() && warm.error.empty() && cold.error.empty() && warm.sourceParseCacheHits == 1 &&
          warm.json == cold.json && warm.fidelity == cold.fidelity,
          "source parse reuse across runtime settings preserves full compilation and fidelity");
    auto edited = source;
    edited.profileYaml.replace(edited.profileYaml.find("port: 9"), 7, "port: 8");
    const auto next = singbox::compileConfig(edited);
    check(next.error.empty() && next.json != initial.json,
          "source parsing is keyed by complete bytes rather than source identity or filename");
    for (int i = 0; i < 8; ++i) {
        auto other = source; other.profileYaml += "# eviction " + std::to_string(i);
        check(singbox::compileConfig(other).error.empty(), "bounded parsing cache accepts independent source bytes");
    }
    check(singbox::compileConfig(source).json == initial.json, "parse cache eviction cannot mutate prior source semantics");
    std::atomic_uint passed = 0;
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        const auto first = singbox::compileConfig(source);
        const auto second = singbox::compileConfig(source);
        if (first.json == initial.json && second.json == initial.json && second.sourceParseCacheHits == 1) ++passed;
    });
    workers.clear();
    check(passed == 4, "mutable compilations never share a YAML node graph between workers");
}
