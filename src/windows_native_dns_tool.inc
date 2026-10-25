// Service-side, bounded use of the bundled official sing-box native direct outbound.
// Its executable identity is already exempted by sing-tun's strict DNS policy.
// No WFP filters, system DNS settings or shared resolver routes are changed.
struct NativeDnsFiles {
    std::filesystem::path directory;
    ~NativeDnsFiles() {
        if (directory.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(directory / "config.json", ignored);
        std::filesystem::remove(directory / "error.log", ignored);
        std::filesystem::remove(directory, ignored);
    }
};

struct NativeDnsProcess {
    clashflux::win32::UniqueHandle job, process, thread;
    ~NativeDnsProcess() {
        if (job.valid()) TerminateJobObject(job.get(), 1);
        if (process.valid()) WaitForSingleObject(process.get(), 2000);
    }
};

class NativeDnsSocket {
public:
    NativeDnsSocket() : value_(socket(AF_INET, SOCK_STREAM, 0)), deadline_(GetTickCount64() + 5000) {
        u_long nonblocking = 1;
        if (value_ != INVALID_SOCKET) ioctlsocket(value_, FIONBIO, &nonblocking);
    }
    ~NativeDnsSocket() { if (value_ != INVALID_SOCKET) closesocket(value_); }
    NativeDnsSocket(const NativeDnsSocket&) = delete;
    NativeDnsSocket& operator=(const NativeDnsSocket&) = delete;
    bool connectTo(unsigned short port) {
        if (value_ == INVALID_SOCKET) return false;
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        if (connect(value_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return true;
        if (WSAGetLastError() != WSAEWOULDBLOCK || !wait(true)) return false;
        int status = 0, size = sizeof(status);
        return getsockopt(value_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size) == 0 && !status;
    }
    bool transfer(unsigned char* data, int size, bool writing) {
        for (int done = 0; done < size;) {
            if (!wait(writing)) return false;
            const int count = writing ? send(value_, reinterpret_cast<const char*>(data + done), size - done, 0) :
                recv(value_, reinterpret_cast<char*>(data + done), size - done, 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (count <= 0) return false;
            done += count;
        }
        return true;
    }
    unsigned short localPort() const {
        sockaddr_in address{}; int size = sizeof(address);
        return getsockname(value_, reinterpret_cast<sockaddr*>(&address), &size) == 0 ? ntohs(address.sin_port) : 0;
    }
private:
    bool wait(bool writing) {
        const auto now = GetTickCount64();
        if (now >= deadline_) return false;
        const auto remaining = deadline_ - now;
        timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
        fd_set ready, errors; FD_ZERO(&ready); FD_ZERO(&errors); FD_SET(value_, &ready); FD_SET(value_, &errors);
        return select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout) > 0 &&
            FD_ISSET(value_, &ready);
    }
    SOCKET value_;
    ULONGLONG deadline_;
};

bool nativeDnsPeerOwned(unsigned short serverPort, unsigned short clientPort, DWORD processId) {
    ULONG size = 0;
    if (!clientPort || GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
            TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> storage(size);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto status = GetExtendedTcpTable(storage.data(), &size, FALSE, AF_INET,
            TCP_TABLE_OWNER_PID_ALL, 0);
        if (status == ERROR_INSUFFICIENT_BUFFER) { storage.resize(size); continue; }
        if (status != NO_ERROR) return false;
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& row = table->table[i];
            if (row.dwState == MIB_TCP_STATE_ESTAB && row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
                row.dwRemoteAddr == htonl(INADDR_LOOPBACK) &&
                ntohs(static_cast<unsigned short>(row.dwLocalPort)) == serverPort &&
                ntohs(static_cast<unsigned short>(row.dwRemotePort)) == clientPort && row.dwOwningPid == processId)
                return true;
        }
        return false;
    }
    return false;
}

std::vector<std::string> queryNativeDnsTool(std::string_view host,
        std::string_view server, ULONG index, std::string& error,
        unsigned short port = 53) {
    using clashflux::win32::UniqueHandle;
    error.clear();
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) { error = "初始化服务 DNS Socket 失败"; return {}; }
    struct Network { ~Network() { WSACleanup(); } } network;
    IN_ADDR serverAddress{};
    if (InetPtonA(AF_INET, std::string(server).c_str(), &serverAddress) != 1) {
        error = "物理 DNS 必须是 IPv4 地址"; return {};
    }
    NET_LUID luid{};
    WCHAR alias[IF_MAX_STRING_SIZE + 1]{};
    if (ConvertInterfaceIndexToLuid(index, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToAlias(&luid, alias, std::size(alias)) != NO_ERROR) {
        error = "物理 DNS 网卡已消失"; return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, alias,
        -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) { error = "物理 DNS 网卡名称无效"; return {}; }
    std::string interfaceName(bytes, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, alias, -1,
            interfaceName.data(), bytes, nullptr, nullptr)) {
        error = "物理 DNS 网卡名称转换失败"; return {};
    }
    interfaceName.pop_back();

