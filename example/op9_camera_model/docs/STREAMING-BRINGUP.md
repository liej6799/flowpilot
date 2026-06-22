# IMX766 streaming bring-up on camerad (OnePlus 9 / SM8350)

Status as of this checkpoint: **camerad runs end-to-end and configures the IMX766**
(road camera) with the real captured register tables, correct CSIPHY/CCI/datarate.
The sensor powers, probes, takes its init+streamon i2c, the IFE/CSID acquire and
start for 4000x3000 NV12, and CSIPHY 2 locks. The remaining blocker is that the
sensor does not emit MIPI data yet (`CSID irq_status_rx = 0x0`, CRM "watchdog
paused" / no SOF).

## Sensor register tables (captured live from the HAL)

Captured with an ftrace kprobe on `camera_io_dev_write` while the OnePlus camera
HAL opened the IMX766 (see SENSOR-EXTRACTION.md). Three i2c bursts at camera open,
in this order: **BASE_INIT(522) -> QSC(3072) -> RES -> 0x0100 (stream on)**.

- BASE_INIT: 522 regs (MCLK/PLL/MIPI/global tuning). No resolution regs.
- QSC: 3072 regs at 0xc800-0xca1b (Quad-Sampling-Coding shading table; image
  quality only, omitted from `init_array_imx766` for now).
- RES (mode): two captured:
  - full-res 8192x6144 (128 regs) RAW10, no binning -- impractical (1.9 GB/s, dual-IFE).
  - **binned 4000x3000 (106 regs)** RAW10, 2x2 binning (0x0900=1, 0x0901=0x22),
    analog crop = full 8000x6000 array (full ultrawide FOV). This is what we use.
    Triggered by opencamera camera-id 2 @ 4000x3000.

`init_array_imx766` = BASE_INIT(522) + binned RES(106). See `sensors/imx766_registers.h`.

## Hardware facts (device tree + live HAL logs)

From `lemonade` camera dtsi and CSIPHY debug logs:

| sensor (DT)           | model        | csiphy-sd-index | cci-master | cci block |
|-----------------------|--------------|-----------------|------------|-----------|
| cam-sensor@0 (wide)   | IMX789       | 1               | 0          | cci0      |
| **cam-sensor@1 (uw)** | **IMX766**   | **2**           | **1**      | **cci0**  |
| cam-sensor@2 (front)  | -            | 5               | 1          | cci1      |
| cam-sensor@3 (tele)   | -            | 0               | 0          | cci0/1    |

- IMX766: CSIPHY **2**, CCI **master 1**, slave **0x34**, MCLK **19.2 MHz**.
- Binned-mode MIPI: **datarate 1,925,500,000** (1.9255 Gbps/lane), **settle 2.8 us**,
  4 lanes (DPHY 2-phase). Confirmed from the HAL `CAM_START_PHYDEV` log AND derived
  from the binned RES PLL regs (0x030B=4, 0x030E:0F=0x15F). Set in `sensors/imx766.cc`.
- The full-res mode datarate is 5.7929 Gbps -- do NOT use it with the binned regs.

## camerad code fixes (cameras/spectra.cc -- on top of the SM8350 port)

1. **NOP poke packet size** (`sensors_poke`). The oplus `cam_sensor_i2c_pkt_parse`
   rejects a packet buffer whose length == `sizeof(struct cam_packet)`:
   `config.offset(0) >= len_of_buff - sizeof(cam_packet)(0)` is always true -> -EINVAL,
   disabling the sensor every frame ("FAILED poke"). Fix: allocate the poke buffer
   `sizeof(cam_packet) + sizeof(cam_cmd_buf_desc)` so `len_of_buff` is strictly
   greater; keep `header.size = sizeof(cam_packet)`.

2. **Sensor subdev selection** (`openSensor`). `cam-sensor-driver` subdev
   enumeration order is not guaranteed; the oplus probe override makes every probe
   "succeed", so the wrong subdev silently configures the wrong sensor. Fix: pick the
   subdev whose `CAM_QUERY_CAP.csiphy_slot_id == csiphy_index`. IMPORTANT: do NOT
   `close()` the non-matching cam-sensor-driver fds during the scan -- closing a
   sensor subdev triggers req-mgr handle-table cleanup that corrupts later
   `cam_create_device_hdl` calls (sensor/ISP acquire then return -EINVAL, configISP
   asserts on `isp_dev_handle_`). Leak the scanned fds until process exit.
   (For IMX766, the matched index happens to be 1 = csiphy_slot_id 2.)

   Note: prior camerad runs that abort/`timeout` leak req-mgr device handles; the
   kernel teardown logs `Shutdown failure for node cam-isp rc=-22`. After enough
   leaks `cam_create_device_hdl` fails for everyone -> reboot to clear.

## Sensor driver geometry (sensors/imx766.cc)

- `data_word = false` (Sony IMX: WORD addr, BYTE data -- confirmed: every captured
  data byte <= 0xff and exposure regs split 16-bit into two byte writes).
- `frame_width=4000, frame_height=3000`, `out_scale=1` for first bring-up (no IFE
  scaler). To match openpilot's road cam (1928x1208) switch to `out_scale=2`
  (-> 2000x1500) + crop once SOF works.
- `mipi_data_rate=1925500000`, `mipi_settle=2800000000`, `csiphy_index=2`.

## ROOT CAUSE of no-MIPI FOUND: wrong (full-res) init paired with binned res

A size-only kprobe recon of a *binned* opencamera open showed the binned mode uses a
DIFFERENT register sequence than full-res:

| mode      | base init | calibration | res |
|-----------|-----------|-------------|-----|
| full-res  | 522       | 3072 (QSC @0xc800) | 128 |
| **binned**| **2232**  | **4047** (0x3a39-0xf92d) | **106** |

The earlier `init_array` wrongly paired the **full-res 522 base init** with the
**binned 106 res** -> MIPI/global config inconsistent with the mode -> sensor took
its i2c and CSIPHY locked but emitted no frames (CSID irq_status_rx=0).

Fix: `init_array_imx766` now = binned **BASE_INIT(2232) + CAL(4047) + RES(106)**
(HAL order, ts-confirmed: 2232 @1041.46 -> 4047 @1041.51 -> 106), captured via a
68-window kprobe (`size>500`) on a cold-HAL opencamera cam-id 2 open. 6385 regs total.

Status: builds and camerad runs with NO errors (no poke fail / no assert / no abort);
SOF confirmation (CSID irq_status_rx != 0) pending a device-online dmesg check.

If SOF still does not fire, remaining candidates:
1. **Initial exposure** before streamon (HAL writes small 8/11-reg bursts).
2. **Separate i2c calls** vs one combined init_array (HAL does 3 CONFIG_DEV calls;
   if combining matters, apply 2232/4047/106 as separate sensors_i2c CONFIG calls).
3. **Streamon timing** -- 0x0100=1 must be the last write after CSID/IFE/CSIPHY start.

## Reproduce

```sh
# device (android, root): free the HAL so camerad can take CSIPHY 2 / cci0-m1
setprop ctl.stop vendor.camera-provider-2-4
echo 0xffffffff > /sys/module/camera/parameters/debug_mdl   # optional, for dmesg
# proot: build + run road camera only
cd /root/openpilot && uv run scons -j4 system/camerad/
export PARAMS_ROOT=/tmp/params DISABLE_WIDE_ROAD=1 DISABLE_DRIVER=1
timeout 14 ./system/camerad/camerad
# check: dmesg | grep irq_status_rx   (want non-zero), and SOF / no "watchdog paused"
```

## SOLVED: no-MIPI root cause = C-PHY + wrong datarate (2026-06-21)

The "sensor takes its i2c but emits no MIPI (`irq_status_rx = 0x0`)" blocker was
**two wrong CSIPHY assumptions**, found by diffing camerad against a live HAL
trace of the HAL streaming the IMX766 (`CAM_START_PHYDEV` + `cam_cmd_buf_parser`
with `/sys/module/camera/parameters/debug_mdl` set, captured on a *cold* open):

| param | camerad had (wrong) | HAL (correct) |
|---|---|---|
| **PHY type** | D-PHY (`csiphy_3phase=0`, 4 lanes) | **C-PHY** (`is_3phase=1`, **3 trios**) |
| **lane_cnt / lane_assign** | 4 / 0x3210 | **3 / 0x210** |
| **CSIPHY datarate** | 1,925,500,000 (that is *IMX689*'s rate) | **2,457,600,000** (2.4576 Gbps) |

The IMX766 transmits **C-PHY** (the sensor reg table even says so: `0x0111=0x03`
= CSI C-PHY signaling, `0x0114=0x02` = 3-lane). camerad hard-coded D-PHY, so the
receiver PHY could never lock the signal. Fixes (in `patches/...sm8350.patch`):

1. `configCSIPHY` acquire: `csiphy_acquire_dev_info.csiphy_3phase = mipi_cphy`.
2. `configCSIPHY` config: `lane_cnt = 3`, `lane_assign = 0x210` when `mipi_cphy`.
3. `configISP` CSID in_port: `lane_type = CAM_ISP_LANE_TYPE_CPHY`, `lane_num = 3`,
   `lane_cfg = 0x210` when `mipi_cphy`.
4. `imx766.cc`: `mipi_cphy = true`, `mipi_data_rate = 2457600000`.

A per-sensor `bool mipi_cphy` (in `SensorInfo`) gates all of this so D-PHY sensors
are unaffected.

### Result: camerad RECEIVES MIPI for the first time
```
CSID:2 Lane type:1 lane_num:3 dt:43   (CPHY, 3-lane, RAW10 -- matches HAL)
irq_status_rx = 0x400077              (was 0x0 -- MIPI packets now received!)
irq_status_ipp = 0x149338            (pixel path active)
```

### Remaining: frame-geometry / mode mismatch (CCIF violation)
camerad now streams but does not yet finalize frames:
```
CSID:2 IPP_PATH_ERROR_CCIF_VIOLATION: Bad frame timings
```
Cause: the captured `imx766_registers.h` table is the **4000x3000** mode, but the
HAL ultra-wide actually streams **4096x3072** (different PLL: `IOPPXCK_DIV` 4 vs 2).
The register table and the working mode differ. To finish: capture the HAL's
**4096x3072** CPHY register table (kprobe on `camera_io_dev_write` during a *cold*
full-init open -- opencamera's warm preview only sends ~1400 delta regs, not the
full ~6000), drop it into `imx766_registers.h`, and set `frame_width=4096,
frame_height=3072` in `imx766.cc`. The datarate (2.4576 Gbps) already matches that
mode; for the present 4000x3000 table the equivalent rate is 1.2288 Gbps (half).

### Capturing the HAL CSIPHY trace (defeats session caching)
The OnePlus camera provider keeps sensors warm, so a normal app open does not
re-log CSIPHY/probe. Force a cold open:
```sh
setprop ctl.stop vendor.camera-provider-2-4 ; sleep 3
echo 0xffffffff > /sys/module/camera/parameters/debug_mdl ; dmesg -C
setprop ctl.start vendor.camera-provider-2-4 ; sleep 5
nohup timeout 60 dmesg -w > /sdcard/follow.log &   # stream to file (no buffer wrap)
# then open Open Camera and select the ultra-wide lens
grep -E "CAM_START_PHYDEV|cam_cmd_buf_parser: 503|is_3phase|irq_status_rx" /sdcard/follow.log
```

## CCIF frame-timing SOLVED: coherent 4096x3072 mode (2026-06-21)

After C-PHY got MIPI flowing, the 4000x3000 table hit `IPP_PATH_ERROR_CCIF_VIOLATION:
Bad frame timings` every frame (datarate- and frame_offset-independent). Root cause:
the captured 4000x3000 table (`BASE_INIT 2232 + CAL 4047 + RES 106`) is **not the
mode the HAL streams**. A kprobe recon of a cold ultra-wide open shows the HAL uses a
**different, simpler mode**:

| mode | structure | who |
|---|---|---|
| 4000x3000 | BASE_INIT(2232)+CAL(4047)+RES(106) | old capture, CCIF violation |
| **4096x3072** | **BASE_INIT(522)+QSC(3072)+RES(144)** | **what opencamera ultra-wide actually streams** |

4096x3072 = the 2x2-binned full array. Its `BASE_INIT(522)` == the captured
`imx766_base_init_522.json`, and its PLL (in RES: `0x030e:0f=0x00e0`, `0x030b=2`)
yields exactly the HAL's 2.4576 Gbps. Extracted the **RES(144)** via a SAFE 3-window
kprobe (180 fetches/call) and rebuilt `imx766_registers.h` = BASE_INIT(522)+RES(144),
`frame_width=4096, frame_height=3072`, `init_group_sizes={522,144}`.

Result: **CCIF violation GONE** (0, was 30). CSID `width 4096 height 3072`, geometry
correct, sensor takes the full init + stream-on.

### Remaining: SOF needs QSC(3072)
camerad now issues frame requests (`reg_update_cmd 0x41`) but **no SOF returns**
(watchdog after 2s). The lone difference from the HAL's working sequence is the
**QSC(3072)** shading table (`0xc800-0xca1b`) applied between BASE_INIT and RES, which
we omit. The 4000x3000 mode streamed continuously because it included its CAL(4047);
4096x3072 without QSC configures perfectly but emits no frames. So QSC is required for
this quad-bayer binned mode to start MIPI output.

### Why QSC is hard to capture
The init is a binary CDM packet (no per-register CCI log). The kprobe-on-
`camera_io_dev_write` method reads the array by fixed offsets -- but QSC needs offsets
up to ~49 KB into the array, and the SAME probe fires on the tiny `size=1` poke calls,
where reading 49 KB past a 16-byte array faults -> **QCOM ramdump** (confirmed: a
9-window probe at offset 8 KB crashed the device). Only LOW-offset reads (<~3 KB, 3
windows) are safe. So QSC(3072) can't be pulled this way. Options: parse
`/odm/lib64/camera/com.qti.sensormodule.lemonade.sunny_imx766.bin`, a small kernel
module that dumps the array safely, or a published IMX766 QSC table.

### Status
| stage | status |
|---|---|
| MIPI reception (C-PHY + 2.4576 Gbps) | done |
| CCIF frame-timing (coherent 4096x3072 mode) | **done** |
| sensor SOF / frames | needs QSC(3072) |
```

## QSC(3072) captured via bpftrace -- and ruled out as the SOF blocker (2026-06-21)

The full 4096x3072 mode is `BASE_INIT(522) + QSC(3072) + RES(144)`. QSC is a binary
CDM packet (no per-register CCI log) and can't be read by the offset-fetch ftrace
kprobe -- high offsets fault on the tiny `size=1` poke arrays -> **QCOM ramdump**
(confirmed twice). **BPF fixes this**: `bpf_probe_read_kernel` is fault-safe and a
bounded/unrolled loop dumps the whole array in one probe hit.

### The bpftrace QSC dumper (the safe extraction)
bpftrace can't run under proot (ptrace conflict) and the kernel has **no BTF** and
**rejects bounded `while` loops** (verifier). Solution: run bpftrace in a real
**chroot** (not proot) and **unroll** the loop. Gotchas, all solved:
- chroot exec fails on the `/lib`->`usr/lib` symlink for PT_INTERP -> invoke the
  explicit linker: `chroot $R /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 bpftrace`.
- needs `PATH`, writable `/tmp`, and the **tracefs submount** bind-mounted
  (`/sys/kernel/tracing`) separately (a plain `--bind /sys` doesn't recurse).
- `unroll()` max is 100 -> nest `unroll(16){unroll(100){}}`; and the fully-unrolled
  body must stay under BPF's 16-bit branch range -> **2 chunks** of 1600 regs.

```
kprobe:camera_io_dev_write {
  $size = *(uint32*)(arg1 + 8);
  if ($size == 3072) {
    $ptr = *(uint64*)(arg1); $i = BASE;
    unroll(16){ unroll(100){ @q[$i] = *(uint64*)($ptr + $i*16); $i = $i+1; } }
    exit();
  }
}
```
Decode each map value: `addr = val & 0xffffffff`, `data = (val>>32) & 0xff`. Two cold
ultra-wide opens (BASE 0 and 1600) capture all 3072, verified vs the recon signature
(`0xc800=0x24...`, range `0xc800-0xd3ff`). See `scripts/qsc_bpftrace.md`.

### Result: QSC is NOT the SOF blocker
Dropped the captured QSC(3072) into `imx766_registers.h` (now 3738 regs,
`init_group_sizes={522,3072,144}`). The QSC verifiably applies (camerad writes
`0xc800=0x24`...), but the runtime result is **identical** to the 522+144 build:
CCIF=0, `irq_status_rx=0x400077` once, then **no continuous SOF** (CRM watchdog).
So the earlier "needs QSC" guess (from 4000x3000-streamed vs 4096x3072-didn't) was
confounded by mode-incoherence; the real remaining blocker is **camerad's
request/streaming-sustain path**, not the sensor register set (which now matches the
HAL's mode exactly). camerad has never produced an EPOCH/SOF in any mode, while the
HAL does -- next suspects: the per-request schedule/sync, `sensors_poke`, or the
CSID/IFE start sequence vs the HAL.

| stage | status |
|---|---|
| MIPI reception (C-PHY + 2.4576 Gbps) | done |
| CCIF frame-timing (coherent 4096x3072 mode) | done |
| full register set BASE_INIT+QSC+RES (matches HAL) | **done** |
| sensor SOF / frames | camerad request/sustain path (registers ruled out) |

## Stream-on bursts captured; registers fully exonerated (2026-06-21)

Captured (bpftrace, same method) the two bursts the HAL applies at stream-on that
camerad omitted -- the recon order is `...RES(144) -> size=11 -> size=8 -> 0x0100=1`:

- **size=11** (grouped-hold exposure): `0x0104=1; 0x3128=0; 0x0340=0x1a,0x0341=0x0e`
  (**frame_length 6670**, overriding RES's 3310); `0x0202=0x19,0x0203=0xde`
  (**integration 6622**); `0x0204=0x3f,0x0205=0` (analog gain); `0x020e=1,0x020f=0`
  (digital gain 1x); `0x0104=0`.
- **size=8**: `0x0b8e=1,0x0b8f=0,0x0b90=1,0x0b91=0xe9,0x0b92=2,0x0b93=0x12,0x0b94=1,
  0x0b95=0` -- an IMX766-specific block camerad never wrote.

Put the exact HAL stream-on sequence into `start_reg_array_imx766` (and disabled
camerad's generic 1000/0 exposure). The sensor register + stream-on path now matches
the HAL **byte for byte**. Result: still **no SOF**.

### Root cause is NOT the sensor -- it's camerad's device-start
Precise timeline (debug_mdl dmesg) of a camerad run:
```
2364.902  CAM_START_PHYDEV (CSIPHY 2) ; CSID reset/enable ; IFE camif start
2365.242  sensor stream-on 0x0100=1            (+340 ms after the receiver started)
2366.913  CRM watchdog: "stream on/off delayed"  (no SOF in 1.6 s)
2376.4    irq_status_rx=0x400077  <-- at TEARDOWN (CSID reset), not during streaming
```
So there is **no rx during the actual streaming window** -- the sensor does not emit
MIPI, even though power/MCLK(19.2)/init/QSC/exposure/stream-on all match the HAL, and
the CSID receiver is armed (`config csi2 rx`, `Config IPP Path`). (The single
`rx=0x400077` seen in earlier runs was teardown noise, not a frame.)

The remaining blocker is camerad's **device-start sequence/timing** vs the HAL --
e.g. CSIPHY is started 340 ms *before* stream-on (CPHY may not re-lock onto a sensor
that starts transmitting later), and/or the `CAM_START_DEV` order + the per-request
schedule differ from the OEM HAL. This is the deep, hardware-bring-up tail the port
hits after every register-level issue is resolved -- it needs a HAL-vs-camerad diff
of the full start/`START_DEV`/request flow, not more sensor registers.

| stage | status |
|---|---|
| all sensor registers + stream-on (match HAL byte-for-byte) | **done** |
| sensor emits MIPI under camerad | blocked: camerad device-start order/timing |

## Final isolation: SM8350 CSID/IFE config, not the sensor (2026-06-21)

Two IMX766 modes, two different failures under camerad's start flow:

| mode | regs | under camerad |
|---|---|---|
| **4000x3000** | BASE_INIT(2232)+CAL(4047)+RES(106) | **transmits ~330 frames** (rx=0x400077 x330) but every frame hits `IPP_PATH_ERROR_CCIF_VIOLATION: Bad frame timings` |
| **4096x3072** | BASE_INIT(522)+QSC(3072)+RES(144) | does NOT transmit (no rx during the streaming window) |

The **4000x3000 mode actually streams** -- the sensor emits 30fps MIPI under camerad's
exact start sequence. So the sensor + stream-on path is fine. The CCIF violation is
the **CSID IPP path rejecting each frame's timing before it reaches the IFE**, and it
is independent of:
- datarate: tested 2.4576, 1.9255 (the HAL-confirmed value for this mode), 1.2288 -- all CCIF.
- `frame_offset`: 0 and 2 (embedded-line skip) -- both CCIF.

The OEM HAL streams this exact mode cleanly, so camerad's **CSID/IFE timing
configuration** (the SDM845/Titan-170 register offsets in `configISP` /
`build_initial_config`) does not match what the SM8350 CSID expects for this sensor's
line/frame timing. This is the deepest SoC-specific port piece the docs predicted
(`docs/BUILD-OPENPILOT-0.11.md`: "the full SM8350 IFE register map").

### Bottom line
Every sensor/register-level cause is resolved and verified against the HAL (C-PHY,
datarate, full BASE_INIT+QSC+RES, stream-on bursts). The sensor demonstrably emits
MIPI under camerad in the 4000x3000 mode. The lone remaining blocker is the **SM8350
CSID/IFE register/timing map** in `spectra.cc::configISP` (and the 4096 mode's
device-start ordering) -- a camerad-internals SoC port, not sensor reverse-engineering.

| stage | status |
|---|---|
| sensor emits MIPI under camerad (4000x3000) | **done** |
| CSID IPP accepts the frame (no CCIF) -> SOF/frames | blocked: SM8350 CSID/IFE config |

## BREAKTHROUGH: frames flow through camerad's pipeline via the RDI/RAW path (2026-06-21)

The CCIF violation is on the **IPP (debayer/NV12) path**. Switching the road camera to
the **RDI/RAW path** (`ROAD_CAMERA_CONFIG.output_type = ISP_RAW_OUTPUT`, which uses
`CAM_ISP_IFE_OUT_RES_RDI_0`) bypasses the SM8350 IFE debayer entirely. With the
**4000x3000 mode** (which transmits) + RAW output:

```
irq_status_rdi0 = 0x149338  x329   <-- RDI path RECEIVES 329 frames at 30fps
reg_update / EPOCH          x330   <-- IFE processes them
```

**Camera frames are physically flowing through camerad's hardware pipeline for the
first time** -- sensor -> C-PHY -> CSID RDI -> IFE, 30fps. (The IPP CCIF still fires
but that path is now unused.)

### The lone remaining step: cam_isp context -> userspace delivery
The frames reach the IFE but **do not reach camerad's userspace handler**
(`DEBUG_FRAMES` prints no `frame_id`; `VIDIOC_DQEVENT` gets no CAM_REQ_MGR frame
events). The kernel cam_isp context only advances **twice** (`reg_upd_in_applied` x2)
then the CRM **watchdog pauses the link** (`process_sof_freeze: stream on/off
delayed`), even though RDI keeps delivering frames. Two suspects:
1. camerad still configures the **IPP path** (it sets the pixel window
   left/right/line in `in_port_info` even for RAW), so the cam_isp context waits on
   IPP SOF (which never validates -> CCIF) instead of the RDI path's SOF. Fix:
   configure **RDI-only** (drop the IPP pixel reservation for `ISP_RAW_OUTPUT`).
2. the RDI **write-master `buf-done`** / request sync isn't progressing the context
   (`ife_buf_depth`, sensor pipeline-delay pd=2, or the SM8350 RDI WM config).

This is the final cam_isp/IFE-internals step -- the sensor + PHY + CSID + IFE-RDI all
work and frames physically arrive; only the request/context delivery to userspace
remains.

| stage | status |
|---|---|
| frames physically received by IFE (RDI, 30fps) | **done (329 frames)** |
| frames delivered to camerad userspace (visionipc) | cam_isp context: RDI-only config + WM buf-done |

## RDI write-master stride: 16-byte alignment (SM8350) (2026-06-21)

With RDI/RAW the cam_isp context halted on a BUS violation. Root cause found in the
IFE write-master config:
```
update_wm: before stride 5000
update_wm: Warning stride 5000 expected 5008   <-- camerad set 4000*10/8=5000
... VFE:4 BUS Err IRQ status_0: 0x40000000 -> RDI0 CAMIF VIOLATION -> HW_ERROR -> HALT
```
The SM8350 IFE write-master requires the RAW10 line stride **16-byte aligned**: 5000 ->
**5008**. Fix in `imx766.cc`: `frame_stride = align(frame_width*10/8, 16)`. This cut
the per-frame violations from **30 to 1** and the WM `frame_inc` is now correct
(15024000 = 5008x3000).

### Remaining: RDI write-master never writes -> no buf-done
One violation still halts the context after ~3 frame-starts (`EPOCH` x3) with **zero
buf-done**. The CAMNOC counters show `rdi0_wr [0 0]` -- the write-master is enabled
(`en_cfg 0x10001`, frame-based) but **never writes to memory**, so no fence/buf-done
fires, the request queue drains, and the 4th frame violates. Suspects: the WM image
dims are 0 (frame-based mode) and may need explicit width/height for the SM8350; or
the buffer fence (`fill_fence 1`) / RDI io_cfg isn't wired so the WM never starts.
This is the final IFE write-master/buf-done config -- the frames physically arrive
(rdi0=0x149338 x327); only the WM->memory->buf-done->CRM delivery remains.

## IFE bandwidth: BW_CONFIG_V2 (SM8350) -- usage_data path tag (2026-06-22)

The write-master never wrote (`CAMNOC rdi0_wr [0 0]`) because the IFE had **zero
memory bandwidth** (`cam_min_camnoc_ib_bw = 0`): camerad first sent the **deprecated**
`CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG` (type 2), which the SM8350 silently drops.
Step 1 (`spectra.cc`): send `CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2` (type 9) with a
`cam_axi_per_path_bw_vote` (RDI0 write path, camnoc/mnoc/ddr bandwidth). The kernel
**accepts** it (`generic_blob_handler: blob_type=9, blob_size=64`) -- but the vote
still never reached CPAS.

### Root cause -- MEASURED (the earlier "u64 alignment" theory was WRONG)
A 4-probe bpftrace run (`bwprobe.bt`: `cam_isp_packet_generic_blob_handler`,
`cam_cpas_update_axi_vote`, `cam_cpas_util_apply_client_axi_vote`) settled it:
- `@blob[path_data_type=4, camnoc_lo=2400000000]` -- the kernel reads our `camnoc_bw`
  **correctly** at its 4-mod-8 offset. ARM64 unaligned normal-memory `LDR` returns the
  right value, so misalignment was never the cause.
- `cam_cpas_update_axi_vote` / `apply_client_axi_vote` **never** saw an RDI0 vote --
  the IFE silently dropped it before CPAS.

`cam_isp_classify_vote_info()` (camera-kernel.lnx.5.0) copies an RDI per-path vote into
the IFE's CPAS vote only when **both** hold:
`axi_path[i].usage_data == CAM_ISP_USAGE_RDI (3)` **and**
`path_data_type - CAM_AXI_PATH_DATA_IFE_RDI0 == res_id - CAM_ISP_HW_VFE_IN_RDI0`.
camerad set `usage_data = 0` (`CAM_ISP_USAGE_INVALID`) -> no path matched ->
`num_paths = 0` -> vote skipped -> `camnoc_ib_bw = 0`.

### Fix (verified end-to-end)
`spectra.cc`: `usage_data = is_raw ? CAM_ISP_USAGE_RDI : CAM_ISP_USAGE_LEFT_PX`.
After rebuild the same probe shows the vote flowing all the way through:
`@upd[handle=17, pdt=4, camnoc=2400000000]` and
`@apply[num_paths=1, pdt=4, camnoc=2400000000]`. Bandwidth blocker cleared.

### Remaining blocker: RDI0 CCIF frame-timing -> WM never completes a frame
With bandwidth voted the context still halts on `error_type=8`:
`VFE:4 BUS error image size violation 0 CCIF violation 1` (`RDI0 CAMIF VIOLATION`,
status_0 `0x40000000`). Measured state of a full run (`debug_mdl=0xffff`):
- **Sensor streams fine**: `irq_status_rx=0x400077` x389, `irq_status_rdi0=0x149338`
  x388 -- the CSID receives ~388 frames. (So 4000x3000 *does* transmit under camerad;
  bandwidth client `[17][ife]` is up.)
- **WM fully programmed**: `en_cfg 0x10001`, `image address 0xFC200000`, `stride 0x1390`
  (5008), `frame_inc 15024000`, multiple ring buffers; requests scheduled (ready_map 1).
- **WM never completes a frame**: bpftrace `cam_vfe_bus_ver3_handle_comp_done_top/bottom
  _half` = 0 hits; `cam_sync_signal` x7 are error-flush, not buf_done; 0 `frame_id`.
- **30 CCIF violations / 388 frames** (`RDI :0 PATH_ERROR_CCIF_VIOLATION: Bad frame
  timings`) -- intermittent (~8%) timing mismatch between the sensor frame and the CSID
  RDI config (`height:3000 line_stop 2999 crop_en 1 hblank 0`, `width 4000`).

So the chain is: bad frame timing -> WM cannot cleanly finish a frame -> no comp-done ->
no buf-done -> CRM request queue drains -> a later frame trips the fatal BUS CCIF
violation -> HALT. **Bandwidth is fully ruled out.**

#### Geometry is NOT the mismatch (checked 2026-06-22)
Decoded the active IMX766 mode table (`imx766_registers.h`) and the in_port config:
- sensor output: `X_OUT_SIZE 0x0fa0=4000`, `Y_OUT_SIZE 0x0bb8=3000`, DIG_CROP 4000x3000,
  2x2 binning; `FRM_LENGTH_LINES 0x0c16=3094` (only 94 VBLANK lines), `LINE_LENGTH_PCK
  0x2e50=11856`.
- in_port/CSID: `left 0..3999` (w=4000), `line 0..2999` (h=3000), dt RAW10, vc 0.
So the sensor really does emit 4000x3000 and the CSID expects exactly 4000x3000 -- the
stale "4096x3072" comment in `imx766.cc` is wrong. This is **not** a width/height
mismatch.

#### The CCIF is a STARVATION symptom, not a sensor/PHY timing fault (2026-06-22)
The violation *timestamps* settle the cause. Relative to stream start (28795.666):
- frames **0..~11 stream clean** (no violation) for ~0.38s;
- the **first** CCIF violation (28796.044) is **simultaneous** with the IFE halt
  (`error_type=8`);
- violations then come in **3 bursts of exactly 10** (every ~33ms = per-frame),
  separated by **~4.7s recovery gaps** -- i.e. CRM recovers, re-fills the buffers,
  drains them in ~10 frames, violates, halts, repeats.
With `ife_buf_depth=7` (+ pipeline) ~= 11 frames, this is textbook **buffer
starvation**: the WM never completes a frame (`comp_done=0`), so no buf-done ever
recycles a buffer; the 7 buffers fill, the CRM request queue drains, and the next frame
has nowhere to land -> the camif raises CCIF "bad frame timings" -> HALT. Random PHY
glitches would be uniformly distributed, not "clean for 11 then every-frame bursts".

**So the real bug is: the IFE RDI0 WM never signals frame-complete (comp_done).** PHY
data-rate / embedded-lines / VBLANK are NOT the cause (reception is 388 clean frames).

Ruled out from the kernel (`cam_vfe_bus_ver3.c`, lnx.5.0):
- **WM mode is not the issue / not ours to change**: `default_line_based` is a *static*
  per-WM SoC property set at driver init (`init_wm_resource`, line 1418) from the HW-info
  table; for this IFE-lite RDI WM it is frame-based (`en_cfg=0x10001`, h/w `0xFFFF`),
  which is the intended mode. camerad cannot flip it.
- **Completion is comp_grp-only**: per-WM done handlers are no-ops (`return -EPERM`), so
  the only completion path is the comp_grp-done IRQ (status_0 bit `comp_grp_type +
  buf_done_mask_shift + comp_done_shift` = bit 4 for grp0). The kernel subscribes it
  correctly (`start_comp_grp ... bus_irq_mask_0: 0x10`).
So the WM hardware itself never asserts comp_grp-done even though ~11 frames reach it.

Also ruled out (the **IRQ wiring is correct**), from `cam_vfe_bus_ver3.c` `start_vfe_out`:
- the comp-done bit (`0x10` for grp0) IS subscribed on `buf_done_controller` with the
  vfe_out done handlers (line ~2120) -- independent of the RUP path;
- the IFE-lite `rdi_only_ctx` branch is taken correctly (RUP *is* subscribed, which only
  happens when `rdi_only_ctx==true`), so the lite RDI context is wired as intended.
So subscriptions/handlers are all correct -- the WM HW just never raises the done bit,
i.e. it never reaches end-of-frame. The remaining unknown is purely the data path: does
the WM actually *write* (advance `addr_status_0`) at all, or does the IFE-lite RDI
src/CAMIF-LITE:3 never deliver a complete frame to it?

The **context side is also correct and waiting**: it runs the `rdi_only` state machine
(`__cam_isp_ctx_rdi_only_reg_upd_in_applied_state` -> `Substate[EPOCH]`), i.e. it applied
the request, got REG_UPDATE, advanced to EPOCH, and is waiting for BUF_DONE -- which
never arrives, so after ~2 frames it falls into `__cam_isp_ctx_handle_error` (x5). No
`handle_buf_done` ever runs.

Every layer is therefore confirmed correct and waiting (CSID receive, camif-lite SOF/
EPOCH, WM program, bus comp-done IRQ subscription, rdi_only context) -- the one missing
hardware event is the WM comp-done. Realistic next moves (need device internals or a
working reference; matching monolithic source is CLO `LA.UM.9.14.1.c30`):
1. **fault-safe bpftrace read of WM `addr_status_0`** from the per-frame RUP bottom-half
   (`cam_vfe_bus_ver3_handle_rup_bottom_half`) -- advancing => WM writes (a comp/IRQ
   issue); 0 => WM never writes (src->WM data-path). `bpf_probe_read_kernel` is fault-safe.
   DONE (`scripts/wmaddr.bt`): traversal **validated** (stored `en_cfg`@+108 AND live WM
   `cfg` reg both `0x10001`; `cfg_off=0x1c00`, `astat_off=0x1c68`). `addr_status_0=0x0`.
   CAUTION: reading the WM `image_addr`(+4)/`debug_status`(+108) regs trips an XPU/bus
   violation -> **ramdump** (bpf can't catch bus faults); use only `cfg`(+0)/`addr_status_0`(+88).

#### DECISIVE: the WM gets no data -> CSID->IFE-lite RDI path stalls (2026-06-22)
From the saved dmesg of a full run:
- **only 2 bus RUP IRQs** fire (not ~11), **0 comp_done/buf_done/vfe_out_done**;
- the CSID receives ~388 frames but the IFE-lite CAMIF sees only ~2 -> the **IFE-lite
  stalls after ~2 frames** while the CSID keeps receiving -> back-pressure -> the CSID
  RDI CCIF "bad frame timings" we chased was the *downstream symptom*;
- the rdi_only context gets only REG_UPDATE-class events then ERROR, never DONE.
So the WM never drains/writes a frame, which backs up the IFE-lite, which backs up the
CSID. The WM is correctly enabled (`cfg=0x10001` confirmed live) but receives/writes no
data. **Root is now: the CSID-RDI -> IFE-lite -> WM data path is acquired+started but the
WM never drains a frame.**

Start sequence verified present (monolithic c30 source matches device): CSID:4
`enable_hw` + `cam_ife_csid_start res_type 3 res_id 0` (RDI active -> it *does* receive
388 frames), camif-lite `Start Done`, `start_wm` writes `cfg=en_cfg(0x10001)` +
image_cfg_0 + burst_limit + constraint-detect. Note: for IFE-lite the camif-lite start
**skips core_cfg** (`if (soc_private->is_ife_lite) goto skip_core_cfg`), so the lite RDI
data routing is implicit (CSID-lite -> bus WM, hardwired) -- *not* a mux camerad sets.
So acquire+start are all correct, yet the WM writes nothing and only ~2 reg-updates are
applied before the rdi_only ctx waits forever for a buf_done.

**Static-analysis isolation is now maximal.** The kernel can clearly make a lite-RDI WM
write (the HAL does it), so the missing piece needs a *working reference* to diff against.

#### HAL reference + IPP switch: the WM-no-write is UNIVERSAL (2026-06-22)
bpftrace'd the QCOM HAL while it streamed the ultra-wide (IMX766) via `scripts/halwm.bt`
(per-WM `addr_status_0`): the HAL drives it on **VFE core 2 (a FULL IFE), WM:8,
line-based (`cfg=0x1`), and that WM WRITES** (`addr_status=0xFE2E0000`, 748 RUPs). It does
*not* use the IFE-lite + frame-based RDI that camerad's `ISP_RAW_OUTPUT` lands on.

So switched `ROAD.output_type` -> `ISP_IFE_PROCESSED` (hw.h). camerad now acquires
**VFE:2 (the same full IFE as the HAL)**, full CAMIF (debayer), 2 NV12 WMs (WM:0 4000x3000
+ WM:1 4000x1500), **`en_cfg=0x1` line-based -- identical to the HAL's WM:8**, addresses
programmed, comp_grp0 (`bus_irq_mask 0x40`). The CSID feeds the **IPP path**
(`irq_status_ipp=0x149338`). Result: **still 0 buf_done, ~2 RUPs, then
`IPP_PATH_ERROR_CCIF_VIOLATION` (same backpressure)**.

**Therefore the WM-never-drains bug is universal** -- it reproduces on the IFE-lite RDI
path AND the full-IFE IPP path, with the WM configured exactly like the HAL's working one,
on the exact IFE the HAL uses. It is **not** RDI/lite-specific and **not** bandwidth (RDI's
vote was proven applied). It is a camerad data-flow/sequence gap that stops *any* IFE WM
from draining on this SoC.

#### HAL buf_done confirmed on IFE2; cold-start config capture blocked (2026-06-22)
Captured the HAL streaming the ultra-wide with `debug_mdl=0xffff`: it does **572
`__cam_isp_ctx_handle_buf_done_for_request_verify_addr` + 381 `handle_hw_buf_done`** on
the full IFE -- i.e. its WM writes+completes continuously, exactly what camerad cannot do.
camerad's CAMIF start programs `VFE:2 TOP core_cfg=0xFF9FFFFC`, `epoch_line_cfg=0x1402ED`,
`CAMIF RUP=0x41`, `pix_pattern=0 format=3`. The key core_cfg field is `input_mux_sel_pp`
(`cam_vfe_camif_ver3_resource_start` line 454), set from a `SET_CORE_CONFIG` cmd.
**Blocked:** the HAL keeps its IFE warm across app close/reopen AND across a
`vendor.camera-provider-2-4` restart, so `cam_vfe_camif_ver3_resource_start` never
re-fires -> couldn't capture the HAL's `core_cfg`/CSID start values to diff.

Remaining options to crack the WM-no-write (all need device internals or a cold HAL):
1. read the live `core_cfg_0` TOP register (VFE:2 base, the `0xFF9FFFFC` reg) during HAL
   vs camerad streaming and diff -- but TOP-register reads are XPU-risky (caused a ramdump
   once); needs the exact base+offset and care;
2. force a truly cold HAL IFE (kill cameraserver / longer idle) to log `resource_start`;
3. compare camerad's `SET_CORE_CONFIG`/`input_mux_sel_pp` + the start-cmd *order* against
   the HAL's cdm command stream;
4. trace camerad's own CAMIF SOF generation -- does the full-IFE CAMIF ever emit SOF, or
   does the pixel pipeline never start (which would also explain ~2 RUPs then HALT)?

## camX 2-cam reference app -> camerad root cause = CAMIF-no-SOF (2026-06-22)

A custom camera2 app (`com.test.camx`, source `/tmp/opencode/camxconcurrent`) was modified
to stream **2 cameras: NORMAL (cam0 = IMX689, 73deg) + WIDE (cam2 = IMX766, 99deg)**. Both
stream concurrently via the HAL ("FIRST FRAME captured"). **This proves both sensors + the
HAL path are fine** -- the blocker is purely camerad's IFE config.

Used as a controlled reference (HAL streaming both cams, `debug_mdl=0xffff`) -> **decisive
diff**:
- HAL: drives VFE:0 + VFE:2 (two full IFEs), `cam_vfe_camif_ver3_handle_irq_bottom_half`
  fires every frame (`Received SOF/EPOCH/EOF`), 1123 buf_done.
- camerad (ROAD=`ISP_IFE_PROCESSED`, VFE:2): `cam_vfe_camif_ver3_handle_irq` = **0** --
  the CAMIF NEVER generates SOF -> pixel pipeline never starts -> WM never writes ->
  starvation -> HALT.

**So camerad's root cause is: the full-IFE CAMIF never receives a frame (no SOF).** Ruled
out at register level:
- **core_cfg** (`0xFF9FFFFC`, `input_mux_sel_pp`/`input_pp_fmt` bits): `resource_start` is
  OR-only and reads the same dirty register the HAL does -> not the diff.
- **CSID resume/sync**: camerad's CSID:2 IPP is `sync_mode 0` (NONE) + `IPP Ctrl val:0x1`
  (= `RESUME_AT_FRAME_BOUNDARY` set) -> it IS resumed/outputting; `decode_fmt:2`; it
  processes **389 frames** (`irq_status_ipp=0x149338`).
So the CSID outputs 389 frames but the **CSID:2 -> VFE:2 CAMIF pixel handoff never delivers**
(CAMIF 0 SOF). Next: register-compare the live CSID-output / IFE-input wiring (CSID
`pxl_ctrl`/output dest + IFE pixel input enable) between HAL VFE:2 and camerad VFE:2 (HAL
keeps cameraserver warm, so use a per-frame bpftrace reg read like `scripts/wmaddr.bt`, not
dmesg). `scripts/halwm.bt` + the camX app are the reusable working reference.

## HAL-vs-camerad blob diff + IFE_CORE_CONFIG attempt (2026-06-22)

bpftrace of the HAL's `cam_isp_packet_generic_blob_handler` while the camX app streams the
2 cams shows the HAL sends generic blob types **0(HFR) 1(CLOCK) 6(UBWC_V2) 7(IFE_CORE_CONFIG)
8(VFE_OUT_CONFIG, per-frame ~3236x) 9(BW_V2) 15**. camerad sends only **0, 1, 9**. The
op9 port (openpilot tici code) is missing 6/7/8/15 -- SM8350 blobs the HAL programs.

Captured the HAL's IFE_CORE_CONFIG values (safe kernel-mem read): `input_mux_sel_pp=0`,
`core_cfg_flag=0`, `input_mux_sel_pdaf=0`. Added the matching `IFE_CORE_CONFIG` (type 7)
blob to `spectra.cc` (`tmp.type_3`/`tmp.core`). It IS sent+parsed (`blob_type=7 size=48`
x8) -- **but the CAMIF still gets 0 SOF**. Confirmed: `cam_vfe_camif_ver3` core_cfg is
OR-only (`resource_start` reads the dirty reg and only ORs; `core_config` blob just sets
`cam_common_cfg.*`), and the HAL reads the same dirty `core_cfg=0xFF9FFFFC`, so type 7 is
**not** the SOF lever.

### State: CSID outputs, CAMIF receives nothing
Timeline (debug run): CAMIF `Start Done` + CSID `start` at T; CSID processes **389 frames**
(`irq_status_ipp=0x149338`); first `IPP_PATH_ERROR_CCIF_VIOLATION` only at **T+0.41s
(~12 frames)** = same back-pressure pattern as RDI. So the CSID streams ~12 clean frames
but the VFE:2 CAMIF SOFs **zero** times -> it never receives them -> IFE never drains ->
CSID output FIFO fills -> CCIF -> HALT. **Root: CSID:2 -> VFE:2 CAMIF pixel handoff delivers
nothing**, and it is NOT `input_mux` (OR-only, HAL-identical).

Remaining candidates for the handoff (next session): (1) **VFE_OUT_CONFIG (type 8)** -- the
per-frame output cfg the HAL sends and camerad omits; (2) the **IFE-top mux** (`cam_vfe_top
_ver3` res-id-0) routing CSID->CAMIF; (3) the CSID IPP **decode_fmt:2** / output-enable vs
the HAL's. ROAD is now `ISP_IFE_PROCESSED` (on VFE:2, the HAL's IFE) + IFE_CORE_CONFIG sent
-- both committed as progress toward mirroring the HAL, though frames are still blocked.

### How the bandwidth fix was proven (reusable probe)
`scripts/bwprobe.bt` + a chroot bpftrace runner (see `scripts/qsc_bpftrace.md` for the
chroot/explicit-linker recipe) attach to `cam_isp_packet_generic_blob_handler` (blob
bytes), `cam_cpas_update_axi_vote` + `cam_cpas_util_apply_client_axi_vote` (CPAS vote).
Pre-fix: blob carried `[pdt=4, camnoc=2400000000]` but no CPAS vote. Post-fix:
`@upd[handle=17, pdt=4, camnoc=2400000000]` + `@apply[1, 4, 2400000000]`.

### Status: frames physically captured; 2-sensor end goal
| piece | state |
|---|---|
| IMX766 frames in IFE hardware (RDI, 30fps) | done |
| IMX766 stride (16-byte align) | done |
| IMX766 IFE bandwidth (BW_CONFIG_V2 + usage_data=RDI) | **done (vote applied, bpftrace-verified)** |
| IMX766 RDI0 CCIF violation | understood -- it's a *symptom* of WM-no-write starvation |
| IMX766 IFE-lite RDI0 WM writes/drains a frame | **open -- current blocker** (WM enabled but writes nothing) |
| IMX766 frames -> userspace/VisionIPC | after WM-write fix (HAL RAW ref or IPP re-test) |
| IMX689 (2nd sensor) streaming | needs its BASE_INIT+QSC+RES captured (bpftrace) |
