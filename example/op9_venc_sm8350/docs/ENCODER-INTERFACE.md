# OnePlus 9 (SM8350 / Iris2) video encoder — V4L2 interface

Reverse-engineered from the OP9 kernel source (`techpack/video/msm/vidc/`, `msm_venc.c`,
`msm_vidc_platform.c`) and validated live with `src/venc_probe.c` / `src/venc_setup_test.c`.

## Hardware / driver

- Video core: **Iris2** (Lahaina / SM8350) — `hfi_iris2.c`, `msm_vidc_bus_iris2.c`.
- Driver: `msm_vidc_driver` (`techpack/video/msm/vidc`), same family as the comma three but newer gen.
- Nodes (both behind `aa00000.qcom,vidc`; **no `/dev/v4l/by-path`** — Android has no udev):
  - `/dev/video32` → `msm_vidc_vdec` (decoder): coded in (H264/HEVC/MPEG2/VP9) → NV12 out
  - `/dev/video33` → `msm_vidc_venc` (**encoder**): NV12/NV21/UBWC in → **H264/HEVC** out

## Buffers & formats

- M2M: **OUTPUT_MPLANE** = raw NV12 frames in; **CAPTURE_MPLANE** = coded HEVC/H264 out.
- Pixel formats (encoder): in = `NV12` / `NV12_UBWC` / `NV21` / `P010` / `RGBA_UBWC`; out = `H264` / `HEVC`.
  openpilot uses **NV12 → HEVC**, both supported.
- `vb2 q->io_modes = VB2_MMAP | VB2_USERPTR` (msm_vidc.c). encoderd uses **USERPTR**; MMAP REQBUFS → EINVAL.
- **No `VIDIOC_S_PARM`** on this venc (ENOTTY). Framerate is the `FRAME_RATE` control, value Q16 (`fps<<16`).

## Control namespace

`msm_venc.c` advertises **47 upstream `V4L2_CID_MPEG_VIDEO_*`** controls and **a few surviving
`V4L2_CID_MPEG_VIDC_VIDEO_*`** private ones. The comma three's encoder code targets the legacy VIDC set,
most of which is **absent** here.

Controls openpilot needs, and where they live on Iris2:

| openpilot intent | Iris2 control | namespace |
|---|---|---|
| bitrate | `MPEG_VIDEO_BITRATE` | upstream |
| rate-control mode | `MPEG_VIDEO_BITRATE_MODE` + `MPEG_VIDEO_FRAME_RC_ENABLE` | upstream |
| GOP / P-frames | `MPEG_VIDEO_GOP_SIZE` | upstream |
| B-frames | `MPEG_VIDEO_B_FRAMES` | upstream |
| header placement | `MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR` | upstream |
| HEVC profile | `MPEG_VIDEO_HEVC_PROFILE` | upstream |
| HEVC tier | `MPEG_VIDEO_HEVC_TIER` | upstream |
| HEVC level | `MPEG_VIDEO_HEVC_LEVEL` | upstream |
| H264 entropy | `MPEG_VIDEO_H264_ENTROPY_MODE` | upstream |
| framerate | `VIDC_VIDEO_FRAME_RATE` (Q16) | VIDC-private (survives) |
| priority / IDR period / VUI timing | — | gone (dropped; non-essential) |

VIDC-private control base: `V4L2_CID_MPEG_MSM_VIDC_BASE = (V4L2_CTRL_CLASS_MPEG | 0x2000)`;
`VIDC_VIDEO_FRAME_RATE = base + 119`.

## Validation (src/venc_setup_test.c on /dev/video33)

Mirrors encoderd's `V4LEncoder` init exactly (S_FMT both queues → every control → REQBUFS USERPTR →
STREAMON). Result: **every** ported control returns `OK`, both STREAMON succeed → encoder armed for HEVC.
This proves the device node, formats, control mapping, framerate control, and buffer memory type are all
correct for this phone. The only remaining step for end-to-end is feeding NV12 frames.

## Headers needed to build

- `media/msm_media_info.h` — Venus buffer-size/stride macros (`VENUS_*`). From the OP9 kernel uapi
  `techpack/video/include/uapi/vidc/media/msm_media_info.h`. (Not vendored here — it's the kernel's, GPL.)
- `media/op9_vidc_compat.h` — this example's `include/`; provides `VIDC_VIDEO_FRAME_RATE` (+ compile shims
  for the few legacy IDs still referenced by the unported code paths).
