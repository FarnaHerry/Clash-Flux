#include "../src/win32_raii.h"
#include <fstream>

// No GUI, installer engine, privileges, network, service or application data.
int main() {
    DWORD pid = 0;
    std::ifstream("expected-parent.txt") >> pid;
    clashflux::win32::UniqueHandle parent(OpenProcess(SYNCHRONIZE, FALSE, pid));
    const bool exited = !parent.valid() || WaitForSingleObject(parent.get(), 0) == WAIT_OBJECT_0;
    std::ofstream("installed.txt") << (exited ? "parent-exited" : "parent-running");
    return exited ? 0 : 1;
}
