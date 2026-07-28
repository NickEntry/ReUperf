#!/system/bin/sh
killall -15 thread_scheduler 2>/dev/null
if [ -e "${0%/*}/libc++_shared.so" ]; then
  export LD_LIBRARY_PATH=/data/adb/ReUperf
fi
printf "" >/data/adb/ReUperf/ReUperf.log
nohup ${0%/*}/thread_scheduler