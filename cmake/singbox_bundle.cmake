# singbox_bundle.cmake — 自带 sing-box 内核（桌面平台）。
#
# clashflux_bundle_singbox(<target>)：configure 期按宿主平台下载官方 release
# （版本与 SHA256 钉死），POST_BUILD 把可执行文件拷到 <exe>/engines/sing-box
# （Windows 为 sing-box.exe —— cfg::singboxBinary() 的第一优先级解析位），
# 构建产物开箱即可启动内核。-DCLASHFLUX_BUNDLE_SINGBOX=OFF 关闭（回落
# <repo>/engines 与 PATH 解析）。
#
# 平台资产表（v1.14.0，https://github.com/SagerNet/sing-box/releases，与
# Android libbox 同版本）：
#   linux   x86_64  sing-box-1.14.0-linux-amd64.tar.gz    （内含 sing-box-1.14.0-linux-amd64/sing-box）
#   linux   arm64   sing-box-1.14.0-linux-arm64.tar.gz
#   windows x86_64  sing-box-1.14.0-windows-amd64.zip     （内含 sing-box-1.14.0-windows-amd64/sing-box.exe）
#   windows arm64   sing-box-1.14.0-windows-arm64.zip
#   darwin  x86_64  sing-box-1.14.0-darwin-amd64.tar.gz
#   darwin  arm64   sing-box-1.14.0-darwin-arm64.tar.gz
# sing-box 发布什么桌面平台/arch，本表就钉什么——GUI 壳能编到的目标都自带
# 内核。新增资产/升版本时：`gh api repos/SagerNet/sing-box/releases/tags/<v>`
# 取 digest，或下载后 sha256sum 钉进下表。

option(CLASHFLUX_BUNDLE_SINGBOX
       "Download and bundle the sing-box kernel next to the executable" ON)

function(clashflux_bundle_singbox target)
    if (NOT CLASHFLUX_BUNDLE_SINGBOX)
        return()
    endif ()

    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        # Android uses sing-box libbox from Gradle; the desktop binary is not
        # downloaded while CMake configures each Android ABI.
        message(STATUS "clash-flux: Android sing-box libbox is supplied by Gradle")
        return()
    endif ()

    set(CLASHFLUX_SINGBOX_VERSION "1.14.0")
    set(_base_url "https://github.com/SagerNet/sing-box/releases/download/v${CLASHFLUX_SINGBOX_VERSION}")

    # ---- 平台资产选择 -----------------------------------------------------
    set(_asset "")
    set(_sha "")
    set(_inner_dir "")  # 压缩包内顶层目录（内含 sing-box[.exe]）
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-linux-amd64.tar.gz")
        set(_sha "2375de6999f4f56ab46b4fc5ddf26a6aba1d3e61a0f4e7ddec2f4690457d5f63")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-linux-amd64")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-linux-arm64.tar.gz")
        set(_sha "04d9b40bc98dc55b6f509ce3292145c65478f65866bea64826ebb2f382385088")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-linux-arm64")
    elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-windows-amd64.zip")
        set(_sha "3ffb56267da14e287be48bd10cf7e6505260125bad940b75101fbb4d5d58e5d6")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-windows-amd64")
    elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-windows-arm64.zip")
        set(_sha "f58dff882b2feb022da8de41943804b38681ecab5e1f490f23602fc37e9d5dd4")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-windows-arm64")
    elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-darwin-amd64.tar.gz")
        set(_sha "6cf26fc3501f3117cf781e9405cf5338f60add6da5affae39421af6800ebbcb4")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-darwin-amd64")
    elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
        set(_asset "sing-box-${CLASHFLUX_SINGBOX_VERSION}-darwin-arm64.tar.gz")
        set(_sha "a150c94012ff768b7261939cd236b9c8554127f45137230295d23a5660225cc9")
        set(_inner_dir "sing-box-${CLASHFLUX_SINGBOX_VERSION}-darwin-arm64")
    else ()
        message(FATAL_ERROR
            "CLASHFLUX_BUNDLE_SINGBOX 暂无 ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} 的预置包；"
            "请 -DCLASHFLUX_BUNDLE_SINGBOX=OFF 并自行放置 engines/sing-box")
    endif ()
    if (WIN32)
        set(_inner_bin "${_inner_dir}/sing-box.exe")
        set(_bundled_name "sing-box.exe")
    else ()
        set(_inner_bin "${_inner_dir}/sing-box")
        set(_bundled_name "sing-box")
    endif ()

    # 解包后的规范名（engines/ 里的拷出名统一为 sing-box / sing-box.exe）。
    set(CLASHFLUX_SINGBOX_DIR "${CMAKE_BINARY_DIR}/vendor/singbox")
    set(_bin "${CLASHFLUX_SINGBOX_DIR}/${_bundled_name}")

    # ---- 下载 + 解包（幂等：规范名已存在则跳过）----------------------------
    if (NOT EXISTS "${_bin}")
        file(MAKE_DIRECTORY "${CLASHFLUX_SINGBOX_DIR}")
        set(_archive "${CLASHFLUX_SINGBOX_DIR}/${_asset}")
        if (NOT EXISTS "${_archive}")
            message(STATUS "clash-flux: 下载自带内核 sing-box v${CLASHFLUX_SINGBOX_VERSION}（${_asset}）")
            file(DOWNLOAD "${_base_url}/${_asset}" "${_archive}"
                EXPECTED_HASH "SHA256=${_sha}"
                STATUS _dl_status)
            list(GET _dl_status 0 _dl_code)
            if (NOT _dl_code EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "sing-box 下载失败：${_dl_status}")
            endif ()
        endif ()
        # tar.gz 与 zip 都由 libarchive 直读（cmake -E tar），无需外部工具。
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_archive}"
            WORKING_DIRECTORY "${CLASHFLUX_SINGBOX_DIR}"
            RESULT_VARIABLE _tar_result)
        if (NOT _tar_result EQUAL 0)
            file(REMOVE "${_archive}")
            message(FATAL_ERROR "sing-box 解压失败（cmake -E tar 退出码 ${_tar_result}）")
        endif ()
        file(RENAME "${CLASHFLUX_SINGBOX_DIR}/${_inner_bin}" "${_bin}")
        file(CHMOD "${_bin}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif ()

    # ---- POST_BUILD 拷到 <exe>/engines/ -----------------------------------
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/engines"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_bin}" "$<TARGET_FILE_DIR:${target}>/engines/${_bundled_name}"
        COMMENT "Bundling sing-box v${CLASHFLUX_SINGBOX_VERSION} into engines/")

    # 暴露解包出的内核路径（打包安装规则用，见顶层 CMakeLists Windows/Linux 打包块）。
    set(CLASHFLUX_SINGBOX_BUNDLED_BINARY "${_bin}" PARENT_SCOPE)
    set(CLASHFLUX_SINGBOX_BUNDLED_NAME "${_bundled_name}" PARENT_SCOPE)
endfunction()
