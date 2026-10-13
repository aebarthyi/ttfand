cmake_minimum_required(VERSION 3.16)
project(ttfand CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ttfand src/ttfand.cpp)

# --- Locate libtt-umd -------------------------------------------------------
# Preferred: UMD was installed (umd-dev .deb, or `cmake --install`) and exports
# the umd::tt-umd target via a package config.
# umdConfig.cmake references fmt::fmt-header-only but doesn't find_dependency()
# it itself, so resolve it first from the same prefix (e.g. a tt-metal build
# tree exports both umd's and fmt's package configs under lib/cmake/).
find_package(fmt CONFIG QUIET)
find_package(umd CONFIG QUIET)

if(umd_FOUND)
    target_link_libraries(ttfand PRIVATE umd::tt-umd)
else()
    # Fallback: point at an in-tree build of tt-umd.
    #   cmake -B build -DTT_UMD_ROOT=/path/to/tt-umd
    # Expects headers under ${TT_UMD_ROOT}/device/api and the built
    # libtt-umd.so under ${TT_UMD_ROOT}/build/lib.
    if(NOT DEFINED TT_UMD_ROOT)
        message(FATAL_ERROR
            "libtt-umd not found. Either install umd-dev, or pass "
            "-DTT_UMD_ROOT=/path/to/tt-umd (with a built build/ dir).")
    endif()
    target_include_directories(ttfand PRIVATE
        ${TT_UMD_ROOT}/device/api
        ${TT_UMD_ROOT})
    find_library(TT_UMD_LIB tt-umd
        PATHS ${TT_UMD_ROOT}/build/lib ${TT_UMD_ROOT}/build
        REQUIRED)
    target_link_libraries(ttfand PRIVATE ${TT_UMD_LIB})
endif()

find_package(Threads REQUIRED)
target_link_libraries(ttfand PRIVATE Threads::Threads)

include(GNUInstallDirs)
install(TARGETS ttfand RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES systemd/ttfand.service DESTINATION /lib/systemd/system)
install(FILES ttfand.conf.example DESTINATION ${CMAKE_INSTALL_SYSCONFDIR} RENAME ttfand.conf)
