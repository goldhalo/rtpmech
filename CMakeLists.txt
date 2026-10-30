# ============================================================================
#  rtpmech —— 基于 netmech 的 RTP/RTCP 库（C++17）
#
#  独立构建：
#      cmake -S . -B build -G "Visual Studio 16 2019" -A x64
#      cmake --build build --config Release
#      ./build/bin/rtpmech_ut
#
#  依赖：
#      * netmech（通过 git submodule 位于 thirdparty/netmech）
#      * 标准库
#      * 不依赖 rtplib 的二进制（仅参考其协议实现，见 NOTICE）
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(rtpmech
        VERSION 0.9.0
        DESCRIPTION "RTP/RTCP library built on netmech"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(RTPMECH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         "${RTPMECH_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${RTPMECH_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RTPMECH_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(RTPMECH_TOP_LEVEL ON)
else()
    set(RTPMECH_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# 1. 选项
# ---------------------------------------------------------------------------
option(RTPMECH_BUILD_TESTS    "构建 rtpmech 单元测试" ON)
option(RTPMECH_BUILD_EXAMPLES "构建 rtpmech 示例" ON)
option(RTPMECH_WARNINGS_AS_ERRORS "把编译告警当错误（CI 用）" OFF)
option(RTPMECH_BUILD_BENCH    "构建压测工具" OFF)

# ---------------------------------------------------------------------------
# 2. 定位 netmech
# ---------------------------------------------------------------------------
set(RTPMECH_NETMECH_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/netmech"
    CACHE PATH "netmech 所在目录（默认使用 git submodule）")

if(NOT EXISTS "${RTPMECH_NETMECH_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 netmech：${RTPMECH_NETMECH_DIR}\n"
        "   若尚未初始化 submodule，请执行：\n"
        "     git submodule update --init --recursive\n"
        "   或指定：cmake -DRTPMECH_NETMECH_DIR=<netmech 路径> ...")
endif()

# 作为子工程引入 netmech（不构建它的测试/示例）
set(NETMECH_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(NETMECH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# 注意：不使用 EXCLUDE_FROM_ALL —— 否则 netmech 的 include 目录不会
# 传递到 rtpmech，导致 fatal error C1083: netmech/net.hpp not found。
add_subdirectory("${RTPMECH_NETMECH_DIR}" "${CMAKE_BINARY_DIR}/netmech")

message(STATUS "rtpmech: netmech @ ${RTPMECH_NETMECH_DIR}")

# ---------------------------------------------------------------------------
# 3. 编译选项
# ---------------------------------------------------------------------------
add_library(rtpmech_compile_options INTERFACE)

if(MSVC)
    target_compile_options(rtpmech_compile_options INTERFACE
        /W4
        /utf-8
        /bigobj
        $<$<BOOL:${RTPMECH_WARNINGS_AS_ERRORS}>:/WX>
    )
    target_compile_definitions(rtpmech_compile_options INTERFACE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    # Asio 内部的符号遮蔽（第三方代码），定向抑制
    target_compile_options(rtpmech_compile_options INTERFACE /wd4459)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    target_compile_options(rtpmech_compile_options INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${RTPMECH_WARNINGS_AS_ERRORS}>:-Werror>
    )
endif()

# ---------------------------------------------------------------------------
# 4. 库
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RTPMECH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(rtpmech STATIC ${RTPMECH_SOURCES})
add_library(rtpmech::rtpmech ALIAS rtpmech)

target_include_directories(rtpmech
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 显式补上 netmech 与 Asio 的 include 目录。
#
# 为什么不能只靠 target_link_libraries(rtpmech PUBLIC NetMechanism)：
#   netmech 的 include 目录用 $<BUILD_INTERFACE:...> 声明，当它作为
#   add_subdirectory 引入时，部分 CMake 版本（实测 4.4）不会把该 genex
#   求值结果传递到父工程的 public 接口 —— 表现为
#       fatal error C1083: 无法打开包括文件 "netmech/net.hpp"
#   因此这里显式追加路径，保证构建稳定。
target_include_directories(rtpmech PUBLIC
    "${RTPMECH_NETMECH_DIR}/include"
    "${RTPMECH_NETMECH_DIR}/thirdparty/asio/include")

target_link_libraries(rtpmech
    PUBLIC  NetMechanism
    PRIVATE rtpmech_compile_options)

target_compile_features(rtpmech PUBLIC cxx_std_17)

set_target_properties(rtpmech PROPERTIES FOLDER "rtpmech")

# ---------------------------------------------------------------------------
# 5. 测试
# ---------------------------------------------------------------------------
if(RTPMECH_BUILD_TESTS)
    file(GLOB RTPMECH_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")

    if(RTPMECH_TEST_SOURCES)
        # 复用 netmech 的测试框架（test_framework.hpp）
        add_executable(rtpmech_ut ${RTPMECH_TEST_SOURCES})
        set_target_properties(rtpmech_ut PROPERTIES FOLDER "rtpmech/test")
        target_include_directories(rtpmech_ut PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"      # 内部头（tracker / scheduler）
            "${RTPMECH_NETMECH_DIR}/test")
        target_link_libraries(rtpmech_ut PRIVATE rtpmech rtpmech_compile_options)

        enable_testing()
        add_test(NAME rtpmech_ut COMMAND rtpmech_ut)
    endif()
endif()

# ---------------------------------------------------------------------------
# 6. 示例
# ---------------------------------------------------------------------------
if(RTPMECH_BUILD_EXAMPLES)
    file(GLOB RTPMECH_EXAMPLE_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/example/*.cpp")
    foreach(src ${RTPMECH_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        set_target_properties(${name} PROPERTIES FOLDER "rtpmech/example")
        target_link_libraries(${name} PRIVATE rtpmech rtpmech_compile_options)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 6b. 压测工具
# ---------------------------------------------------------------------------
if(RTPMECH_BUILD_BENCH)
    file(GLOB RTPMECH_BENCH_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
    foreach(src ${RTPMECH_BENCH_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        set_target_properties(${name} PROPERTIES FOLDER "rtpmech/bench")
        target_link_libraries(${name} PRIVATE rtpmech rtpmech_compile_options)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 7. 摘要
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS " rtpmech ${PROJECT_VERSION}")
message(STATUS "   C++ 标准      : ${CMAKE_CXX_STANDARD}")
message(STATUS "   netmech        : ${RTPMECH_NETMECH_DIR}")
message(STATUS "   输出目录      : ${RTPMECH_OUTPUT_DIR}")
message(STATUS "----------------------------------------------------------")
