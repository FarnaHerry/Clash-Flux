#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>
#include <fwpmu.h>
#include <combaseapi.h>
#include <cstdio>
#include "../src/win32_raii.h"

import std;
import nlohmann.json;

namespace {
void check(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
std::string winError(std::string_view operation, std::string_view, ULONG code) {
    return std::format("{} ({})", operation, code);
}
std::string addressText(const IN_ADDR& address) {
    char text[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address, text, sizeof(text)) ? text : "";
}
#include "../src/windows_physical_dns.inc"
#include "../src/windows_native_dns_tool.inc"

struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
struct Resolver {
    Socket listener{socket(AF_INET, SOCK_STREAM, 0)};
    unsigned short port = 0;
    std::atomic<bool> served = false;
    std::jthread worker;
    explicit Resolver(int mode, unsigned short requestedPort = 0) {
        sockaddr_in local{}; local.sin_family = AF_INET;
        local.sin_port = htons(requestedPort);
        InetPtonA(AF_INET, "127.0.0.241", &local.sin_addr);
        check(bind(listener.value, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "bind resolver");
        check(listen(listener.value, 4) == 0, "listen resolver");
        int length = sizeof(local);
        check(getsockname(listener.value, reinterpret_cast<sockaddr*>(&local), &length) == 0, "resolver port");
        port = ntohs(local.sin_port);
        worker = std::jthread([this, mode](std::stop_token stop) {
            while (!stop.stop_requested()) {
                fd_set set; FD_ZERO(&set); FD_SET(listener.value, &set);
                timeval timeout{0, 100000};
                if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
                Socket client{accept(listener.value, nullptr, nullptr)};
                if (client.value == INVALID_SOCKET) continue;
                DWORD socketTimeout = 1000;
                setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&socketTimeout), sizeof(socketTimeout));
                auto read = [&](unsigned char* data, int size) {
                    for (int done = 0; done < size;) {
                        const int count = recv(client.value, reinterpret_cast<char*>(data + done), size - done, 0);
                        if (count <= 0) return false;
                        done += count;
                    }
                    return true;
                };
                unsigned char size[2]{};
                if (!read(size, 2)) continue;
                std::vector<unsigned char> answer((size[0] << 8) | size[1]);
                if (answer.size() < 12 || !read(answer.data(), static_cast<int>(answer.size()))) continue;
                served = true;
                answer[2] = 0x81; answer[3] = mode == 2 ? 0x83 : 0x80;
                answer[6] = 0; answer[7] = 1;
                if (mode == 1) answer[0] ^= 1;
                const unsigned char record[]{0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 0, 2, 99};
                answer.insert(answer.end(), std::begin(record), std::end(record));
                std::vector<unsigned char> frame{static_cast<unsigned char>(answer.size() >> 8),
                    static_cast<unsigned char>(answer.size())};
                frame.insert(frame.end(), answer.begin(), answer.end());
                // Fragment both TCP framing and DNS payload deliberately.
                for (const auto byte : frame)
                    if (send(client.value, reinterpret_cast<const char*>(&byte), 1, 0) != 1) break;
            }
        });
    }
};

struct WfpFixture {
    HANDLE engine = nullptr;
    ~WfpFixture() { if (engine) FwpmEngineClose0(engine); }
    void install() {
        FWPM_SESSION0 session{}; session.flags = FWPM_SESSION_FLAG_DYNAMIC;
        check(FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine) == ERROR_SUCCESS,
            "WFP fixture requires administrator");
        GUID key{}; check(CoCreateGuid(&key) == S_OK, "fixture UUID");
        FWPM_SUBLAYER0 layer{}; layer.subLayerKey = key; layer.weight = 65535;
        layer.displayData.name = const_cast<wchar_t*>(L"Clash-Flux isolated DNS regression");
        check(FwpmSubLayerAdd0(engine, &layer, nullptr) == ERROR_SUCCESS, "add WFP fixture sublayer");
        const auto core = std::filesystem::path([] {
            std::wstring value(32768, L'\0');
            value.resize(GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()))); return value;
        }()).parent_path() / "engines" / "sing-box.exe";
        FWP_BYTE_BLOB* appId = nullptr;
        check(FwpmGetAppIdFromFileName0(core.c_str(), &appId) == ERROR_SUCCESS, "core app ID");
        struct Memory {
            FWP_BYTE_BLOB* value;
            ~Memory() { void* pointer = value; FwpmFreeMemory0(&pointer); }
        } memory{appId};
        FWPM_FILTER_CONDITION0 identity{};
        identity.fieldKey = FWPM_CONDITION_ALE_APP_ID; identity.matchType = FWP_MATCH_EQUAL;
        identity.conditionValue.type = FWP_BYTE_BLOB_TYPE; identity.conditionValue.byteBlob = appId;
        FWPM_FILTER_CONDITION0 conditions[2]{};
        conditions[0].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
        conditions[0].matchType = FWP_MATCH_EQUAL;
        conditions[0].conditionValue.type = FWP_UINT32;
        conditions[0].conditionValue.uint32 = 0x7f0000f1;
        conditions[1].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        conditions[1].matchType = FWP_MATCH_EQUAL;
        conditions[1].conditionValue.type = FWP_UINT16; conditions[1].conditionValue.uint16 = 53;
        FWPM_FILTER0 permit{}; permit.subLayerKey = key; permit.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
        permit.displayData.name = const_cast<wchar_t*>(L"Clash-Flux isolated native permit");
        permit.action.type = FWP_ACTION_PERMIT; permit.weight.type = FWP_UINT8; permit.weight.uint8 = 13;
        permit.flags = FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT;
        FWPM_FILTER_CONDITION0 permitConditions[]{identity, conditions[0], conditions[1]};
        permit.numFilterConditions = 3; permit.filterCondition = permitConditions;
        const auto permitStatus = FwpmFilterAdd0(engine, &permit, nullptr, nullptr);
        check(permitStatus == ERROR_SUCCESS, winError("add native executable permit", {}, permitStatus));
        FWPM_FILTER0 block{}; block.subLayerKey = key; block.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
        block.displayData.name = const_cast<wchar_t*>(L"Clash-Flux isolated DNS block");
        block.action.type = FWP_ACTION_BLOCK; block.weight.type = FWP_UINT8; block.weight.uint8 = 10;
        block.numFilterConditions = 2; block.filterCondition = conditions;
        const auto blockStatus = FwpmFilterAdd0(engine, &block, nullptr, nullptr);
        check(blockStatus == ERROR_SUCCESS, winError("add isolated DNS block", {}, blockStatus));
    }
};

