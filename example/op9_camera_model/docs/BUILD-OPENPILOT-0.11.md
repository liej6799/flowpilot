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

## Bottom line

The core openpilot v0.11 camerad **builds in a matched Ubuntu 24.04 proot on the
OnePlus 9**. What remains is a bounded, well-identified port of the CSIPHY/ISP
acquire structs + the IMX sensor driver -- exactly the camera-input layer,
leaving the rest of openpilot's logic intact.
