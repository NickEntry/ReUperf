cmake_minimum_required(VERSION 3.10)
project(thread_scheduler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_STATIC "Build static executable" OFF)

if(BUILD_STATIC)
    message(STATUS "Building with static linking")
    set(BUILD_SHARED_LIBS OFF)
    
    # 检查是否在 Termux 环境
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/data/data/com.termux")
        # Termux 环境：检查静态库是否可用
        find_library(LIBC_STATIC c PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        find_library(LIBM_STATIC m PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        find_library(LIBCXX_STATIC c++ PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        
        # 检查静态库是否全部存在
        if(LIBC_STATIC AND LIBM_STATIC AND LIBCXX_STATIC)
            message(STATUS "Termux: 检测到静态库，启用静态链接")
            message(STATUS "  libc.a: ${LIBC_STATIC}")
            message(STATUS "  libm.a: ${LIBM_STATIC}")
            message(STATUS "  libc++.a: ${LIBCXX_STATIC}")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
        else()
            message(WARNING "Termux: 未找到完整的静态库，将使用动态链接")
            message(WARNING "如需静态链接，请运行:")
            message(WARNING "  pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static")
            # 禁用静态链接，使用动态链接
            set(BUILD_STATIC OFF CACHE BOOL "Build static executable" FORCE)
        endif()
    else()
        # 非 Termux 环境：正常静态链接
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    endif()
endif()

# 在 Termux 环境中使用 lld 链接器（修复 tagged pointers 问题）
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/data/data/com.termux")
    # 检查 lld 是否可用
    find_program(LLD_EXECUTABLE lld)
    if(LLD_EXECUTABLE)
        message(STATUS "检测到 lld，使用 lld 链接器（修复 tagged pointers 问题）")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fuse-ld=lld")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
    else()
        message(WARNING "未找到 lld，尝试使用默认链接器")
        # 备选方案：禁用 PIE
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-PIE")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
    endif()
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# 始终使用 stub 来避免 libcap 链接问题
# stub 提供空的 capability 函数实现
set(USE_CAPABILITY_STUB ON)
message(STATUS "Using stub for capability functions (cross-platform)")

# 注意：target_compile_definitions 将在 add_executable 后调用

if(NOT nlohmann_json_FOUND)
    # 检查本地离线文件
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include/nlohmann/json.hpp")
        message(STATUS "使用本地 nlohmann-json 头文件")
        add_library(json INTERFACE)
        target_include_directories(json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include)
        add_library(nlohmann_json::nlohmann_json ALIAS json)
    else()
        # 回退到 FetchContent 从 GitHub 下载
        message(STATUS "未找到本地 nlohmann-json，尝试从 GitHub 下载...")
        include(FetchContent)
        FetchContent_Declare(
            json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(json)
    endif()
endif()

add_executable(thread_scheduler
    main.cpp
)

target_include_directories(thread_scheduler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 传递宏给编译器（在目标创建后）
target_compile_definitions(thread_scheduler PRIVATE USE_CAPABILITY_STUB=1)

target_link_libraries(thread_scheduler PRIVATE
    nlohmann_json::nlohmann_json
)

# libcap 链接逻辑：
# - 仅在原生 x86_64/x86 编译时链接 libcap（libcap-dev 提供的头文件和库）
# - 交叉编译（ARM）或静态构建时跳过 libcap（使用代码中的 stub）
if(CMAKE_C_COMPILER STREQUAL "/usr/bin/gcc" OR CMAKE_C_COMPILER STREQUAL "/usr/bin/c++")
    # 原生 x86_64/x86 编译：检查 libcap-dev 是否已安装
    find_package(PkgConfig QUIET)
    pkg_check_modules(LIBCAP QUIET libcap)
    if(LIBCAP_FOUND)
        target_link_libraries(thread_scheduler PRIVATE ${LIBCAP_LIBRARIES})
        message(STATUS "Found libcap, linking: ${LIBCAP_LIBRARIES}")
    else()
        message(STATUS "libcap-dev not found, will use stub")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "i686")
    # 交叉编译 x86 架构：检查 libcap-dev
    find_package(PkgConfig QUIET)
    pkg_check_modules(LIBCAP QUIET libcap)
    if(LIBCAP_FOUND)
        target_link_libraries(thread_scheduler PRIVATE ${LIBCAP_LIBRARIES})
        message(STATUS "Found libcap for cross-compile, linking: ${LIBCAP_LIBRARIES}")
    else()
        message(STATUS "libcap-dev not found for cross-compile, will use stub")
    endif()
else()
    # ARM 交叉编译：跳过 libcap，使用 stub
    message(STATUS "Cross-compile for ARM, using stub for capability functions")
endif()

install(TARGETS thread_scheduler DESTINATION bin)
