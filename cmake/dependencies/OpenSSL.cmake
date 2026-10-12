# The pinned static fallback has no openssl.pc. Its link contract is dl/threads;
# do not infer dependencies from a different system OpenSSL installation.
function(clashflux_find_openssl)
    set(vendor_prefix "${CLASHFLUX_VENDOR_DIR}/openssl/${CLASHFLUX_DEP_openssl_TOPDIR}")
    if(UNIX AND OPENSSL_ROOT_DIR STREQUAL vendor_prefix)
        set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
        set(PKG_CONFIG_FOUND FALSE)
        foreach(variable _OPENSSL_LIBRARIES _OPENSSL_STATIC_LIBRARIES
                _OPENSSL_LDFLAGS_OTHER _OPENSSL_STATIC_LDFLAGS_OTHER)
            set(${variable} "")
        endforeach()
    endif()
    find_package(OpenSSL ${ARGN})
    foreach(variable OpenSSL_FOUND OPENSSL_FOUND OpenSSL_VERSION OPENSSL_VERSION
            OPENSSL_VERSION_MAJOR OPENSSL_VERSION_MINOR OPENSSL_VERSION_PATCH
            OPENSSL_INCLUDE_DIR OPENSSL_LIBRARIES OPENSSL_SSL_LIBRARIES OPENSSL_CRYPTO_LIBRARIES)
        set(${variable} "${${variable}}" PARENT_SCOPE)
    endforeach()
endfunction()
