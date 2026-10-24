# 猫咪出箱动画 Demo

独立的 Windows / Linux HuxerUI 应用，当前方案为「开箱睡觉 → 轻抚加载 → 猫咪醒来探头 → 运行卡片」。
主程序首页已通过 `src/ui/box_cat.cpp` 接入同一套视觉与动作，由真实内核/VPN 状态驱动；本 Demo 继续只用于隔离调试。
箱盖始终展开，后盖、箱内后壁、前盖与箱体都保持不动；停止或失败后猫咪回去睡觉，不合箱。
点击放大预览或实际按钮均可唤醒/停止，点击范围覆盖整个按钮，不需要精确点中细线。
待机猫咪使用原 Logo 的轮廓和闭眼表情，三枚 z 循环由小变大向右上方飘起、淡出。
手轻抚猫咪作为启动期间的加载指示：猫咪只保留舒服的弯眼，不画嘴巴，头部轻轻低下、靠向手掌；
等待模拟结果时持续轻抚，猫咪起身最后一段时手轻轻抬起、淡出；
起身完成就退场，随后展开统计卡片。慢启动在等待期间持续显示；待机、运行、失败或取消后均隐藏。
点击后睡眠 zzz 消失，完全启动后保留原有运行态 Logo、耳尾轻摆及统计卡片展开。
窗口并排提供放大预览与实际卡片，共用动画和进度控制；项目正式 Logo 与旧闭箱素材均保留。
默认使用与主程序相同的 `third_party/huxerui` 固定源码；不链接 Clash-Flux 的领域模块，不访问数据库、订阅、内核、
系统代理或 TUN。时长和连接统计均为固定示例数据，不代表真实运行状态。

Windows 从仓库根目录使用 PowerShell（需要 Visual Studio C++ 工具、Windows SDK、
CMake 4.4+ 和 Ninja，不需要 WSL）：

```powershell
./demos/box-cat-demo/run-demo.ps1 -BuildOnly
./demos/box-cat-demo/run-demo.ps1
./demos/box-cat-demo/run-demo.ps1 -DemoArguments '--version'
```

脚本按需加载 Visual Studio x64 开发环境，使用仓库固定 HuxerUI 源码，
完成 codegen、资源生成和原生编译；优先复用主工程构建缓存中的 CMake，
也可用 `-CMakeExecutable 'C:/tools/cmake/bin/cmake.exe'` 指定 4.4+ 版本。
独立产物在 `demos/box-cat-demo/build-windows/`，
直接运行以下命令可跳过构建，冒烟检查会打开独立窗口并自动退出：

```powershell
& ./demos/box-cat-demo/build-windows/box_cat_demo.exe --version
& ./demos/box-cat-demo/build-windows/box_cat_demo.exe --smoke-test | Out-Host
& ./demos/box-cat-demo/build-windows/box_cat_demo.exe --brand-preview --smoke-test | Out-Host
```

Windows 使用 GUI 子系统和原生 DPI manifest，正常启动不弹出额外控制台。
Linux 从仓库根目录启动：

```bash
./demos/box-cat-demo/run-demo.sh
```

脚本每次先编译独立的 `box_cat_demo` 目标，然后启动 demo。需要 CMake 4.4+、Ninja、
C++ 编译器、项目的固定 HuxerUI checkout 与相应 GTK/epoxy/libsoup 开发依赖。
首次编译会在 demo 自己的构建目录生成框架静态库，后续为增量构建。
不启动 Clash-Flux 主程序，可与正在提供代理的主程序并行运行。

操作：

- **唤醒**：手轻抚猫咪，猫咪带着舒服的弯眼起身；起身后手退场并恢复睁眼，再展开统计卡片，完全启动后尾巴轻摆与双耳轻抖。
- **慢唤醒**：延长等待，手持续轻抚表示启动中，猫咪起身或取消后隐藏。
- **失败演示**：猫咪偷偷探出半个头，轻轻歪头 wink 一下，再回到睡脸与 zzz 待机，回到可重试状态。
- **停止**：收起统计卡片，猫咪缩回睡觉，箱盖保持展开；也可中断正在进行的唤醒。
- **重置**：取消待完成的模拟请求，立即回到开箱睡觉。
- **手动进度**：可切换成功/失败动画，检查任意中间画面，同时取消旧模拟请求。
- **播放速度**：0.25–2 倍，下一次操作生效。
- **减少动态效果**：立即解析到目标画面，并停止睡眠 zzz、加载轻抚与运行耳尾循环（加载中仍显示静态抚摸手势）；也遵循 SDK 的系统减少动态效果策略。

