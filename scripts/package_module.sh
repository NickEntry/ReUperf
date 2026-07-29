#!/bin/bash
# Magisk Module Packaging Script
# 将编译好的二进制打包为 Magisk 模块 zip
#
# Usage:
#   ./scripts/package_module.sh --binary <path> --arch <arch> --type <dynamic|static> [--ndk <path>] [--output <dir>]
#
# 参数:
#   --binary   编译产物路径 (如 build/thread_scheduler)
#   --arch     目标架构 (arm64-v8a | armeabi-v7a | x86_64 | x86)
#   --type     链接类型 (dynamic | static)
#   --ndk      NDK 路径 (可选，动态版需要此路径来获取 libc++_shared.so)
#   --output   输出目录 (可选，默认 ./out)
#
# 例如:
#   ./scripts/package_module.sh --binary build/thread_scheduler --arch arm64-v8a --type dynamic --ndk ~/android-ndk-r26b
#   ./scripts/package_module.sh --binary build_static/thread_scheduler --arch arm64-v8a --type static

set -e

# ── 参数解析 ──────────────────────────────────────────────
BINARY=""
ARCH=""
BUILD_TYPE=""
NDK_PATH=""
OUTPUT_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)   BINARY="$2";   shift 2 ;;
        --arch)     ARCH="$2";     shift 2 ;;
        --type)     BUILD_TYPE="$2"; shift 2 ;;
        --ndk)      NDK_PATH="$2"; shift 2 ;;
        --output)   OUTPUT_DIR="$2"; shift 2 ;;
        *) echo "ERROR: Unknown option: $1"; exit 1 ;;
    esac
done

# ── 参数校验 ──────────────────────────────────────────────
if [ -z "$BINARY" ] || [ -z "$ARCH" ] || [ -z "$BUILD_TYPE" ]; then
    echo "Usage: $0 --binary <path> --arch <arch> --type <dynamic|static> [--ndk <path>] [--output <dir>]"
    echo ""
    echo "  --binary   编译产物路径"
    echo "  --arch     目标架构: arm64-v8a | armeabi-v7a | x86_64 | x86"
    echo "  --type     链接类型: dynamic | static"
    echo "  --ndk      NDK 路径 (动态版需要)"
    echo "  --output   输出目录 (默认: ./out)"
    exit 1
fi

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    exit 1
fi

if [ "$BUILD_TYPE" != "dynamic" ] && [ "$BUILD_TYPE" != "static" ]; then
    echo "ERROR: --type must be 'dynamic' or 'static', got: $BUILD_TYPE"
    exit 1
fi

case "$ARCH" in
    arm64-v8a)   TARGET_TRIPLE="aarch64-linux-android"    ; MAGISK_ARCH="arm64" ;;
    armeabi-v7a) TARGET_TRIPLE="arm-linux-androideabi"    ; MAGISK_ARCH="arm"   ;;
    x86_64)      TARGET_TRIPLE="x86_64-linux-android"     ; MAGISK_ARCH="x64"   ;;
    x86)         TARGET_TRIPLE="i686-linux-android"       ; MAGISK_ARCH="x86"   ;;
    *)
        echo "ERROR: Unsupported architecture: $ARCH"
        echo "Supported: arm64-v8a, armeabi-v7a, x86_64, x86"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_TEMPLATE="$REPO_DIR/module_template"
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_DIR/out}"
# 确保是绝对路径（避免 cd 后路径失效）
OUTPUT_DIR="$(cd "$OUTPUT_DIR" 2>/dev/null && pwd || (mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd))"
STAGING_DIR="$OUTPUT_DIR/.staging_${ARCH}_${BUILD_TYPE}"

