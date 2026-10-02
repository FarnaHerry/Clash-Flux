#include "../src/profile_link.h"

#include <cstdio>
#include <string>

int main() {
    namespace links = clashflux::profile_link;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    std::string error;
    auto value = links::Parse("sing-box://import-remote-profile?url=https%3A%2F%2Fexample.test%2Fsub%3Ftoken%3Da%252Bb%26x%3D1#%E8%AE%A2%E9%98%85+A", error);
    check(value && value->url == "https://example.test/sub?token=a%2Bb&x=1" &&
          value->name == "订阅+A", "URL decodes once, fragment plus and UTF-8 name survive");
    for (const char* scheme : {"flclash", "clash", "clashmeta", "clash-flux"}) {
        value = links::Parse(std::string(scheme) + "://install-config?url=http%3A%2F%2Flocalhost%3A8080%2Fsub&name=A+B", error);
        check(value && value->url == "http://localhost:8080/sub" && value->name == "A B",
              "FlClash aliases and query name");
    }
    value = links::Parse("SING-BOX://IMPORT-REMOTE-PROFILE/?url=https://example.test/a&name=query#fragment", error);
    check(value && value->name == "fragment", "case-insensitive scheme and fragment name precedence");
    for (const char* invalid : {
        "sing-box://install-config?url=https://example.test",
        "flclash://install-config/extra?url=https://example.test",
        "flclash://install-config",
        "flclash://install-config?url=",
        "flclash://install-config?url=file%3A%2F%2F%2Ftmp%2Fa",
        "flclash://install-config?url=javascript%3Aalert(1)",
        "flclash://install-config?url=https%3A%2F%2F",
        "flclash://install-config?url=https://example.test&url=https://other.test",
        "flclash://install-config?url=https://example.test&name=a&name=b",
        "flclash://install-config?url=https://example.test/%ZZ",
        "flclash://install-config?url=https://example.test/%",
        "flclash://install-config?url=https://example.test/%00",
        "flclash://install-config?url=https://example.test/%0A",
        "flclash://install-config?url=https://example.test#name%0D",
        "other://install-config?url=https://example.test"}) {
        check(!links::Parse(invalid, error) && !error.empty(), "malformed or ambiguous link rejected with reason");
    }
    check(!links::Parse("flclash://install-config?url=https://example.test/" +
          std::string(65536, 'a'), error), "bounded external payload");
    int wakes = 0;
    check(links::Submit({"https://first.test", "first"}, error), "cold-start request queues before UI");
    check(links::Submit({"https://first.test", "first"}, error), "duplicate cold activation accepted once");
    links::SetWakeHandler([&] { ++wakes; });
    check(wakes == 1, "installing UI handler wakes an existing request");
    check(links::Submit({"https://second.test", "second"}, error) && wakes == 2,
          "subsequent request wakes UI");
    const auto queue = links::TakePending();
    check(queue.size() == 2 && queue[0].name == "first" && queue[1].name == "second",
          "cold and warm activations retain FIFO ordering");
    check(links::TakePending().empty(), "each request consumed once");
    links::SetWakeHandler({});
    for (int i = 0; i < 16; ++i)
        check(links::Submit({"https://example.test", std::to_string(i)}, error), "bounded queue accepts its capacity");
    check(!links::Submit({"https://example.test", "overflow"}, error) && !error.empty(),
          "bounded queue refuses overflow with feedback");
    links::TakePending();
    return failures ? 1 : 0;
}
