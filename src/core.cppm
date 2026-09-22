// core.cppm — clashflux.core：sing-box 内核进程生命周期（接口模块）。
//
// 进程模型（对齐 apitab k6_engine）：start() 由 store 调用，POSIX（Linux/macOS）
// posix_spawn / Windows CreateProcessW 拉起 sing-box 子进程（stdout+stderr 合并进
// 一根管道），监视线程按 \r / \n 拆行入队（内核自身的启动日志，日志页在 WS
// 断线时也能看到这些）；stop() POSIX 先 SIGTERM，2s 宽限后 SIGKILL；Windows 无
// SIGTERM 语义，直接 TerminateProcess。
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

// 去掉内核输出里的 ANSI 颜色转义（sing-box 的 FATAL/INFO 前缀带 CSI 序列），
// 供诊断文本进 UI/CLI 前净化。
export std::string stripAnsi(std::string text);

// 以脱离会话方式启动内核（CLI `core start` 用：CLI 退出后内核驻留）：
// POSIX setsid + stdout/stderr 追加到 <workDir>/core.log；Windows
// DETACHED_PROCESS + 同样重定向日志。成功后 pid 写 <workDir>/core.pid
// （接管/停止路径的活判与 killPid 依据）。失败返回 false 并填 error。
export bool spawnDetached(const std::filesystem::path& binary,
                          const std::filesystem::path& workDir,
                          const std::filesystem::path& configFile,
                          std::string& error);

// ---- TUN 打开门禁 -----------------------------------------------------------
// TUN 需要内核侧持有 root/CAP_NET_ADMIN（Linux 建虚拟网卡）或 Windows 管理员。
export enum class TunGate {
    Ok,        // 权限足够，可直接 applyTun
    Elevated,  // 仅 Windows：已拉起 UAC 提权重启自身，本次放弃（新实例里操作）
    Denied,    // 权限不足且无自动提权路径（UI 弹终端指令引导框）
};

// 打开 TUN 前的门禁（阻塞：Linux 会连服务 socket 探测 root 服务托管；任务线程
// 调用）。判定：Windows 看 TokenElevation（不足时顺带尝试提权重启自身）；
// Linux 的设计取向是应用自身保持非 root（更安全），root 只在服务侧——服务模式
// 可用即 Ok；euid==0 仅作兜底事实判断（已经 root 的环境 TUN 本就能建）；
// macOS 同理。Denied 时 UI 引导安装服务。
export TunGate tunGate();

// 用系统默认浏览器打开 URL（订阅卡「首页」跳转）。fork+exec（不走 shell，
// URL 无注入面）/ Windows ShellExecuteW。失败静默（best-effort）。
export void openInBrowser(const std::string& url);

// 合成 sing-box 运行时配置（含平台形态修正：Android 按 tunEnabled 生成
// tun inbound、仅 IPv4、宽松路由）。内核常驻与 TUN/系统代理互相独立。
// 返回的 CompileResult.json 可直接 `sing-box run -c`；失败 json 为空并填
// error；订阅降级细节（不支持的节点/规则）经 warnings 带回。
export singbox::CompileResult generateConfig(singbox::CompileOptions options);

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
