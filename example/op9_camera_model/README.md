# op9_camera_model

Raw camera access on the **OnePlus 9 (LE2110, SM8350 / Snapdragon 888)** for
running the **openpilot v0.11 driving model** with **no Android frontend** —
reading frames straight from the Qualcomm **Spectra ISP** via the
`cam_req_mgr` kernel interface, the same way openpilot's `camerad` does on comma
hardware.

Goal: open the two road cameras (wide + narrow), get raw/NV12 frames directly
from the ISP, feed them to the v0.11 supercombo model, and measure inference
speed — all inside the Termux **proot Linux** environment, bypassing
`ai.flow.android` / Camera2 entirely.

## Status / honesty

This repo is in **stages**. What runs today vs. what still needs the
SoC-specific reverse-engineering is clearly marked.

### ✅ First frame captured (IMX689 → camerad → PNG)

A real IMX689 frame was captured straight through the **Spectra ISP via `camerad`** (no Camera2)
and dumped to PNG — the full `cam_req_mgr` acquire→link→start→stream→**buf_done** pipeline works.

![IMX689 first frame](docs/imx689-first-frame.png)

The working code is the camerad port (`cameras/spectra.cc`, `cameras/hw.h`, `sensors/imx689.cc`)
plus the device-kernel diffs in `patches/camera-kernel-sm8350.patch`. The full recipe (the
`hw.h` PHY-macro bug, RDI + `RDI_SOF_EN`, forcing a full IFE via `can_use_lite=false`, and the
decisive **frame-based WM** `wm_mode=1`) is written up in `docs/STREAMING-BRINGUP.md`.

| Piece | State |
|---|---|
| `cameras/spectra.cc` — camerad RDI capture (acquire→link→start→**buf_done**→raw dump) | **WORKS** — frame captured |
| `src/cam_probe.c` — open `cam_req_mgr`/`cam_sync`/ISP, `CAM_QUERY_CAP`, get IOMMU handles | **WORKS** (validated on device) |
| `src/spectra_capture.c` — full acquire→link→start→stream sequence | **scaffold**: correct ioctl flow, OnePlus-9 sensor/IFE magic = `TODO` |
| `src/sensors/imx766.h`, `imx689.h` — sensor init/start register arrays, power seq | **TODO** (extract from LineageOS kernel, see `docs/SENSOR-EXTRACTION.md`) |
| `model/bench.py` — load v0.11 supercombo, 2-cam tensors, measure inference Hz | **WORKS** (synthetic frames; real frames wire in once capture streams) |

The hard, device-specific deliverables (documented, not guessed):
1. **IMX689 + IMX766 sensor data**: probe IDs, I2C init/start register arrays,
   power sequence, MCLK, MIPI settle/datarate. Source: the OnePlus 9 LineageOS
   kernel `techpack/camera` sensor drivers / Sony datasheets.
2. **SM8350 IFE register offsets** (only if you want hardware NV12 instead of
   RAW Bayer). The easy path uses the **RDI/RAW** ISP output and skips these.
3. **SM8350 `media/cam_*.h` UAPI headers** — must match the OnePlus 9
   camera-kernel, not comma's SDM845 copies.

## Why (the motivation)

The Android-app camera path on this phone forces the **ultra-wide IMX766** lens
through Camera2 with digital-zoom warping, which gives the model a distorted
~99° FOV it wasn't trained for → poor depth/lead estimation. Direct ISP access
lets us:
- pick the right sensor/lens (IMX689 main wide) and feed a clean, undistorted
  frame at the correct intrinsics,
- get low-latency raw/NV12 directly (no Camera2 → JPEG/warp pipeline),
- run a true **two-camera** (wide + narrow) v0.11 model.

## Hardware (probed)

- SoC: Qualcomm SM8350, Spectra (Titan) ISP, kernel `5.4.268-qgki`
- Camera kernel nodes: `/dev/video0`=`cam-req-mgr`, `/dev/video1`=`cam_sync`,
  `cam-isp`, `cam-icp`, 6× `cam-csiphy-driver`, 4× `cam-sensor-driver`
- Sensors (by `sensor_id`): **0x766 IMX766** (50MP), **0x689 IMX689** (48MP),
  0x471, 0x2 (mono). IMX766 seen streaming on **CSIPHY_IDX 2**, I2C `0x34`,
  datarate `5.7929 Gbps`, settle `2.8 µs`.

## The blocker, and how we get around it

`/dev/video0` is normally held by the Android camera HAL
(`android.hardware.camera.provider@2.4-service`, → `open()` = `EALREADY`).
**Confirmed**: `setprop ctl.stop vendor.camera-provider-2-4` releases it, and the
proot (as root) can then open `cam_req_mgr` directly. Restart with
`ctl.start vendor.camera-provider-2-4`. See `scripts/hal.sh`.

