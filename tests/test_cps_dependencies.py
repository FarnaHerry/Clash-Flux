"""Consume real relocated CPS packages and reject bad versions/warm archive tampering."""
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import hashlib


def run(args, *, success=True, env=None):
    result = subprocess.run(args, capture_output=True, text=True, timeout=90, env=env)
    if (result.returncode == 0) != success:
        raise AssertionError(f"{' '.join(map(str, args))}\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def check_system_configurations(cmake, root, repo):
    """Round-trip config-specific system artifacts through native CPS export/import."""
    project = root / "system-provider"
    project.mkdir()
    for name in ("debug", "release", "unconfigured"):
        project.joinpath(name + ".lib").write_bytes(b"system artifact fixture")
    project.joinpath("curl.c").write_text("int cps_fixture(void) { return 0; }\n")
    helper = (repo / "cmake/dependencies/SystemComponents.cmake").as_posix()
    project.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 4.4)
project(SystemProvider LANGUAGES C)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
include("{helper}")
add_library(Test::Library UNKNOWN IMPORTED)
set_target_properties(Test::Library PROPERTIES IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_LOCATION_DEBUG "${{CMAKE_CURRENT_SOURCE_DIR}}/debug.lib"
    IMPORTED_LOCATION_RELEASE "${{CMAKE_CURRENT_SOURCE_DIR}}/release.lib"
    MAP_IMPORTED_CONFIG_DEBUG "debug")
add_library(Test::Fallback UNKNOWN IMPORTED)
set_target_properties(Test::Fallback PROPERTIES IMPORTED_CONFIGURATIONS RELEASE
    IMPORTED_LOCATION "${{CMAKE_CURRENT_SOURCE_DIR}}/unconfigured.lib"
    IMPORTED_LOCATION_RELEASE "${{CMAKE_CURRENT_SOURCE_DIR}}/release.lib"
    MAP_IMPORTED_CONFIG_DEBUG "" MAP_IMPORTED_CONFIG_RELEASE "")
clashflux_cps_system_component(Test::Library library)
clashflux_cps_system_component(Test::Fallback fallback)
add_library(libcurl STATIC curl.c)
install(TARGETS libcurl ${{library}} ${{fallback}} EXPORT SystemTargets
    ARCHIVE DESTINATION "lib/$<CONFIG>" COMPONENT ClashFluxDependencies)
install(PACKAGE_INFO CURL EXPORT SystemTargets VERSION 1.0.0 DESTINATION lib/cps/CURL
    COMPONENT ClashFluxDependencies)
clashflux_install_system_configurations()
''', encoding="utf-8")
    build = root / "system-provider-build"
    prefix = root / "system packages"
    run([cmake, "-S", str(project), "-B", str(build), "-G", "Ninja Multi-Config"])
    for config in ("Debug", "Release"):
        run([cmake, "--build", str(build), "--config", config, "--target", "libcurl", "--parallel", "2"])
        run([cmake, "--install", str(build), "--config", config, "--prefix", str(prefix),
             "--component", "ClashFluxDependencies"])
    consumer = root / "system-consumer"
    consumer.mkdir()
    consumer.joinpath("CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.4)
project(SystemConsumer LANGUAGES NONE)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
find_package(CURL 1.0.0 EXACT CONFIG REQUIRED COMPONENTS system_Test__Library system_Test__Fallback
    PATHS "${PACKAGE_PREFIX}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
foreach(component Library Fallback)
    set(target CURL::system_Test__${component})
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/${component}-$<CONFIG>.txt"
        CONTENT "$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},INTERFACE_LINK_LIBRARIES>>")
endforeach()
''', encoding="utf-8")
    build = root / "system-consumer-build"
    run([cmake, "-S", str(consumer), "-B", str(build), "-G", "Ninja Multi-Config",
         "-DPACKAGE_PREFIX=" + prefix.as_posix()])
    for config in ("Debug", "Release"):
        assert build.joinpath(f"Library-{config}.txt").read_text().strip(";") == project.joinpath(config.lower() + ".lib").as_posix()
        assert build.joinpath(f"Fallback-{config}.txt").read_text().strip(";") == project.joinpath("unconfigured.lib").as_posix()


def main():
    cmake, prefix, cache, repo, config = sys.argv[1:]
    repo = pathlib.Path(repo).resolve()
    with tempfile.TemporaryDirectory(prefix="clash-flux-cps-") as temp:
        root = pathlib.Path(temp)
        check_system_configurations(cmake, root, repo)
        relocated = root / "relocated packages"
        shutil.copytree(prefix, relocated)
        project = root / "consumer"
        project.mkdir()
        bootstrap = (repo / "cmake/dependencies/Bootstrap.cmake").as_posix()
        project.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 4.4)
