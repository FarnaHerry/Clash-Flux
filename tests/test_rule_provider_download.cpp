// The Python driver supplies an isolated loopback HTTP(S) server and files.
import std;
import clashflux.api;
import clashflux.singbox;

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) return 2;
    api::ClashApi client("http://127.0.0.1:1", "");
    api::ClashApi::DownloadOptions options;
    options.timeoutSecs = 2;
    options.maxBytes = static_cast<std::size_t>(std::stoull(argv[3]));
    const bool mrs = argc == 6 && std::string_view(argv[5]) == "mrs";
    if (argc == 6 && !mrs) {
        options.headers = {{"User-Agent", "provider-test/1"},
                           {"Authorization", "Bearer test-only"}, {"X-Token", "local-test"}};
        if (std::string_view(argv[5]) == "invalid") options.headers["X-Token"] = "bad\r\nInjected: yes";
    }
    singbox::HttpRuleProviderResource resource;
    resource.behavior = mrs ? "ipcidr" : "domain";
    resource.format = mrs ? "mrs" : "yaml";
    resource.maxBytes = options.maxBytes;
    options.validate = [&resource](const auto& file) {
        std::string error;
        const auto text = singbox::ReadRuleProviderText(file, resource.maxBytes, error);
        if (text) singbox::ValidateHttpRuleProvider(resource, *text, error);
        return error;
    };
    const auto result = client.downloadToFile(argv[1], argv[2], options);
    const bool expected = std::string_view(argv[4]) == "ok";
    if (result.ok != expected) {
        std::cerr << "unexpected download result: " << result.error << '\n'; return 1;
    }
    return 0;
}
