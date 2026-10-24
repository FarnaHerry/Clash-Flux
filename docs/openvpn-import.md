# OpenVPN 普通来源导入

远程订阅和本地文件表示内容来源（扫码得到的文本同样进入内容识别）；Clash YAML、
sing-box JSON、OpenVPN `.ovpn` 表示内容格式。`singbox::DetectProfileFormat`
按内容识别，支持 UTF-8 BOM、CRLF 和 OpenVPN 注释，文件扩展名及 URL 后缀不决定
编译路径；配置文本粘贴入口已并入本地文件导入。
新建界面不再有独立 OpenVPN 类型；桌面弹窗和窄屏页面复用同一创建表单，手机
的 URL、文件入口与 PPTP 表单也使用普通导入服务。

## 运行角色

- 主 `.ovpn` 来源输出一个 `openvpn-client` 用户态 endpoint，作为默认出口。
- 桌面次 `.ovpn` 来源只在启用的编排规则引用时参与，默认出口及单节点对象
  `OpenVPN` 使用来源 ID 隔离的 tag；两个同名来源可以共存，不接管主来源默认出口。
- 普通次来源的启用由规则决定，不增加单独的 OpenVPN 拨号线程或系统接口。
- 手机仍只允许一个活动普通订阅，不因 `.ovpn` 格式开放普通多来源编排。
- PPTP 仍由平台拨号服务持有，保留独立配置入口。

下载、刷新、文件编辑、候选配置检查和保真度报告沿用普通订阅路径。
源文件保存原始 `.ovpn` 文本；内部文件名可以仍是 `<id>.yaml`，读取按内容判断。
刷新/编辑候选失败时保留旧文件及数据库引用；桌面候选仍经过固定打包内核检查。
默认下载 TLS 证书校验保持，只有显式订阅选项允许跳过。

## 转换范围

支持已有的多 remote、remote-random、静态密钥、TLS inline ca/cert/key、
peer-fingerprint、TLS 控制密钥及 inline auth-user-pass。凭据按两行原文读取，
保留空格、制表符和引号；认证不会变成交互输入，也不读取外部文件。

普通导入明确拒绝外部文件、脚本、未知指令和未转换的 route、redirect-gateway、
dhcp-option 等路由/DNS 指令，并保留来源级 unsupported 账本。内网分流通过应用
规则编辑器明确配置。CLI 的 client/dev tun/nobind/persist、日志与重试参数由
sing-box 用户态 endpoint 生命周期管理，差异进入 approx 账本。
原生 sing-box JSON 的 OpenVPN endpoint 继续保留原字段，不套用 .ovpn 指令白名单。

## 移除旧独立连接

OpenVPN 只通过普通 remote/local 来源导入、编辑、启用和参与规则，不再保留旧
`type=openvpn` 的独立会话、勾选、批量开关、专用编辑器或状态快照。
独立系统拨号和内网路由表单只保留 PPTP；`importNative` 仅接受 PPTP。
用户已确认没有需要保留的旧独立 OpenVPN 配置，因此不新增旧记录迁移，
也不扫描、删除或重写现有数据库与订阅文件。SQLite schema 不变。
