# Raw Camera Access on the OnePlus 9 (LE2110) — Investigation

Goal: get raw camera frames in the proot Linux env the way openpilot's `camerad`
does on comma hardware (Qualcomm Spectra ISP via `cam_req_mgr` ioctls), instead
of going through the Android app's Camera2 path (which forces the ultra-wide
cam, warps the image, and gives us the intrinsic/lead-detection problems).

## TL;DR
- The kernel exposes the **full Qualcomm Spectra camera stack** — same family
  comma uses. The proot (as root) can **see and open** the device nodes.
- **Blocker:** the capture control nodes (`/dev/video0` = `cam-req-mgr`,
  `/dev/video1` = `cam_sync`) are **held exclusively by the Android camera HAL**
  `android.hardware.camera.provider@2.4-service` (PID 1035) → open() returns
  `EALREADY`.
- These are **NOT** plain V4L2 `/dev/videoN` capture devices (you can't
  `cv2.VideoCapture` them). They are Qualcomm proprietary ioctl interfaces. The
  openpilot "USE_WEBCAM" `/dev/videoN` path does NOT apply here.
- To get raw frames the comma way you must: **stop the Android camera HAL** and
  run a **Spectra `camerad` ported to this device's sensors** against
  `cam_req_mgr`. Feasible with root, but a real driver-porting project.

## Hardware (probed 2026-06-21)
- SoC: Qualcomm SM8350 (Snapdragon 888), Spectra/Titan ISP
- Kernel: `5.4.268-qgki` (Android 13 / Lineage 20)

### Camera pipeline (v4l-subdev topology)
| subdev | name | role |
|---|---|---|
| 0 | cam-cpas | CPAS bus/clock arbiter |
| 1 | cam-isp | **Spectra ISP** |
| 2-7 | cam-csiphy-driver | 6x CSI PHY MIPI lanes |
| 8-9 | cam-actuator-driver | autofocus |
| 10-13 | cam-sensor-driver | **4 camera sensors** |
| 14-16 | cam-eeprom | per-lens calibration EEPROM |
| 17-19 | cam-flash-dev | flash |
| 20 | cam-icp | ICP (camera DSP) |
| video0 | cam-req-mgr | **Request Manager (scheduler)** |
| video1 | cam_sync | **HW frame sync (CAM_SYNC)** |

### Sensors (from dmesg sensor_id)
| sensor_id | part | role (likely) | notes |
|---|---|---|---|
| 0x766 | Sony IMX766 50MP | main/ultra-wide | seen streaming, CSIPHY_IDX 2, I2C 0x34, 5.79 Gbps |
| 0x689 | Sony IMX689 48MP | main wide | |
| 0x471 | Samsung/Sony 471 | front/aux | |
| 0x2 | 2MP mono/macro | mono | |

Live dmesg confirmed the standard cam ioctl flow works:
`CAM_ACQUIRE_DEV -> CAM_START_PHYDEV (CSIPHY_IDX 2) -> CAM_START_DEV` for
sensor 0x766. OnePlus adds `oplus_cam_sensor` vendor hooks (QSC tool versioning).

## Access test results (proot, as root)
| node | open() result |
|---|---|
| /dev/media0 | OK |
| /dev/v4l-subdev1 (cam-isp) | OK |
| /dev/video0 (cam-req-mgr) | EALREADY (held by camera HAL) |
| /dev/video1 (cam_sync) | EALREADY (held by camera HAL) |

`MEDIA_IOC_DEVICE_INFO` on /dev/media0 -> ENOTTY (not a standard media-ctl
device; Qualcomm uses private cam_req_mgr ioctls).

Holder of /dev/video0: `/vendor/bin/hw/android.hardware.camera.provider@2.4-service_64` (PID 1035)

## Path to a real Spectra camerad port (option 1)
1. Stop the Android camera HAL so it releases cam_req_mgr:
   `stop vendor.camera-provider` / kill PID 1035 (breaks Android camera).
2. Build openpilot's `camerad` for this kernel's `cam_req_mgr` uapi
   (`include/uapi/media/cam_*.h` from the OnePlus 9 / SM8350 kernel source).
3. Provide the per-sensor init: IMX766/IMX689 register settings, CSIPHY lane
   config (datarate 5.79Gbps, settle 2.8us for 0x766), ISP output format.
   comma's camerad has these for ITS sensors (ox03c10/ar0231/os04c10) — they
   must be re-derived for the OnePlus sensors (the hard part).
4. Pipe frames into visionipc/msgq -> modeld, same as comma.

This displaces the Android camera entirely and is sensor-specific work, not a
config change. But the kernel + nodes + ioctl flow are all present and reachable.
