// config.cppm — clashflux.config：用户数据目录、mihomo 内核二进制与控制器端点解析。
// 无 UI 依赖，core / api / store / UI 共用。
//
// mihomo 不在本仓库：CI 打包时下载对应平台二进制放进包内 engines/，运行时按
//   1. <exe 目录>/engines/mihomo(.exe)   —— 打包分发形态
//   2. <exe 目录>/mihomo(.exe)
//   3. <repo>/engines/mihomo             —— 开发形态（exe 在 <repo>/build/）
//   4. PATH 里的 mihomo                  —— 系统已装
// 顺序解析。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>  // access, X_OK
#else
#include <unistd.h>  // readlink, access, X_OK
#include <limits.h>  // PATH_MAX
#endif
#include <cstdio>  // popen / pclose（跟随系统的深色检测）

export module clashflux.config;

import std;

namespace cfg {

// 可执行文件目录（engines/ 相对解析 / 资源回退用）。
export std::filesystem::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::weakly_canonical(buffer).parent_path();
#else
    char buf[PATH_MAX]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n))).parent_path();
#endif
}

// 用户数据目录：Linux $XDG_DATA_HOME/clash-flux（~/.local/share/clash-flux）/
// Windows %APPDATA%\clash-flux / macOS ~/Library/Application Support/clash-flux。
// SQLite 库、mihomo 工作目录、订阅 YAML 都放这里 —— 安装版启动时 cwd 可能
// 不可写，不能依赖 cwd。
export std::filesystem::path dataDir() {
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        return std::filesystem::path(a) / "clash-flux";
    }
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / "Library" / "Application Support" / "clash-flux";
    }
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path(x) / "clash-flux";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / ".local" / "share" / "clash-flux";
    }
#endif
    return std::filesystem::current_path();
}

// SQLite 数据库文件路径（确保父目录存在）。
export std::filesystem::path databaseFile() {
    const std::filesystem::path dir = dataDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "clash-flux.db";
}

// 订阅 YAML 存放目录（profiles/<id>.yaml）。
export std::filesystem::path profilesDir() {
    const std::filesystem::path dir = dataDir() / "profiles";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// mihomo 工作目录（-d 参数）：运行时生成的 config.yaml、Country.mmdb、
// cache.db、ui/ 等都落这里，与用户数据隔离在一处便于清理。
export std::filesystem::path coreWorkDir() {
    const std::filesystem::path dir = dataDir() / "core";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool executableExists(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

// PATH 查找（which 语义）。
std::filesystem::path findInPath(std::string_view name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::string_view rest{pathEnv};
    while (!rest.empty()) {
        const auto pos = rest.find(sep);
        const std::string_view dir = rest.substr(0, pos);
        rest = (pos == std::string_view::npos) ? std::string_view{} : rest.substr(pos + 1);
        if (dir.empty()) continue;
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (executableExists(candidate)) return candidate;
    }
    return {};
}

// mihomo 二进制解析：exe 旁 engines/ → exe 旁 → <repo>/engines（开发形态）→ PATH。
// 找不到返回空路径。
export std::filesystem::path mihomoBinary() {
#ifdef _WIN32
    constexpr std::string_view exeName = "mihomo.exe";
#else
    constexpr std::string_view exeName = "mihomo";
#endif
    const std::filesystem::path exeDir = executableDir();
    if (!exeDir.empty()) {
        if (const auto p = exeDir / "engines" / exeName; executableExists(p)) return p;
        if (const auto p = exeDir / exeName; executableExists(p)) return p;
        // 开发形态：exe 在 <repo>/build/，仓库根的 engines/ 是其上一层。
        if (const auto p = exeDir / ".." / "engines" / exeName; executableExists(p)) {
            return std::filesystem::weakly_canonical(p);
        }
    }
    return findInPath(exeName);
}

// ---- external-controller 端点（本地回环，仅本进程与内核通信）----------------
export constexpr std::string_view kControllerHost = "127.0.0.1";
export constexpr int kControllerPort = 9097;  // 避开常见的 9090 占用

export std::string controllerAddress() {
    return std::format("{}:{}", kControllerHost, kControllerPort);
}

// REST API base URL（http://127.0.0.1:9097）。
export std::string controllerBaseUrl() {
    return std::format("http://{}", controllerAddress());
}

// WebSocket base URL（ws://127.0.0.1:9097）。
export std::string controllerWsUrl() {
    return std::format("ws://{}", controllerAddress());
}

// 生成随机 secret（external-controller Bearer 鉴权；首次启动生成后存 settings）。
export std::string randomSecret() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(hex[gen() & 0xF]);
    return out;
}

// 系统是否偏好深色（"跟随系统"主题模式用）。启动时读取一次即可。
export bool systemPrefersDark() {
#if defined(_WIN32)
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 1, size = sizeof(value);
    const LONG rc = RegQueryValueExA(key, "AppsUseLightTheme", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && value == 0;
#elif defined(__APPLE__)
    FILE* pipe = ::popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
    if (pipe == nullptr) return false;
    std::array<char, 32> buf{};
    const bool dark = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr &&
                      std::string_view(buf.data()).starts_with("Dark");
    ::pclose(pipe);
    return dark;
#else
    FILE* pipe = ::popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (pipe == nullptr) return false;
    std::array<char, 64> buf{};
    const bool dark = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr &&
                      std::string_view(buf.data()).find("prefer-dark") != std::string_view::npos;
    ::pclose(pipe);
    return dark;
#endif
}

} // namespace cfg
