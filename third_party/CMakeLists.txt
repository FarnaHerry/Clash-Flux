# Application-facing dependency layer: verified CPS packages + local module glue.
# The separate producer preserves each library's locked source/options; no upstream
# CMake targets are added to the application graph here.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/dependencies/Bootstrap.cmake")
clashflux_prepare_dependencies()
clashflux_find_cps(Glaze ${CLASHFLUX_DEP_glaze_VERSION} glaze)
clashflux_find_cps(nlohmann_json ${CLASHFLUX_DEP_JSON_VERSION} nlohmann_json)
clashflux_find_cps(QrCode ${CLASHFLUX_DEP_QRCODE_VERSION} qrcodegen)
clashflux_find_cps(zstd ${CLASHFLUX_DEP_zstd_VERSION} libzstd_static)
clashflux_find_cps(yaml-cpp ${CLASHFLUX_DEP_yamlcpp_VERSION} yaml-cpp)
clashflux_find_cps(CURL ${CLASHFLUX_DEP_curl_VERSION} libcurl)
clashflux_find_cps(IXWebSocket ${CLASHFLUX_DEP_ixwebsocket_VERSION} ixwebsocket)

# Keep Glaze templates private and outside codegen/exported module interfaces.
add_library(clashflux_wire STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../src/wire_codec.cpp")
target_compile_features(clashflux_wire PRIVATE cxx_std_23)
target_link_libraries(clashflux_wire PRIVATE Glaze::glaze)
set_target_properties(clashflux_wire PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(clashflux_wire PRIVATE /Zc:preprocessor /permissive- /Zc:lambda /utf-8)
endif()

# BMIs belong to the application's compiler/configuration, not to the CPS cache.
# Android keeps its ordinary-header Legacy adaptation of the same JSON package.
if(ANDROID)
    add_library(clashflux_json INTERFACE)
    target_link_libraries(clashflux_json INTERFACE nlohmann_json::nlohmann_json)
else()
    add_library(clashflux_json STATIC)
    target_sources(clashflux_json PUBLIC FILE_SET json_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nlohmann.json.cppm")
    set_target_properties(clashflux_json PROPERTIES CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
    target_link_libraries(clashflux_json PUBLIC nlohmann_json::nlohmann_json)
endif()

# HuxerUI and its extensions retain their codegen/resource/Android graph contract.
if(NOT CLASHFLUX_HUXERUI_FROM_SOURCE)
    set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
endif()
