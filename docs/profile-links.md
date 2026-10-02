# 订阅唤醒链接

网页可使用以下链接打开 Clash-Flux 的订阅导入表单：

```text
sing-box://import-remote-profile?url=https%3A%2F%2Fexample.com%2Fsubscription#My%20Profile
flclash://install-config?url=https%3A%2F%2Fexample.com%2Fsubscription&name=My%20Profile
clash-flux://install-config?url=https%3A%2F%2Fexample.com%2Fsubscription
```

sing-box 格式遵循[官方客户端文档](https://sing-box.sagernet.org/clients/general/)。
FlClash 的 `install-config?url=...` 与 `clash` / `clashmeta` 别名对应其
[链接处理](https://github.com/chen08209/FlClash/blob/main/lib/common/link.dart)和
[协议定义](https://github.com/chen08209/FlClash/blob/main/lib/common/protocol.dart)。
Clash-Flux 也读取可选 `name`；非空 fragment 优先作为名称，fragment 中 `+` 保持原样。
URL 必须整体进行一次百分号编码，特别是内层 URL 的 `&`、`#` 与已有 `%`。
解码后的 URL 保留原始路径、参数和 token，不二次解码、不改写订阅格式。

收到链接后只打开独立表单，用户可检查名称、URL、下载设置后导入；不会自动启用订阅，
不会改变当前主订阅。下载与候选配置检查使用现有导入流程，保持系统证书校验。
普通 URL 导入及扫码入口也能解析这些包装链接。无效编码、控制字符、重复 url/name、
非 HTTP(S) 地址、不支持的操作与过长链接会被拒绝，错误消息不回显 token。

## 平台接入

- Linux：安装包的 desktop entry 使用 `%u` 和五个 `x-scheme-handler`。桌面环境
  应刷新其应用数据库；有多个兼容客户端时，在系统“打开方式”中选择 Clash-Flux。
- Windows：GUI 启动时经 HuxerUI 公共 API 注册协议；已有其它应用拥有协议时保留
  其注册，使用专用 `clash-flux` 链接或调整系统默认应用。无需后台 shell 命令。
- macOS：app bundle 的 Info.plist 声明这五个 URL scheme，系统 activation 经
  HuxerUI 交付给应用。
- Android：MainActivity 的 VIEW/BROWSABLE 过滤器接收链接；已有兼容客户端时由
  Android 选择处理应用。导入页覆盖底部导航，返回回到订阅页，仍保持单活动代理订阅。
- iOS 继续按项目 TODO 暂缓，不承诺此功能。

桌面命令行将唤醒链接识别为 GUI 请求；已有实例时使用现有 CLI IPC 转发，保持单个
Runtime、数据库缓存与托盘。尚在启动的 owner 不会删除刚转发的请求；IPC 请求文件
原子发布，避免轮询读取半写文件。待处理队列有上限，同一冷启动的重复链接合并。
事件通知与兜底轮询串行服务请求，避免重复导入与进程 stdout 重定向互相干扰。
关闭表单取消其任务域，避免旧下载完成后关闭新打开的页面。

## 本地验证

```bash
cmake --build build --target clash-flux test_profile_links test_singbox
./run.sh --version
ctest --test-dir build -R '^(profile_links|singbox)$' --output-on-failure
./run.sh 'sing-box://import-remote-profile?url=https%3A%2F%2Fexample.com%2Fsubscription#My%20Profile'
./run.sh 'flclash://install-config?url=https%3A%2F%2Fexample.com%2Fsubscription&name=Second'
```

两个链接依次在同一窗口预填导入表单，第二个等待第一个完成或返回后显示。
Android 构建完成后可用 `adb shell am start -W -a android.intent.action.VIEW -d '链接'`
检查 intent 路由；实际浏览器选择与系统默认应用设置仍需在对应平台验证。
