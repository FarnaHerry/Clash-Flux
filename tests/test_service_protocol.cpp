#include "../src/service_protocol.h"
#include "service_startup_cases.h"
#include "runtime_health_cases.h"
#include <iostream>
#include <stdexcept>
using namespace clashflux::service_protocol;
void require(bool condition) { if (!condition) throw std::runtime_error("service protocol regression"); }
int main() {
    testServiceStartup();
    testRuntimeHealth();
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
