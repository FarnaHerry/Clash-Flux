// core.cppm — clashflux.core：sing-box 内核进程生命周期（接口模块）。
//
// Windows/Linux 的数据面由 SCM/systemd 服务持有，CoreProcess::start 与
// spawnDetached 均拒绝在应用进程启动内核。macOS 保留 posix_spawn、日志管道和
// SIGTERM→SIGKILL 的进程后端；移动端由各自系统 VPN/Libbox runtime 托管。
//
// 运行时配置：generateConfig() 经 clashflux.singbox 编译器把订阅（Clash YAML、
// 原生 sing-box JSON 或空）与本应用托管的设置（clash_api / mixed 入站 / tun /
// 日志级别等）合成 sing-box JSON，写 coreWorkDir/config.json，再以
// `run -c <config> -D <workdir>` 启动。
export module clashflux.core;

import std;
import clashflux.singbox;

namespace core {

export enum class CoreState {
    Stopped,   // 未运行
    Starting,  // 已 spawn，等待 external-controller 就绪
    Running,   // 控制器已应答
    Failed,    // 启动失败 / 异常退出
};

export const char* stateName(CoreState s);

// 终止指定 pid 的内核进程（接管/detached 形态的停止路径：目标不在本进程
// 的 CoreProcess 句柄里）。POSIX SIGTERM；Windows OpenProcess+TerminateProcess。
// pid ≤0 或进程已不在时为 no-op。
export void killPid(long pid);

// pid 对应进程是否仍存活（接管判定的门禁：pidfile 命中但进程已死时不得
// 视为本应用驻留内核）。POSIX kill(pid,0)（EPERM 视为存活——进程存在但
// 属其他用户）；Windows OpenProcess 探测。pid ≤0 返回 false。
export bool pidAlive(long pid);

// pid 对应进程的镜像/可执行文件名（basename，小写不敏感无关）。
// Linux 读 /proc/<pid>/comm；Windows 查询进程镜像路径；macOS/iOS 无对应能力，
// 返回空串。用于"按 pidfile 终止之前先确认它还是不是我们期望的进程"——pid 会被
// 系统复用，裸 kill 可能误杀无关进程。pid ≤0 或读取失败返回空串。
export std::string pidImageName(long pid);

// 去掉内核输出里的 ANSI 颜色转义（sing-box 的 FATAL/INFO 前缀带 CSI 序列），
// 供诊断文本进 UI/CLI 前净化。
export std::string stripAnsi(std::string text);

// macOS 的兼容驻留后端：setsid + 日志文件 + core.pid。
// Windows/Linux 必须经服务启动，此函数只返回错误，不创建进程或写 pidfile。
export bool spawnDetached(const std::filesystem::path& binary,
                          const std::filesystem::path& workDir,
                          const std::filesystem::path& configFile,
                          std::string& error);

// ---- TUN 打开门禁 -----------------------------------------------------------
// Linux TUN 由 root 服务创建虚拟网卡；Windows TUN 由 SCM 服务管理。
export enum class TunGate {
    Ok,      // 服务可用，可直接 applyTun
    Denied,  // 服务不可用，UI 引导安装或更新
};

// 打开 TUN 前的门禁（阻塞：Linux 会连服务 socket 探测 root 服务托管；任务线程
// 调用）。Windows 检查 SCM 网络服务，权限只由服务提供，不提权 GUI；
// Linux 同样仅接受服务，包括 euid==0 的 GUI；macOS 保留原 root 门禁。
// Denied 时 UI 引导安装服务。
export TunGate tunGate();

// 用系统默认浏览器打开 URL（订阅卡「首页」跳转）。fork+exec（不走 shell，
// URL 无注入面）/ Windows ShellExecuteW。失败静默（best-effort）。
export void openInBrowser(const std::string& url);

// 合成 sing-box 运行时配置（含平台形态修正：Android 按 tunEnabled 生成
// tun inbound 与宽松路由，保留 IPv6 设置）。内核常驻与 TUN/系统代理互相独立。
// 返回的 CompileResult.json 可直接 `sing-box run -c`；失败 json 为空并填
// error；订阅降级细节（不支持的节点/规则）经 warnings 带回。
export singbox::CompileResult generateConfig(singbox::CompileOptions options);
// 桌面候选配置的固定内核检查；调用方任务线程，Windows 无控制台。
export bool checkConfig(const std::filesystem::path& binary,
                       const std::filesystem::path& workDir,
                       const std::filesystem::path& configFile, std::string& error);

export inline bool SupportsRuleSetTools() noexcept {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    return false;
#else
    return true;
#endif
}
// Import a bounded binary snapshot through the pinned native SRS decoder.
// Returns editable source JSON; never writes the selected user file.
export std::optional<std::string> importRuleSet(const std::filesystem::path& binary,
    const std::filesystem::path& workDir, const std::filesystem::path& source, std::string& error);

export class CoreProcess {
public:
    CoreProcess();
    ~CoreProcess();
    CoreProcess(const CoreProcess&) = delete;
    CoreProcess& operator=(const CoreProcess&) = delete;

    // 启动内核。binary 为空或 spawn 失败返回 false 并填 lastError()。
    bool start(const std::filesystem::path& binary,
               const std::filesystem::path& workDir,
               const std::filesystem::path& configFile);
    // 停止：POSIX SIGTERM → 2s 宽限 → SIGKILL；Windows TerminateProcess。
    void stop();
    bool running() const;
    // 子进程退出码（未退出/运行中为 -1；stop 后由监视线程填）。
    int exitCode() const;
    std::string lastError() const;
    // UI 线程泵：取走累计的内核 stdout/stderr 行（一次取空）。
    std::vector<std::string> drainOutput();
    // 非破坏性尾部快照：最近 ≤3 行内核输出（单行截断到 120 字符，
    // " / " 连接）。drainOutput 的消费者（日志页）不影响本缓冲，供启动
    // 失败/异常退出时把内核真实报错（如端口占用、GeoIP 拉取失败）带回
    // lastError，而不是只给一个 exit 码。
    std::string recentTail() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
