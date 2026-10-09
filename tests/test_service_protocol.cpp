#include "../src/service_protocol.h"
#include <iostream>
#include <stdexcept>
using namespace clashflux::service_protocol;
void require(bool condition) { if (!condition) throw std::runtime_error("service protocol regression"); }
int main() {
    for (const std::string value : {std::string{}, std::string("password with spaces\n第二行"), std::string("C:\\用户\\core\\config.json")})
        require(unhex(hex(value)) == value);
    for (const auto* value : {"", "0", "gg", "00", "410042", "-1"}) require(!unhex(value));
    require(!unhex(std::string(kMaxCommand + 2, 'a')));
    require(words("START abc") == std::vector<std::string>({"START", "abc"}));
    for (const auto* value : {" START abc", "START  abc", "START abc "}) require(words(value).empty());
    std::cout << "service protocol: owning Unicode fields, empty values, malformed/bounded input passed\n";
}
