#!/bin/bash
rm -f /tmp/stubs/*.so
LIBS="libc++.so libcutils.so liblog.so libofflinelog.so libsync.so libhardware.so libhidlbase.so libhidltransport.so libqdMetaData.so libcamera_metadata.so libutils.so libqti_vndfwk_detect.so libcdsprpc.so libgralloc.qti.so libgralloctypes.so android.hardware.graphics.allocator@3.0.so android.hardware.graphics.common@1.1.so android.hardware.graphics.mapper@3.0.so android.hardware.graphics.mapper@2.0.so android.hardware.graphics.mapper@2.1.so vendor.qti.hardware.display.allocator@3.0.so vendor.qti.hardware.display.mapper@2.0.so vendor.qti.hardware.display.mapper@3.0.so android.hardware.graphics.allocator@4.0.so android.hardware.graphics.mapper@4.0.so vendor.qti.hardware.display.allocator@4.0.so vendor.qti.hardware.display.mapper@4.0.so vendor.qti.hardware.display.mapperextensions@1.1.so vendor.qti.hardware.vpp@1.1.so vendor.qti.hardware.vpp@1.2.so"
n=0
for L in $LIBS; do
  /root/clang-r416183b/bin/clang --target=aarch64-linux-android21 -shared -nostdlib -fuse-ld=lld \
    -Wl,-soname,"$L" -o "/tmp/stubs/$L" /tmp/empty.c && n=$((n+1))
done
echo "built $n stubs"; ls /tmp/stubs | wc -l
