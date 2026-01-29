# PlatformHelper.cmake
# Platform-specific helper macros for WonderTrader

# FIX_PLATFORM_LIBS: Remove stdc++fs and add iconv on APPLE
# Apple Clang includes filesystem support in libc++ without needing stdc++fs
# macOS needs explicit iconv linkage (not bundled in libc like glibc)
MACRO(FIX_PLATFORM_LIBS LIBS_VAR)
    IF(APPLE)
        LIST(REMOVE_ITEM ${LIBS_VAR} stdc++fs)
        LIST(APPEND ${LIBS_VAR} iconv)
    ENDIF()
ENDMACRO()

# SET_STRIP_PROPS: Set strip flags on release builds (not supported on Apple ld)
MACRO(SET_STRIP_PROPS TARGET_NAME)
    IF(NOT APPLE)
        SET_TARGET_PROPERTIES(${TARGET_NAME} PROPERTIES
            LINK_FLAGS_RELEASE -s)
    ENDIF()
ENDMACRO()

# CTP 6.6.9 darwin static library support
# The darwin .a libs require OpenSSL and stub implementations for internal symbols
IF(APPLE)
    SET(CTP669_DIR ${CMAKE_CURRENT_SOURCE_DIR}/API/CTP6.6.9/darwin)

    # Build stubs as a static library

    SET(CTP669_TRADER_LIBS
        ${CTP669_DIR}/libthosttraderapi_se.a
        ${CTP669_DIR}/libs/libssl.a
        ${CTP669_DIR}/libs/libcrypto.a
        ${CTP669_DIR}/libs/comunicationkeylib.a
    )
    SET(CTP669_MD_LIBS
        ${CTP669_DIR}/libthostmduserapi_se.a
        ${CTP669_DIR}/libs/libssl.a
        ${CTP669_DIR}/libs/libcrypto.a
        ${CTP669_DIR}/libs/comunicationkeylib.a
    )
ENDIF()