放大图与实际卡片使用同一套保留动画。转场由 `MotionController` 逐帧推进，
成功后的耳尾循环由保留节点的帧时钟推进，限频 30fps，不逐帧写入 UI State 或重新布局。
尾巴约每 3.8 秒轻摆一轮，双耳约每 5.6 秒错开抖动，幅度均为几度；头部、眼睛与箱体保持稳定。
停止、失败或重置后停止耳尾循环，回到睡觉时恢复 zzz；窗口隐藏/最小化或减少动态效果暂停所有循环，恢复显示时从自然姿势重新开始。
猫咪与耳朵沿用原有轮廓，尾巴保留卷曲端并把根部延伸到箱内，箱体 SVG 从新版单中线 Logo 拆出；
运行态静止及减少动态效果时显示 `logo_completed.svg` 的补全箱体版本。
`logo.svg` 继续保留未经修改的新 Logo 作为参考，猫咪、前盖和单中线沿用这份图稿。
失败使用独立的有限时间轴，只探头、不出现尾巴或运行统计；右眼由独立 SVG 图层压缩、闭合为弧线，
保持片刻后睁开，随后回去睡觉，箱盖不动。失败表情素材沿用现有猫咪轮廓与左眼。
头部位移与卡片展开由保留进度驱动，手与舒服表情的轻微运动共用加载时钟。睡眠 zzz 使用保留帧时钟，
2.8 秒一轮、三枚错开，限频 30fps，不逐帧写 State 或重新布局；窗口隐藏时暂停。
加载轻抚同样使用保留帧时钟，1.8 秒一轮、限频 30fps，在起身进度 0.55–0.63 时抬起、淡出，0.63 起身完成即停止轻抚时钟；
卡片随后在 0.72–1.0 展开，busy 仍结束于整个模拟操作完成，不因手提前退场而改变启动状态。停止时不倒放轻抚；减少动态效果保留静态 zzz 或加载抚摸手势。
箱体和所有开盖姿势复用完整运行态 SVG 图层，固定基线保持唤醒前后箱体位置稳定。
当前 demo 专注深色主题和桌面效果，未接入主程序启停生命周期或手机 VPN。

猫头轮廓统一使用不透明的 `cat_head.svg`；睡脸、舒服弯眼和睁眼拆为独立表情资源，
只对眼睛渐变，不交叉淡化两张实心猫头。失败探头、回箱和普通唤醒都保持遮挡箱内背景，
轻微歪头时也只绘制一个实体轮廓；前盖始终最后绘制。原完整表情 SVG 作为参考保留。
加载时只使用弯眼，不画嘴巴；手势参考 `cat.html` 的圆润线稿：右手手背朝向用户、拇指在画面左侧，四指朝上且长短错落，掌心饱满。手掌在猫咪头顶左右往返并轻转约 ±4°，掌形收在双眼之间上方，让两只眼睛保持完整可见；视觉风格沿用项目 Logo 的深色填充与浅色圆头描边。
`petting_hand_fill.svg` 是深色掌形，`petting_hand.svg` 是轮廓与指节线稿；描边沿用前景色，实心掌形遮挡后方耳朵线条。
左右轻抚与猫咪的轻微低头、反向侧倾共用加载时钟；原 `cat_wand.svg` 作为历史素材保留，不参与当前加载。
运行造型和正式项目品牌资源不变。
历史四折盖几何、闭箱十字盖缝与生成脚本仍保留为素材，不参与当前唤醒动画。
尾巴仍位于后盖/后壁前方、猫咪/前盖后方，绕箱内 (92, 74) 支点轻摆。

独立验证（需要图形会话）：

```bash
./demos/box-cat-demo/run-demo.sh --version
cmake --build demos/box-cat-demo/build --target box_cat_demo
./demos/box-cat-demo/build/box_cat_demo --version
./demos/box-cat-demo/build/box_cat_demo --smoke-test
./demos/box-cat-demo/build/box_cat_demo --preview-progress 0.5
./demos/box-cat-demo/build/box_cat_demo --preview-failure 0.5
```

`--smoke-test` 打开独立窗口，检查放大预览与实际卡片的睡脸与 zzz 循环及帧预算、等待期间保持、起身后提前退场、慢启动轻抚及帧预算、取消退场、耳尾推进及帧预算、减少动态效果的暂停/恢复、
停止后循环结束、失败 wink、失败后重试、探头阶段停止/重置与慢启动被停止后旧结果失效，
再播放成功/失败的分阶段预览并自动关闭。
状态断言失败会报告错误；视觉效果需实际查看。
仓库要求的主目标编译与 `./run.sh --version` 仍需执行；不需要打开主程序 GUI。


## 正式项目 Logo 预览

项目正式 Logo 采用本 demo 的完整运行态造型。`assets/branding/logo.svg` 是正式资源母版；
`scripts/generate-branding.py` 将深浅版本的字节一致副本放入 demo，供框架真实挂载检查。

```bash
./demos/box-cat-demo/run-demo.sh --brand-preview
./demos/box-cat-demo/build/box_cat_demo --brand-preview --smoke-test
```

该模式并排显示浅色和深色主题，以及 32 / 48 / 64pt 实际尺寸。第二条命令挂载后自动退出，
用于检查资源加载与启动；视觉细节仍需查看窗口或 `docs/branding/logo-preview.png`。
唤醒模式保留历史原稿和开盖素材，不依赖主程序运行时。
