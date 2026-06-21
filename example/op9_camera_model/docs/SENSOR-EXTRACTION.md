# Extracting IMX689 / IMX766 sensor data from LineageOS source

The single biggest piece of new work for live capture is the per-sensor data:
probe ID, I2C init/start register arrays, power sequence, MCLK, and the MIPI
CSIPHY settle/datarate. openpilot has no IMX driver, so we derive it from the
OnePlus 9 (LineageOS) kernel + device tree.

## Sources

- Kernel: `github.com/LineageOS/android_kernel_oneplus_sm8350` (branch
  `lineage-23.2`), camera UAPI under
  `techpack/camera/include/uapi/camera/media/`.
- Device tree (the per-sensor config Qualcomm cameras use instead of in-driver
  register arrays): the OnePlus 9 `lemonade` dtsi camera nodes, e.g.
  `arch/arm64/boot/dts/vendor/qcom/...camera-sensor-*.dtsi`.
- Confirmed live from the device (dmesg): sensor_id **0x766 (IMX766)** on
  **CSIPHY_IDX 2**, I2C slave **0x34**, datarate **5792900000** (5.7929 Gbps),
  settle **2800000000**. IMX689 = sensor_id **0x689**.

## What you need to fill in (src/sensors/imx766.h, imx689.h)

For each sensor, a `SensorInfo`-equivalent:

| field | source |
|---|---|
| `probe_reg_addr`, `probe_expected_data` | sensor id register (0x0016/0x0017 on Sony IMX = 0x0766 / 0x0689) |
| `slave_addr` | dmesg / dtsi = 0x34 (IMX766) |
| `mclk_frequency` | dtsi `clock-rates` (typically 24 MHz) |
| `init_reg_array[]` | the streaming-mode register settings. Qualcomm puts these in the **camera HAL** (`vendor/etc/camera/*.xml` sensor libs) or a sensor `.so`, NOT the kernel. Two ways to get them: (a) dump from the HAL config, or (b) capture the i2c writes the HAL does via the kernel's cam_sensor i2c trace (ftrace / the CAM_DBG i2c logs) while the Android camera streams. |
| `start_reg_array[]` | the "stream on" write (Sony: reg 0x0100 = 0x01) |
| power sequence | dtsi `cam_vana`/`cam_vdig`/`cam_vio` regulators + reset GPIO order |
| CSIPHY `settle`/`data_rate` | from dmesg above for IMX766; for IMX689 recompute from its line rate (openpilot's `camera_freqs.py` does this math) |

### Easiest extraction: trace the HAL's i2c writes

With the Android camera HAL running and streaming (e.g. open the flowpilot app),
the kernel cam_sensor driver logs/handles every i2c register write. Enable the
camera i2c debug and capture the init burst:

```sh
# on device (root)
echo 0x8 > /sys/module/cam_sensor/parameters/debug_mask   # if available, or use ftrace
# trace_event: cam_sensor i2c writes  (CAM_DBG CAM_SENSOR)
# then open the camera; the dmesg / trace will show reg/val pairs.
```

Those reg/val pairs ARE the `init_reg_array`. This avoids needing the proprietary
sensor `.so`. (The QSC/oplus calibration step seen in dmesg is extra; the base
streaming init is the i2c burst.)

### Alternative: pull from the camera HAL sensor libs

```sh
ls /vendor/lib64/camera/components/ /vendor/lib64/camera/
# com.qti.sensor.imx766.so etc. -- the register tables are embedded here.
```

## CSIPHY / IFE input

- IMX766 uses CSIPHY index 2 -> IFE input `CAM_ISP_IFE_IN_RES_PHY_2`.
- Find IMX689's CSIPHY index the same way (dmesg while the main wide streams, or
  the dtsi `cci-master`/`phy-sd-index`).
- Data type: Sony sensors stream **RAW10** in normal modes -> `CSI_RAW10`,
  `CAM_FORMAT_MIPI_RAW_10`.

## Output path choice

For a first working capture, use the **RDI/RAW** ISP out port
(`CAM_ISP_IFE_OUT_RES_RDI_0`, `CAM_FORMAT_MIPI_RAW_10`). This dumps raw Bayer and
**avoids all SM8350-specific IFE debayer/NV12 register offsets** (the hard part).
You then debayer in software (or feed a model variant). Move to hardware NV12
(`CAM_ISP_IFE_OUT_RES_FULL`, `CAM_FORMAT_NV12`) only after RAW works, and only
after deriving the SM8350 IFE register map.
