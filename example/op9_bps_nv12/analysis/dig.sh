#!/system/bin/sh
K=/data/data/com.termux/files/home/bps_kmsg.txt
echo "=== FW frame data (cdm/iq/strip iovas) ==="
grep -aoE 'op9bps. FW:[^;]*' $K | tail -2
echo "=== per-buffer / IO_CONFIG / patch dumps ==="
grep -aoE 'op9bps. (IO_CONFIG|FRAME_PROCESS|FRAME|BUF|buffers|patch|PATCH|in.out)[^;]*' $K | tail -30
echo "=== all distinct iova-looking hex in op9bps lines ==="
grep -a 'op9bps' $K | grep -aoE '0x[0-9a-f]{7,8}|iova=[0-9a-f]+|=[0-9a-f]{8}' | sort -u | head -40
echo "=== context lines around the abort ==="
grep -an 'FAR=0x9101e0' $K | head -1
