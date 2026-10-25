#include "../src/native_control_wire.h"
#include "../src/service_protocol.h"
#include "service_startup_cases.h"
#include "runtime_health_cases.h"
#include <iostream>
#include <stdexcept>
using namespace clashflux::service_protocol;
void require(bool condition) { if (!condition) throw std::runtime_error("service protocol regression"); }
int main() {
    {
        namespace native = clashflux::native_control;
        const auto frame = [](unsigned char flag, const std::string& value) {
            std::string result(5, '\0'); result[0] = static_cast<char>(flag);
            for (int i = 0; i < 4; ++i) result[4 - i] = static_cast<char>(value.size() >> (8 * i));
            return result + value;
        };
        const std::string version("\x0a\x06" "1.14.3" "\x10\x04", 10);
        const auto valid = frame(0, version) + frame(128, "grpc-status: 0\r\n");
        require(native::UnaryMessage(valid) == version);
        int grpc = -1;
        require(!native::UnaryMessage(frame(128, "grpc-status: 16\r\n"), &grpc) && grpc == 16);
        require(!native::UnaryMessage(frame(128, "grpc-status: 7\r\n"), &grpc) && grpc == 7);
        require(!native::UnaryMessage(frame(128, "grpc-status: garbage\r\n"), &grpc) && grpc == -1);
        require(!native::UnaryMessage(frame(128, "grpc-status: 17\r\n"), &grpc) && grpc == -1);
        const auto decoded = native::DecodeVersion(version);
        require(decoded && decoded->version == "1.14.3" && decoded->api == 4);
        require(native::DecodeStartedAt(std::string("\x08\x81\x01", 3)) == 129);
        for (const auto& invalid : {valid.substr(0, valid.size() - 1), frame(0, version),
                frame(1, version) + frame(128, "grpc-status: 0\r\n"),
                frame(0, version) + frame(128, "grpc-status: 16\r\n"),
                frame(0, version) + frame(0, version) + frame(128, "grpc-status: 0\r\n"),
                frame(0, version) + frame(128, "grpc-status: 0\r\ngrpc-status: 0\r\n"),
                valid + frame(0, "trailing"), std::string(65537, 'x')})
            require(!native::UnaryMessage(invalid));
        require(!native::DecodeVersion(version.substr(0, 8)));
        require(!native::DecodeVersion(version + version));
        require(!native::DecodeStartedAt(std::string("\x08\0", 2)));
        require(!native::DecodeStartedAt(std::string("\x08\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 11)));
        require(!native::DecodeStartedAt(std::string("\x08\xff\xff\xff\xff\xff\xff\xff\xff\xff\x02", 11)));
    }

    testServiceStartup();
    testRuntimeHealth();
    require(pptpEvidence("CONNECTED") == true);
    require(pptpEvidence("DISCONNECTED") == false);
    require(!confirmedPptpDisconnect(std::nullopt));
    require(!confirmedPptpDisconnect(pptpEvidence("CONNECTED")));
    require(confirmedPptpDisconnect(pptpEvidence("DISCONNECTED")));
    for (const auto reply : {"", "ERR denied", "CONNECTED ", "DISCONNECTED 1", "UNKNOWN"}) {
        require(!pptpEvidence(reply).has_value());
        require(!confirmedPptpDisconnect(pptpEvidence(reply)));
    }
    require(runningEvidence("RUNNING 123") == true);
    require(runningEvidence("STOPPED") == false);
    for (auto value : {"", "ERR denied", "RUNNING", "RUNNING -1", "RUNNING 1junk", "RUNNING 0"}) require(!runningEvidence(value).has_value());
    for (const std::string value : {std::string{}, std::string("password with spaces\n第二行"), std::string("C:\\用户\\core\\config.json")})
        require(unhex(hex(value)) == value);
    for (const auto* value : {"", "0", "gg", "00", "410042", "-1"}) require(!unhex(value));
    require(!unhex(std::string(kMaxCommand + 2, 'a')));
    require(words("START abc") == std::vector<std::string>({"START", "abc"}));
    for (const auto* value : {" START abc", "START  abc", "START abc "}) require(words(value).empty());
    std::cout << "service protocol and startup: version update, update failure, absent/unreachable services passed\n";
}
