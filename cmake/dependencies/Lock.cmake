# Immutable package inputs. Archive contents are checked even on a warm cache.
include_guard(GLOBAL)
get_filename_component(CLASHFLUX_DEPENDENCY_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
macro(clashflux_lock_archive name version archive sha256 topdir)
    set(CLASHFLUX_DEP_${name}_VERSION "${version}")
    set(CLASHFLUX_DEP_${name}_ARCHIVE "${CLASHFLUX_DEPENDENCY_REPO}/third_party/tarballs/${archive}")
    set(CLASHFLUX_DEP_${name}_SHA256 "${sha256}")
    set(CLASHFLUX_DEP_${name}_TOPDIR "${topdir}")
endmacro()
clashflux_lock_archive(glaze 9.0.0 glaze-9.0.0-headers.tar.gz
    872764412db2ba94cf57e39cf6518f86c0e13728c1c668f7ed398801aadff812 glaze-9.0.0)
clashflux_lock_archive(zstd 1.5.7 zstd-1.5.7.tar.gz
    37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3 zstd-1.5.7)
clashflux_lock_archive(yamlcpp 0.8.0 yaml-cpp-0.8.0.tar.gz
    fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16 yaml-cpp-0.8.0)
clashflux_lock_archive(curl 8.22.0 curl-8.22.0.tar.gz
    d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1 curl-8.22.0)
clashflux_lock_archive(ixwebsocket 12.0.1 ixwebsocket-12.0.1.tar.gz
    d23bdc91dbfe2b9ae13c322d539392d7a6b8b506560f41c90e227fa0f86a2405 IXWebSocket-12.0.1)
clashflux_lock_archive(huxerui 0.3.0 huxerui-sdk-0.3.0-linux-x86_64.tar.gz
    5413ccd9d35d0ad50c08c67045d73cb9faff23c04005a554af0faa7a914eb916 huxerui-sdk-0.3.0-linux-x86_64)
clashflux_lock_archive(openssl 3.5.1 openssl-3.5.1-linux-x86_64.tar.gz
    d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf openssl-3.5.1-linux-x86_64)
set(CLASHFLUX_DEP_JSON_VERSION 3.12.0)
set(CLASHFLUX_DEP_QRCODE_VERSION 1.8.0)
set(CLASHFLUX_DEPENDENCY_ARCHIVES glaze zstd yamlcpp curl ixwebsocket)
set(CLASHFLUX_DEPENDENCY_FILES
    "third_party/json/nlohmann/json.hpp|aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"
    "third_party/qrcodegen/qrcodegen.hpp|b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793"
    "third_party/qrcodegen/qrcodegen.cpp|1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757")

# Framework extensions still use the framework's codegen/resource/Android graph.
set(CLASHFLUX_HUXERUI_CAMERA_URL "https://github.com/FarnaHerry/Lib-Camera.git")
set(CLASHFLUX_HUXERUI_CAMERA_COMMIT "b8b3b9304646cdc1afb9722ead16d85cf2160610")
set(CLASHFLUX_HUXERUI_SQLITE_URL "https://github.com/FarnaHerry/Lib-SQLite.git")
set(CLASHFLUX_HUXERUI_SQLITE_COMMIT "88f610fe66c57260b1ac38e676fc36b23c900fc2")
set(CLASHFLUX_HUXERUI_CHARTS_URL "https://github.com/FarnaHerry/Lib-Charts.git")
set(CLASHFLUX_HUXERUI_CHARTS_COMMIT "9c183e87393ef589f3bf20dd8d80281a1fe9c9cd")

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
