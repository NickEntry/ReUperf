#!/system/bin/sh
# ReUperf 卸载脚本
killall -15 thread_scheduler 2>/dev/null
sleep 1
rm -rf /data/adb/ReUperf 2>/dev/null
rm -f /data/adb/ReUperf.log 2>/dev/null