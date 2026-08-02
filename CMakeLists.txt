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

find_package(Threads REQUIRED)

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
            # nlohmann/json v3.11.3 (MIT); pin the immutable release commit.
            GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

add_executable(thread_scheduler main.cpp)

target_compile_features(thread_scheduler PRIVATE cxx_std_17)
set_target_properties(thread_scheduler PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(thread_scheduler PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    ${REUPERF_COMPILE_OPTIONS})

option(REUPERF_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(REUPERF_ENABLE_SANITIZERS AND NOT ANDROID)
    target_compile_options(thread_scheduler PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(thread_scheduler PRIVATE -fsanitize=address,undefined)
endif()
target_link_options(thread_scheduler PRIVATE ${REUPERF_LINK_OPTIONS})
target_include_directories(thread_scheduler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(thread_scheduler PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

install(TARGETS thread_scheduler DESTINATION bin)

include(CTest)
if(BUILD_TESTING AND NOT ANDROID)
    add_executable(reuperf_tests tests/reuperf_tests.cpp)
    target_compile_features(reuperf_tests PRIVATE cxx_std_17)
    set_target_properties(reuperf_tests PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(reuperf_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_include_directories(reuperf_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(reuperf_tests PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
    if(REUPERF_ENABLE_SANITIZERS)
        target_compile_options(reuperf_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(reuperf_tests PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME reuperf_tests COMMAND reuperf_tests)
endif()
