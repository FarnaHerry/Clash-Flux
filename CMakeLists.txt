# CMakeLists.txt — Clash-Flux 顶层构建（HuxerUI CLI 生成格式 + 本仓库 vendor 依赖）。
#
# 形状对齐 `huxerui create app` 产物（脚手架移植自姊妹项目 apitab）：顶部项目自省块
# （CLI plan-only 配置读取）、platform/<平台>/main.cpp 平台入口、src/ 递归源码发现、
# resources/ 以 app 命名空间注册。差异仅在于：依赖 vendor 在 third_party/（HuxerUI
# 优先 third_party/huxerui 源码编译，tarballs 里的预编译 SDK 兜底），领域层以
# C++23 模块（FILE_SET）追加到同一 app 目标。
#
# 常用命令：
#   huxerui build linux [--profile debug|release]   # CLI 构建（.huxerui/build/linux/...）
#   huxerui run linux                               # CLI 构建并运行
#   cmake -B build -G Ninja && cmake --build build  # 直接 CMake 流程（仍然可用）
#   ctest --test-dir build                          # 冒烟测试
#   ./run.sh                                        # 运行 build/ 出的 GUI
cmake_minimum_required(VERSION 3.30)

# ---- HuxerUI CLI 项目自省 --------------------------------------------------
# `huxerui build/run/package` 先用 -DHUXERUI_PROJECT_PLAN_ONLY=ON 配置本项目，
# 读本块写出的 JSON 获知 kind/name/id/target；本块必须位于 project() 之前，
# 且不依赖任何语言/包解析（plan-only 配置会直接 return）。
if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "app",
  "name": "clash-flux",
  "id": "dev.farna.clashflux",
  "target": "clash-flux"
}
]=])
endif ()
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

project(clash-flux VERSION 0.1.0 DESCRIPTION "Clash/mihomo proxy client (Clash Verge Rev style)" LANGUAGES NONE)

# HUXERUI_LIBRARY_GRAPH_ONLY：CLI 扫描 huxerui 库依赖图时的特殊配置——
# 不启用语言、不构建，huxerui_add_app 只写出库图 JSON 即返回（见 SDK
# HuxerUIApp.cmake）。本项目的重活（第三方依赖/模块/链接）全部在该模式下跳过。
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # `import std` 的 experimental 门控必须在 enable_language(CXX) 前设置
    # （UUID 随 CMake 版本变，见 cmake/CxxImportStdGate.cmake 顶部注释）。
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(CxxImportStdGate)
    enable_language(C)
    enable_language(CXX)
    if (WIN32)
        enable_language(RC)
    endif ()

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)            # gnu++23：模块实现与 GCC 标准库组合最顺
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # 只给用 import std 的目标开 CXX_MODULE_STD（clash-flux / test_smoke），
    # 不影响 curl 等 C++17 子项目。
    set(CMAKE_CXX_MODULE_STD OFF)

    # 默认 Release（单配置生成器）；调试请显式 -DCMAKE_BUILD_TYPE=Debug
    # （huxerui CLI 会按 --profile 显式传入，不受此默认影响）
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif ()

    find_package(Threads REQUIRED)
endif ()

