// Used only by the test helper; never requests authorization or starts a package manager.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc != 5 || std::string(argv[1]) != "--disable-internal-agent") return 90;
    const fs::path root(std::getenv("CLASHFLUX_UPDATE_TEST_ROOT"));
    std::ofstream(root / "started") << argv[2] << ' ' << argv[3];
    for (unsigned i = 0; !fs::exists(root / "allow") && i < 500; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!fs::exists(root / "allow")) return 91;
    std::ifstream snapshot(argv[4]);
    std::string contents((std::istreambuf_iterator<char>(snapshot)), {});
    if (contents != "abc") return 92;
    std::ofstream writeAttempt(argv[4], std::ios::binary | std::ios::app);
    writeAttempt << 'x';
    writeAttempt.flush();
    if (writeAttempt) return 93; // The retained snapshot must reject writes.
    const int code = std::atoi(std::getenv("CLASHFLUX_UPDATE_TEST_RESULT"));
    if (code) std::cerr << "fixture authorization/package failure " << code << '\n';
    else std::ofstream(root / "installed") << "abc";
    return code;
}
