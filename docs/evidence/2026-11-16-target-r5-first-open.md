# R5 后续测量 · 应用重启后的首次打开

2026-11-16，`6b00957` 加 R3–R5 工作区，应用 v0.3.111。Fedora 44 / WSL2 / Intel Core i5-13500H，Clang 21 / libc++，CMake 4.4.2，Linux Release 构建，HuxerUI `89359f43`。使用隔离 Xvfb 和软件 OpenGL，应用数据位于 WSL ext4。运行器 Git 对象为 `4e5c0af42d12445be3a19b5a6950581337128bdd`。

每档先由生产 `profile import` 从临时环回 HTTP 端点导入一份合成 Clash 订阅。订阅有节点、组、内联 provider、多种域名规则、IP-CIDR 和 MATCH。新进程通过真实数据库及版本文件重新打开该订阅，后台按实际启动路径准备默认规则来源。每个时机用 5 个独立应用进程测量，轮换点击顺序。时间从首页标题首次呈现起决定何时点击，结果为真实规则页匹配首批目标规则画面的中位数。应用日志记录每次来源读取及转换，读取画面和日志轮询约每 8 ms 一次。

| 源规则数 | 点击时机 ms | 点击到首批规则 ms | 点击前预生成完成 | 每次启动到首次打开的来源读取 | 页面快照记账 MiB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0 / 250 / 1,000 | 63 / 44 / 48 | 0/5 / 5/5 / 5/5 | 1 / 1 / 1 | 1.20 |
| 10,000 | 0 / 250 / 1,000 | 98 / 48 / 44 | 0/5 / 5/5 / 5/5 | 1 / 1 / 1 | 14.78 |
| 50,000 | 0 / 250 / 1,000 | 363 / 148 / 428 | 0/5 / 0/5 / 5/5 | 1 / 1 / 2 | 66.85 |

- 1 万条规则在首页出现 250 ms 后点击，5 次都消费已就绪快照，页面无需再次读取或转换。点击到首批规则的 48 ms 包含入口切换、布局和渲染。
- 5 万条规则在 250 ms 点击时，页面在生成完成前固定了来源条目，5 次都复用同一个在途任务。等 1 秒再打开时，5 次预生成都完成，但发布后的快照超出 32 MiB 闲置预算并已淘汰。首次打开重新读同一个订阅版本，再做一次转换。这一组原文读取中位数 10.44 ms，第一次和第二次的目标准备中位数分别为 369.13 和 366.78 ms。点击至首批规则范围 397–491 ms。
- 5 万条规则在立即点击、等 250 ms 和等 1 秒三组中，应用峰值 RSS 的中位数约 315、317 和 409 MiB。峰值取应用进程 `/proc/<pid>/status` 的 `VmHWM`，包含窗口、运行时和软件渲染内存。单次读取的文件 I/O、锁等待、转换和各次样本见 [原始 CSV](2026-11-16-target-r5-first-open.csv)。

现有后台预生成覆盖了中等规模样本的首次打开等待。大型样本的重复转换发生在闲置淘汰和页面首次请求之间。后续 JSON 试点应优先验证这条路径，分别量化超预算快照的启动工作、页面按需读取、首批规则就绪时间及当前闲置上限；能保留的快照继续用现有内存预算衡量。原始 R5 的 JSON 恢复耗时来自独立探针，尚无应用集成后的落盘、恢复和淘汰测量。

## 复现与验证

从仓库根构建 Release 主目标。复现需要 Linux、bubblewrap、Xvfb、xwininfo、xkbcomp、libX11、libXtst 和软件 OpenGL。设备工具已装在系统上时可直接运行以下命令。若使用本次 Fedora 解包的 Xvfb，设置 `CLASHFLUX_XVFB_ROOT` 指向含 `usr/bin`、`usr/lib64` 的工具根。构建工具链若需要共享库目录，通过原有 `LD_LIBRARY_PATH` 传入。

```bash
cmake --build build-release --target clash-flux
work=$(mktemp -d "$HOME/.cache/clashflux-first-open.XXXXXX")
xvfb_args=()
if [[ -n ${CLASHFLUX_XVFB_ROOT:-} ]]; then
  xvfb_args=(--overlay-src /usr/bin --overlay-src "$CLASHFLUX_XVFB_ROOT/usr/bin"
    --ro-overlay /usr/bin --setenv LD_LIBRARY_PATH
    "$CLASHFLUX_XVFB_ROOT/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
fi
bwrap --die-with-parent --unshare-net --ro-bind / / --dev-bind /dev /dev --proc /proc \
  --tmpfs /tmp --tmpfs /run --bind "$work" "$work" "${xvfb_args[@]}" \
  --setenv DISPLAY :91 --unsetenv WAYLAND_DISPLAY --unsetenv XAUTHORITY \
  --unsetenv DBUS_SESSION_BUS_ADDRESS --unsetenv DBUS_STARTER_ADDRESS \
  --setenv GTK_A11Y none --setenv GDK_BACKEND x11 --setenv GSK_RENDERER gl \
  --setenv LIBGL_ALWAYS_SOFTWARE 1 -- python3 tests/benchmark_rules_first_open.py \
  build-release/clash-flux --work-dir "$work" > "$work/results.csv"
```

测量使用临时订阅、重复启动和自然预热的 OS 页缓存。测试收尾终止隔离应用进程，重启重新读取已经提交的数据。测量范围为合成订阅、Linux 软件渲染和隔离窗口；真实订阅、冷磁盘、硬件显示、其他桌面平台、Android 和 VPN 仍需各自验证。

Linux Release 主目标构建及设置 `CLASHFLUX_BIN` 后执行 `run.sh --version` 均通过，输出 v0.3.111。Linux Debug `test_page_transition --rules-cache`、`ctest --output-on-failure --no-tests=error -R '^core_api_lifetime$'` 与 `git diff --check` 通过。45 项测量均到达预期规则画面，CSV SHA-256 为 `39a629d2b1875be4e29bb3d19f595739e5a95249b6c67a9e03d242761428ba4c`。
