#!/system/bin/sh
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
H=$HOME; RFS=$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs
BH=$RFS/root/openpilot/system/camerad/cameras/bps_blobs.h
cp $H/bps_blobs.h $BH && echo "deployed bps_blobs.h ($(wc -c < $BH) bytes, multi-output cfg + 21736 striping)"
ls /data/local/tmp/kmsgcat 2>/dev/null || echo "NOTE: kmsgcat missing (will use dmesg fallback)"
sh $H/op9_bps_iter.sh
echo "=== dmesg FAR fallback ==="
dmesg 2>/dev/null | grep -aoE "FAR=0x[0-9a-f]+|Data Abort|op9bps] FW:" | tail -3
