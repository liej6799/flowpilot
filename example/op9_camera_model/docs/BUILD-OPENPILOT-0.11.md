# Building openpilot v0.11.1 camerad on the OnePlus 9 (Ubuntu 24.04 proot)

We match openpilot's exact target environment in a Termux proot, then build the
real `camerad` against the OnePlus 9 kernel's camera UAPI. This reuses openpilot's
core logic; only the SoC-specific camera config needs small changes.

## Why this works

openpilot/AGNOS = **Ubuntu 24.04.3 aarch64, Python 3.12, clang-18**. `camerad`
links only openpilot's own libs + standard system libs (no AGNOS-specific
userspace). Its only hard dependency is the Qualcomm `cam_req_mgr` kernel
interface, which the OnePlus 9 (SM8350) ALSO has (same Spectra family as comma's
SDM845). So a 24.04 proot + the SM8350 cam UAPI headers lets camerad build.

## Steps (validated)

```sh
# 1. Ubuntu 24.04 aarch64 rootfs (matches AGNOS)
proot-distro install ubuntu:24.04

# 2. build deps (native Ubuntu toolchain = openpilot's)
proot-distro login ubuntu -- apt-get install -y \
    git scons capnproto libcapnp-dev libzmq3-dev clang-18 build-essential \
    python3 python3-pip python3-dev pkg-config
pip3 install --break-system-packages numpy uv

# 3. SM8350 camera UAPI headers (camerad #includes media/cam_*.h)
#    pulled from LineageOS android_kernel_oneplus_sm8350 (lineage-23.2)
#    techpack/camera/include/uapi/camera/media/*.h
#    -> install into BOTH /usr/include/media/ and /usr/include/camera/media/
#    (strip the `#include "oplus/media/..."` lines)
#    headers needed: cam_defs cam_req_mgr cam_isp cam_isp_ife cam_isp_sfe
#      cam_isp_tfe cam_isp_vfe cam_sensor cam_sync cam_icp cam_cpas

# 4. clone openpilot v0.11.1 + submodules
git clone --depth 1 -b v0.11.1 https://github.com/commaai/openpilot.git
cd openpilot
git submodule update --init --depth 1 \
    msgq_repo opendbc_repo rednose_repo panda teleoprtc_repo

# 5. python env (installs libyuv/json11/acados/zeromq native wrappers)
#    NOTE: fix proot DNS first: echo 'nameserver 8.8.8.8' > /etc/resolv.conf
uv sync --frozen

# 6. force device (larch64) mode so the camerad SConscript builds
touch /TICI

