# QSC(3072) extraction via bpftrace (fault-safe, no ramdump)

The IMX766 4096x3072 mode = `BASE_INIT(522) + QSC(3072) + RES(144)`. The init is a
binary CDM packet (no per-register CCI log). The offset-fetch ftrace kprobe **cannot**
read QSC: it needs offsets ~49 KB into the array, and the same probe fires on the tiny
`size=1` poke arrays where that offset faults -> **QCOM ramdump**.

BPF solves it: `bpf_probe_read_kernel` is fault-safe, and an unrolled loop dumps the
whole array in one probe hit. Below is the full, reproduced recipe.

## Why not the obvious ways
- ftrace kprobe offset-fetch: ramdumps (high offset on small arrays).
- kernel module: `CONFIG_CFI_CLANG` + `CONFIG_MODVERSIONS` need the exact kernel build.
- parse `com.qti.sensormodule.lemonade.sunny_imx766.bin`: opaque CSL tagged format
  (even known RES(144) isn't findable in any flat encoding).
- bpftrace under proot: hangs (ptrace conflict). Kernel has no BTF, and rejects
  bounded `while` loops in the verifier.

## The recipe (Ubuntu 24.04 proot rootfs, run via real chroot)
```sh
# in the proot once:  apt-get install -y bpftrace
R=.../proot-distro/containers/ubuntu/rootfs
LD=/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
setenforce 0
mkdir -p $R/tmp $R/sys/kernel/tracing; chmod 1777 $R/tmp
mount -o bind /proc $R/proc; mount -o bind /sys $R/sys; mount -o bind /dev $R/dev
mount -o bind /sys/kernel/tracing $R/sys/kernel/tracing   # tracefs submount (not recursive)
# explicit linker (the /lib->usr/lib symlink breaks PT_INTERP in chroot), clean env:
env -i PATH=/usr/bin:/bin HOME=/root TMPDIR=/tmp chroot $R $LD /usr/bin/bpftrace prog.bt
```

## prog.bt (one chunk; BASE = 0 then 1600)
`unroll()` max is 100 -> nest; full unroll must stay under BPF's 16-bit branch range
-> 1600 regs/chunk, 2 chunks. Store the 16-byte entry raw (addr lo32, data hi32).
```
kprobe:camera_io_dev_write {
  $size = *(uint32*)(arg1 + 8);
  if ($size == 3072) {
    $ptr = *(uint64*)(arg1); $i = BASE;
    unroll(16) { unroll(100) { @q[$i] = *(uint64*)($ptr + $i*16); $i = $i + 1; } }
    exit();
  }
}
interval:s:150 { exit(); }
```

## Capture flow (per chunk)
1. `setprop ctl.stop vendor.camera-provider-2-4; sleep 3; ctl.start; sleep 4` (cold).
2. start the chroot+bpftrace in the background -> output file.
3. open Open Camera, switch to the ultra-wide lens (IMX766 cold init -> QSC fires).
4. bpftrace `exit()`s on capture; read `@q[i]: <val>` -> `addr = val & 0xffffffff`,
   `data = (val>>32) & 0xff`.

Verify vs the recon signature: `0xc800=0x24, 0xc801=0x31, 0xc802=0x2b, ...`, range
`0xc800-0xd3ff`. Reassemble BASE 0 (regs 0-1599) + BASE 1600 (1600-3071) = QSC(3072).

NOTE: capturing QSC did NOT enable SOF -- the remaining blocker is camerad's
request/streaming-sustain path, not the register set. See STREAMING-BRINGUP.md.
