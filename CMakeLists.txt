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

# Transport backend on Unix-likes: "termios" talks to a /dev/ttyUSB* node via
# the ftdi_sio VCP driver; "libusb" drives the FTDI chip directly over libusb,
# for targets (notably Android) whose kernel lacks ftdi_sio. Ignored on Windows,
# which always uses FTDI D2XX.
set(MVCI_TRANSPORT "termios" CACHE STRING "Unix transport backend: termios or libusb")
set_property(CACHE MVCI_TRANSPORT PROPERTY STRINGS termios libusb)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# Resolve the libusb transport once (used by the driver and the self-test).
# Prefer pkg-config (libusb-1.0.pc), fall back to a plain find_library.
# --------------------------------------------------------------------------
if(NOT WIN32 AND MVCI_TRANSPORT STREQUAL "libusb")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
    endif()
    if(NOT LIBUSB_FOUND)
        # NAMES is usb-1.0 only — NOT plain "usb", which is the incompatible
        # legacy libusb-0.1 (the libusb-dev package). We need libusb-1.0-0-dev.
        find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
        find_library(LIBUSB_LIBRARY NAMES usb-1.0)
        if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
            message(FATAL_ERROR
                "MVCI_TRANSPORT=libusb but libusb-1.0 was not found. Install the "
                "libusb-1.0 dev package: 'sudo apt install libusb-1.0-0-dev' "
                "(NOT 'libusb-dev', which is the incompatible legacy libusb-0.1). "
                "Or point CMake at it, or build with -DMVCI_TRANSPORT=termios.")
        endif()
    endif()
endif()

# Apply the selected Unix transport (compile define + libs) to a target.
function(mvci_apply_transport tgt)
    if(NOT WIN32 AND MVCI_TRANSPORT STREQUAL "libusb")
        target_compile_definitions(${tgt} PRIVATE MVCI_TRANSPORT_LIBUSB)
        if(TARGET PkgConfig::LIBUSB)
            target_link_libraries(${tgt} PRIVATE PkgConfig::LIBUSB)
        else()
            target_include_directories(${tgt} PRIVATE ${LIBUSB_INCLUDE_DIR})
            target_link_libraries(${tgt} PRIVATE ${LIBUSB_LIBRARY})
        endif()
    endif()
endfunction()

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

# Aliases consumers can link — identical names to the installed package, so a
# vendored (add_subdirectory) and an installed (find_package) build are
# interchangeable in target_link_libraries():
#   libmvci           — plain name for add_subdirectory() consumers
#   libMVCI::libmvci  — namespaced name that find_package(libmvci) also provides
add_library(libmvci ALIAS MVCI)
add_library(libMVCI::libmvci ALIAS MVCI)

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

mvci_apply_transport(MVCI)

set_target_properties(MVCI PROPERTIES
    C_VISIBILITY_PRESET hidden
    EXPORT_NAME libmvci               # -> libMVCI::libmvci when exported
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
    mvci_apply_transport(mvci_test)

    # No-argument run is the hardware-free codec self-test.
    add_test(NAME mvci_selftest COMMAND mvci_test)
endif()

# --------------------------------------------------------------------------
# Install + export: consumers use
#   find_package(libmvci CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE libMVCI::libmvci)
# --------------------------------------------------------------------------
if(MVCI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(_mvci_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/libmvci)

    install(TARGETS MVCI
        EXPORT libmvciTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY include/mvci
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT libmvciTargets
        FILE libmvciTargets.cmake
        NAMESPACE libMVCI::
        DESTINATION ${_mvci_cmakedir})

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmvciConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfig.cmake"
        INSTALL_DESTINATION ${_mvci_cmakedir})

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfigVersion.cmake"
        DESTINATION ${_mvci_cmakedir})
endif()