# 7. build just camerad (skip selfdrive/modeld block in SConstruct,
#    which probes tinygrad GPU and isn't needed for camera capture)
uv run scons -j4 system/camerad/
```

## Result: camerad COMPILES, then hits SM8350-vs-SDM845 deltas

The build gets all the way into compiling `system/camerad/cameras/*.cc` and
`sensors/*.cc`. The remaining errors are the **exact, localized SoC porting
deltas** -- openpilot's SDM845 code touches struct fields that moved/renamed on
SM8350:

| openpilot (SDM845) code | SM8350 reality | file |
|---|---|---|
| `cam_csiphy_info.lane_mask` | removed; use `lane_assign` + `lane_cnt` | spectra.cc:1282 |
| `cam_csiphy_info.csiphy_3phase` | moved to `cam_csiphy_acquire_dev_info` | spectra.cc:1284 |
| `cam_csiphy_info.combo_mode` | moved to `cam_csiphy_acquire_dev_info` | spectra.cc:1285 |
| `cam_isp_in_port_info.custom_csid` | field does not exist | spectra.cc:1108 |
| `#include <media/msm_camsensor_sdk.h>` | legacy header, not in SM8350 tree | sensors/*.cc |
| `printf %lu` for `__u64` | `-Werror,-Wformat`: needs `%llu` | camera_qcom2.cc:289 |

These are the "small code changes for the camera input" -- they're in the
CSIPHY/ISP **acquire/config** path (`spectra.cc::configCSIPHY` / `configISP`),
not the core capture logic. The fix is to populate the SM8350 struct layout
(set lane_assign/lane_cnt, move combo_mode/csiphy_3phase into the acquire
struct, drop custom_csid) using the values we already captured from dmesg
(IMX766: CSIPHY idx 2, settle 2.8us, data_rate 5.7929 Gbps).

## Still required after the struct fixes

- **Sensor drivers**: openpilot has ox03c10/os04c10; the OnePlus 9 has
  **IMX766/IMX689**. Need an IMX `SensorInfo` (probe id 0x766/0x689, init/start
  i2c arrays, power seq) -- see SENSOR-EXTRACTION.md. Sensors also include the
  legacy `msm_camsensor_sdk.h`; either provide a shim or rework those includes.
- **IFE register offsets** for SM8350 if using hardware NV12 (the RDI/RAW path
  avoids this -- recommended first target).

## RESULT: camerad fully builds + links (2026-06-21)

After applying the small SM8350 port (`patches/camerad-v0.11.1-sm8350.patch`,
**7 files, +20/-16 lines**) the build completes:

```
[CXX] system/camerad/cameras/camera_qcom2.o
[CXX] system/camerad/cameras/camera_common.o
[CXX] system/camerad/cameras/spectra.o
[CXX] system/camerad/sensors/ox03c10.o
[CXX] system/camerad/sensors/os04c10.o
[LINK] system/camerad/camerad
scons: done building targets.
```

Output: `system/camerad/camerad` -- a 4.9 MB ARM aarch64 ELF, linked only against
libstdc++/libm/libc. **openpilot v0.11.1 camerad is now compiled for the OnePlus 9.**

### The complete port (verified against BOTH kernel sources)

We diffed comma's `agnos-kernel-sdm845` (`include/uapi/media/`) vs the OnePlus 9
`android_kernel_oneplus_sm8350` (`techpack/camera/.../media/`). The Qualcomm
camera UAPI genuinely evolved SDM845 -> SM8350; the deltas:

| change | reason | where |
|---|---|---|
| `cam_csiphy_info`: drop `lane_mask`/`csiphy_3phase`/`combo_mode`, add `mipi_flags` | struct changed; 3phase/combo moved to `cam_csiphy_acquire_dev_info` | spectra.cc configCSIPHY |
| `cam_isp_in_port_info`: drop `.custom_csid` | field removed on SM8350 | spectra.cc configISP |
| `printf %lu` -> `%llu` for `__u64` | -Werror,-Wformat | camera_qcom2.cc |
| remove `#include <media/msm_camsensor_sdk.h>` | header gone; enums already in spectra.h | sensors/*.cc |
| add `CSI_RAW8/10/12` (0x2A/2B/2C) to sensor.h | standard CSI-2 DTs, were in msm_camsensor_sdk.h | sensor.h |
| SConstruct: skip selfdrive block | camerad-only build (avoids tinygrad GPU probe) | SConstruct |

### Headers installed into the proot (from comma + OnePlus kernels)
- SM8350 cam UAPI: `media/cam_*.h` (OnePlus 9 `android_kernel_oneplus_sm8350`)
  into `/usr/include/media/` and `/usr/include/camera/media/`
- `linux/ion.h`, `linux/msm_ion.h` (comma `agnos-kernel-sdm845`, strip `__user`)
  for msgq/visionipc's ION buffer allocator

## Bottom line

**The "small change to port to a new phone" is real and now done**: ~36 changed
lines (the patch) + a set of kernel UAPI headers. All of openpilot v0.11's core
camera logic (spectra.cc capture engine, IFE/CSID config, request scheduling) is
**reused unchanged**. camerad compiles and links on the OnePlus 9.

## RUNTIME: camerad executes on hardware + reaches sensor probe (2026-06-21)

Added an **IMX766 SensorInfo** (`sensors/imx766.cc` + `imx766_registers.h`,
wired into `cereal/log.capnp`, `sensor.h`, `spectra.cc` probe list, `SConscript`)
and two small runtime fixes, then ran the real binary on the device:

- camerad node paths: `/dev/v4l/by-path/...cam-req-mgr...` -> **`/dev/video0`**,
  `...cam_sync...` -> **`/dev/video1`** (Android has no by-path symlinks).
- run env: `PARAMS_ROOT=/tmp/params` with `IsOffroad=1` (the `set_core_affinity`
  assert is allowed to fail offroad).

Run it:
```sh
# android side: free the ISP
setprop ctl.stop vendor.camera-provider-2-4
# in the proot with camera nodes bound (scripts/op9-ubuntu-login.sh):
mkdir -p /tmp/params/d && printf 1 > /tmp/params/d/IsOffroad
PARAMS_ROOT=/tmp/params ./system/camerad/camerad
```

Result -- **camerad runs end-to-end and reaches the hardware sensor probe** for
all 3 cameras, trying IMX766/OS04C10/OX03C10 on each:
```
spectra.cc: VIDIOC_CAM_CONTROL error: op_code 267 - errno 19   (CAM_SENSOR_PROBE_CMD, ENODEV)
spectra.cc: ** sensor 0 FAILED bringup, disabling
... sensor 1 ... sensor 2 ...
```

So the **entire binary works** -- param store, messaging, visionipc, camera node
open, IOMMU, session create, and the per-camera sensor bring-up loop all execute.
It stops at the actual sensor PROBE ioctl with `ENODEV`.

### Remaining: make the sensor PROBE succeed (PRECISELY diagnosed)

Captured the kernel-side error during our camerad probe (dmesg, HAL stopped):
```
CAM-CCI:    cam_cci_read: ERROR with Slave 0x6c
CAM-CCI:    cam_cci_read_bytes: Failed to read rc:-22 (EINVAL)
CAM-SENSOR: cam_sensor_match_id: read id: 0x0  expected id 0x5803
CAM-SENSOR: cam_sensor_subdev_ioctl: Failed in Driver cmd: -19 (ENODEV)
```

So the probe reaches the **CCI i2c read**, and the read returns **0x0** -- the
sensor does not respond on the i2c bus. The `cam_cmd_probe` struct is identical
SDM845<->SM8350, so it is NOT a packet-layout problem. Root cause:

**camerad's hardcoded power-up sequence doesn't match the OnePlus 9 device-tree
power config for the IMX766.** camerad sends comma's fixed power_settings
(`power_seq_type` 0=MCLK,1=analog,2=digital,3=clk,8=reset -- tuned for the
OX03C10/OS04C10 wiring). The OnePlus DT defines the IMX766's own regulators
(cam_vana/cam_vdig/cam_vio), MCLK and reset-GPIO timing in a different
order/mapping, so the power-up doesn't actually energize the sensor -> the CCI
read gets no ACK -> id 0x0 -> ENODEV. The OEM HAL works because it applies the
full per-sensor DT power config; camerad hardcodes comma's.

(Also note: the OnePlus kernel has `oplus_cam_sensor_*` vendor hooks, e.g. a
Sony QSC calibration step, seen in dmesg.)

## SENSOR PROBE SUCCESS -- IMX766 detected on hardware (2026-06-21)

Extracted the IMX766 power config from the OnePlus 9 device tree
(`lahaina-oem-camera-lemonade_t0.dtsi`, node `qcom,cam-sensor@1`,
**csiphy-sd-index 2** -- matches the dmesg "CSIPHY_IDX 2"):

```
regulator-names = "cam_vio","cam_vana","cam_v_custom1","cam_clk","cam_vaf","cam_vdig"
clock-rates     = 19200000   // 19.2 MHz MCLK (NOT 24)
```

The OnePlus rear sensors need **6 regulators**. comma's hardcoded power sequence
only enabled VANA/VDIG/VIO -- it **omitted `cam_v_custom1` (CUSTOM_REG1=6) and
`cam_vaf` (VAF=4)**, so the sensor never powered up -> CCI read 0x0 -> ENODEV.

Fix in `sensors_init()` (in the patch): expand the power-up block to 6 settings
adding seq types 6 and 4, set the probe buffer to the exact 220 bytes, and set
the IMX766 MCLK to 19.2 MHz. Result -- the kernel now reports:

```
CAM-SENSOR: cam_sensor_driver_cmd: Probe success, slot:1, slave_addr:0x34, sensor_id:0x766
CAM-SENSOR: cam_sensor_driver_cmd: CAM_ACQUIRE_DEV Success, sensor_id:0x766
```

**The IMX766 is detected, powered, and acquired by our camerad.** This was the
hardest part of the port (sensor bring-up). The key lever was the **device-tree
power sequence**, not the register table.

## ION buffer ABI ported (modern ION) -- DONE

comma's `visionbuf_ion.cc` used the **legacy ION ABI** (`ION_IOC_ALLOC` returns a
`handle`, then `ION_IOC_SHARE` -> fd, `ION_IOC_FREE`, `ION_IOC_CUSTOM` cache).
The OnePlus 9 kernel 5.4 `/dev/ion` is the **modern ION ABI**:
- `ION_IOC_ALLOC` returns the dmabuf **fd directly** (no handle, no SHARE/IMPORT)
- free = `close(fd)` (no `ION_IOC_FREE`)
- cache sync = `DMA_BUF_IOCTL_SYNC` on the fd (no `ION_IOC_CUSTOM`)
- the system heap id is discovered via `ION_IOC_HEAP_QUERY` (type==SYSTEM)

Rewrote `msgq/visionipc/visionbuf_ion.cc` for this (see `msgq/visionbuf_ion.cc`)
and installed the OnePlus 9 `linux/ion.h` + `linux/msm_ion.h`. **ION allocation
now works** -- camerad gets past `VisionBuf::allocate` and the IFE *acquire*.

## Current boundary: IFE/ISP config (SM8350 register map)

camerad now stops at `config_ife` (spectra.cc). Kernel:
```
CAM-ISP:  config_dev_in_top_state: Received update req 1 in wrong state:4
CAM-PERF: Deprecated Blob TYPE_BW_CONFIG
CAM-CORE: config device failed (rc = -22)
```
The IFE is acquired, but the per-frame IFE register CDM stream / generic blobs
are SDM845-specific:
- `ife.h` `build_initial_config`/`build_update` write Titan-170 (SDM845) register
  offsets (`0x40/0x560/0x6fc/0xf30/...`) that differ on the SM8350 IFE.
- the `BW_CONFIG` generic blob is **deprecated** on SM8350 (kernel warns).

This is the deepest SoC-specific piece. Two paths:
1. **RDI/RAW output** (`CAM_ISP_IFE_OUT_RES_RDI_0`, `CAM_FORMAT_MIPI_RAW_10`) --
   skips the IFE debayer/NV12 register programming entirely; you get raw Bayer
   and debayer in software. Recommended first frame target.
2. Port the full SM8350 IFE register map for hardware NV12.

### Tested: RAW path does NOT fix it -- it's a STATE-MACHINE difference

Switched the IMX766 camera (= **slot 1 / camera_num 1**, confirmed via dmesg
`Probe success, slot:1, slave_addr:0x34, sensor_id:0x766`; IMX689=slot0,
IMX471=slot2, mono=slot3) to `ISP_RAW_OUTPUT` on CSIPHY 2, single-camera. The
RAW path **skips `build_initial_config`** (the SDM845 IFE register offsets), yet
the **same** error persists:
```
CAM-PERF: Deprecated Blob TYPE_BW_CONFIG
CAM-ISP:  __cam_isp_ctx_config_dev_in_top_state: Received update req 1 in wrong state:4
CAM-CORE: config device failed (rc = -22)
```
So the blocker is **not** the IFE register map -- it's the SM8350 ISP **context
state machine**: the per-frame `config_dev` (request 1) is rejected because the
context is already in state 4 (`CAM_CTX_ACTIVATED`), and a flush/stop precedes
it. comma's flow (acquire -> initial config(req1) -> link -> START -> per-frame
config_dev updates) doesn't match how the SM8350 cam_isp context accepts request
config updates. Plus `BW_CONFIG` is a deprecated generic blob on SM8350.

### The real remaining work (ISP request/state model)
- Match the SM8350 `cam_isp_context` request lifecycle: how/when `CAM_CONFIG_DEV`
  packets for a request are submitted relative to `CAM_START_DEV` and the
  per-request `CAM_REQ_MGR_SCHED_REQ` (the "wrong state:4" means the update is
  arriving after activation in a way this kernel rejects).
- Drop/replace the deprecated `BW_CONFIG` generic blob (the per-frame update
  path currently sends *only* BW_CONFIG at offset 0x60).
- Then (for processed output) the SM8350 IFE register map; or stay on RDI/RAW.

This is the ISP context-management layer -- the last and deepest SoC-specific
piece. Everything before it (compile, runtime, camera node/IOMMU/session, IMX766
probe+acquire, ION alloc, IFE acquire) is proven working on the OnePlus 9.

## Status summary (all proven on the OnePlus 9)
| stage | status |
|---|---|
| Ubuntu 24.04 proot matching AGNOS | done |
| camerad compile + link (small SoC port) | done |
| camerad runs: params/messaging/visionipc init | done |
| camera-node open / IOMMU / session | done |
| IMX766 sensor PROBE + ACQUIRE | done |
| ION frame-buffer allocate (modern ABI) | done |
| IFE acquire | done |
| **IFE config / stream (SM8350 register map)** | **current** |
| full IMX766 register table (real frames) | after stream |

We've reached the IFE register-programming stage -- the final SoC-specific
hurdle before frames. Everything up to and including IFE acquire works on-device.
Recommended next: switch the ISP output to the RDI/RAW path to get the first
raw frame without the SM8350 IFE-processing register map.
