#!/bin/bash

# Static build script for thread_scheduler
# Usage: ./build_static.sh or bash build_static.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_static"

echo "=========================================="
echo "Thread Scheduler - Static Build"
echo "=========================================="

# Clean build directory
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning old build directory..."
    rm -rf "$BUILD_DIR"
fi

# Clean stale cmake artifacts from source directory
echo "Cleaning stale CMake artifacts from source directory..."
for f in CMakeCache.txt Makefile cmake_install.cmake; do
    if [ -f "$SCRIPT_DIR/$f" ]; then
        rm -f "$SCRIPT_DIR/$f"
    fi
done
if [ -d "$SCRIPT_DIR/CMakeFiles" ]; then
    rm -rf "$SCRIPT_DIR/CMakeFiles"
fi

mkdir -p "$BUILD_DIR"

IS_TERMX=false
if [ -n "$PREFIX" ] && [ "$PREFIX" = "/data/data/com.termux/files/usr" ]; then
    IS_TERMX=true
fi

if $IS_TERMX; then
    echo "Detected Termux environment, using native static compiler..."
    export CXX="c++"
    export CC="clang"
    
    # 检查 nlohmann-json 是否已安装
    if [ ! -f "$PREFIX/include/nlohmann/json.hpp" ]; then
        echo ""
        echo "警告：未找到 nlohmann-json 开发库"
        echo "需要运行: pkg install nlohmann-json"
        echo ""
        read -p "是否自动安装? (y/n): " INSTALL_JSON
        if [ "$INSTALL_JSON" = "y" ] || [ "$INSTALL_JSON" = "Y" ]; then
            echo "正在安装 nlohmann-json..."
            pkg install nlohmann-json -y
            if [ $? -ne 0 ]; then
                echo "安装失败，请手动运行: pkg install nlohmann-json"
                exit 1
            fi
        else
            echo "提示：CMake 将尝试从 GitHub 下载（需要网络连接）"
        fi
    else
        echo "nlohmann-json 已安装"
    fi
    
    # 检查 lld 是否已安装（修复 tagged pointers 问题）
    if ! pkg list-installed 2>/dev/null | grep -q "^lld/"; then
        echo ""
        echo "提示：未找到 lld 链接器，可能遇到 tagged pointers 问题"
        echo "建议运行: pkg install lld"
        echo ""
        read -p "是否自动安装 lld? (y/n): " INSTALL_LLD
        if [ "$INSTALL_LLD" = "y" ] || [ "$INSTALL_LLD" = "Y" ]; then
            echo "正在安装 lld..."
            pkg install lld -y
            if [ $? -ne 0 ]; then
                echo "安装失败，请手动运行: pkg install lld"
                echo "将尝试使用备选方案（禁用 PIE）"
            fi
        else
            echo "将尝试使用备选方案（禁用 PIE）"
        fi
    else
        echo "lld 链接器已安装"
    fi
    
    # 检查静态链接依赖（使用 pkg 查询已安装的包）
    STATIC_DEPS_MISSING=""
    
    if ! pkg list-installed 2>/dev/null | grep -q "^ndk-multilib-native-static/"; then
        STATIC_DEPS_MISSING="ndk-multilib-native-static"
    fi
    if ! pkg list-installed 2>/dev/null | grep -q "^ndk-sysroot/"; then
        STATIC_DEPS_MISSING="$STATIC_DEPS_MISSING ndk-sysroot"
    fi
    if ! pkg list-installed 2>/dev/null | grep -q "^libandroid-support-static/"; then
        STATIC_DEPS_MISSING="$STATIC_DEPS_MISSING libandroid-support-static"
    fi
    
    if [ -n "$STATIC_DEPS_MISSING" ]; then
        echo ""
        echo "警告：缺少静态链接所需的依赖包"
        echo "需要安装以下包："
        echo "  pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static"
        echo ""
        read -p "是否自动安装这些依赖？(y/n): " INSTALL_STATIC_DEPS
        if [ "$INSTALL_STATIC_DEPS" = "y" ] || [ "$INSTALL_STATIC_DEPS" = "Y" ]; then
            echo "正在安装静态链接依赖..."
            pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static -y
            if [ $? -ne 0 ]; then
                echo "安装失败，请手动运行："
                echo "  pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static"
                exit 1
            fi
        else
            echo "静态链接需要这些依赖，将退出构建"
            echo "请手动安装：pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static"
            exit 1
        fi
    else
        echo "静态链接依赖检查通过"
    fi

