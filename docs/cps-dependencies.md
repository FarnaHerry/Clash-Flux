# CPS 依赖管理

当前构建要求 CMake 4.4，本机与 CI 验证基线为 4.4.2。通用依赖使用 CMake
`install(PACKAGE_INFO)` 生成真正的 CPS 包；应用通过带精确版本与组件的
`find_package(... CONFIG REQUIRED)` 消费，并断言找到的是 `.cps` 文件。

## 构建流程

1. `cmake/dependencies/Lock.cmake` 集中声明归档版本/SHA、直提文件 SHA 和
   HuxerUI 扩展的固定 URL/COMMIT。归档默认来自仓库，无新增联网下载。
2. 首次应用配置自动运行 `cmake/dependencies` 独立 producer，使用与应用相同
   的目标工具链、编译器和 flags。Glaze、nlohmann_json、QrCode、zstd、yaml-cpp、
   CURL、IXWebSocket 分别导出 CPS 包。
3. 包存放于 `build/dependencies/<fingerprint>/prefix`。身份覆盖 recipe、锁定输入、
   CMake、编译器版本、ABI、构建类型、SDK/NDK、编译/链接 flags 和 OpenSSL。
   多配置生成器分别安装各配置的库，应用的 C++ 模块/BMI 继续在本体构建中生成。
4. 后续配置校验完整包内容和已记录的系统链接依赖，再复用缓存。缺失或改动的包
   重新构建/安装；归档 SHA 不符则明确失败并保留原源码缓存，不能凭旧 SHA 标记放行。
5. 安装先进入独立 staging，成功后才替换包前缀。失败不接受半份包；不承诺多文件
   掉电事务。日志保留在对应缓存目录的 configure/build/install 文件中。

常规命令不变：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON
cmake --build build --target clash-flux
./run.sh --version
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
```

`CLASHFLUX_DEPENDENCY_CACHE_DIR` 可指定独立包缓存目录；
`CLASHFLUX_DEPENDENCY_JOBS` 控制 producer 的编译并行度（默认 4）。

## 包与平台边界

- curl 保留静态库、桌面 OpenSSL、HTTP/2/IDN 自动发现和本地 WebSocket 的既有
  构建行为。它发现的系统链接要求成为 CURL 包的私有接口组件，保留 includes、
  definitions、配置对应的库路径及顺序；系统库不复制、不替换。
- 显式使用仓库固定静态 OpenSSL fallback 时，FindOpenSSL 不借用其它系统
  OpenSSL 的 pkg-config 元数据，避免错误的传递依赖。其它 OpenSSL 仍走原生查找。
- Android/iOS curl 保持原有无 TLS 编译路径；Android 订阅下载的 Java TLS 桥接
  保持证书校验，不能拿宿主机 CPS 包替代 NDK 包。Android Gradle 与 CI 同用 4.4.2。
- Glaze 模板仍只在 `clashflux_wire` 普通 TU；JSON 命名模块包装留在本体。
- HuxerUI 的 codegen、资源、宿主工具、扩展和 Android Java/native 图继续通过
  框架接入。框架 revision 不变，不把复杂框架流程伪装成普通链接库。
- sing-box 二进制、libbox AAR 与打包规则集继续使用原有版本/revision/SHA 资产流程；
  CPS 不承担下载器、系统包管理器或 APK 资源管理器的职责。

## 验证

桌面门禁增加 `cps_dependencies`：消费迁移到另一目录的七个真实包，检查静态
编译定义与新前缀内的库路径，编译/运行 Glaze、JSON、YAML、QR、Zstd、curl 与
WebSocket 客户端；拒绝错误版本，并验证暖缓存下归档篡改仍被拒绝且旧源码保留。
系统依赖另经真实 CPS 导出/导入验证 Debug/Release 库路径、大小写配置映射和
显式选择无配置库的映射。
通用桌面为 21 项（含 runtime_health、native_control_runtime、ui_resources、runtime_evidence），Windows 加 windows_service、
windows_physical_dns、windows_physical_dns_wfp 为 24 项；已记录的 MSVC C4737
豁免仅去掉直接 ORM，Windows 为 23 项，persistence 保持必跑。Windows 服务与 WFP 测试需管理员环境。

本次本地验证使用 CMake 4.4.2：Linux 应用构建与 17 项必跑回归通过；
独立 Ninja Multi-Config producer 的 Debug/Release 包均通过重定位后的编译与运行；
缓存 CPS 元数据篡改触发重新安装，随后配置复用验证通过的包。
Android NDK 29 / arm64-v8a Debug APK 构建通过，包含 libbox 与两个打包规则集。
扩展库下载受阻时使用已验证固定提交的独立 Git 对象镜像作为本次下载缓存，
没有修改 URL/COMMIT 或使用可变源码工作树。Windows/macOS 尚未运行这次修改后的 CI。