project(CpsConsumer LANGUAGES CXX)
include("{bootstrap}")
if(REJECT_VERSION)
    find_package(Glaze 999.0.0 EXACT CONFIG REQUIRED PATHS "${{CLASHFLUX_DEPENDENCY_PREFIX}}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
clashflux_find_cps(Glaze 9.0.0 glaze)
clashflux_find_cps(nlohmann_json 3.12.0 nlohmann_json)
clashflux_find_cps(QrCode 1.8.0 qrcodegen)
clashflux_find_cps(zstd 1.5.7 libzstd_static)
clashflux_find_cps(yaml-cpp 0.8.0 yaml-cpp)
clashflux_find_cps(CURL 8.22.0 libcurl)
clashflux_find_cps(IXWebSocket 12.0.1 ixwebsocket)
get_target_property(curl_defs CURL::libcurl INTERFACE_COMPILE_DEFINITIONS)
get_target_property(yaml_defs yaml-cpp::yaml-cpp INTERFACE_COMPILE_DEFINITIONS)
if(NOT "CURL_STATICLIB" IN_LIST curl_defs OR NOT "YAML_CPP_STATIC_DEFINE" IN_LIST yaml_defs)
    message(FATAL_ERROR "Static usage requirements were lost")
endif()
foreach(target QrCode::qrcodegen zstd::libzstd_static yaml-cpp::yaml-cpp CURL::libcurl IXWebSocket::ixwebsocket)
    string(TOUPPER "${{CMAKE_BUILD_TYPE}}" config)
    get_target_property(location ${{target}} IMPORTED_LOCATION_${{config}})
    cmake_path(IS_PREFIX CLASHFLUX_DEPENDENCY_PREFIX "${{location}}" NORMALIZE inside)
    if(NOT inside OR NOT EXISTS "${{location}}")
        message(FATAL_ERROR "Artifact did not relocate: ${{target}} ${{location}}")
    endif()
endforeach()
add_executable(cps_consumer main.cpp)
target_link_libraries(cps_consumer PRIVATE Glaze::glaze nlohmann_json::nlohmann_json
    QrCode::qrcodegen zstd::libzstd_static yaml-cpp::yaml-cpp CURL::libcurl IXWebSocket::ixwebsocket)
file(WRITE "${{CMAKE_BINARY_DIR}}/runtime-path.txt" "${{OPENSSL_ROOT_DIR}}/bin")
''', encoding="utf-8")
        project.joinpath("main.cpp").write_text('''#include <glaze/glaze.hpp>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <qrcodegen.hpp>
#include <zstd.h>
#include <curl/curl.h>
#include <ixwebsocket/IXWebSocket.h>
#include <string_view>
struct Sample { int value = 0; };
int main() {
    Sample sample;
    if (glz::read_json(sample, std::string_view{R"({"value":7})"}) || sample.value != 7) return 1;
    if (nlohmann::json::parse(R"({"value":7})")["value"] != 7) return 2;
    if (YAML::Load("value: 7")["value"].as<int>() != 7) return 3;
    if (qrcodegen::QrCode::encodeText("CPS", qrcodegen::QrCode::Ecc::MEDIUM).getSize() <= 0) return 4;
    char compressed[128], restored[16];
    const auto size = ZSTD_compress(compressed, sizeof(compressed), "CPS", 3, 1);
    if (ZSTD_isError(size) || ZSTD_decompress(restored, sizeof(restored), compressed, size) != 3) return 5;
    if (std::string_view(restored, 3) != "CPS" || ZSTD_versionNumber() != 10507) return 6;
    if (curl_version_info(CURLVERSION_NOW)->version_num != 0x081600) return 7;
    auto* curl = curl_easy_init();
    if (!curl) return 8;
    curl_easy_cleanup(curl);
    ix::WebSocket websocket;
    return websocket.getUrl().empty() ? 0 : 9;
}
''', encoding="utf-8")
        build = root / "consumer-build"
        args = [cmake, "-S", str(project), "-B", str(build), "-G", "Ninja", "-C", cache,
                f"-DCLASHFLUX_DEPENDENCY_PREFIX={relocated.as_posix()}", f"-DCMAKE_BUILD_TYPE={config}", f"-DGlaze_DIR={pathlib.Path(prefix).as_posix()}/lib/cps/Glaze",
                f"-DCURL_DIR={pathlib.Path(prefix).as_posix()}/lib/cps/CURL"]
        run(args)
        run([cmake, "--build", str(build), "--parallel", "2"])
        executable = build / ("cps_consumer.exe" if os.name == "nt" else "cps_consumer")
        env = os.environ.copy()
        runtime = build.joinpath("runtime-path.txt").read_text(encoding="utf-8")
        env["PATH"] = runtime + os.pathsep + env.get("PATH", "")
        run([str(executable)], env=env)
        rejected = run(args + ["-DREJECT_VERSION=ON"], success=False)
        assert "999.0.0" in rejected and "Glaze" in rejected, rejected

        # Production extraction must verify bytes even when the old SHA marker
        # and extracted source directory already exist. Failure preserves the cache.
        fixture = root / "fixture"
        fixture.mkdir()
        source = fixture / "source" / "fixture-v1"
        source.mkdir(parents=True)
        source.joinpath("payload.txt").write_text("keep", encoding="utf-8")
        archive = fixture / "fixture.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(source, arcname="fixture-v1")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        vendor = (repo / "cmake/clashflux_vendor.cmake").as_posix()
        fixture.joinpath("CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 4.4)
project(ArchiveIntegrity LANGUAGES NONE)
include("{vendor}")
clashflux_extract(fixture "${{CMAKE_CURRENT_SOURCE_DIR}}/fixture.tar.gz" "{digest}" fixture-v1)
''', encoding="utf-8")
        fixture_build = fixture / "build"
        args = [cmake, "-S", str(fixture), "-B", str(fixture_build), "-G", "Ninja"]
        run(args)
        payload = fixture_build / "vendor/fixture/fixture-v1/payload.txt"
        assert payload.read_text(encoding="utf-8") == "keep"
        archive.write_bytes(b"tampered archive")
        rejected = run(args, success=False)
        assert "SHA256 mismatch" in rejected and payload.read_text(encoding="utf-8") == "keep", rejected
    print("cps_dependencies: system configurations, seven relocated packages, static definitions, linked runtime, version rejection and warm-cache integrity passed")


if __name__ == "__main__":
    main()
