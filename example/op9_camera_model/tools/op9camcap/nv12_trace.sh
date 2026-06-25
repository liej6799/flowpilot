#!/system/bin/sh
# Capture BOTH cam_io_w and cam_io_w_mb (the WM packer_cfg/image_cfg use cam_io_w) during
# the op9camcap NV12 (1280x720, ISP_IFE_PROCESSED) stream.
T=/sys/kernel/tracing
echo 0 > $T/tracing_on
echo > $T/kprobe_events 2>/dev/null
echo 'p:camiow cam_io_w_mb data=$arg1:x32 addr=$arg2:x64' > $T/kprobe_events
echo 'p:camiow2 cam_io_w data=$arg1:x32 addr=$arg2:x64' >> $T/kprobe_events
ls $T/events/kprobes/camiow2/enable >/dev/null 2>&1 || { echo "KPROBE FAIL"; cat $T/error_log 2>/dev/null | tail -3; exit 1; }
echo 65536 > $T/buffer_size_kb 2>/dev/null
echo 1 > $T/events/kprobes/camiow/enable
echo 1 > $T/events/kprobes/camiow2/enable
echo "kprobes armed"
input keyevent KEYCODE_WAKEUP 2>/dev/null; svc power stayon true 2>/dev/null
am force-stop com.op9.camcap 2>/dev/null; sleep 1
echo > $T/trace
echo 1 > $T/tracing_on
am start-foreground-service -n com.op9.camcap/.CamService --es logical 0 --ei w 1280 --ei h 720 --ei dump 1 --ei skip 25 >/dev/null 2>&1
sleep 6
echo 0 > $T/tracing_on
am force-stop com.op9.camcap 2>/dev/null
cp $T/trace /data/local/tmp/nv12_iow2.trace
echo 0 > $T/events/kprobes/camiow/enable; echo 0 > $T/events/kprobes/camiow2/enable; echo > $T/kprobe_events
echo "trace lines: $(grep -acE 'camiow' /data/local/tmp/nv12_iow2.trace)"
chmod 644 /data/local/tmp/nv12_iow2.trace