# 版本号：优先使用 git commit 时间（统一），否则用当前时间
if git -C "$REPO_DIR" rev-parse HEAD >/dev/null 2>&1; then
    VERSION=$(git -C "$REPO_DIR" log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M:%S')
    VERSION_SHORT=$(git -C "$REPO_DIR" log -1 --format=%cd --date=format:'%Y%m%d%H%M%S')
else
    VERSION=$(date -u +"%Y-%m-%d %H:%M:%S" 2>/dev/null || date +"%Y-%m-%d %H:%M:%S")
    VERSION_SHORT=$(date -u +%Y%m%d%H%M%S 2>/dev/null || date +%Y%m%d%H%M%S)
fi
VERSION_CODE="1"

# 模块 zip 文件名
MODULE_NAME="ReUperf-${ARCH}-${BUILD_TYPE}-${VERSION_SHORT}"

echo "=========================================="
echo "Magisk Module Packaging"
echo "=========================================="
echo "  Binary:      $BINARY"
echo "  Architecture: $ARCH (triple: $TARGET_TRIPLE, Magisk: $MAGISK_ARCH)"
echo "  Build type:  $BUILD_TYPE"
echo "  Version:     $VERSION (code: $VERSION_CODE)"
echo "  Module name: $MODULE_NAME"
echo ""

# ── 清理旧 staging 目录 ──────────────────────────────────
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

# ── 复制模块模板文件 ─────────────────────────────────────
echo "[1/6] Copying module template files..."
cp -r "$MODULE_TEMPLATE/." "$STAGING_DIR/"

# ── 复制二进制 ───────────────────────────────────────────
echo "[2/6] Copying binary..."
cp "$BINARY" "$STAGING_DIR/ReUperf/thread_scheduler"
chmod 0755 "$STAGING_DIR/ReUperf/thread_scheduler"

# ── 动态版：复制 libc++_shared.so ─────────────────────────
if [ "$BUILD_TYPE" = "dynamic" ]; then
    echo "[3/6] Resolving libc++_shared.so..."

    # 优先从 NDK 获取
    LIBCXX_SOURCE=""
    if [ -n "$NDK_PATH" ] && [ -d "$NDK_PATH" ]; then
        NDK_LIBCXX="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TARGET_TRIPLE/libc++_shared.so"
        if [ -f "$NDK_LIBCXX" ]; then
            LIBCXX_SOURCE="$NDK_LIBCXX"
            echo "  → Found in NDK: $NDK_LIBCXX"
        fi
    fi

    # 备选：从 ANDROID_NDK_HOME 环境变量
    if [ -z "$LIBCXX_SOURCE" ] && [ -n "$ANDROID_NDK_HOME" ]; then
        NDK_LIBCXX="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TARGET_TRIPLE/libc++_shared.so"
        if [ -f "$NDK_LIBCXX" ]; then
            LIBCXX_SOURCE="$NDK_LIBCXX"
            echo "  → Found in ANDROID_NDK_HOME: $NDK_LIBCXX"
        fi
    fi

    # 备选：从 ANDROID_NDK 环境变量
    if [ -z "$LIBCXX_SOURCE" ] && [ -n "$ANDROID_NDK" ]; then
        NDK_LIBCXX="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TARGET_TRIPLE/libc++_shared.so"
        if [ -f "$NDK_LIBCXX" ]; then
            LIBCXX_SOURCE="$NDK_LIBCXX"
            echo "  → Found in ANDROID_NDK: $NDK_LIBCXX"
        fi
    fi

    # 备选：在系统路径中搜索
    if [ -z "$LIBCXX_SOURCE" ]; then
        echo "  → Searching system paths..."
        LIBCXX_SOURCE=$(find / -path "*/${TARGET_TRIPLE}/libc++_shared.so" -type f 2>/dev/null | head -1)
        if [ -n "$LIBCXX_SOURCE" ]; then
            echo "  → Found at: $LIBCXX_SOURCE"
        fi
    fi

    if [ -z "$LIBCXX_SOURCE" ]; then
        echo "  ✗ ERROR: libc++_shared.so not found for $ARCH ($TARGET_TRIPLE)"
        echo ""
        echo "  请通过 --ndk 参数指定 NDK 路径，或设置 NDK/ANDROID_NDK_HOME 环境变量。"
        echo "  libc++_shared.so 在 NDK 中的位置:"
        echo "    \$NDK/toolchains/llvm/prebuilt/<host>/sysroot/usr/lib/$TARGET_TRIPLE/libc++_shared.so"
        exit 1
    fi

    cp "$LIBCXX_SOURCE" "$STAGING_DIR/ReUperf/libc++_shared.so"
    chmod 0644 "$STAGING_DIR/ReUperf/libc++_shared.so"
    echo "  → Copied: $(ls -lh "$STAGING_DIR/ReUperf/libc++_shared.so" | awk '{print $5}')"
else
    echo "[3/6] Static build — skipping libc++_shared.so"
fi

# ── 下载 update-binary ────────────────────────────────────
echo "[4/6] Fetching update-binary from Magisk official repo..."
MAGISK_INSTALLER_URL="https://raw.githubusercontent.com/topjohnwu/Magisk/master/scripts/module_installer.sh"
UPDATE_BINARY="$STAGING_DIR/META-INF/com/google/android/update-binary"

if command -v curl &>/dev/null; then
    if curl -sL --connect-timeout 10 "$MAGISK_INSTALLER_URL" -o "$UPDATE_BINARY"; then
        echo "  → Downloaded via curl"
    else
        echo "  ✗ Download failed; using fallback"
        echo '#!/sbin/sh' > "$UPDATE_BINARY"
        echo 'umask 022' >> "$UPDATE_BINARY"
        echo 'ui_print() { echo "$1"; }' >> "$UPDATE_BINARY"
        echo 'require_new_magisk() { ui_print "Please install Magisk v20.4+!"; exit 1; }' >> "$UPDATE_BINARY"
        echo 'OUTFD=$2; ZIPFILE=$3' >> "$UPDATE_BINARY"
        echo 'mount /data 2>/dev/null' >> "$UPDATE_BINARY"
        echo '[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk' >> "$UPDATE_BINARY"
        echo '. /data/adb/magisk/util_functions.sh' >> "$UPDATE_BINARY"
        echo '[ $MAGISK_VER_CODE -lt 20400 ] && require_new_magisk' >> "$UPDATE_BINARY"
        echo 'install_module' >> "$UPDATE_BINARY"
        echo 'exit 0' >> "$UPDATE_BINARY"
    fi
elif command -v wget &>/dev/null; then
    if wget -q --timeout=10 "$MAGISK_INSTALLER_URL" -O "$UPDATE_BINARY"; then
        echo "  → Downloaded via wget"
    else
        echo "  ✗ Download failed; using fallback"
        echo '#!/sbin/sh' > "$UPDATE_BINARY"
        echo 'umask 022' >> "$UPDATE_BINARY"
        echo 'ui_print() { echo "$1"; }' >> "$UPDATE_BINARY"
        echo 'require_new_magisk() { ui_print "Please install Magisk v20.4+!"; exit 1; }' >> "$UPDATE_BINARY"
        echo 'OUTFD=$2; ZIPFILE=$3' >> "$UPDATE_BINARY"
        echo 'mount /data 2>/dev/null' >> "$UPDATE_BINARY"
        echo '[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk' >> "$UPDATE_BINARY"
        echo '. /data/adb/magisk/util_functions.sh' >> "$UPDATE_BINARY"
        echo '[ $MAGISK_VER_CODE -lt 20400 ] && require_new_magisk' >> "$UPDATE_BINARY"
        echo 'install_module' >> "$UPDATE_BINARY"
        echo 'exit 0' >> "$UPDATE_BINARY"
    fi
else
    echo "  ✗ Neither curl nor wget found; using fallback"
    echo '#!/sbin/sh' > "$UPDATE_BINARY"
    echo 'umask 022' >> "$UPDATE_BINARY"
    echo 'ui_print() { echo "$1"; }' >> "$UPDATE_BINARY"
    echo 'require_new_magisk() { ui_print "Please install Magisk v20.4+!"; exit 1; }' >> "$UPDATE_BINARY"
    echo 'OUTFD=$2; ZIPFILE=$3' >> "$UPDATE_BINARY"
    echo 'mount /data 2>/dev/null' >> "$UPDATE_BINARY"
    echo '[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk' >> "$UPDATE_BINARY"
    echo '. /data/adb/magisk/util_functions.sh' >> "$UPDATE_BINARY"
    echo '[ $MAGISK_VER_CODE -lt 20400 ] && require_new_magisk' >> "$UPDATE_BINARY"
    echo 'install_module' >> "$UPDATE_BINARY"
    echo 'exit 0' >> "$UPDATE_BINARY"
fi

# ── 生成 module.prop ──────────────────────────────────────
echo "[5/6] Generating module.prop..."
if [ "$BUILD_TYPE" = "dynamic" ]; then
    TYPE_LABEL="Dynamic"
    TYPE_DESC="动态链接版，需要 libc++_shared.so"
else
    TYPE_LABEL="Static"
    TYPE_DESC="静态链接版，无需额外依赖"
fi

cat > "$STAGING_DIR/module.prop" << MODULEPROP
id=ReUperf
name=ReUperf Thread Scheduler (${TYPE_LABEL}) - ${ARCH}
version=${VERSION}
versionCode=${VERSION_CODE}
author=ReUperf
description=Android 线程调度优化模块 - ${TYPE_DESC}
MODULEPROP

# ── 注入架构信息到 customize.sh ──────────────────────────
sed -i "s/__TARGET_ARCH__/${ARCH}/g" "$STAGING_DIR/customize.sh"
sed -i "s/__TARGET_ARCH_MAGISK__/${MAGISK_ARCH}/g" "$STAGING_DIR/customize.sh"

# ── 打包为 zip ───────────────────────────────────────────
echo "[6/6] Creating module zip..."
mkdir -p "$OUTPUT_DIR"

ZIP_FILE="$OUTPUT_DIR/${MODULE_NAME}.zip"
rm -f "$ZIP_FILE"

cd "$STAGING_DIR"
zip -r "$ZIP_FILE" . -x "*.DS_Store" -x "__MACOSX/*"
cd "$REPO_DIR"

# ── 清理 staging ─────────────────────────────────────────
rm -rf "$STAGING_DIR"

# ── 输出结果 ──────────────────────────────────────────────
echo ""
echo "=========================================="
echo "Module package created!"
echo "=========================================="
echo "  File: $ZIP_FILE"
echo "  Size: $(ls -lh "$ZIP_FILE" | awk '{print $5}')"
echo ""
echo "Contents:"
unzip -l "$ZIP_FILE" | tail -n +4 | head -n -2
echo ""
echo "Install via Magisk:"
echo "  adb push $ZIP_FILE /sdcard/"
echo "  # Then flash in Magisk app"
echo "=========================================="