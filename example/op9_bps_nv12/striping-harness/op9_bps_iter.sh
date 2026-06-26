#!/system/bin/sh
export PREFIX=/data/data/com.termux/files/usr
export HOME=/data/data/com.termux/files/home
export PATH=$PREFIX/bin:$PREFIX/bin/applets:/system/bin:/system/xbin
export LD_LIBRARY_PATH=$PREFIX/lib
H=$HOME; RFS=$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs; OP=$RFS/root/openpilot/system/camerad
cp $H/spectra_device.cc $OP/cameras/spectra.cc && echo "deployed"
proot-distro login ubuntu -- /bin/bash -lc "cd /root/openpilot && uv run scons -j6 system/camerad/camerad 2>&1 | grep -iE 'error|done building' | head -4"
ls -la $OP/camerad | awk '{print "binary",$5,$8}'
N=$(ls /dev/v4l-subdev* 2>/dev/null|wc -l); [ "$N" -lt 5 ] && insmod $H/camera_diag.ko && sleep 3
echo 1 > /sys/module/camera/parameters/op9_tolerate_violation; echo 1 > /sys/module/camera/parameters/op9_tolerate_pp
echo 1 > /sys/module/camera/parameters/op9_bps_dump; echo 1 > /proc/sys/kernel/printk
setprop ctl.stop vendor.camera-provider-2-4; sleep 4
OUT=$H/bps_kmsg.txt; rm -f $OUT; /data/local/tmp/kmsgcat > $OUT 2>/dev/null & KP=$!
sleep 1
BINDS="--bind /dev/video0 --bind /dev/video1 --bind /dev/video32 --bind /dev/video33"
i=0; while [ $i -le 40 ]; do [ -e /dev/v4l-subdev$i ] && BINDS="$BINDS --bind /dev/v4l-subdev$i"; i=$((i+1)); done
BINDS="$BINDS --bind /dev/media0 --bind /dev/media1 --bind /sys/class/video4linux --bind /dev/dma_heap --bind /dev/ion"
proot-distro login ubuntu $BINDS -- /bin/bash -lc "cd /root/openpilot; mkdir -p /tmp/params/d; printf 1 > /tmp/params/d/IsOffroad; OP9_BPS=1 OP9_SINGLE_IFE=1 OP9_RDI=1 PARAMS_ROOT=/tmp/params DISABLE_ROAD=1 DISABLE_DRIVER=1 timeout 14 ./system/camerad/camerad 2>&1" > $H/bps_stdout.log 2>&1
sleep 1; kill $KP 2>/dev/null; echo 7 > /proc/sys/kernel/printk; echo 0 > /sys/module/camera/parameters/op9_bps_dump
setprop ctl.start vendor.camera-provider-2-4; chmod 666 $OUT
echo "=== FAR ==="; grep -aoE "Data Abort.*FAR=0x[0-9a-f]+|FAR=0x[0-9a-f]+" $OUT | tail -1
echo "=== BPS done / IPE / frame / abort ==="; grep -aiE "bps.*done|ipe|frame done|recover|watch dog|Assertion|REPROCESS|wideRoad" $OUT $H/bps_stdout.log | grep -aivE "skip|cvp" | tail -6
echo "=== camerad tail ==="; tail -3 $H/bps_stdout.log
