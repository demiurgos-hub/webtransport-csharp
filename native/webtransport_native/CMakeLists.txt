cmake_minimum_required(VERSION 3.21)

project(webtransport_native LANGUAGES C)

option(WT_ENABLE_TEST_HOOKS "Export native protocol test hooks." OFF)

add_library(webtransport_native SHARED
    webtransport_native.c
    wt_datagrams.c
    wt_http3.c
    wt_msquic.c
    wt_qpack.c
    wt_streams.c
    wt_url.c
)

target_include_directories(webtransport_native
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(webtransport_native PRIVATE c_std_11)

if(MSVC)
    target_compile_options(webtransport_native PRIVATE /W4)
else()
    target_compile_options(webtransport_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WT_ENABLE_TEST_HOOKS)
    target_compile_definitions(webtransport_native PRIVATE WT_ENABLE_TEST_HOOKS=1)
endif()

set(MSQUIC_INCLUDE_DIR "" CACHE PATH "Directory containing msquic.h")
set(MSQUIC_LIBRARY "" CACHE FILEPATH "Path to the MsQuic library")

find_package(msquic CONFIG QUIET)

if(msquic_FOUND)
    target_compile_definitions(webtransport_native PRIVATE WT_HAS_MSQUIC=1)
    target_link_libraries(webtransport_native PRIVATE msquic)
    message(STATUS "MsQuic enabled through CMake package target.")
else()
    if(NOT MSQUIC_INCLUDE_DIR)
        find_path(MSQUIC_INCLUDE_DIR msquic.h)
    endif()

    if(NOT MSQUIC_LIBRARY)
        find_library(MSQUIC_LIBRARY msquic)
    endif()
endif()

if(NOT msquic_FOUND AND MSQUIC_INCLUDE_DIR AND MSQUIC_LIBRARY)
    target_compile_definitions(webtransport_native PRIVATE WT_HAS_MSQUIC=1)
    target_include_directories(webtransport_native PRIVATE ${MSQUIC_INCLUDE_DIR})
    target_link_libraries(webtransport_native PRIVATE ${MSQUIC_LIBRARY})
    message(STATUS "MsQuic enabled: ${MSQUIC_LIBRARY}")
elseif(NOT msquic_FOUND)
    message(FATAL_ERROR "MsQuic was not found. Install dependencies with vcpkg or set MSQUIC_INCLUDE_DIR and MSQUIC_LIBRARY.")
endif()
