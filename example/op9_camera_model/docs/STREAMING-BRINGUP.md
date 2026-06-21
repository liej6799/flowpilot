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

## Open issue: no MIPI output (CSID irq_status_rx = 0)

Everything configures cleanly but the sensor emits no data. Candidates, in order:

1. **QSC / init ordering.** The HAL applies init -> QSC -> res; we apply init+res
   with no QSC. Even though 2x2 binning is not the remosaic mode, the res table may
   enable a path that stalls without QSC loaded. Next: capture the full 3072-reg QSC
   (52 kprobe windows) and apply init -> QSC -> res in HAL order.
2. **Initial exposure.** The HAL writes small exposure/gain bursts (size 8/11)
   before/with streamon. Try applying a sane default 0x0202/0x0204 before 0x0100.
3. **Streamon mechanism/timing.** Confirm 0x0100=1 reaches slave 0x34 and that it is
   the last write after CSID/IFE/CSIPHY are started.

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
