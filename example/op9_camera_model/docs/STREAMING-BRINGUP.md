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
```
