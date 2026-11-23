# 网站访问测试

桌面宽屏在日志之后、设置之前提供「网站测试」侧栏入口；桌面窄窗和 Android
在设置顶部「更多」进入。Compact 返回设置，二级页面覆盖底部导航。
两处入口共用 `website_test.svg`，用圆角窗口轮廓与一条简短波形表示网站测速。

## 固定站点与卡片

- 固定提供 Baidu、Bilibili、QQ、JD、Google、YouTube、GitHub、Cloudflare。每个站点在
  `src/ui/website_test_model.h` 中指定名称、测试地址和打包图标。宽屏为自适应网格，Compact 单列。
  卡片显示网站图标与名称，页头提供全部测试，卡片提供单站测试。
- 使用站点提供的 favicon，PNG 保存在 `resources/images/website_*.png`；GitHub 复用项目已有的
  Octicons 图标，随深浅主题适配。来源为
  [Baidu](https://www.baidu.com/favicon.ico)、[Bilibili](https://www.bilibili.com/favicon.ico)、
  [QQ](https://www.qq.com/favicon.ico)、[JD](https://www.jd.com/favicon.ico)、
  [Google](https://www.google.com/favicon.ico)、[YouTube](https://www.youtube.com/s/desktop/95c3254f/img/favicon_96x96.png)、
  [Cloudflare](https://www.cloudflare.com/favicon.ico) 和 [GitHub Octicons](https://github.com/primer/octicons/blob/main/icons/mark-github-16.svg)。
- 左下角显示命中的代理节点或「直连」。收到响应但缺少出站证据时显示「出站未确认」，长节点名可悬停查看全文。
- 右下角固定为测速槽。待测时显示 14pt 空心对称闪电，成功后显示毫秒数字，失败时显示「超时」或「失败」。
  实际按钮在槽内右对齐，悬停底色、提示和点击范围围绕图标或文字。
  点击闪电、延迟或失败结果可重测。排队与运行期间使用代理卡片共用的圆点，当前卡暂停点击，其它空闲卡可以测试。
  页头「全部测试」使用实心小闪电，任意卡忙碌时禁用。
- 卡片保留主题底色与圆角，不描边。网格左右缩进与滚动正文首尾空白取
  `IslandTheme::nested_radius`，卡间距 8pt；ScrollArea 铺满正文。
  网格控制器随挂载，宽窄切换或卸载重挂恢复实际滚动偏移。
- 测试结果保留在当前 Runtime，跨切页和视口变化可用。

## 请求、延迟与实际出站

- 网站测试使用应用共享的 HuxerUI HttpClient，遵循系统当前代理、路由、VPN/TUN 及证书策略。
  网络请求只对目标发送 HTTP HEAD，随平台处理重定向。延迟以实际发送到收到最终响应头的单调时钟耗时计量，
  包含建立连接与重定向，排除排队、读取内核出站及读取响应体的时间。2xx/3xx 成功，4xx/5xx 及传输错误显示失败。
- 仅显式单站或批量测试发请求，最多三个并发。实际执行后共享 10 秒预算，模型超时兜底释放并发槽，
  按稳定站点 ID 和请求代次拒绝已结束请求的回调。切页和宽窄切换保留应用级任务。
- 桌面内核开放代理监听时，读取请求前的连接 ID 与运行实例、计划和端口，
  在 HEAD 响应流存活时从 `/connections` 查最终目标主机的叶子出站，必要时在流结束后再读一次。
  新连接优先，旧连接仅在出站一致时作为复用证据，冲突或运行身份变化时保留未确认。
  节点标签通过当前运行目录映射为名称，多来源沿用代理页的来源后缀；`DIRECT` 显示「直连」。
- Windows 在本应用无活动代理监听、且未发现系统代理、PAC/WPAD、代理环境变量或接管隧道时可确认直连。
  外部代理、无法关联的连接及未开放连接表的平台保留未确认。Android 网站测试仍反映应用自身网络，
  包括应用级 VPN 排除，不能用于判断其它应用的 VPN 接管情况。
- 测速只发送目标网站请求并读取当前运行环境的出站证据。

## 验证

`test_page_transition --website-tests` 挂载生产卡片，覆盖 320/960pt、站点图标、
右下角闪电与重测、出站标签和延迟、失败、队列上限、卸载/取消、超时后重试、滚动与宽窄重挂。
网络夹具核对 HEAD、响应头耗时传播、最终主机匹配、出站冲突、确认直连及未确认时保留延迟。
`ui_resources` 检查中英文资源。实际命令与覆盖范围见[验收记录](l1-l2-l3-status.md)。
