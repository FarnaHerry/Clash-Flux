set(HUXERUI_MACOS_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")
set(HUXERUI_MACOS_APP_ICON "${CMAKE_CURRENT_LIST_DIR}/AppIcon.icns")

function(huxerui_configure_macos_project_package target_name install_component)
    if (HUXERUI_PACKAGE)
        install(TARGETS ${target_name}
                BUNDLE DESTINATION .
                COMPONENT "${install_component}"
        )
        _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
                "$<TARGET_FILE_NAME:${target_name}>.app/Contents" "MacOS/$<TARGET_FILE_NAME:${target_name}>"
        )
    endif ()
endfunction()
