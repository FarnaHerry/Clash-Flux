# Loaded from SweetEditor's project() scope so CMake can promote its import.
function(clashflux_import_ios_sweeteditor_simdutf)
    # The vendor selector matches lowercase SDK names. Normalize only its
    # input so compiler and linker flags still use Xcode's actual SDK path.
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" CMAKE_OSX_SYSROOT)
    include("${CMAKE_CURRENT_SOURCE_DIR}/3dparty/simdutf/sweeteditor_3p.cmake")
    set_property(TARGET sweeteditor_3p_simdutf PROPERTY IMPORTED_GLOBAL TRUE)
endfunction()

clashflux_import_ios_sweeteditor_simdutf()