bool directBlocked() {
    Socket socket{::socket(AF_INET, SOCK_STREAM, 0)};
    u_long nonblocking = 1; ioctlsocket(socket.value, FIONBIO, &nonblocking);
    sockaddr_in remote{}; remote.sin_family = AF_INET; remote.sin_port = htons(53);
    InetPtonA(AF_INET, "127.0.0.241", &remote.sin_addr);
    if (connect(socket.value, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) return false;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return true;
    fd_set write, errors; FD_ZERO(&write); FD_ZERO(&errors);
    FD_SET(socket.value, &write); FD_SET(socket.value, &errors);
    timeval timeout{0, 500000};
    if (select(0, nullptr, &write, &errors, &timeout) <= 0) return true;
    int status = 0, size = sizeof(status);
    return getsockopt(socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size) != 0 || status != 0;
}
}

int main(int argc, char** argv) {
    try {
        WSADATA data{}; check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
        struct Network { ~Network() { WSACleanup(); } } network;
        ULONG index = 0; NET_LUID luid{};
        check(ConvertInterfaceAliasToLuid(L"Loopback Pseudo-Interface 1", &luid) == NO_ERROR ||
            ConvertInterfaceIndexToLuid(1, &luid) == NO_ERROR, "loopback interface");
        check(ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR, "loopback index");
        {
            Socket listener{socket(AF_INET, SOCK_STREAM, 0)};
            sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int size = sizeof(local);
            check(bind(listener.value, reinterpret_cast<sockaddr*>(&local), size) == 0 &&
                getsockname(listener.value, reinterpret_cast<sockaddr*>(&local), &size) == 0 &&
                listen(listener.value, 1) == 0, "peer ownership listener");
            NativeDnsSocket client;
            check(client.connectTo(ntohs(local.sin_port)), "peer ownership connection");
            Socket accepted{accept(listener.value, nullptr, nullptr)};
            check(nativeDnsPeerOwned(ntohs(local.sin_port), client.localPort(), GetCurrentProcessId()),
                "connected peer process identity");
            check(!nativeDnsPeerOwned(ntohs(local.sin_port), client.localPort(), GetCurrentProcessId() + 1),
                "wrong peer process must be rejected");
        }
        for (int mode = 0; mode < 3; ++mode) {
            Resolver resolver(mode);
            std::string error;
            const auto result = queryNativeDnsTool("pptp.example.test", "127.0.0.241", index, error, resolver.port);
            if (mode == 0) check(result == std::vector<std::string>{"192.0.2.99"}, error);
            else check(result.empty() && !error.empty(), "reject invalid DNS response");
            check(resolver.served, "DNS TCP fixture was not reached");
        }
        if (argc == 2 && std::string_view(argv[1]) == "--require-wfp") {
            Resolver resolver(0, 53);
            WfpFixture filter; filter.install();
            check(directBlocked(), "baseline DNS connection must be blocked");
            std::string error;
            const auto result = queryNativeDnsTool("pptp.example.test", "127.0.0.241", index, error);
            check(result == std::vector<std::string>{"192.0.2.99"}, error);
            check(directBlocked(), "native query must not remove the DNS block");
            std::println("strict DNS WFP regression passed");
        }
        std::println("native physical DNS regression passed");
        return 0;
    } catch (const std::exception& e) { std::println(stderr, "{}", e.what()); return 1; }
}
