cmake_minimum_required(VERSION 3.14)
project(thread_scheduler LANGUAGES CXX)

option(BUILD_STATIC "Build static executable" OFF)

set(REUPERF_COMPILE_OPTIONS)
set(REUPERF_LINK_OPTIONS)

if(BUILD_STATIC)
    message(STATUS "Building with static linking")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/data/data/com.termux")
        find_library(LIBC_STATIC c PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        find_library(LIBM_STATIC m PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        find_library(LIBCXX_STATIC c++ PATHS /data/data/com.termux/files/usr/lib NO_DEFAULT_PATH)
        if(LIBC_STATIC AND LIBM_STATIC AND LIBCXX_STATIC)
            message(STATUS "Termux: detected static libraries, enabling static linking")
            list(APPEND REUPERF_LINK_OPTIONS -static)
        else()
            message(WARNING "Termux: complete static libraries not found; using dynamic linking")
            message(WARNING "For static linking: pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static")
            set(BUILD_STATIC OFF CACHE BOOL "Build static executable" FORCE)
        endif()
    else()
        list(APPEND REUPERF_LINK_OPTIONS -static)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/data/data/com.termux")
    find_program(LLD_EXECUTABLE lld)
    if(LLD_EXECUTABLE)
        message(STATUS "Termux: using lld linker")
        list(APPEND REUPERF_COMPILE_OPTIONS -fuse-ld=lld)
        list(APPEND REUPERF_LINK_OPTIONS -fuse-ld=lld)
    else()
        message(WARNING "Termux: lld not found; disabling PIE as fallback")
        list(APPEND REUPERF_COMPILE_OPTIONS -fno-PIE)
        list(APPEND REUPERF_LINK_OPTIONS -no-pie)
    endif()
endif()

find_package(nlohmann_json 3.11.3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include/nlohmann/json.hpp")
        message(STATUS "Using vendored nlohmann-json header")
        add_library(reuperf_json INTERFACE)
        target_include_directories(reuperf_json INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include")
        add_library(nlohmann_json::nlohmann_json ALIAS reuperf_json)
    else()
        message(STATUS "nlohmann-json not installed; fetching pinned v3.11.3 source")
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

add_executable(thread_scheduler main.cpp)

target_compile_features(thread_scheduler PRIVATE cxx_std_17)
set_target_properties(thread_scheduler PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(thread_scheduler PRIVATE -Wall -Wextra ${REUPERF_COMPILE_OPTIONS})
target_link_options(thread_scheduler PRIVATE ${REUPERF_LINK_OPTIONS})
target_include_directories(thread_scheduler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(thread_scheduler PRIVATE USE_CAPABILITY_STUB=1)
target_link_libraries(thread_scheduler PRIVATE nlohmann_json::nlohmann_json)

install(TARGETS thread_scheduler DESTINATION bin)
