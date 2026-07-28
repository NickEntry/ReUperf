#!/system/bin/sh
# ReUperf Thread Scheduler - 开机自启动
MODDIR=${0%/*}

# 等待系统启动完成
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 5
done
sleep 10

# 确保目标目录存在
TARGET_DIR="/data/adb/ReUperf"
mkdir -p "$TARGET_DIR" 2>/dev/null || { echo "[ReUperf] Failed to create $TARGET_DIR"; exit 1; }

# 配置文件：保留用户修改，仅首次安装时复制
if [ ! -f "$TARGET_DIR/ReUperf.json" ]; then
  cp -f "$MODDIR/ReUperf/ReUperf.json" "$TARGET_DIR/ReUperf.json" 2>/dev/null
  chmod 0644 "$TARGET_DIR/ReUperf.json" 2>/dev/null
fi

# 二进制：大小不一致时强制覆盖（模块更新后自动同步）
if [ ! -f "$TARGET_DIR/thread_scheduler" ] || \
   [ "$(stat -c %s "$TARGET_DIR/thread_scheduler" 2>/dev/null)" != "$(stat -c %s "$MODDIR/ReUperf/thread_scheduler" 2>/dev/null)" ]; then
  cp -f "$MODDIR/ReUperf/thread_scheduler" "$TARGET_DIR/thread_scheduler" 2>/dev/null
  chmod 0755 "$TARGET_DIR/thread_scheduler" 2>/dev/null
fi

# 测试脚本：大小不一致时强制覆盖
for SCRIPT in AlreadyRun.sh OnceRun.sh; do
  if [ ! -f "$TARGET_DIR/$SCRIPT" ] || \
     [ "$(stat -c %s "$TARGET_DIR/$SCRIPT" 2>/dev/null)" != "$(stat -c %s "$MODDIR/ReUperf/$SCRIPT" 2>/dev/null)" ]; then
    cp -f "$MODDIR/ReUperf/$SCRIPT" "$TARGET_DIR/$SCRIPT" 2>/dev/null
    chmod 0755 "$TARGET_DIR/$SCRIPT" 2>/dev/null
  fi
done

# libc++_shared.so：如果存在则同步（动态版本需要）
if [ -f "$MODDIR/ReUperf/libc++_shared.so" ]; then
  if [ ! -f "$TARGET_DIR/libc++_shared.so" ] || \
     [ "$(stat -c %s "$TARGET_DIR/libc++_shared.so" 2>/dev/null)" != "$(stat -c %s "$MODDIR/ReUperf/libc++_shared.so" 2>/dev/null)" ]; then
    cp -f "$MODDIR/ReUperf/libc++_shared.so" "$TARGET_DIR/libc++_shared.so" 2>/dev/null
    chmod 0644 "$TARGET_DIR/libc++_shared.so" 2>/dev/null
  fi
fi

# 杀死旧进程
killall -15 thread_scheduler 2>/dev/null
sleep 1

# 设置库路径（动态版需要）
if [ -f "$TARGET_DIR/libc++_shared.so" ]; then
  export LD_LIBRARY_PATH="$TARGET_DIR"
fi

# 启动
printf "" > "$TARGET_DIR/ReUperf.log" 2>/dev/null
nohup "$TARGET_DIR/thread_scheduler" >/dev/null 2>&1 &
echo "$!" > "$TARGET_DIR/daemon.pid" 2>/dev/null