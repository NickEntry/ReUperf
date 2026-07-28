#!/bin/bash

# Normal build script for thread_scheduler
# Usage: ./build.sh or bash build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=========================================="
echo "Thread Scheduler - Normal Build"
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
    echo "Detected Termux environment, using native compiler..."
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
echo "Configuring CMake..."
cmake -S "$SCRIPT_DIR" \
      -B "$BUILD_DIR" \
      -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install" \
      -DBUILD_STATIC=OFF

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
    
    # ── 打包 Magisk 模块 ──────────────────────────────────
    echo ""
    echo "=========================================="
    echo "Packaging Magisk Module..."
    echo "=========================================="
    
    PACKAGE_SCRIPT="$SCRIPT_DIR/scripts/package_module.sh"
    if [ -f "$PACKAGE_SCRIPT" ]; then
        chmod +x "$PACKAGE_SCRIPT"
        
        # 传递 NDK 路径 (动态版需要 libc++_shared.so)
        NDK_ARG=""
        if [ -n "$NDK" ] && [ -d "$NDK" ]; then
            NDK_ARG="--ndk $NDK"
        elif [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
            NDK_ARG="--ndk $ANDROID_NDK_HOME"
        fi
        
        bash "$PACKAGE_SCRIPT" \
            --binary "$BIN" \
            --arch "$TARGET_ARCH" \
            --type dynamic \
            $NDK_ARG \
            --output "$SCRIPT_DIR/out"
    else
        echo "WARNING: package_module.sh not found, skipping module packaging"
    fi
fi
