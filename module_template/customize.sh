#!/system/bin/sh
# ReUperf 自定义安装脚本
# TARGET_ARCH / TARGET_ARCH_MAGISK 由打包脚本注入

TARGET_ARCH="__TARGET_ARCH__"
TARGET_ARCH_MAGISK="__TARGET_ARCH_MAGISK__"
TARGET_DIR="/data/adb/ReUperf"

if [ "$ARCH" != "$TARGET_ARCH_MAGISK" ]; then
  abort "架构不匹配！模块编译目标: ${TARGET_ARCH} (Magisk ARCH=$TARGET_ARCH_MAGISK)，当前设备: $ARCH"
fi

ui_print "- 架构: $ARCH ✓ | API: $API"
ui_print "- 安装 ReUperf..."

mkdir -p "$TARGET_DIR" 2>/dev/null || abort "无法创建 $TARGET_DIR"

cp -f "$MODPATH/ReUperf/thread_scheduler" "$TARGET_DIR/thread_scheduler" 2>/dev/null || abort "复制 thread_scheduler 失败"
chmod 0755 "$TARGET_DIR/thread_scheduler" 2>/dev/null

cp -f "$MODPATH/ReUperf/ReUperf.json" "$TARGET_DIR/ReUperf.json" 2>/dev/null || abort "复制 ReUperf.json 失败"
chmod 0644 "$TARGET_DIR/ReUperf.json" 2>/dev/null

# libc++_shared.so: 动态版本需要，静态版本不存在
if [ -f "$MODPATH/ReUperf/libc++_shared.so" ]; then
  cp -f "$MODPATH/ReUperf/libc++_shared.so" "$TARGET_DIR/libc++_shared.so" 2>/dev/null
  chmod 0644 "$TARGET_DIR/libc++_shared.so" 2>/dev/null
  ui_print "- libc++_shared.so 已安装"
fi

cp -f "$MODPATH/ReUperf/AlreadyRun.sh" "$TARGET_DIR/AlreadyRun.sh" 2>/dev/null
cp -f "$MODPATH/ReUperf/OnceRun.sh" "$TARGET_DIR/OnceRun.sh" 2>/dev/null
chmod 0755 "$TARGET_DIR/AlreadyRun.sh" 2>/dev/null
chmod 0755 "$TARGET_DIR/OnceRun.sh" 2>/dev/null

printf "" > "$TARGET_DIR/ReUperf.log" 2>/dev/null

ui_print "- 安装完成，重启后自动生效"
ui_print "- 测试: sh /data/adb/ReUperf/OnceRun.sh"