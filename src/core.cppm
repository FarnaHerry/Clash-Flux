// core.cppm — clashflux.core：mihomo 内核进程生命周期（接口模块）。
//
// 进程模型（对齐 apitab k6_engine）：start() 由 store 调用，posix_spawn /
// CreateProcess 拉起 mihomo 子进程（stdout+stderr 合并进一根管道），监视线程
// 按 \r / \n 拆行入队（内核自身的启动日志，日志页在 WS 断线时也能看到这些）；
// stop() POSIX 先 SIGTERM，2s 宽限后 SIGKILL。
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

// 合成运行时配置文本。
//   profileYaml  订阅（或手写）配置原文；可为空（生成最小可用配置）。
//   controller   "127.0.0.1:9097"
//   secret       external-controller 鉴权
//   mixedPort    混合入站端口（7890）
//   mode         rule / global / direct
// 返回可直接喂给 mihomo -f 的完整 YAML 文本。
export std::string generateConfig(const std::string& profileYaml,
                                  const std::string& controller,
                                  const std::string& secret,
                                  int mixedPort,
                                  const std::string& mode,
                                  bool allowLan,
                                  const std::string& logLevel);

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
