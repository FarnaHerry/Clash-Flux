set(HUXERUI_WINDOWS_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/app.manifest")
set(HUXERUI_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/assets/icon.ico")
set(HUXERUI_WINDOWS_RESOURCE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/huxerui-platform/windows"
)
file(MAKE_DIRECTORY "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}")
configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/app.rc.in"
        "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}/app.rc"
        @ONLY
)
set(HUXERUI_WINDOWS_RESOURCE
        "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}/app.rc"
)

function(huxerui_configure_windows_project_package target_name install_component)
    if (NOT HUXERUI_PACKAGE)
        return()
    endif ()

    install(TARGETS ${target_name}
            RUNTIME DESTINATION .
            COMPONENT "${install_component}"
    )
    # 将 HuxerUI、OpenSSL 和 MSVC Release runtime 一并部署到安装目录。
    # 只安装主 exe 会导致 Windows 上启动时找不到 huxerui.dll 等运行时依赖。
    # OpenSSL 的 imported target 不总是向 CMake runtime scanner 暴露 DLL
    # 所在目录，因此把实际 DLL 和搜索路径显式注册进去。
    if (WIN32 AND OPENSSL_ROOT_DIR)
        file(GLOB HUXERUI_OPENSSL_RUNTIME_FILES
            LIST_DIRECTORIES FALSE
            "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll"
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll"
        )
        # 即使 GLOB 因 OpenSSL 版本命名变化没有命中，也必须保留目录；
        # runtime scanner 会按 clash-flux.exe 的实际导入名在这里解析 DLL。
        huxerui_add_runtime_dependencies(${target_name}
            FILES ${HUXERUI_OPENSSL_RUNTIME_FILES}
            SEARCH_DIRECTORIES "${OPENSSL_ROOT_DIR}/bin"
        )
    endif ()
    _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
            . "$<TARGET_FILE_NAME:${target_name}>"
    )
    get_target_property(HUXERUI_WINDOWS_APP_RESOURCES
            ${target_name}
            HUXERUI_RESOURCE_PACKAGE
    )
    if (HUXERUI_WINDOWS_APP_RESOURCES
            AND NOT HUXERUI_WINDOWS_APP_RESOURCES MATCHES "-NOTFOUND$")
        install(DIRECTORY "${HUXERUI_WINDOWS_APP_RESOURCES}/"
                DESTINATION "${target_name}.resources"
                COMPONENT "${install_component}"
        )
    endif ()

    string(UUID HUXERUI_WINDOWS_MSI_UPGRADE_CODE
            NAMESPACE 6ba7b810-9dad-11d1-80b4-00c04fd430c8
            NAME "dev.farna.clashflux.msi"
            TYPE SHA1
            UPPER
    )
    string(UUID HUXERUI_WINDOWS_BUNDLE_UPGRADE_CODE
            NAMESPACE 6ba7b810-9dad-11d1-80b4-00c04fd430c8
            NAME "dev.farna.clashflux.bundle"
            TYPE SHA1
            UPPER
    )
    set(HUXERUI_WINDOWS_PACKAGE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/huxerui-package/windows"
    )
    set(HUXERUI_WINDOWS_ICON_FILE "${PROJECT_SOURCE_DIR}/assets/icon.ico")
    set(HUXERUI_WINDOWS_PROJECT_VERSION "${PROJECT_VERSION}")
    file(MAKE_DIRECTORY "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}")

    # The installer has its own resource package; stage the shared mascot into
    # that package so the branding panel uses the same artwork as the app.
    set(HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY
            "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/resources"
    )
    file(GLOB_RECURSE HUXERUI_WINDOWS_INSTALLER_RESOURCE_FILES CONFIGURE_DEPENDS
            LIST_DIRECTORIES FALSE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/resources/*"
    )
    foreach (resource_file IN LISTS HUXERUI_WINDOWS_INSTALLER_RESOURCE_FILES)
        file(RELATIVE_PATH resource_relative_path
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/resources"
                "${resource_file}"
        )
        get_filename_component(resource_output_directory
                "${HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY}/${resource_relative_path}"
                DIRECTORY
        )
        file(MAKE_DIRECTORY "${resource_output_directory}")
        configure_file(
                "${resource_file}"
                "${HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY}/${resource_relative_path}"
                COPYONLY
        )
    endforeach ()
    file(MAKE_DIRECTORY "${HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY}/images")
    configure_file(
            "${PROJECT_SOURCE_DIR}/resources/images/mascot_logo_dark.png"
            "${HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY}/images/mascot_logo_dark.png"
            COPYONLY
    )

    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/Package.wxs.in"
            "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/Package.wxs"
            @ONLY
    )
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/Bundle.wxs.in"
            "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/Bundle.wxs"
            @ONLY
    )

    file(GLOB_RECURSE HUXERUI_WINDOWS_INSTALLER_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/src/*.cpp"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/src/*.cc"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/src/*.cxx"
    )
    huxerui_add_windows_installer(${target_name}_installer
            SOURCES
                ${HUXERUI_WINDOWS_INSTALLER_SOURCES}
                "${HUXERUI_WINDOWS_RESOURCE}"
            RESOURCES
                "${HUXERUI_WINDOWS_INSTALLER_RESOURCE_DIRECTORY}"
            RESOURCE_NAMESPACE
                installer
            INTEGRATION_OUTPUT
                "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/$<CONFIG>/installer.json"
    )
    set_target_properties(${target_name}_installer PROPERTIES
            OUTPUT_NAME "clash-flux-Installer"
    )
    file(GENERATE
            OUTPUT "${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/$<CONFIG>/package.json"
            CONTENT "{\n  \"schema\": 1,\n  \"name\": \"@PROJECT_NAME@\",\n  \"target\": \"clash-flux\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"installComponent\": \"${install_component}\",\n  \"packageSource\": \"${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/Package.wxs\",\n  \"bundleSource\": \"${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/Bundle.wxs\",\n  \"installerPlan\": \"${HUXERUI_WINDOWS_PACKAGE_DIRECTORY}/$<CONFIG>/installer.json\"\n}\n"
    )
endfunction()
