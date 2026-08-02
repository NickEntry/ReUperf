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

set -euo pipefail

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
    arm64-v8a)   TARGET_TRIPLE="aarch64-linux-android" ; MAGISK_ARCH="arm64"; EXPECTED_MACHINE="AArch64" ;;
    armeabi-v7a) TARGET_TRIPLE="arm-linux-androideabi" ; MAGISK_ARCH="arm"; EXPECTED_MACHINE="ARM" ;;
    x86_64)      TARGET_TRIPLE="x86_64-linux-android" ; MAGISK_ARCH="x64"; EXPECTED_MACHINE="Advanced Micro Devices X86-64" ;;
    x86)         TARGET_TRIPLE="i686-linux-android" ; MAGISK_ARCH="x86"; EXPECTED_MACHINE="Intel 80386" ;;
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

READELF=""
for candidate in llvm-readelf readelf; do
    if command -v "$candidate" >/dev/null 2>&1; then
        READELF="$candidate"
        break
    fi
done
if [ -z "$READELF" ]; then
    echo "ERROR: llvm-readelf or readelf is required to validate the target binary"
    exit 1
fi

ELF_HEADER="$($READELF -h "$BINARY" 2>/dev/null)" || {
    echo "ERROR: Binary is not a readable ELF file: $BINARY"
    exit 1
}
ACTUAL_MACHINE="$(printf '%s\n' "$ELF_HEADER" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
if [ "$ACTUAL_MACHINE" != "$EXPECTED_MACHINE" ]; then
    echo "ERROR: Binary architecture mismatch: expected $EXPECTED_MACHINE for $ARCH, got $ACTUAL_MACHINE"
    exit 1
fi
if ! "$READELF" -W -S "$BINARY" 2>/dev/null | grep -Fq '.note.android.ident'; then
    echo "ERROR: Binary lacks the Android NDK identification note"
    exit 1
fi

PROGRAM_HEADERS="$($READELF -l "$BINARY" 2>/dev/null)"
DYNAMIC_SECTION="$($READELF -d "$BINARY" 2>/dev/null || true)"
INTERPRETER="$(printf '%s\n' "$PROGRAM_HEADERS" | sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')"
if [ -n "$INTERPRETER" ] && ! printf '%s\n' "$INTERPRETER" | grep -Eq '^/system/bin/linker(64)?$'; then
    echo "ERROR: Binary uses a non-Android program interpreter: $INTERPRETER"
    exit 1
fi
if printf '%s\n' "$DYNAMIC_SECTION" | grep -Eq 'Shared library: \[(libc\.so\.6|libstdc\+\+\.so\.6|libgcc_s\.so\.1)\]'; then
    echo "ERROR: Binary depends on host GNU runtime libraries and is not an Android artifact"
    exit 1
fi
if [ "$BUILD_TYPE" = "dynamic" ]; then
    if [ -z "$INTERPRETER" ]; then
        echo "ERROR: --type dynamic requires a dynamically linked Android executable"
        exit 1
    fi
    if ! printf '%s\n' "$DYNAMIC_SECTION" | grep -Fq 'Shared library: [libc++_shared.so]'; then
        echo "ERROR: Dynamic package requires the binary to depend on libc++_shared.so"
        exit 1
    fi
else
    if [ -n "$INTERPRETER" ] || printf '%s\n' "$DYNAMIC_SECTION" | grep -q '(NEEDED)'; then
        echo "ERROR: --type static requires an executable without a dynamic interpreter or shared-library dependencies"
        exit 1
    fi
fi

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
    if [ -z "$LIBCXX_SOURCE" ] && [ -n "${ANDROID_NDK_HOME:-}" ]; then
        NDK_LIBCXX="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TARGET_TRIPLE/libc++_shared.so"
        if [ -f "$NDK_LIBCXX" ]; then
            LIBCXX_SOURCE="$NDK_LIBCXX"
            echo "  → Found in ANDROID_NDK_HOME: $NDK_LIBCXX"
        fi
    fi

    # 备选：从 ANDROID_NDK 环境变量
    if [ -z "$LIBCXX_SOURCE" ] && [ -n "${ANDROID_NDK:-}" ]; then
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

# ── 使用仓库内自有安装入口 ────────────────────────────────
echo "[4/6] Using repository-local update-binary..."
UPDATE_BINARY="$STAGING_DIR/META-INF/com/google/android/update-binary"
if [ ! -f "$UPDATE_BINARY" ]; then
    echo "ERROR: Local update-binary is missing: $UPDATE_BINARY"
    exit 1
fi
chmod 0755 "$UPDATE_BINARY"

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