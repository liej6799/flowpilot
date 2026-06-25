# How the Spectra ISP outputs NV12 directly (OnePlus 9 / SM8350)

Goal: make camerad get **NV12** straight from the Spectra IFE (the "PROCESSED" path)
instead of dumping raw Bayer over RDI. The stock OnePlus HAL already does this, so we use
it as the reference: an app captures NV12 to prove + validate the path, and a kprobe trace
of the live HAL recovers the IFE register programming that produces NV12.

## TL;DR

* The sensor emits **raw Bayer (RAW10)**. NV12 is produced by the **IFE hardware**
  (debayer → CSC → YUV), not the sensor and not software.
* The IFE delivers a **semi-planar NV12** buffer: Y plane tightly packed, then interleaved
  CbCr. Confirmed on-device: `Y[rowStride=1280, pixelStride=1] U/V[pixelStride=2] semiplanar=true`.
* NV12 = the IFE **FULL output**, which is **two write-masters** (`cam_vfe480.h`
  `CAM_VFE_BUS_VER3_VFE_OUT_FULL`, `num_wm=2`, `wm_idx={0,1}`): WM0 = Y plane, WM1 = CbCr
  plane, both packer `PLAIN_8`.
* The pixel pipeline is turned on by IFE top **`core_cfg_0 (off 0x2c) = 0x600c2b08`** (debayer
  + CSC + stats + DS4/DS16). camerad's RAW_DUMP path uses `0x78002b00` (pipeline bypassed).
