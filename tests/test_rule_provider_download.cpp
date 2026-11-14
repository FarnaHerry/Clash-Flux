// The Python driver supplies an isolated loopback HTTP(S) server and files.
#include <cstdlib>
#include <fstream>
#include <curl/curl.h>

import std;
import clashflux.api;
import clashflux.singbox;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

int testDownloadPool(char** argv) {
    const std::string base = argv[2], tls = argv[3], proxyA = argv[4], proxyB = argv[5];
    const std::filesystem::path root = argv[6];
    api::ClashApi client("", "");
    api::ClashApi::DownloadOptions options;
    options.timeoutSecs = 3;
    options.maxBytes = 1024;
    long coldConnections = 0;
    for (int i = 0; i < 8; ++i) {
        api::ClashApi cold("", "");
        const auto result = cold.downloadToFile(base + "/reuse", root / "cold", options);
        require(result.ok, "fresh-client baseline failed");
        coldConnections += result.download.newConnections;
    }
    require(coldConnections == 8, "fresh-client baseline did not measure its real connections");
    long connections = 0;
    for (int i = 0; i < 8; ++i) {
        const auto file = root / ("pooled-" + std::to_string(i));
        const auto result = client.downloadToFile(base + "/reuse", file, options);
        require(result.ok && result.download.transportKnown, "pooled transfer failed");
        require(result.download.newConnections == (i == 0 ? 1 : 0), "warm download opened another connection");
        require(readFile(file) == "payload: [pool.test]\n", "pooled transfer used another output buffer");
        connections += result.download.newConnections;
    }
    std::cout << "sequential downloads=8 fresh_client_connections=" << coldConnections
              << " pooled_connections=" << connections << '\n';

    // Vendored curl on Windows is built without zlib, so it advertises no
    // Accept-Encoding and receives identity. Gzip assertions only apply when
    // the linked curl can actually decode it; both paths must store the source.
    const bool gzip = (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_LIBZ) != 0;
    if (gzip) {
        const auto compressed = client.downloadToFile(base + "/gzip", root / "compressed", options);
        require(compressed.ok && readFile(root / "compressed") == "payload: [pool.test]\n",
                "HTTP gzip must decode before storing the source");
        const auto preservedCompressed = readFile(root / "compressed");
        require(!client.downloadToFile(base + "/gzip-large", root / "compressed", options).ok &&
                readFile(root / "compressed") == preservedCompressed,
                "decoded size, not compressed transfer size, must enforce maxBytes and retain the old source");
        require(!client.downloadToFile(base + "/gzip-short", root / "compressed", options).ok &&
                readFile(root / "compressed") == preservedCompressed,
                "truncated compressed transfer must retain the old source");
    } else {
        const auto identity = client.downloadToFile(base + "/gzip", root / "compressed", options);
        require(identity.ok && readFile(root / "compressed") == "payload: [pool.test]\n",
                "builds without zlib must store the plain identity response");
    }

    options.headers = {{"Authorization", "Bearer pool-A"}};
    require(client.downloadToFile(base + "/token-a", root / "token", options).download.newConnections == 1,
            "different credential policy must use an isolated connection");
    require(client.downloadToFile(base + "/token-a", root / "token", options).download.newConnections == 0,
            "same credential policy did not reuse a connection");
    options.headers["Authorization"] = "Bearer pool-B";
    const auto tokenB = client.downloadToFile(base + "/token-b", root / "token", options);
    require(tokenB.ok && tokenB.download.newConnections == 1, "credential policy was not isolated");
    options.headers["Authorization"] = "Bearer pool-A";
    options.headers["If-None-Match"] = "\"pool\"";
    options.acceptNotModified = true;
    const auto before = readFile(root / "token");
    const auto conditional = client.downloadToFile(base + "/conditional", root / "token", options);
    require(conditional.ok && conditional.status == 304 && conditional.download.newConnections == 0,
            "conditional request did not reuse its matching transport policy");
    require(readFile(root / "token") == before, "304 replaced the previous representation");
    options.headers.erase("If-None-Match");
    options.acceptNotModified = false;
    const auto cleared = client.downloadToFile(base + "/no-validator", root / "token", options);
    require(cleared.ok && cleared.download.newConnections == 0, "validator leaked into a later request");

    for (int i = 0; i < 5; ++i) {
        options.headers = {{"X-Pool-Policy", std::to_string(i)}};
        const auto result = client.downloadToFile(base + "/reuse", root / "eviction", options);
        require(result.ok && result.download.newConnections == 1, "new header policy reused an unrelated session");
    }
    options.headers = {{"X-Pool-Policy", "0"}};
    require(client.downloadToFile(base + "/reuse", root / "eviction", options).download.newConnections == 1,
            "idle session retention exceeded its bound");

    options.headers.clear();
    std::barrier start(6);
    std::vector<std::future<api::ApiResult>> pending;
    for (int i = 0; i < 6; ++i) {
        pending.push_back(std::async(std::launch::async, [&, i] {
            start.arrive_and_wait();
            return client.downloadToFile(base + "/concurrent-" + std::to_string(i),
                                         root / ("concurrent-" + std::to_string(i)), options);
        }));
    }
    for (int i = 0; i < 6; ++i) {
        require(pending[i].get().ok, "concurrent pooled transfer failed");
        require(readFile(root / ("concurrent-" + std::to_string(i))) ==
                    "payload: [concurrent-" + std::to_string(i) + ".test]\n",
                "concurrent transfers shared callbacks or output state");
    }

    api::ClashApi recovery("", "");
    require(recovery.downloadToFile(base + "/reuse", root / "recovery", options).ok, "recovery seed failed");
    const auto preserved = readFile(root / "recovery");
    require(!recovery.downloadToFile(base + "/truncate", root / "recovery", options).ok,
            "truncated transfer succeeded");
    require(readFile(root / "recovery") == preserved, "failed transfer replaced the old file");
    const auto recovered = recovery.downloadToFile(base + "/reuse", root / "recovery", options);
    require(recovered.ok && recovered.download.newConnections == 1, "failed transport was retained in the pool");
    options.maxBytes = 1;
    require(!recovery.downloadToFile(base + "/reuse", root / "recovery", options).ok &&
                readFile(root / "recovery") == preserved, "pooled size limit did not preserve the old file");
    options.maxBytes = 1024;
    require(recovery.downloadToFile(base + "/reuse", root / "recovery", options).ok,
            "old size limit leaked into the next request");
    bool validated = false;
    options.validate = [&](const auto&) {
        const auto nested = recovery.downloadToFile(base + "/reuse", root / "nested", {});
        validated = nested.ok && nested.download.newConnections == 0;
        return std::string{"fixture candidate refused"};
    };
    require(!recovery.downloadToFile(base + "/reuse", root / "recovery", options).ok && validated &&
                readFile(root / "recovery") == preserved, "candidate validation held or leaked the transport");
    options.validate = {};
    options.subscriptionRequest = true;
    const auto compatible = recovery.downloadToFile(base + "/identity", root / "identity", options);
    require(compatible.ok && compatible.download.attempts == 2,
            "ordinary subscriptions get one bounded 403 user-agent fallback");
    const auto deniedSource = recovery.downloadToFile(base + "/direct-denied", root / "identity", options);
    require(!deniedSource.ok && deniedSource.download.attempts == 2 && deniedSource.failurePhase == "http",
            "persistent HTTP denial is classified and never loops the user-agent fallback");
    options.headers = {{"User-Agent", "explicit-test-agent"}};
    const auto customAgent = recovery.downloadToFile(base + "/identity", root / "identity", options);
    require(!customAgent.ok && customAgent.download.attempts == 1,
            "source fallback must not override caller headers");
    options.headers.clear(); options.subscriptionRequest = false;
    const auto deniedResource = recovery.downloadToFile(base + "/identity", root / "identity", options);
    require(!deniedResource.ok && deniedResource.download.attempts == 1,
            "provider downloads never implicitly change their user agent");
    options.attempts = 2;
    const auto retriedShort = recovery.downloadToFile(base + "/truncate", root / "recovery", options);
    require(!retriedShort.ok && retriedShort.download.attempts == 2 && readFile(root / "recovery") == preserved,
            "bounded transport retries preserve the accepted file on repeated truncation");
    options.attempts = 1;

    const std::string proxyTarget = base + "/direct-denied";
    options.proxyUrl = proxyA;
    const auto firstProxy = client.downloadToFile(proxyTarget, root / "proxy", options);
    const auto reusedProxy = client.downloadToFile(proxyTarget, root / "proxy", options);
    require(firstProxy.ok && firstProxy.download.newConnections == 1 &&
                reusedProxy.ok && reusedProxy.download.newConnections == 0, "explicit proxy reuse failed");
    options.proxyUrl = proxyB;
    const auto changedProxy = client.downloadToFile(proxyTarget, root / "proxy", options);
    require(changedProxy.ok && changedProxy.download.newConnections == 1, "proxy policy was not isolated");
    options.proxyUrl.clear();
    require(!client.downloadToFile(proxyTarget, root / "proxy", options).ok,
            "explicit proxy settings leaked into a direct request");
#if !defined(_WIN32)
    // The driver installs an explicitly empty NO_PROXY. Preserve the distinction
    // from an absent variable even when both currently use the same proxy route.
    require(std::getenv("NO_PROXY") && std::string_view(std::getenv("NO_PROXY")).empty(),
            "environment fixture did not start with an empty bypass value");
    require(::unsetenv("NO_PROXY") == 0, "cannot remove fixture bypass value");
    options.proxyUrl = proxyA;
    const auto changedEnvironment = client.downloadToFile(proxyTarget, root / "proxy", options);
    require(changedEnvironment.ok && changedEnvironment.download.newConnections == 1,
            "absent and empty proxy environments shared a session");
    require(::setenv("NO_PROXY", "", 1) == 0, "cannot restore fixture bypass value");
    options.proxyUrl.clear();
#endif

    if (!tls.empty()) {
        const auto file = root / "tls";
        { std::ofstream output(file); output << "previous trusted subscription"; }
        for (int i = 0; i < 2; ++i) {
            const auto rejected = client.downloadToFile(tls + "/reuse", file, options);
            require(!rejected.ok && rejected.error.find("TLS") != std::string::npos,
                    "invalid TLS certificate must always be rejected");
            require(rejected.error.find("允许无效证书") == std::string::npos &&
                        readFile(file) == "previous trusted subscription",
                    "TLS failure must preserve the previous file without suggesting a bypass");
        }
    }
    require(!std::ranges::any_of(std::filesystem::directory_iterator(root), [](const auto& entry) {
        return entry.path().filename().string().starts_with(".clash-flux-download-");
    }), "pooled transfers leaked staging directories");
    std::cout << "download pool: credentials, validators, bounds, concurrency, failure, proxy and TLS checks passed\n";
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc == 7 && std::string_view(argv[1]) == "--download-pool") {
        try { return testDownloadPool(argv); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--native-runtime") {
        api::NativeApi client(argv[2], argv[3]);
        const auto result = client.runtime();
        if (!result.ok) { std::cerr << result.failurePhase << ": " << result.error << '\n'; return 1; }
        std::cout << result.version << ' ' << result.apiVersion << ' ' << result.startedAt << '\n';
        return 0;
    }
    if (argc != 5 && argc != 6) return 2;
    api::ClashApi client("http://127.0.0.1:1", "");
    api::ClashApi::DownloadOptions options;
    options.timeoutSecs = 2;
    options.maxBytes = static_cast<std::size_t>(std::stoull(argv[3]));
    const bool mrs = argc == 6 && std::string_view(argv[5]) == "mrs";
    const bool conditional = argc == 6 && std::string_view(argv[5]) == "not-modified";
    if (conditional) {
        options.acceptNotModified = true;
        options.headers = {{"If-None-Match", "\"fixture\""}};
    } else if (argc == 6 && !mrs) {
        options.headers = {{"User-Agent", "provider-test/1"},
                           {"Authorization", "Bearer test-only"}, {"X-Token", "local-test"}};
        if (std::string_view(argv[5]) == "invalid") options.headers["X-Token"] = "bad\r\nInjected: yes";
    }
    singbox::HttpRuleProviderResource resource;
    resource.behavior = mrs ? "ipcidr" : "domain";
    resource.format = mrs ? "mrs" : "yaml";
    resource.maxBytes = options.maxBytes;
    bool validated = false;
    options.validate = [&resource, &validated](const auto& file) {
        validated = true;
        std::string error;
        const auto text = singbox::ReadRuleProviderText(file, resource.maxBytes, error);
        if (text) singbox::ValidateHttpRuleProvider(resource, *text, error);
        return error;
    };
    const auto result = client.downloadToFile(argv[1], argv[2], options);
    const bool invalidHeaders = argc == 6 && std::string_view(argv[5]) == "invalid";
    if (!invalidHeaders && (!result.download.transportKnown || result.download.networkMicros < 0 ||
            (result.status != 0 && result.download.newConnections < 1))) {
        std::cerr << "real transfer must report time and measured connection count\n"; return 1;
    }
    if ((result.status == 304 && result.download.bytes != 0) ||
            (!validated && result.download.validationMicros != 0)) {
        std::cerr << "304 and validation metrics must describe actual work\n"; return 1;
    }
    if (result.status == 304 && (validated || (conditional && !result.ok))) {
        std::cerr << "304 must return metadata without validating or replacing a body\n"; return 1;
    }
    const bool expected = std::string_view(argv[4]) == "ok";
    if (result.ok != expected) {
        std::cerr << "unexpected download result: " << result.error << '\n'; return 1;
    }
    return 0;
}
