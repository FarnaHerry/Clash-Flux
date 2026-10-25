# 首页状态、猫咪动画与隔离 Demo

生产组件 src/ui/box_cat.cpp 只映射真实内核/VPN 状态；移动端与桌面 Compact 共用 home_header_control.cpp 的固定 48pt 标题槽位。
未运行显示“首页”，真实成功且出箱完成后飞入并展开“运行中 + HH:MM:SS”；不能用模拟进度驱动生产动画。
详细运动、稳定姿势、后台/reduced motion 与生产组件回归见 [UI 文档](../../../docs/ui-development.md#页内标签与导航图标)。

桌面时长从内核确认就绪的 runningSince 起计；Android 从后台 VpnUptime 的 VPN 会话计，不以 core-only 或 UI 挂载为起点。
状态/时长同一 Binder 快照传递；UI 不逐秒累加或跨 Java/C++ 比较时间基准。锁屏、切页、重建、重复 connected 不重置；真正重连才新会话，Binder 暂不可用保留末次快照。
后台契约与 Java 回归见 [Android 生命周期](../../../docs/android-background-lifecycle.md#首页-vpn-运行时长)。

隔离 Demo 仅模拟状态，不接内核、代理、数据库或订阅，无需启动主程序 GUI：

```bash
./demos/box-cat-demo/run-demo.sh
```

```powershell
./demos/box-cat-demo/run-demo.ps1
./demos/box-cat-demo/run-demo.ps1 -BuildOnly
```

Windows 不依赖 WSL，产物 demos/box-cat-demo/build-windows/box_cat_demo.exe；品牌挂载使用 --brand-preview。
Demo 构建不能代替主目标构建与 version 验证。时长回归用生产标题/悬浮组件及 tests/android/VpnUptimeTest.java，不能仅测模拟画面。
