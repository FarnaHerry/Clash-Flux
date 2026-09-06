// core.cppm — clashflux.core：mihomo 内核进程生命周期（接口模块）。
//
// 进程模型（对齐 apitab k6_engine）：start() 由 store 调用，POSIX（Linux/macOS）
// posix_spawn / Windows CreateProcessW 拉起 mihomo 子进程（stdout+stderr 合并进
// 一根管道），监视线程按 \r / \n 拆行入队（内核自身的启动日志，日志页在 WS
// 断线时也能看到这些）；stop() POSIX 先 SIGTERM，2s 宽限后 SIGKILL；Windows 无
// SIGTERM 语义，直接 TerminateProcess。
//
// 运行时配置：generateConfig() 把订阅 YAML（或空）与本应用托管的注入块
// （external-controller / secret / mixed-port 等）合成 coreWorkDir/config.yaml，
// 再以 -d <workdir> -f <config> 启动。注入块优先级最高：先文本级剔除订阅里
// 的同名顶层键，再把注入块放到文件头部。
export module clashflux.core;

import std;

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

// 以脱离会话方式启动内核（CLI `core start` 用：CLI 退出后内核驻留）：
// POSIX setsid + stdout/stderr 追加到 <workDir>/mihomo.log；Windows
// DETACHED_PROCESS + 同样重定向日志。成功后 pid 写 <workDir>/mihomo.pid
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
// Linux 看自身 euid==0 或服务模式可用（root 服务托管的内核能建 TUN）；
// macOS 看自身 euid==0。
export TunGate tunGate();

// 合成运行时配置文本。
//   profileYaml  订阅（或手写）配置原文；可为空（生成最小可用配置）。
//   controller   "127.0.0.1:9097"
//   secret       external-controller 鉴权
//   mixedPort    混合入站端口（7899）
//   mode         rule / global / direct
//   tunEnabled   TUN 透明代理（注入 tun: 块；需 root/CAP_NET_ADMIN）
// 返回可直接喂给 mihomo -f 的完整 YAML 文本。
export std::string generateConfig(const std::string& profileYaml,
                                  const std::string& controller,
                                  const std::string& secret,
                                  int mixedPort,
                                  const std::string& mode,
                                  bool allowLan,
                                  const std::string& logLevel,
                                  bool tunEnabled);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
