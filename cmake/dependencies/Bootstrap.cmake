include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/Lock.cmake")

function(clashflux_cache_setting file name value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "$" "\\$" value "${value}")
    file(APPEND "${file}" "set(${name} \"${value}\" CACHE STRING \"\" FORCE)\n")
endfunction()

function(clashflux_package_fingerprint directory output)
    file(GLOB_RECURSE paths LIST_DIRECTORIES FALSE RELATIVE "${directory}" "${directory}/*")
    set(identity "")
    foreach(path IN LISTS paths)
        file(SHA256 "${directory}/${path}" digest)
        string(APPEND identity "${path}|${digest}\n")
    endforeach()
    string(SHA256 digest "${identity}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()

function(clashflux_dependencies_command log)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
        OUTPUT_FILE "${log}" ERROR_FILE "${log}")
    if(NOT result STREQUAL "0")
        file(READ "${log}" details)
        string(LENGTH "${details}" length)
        if(length GREATER 8000)
            math(EXPR offset "${length} - 8000")
            string(SUBSTRING "${details}" ${offset} -1 details)
        endif()
        message(FATAL_ERROR "CPS dependency command failed (${result}); ${log}\n${details}")
    endif()
endfunction()

function(clashflux_prepare_dependencies)
    clashflux_verify_dependency_inputs()
    set(CLASHFLUX_DEPENDENCY_CACHE_DIR "${CMAKE_BINARY_DIR}/dependencies" CACHE PATH
        "Cache of toolchain-specific CPS dependency packages")
    set(CLASHFLUX_DEPENDENCY_JOBS 4 CACHE STRING "Parallel jobs for the CPS library producer")
    if(NOT CLASHFLUX_DEPENDENCY_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "CLASHFLUX_DEPENDENCY_JOBS must be a positive integer")
    endif()
    set(recipe "${CLASHFLUX_DEPENDENCY_REPO}/cmake/dependencies")
    file(GLOB recipe_files CONFIGURE_DEPENDS "${recipe}/*.cmake" "${recipe}/*.cmake.in" "${recipe}/CMakeLists.txt")
    set(identity "CMake=${CMAKE_VERSION}\n")
    foreach(file IN LISTS recipe_files)
        file(SHA256 "${file}" digest)
        get_filename_component(name "${file}" NAME)
        string(APPEND identity "${name}=${digest}\n")
    endforeach()
    # Reuse requires the same compiler ABI, flags, configuration and SDK/NDK.
    set(settings CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES CMAKE_MAKE_PROGRAM
        CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_C_COMPILER_ARG1 CMAKE_CXX_COMPILER_ARG1
        CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN
        CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER
        CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
        CMAKE_OSX_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET
        CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR CMAKE_SYSTEM_VERSION
        CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        CMAKE_INTERPROCEDURAL_OPTIMIZATION CMAKE_C_VISIBILITY_PRESET CMAKE_CXX_VISIBILITY_PRESET
        CMAKE_VISIBILITY_INLINES_HIDDEN
        CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS CMAKE_PREFIX_PATH CMAKE_FIND_ROOT_PATH
        CMAKE_FIND_ROOT_PATH_MODE_PROGRAM CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
        CMAKE_FIND_ROOT_PATH_MODE_INCLUDE CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
        ANDROID_ABI ANDROID_PLATFORM ANDROID_NDK ANDROID_STL ANDROID_TOOLCHAIN
        ANDROID CMAKE_ANDROID_NDK CMAKE_ANDROID_ARCH_ABI CMAKE_ANDROID_API
        CMAKE_ANDROID_STL_TYPE IOS
        OPENSSL_ROOT_DIR OPENSSL_USE_STATIC_LIBS OPENSSL_MSVC_STATIC_RT
        OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    set(flag_configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES flag_configs)
    foreach(config IN LISTS flag_configs)
        string(TOUPPER "${config}" config)
        list(APPEND settings CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config})
        foreach(kind C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
            list(APPEND settings CMAKE_${kind}_FLAGS_${config})
        endforeach()
    endforeach()
    foreach(variable IN LISTS settings)
        if(DEFINED ${variable})
            string(APPEND identity "${variable}=${${variable}}\n")
        endif()
    endforeach()
    foreach(variable CMAKE_C_COMPILER_ID CMAKE_CXX_COMPILER_ID CMAKE_C_COMPILER_VERSION
            CMAKE_CXX_COMPILER_VERSION CMAKE_SIZEOF_VOID_P CMAKE_GENERATOR CMAKE_GENERATOR_PLATFORM
            CMAKE_GENERATOR_TOOLSET)
        string(APPEND identity "${variable}=${${variable}}\n")
    endforeach()
    foreach(path "${CMAKE_TOOLCHAIN_FILE}" "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
            "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h")
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(SHA256 "${path}" digest)
            string(APPEND identity "${path}=${digest}\n")
        endif()
    endforeach()
    string(SHA256 key "${identity}")
    # Keep Windows object/try-compile paths short; verify the full identity too.
    string(SUBSTRING "${key}" 0 24 directory_key)
    set(root "${CLASHFLUX_DEPENDENCY_CACHE_DIR}/${directory_key}")
    set(prefix "${root}/prefix")
    file(MAKE_DIRECTORY "${root}")
    file(LOCK "${root}/bootstrap.lock" GUARD FUNCTION TIMEOUT 120)
    set(ready FALSE)
    if(EXISTS "${root}/payload.sha256" AND EXISTS "${prefix}/lib/cps/CURL/CURL.cps"
            AND EXISTS "${root}/external-inputs.txt" AND EXISTS "${root}/identity.txt")
        file(READ "${root}/payload.sha256" expected)
        clashflux_package_fingerprint("${prefix}" actual)
        file(READ "${root}/identity.txt" recorded_identity)
        if(actual STREQUAL expected AND identity STREQUAL recorded_identity)
            set(ready TRUE)
        endif()
        file(STRINGS "${root}/external-inputs.txt" externals)
        foreach(record IN LISTS externals)
            string(FIND "${record}" "|" split)
            string(SUBSTRING "${record}" 0 ${split} expected)
            math(EXPR split "${split} + 1")
            string(SUBSTRING "${record}" ${split} -1 path)
            if(NOT EXISTS "${path}")
                set(ready FALSE)
                break()
            endif()
            file(SHA256 "${path}" actual)
            if(NOT actual STREQUAL expected)
                set(ready FALSE)
                break()
            endif()
        endforeach()
    endif()
    set(cache "${root}/toolchain.cmake")
    file(WRITE "${cache}" "# Generated toolchain settings for the CPS producer.\n")
    foreach(variable IN LISTS settings)
        # CMake treats explicitly setting SYSTEM_NAME as cross-compiling, even
        # when it equals the host. Let native producers detect their host normally.
        if(NOT CMAKE_CROSSCOMPILING AND variable MATCHES "^CMAKE_SYSTEM_(NAME|PROCESSOR|VERSION)$")
            continue()
        endif()
        if(DEFINED ${variable})
            clashflux_cache_setting("${cache}" "${variable}" "${${variable}}")
        endif()
    endforeach()
    clashflux_cache_setting("${cache}" CLASHFLUX_DEPENDENCY_SOURCE_CACHE "${CLASHFLUX_VENDOR_DIR}")
    clashflux_cache_setting("${cache}" CMAKE_INSTALL_PREFIX "${prefix}")
    if(NOT ready)
        message(STATUS "clash-flux: building locked CPS dependencies (${key})")
        set(build "${root}/build")
        set(generator -G "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND generator -A "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        if(CMAKE_GENERATOR_TOOLSET)
            list(APPEND generator -T "${CMAKE_GENERATOR_TOOLSET}")
        endif()
        # Only this producer's own cache is refreshed; sources/other builds remain intact.
        file(REMOVE "${build}/CMakeCache.txt")
        clashflux_dependencies_command("${root}/configure.log" "${CMAKE_COMMAND}"
            -S "${recipe}" -B "${build}" ${generator} -C "${cache}")
        if(CMAKE_CONFIGURATION_TYPES)
            set(configurations "${CMAKE_CONFIGURATION_TYPES}")
        else()
            set(configurations "${CMAKE_BUILD_TYPE}")
        endif()
        if(NOT configurations)
            set(configurations _CLASHFLUX_NOCONFIG)
        endif()
        set(staging "${root}/staging")
        file(REMOVE_RECURSE "${staging}")
        foreach(config IN LISTS configurations)
            if(config STREQUAL "_CLASHFLUX_NOCONFIG")
                set(config "")
            endif()
            clashflux_dependencies_command("${root}/build-${config}.log" "${CMAKE_COMMAND}"
                --build "${build}" --config "${config}" --target clashflux_dependency_payload
                --parallel "${CLASHFLUX_DEPENDENCY_JOBS}")
            clashflux_dependencies_command("${root}/install-${config}.log" "${CMAKE_COMMAND}"
                --install "${build}" --config "${config}" --component ClashFluxDependencies --prefix "${staging}")
        endforeach()
        clashflux_package_fingerprint("${staging}" payload)
        file(REMOVE_RECURSE "${root}/previous")
        if(EXISTS "${prefix}")
            file(RENAME "${prefix}" "${root}/previous")
        endif()
        file(RENAME "${staging}" "${prefix}")
        configure_file("${build}/external-inputs.txt" "${root}/external-inputs.txt" COPYONLY)
        file(WRITE "${root}/payload.sha256" "${payload}")
        file(WRITE "${root}/identity.txt" "${identity}")
        file(REMOVE_RECURSE "${root}/previous")
    else()
        message(STATUS "clash-flux: reusing verified CPS dependencies (${key})")
    endif()
    set(CLASHFLUX_DEPENDENCY_PREFIX "${prefix}" CACHE INTERNAL "Verified CPS package prefix" FORCE)
    set(CLASHFLUX_DEPENDENCY_TOOLCHAIN "${cache}" CACHE INTERNAL "Producer toolchain settings" FORCE)
endfunction()

# Bind the exact package from our verified prefix, never an ambient version.
macro(clashflux_find_cps package version component)
    # *_DIR is an explicit hint that otherwise survives even NO_DEFAULT_PATH.
    # Rebind it when the toolchain/options/cache identity changes.
    set(${package}_DIR "${CLASHFLUX_DEPENDENCY_PREFIX}/lib/cps/${package}" CACHE PATH
        "Locked CPS package directory" FORCE)
    find_package(${package} ${version} EXACT CONFIG REQUIRED GLOBAL COMPONENTS ${component}
        PATHS "${CLASHFLUX_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    cmake_path(IS_PREFIX CLASHFLUX_DEPENDENCY_PREFIX "${${package}_CONFIG}" NORMALIZE inside)
    if(NOT inside OR NOT "${${package}_CONFIG}" MATCHES "\\.cps$")
        message(FATAL_ERROR "${package} was not imported from a CPS package")
    endif()
endmacro()
