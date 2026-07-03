cmake_minimum_required(VERSION 3.15)

project(MVCI
    VERSION 1.0.0
    DESCRIPTION "MVCI SAE J2534 PassThru driver (ISO14230 K-line)"
    LANGUAGES C)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
# When MVCI is pulled in via add_subdirectory() we default the extras off so
# an embedding project isn't burdened with our tests or install rules.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_mvci_standalone ON)
else()
    set(_mvci_standalone OFF)
endif()

option(MVCI_BUILD_TESTS   "Build the MVCI codec self-test"       ${_mvci_standalone})
option(MVCI_INSTALL       "Generate MVCI install/export rules"   ${_mvci_standalone})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --------------------------------------------------------------------------
# The driver library (J2534 PassThru DLL / .so / .dylib)
# --------------------------------------------------------------------------
add_library(MVCI SHARED
    src/passthru.c
    src/serial.c
    src/io.c
    src/des.c)
add_library(MVCI::MVCI ALIAS MVCI)

target_include_directories(MVCI
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(MVCI PRIVATE MVCI_BUILDING_DLL)

if(MSVC)
    target_compile_definitions(MVCI PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(MVCI PRIVATE
        -Wall -Wextra -Wno-deprecated-declarations)
endif()

# --------------------------------------------------------------------------
# Per-platform transport + crypto backends
# --------------------------------------------------------------------------
if(WIN32)
    # x86 -> MVCI32, x64 -> MVCI64, matching the original driver naming.
    # The architecture is whatever the active MSVC environment targets (it is
    # fixed per build tree), so the two DLLs come from two separate configures.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_mvci_arch "64")
    else()
        set(_mvci_arch "32")
    endif()
    set_target_properties(MVCI PROPERTIES OUTPUT_NAME "MVCI${_mvci_arch}")

    # The win32/win64 presets pass MVCI_EXPECT_ARCH so that running one from the
    # wrong Native Tools prompt fails here instead of silently building the
    # other architecture into the win32/win64 directory.
    if(DEFINED MVCI_EXPECT_ARCH AND NOT MVCI_EXPECT_ARCH STREQUAL _mvci_arch)
        if(MVCI_EXPECT_ARCH STREQUAL "32")
            set(_mvci_prompt "x86")
        else()
            set(_mvci_prompt "x64")
        endif()
        message(FATAL_ERROR
            "This preset targets ${MVCI_EXPECT_ARCH}-bit but the active MSVC "
            "environment is ${_mvci_arch}-bit.\n"
            "Open the \"${_mvci_prompt} Native Tools Command Prompt\" (or run "
            "'vcvarsall ${_mvci_prompt}') and re-run the preset.")
    endif()

    # Undecorated J2534 exports (shared by the 32- and 64-bit DLLs).
    if(MSVC)
        set_target_properties(MVCI PROPERTIES
            LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/mvci.def\"")
    else()  # MinGW
        target_sources(MVCI PRIVATE src/mvci.def)
    endif()

    # CNG/BCrypt for DES; ftd2xx.dll is loaded at runtime (no import lib).
    target_link_libraries(MVCI PRIVATE bcrypt)

elseif(APPLE)
    # DES via CommonCrypto (in libSystem — no link needed). Threads for the
    # keepalive; termios transport is part of the C library.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(MVCI PRIVATE Threads::Threads)

else()  # Linux / other Unix
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)   # libcrypto for DES
    target_link_libraries(MVCI PRIVATE OpenSSL::Crypto Threads::Threads)
endif()

set_target_properties(MVCI PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# --------------------------------------------------------------------------
# Self-test (links the protocol layer directly, not the J2534 exports)
# --------------------------------------------------------------------------
if(MVCI_BUILD_TESTS)
    enable_testing()
    add_executable(mvci_test
        test/mvci_test.c
        src/serial.c
        src/io.c
        src/des.c)
    target_include_directories(mvci_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if(WIN32)
        target_link_libraries(mvci_test PRIVATE bcrypt)
    elseif(APPLE)
        target_link_libraries(mvci_test PRIVATE Threads::Threads)
    else()
        target_link_libraries(mvci_test PRIVATE OpenSSL::Crypto Threads::Threads)
    endif()

    # No-argument run is the hardware-free codec self-test.
    add_test(NAME mvci_selftest COMMAND mvci_test)
endif()

# --------------------------------------------------------------------------
# Install + export (so consumers can `find_package(MVCI)`)
# --------------------------------------------------------------------------
if(MVCI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS MVCI
        EXPORT MVCITargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY include/mvci
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT MVCITargets
        FILE MVCITargets.cmake
        NAMESPACE MVCI::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MVCI)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/MVCIConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MVCIConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/MVCIConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MVCI)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/MVCIConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MVCIConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MVCI)
endif()
