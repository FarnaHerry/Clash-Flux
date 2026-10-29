# VMess 节点订阅

远程订阅响应和本地文件按内容识别，支持逐行 `vmess://<Base64 JSON>`，以及整份列表再包一层
Base64 的形式。不是通过 URL 后缀判断格式；IP、自定义端口、路径和查询参数按原订阅 URL 使用。
普通订阅地址输入仍用于 HTTP(S) 下载；本功能不新增扫码或直接粘贴任意节点 URI 的入口。

## 输入与边界

- 接受标准及 URL-safe Base64，有/无 padding、ASCII 空白、BOM、CRLF；拒绝非法字符、padding
  和非零尾位。原文与解码列表上限 8 MiB，最多 4096 节点，每个 VMess JSON 上限 64 KiB。
- 当前只转换 VMess JSON 分享格式，版本缺省/显式 2；其它节点协议、VMess URI authority 格式、
  旧版 JSON 1、未知字段或重复 JSON 字段明确拒绝，不只导入列表中的合法部分。
- 保留节点名称/顺序、服务器、端口、UUID、alterId、cipher（缺省 auto）；数字字段允许整数或
  十进制整数字符串。名称缺省生成 VMess 序号，重复名称及内置出口重名拒绝，避免节点消失或引用改向。
- 支持 TCP、WebSocket、HTTP/2（h2/http）、gRPC 的现有映射；保留 WS Host/path、HTTP host/path、
  gRPC serviceName（path）。gRPC authority、KCP/QUIC、TCP HTTP 伪装及其它模式不能等价表达时拒绝。
- TLS 必须显式 `tls=tls`；默认验证证书。仅显式 insecure/allowInsecure 才跳过验证，两者冲突拒绝。
  保留显式 SNI、ALPN 和指纹；无显式 SNI 时用首个 transport host，否则由内核按服务器处理。
  非空 vcn/pcs 证书约束尚未转换，拒绝候选，不忽略约束。
- 部分服务商使用非标准 `net=tls`；仅同时显式 `tls=tls`、无伪装且路径为空或 `/` 时，按 TCP+TLS
  兼容并记节点 approx。其它未知传输不降为 TCP。

## 编译、数据和规则

`src/singbox_node_subscription.inc` 在共享编译器内构造临时 Clash 文档，再复用现有 VMess 字段、
传输和 TLS 转换。下载、订阅文件和数据库保持原内容，不落盘重写成生成的 YAML。
导入、更新、规则投影、激活前对象目录、预览及桌面次来源共用这条转换，手机保持单普通来源。

节点列表没有原始路由规则，因此生成包含全部节点的手动 selector 和 MATCH 默认出口；组名避开
已有节点名称，不自动增加 DIRECT 成员。这项默认策略记字段 approx；用户可使用个人/全局规则分流。
标准节点参数和包装解码按现有协议映射记账，不把新增输入格式当成新增内核能力。

任何坏条目、未知字段、转换失败或后续内核检查失败，均拒绝整份候选；仍沿用独占暂存、候选检查、
原子替换及旧订阅/引用/活动配置保护。新解析诊断仅含行号、字段或原因，不输出原始 URI、UUID 或 JSON。

格式依据：[VMess 分享说明](https://github.com/2dust/v2rayN/wiki/Description-of-VMess-share-link)、
[订阅说明](https://github.com/2dust/v2rayN/wiki/Description-of-subscription)。

## 验证

`test_singbox` 覆盖包装变体、字段/传输/TLS 保留、整份拒绝、大小/数量上限、规则投影和对象目录。
服务商样本只在独占临时文件中核对，不提交订阅地址、节点凭据或生成配置；编译与内核 check
不等于节点连通性、VPN 或真机测试。实际证据见 [验收记录](l1-l2-l3-status.md)。