    // WinDNS requires a buffer larger than 1472 bytes even for a short query.
    std::array<unsigned char, 4096> question{};
    DWORD questionSize = static_cast<DWORD>(question.size());
    if (!DnsWriteQuestionToBuffer_UTF8(reinterpret_cast<DNS_MESSAGE_BUFFER*>(question.data()),
            &questionSize, std::string(host).c_str(), DNS_TYPE_A,
            static_cast<WORD>(std::random_device{}()), TRUE)) {
        error = "无法构建 PPTP 服务器 DNS 查询"; return {};
    }
    std::wstring image(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!size || size == image.size()) { error = "无法定位服务内核目录"; return {}; }
    image.resize(size);
    const auto root = std::filesystem::path(image).parent_path();
    const auto executable = root / "engines" / "sing-box.exe";
    NativeDnsFiles files;
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto directory = root / ("dns-query-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code ec;
        if (std::filesystem::create_directory(directory, ec)) {
            files.directory = std::move(directory); break;
        }
        if (ec && ec != std::errc::file_exists) break;
    }
    if (files.directory.empty()) { error = "无法创建服务 DNS 查询目录"; return {}; }
    const auto configPath = files.directory / "config.json";
    unsigned short localPort = 0;
    {
        const SOCKET reservation = socket(AF_INET, SOCK_STREAM, 0);
        if (reservation == INVALID_SOCKET) { error = "无法预留 DNS 控制端口"; return {}; }
        struct SocketOwner { SOCKET value; ~SocketOwner() { closesocket(value); } } owner{reservation};
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(local);
        if (bind(reservation, reinterpret_cast<sockaddr*>(&local), length) != 0 ||
            getsockname(reservation, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
            error = "无法预留 DNS 控制端口"; return {};
        }
        localPort = ntohs(local.sin_port);
    }
    // Random client credentials restrict use of this short-lived endpoint.
    // Before sending them, verify the connected peer belongs to our child:
    // SOCKS password authentication alone cannot authenticate the server.
    const std::string password = std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
    const nlohmann::json config = {
        {"log", {{"disabled", true}}},
        {"inbounds", nlohmann::json::array({{{"type", "socks"}, {"listen", "127.0.0.1"},
            {"listen_port", localPort}, {"users", nlohmann::json::array({{{"username", "dns"}, {"password", password}}})}}})},
        {"outbounds", nlohmann::json::array({{{"type", "direct"}, {"tag", "physical-dns"},
            {"bind_interface", interfaceName}}})},
        {"route", {{"final", "physical-dns"}}}};
    {
        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << config.dump(); output.close();
        if (!output) { error = "无法写入服务 DNS 查询配置"; return {}; }
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    UniqueHandle nullInput(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    UniqueHandle nullOutput(CreateFileW((files.directory / "error.log").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput.valid() || !nullOutput.valid()) { error = "无法准备服务 DNS 诊断输出"; return {}; }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        error = "初始化 DNS 句柄继承列表失败"; return {};
    }
    struct AttributeOwner {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeOwner() { DeleteProcThreadAttributeList(value); }
    } attributeOwner{attributes};
    HANDLE inherited[]{nullInput.get(), nullOutput.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr)) {
        error = "设置 DNS 句柄继承列表失败"; return {};
    }
    NativeDnsProcess child;
    child.job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!child.job.valid() || !SetInformationJobObject(child.job.get(),
            JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "创建服务 DNS 查询作业失败"; return {};
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.get();
    startup.StartupInfo.hStdOutput = nullOutput.get();
    startup.StartupInfo.hStdError = nullOutput.get();
    startup.lpAttributeList = attributes;
    // The private configuration has no TUN, API, cache, profiles or services.
    std::wstring command = L"\"" + executable.wstring() + L"\" run -c \"" + configPath.wstring() + L"\"";
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, root.c_str(), &startup.StartupInfo, &process)) {
        error = winError("启动原生 DNS 连接工具失败", {}, GetLastError()); return {};
    }
    child.process.reset(process.hProcess); child.thread.reset(process.hThread);
    if (!AssignProcessToJobObject(child.job.get(), child.process.get())) {
        // This process is still suspended and has never executed user code.
        TerminateProcess(child.process.get(), 1);
        error = "无法约束原生 DNS 连接工具生命周期"; return {};
    }
    if (ResumeThread(child.thread.get()) == static_cast<DWORD>(-1)) {
        error = "无法启动原生 DNS 连接工具线程"; return {};
    }
    std::unique_ptr<NativeDnsSocket> connection;
    const auto deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline && WaitForSingleObject(child.process.get(), 0) == WAIT_TIMEOUT) {
        auto attempt = std::make_unique<NativeDnsSocket>();
        if (attempt->connectTo(localPort) && nativeDnsPeerOwned(localPort, attempt->localPort(),
                GetProcessId(child.process.get()))) { connection = std::move(attempt); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!connection) {
        error = "原生 DNS 出口启动失败或超时";
        std::ifstream diagnostic(files.directory / "error.log", std::ios::binary);
        std::array<char, 2048> text{}; diagnostic.read(text.data(), text.size());
        if (diagnostic.gcount()) error += ": " + std::string(text.data(), diagnostic.gcount());
        return {};
    }
    auto transfer = [&](unsigned char* bytes, int count, bool writing) {
        return connection->transfer(bytes, count, writing);
    };
    unsigned char hello[]{5, 1, 2}, reply[2]{};
    if (!transfer(hello, 3, true) || !transfer(reply, 2, false) || reply[0] != 5 || reply[1] != 2) {
        error = "原生 DNS 出口认证协商失败"; return {};
    }
    std::vector<unsigned char> authentication{1, 3, 'd', 'n', 's', static_cast<unsigned char>(password.size())};
    authentication.insert(authentication.end(), password.begin(), password.end());
    if (!transfer(authentication.data(), static_cast<int>(authentication.size()), true) ||
        !transfer(reply, 2, false) || reply[0] != 1 || reply[1] != 0) {
        error = "原生 DNS 出口认证失败"; return {};
    }
    unsigned char request[]{5, 1, 0, 1, 0, 0, 0, 0,
        static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port)};
    std::memcpy(request + 4, &serverAddress.s_addr, 4);
    unsigned char connected[4]{};
    if (!transfer(request, sizeof(request), true) || !transfer(connected, 4, false) ||
        connected[0] != 5 || connected[1] != 0 || connected[2] != 0) {
        error = "原生 DNS 物理出口连接失败"; return {};
    }
    int remaining = connected[3] == 1 ? 6 : connected[3] == 4 ? 18 : -1;
    std::array<unsigned char, 258> ignored{};
    if (connected[3] == 3) {
        if (!transfer(ignored.data(), 1, false)) { error = "原生 DNS 出口应答残缺"; return {}; }
        remaining = ignored[0] + 2;
    }
    if (remaining < 0 || !transfer(ignored.data(), remaining, false)) {
        error = "原生 DNS 出口应答无效"; return {};
    }
    std::vector<unsigned char> framed{static_cast<unsigned char>(questionSize >> 8),
        static_cast<unsigned char>(questionSize)};
    framed.insert(framed.end(), question.begin(), question.begin() + questionSize);
    unsigned char length[2]{};
    if (!transfer(framed.data(), static_cast<int>(framed.size()), true) || !transfer(length, 2, false)) {
        error = "原生 DNS 查询失败或超时"; return {};
    }
    std::vector<unsigned char> response((length[0] << 8) | length[1]);
    if (response.size() < 12 || !transfer(response.data(), static_cast<int>(response.size()), false)) {
        error = "原生 DNS 应答残缺或查询超时"; return {};
    }
    return decodePhysicalDns(host, std::span(question).first(questionSize), response, error);
}
