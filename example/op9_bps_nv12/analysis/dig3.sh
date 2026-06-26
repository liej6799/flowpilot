#!/system/bin/sh
K=/data/data/com.termux/files/home/bps_kmsg.txt
echo "=== full cb0 (frame_buffer bps_frame_process_data) words 0-45 ==="
grep -aoE 'op9bps. cb0 +[0-9]+:[0-9a-f ]*' $K | head -46
echo "=== FW: scalar (cdm/iq/strip/cdm_prog) ==="
grep -aoE 'op9bps. FW:[^;]*' $K | tail -1
echo "=== output buffer (yuv) patch: look for dst_off 56-72 ==="
grep -aoE 'op9bps. PATCH.[0-9]+. src_hdl=[0-9a-f]+ iova=[0-9a-f]+ len=[0-9]+ dst_off=(5[6-9]|6[0-9]|7[0-2])' $K | head
