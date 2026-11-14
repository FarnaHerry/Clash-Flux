# Immutable package inputs. Archive contents are checked even on a warm cache.
include_guard(GLOBAL)
get_filename_component(CLASHFLUX_DEPENDENCY_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
macro(clashflux_lock_archive name version archive sha256 topdir)
    set(CLASHFLUX_DEP_${name}_VERSION "${version}")
    set(CLASHFLUX_DEP_${name}_ARCHIVE "${CLASHFLUX_DEPENDENCY_REPO}/third_party/tarballs/${archive}")
    set(CLASHFLUX_DEP_${name}_SHA256 "${sha256}")
    set(CLASHFLUX_DEP_${name}_TOPDIR "${topdir}")
endmacro()
clashflux_lock_archive(glaze 9.1.0 glaze-9.1.0-headers.tar.gz
    9bb4c9897e8b0f0d6b66b3ee4c1519efbf4288a4c331fd382d8c9412e67a244a glaze-9.1.0)
clashflux_lock_archive(zstd 1.5.7 zstd-1.5.7.tar.gz
    37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3 zstd-1.5.7)
clashflux_lock_archive(yamlcpp 0.9.0 yaml-cpp-0.9.0.tar.gz
    25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a yaml-cpp-yaml-cpp-0.9.0)
clashflux_lock_archive(curl 8.23.0 curl-8.23.0.tar.gz
    e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49 curl-8.23.0)
clashflux_lock_archive(ixwebsocket 12.1.0 ixwebsocket-12.1.0.tar.gz
    55f3a5d3a1a2c5f19d737c24e6aced28d1200f8f5deb45ecd8e5d46f765b5288 IXWebSocket-12.1.0)
clashflux_lock_archive(huxerui 0.4.0 huxerui-sdk-0.4.0-linux-x86_64.tar.gz
    86837f3e872fe6716464d1804c10d894957742a6616a8d2977ea93e630799583 huxerui-sdk-0.4.0-linux-x86_64)
clashflux_lock_archive(openssl 3.5.1 openssl-3.5.1-linux-x86_64.tar.gz
    d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf openssl-3.5.1-linux-x86_64)
set(CLASHFLUX_DEP_JSON_VERSION 3.12.0)
set(CLASHFLUX_DEPENDENCY_ARCHIVES glaze zstd yamlcpp curl ixwebsocket)
set(CLASHFLUX_DEPENDENCY_FILES
    "third_party/json/nlohmann/json.hpp|aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")

# Framework extensions still use the framework's codegen/resource/Android graph.
set(CLASHFLUX_HUXERUI_CAMERA_URL "https://github.com/FarnaHerry/Lib-Camera.git")
set(CLASHFLUX_HUXERUI_CAMERA_COMMIT "5d6b8e814e3e5d8c22e63a4e470c2b6c1cd82cd0")
set(CLASHFLUX_HUXERUI_SQLITE_URL "https://github.com/FarnaHerry/Lib-SQLite.git")
set(CLASHFLUX_HUXERUI_SQLITE_COMMIT "882665a9772cc973b1cbfb7f491913703da1d636")
set(CLASHFLUX_HUXERUI_CHARTS_URL "https://github.com/FarnaHerry/Lib-Charts.git")
set(CLASHFLUX_HUXERUI_CHARTS_COMMIT "2251bab5ad2a5df8e6e133c039e0b76bbe237add")
set(CLASHFLUX_HUXERUI_CODEEDITOR_URL "https://github.com/FarnaHerry/Lib-CodeEditor.git")
set(CLASHFLUX_HUXERUI_CODEEDITOR_COMMIT "a332e7c3158818a891bb8181756d439a62df6268")
set(CLASHFLUX_HUXERUI_MEDIA_URL "https://github.com/HuxerUI/Lib-Media.git")
set(CLASHFLUX_HUXERUI_MEDIA_COMMIT "fbe5b7e652f8cb79191444fa2a51d0dd0c2299bb")
set(CLASHFLUX_HUXERUI_QRCODE_URL "https://github.com/HuxerUI/Lib-QRCode.git")
set(CLASHFLUX_HUXERUI_QRCODE_COMMIT "23bc79b19a6a6d104efc00bfa3934cd7ab78b08f")

function(clashflux_dependency_source name)
    clashflux_extract(${name} "${CLASHFLUX_DEP_${name}_ARCHIVE}"
        "${CLASHFLUX_DEP_${name}_SHA256}" "${CLASHFLUX_DEP_${name}_TOPDIR}")
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(clashflux_verify_dependency_inputs)
    foreach(name IN LISTS CLASHFLUX_DEPENDENCY_ARCHIVES)
        file(SHA256 "${CLASHFLUX_DEP_${name}_ARCHIVE}" actual)
        if(NOT actual STREQUAL CLASHFLUX_DEP_${name}_SHA256)
            message(FATAL_ERROR "Dependency archive SHA256 mismatch: ${name}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CLASHFLUX_DEP_${name}_ARCHIVE}")
    endforeach()
    foreach(record IN LISTS CLASHFLUX_DEPENDENCY_FILES)
        string(REPLACE "|" ";" fields "${record}")
        list(GET fields 0 path)
        list(GET fields 1 expected)
        file(SHA256 "${CLASHFLUX_DEPENDENCY_REPO}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Dependency file SHA256 mismatch: ${path}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CLASHFLUX_DEPENDENCY_REPO}/${path}")
    endforeach()
endfunction()