* The FULL WM0/WM1 NV12 format + the debayer/CSC coefficients are programmed via the **CDM
  command buffer** (DMA'd), not the direct register-write path — so the final replication
  step is a CDM-buffer capture (see "Remaining gap").

## The app (`tools/op9camcap`)

`CamService` is a **headless foreground Service** (no Activity/Window — the device's
WindowManager surface path is wedged on this unit) that runs Camera2 → `ImageReader`
(`YUV_420_888`) and writes each frame to disk as packed NV12, logging the plane layout.

```
am start-foreground-service -n com.op9.camcap/.CamService \
    --es logical 0 --ei w 1280 --ei h 720 --ei dump 2 --ei skip 25
# -> /data/data/com.op9.camcap/files/nv12_1280x720_<n>.yuv  (w*h*3/2 bytes, NV12)
```

`MainActivity` (the original Activity entry point) is kept for environments where a display
is available. Build with `/root/camapp/build.sh` (javac → d8 → aapt → zipalign → apksigner).

Notes / gotchas found:
* The stock OnePlus HAL crashes the camera provider (`ExtensionModule::SignalRecoveryCondition`
  ← `IFENode::PostPipelineCreate`, the OnePlus "AdvancedCameraUsecase" recovery abort) for
  **large** YUV stream sizes (1920×1080, 4096×3072) — but streams **1280×720** cleanly. Use a
  preview-class size.
* `camera.ko` must be loaded (it is missing from `/vendor/lib/modules` on this unit — a prior
  session replaced it). `insmod /data/local/tmp/camera_allreg_stock.ko`, then
  `setprop ctl.restart cameraserver; setprop ctl.start vendor.camera-provider-2-4`.
* A clean **reboot** is needed if the camera/ISP is in a degraded state (the provider crash-
  loops or `onError 4` on every open).

## Recovering the IFE register config (`nv12_trace.sh` + `decode_ife_trace.py`)

tracefs kprobes (no module / no debugfs needed) on **both** `cam_io_w` and `cam_io_w_mb`
capture every kernel-side IFE register write while the app streams NV12. `cam_io_w` is the
one that carries `image_cfg_0`/`packer_cfg`; `cam_io_w_mb` carries the WM `cfg` enable —
probe both. Identify the IFE block by the `core_cfg_0 (0x2c)` + `camif_epoch (0x2680)`
signature, then decode each WM (offsets from `cam_vfe480.h`: `cfg=+0, image_cfg_0=+0xC,
image_cfg_2/stride=+0x14, packer_cfg=+0x18`).

Observed for the 1280×720 NV12 session: 16 write-masters enabled (FULL + DS4 + DS16 + FD +
stats HDR_BE/BHIST/IHIST/BG…) — the full processed pipeline. `core_cfg_0 = 0x600c2b08`.

## Important correction: the SM8350 debayer→NV12 path WORKS

`cameras/hw.h` says `ISP_IFE_PROCESSED` "hangs (SM8350 debayer regs differ from tici)" and the
HAL "uses RDI raw, not the debayer". That conclusion came from tracing a **raw** Camera2 stream.
This YUV capture proves the opposite: when the app requests `YUV_420_888`, the HAL runs the
**full debayer→CSC→NV12 pixel pipe** (`core_cfg_0=0x600c2b08`, semi-planar NV12 out). So the
SM8350 IFE debayer→NV12 path is fine — camerad's PROCESSED path just has a config bug.

## camerad PROCESSED diagnosis (road camera = ISP_IFE_PROCESSED, run under full IFE debug)

camerad's PROCESSED path does the right top-level thing: it acquires the FULL output (`0x3000`)
and starts the correct **2-WM NV12 structure — WM0=Y (4096×3072), WM1=CbCr (4096×1536)**. But it
**`PIXEL PIPE OVERFLOW`→hang**s on frame 0, for two concrete reasons:

1. **`Start ... WM:0 pk_fmt:0 stride:0`** (same for WM1). The NV12 packer (should be `PLAIN_8`)
   and stride (should be 4096) never reach the write-master, so it can't DMA → backs up the pipe.
   The op9 port wired the format/stride blob only for the RAW path; the PROCESSED io_cfg
   (`CAM_FORMAT_NV12`, spectra.cc ~800/1186) isn't producing a valid pack_fmt/stride.
2. The first violations are **`STATS IHIST: CCIF violation`** and **`STATS BAF: CCIF violation`**:
   the pixel pipe generates stats (IHIST, BAF, HDR_BE/BHIST…) that camerad never drains. The HAL
   configures **all 16 WMs** (FULL + DS4 + DS16 + FD + every stats output); camerad drains only 2.

### Fixes applied (spectra.cc) and result

Implemented and tested on-device (road cam = ISP_IFE_PROCESSED):

1. **Arm the NV12 WM at init** — `arm_wm_in_init = init` for BOTH raw and PROCESSED (was raw-only),
   always reserve the init io_cfg, and give the PROCESSED FULL output a throwaway init fence. This is
   the proven RAW-path pattern. **Result: fixed `stride:0 → 4096`.**
2. **Pin format to 32** — this kernel's `cam_defs.h` has `CAM_FORMAT_NV12 == 32`; pinned the acquire
   out-port and io_cfg format to `32` -> kernel maps it to `PLAIN_8_LSB_MSB_10`.

These fixes are correct and kept in the project (harmless for the RAW path). NOTE: the `pk_fmt:0` in
the kernel `start_wm` log is a **logging artifact** — it prints `pack_fmt & PACKER_FMT_VER3_MAX`
(`MAX=12=0xC`); the NV12 packer `PLAIN_8_LSB_MSB_10=3`, and `3 & 12 == 0`. The packer is correct.

### Where it stands now — the remaining wall

With the fixes the PROCESSED path gets much further than ever before (kernel debug, single+dual IFE):

* FULL output acquired (`out_type 0x3000`), WM0=Y (4096×3072) + WM1=CbCr (4096×1536), `format:0x20`.
* **WM gets real buffer addresses** (`io_addr plane0=0xfd400000, plane1=0xfe000000`) — *not* the
  RAW path's `image_addr=0` deadlock.
* Reaches **SOF → EPOCH → RUP done** (the pixel pipe actually runs).
* Then **`PIXEL PIPE OVERFLOW` → Module hang → sof_freeze**, `CCIF violation status 0x1FFF3F`.

The violation list (VID/DISP/FD/RAW DUMP/all STATS) is a **global dump** from
`cam_vfe_print_violations`, not per-module — disabling the `core_cfg` stats bits (8,9) did **not**
help. So the blocker is the overall **pixel-pipe CCIF overflow** (the project's long-standing CAMIF
"bad frame timings"/overflow wall), now reached on the PROCESSED path with an otherwise-correct WM.

### Next hypotheses (not yet tried)

1. **Dual-IFE BW / striping**: PROCESSED runs dual IFE (`usage_type=1`) but votes only one BW path
   (`bw_num_paths=1`) and sends **no DUAL_CONFIG stripe blob** (`add_dual_cfg` is false for
   PROCESSED) — each IFE may try to write the full 4096 width. Try single-IFE + per-IFE BW, or add
   the FULL stripe blob like the RAW path has for RAW_DUMP.
2. **CAMIF frame timing / CCIF geometry**: match the HAL CAMIF epoch/line config and confirm the
   CSID pixel-path crop equals the sensor's real emitted frame (same CCIF check that bit the RAW
   path). HAL CAMIF epoch `0x001402ed` vs camerad `0x1402FF`.
3. The HAL's per-pixel debayer/CSC + WM0/1 format is built in camx **userspace** CDM buffers (DMA'd,
   never parsed kernel-side) so it's not kprobe-recoverable — remaining work is camerad-side
   pipe/BW/timing config, not more HAL extraction. `core_cfg_0=0x600c2b08` is the reference.
