#!/system/bin/sh
K=/data/data/com.termux/files/home/bps_kmsg.txt
echo "=== camerad cb1 (count + first/last) ==="
grep -ac 'op9bps. cb1' $K
grep -aoE 'op9bps. cb1 +[0-9]+:[0-9a-f ]*' $K | head -6
echo "--- cb1 lines mentioning a 0x9 / 0x91 value ? ---"
grep -aoE 'op9bps. cb1[^;]*' $K | grep -aE '0091|009101|9101e0|00910' | head
echo "=== num_cmd_buf / cmd_buf sizes ==="
grep -aoE 'op9bps. FRAME_PROCESS[^;]*' $K | head -1
grep -aoE 'op9bps. cb[01] (len|size|hdl)[^;]*' $K | head -4
