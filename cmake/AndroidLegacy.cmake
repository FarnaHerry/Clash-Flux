# AndroidLegacy.cmake — 将 Clash-Flux 的模块源码适配到 Android NDK。
#
# Android NDK 的 Clang 尚未提供本项目桌面构建所依赖的 CMake `import std`
# 集成（也没有可供 CMAKE_CXX_MODULE_STD 使用的 __CMAKE::CXX23）。Android
# 仍然使用同一套领域代码：这里在 configure 期生成一个只用于 Android 的
# 兼容头和实现单元，桌面目标完全不经过本文件。
include_guard(GLOBAL)
set(CLASHFLUX_ANDROID_LEGACY_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

function(clashflux_prepare_android_sources output_directory output_header
        output_core output_ui)
    set(_root "${CLASHFLUX_ANDROID_LEGACY_REPO_ROOT}")
    file(MAKE_DIRECTORY "${output_directory}/ui")

    # `import std;` 在模块接口里承担了统一标准库包含的职责；兼容头显式包含
    # 这些头，避免依赖 NDK libc++ 的传递包含行为。
    set(_header "#pragma once\n")
    string(APPEND _header
        "#include <algorithm>\n"
        "#include <array>\n"
        "#include <atomic>\n"
        "#include <chrono>\n"
        "#include <charconv>\n"
        "#include <condition_variable>\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "#include <cstdio>\n"
        "#include <cstdlib>\n"
        "#include <ctime>\n"
        "#include <filesystem>\n"
        "#include <format>\n"
        "#include <fstream>\n"
        "#include <functional>\n"
        "#include <iomanip>\n"
        "#include <iostream>\n"
        "#include <limits>\n"
        "#include <map>\n"
        "#include <memory>\n"
        "#include <mutex>\n"
        "#include <optional>\n"
        "#include <queue>\n"
        "#include <random>\n"
        "#include <ranges>\n"
        "#include <span>\n"
        "#include <sstream>\n"
        "#include <stdexcept>\n"
        "#include <string>\n"
        "#include <string_view>\n"
        "#include <system_error>\n"
        "#include <thread>\n"
        "#include <tuple>\n"
        "#include <type_traits>\n"
        "#include <unordered_map>\n"
        "#include <utility>\n"
        "#include <vector>\n"
        "#include <nlohmann/json.hpp>\n"
        "#include <qrcodegen.hpp>\n\n"
    )

    # The interface units are included as declarations/inline utility definitions.
    # Classes and structs are type declarations; free functions defined in an
    # interface are made inline so every UI codegen TU can include this header.
    set(_interfaces
        "${_root}/src/config.cppm"
        "${_root}/src/utils.cppm"
        "${_root}/src/db.cppm"
        "${_root}/src/core.cppm"
        "${_root}/src/api.cppm"
        "${_root}/src/stream.cppm"
        "${_root}/src/sysproxy.cppm"
        "${_root}/src/service.cppm"
        "${_root}/src/store/core_store.cppm"
        "${_root}/src/store/profiles.cppm"
    )
    set(_android_legacy_inputs
        ${_interfaces}
        "${_root}/src/db.cpp"
        "${_root}/src/core.cpp"
        "${_root}/src/api.cpp"
        "${_root}/src/stream.cpp"
        "${_root}/src/ui/app.cpp"
        "${_root}/src/ui/common.cpp"
        "${_root}/src/ui/connections_page.cpp"
        "${_root}/src/ui/home_page.cpp"
        "${_root}/src/ui/logs_page.cpp"
        "${_root}/src/ui/profiles_page.cpp"
        "${_root}/src/ui/proxies_page.cpp"
        "${_root}/src/ui/rules_page.cpp"
        "${_root}/src/ui/settings_page.cpp"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_android_legacy_inputs}
    )
    foreach (_source IN LISTS _interfaces)
        file(READ "${_source}" _content)
        string(REPLACE "module;\n" "" _content "${_content}")
        string(REGEX REPLACE "export module [A-Za-z0-9_.]+;" "" _content "${_content}")
        string(REGEX REPLACE "import [A-Za-z0-9_.]+;" "" _content "${_content}")
        string(REPLACE "export struct" "struct" _content "${_content}")
        string(REPLACE "export class" "class" _content "${_content}")
        string(REPLACE "export enum" "enum" _content "${_content}")
        string(REGEX REPLACE "export ([^;{}]*\\{)" "inline \\1" _content "${_content}")
        string(REPLACE "export " "" _content "${_content}")
        string(APPEND _header "// Generated from ${_source}.\n${_content}\n")
    endforeach()
    set(_header_path "${output_directory}/clashflux_android_legacy.h")
    file(WRITE "${_header_path}" "${_header}")

    # Implementations are placed after the interface header in a single TU. This
    # retains the original module dependency order while avoiding duplicate
    # non-inline implementation symbols across Android UI TUs.
    set(_core "#include \"clashflux_android_legacy.h\"\n\n")
    foreach (_source IN ITEMS
            "${_root}/src/db.cpp"
            "${_root}/src/core.cpp"
            "${_root}/src/api.cpp"
            "${_root}/src/stream.cpp")
        file(READ "${_source}" _content)
        string(REPLACE "module;\n" "" _content "${_content}")
        string(REGEX REPLACE "module [A-Za-z0-9_.]+;" "" _content "${_content}")
        string(REGEX REPLACE "import [A-Za-z0-9_.]+;" "" _content "${_content}")
        string(APPEND _core "// Generated from ${_source}.\n${_content}\n")
    endforeach()
    set(_core_path "${output_directory}/clashflux_android_core.cpp")
    file(WRITE "${_core_path}" "${_core}")

    # HuxerUI codegen must see real .cpp inputs. Each Android copy has module
    # imports removed and includes the compatibility declarations above.
    set(_ui_sources)
    foreach (_source IN ITEMS
            "${_root}/src/ui/app.cpp"
            "${_root}/src/ui/common.cpp"
            "${_root}/src/ui/connections_page.cpp"
            "${_root}/src/ui/home_page.cpp"
            "${_root}/src/ui/logs_page.cpp"
            "${_root}/src/ui/profiles_page.cpp"
            "${_root}/src/ui/proxies_page.cpp"
            "${_root}/src/ui/rules_page.cpp"
            "${_root}/src/ui/settings_page.cpp")
        file(READ "${_source}" _content)
        string(REGEX REPLACE "import [A-Za-z0-9_.]+;" "" _content "${_content}")
        string(PREPEND _content "#include \"clashflux_android_legacy.h\"\n")
        get_filename_component(_name "${_source}" NAME)
        set(_ui_path "${output_directory}/ui/${_name}")
        file(WRITE "${_ui_path}" "${_content}")
        list(APPEND _ui_sources "${_ui_path}")
    endforeach()

    set(${output_header} "${_header_path}" PARENT_SCOPE)
    set(${output_core} "${_core_path}" PARENT_SCOPE)
    set(${output_ui} "${_ui_sources}" PARENT_SCOPE)
endfunction()
