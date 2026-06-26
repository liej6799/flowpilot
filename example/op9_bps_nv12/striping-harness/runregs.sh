#!/system/bin/sh
P=/data/data/com.termux/files/usr
cd /data/local/tmp
logcat -b crash -c 2>/dev/null
echo "=== harness ptrs ==="
LD_LIBRARY_PATH=/data/local/tmp/stubs:$P/lib:/system/lib64 ./gen2 imx689_bps_cfg.bin imx689_bps_settings.bin /vendor/lib64/libipebpsstriping.so out_striping.bin 2>&1 | grep -E "M6|M8b"
sleep 1
echo "=== crash registers ==="
logcat -b crash -d 2>/dev/null | grep -E "x0 |x1 |x2 |x3 |x4 |x5 |x6 |x8 |x10|x11|x2[0-8]| pc |fault addr" | head -20
