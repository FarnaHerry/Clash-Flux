# mihomo_bundle.cmake — 自带 mihomo 内核（多平台）。
#
# clashflux_bundle_mihomo(<target>)：configure 期按宿主平台下载官方 release
# （版本与 SHA256 钉死），POST_BUILD 把可执行文件拷到 <exe>/engines/mihomo
# （Windows 为 mihomo.exe —— cfg::mihomoBinary() 的第一优先级解析位），
# 构建产物开箱即可启动内核。-DCLASHFLUX_BUNDLE_MIHOMO=OFF 关闭（回落
# <repo>/engines 与 PATH 解析）。
#
# 平台资产表（v1.19.30，https://github.com/MetaCubeX/mihomo/releases）：
#   linux   x86_64  mihomo-linux-amd64-v1.19.30.gz      （gzip 单文件）
#   windows x86_64  mihomo-windows-amd64-v1.19.30.zip   （内含 mihomo-windows-amd64.exe）
#   darwin  x86_64  mihomo-darwin-amd64-v1.19.30.gz
#   darwin  arm64   mihomo-darwin-arm64-v1.19.30.gz
# 新增资产/升版本时：curl -sSfL 下载后 sha256sum 钉进下表。

option(CLASHFLUX_BUNDLE_MIHOMO
       "Download and bundle the mihomo kernel next to the executable" ON)

function(clashflux_bundle_mihomo target)
    if (NOT CLASHFLUX_BUNDLE_MIHOMO)
        return()
    endif ()

    set(CLASHFLUX_MIHOMO_VERSION "v1.19.30")
    set(_base_url "https://github.com/MetaCubeX/mihomo/releases/download/${CLASHFLUX_MIHOMO_VERSION}")

    # ---- 平台资产选择 -----------------------------------------------------
    set(_asset "")
    set(_sha "")
    set(_format "")  # gz | zip
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "mihomo-linux-amd64-${CLASHFLUX_MIHOMO_VERSION}.gz")
        set(_sha "cf06ce2c7d1421bdbda14ee4a5b6046672dc35ebf8eecd8e77504ec3c0ed9a84")
        set(_format "gz")
    elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "mihomo-windows-amd64-${CLASHFLUX_MIHOMO_VERSION}.zip")
        set(_sha "22c09fd67673895ef7cd6b1820563918275c3d316f2462b306208675118db3c0")
        set(_format "zip")
    elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        set(_asset "mihomo-darwin-amd64-${CLASHFLUX_MIHOMO_VERSION}.gz")
        set(_sha "99dfcfe454ed58fb95ee4ba222c39defd051b687ad3e5deabb1b9d6be3103e2f")
        set(_format "gz")
    elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
        set(_asset "mihomo-darwin-arm64-${CLASHFLUX_MIHOMO_VERSION}.gz")
        set(_sha "2c7f3a7904fa1cee291e124123e630e7b1ebd13765dd9bf26c0a28432004d9f4")
        set(_format "gz")
    else ()
        message(FATAL_ERROR
            "CLASHFLUX_BUNDLE_MIHOMO 暂无 ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} 的预置包；"
            "请 -DCLASHFLUX_BUNDLE_MIHOMO=OFF 并自行放置 engines/mihomo")
    endif ()

    # 解包后的规范名（ engines/ 里的拷出名统一为 mihomo / mihomo.exe ）。
    set(CLASHFLUX_MIHOMO_DIR "${CMAKE_BINARY_DIR}/vendor/mihomo")
    if (WIN32)
        set(_bin "${CLASHFLUX_MIHOMO_DIR}/mihomo-${CLASHFLUX_MIHOMO_VERSION}.exe")
        set(_bundled_name "mihomo.exe")
    else ()
        set(_bin "${CLASHFLUX_MIHOMO_DIR}/mihomo-${CLASHFLUX_MIHOMO_VERSION}")
        set(_bundled_name "mihomo")
    endif ()

    # ---- 下载 + 解包（幂等：规范名已存在则跳过）----------------------------
    if (NOT EXISTS "${_bin}")
        file(MAKE_DIRECTORY "${CLASHFLUX_MIHOMO_DIR}")
        set(_archive "${CLASHFLUX_MIHOMO_DIR}/${_asset}")
        if (NOT EXISTS "${_archive}")
            message(STATUS "clash-flux: 下载自带内核 mihomo ${CLASHFLUX_MIHOMO_VERSION}（${_asset}）")
            file(DOWNLOAD "${_base_url}/${_asset}" "${_archive}"
                EXPECTED_HASH "SHA256=${_sha}"
                STATUS _dl_status)
            list(GET _dl_status 0 _dl_code)
            if (NOT _dl_code EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "mihomo 下载失败：${_dl_status}")
            endif ()
        endif ()
        if (_format STREQUAL "zip")
            # zip 用 cmake -E tar 解（libarchive 直读 zip，无需外部工具）；
            # 内含固定名 mihomo-windows-amd64.exe，解出后改名为规范名。
            execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_archive}"
                WORKING_DIRECTORY "${CLASHFLUX_MIHOMO_DIR}"
                RESULT_VARIABLE _tar_result)
            if (NOT _tar_result EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "mihomo 解压失败（cmake -E tar 退出码 ${_tar_result}）")
            endif ()
            file(RENAME "${CLASHFLUX_MIHOMO_DIR}/mihomo-windows-amd64.exe" "${_bin}")
        else ()
            find_program(CLASHFLUX_GZIP gzip REQUIRED)
            execute_process(COMMAND "${CLASHFLUX_GZIP}" -dkf "${_archive}"
                WORKING_DIRECTORY "${CLASHFLUX_MIHOMO_DIR}"
                RESULT_VARIABLE _gzip_result)
            if (NOT _gzip_result EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "mihomo 解压失败（gzip -d 退出码 ${_gzip_result}）")
            endif ()
            # gzip 解出的是资产原名（mihomo-<平台>-<版本>），改名为规范名。
            get_filename_component(_extracted "${_asset}" NAME_WLE)
            file(RENAME "${CLASHFLUX_MIHOMO_DIR}/${_extracted}" "${_bin}")
            file(CHMOD "${_bin}" PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)
        endif ()
    endif ()

    # ---- POST_BUILD 拷到 <exe>/engines/ -----------------------------------
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/engines"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_bin}" "$<TARGET_FILE_DIR:${target}>/engines/${_bundled_name}"
        COMMENT "Bundling mihomo ${CLASHFLUX_MIHOMO_VERSION} into engines/")
endfunction()
