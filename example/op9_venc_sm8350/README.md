# op9_venc_sm8350

Ports openpilot's hardware H.265 encoder (`system/loggerd/encoder/v4l_encoder.cc`, used by `encoderd`)
from the **comma three's Venus** V4L2 interface to the **OnePlus 9 (SM8350 / Iris2)** video encoder.

## Status / honesty

✅ **Encoder setup validated on real hardware.** The full init path (device open → S_FMT → all controls →
REQBUFS → STREAMON) succeeds on the OnePlus 9's `/dev/video33` (`msm_vidc_venc`):

```
All 9 ported upstream controls -> OK
FRAME_RATE control             -> OK
REQBUFS (USERPTR) CAP/OUT      -> OK
STREAMON CAP/OUT               -> OK
==> Iris2 armed for HEVC
```

⏳ **Not yet end-to-end.** Live encoding still needs NV12 frames fed in (from `camerad` via VisionIPC, or
the `src/` test extended to push one frame and pull a `.hevc` out). The *interface* port is done & proven.

## Why it's different on this phone

The comma three uses an older Venus core driven by **legacy private `V4L2_CID_MPEG_VIDC_VIDEO_*` controls**.
The OnePlus 9 (Lahaina/SM8350) uses the newer **Iris2** core (`techpack/video/msm/vidc`, `hfi_iris2.c`), whose
`msm_venc.c` exposes **upstream `V4L2_CID_MPEG_VIDEO_*` controls** instead — most of the comma three's VIDC
control IDs simply don't exist here. So encoderd's encoder programming has to be remapped.

## The mapping (comma three → OnePlus 9 / Iris2)

| comma three (legacy VIDC) | OnePlus 9 (upstream) |
|---|---|
| `VIDC_VIDEO_NUM_P_FRAMES` | `MPEG_VIDEO_GOP_SIZE` |
| `VIDC_VIDEO_NUM_B_FRAMES` | `MPEG_VIDEO_B_FRAMES` |
| `VIDC_VIDEO_RATE_CONTROL` (VBR_CFR) | `MPEG_VIDEO_BITRATE_MODE` (VBR) + `MPEG_VIDEO_FRAME_RC_ENABLE` |
| `VIDC_VIDEO_HEVC_PROFILE` | `MPEG_VIDEO_HEVC_PROFILE` |
| `VIDC_VIDEO_HEVC_TIER_LEVEL` | `MPEG_VIDEO_HEVC_TIER` **+** `MPEG_VIDEO_HEVC_LEVEL` (split) |
| `VIDC_VIDEO_H264_CABAC_MODEL` | dropped — `H264_ENTROPY_MODE=CABAC` already covers it |
| `HEADER_MODE_SEPARATE` | `PREPEND_SPSPPS_TO_IDR` (no SEPARATE on Iris2) |
| `VIDC_VIDEO_PRIORITY`, `VIDC_VIDEO_IDR_PERIOD`, `VIDC_VIDEO_VUI_TIMING_INFO` | dropped (non-essential tuning) |
| `VIDIOC_S_PARM` (framerate) | **`VIDC_VIDEO_FRAME_RATE` control**, value Q16 (`fps<<16`) — Iris2 has no S_PARM |
| `/dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index1` | **`/dev/video33`** (no udev/by-path on Android) |

Unchanged & confirmed working: **NV12** input / **HEVC** output formats (both supported), and
**`V4L2_MEMORY_USERPTR`** buffers (Iris2 io_modes = MMAP|USERPTR; encoderd already uses USERPTR — MMAP gives
REQBUFS EINVAL). `FRAME_RATE` is the only surviving VIDC-private control we still set; its define lives in
`include/op9_vidc_compat.h` (= `(V4L2_CTRL_CLASS_MPEG|0x2000)+119`).

## Device map (from `src/venc_probe`)

```
/dev/video32  msm_vidc_vdec  (decoder)  coded in  -> NV12 out
/dev/video33  msm_vidc_venc  (ENCODER)  NV12 in   -> H264/HEVC out
```

## Build / run

```bash
# port the encoder (against the v0.11.2 / flowpilot tree)
git apply example/op9_venc_sm8350/patches/encoderd-v4l-sm8350-v0.11.2.patch
# headers it needs (vendored from the OP9 kernel uapi), placed on the include path:
#   media/msm_media_info.h   (techpack/video/.../uapi/vidc/media/msm_media_info.h — Venus buffer macros)
#   media/op9_vidc_compat.h  (this dir's include/ — the FRAME_RATE control id)
scons -j$(nproc) system/loggerd/encoderd

# validate the interface on the real encoder WITHOUT camerad:
clang src/venc_probe.c      -o venc_probe      && ./venc_probe /dev/video32 /dev/video33
clang src/venc_setup_test.c -o venc_setup_test && ./venc_setup_test /dev/video33   # -> "Iris2 armed for HEVC"
```

(Node perms: `chmod 666 /dev/video33`. `OP9_ENC_DEV` env overrides the device path.)

## Porting to mainline openpilot

The diff is intentionally readable — every line is an explicit VIDC→upstream swap. Upstream openpilot could
take the upstream-control form behind a platform check, since the upstream controls are the standard V4L2
ones (the legacy VIDC IDs are the special-case). The Iris2 path is closer to mainline-kernel V4L2 than the
comma three's, so this is arguably the *more* portable variant.

## Layout

```
patches/encoderd-v4l-sm8350-v0.11.2.patch  the port (v4l_encoder.cc, +20/-25)
include/op9_vidc_compat.h                  VIDC FRAME_RATE control id (+ compile shims)
src/venc_probe.c                           identify enc/dec nodes + their formats
src/venc_setup_test.c                      mirror encoderd init; reach STREAMON to prove the port
docs/ENCODER-INTERFACE.md                  full Iris2 control set + analysis
```
