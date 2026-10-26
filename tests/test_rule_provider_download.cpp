// The Python driver supplies an isolated loopback HTTP(S) server and files.
import std;
import clashflux.api;
import clashflux.singbox;

int main(int argc, char** argv) {
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
    if (result.status == 304 && (validated || (conditional && !result.ok))) {
        std::cerr << "304 must return metadata without validating or replacing a body\n"; return 1;
    }
    const bool expected = std::string_view(argv[4]) == "ok";
    if (result.ok != expected) {
        std::cerr << "unexpected download result: " << result.error << '\n'; return 1;
    }
    return 0;
}