## Build / run

```bash
# in the proot (sudo login-flowpilot-root), from this dir
make probe                      # builds + runs the validated cam_req_mgr probe
./scripts/hal.sh stop           # release the camera from Android HAL
sudo ./build/cam_probe          # open nodes, query cap, print IOMMU handles
./scripts/hal.sh start          # give the camera back to Android

# model inference benchmark (works now, synthetic frames)
python3 model/bench.py --model /path/to/supercombo.onnx --runs 200
```

## Layout

```
src/cam_probe.c        validated: open nodes + CAM_QUERY_CAP + IOMMU handles
src/spectra_capture.c  full Spectra acquire/stream scaffold (TODOs marked)
src/cam_uapi.h         minimal cam_req_mgr/cam_defs structs+opcodes (port to SM8350)
src/sensors/           imx766.h, imx689.h  (register arrays = TODO)
sensors/imx766.cc      IMX766 (ultrawide/road) SensorInfo: C-PHY, 2.4576 Gbps; init built at runtime
sensors/imx689.cc      IMX689 (main wide) SensorInfo -- 2-camera support; CSIPHY 1
sensors/sensor_qsc.h   build_sensor_init(): splice per-unit EEPROM QSC into the model-constant init
sensors/generated/<name>_mode_init.h   model-constant init tables (QSC removed; NO per-unit data)
sensors/generated/<name>_init_meta.json EEPROM QSC window offsets for the generator
tools/gen_sensor_init.py  reconstruct/verify the full init from tables + a runtime EEPROM image
docs/SENSOR-CALIBRATION-EEPROM.md  proof the QSC blob = per-unit EEPROM copy + integration design
patches/camerad-v0.11.1-sm8350.patch  the full openpilot v0.11.1 -> OnePlus 9 port (regen from tree)
model/bench.py         v0.11 supercombo two-camera inference-speed benchmark
model/frames.py        NV12/YUV420 frame formatting for the model input
scripts/hal.sh         stop/start the Android camera HAL
docs/SENSOR-EXTRACTION.md   how to pull IMX689/IMX766 data from LineageOS source
docs/SPECTRA-SEQUENCE.md    the exact ioctl sequence (from openpilot camerad)
docs/STREAMING-BRINGUP.md   IMX766 streaming: the C-PHY/datarate fix that got MIPI flowing
```

## camerad port status (openpilot v0.11.1 on OnePlus 9)

Apply `patches/camerad-v0.11.1-sm8350.patch` to a clean openpilot v0.11.1. The
sensor streaming init is **no longer a checked-in register blob** — it is built at
runtime from the model-constant tables in `sensors/generated/*_mode_init.h` plus
this unit's QSC calibration read from the sensor EEPROM
(`/mnt/vendor/persist/camera/eeprom_*.bin`) via `sensors/sensor_qsc.h`. So one
build runs on any unit, with zero per-unit data compiled in. See
`docs/SENSOR-CALIBRATION-EEPROM.md`. (The patch is a snapshot of this tree;
regenerate it from the tree before deploying.)

| stage | status |
|---|---|
| compile + link, runtime, IOMMU/session | done |
| IMX766 + IMX689 probe + acquire (2 rear cams) | done |
| ION buffers, IFE acquire + config, CSIPHY start | done |
| **IMX766 emits MIPI (C-PHY + 2.4576 Gbps fix)** | **done -- `irq_status_rx=0x400077`** |
| **CCIF frame-timing (coherent 4096x3072 mode)** | **done -- violation gone** |
| sensor SOF / frames | done -- both rear cameras capture real frames |
| QSC shading table source | solved -- per-unit EEPROM copy, read at runtime |

Three big unlocks (see `docs/STREAMING-BRINGUP.md` + `docs/SENSOR-CALIBRATION-EEPROM.md`):
1. The IMX766 is **C-PHY** (3 trios), not D-PHY, at **2.4576 Gbps** (1.9255 was
   IMX689's) -- that got MIPI flowing.
2. The HAL streams a **4096x3072** mode (`BASE_INIT + QSC + RES`), not the old
   4000x3000 capture -- switching to it cleared the CCIF frame-timing error.
3. The **QSC** block (the bulk of the init that looked like an opaque blob) is
   **per-unit factory calibration the HAL copies byte-for-byte out of the sensor
   EEPROM**. Proven byte-exact (`tools/gen_sensor_init.py --verify`). So it is read
   from the device at runtime, not hard-coded -- no per-unit data in the repo.

## Credits / references

The ioctl sequence is reverse-engineered from **commaai/openpilot**'s
`system/camerad` (`spectra.cc`, `camera_qcom2.cc`, `sensors/*`). This is a clean
reimplementation for the OnePlus 9 sensors; no comma code is copied.