# ---- HuxerUI（0.2.0 SDK / 源码双通道，HUXERUI_HOME 契约）-------------------
# 优先级（固定顺序，与 apitab 一致）：
#   1. 显式 HUXERUI_HOME 源码（有 CMakeLists.txt + include/huxerui/huxerui.h）。
#   2. third_party/huxerui 源码（本地 git clone 的上游仓库，add_subdirectory 编译，
#      跟踪上游 `git pull` 即可；不入库，.gitignore 已忽略）。
#      CLI 自动注入的已安装 SDK 前缀（HUXERUI_HOME 指向 SDK）不得越过前两级。
#   3. 已安装 SDK（HUXERUI_HOME 指向含 lib/cmake/HuxerUI 的 SDK 目录）。
#   4. 仓库离线 SDK（tarball，Linux 专用；macOS/Windows 不提供离线包）。
#
# CLASHFLUX_HUXERUI_FORCE_SDK=ON：唯一明确跳过源码的开关，只走第 3/4 通道（发布门禁
# 与兼容测试用）。源码模式编译 Linux 后端需要 gtk4-devel libsoup3-devel。
include(cmake/clashflux_vendor.cmake)
set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI 0.2.0 SDK or source directory")
set(CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui" CACHE PATH "Bundled HuxerUI source checkout (override for testing to simulate missing source without moving shared directory)")
set(CLASHFLUX_HUXERUI_SOURCE_DIR "")
option(CLASHFLUX_HUXERUI_FORCE_SDK "Skip source checkout; use SDK fallback only" OFF)
option(CLASHFLUX_HUXERUI_MOCK_MISSING_DEPS "Force missing gtk4/libsoup for selection-matrix testing" OFF)
set(CLASHFLUX_HUXERUI_FROM_SOURCE OFF)

# 优先级 1：显式 HUXERUI_HOME 源码（用户显式指向源码目录时优先于仓库内源码）
if (NOT CLASHFLUX_HUXERUI_FORCE_SDK
        AND HUXERUI_HOME
        AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
        AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
    set(CLASHFLUX_HUXERUI_SOURCE_DIR "${HUXERUI_HOME}")
endif ()

# 优先级 2：仓库内 third_party/huxerui 源码（默认 source-first）
if (NOT CLASHFLUX_HUXERUI_SOURCE_DIR
        AND NOT CLASHFLUX_HUXERUI_FORCE_SDK
        AND EXISTS "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}/CMakeLists.txt"
        AND EXISTS "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}/include/huxerui/huxerui.h")
    set(CLASHFLUX_HUXERUI_SOURCE_DIR "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}")
endif ()

# 源码依赖检查（Linux）：与 HuxerUI 的 GTK Snapshot / GPU 纹理依赖保持一致。
if (CLASHFLUX_HUXERUI_SOURCE_DIR
        AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (CLASHFLUX_HUXERUI_MOCK_MISSING_DEPS)
        set(CLASHFLUX_HUXERUI_GTK4_FOUND OFF)
        set(CLASHFLUX_HUXERUI_EPOXY_FOUND OFF)
        set(CLASHFLUX_HUXERUI_LIBSOUP_FOUND OFF)
    else ()
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(CLASHFLUX_HUXERUI_GTK4 QUIET gtk4>=4.14)
            pkg_check_modules(CLASHFLUX_HUXERUI_EPOXY QUIET epoxy>=1.5)
            pkg_check_modules(CLASHFLUX_HUXERUI_LIBSOUP QUIET libsoup-3.0>=3.0)
        endif ()
    endif ()
    if (NOT CLASHFLUX_HUXERUI_GTK4_FOUND OR NOT CLASHFLUX_HUXERUI_EPOXY_FOUND
            OR NOT CLASHFLUX_HUXERUI_LIBSOUP_FOUND)
        message(WARNING "clash-flux: ${CLASHFLUX_HUXERUI_SOURCE_DIR} 存在但缺 GTK >=4.14 / epoxy >=1.5 / libsoup >=3.0 开发包"
            "（sudo dnf install gtk4-devel libepoxy-devel libsoup3-devel），回落预编译 SDK —— 本次未使用源码")
        set(CLASHFLUX_HUXERUI_SOURCE_DIR "")
    endif ()
endif ()

# 源码模式
if (CLASHFLUX_HUXERUI_SOURCE_DIR)
    set(CLASHFLUX_HUXERUI_FROM_SOURCE ON)
    # 上游顶层作为子项目时 tests/examples/CLI 默认 OFF；static 切片必须关掉——
    # _huxerui_select_framework_target 偏好 static，留着会让 app 链到静态库。
    set(HUXERUI_BUILD_STATIC OFF)
    # 上游预置宿主工具可能落后于源码（linux 预置 hrc 旧 → 新运行时解旧资源包直接
    # 抛 "unknown path operation"）。本地若有用当前源码编出的工具
    # （third_party/huxerui-tools/<platform>/<arch>/{hcg,hrc}），优先使用；
    # 缺失时回落上游预置。
    set(CLASHFLUX_HUXERUI_LOCAL_TOOLS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-tools")
    if (CLASHFLUX_HUXERUI_SOURCE_DIR STREQUAL CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR
            AND EXISTS "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hrc"
            AND EXISTS "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hcg")
        set(HUXERUI_HOST_TOOL_ROOT "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}")
        message(STATUS "clash-flux: 宿主工具用本地源码构建（third_party/huxerui-tools）")
    endif ()
    add_subdirectory("${CLASHFLUX_HUXERUI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/huxerui-sdk"
        EXCLUDE_FROM_ALL)
    # 打印实际来源与 commit，使 `huxerui build/run` 默认输出可见
    execute_process(COMMAND git -C "${CLASHFLUX_HUXERUI_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE CLASHFLUX_HUXERUI_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (CLASHFLUX_HUXERUI_SOURCE_COMMIT)
        message(STATUS "clash-flux: HuxerUI 源码编译（${CLASHFLUX_HUXERUI_SOURCE_DIR} @ ${CLASHFLUX_HUXERUI_SOURCE_COMMIT}）")
    else ()
        message(STATUS "clash-flux: HuxerUI 源码编译（${CLASHFLUX_HUXERUI_SOURCE_DIR}）")
    endif ()
else ()
    # SDK 模式：优先级 3（HUXERUI_HOME SDK）→ 优先级 4（离线 tarball，Linux 专用）
    # find_package 不依赖语言启用，graph-only 模式照常执行。
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/lib/cmake/HuxerUI/HuxerUIConfig.cmake")
        set(HUXERUI_SDK_DIR "${HUXERUI_HOME}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "clash-flux: HuxerUI SDK（已安装，${HUXERUI_SDK_DIR}）")
    elseif (APPLE OR WIN32)
        message(FATAL_ERROR
            "clash-flux: macOS/Windows 未提供离线 SDK 包；"
            "请用 HUXERUI_HOME 指向 0.2.0 源码或已安装 SDK，"
            "或 clone third_party/huxerui 后默认以源码模式编译")
    else ()
        clashflux_extract(huxerui
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.2.0-linux-x86_64.tar.gz"
            "e184a62a0e5e22cd71835d5278034d65f8a30a2c48273513633f88719a1243e5"
            "huxerui-sdk-0.2.0-linux-x86_64")
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "clash-flux: HuxerUI SDK（离线 tarball，${HUXERUI_SDK_DIR}）")
    endif ()
    if (NOT HUXERUI_SDK_DIR)
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
    endif ()
    set(HuxerUI_DIR "${HUXERUI_SDK_DIR}/lib/cmake/HuxerUI" CACHE PATH "HuxerUI package" FORCE)
    set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
    # shared 组件：桌面 Linux/Windows 运行时动态库（static 切片在 Linux 要 gtk4
    # 开发包，本仓库不用）。
    find_package(HuxerUI 0.2.0 CONFIG REQUIRED COMPONENTS shared
        PATHS "${HUXERUI_SDK_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif ()

# 平台壳的 HuxerUI 配置钩子（生成格式：platform/<平台>/huxerui.cmake 可选）
if (WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake" OPTIONAL)
elseif (APPLE AND NOT IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake" OPTIONAL)
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # ---- OpenSSL 解析（必须在 add_subdirectory(third_party) 前：imported target
    # 要对 curl 等子项目全局可见）----------------------------------------
    # 优先系统 OpenSSL；系统没有时，linux x86_64 使用 vendor 的静态 3.5.1。
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND)
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
            clashflux_extract(openssl
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz"
                "d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf"
                "openssl-3.5.1-linux-x86_64")
            set(OPENSSL_ROOT_DIR "${openssl_SOURCE_DIR}" CACHE PATH "vendored OpenSSL" FORCE)
            set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
            find_package(OpenSSL REQUIRED)
            message(STATUS "clash-flux: using vendored OpenSSL ${OPENSSL_VERSION} (static)")
        else ()
            message(FATAL_ERROR
                "未找到 OpenSSL 开发头文件/库。请安装系统 OpenSSL（libssl-dev / "
                "openssl@3 / vcpkg openssl），或为该平台补充 third_party/tarballs 下的静态包。")
        endif ()
    endif ()

    add_subdirectory(third_party)
endif ()

# ---- clash-flux app 目标 ----------------------------------------------------
# 源码发现对齐生成格式：src/ 下递归 glob 普通 C++ 源（UI composable 源 + 领域
# 模块的实现单元 .cpp；codegen 无 composable 标记时原样放行）。模块接口 .cppm
# 不经 glob，由 FILE_SET 显式追加。
file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
# 平台入口（graph-only 配置不编译任何东西，跳过）
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

# CLI 依赖图输出位置（生成格式约定；huxerui_add_app 在存在该变量时写出）
set(HUXERUI_LIBRARY_GRAPH_OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/.huxerui/generated/libraries.json"
)

set(CLASHFLUX_MODULE_UNITS
    # 基础设施
    src/config.cppm
    src/utils.cppm
    src/db.cppm
    # mihomo 核心：进程生命周期 / REST API / 推送流 / 系统代理
    src/core.cppm
    src/api.cppm
    src/stream.cppm
    src/sysproxy.cppm
    # 服务模式（root 托管内核；非 Linux 平台编译为同签名 stub）+ 完整 CLI
    src/service.cppm
    src/cli.cppm
    # 领域 store（UI 无关）
    src/store/core_store.cppm
    src/store/profiles.cppm
)

huxerui_add_app(clash-flux
    SOURCES
        ${APP_SOURCE_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    RESOURCE_NAMESPACE app
    BUNDLE_NAME "Clash-Flux"
    BUNDLE_IDENTIFIER "dev.farna.clashflux"
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # 领域层 C++23 模块（.cppm 接口；对应 .cpp 实现单元已随 glob 进 SOURCES）
    target_sources(clash-flux PRIVATE FILE_SET clashflux_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${CLASHFLUX_MODULE_UNITS})
    set_target_properties(clash-flux PROPERTIES CXX_MODULE_STD ON)
    # 显式打开全源扫描：模块实现单元（普通 .cpp 里的 module clashflux.*;）必须经
    # 扫描拿到 -fmodules-ts。SDK 模式下 CMP0155 默认行为恰好扫描全部源，源码
    # 模式（add_subdirectory huxerui）下该默认不覆盖普通 .cpp，会丢模块标志。
    set_target_properties(clash-flux PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_compile_features(clash-flux PRIVATE cxx_std_23)
    # 应用版本编译期常量（设置页“关于”展示）：唯一来源是顶层 project(VERSION)。
    target_compile_definitions(clash-flux PRIVATE CLASHFLUX_VERSION="${PROJECT_VERSION}")
    # windows.h 的 min/max 宏会毁掉所有经 ixwebsocket/curl 头链引入它的
    # TU 里的 std::max/std::min（MSVC C2589）；宏不跨 TU，必须整个目标定义。
    if (WIN32)
        target_compile_definitions(clash-flux PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        # GUI 子系统入口（CLI 子命令运行时经 AttachConsole 附着父控制台，
        # 见 cli.cppm）。
        if (MSVC)
            target_link_options(clash-flux PRIVATE
                /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        else ()
            target_link_options(clash-flux PRIVATE -mwindows)
        endif ()
    endif ()
    # 源码注释含中文：MSVC 必须显式 /utf-8，否则按 codepage 误读
    if (MSVC)
        target_compile_options(clash-flux PRIVATE /utf-8)
    endif ()
    target_include_directories(clash-flux PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(clash-flux PRIVATE
        ixwebsocket
        CURL::libcurl
        SQLiteCpp
        OpenSSL::SSL
        OpenSSL::Crypto
        clashflux_json
        Threads::Threads
    )

    # 生成格式钩子：平台清单 / Info.plist
    if (WIN32 AND HUXERUI_WINDOWS_MANIFEST)
        target_sources(clash-flux PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
    endif ()
    if (APPLE AND NOT IOS AND HUXERUI_MACOS_INFO_PLIST)
        set_target_properties(clash-flux PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}")
    endif ()

    # ---- 自带 mihomo 内核 ---------------------------------------------------
    # 项目附带引擎：configure 期按宿主平台下载官方 release（版本与 SHA256 钉死，
    # 见 cmake/mihomo_bundle.cmake 的平台资产表），POST_BUILD 拷到
    # <exe>/engines/mihomo（Windows 为 mihomo.exe —— cfg::mihomoBinary() 第一
    # 优先级解析位），构建产物开箱即可启动内核。-DCLASHFLUX_BUNDLE_MIHOMO=OFF
    # 关闭（回落 <repo>/engines 与 PATH 解析）。覆盖 linux/windows x86_64 与
    # macOS x86_64/arm64；其他平台显式关闭并自行放置内核。
    include(cmake/mihomo_bundle.cmake)
    clashflux_bundle_mihomo(clash-flux)

    # 图标资源放 <exe>/assets；HuxerUI 资源包（内置+app 合并，由 huxerui_add_app 的
    # 资源构建生成）放 <exe>.resources。最新运行时除了 resources.bin 索引外，
    # 还会按索引读取同目录下的 .huxv/.png 等实际 payload，必须把整个 package
    # 目录复制到运行目录。
    add_custom_command(TARGET clash-flux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:clash-flux>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/huxerui-resources/clash-flux/package"
                "$<TARGET_FILE_DIR:clash-flux>/clash-flux.resources"
        COMMENT "Copying assets and the merged HuxerUI resource package next to clash-flux")

    # 仓库根的 compile_commands.json 软链（给 clangd 用；.gitignore 已忽略）
    # 并行测试/多构建并发时多个 build 目录同时创建同一软链接会竞态；已存在则跳过
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json")
        file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
                         "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
                         SYMBOLIC)
    endif()

    # ---- 测试 ---------------------------------------------------------------
    # 独立于 CTest 的 BUILD_TESTING 变量：curl 等子项目会把它改成 OFF，不能让它
    # 决定 clash-flux 自己的测试。
    option(CLASHFLUX_BUILD_TESTS "Build clash-flux test executables" ON)
    if (CLASHFLUX_BUILD_TESTS)
        enable_testing()
        add_executable(test_smoke tests/test_smoke.cpp)
        set_target_properties(test_smoke PROPERTIES CXX_MODULE_STD ON)
        target_compile_features(test_smoke PRIVATE cxx_std_23)
        add_test(NAME smoke COMMAND test_smoke)
    endif ()
endif ()