else
    if [ -z "$NDK" ]; then
        if [ -d "$HOME/android-ndk-r25c" ]; then
            export NDK="$HOME/android-ndk-r25c"
        elif [ -d "$HOME/android-ndk-r26b" ]; then
            export NDK="$HOME/android-ndk-r26b"
        elif [ -d "/opt/android-ndk" ]; then
            export NDK="/opt/android-ndk"
        else
            echo "ERROR: NDK not found. Please set NDK environment variable."
            echo "Example: export NDK=/path/to/android-ndk-r25c"
            exit 1
        fi
    fi

    echo "NDK path: $NDK"

    if [ ! -d "$NDK" ]; then
        echo "ERROR: NDK directory not found: $NDK"
        exit 1
    fi

    TOOLCHAIN=""
    case "$(uname -s)" in
        Linux*)
            TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
            ;;
        Darwin*)
            TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
            ;;
        MINGW*|MSYS*)
            TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
            ;;
        *)
            echo "ERROR: Unknown OS"
            exit 1
            ;;
    esac

    echo "Toolchain path: $TOOLCHAIN"

    if [ ! -d "$TOOLCHAIN" ]; then
        echo "ERROR: Toolchain not found for this platform"
        exit 1
    fi

    TARGET_ARCH="${TARGET_ARCH:-arm64-v8a}"
    API_LEVEL="${API_LEVEL:-29}"

    case "$TARGET_ARCH" in
        arm64-v8a)
            TARGET="aarch64-linux-android"
            ;;
        armeabi-v7a)
            TARGET="armv7a-linux-androideabi"
            ;;
        x86_64)
            TARGET="x86_64-linux-android"
            ;;
        x86)
            TARGET="i686-linux-android"
            ;;
        *)
            echo "ERROR: Unknown architecture: $TARGET_ARCH"
            exit 1
            ;;
    esac

    echo "Target: $TARGET (API $API_LEVEL)"

    export AR="$TOOLCHAIN/bin/llvm-ar"
    export CC="$TOOLCHAIN/bin/${TARGET}${API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/${TARGET}${API_LEVEL}-clang++"
    export LD="$TOOLCHAIN/bin/ld"
    export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
    export STRIP="$TOOLCHAIN/bin/llvm-strip"

    echo "CXX: $CXX"

    if [ ! -x "$CXX" ]; then
        echo "ERROR: C++ compiler not found: $CXX"
        exit 1
    fi
fi

echo ""
echo "Configuring CMake with static linking..."
cmake -S "$SCRIPT_DIR" \
      -B "$BUILD_DIR" \
      -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install" \
      -DBUILD_STATIC=ON

echo ""
echo "Building..."
cmake --build "$BUILD_DIR" -- -j$(nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)

BIN="$BUILD_DIR/thread_scheduler"
if [ -f "$BIN" ]; then
    echo ""
    echo "Stripping symbols..."
    if $IS_TERMX; then
        strip --strip-all "$BIN" 2>/dev/null || true
    else
        "$STRIP" --strip-all "$BIN"
    fi
    
    echo ""
    echo "=========================================="
    echo "Build Complete!"
    echo "=========================================="
    echo "Binary: $BIN"
    echo ""
    ls -lh "$BIN"
    
    echo ""
    echo "File type:"
    readelf -h "$BIN" | grep -E "(Class|Machine)" || true
    echo ""
    echo "Dependencies:"
    ldd "$BIN" 2>&1 | head -5 || true
fi
