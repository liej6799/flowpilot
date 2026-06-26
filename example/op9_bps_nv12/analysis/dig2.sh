#!/system/bin/sh
K=/data/data/com.termux/files/home/bps_kmsg.txt
echo "=== FW: line ==="
grep -aoE 'op9bps. FW:[^;]*' $K | tail -1
echo "=== PATCH[0..13] (input/output/blob iovas) ==="
grep -aoE 'op9bps. PATCH.[0-9]+. src_hdl=[0-9a-f]+ iova=[0-9a-f]+ len=[0-9]+ dst_off=[0-9]+' $K | head -14
echo "=== IO_CONFIG line ==="
grep -aoE 'op9bps. IO_CONFIG[^;]*' $K | tail -2
echo "=== buffer 0/1 addr dumps if any ==="
grep -aoE 'op9bps. (cb0|FW2|out|OUT|yuv|raw)[^;]*' $K | tail -10
echo "=== what iova range contains 0x9101e0? list buffers with iova<=0x920000 ==="
grep -aoE 'iova=[0-9a-f]+ len=[0-9]+' $K | sort -u | head -20
