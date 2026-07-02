# Recording: camerad → encoderd (HW HEVC) → loggerd on the OnePlus 9 (stock kernel)

Gets the op9 BPS/RAWSOF camerad streams **recorded to disk as HEVC** via the SM8350
Iris2 hardware video encoder, on the **unmodified stock `camera.ko`**.

## Result

The full openpilot logging chain runs and produces a normal route segment:
```
<LOG_ROOT>/00000000--<id>--0/
  fcamera.hevc   # road  (imx766) — HW HEVC, decodes to real frames
  ecamera.hevc   # wide  (imx689) — HW HEVC
  rlog.zst       # all msgq services (camera state, etc.)
  qlog.zst       # downsampled log
```
`fcamera.hevc`/`ecamera.hevc` are valid HEVC (VPS/SPS/PPS + IDR + P frames) and
decode with ffmpeg to the real camera image.

## Run

```
# stock camera.ko loaded (see ../STOCK-KERNEL.md), then in the proot:
OP9_BPS=1 OP9_RAWSOF=1 OP9_SINGLE_IFE=1 OP9_SENSOR_CROP_H=3000 OP9_INTEG=8000 OP9_GAIN=800 \
  ./system/camerad/camerad &
./system/loggerd/encoderd &
LOG_ROOT=/tmp/oplog ./system/loggerd/loggerd &
```
camerad publishes NV12 to VisionIPC (ROAD + WIDE_ROAD); encoderd HW-encodes each to
HEVC via `/dev/video33` (`msm_vidc_venc`, `OP9_ENC_DEV` overridable); loggerd writes
the segment. All three share `/tmp` (VisionIPC + msgq sockets) in one proot.

## The five fixes (files in this dir are the modified openpilot sources)

1. **`visionbuf_ion.cc` — allocate from the msm system heap.** The device exposes two
   `ION_HEAP_TYPE_SYSTEM` heaps: `ion_system_heap` (id 0, generic, **no** `dma_buf get_flags`)
   and `system` (id 25, msm, **has** `get_flags`). The venc calls QCOM `dma_buf_get_flags()`
   on every queued buffer and rejects the generic-heap ones with `-95 (EOPNOTSUPP)` →
   `QBUF`/`STREAMON` fail. `ion_system_heap_id()` now prefers the heap **named `"system"`**.
   (Affects camerad's frame buffers *and* the encoder buffers — both are queued to the venc.)

2. **`loggerd.h` — drop the qcamera H264 encoder.** The qcamera preview is 526×**330**, but
   the Iris2 venc has **min height 384** and can't scale (`Unsupported height 384 vs 330;
   scaling is not supported`). Its `start_streaming` fails and the `kill_session` cascade
   aborts the road/wide HEVC sessions too. `road_camera_info.encoder_infos` now = main HEVC
   only. (qcamera would need a separate pre-downscaler.)

3. **`v4l_encoder.cc` — 2-plane NV12 input QBUF/DQBUF.** The Iris2 venc forces the NV12
   input to `num_planes=2`: plane0 = the full NV12 data, plane1 = a **4096-byte extradata/
   metadata** plane (NOT a Y/UV split). The upstream single-plane submission fails EINVAL.
   `queue_buffer`/`dequeue_buffer` now submit plane0 = the whole camerad NV12 buffer and
   plane1 = a per-slot 4096B scratch extradata buffer (msm-heap, so `get_flags` works).

4. **`spectra.cc` (../camerad/) — BPS FULL output → the VisionIPC buffer.** `config_bps`
   was writing the demosaiced NV12 to `bps_fullres_dummy` (a validation scratch), so the
   published/encoded frame was all-zero → **green** recording. The FULL Y/C io_cfg + the
   `buffers[1]`/`buffers[2]` patches now target `buf_handle_yuv[idx]` (the vipc buffer),
   which is stride-4096 NV12 matching the BPS's fixed layout.

5. **`SConstruct` — build loggerd/encoderd.** The op9-trimmed SConstruct only built camerad.
   Add after the camerad SConscript:
   ```python
   if True:  # [op9] build loggerd + encoderd for on-device recording
     SConscript(['system/loggerd/SConscript'])
   ```
   Also: the build must be **larch64** (`touch /TICI` in the proot rootfs) so encoderd
   compiles the HW `v4l_encoder` (`#ifdef __TICI__`) instead of the SW ffmpeg encoder.

## Remaining / notes

- Exposure is fixed (`OP9_INTEG`/`OP9_GAIN`); frames are dark indoors — auto-exposure is the
  open quality item (see the AE discussion), not a recording-mechanism issue.
- qcamera (cloud preview) is disabled; only fcamera/ecamera + rlog/qlog are recorded.
