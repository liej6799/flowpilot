# Running camerad on the STOCK OnePlus 9 camera kernel (no module patch)

Goal: make `camerad`/`spectra.cc` stream continuously on the **unmodified stock
`camera.ko`**, not the project's `camera_diag.ko` debug build. Investigated
2026-07-02 on-device (stock module = 0 `op9_*` params).

> ## ✅ SOLVED (2026-07-02) — no kernel patch required
> camerad streams **continuously on the unmodified stock `camera.ko`**: 330+ frames,
> no freeze, no flush, request_id 1:1 with frames, and the full **RAW→BPS→NV12**
> pipeline produces a real demosaiced 4000×3000 color frame (imx689 main).
> **Recipe:** `OP9_BPS=1 OP9_RAWSOF=1 OP9_SINGLE_IFE=1 OP9_SENSOR_CROP_H=3000`
> (`OP9_INTEG=20000 OP9_GAIN=960` for exposure). The three fixes that got there
> (see Updates 3–6): (1) `OP9_RAWSOF` RAW_DUMP-with-demosaic-bypass keep-alive →
> CAMIF per-frame SOF (res_id=0 dodges the OPLUS RDI hijack) → normal CRM apply;
> (2) `OP9_SENSOR_CROP_H=3000` so the RAW_DUMP WM geometry matches; (3) `validateEvent`
> resyncs instead of flush-requeuing on the benign frame_id gaps caused by idle SOF.
> **Update 6 = the final fix (3), detailed at the very bottom.**

## TL;DR — what's the actual gap

The stock ISP kernel is **fine** for everything except **per-frame SOF on an
RDI-only context**. Concretely, verified on-device + against the stock source in
`/root/op9_kernel` (branch `lineage-20`, read via `git show HEAD:`):

| piece | stock behavior | needs |
|---|---|---|
| sensor probe / acquire / power / i2c init | works | — |
| CSIPHY C-PHY/D-PHY bring-up, MIPI | works | — |
| IFE-lite RDI acquire, BW vote, WM DMA | works (`Acquired single IFE[..] [1 rdi]`) | — |
| BPS acquire + NV12 job + firmware | works (frame 0 completes, NV12 dumped) | — |
| **per-frame SOF → CRM apply → continuity** | **frame 0 only, then `sof_freeze`** | a SOF source |
| the `op9_tolerate_violation` / `op9_tolerate_pp` params | **not needed** — 0 violations with them off | — |

So the debug module's *functional* value was **one thing**: it synthesizes a
per-frame RDI SOF (`cam_ife_csid_irq` → `cam_csid_handle_hw_err_irq(RDI0,
ERROR_NONE)`), which the stock kernel does **not** do. Everything else in the
kernel diff is debug logging / blob-capture (`op9_bps_dump`, `op9_dumpreq`,
`op9_i2clog`, the WM/CAMIF dumps) or now-obsolete violation tolerance.

## Why the RDI path can't self-drive on stock (code-cited)

Two independent facts, both confirmed:

1. **Stock never fires a per-frame RDI SOF IRQ.** With `debug_mdl=0xffff` on the
   stock module, an RDI-only camerad ctx logs exactly **one** `Received RDI0 RUP`
   + **one** `send_sof_timestamp req 1` (the initial reg-update in the APPLIED
   substate), then `__cam_req_mgr_process_sof_freeze`. The CSID RDI SOF interrupt
   is not routed to `cam_ife_hw_mgr_handle_hw_sof` per-frame. (The diag kernel adds
   exactly that route in `cam_ife_csid_irq`.)

2. **Even if it fired, the OPLUS hijack mis-routes it.** In stock
   `cam_isp_context.c:6043` (`__cam_isp_ctx_handle_irq_in_activated`), an
   `#ifdef OPLUS_FEATURE_CAMERA_COMMON` block intercepts `res_id ==
   CAM_ISP_HW_VFE_IN_RDI0`, notifies the CRM with `trigger =
   CAM_TRIGGER_POINT_RDI_SOF`, and `return`s — skipping the normal state machine.
   `CAM_TRIGGER_POINT_RDI_SOF` only applies a request if a linked device has
   `dev_info.trigger == RDI_SOF`, which the sensor sets **only** when acquired with
   `reserved != 0` (`cam_sensor_core.c:1024` → `use_rdi_sof_apply`). camerad's
   sensor acquire uses `reserved = 0`, so nothing consumes the RDI_SOF trigger.

   **Neat subtlety** (verified): in `cam_ife_hw_mgr_handle_hw_sof`
   (`ife_hw_mgr.c:7951`), the RDI case only sets `sof_event_data.res_id =
   event_info->res_id` in the `use_rdi_sof` branch. With `use_rdi_sof = FALSE` the
   callback carries `res_id = 0` (CAMIF, from `memset`), which **dodges** the
   `res_id==RDI0` hijack and would flow to the normal
   `__cam_isp_ctx_rdi_only_sof_in_top_state` → `CAM_TRIGGER_POINT_SOF` → normal
   apply. This is why `OP9_NO_RDISOF` (force `reserved=0`) is the right lever — BUT
   it only helps once fact (1) is solved, because stock still never delivers the
   per-frame RDI SOF IRQ in the first place. On the **diag** kernel `OP9_NO_RDISOF`
   streams 270+ frames; on **stock** it still `sof_freeze`s after frame 0.

## The only userspace-only route on stock: pixel-path SOF (what the HAL does)

Stock HAL trace (`op9camcap` YUV 1280×720, `debug_mdl=0xffff`): the HAL acquires
**one dual-IFE ctx with `[12 pix] [1 pd] [1 rdi]`** and all 129 SOFs/EOFs come
from the **full CAMIF** (`cam_vfe_camif_ver3`), normal `CAM_TRIGGER_POINT_SOF`.
The RDI WM just rides inside the pixel context; the pixel CAMIF is the SOF source.
Zero camif-**lite** SOFs anywhere. So the supported pattern is: **acquire a pixel
output alongside the RDI**, let the CAMIF drive per-frame SOF, and the shared ctx
applies both WMs each frame.

`OP9_FD_SOF` implements exactly this (a keep-alive pixel WM + RDI in one ctx).
Progress as of 2026-07-02 on the diag kernel (for fast iteration with WM dumps):

- **Image-size violations: ELIMINATED.** The keep-alive pixel port with HFR
  `framedrop_pattern = 0` (`config_ife` 2-port blob) makes the pixel WM accept the
  CCIF frame without a NOC write and **skips the 2D image-size check** — dumps went
  from `Image Size violation status 0x200/0x30` to `0x0`.
- **Remaining wall: one `FD C: CCIF violation` (status 0x200).** A chroma-side
  pixel-pipe module in the base `ife.h` graft is enabled but its geometry doesn't
  match the acquired keep-alive WM. The base pixel-pipe graft (`ife.h`
  `0x3000–0x8fff`) was captured from a HAL session at **4000×3000** (`0x308c =
  0x0bb80fa0` = 3000×4000); the keep-alive was tried at FD 640×480 and FULL_DISP
  2000×1500. Clearing the last CCIF needs the demosaic→CSC→chroma-out geometry to
  match the WM **exactly** — bounded but deep register RE (the same SM8350
  debayer/CSC offsets the project flagged as the hard part in
  `../op9_camera_model/docs/STREAMING-BRINGUP.md`).

### Update 2 (2026-07-02, deeper): the pixel keep-alive hits the SM8350 pixel-pipe wall

Pushing the `OP9_FD_SOF` keep-alive further surfaced the real difficulty and a
much clearer picture of what the HAL does:

- **HAL CSID structure (from `debug_mdl` trace):** the HAL acquires **three**
  CSID resources — **IPP** (res 4, pixel, `line 0..2999`, height **3000 exactly**),
  **PPP** (res 5, the embedded/PDAF stream on its own path), and **RDI** (res 1,
  raw). "Session has PIX or PIX and RDI resources". So the embedded data is
  *never* on the image CID — it rides PPP — which is why the HAL's IPP frames
  cleanly at 3000 lines.
- **`UNBOUNDED_FRAME` root cause (fixed):** on a **single IFE** a 4000-wide pixel
  frame overruns the IPP; `OP9_CROP_W=2000` (center-crop the CAMIF) **clears
  UNBOUNDED_FRAME** completely. Mapping the embedded DT onto the image CID
  (`num_valid_vc_dt=2`) also causes it (3002 vs 3000 lines) — keep it at 1.
- **Remaining wall = `PIXEL PIPE OVERFLOW` / `IPP_PATH_OVERFLOW`.** The base
  `ife.h` graft is a **complete HAL pixel pipe** (demosaic + FD + DISP + DS4/DS16 +
  stats all enabled). The HAL drains **all ~12 pixel WMs**; our single-IFE keep-alive
  drains only the 2 FULL_DISP WMs, so every other enabled module back-pressures the
  shared IPP fifo → overflow → no SOF. Disabling individual FD module_cfg regs
  (`0x4460/0x4c60/0x4860/0x4a60/0x8a60/0x8e60/0x4600/0x8060/0x8260/0x8460/0x8660`,
  top enables `0x48/0x4c`) did **not** clear the residual `FD C: CCIF` — the FD
  module enable wasn't among them, and the overflow persists regardless.

**Two concrete ways forward (both real work):**
1. **Match the HAL: dual IFE + drain all pixel WMs.** Acquire the full pixel-output
   set the HAL does and give each a scratch buffer. Deterministic (the graft *is*
   the HAL config) but heavy — many outputs + dual-stripe config.
2. **RAW_DUMP with the pixel pipe BYPASSED (recommended next).** Don't run the
   demosaic pipe at all: program `core_cfg_0 = 0x78002b00` (pipeline-bypass, the
   value camerad already documents for RAW_DUMP in `docs/ISP-NV12-OUTPUT.md`) +
   CAMIF `module_cfg 0x2660` + demux, acquire **only** the `RAW_DUMP` output, drain
   its one WM. The CAMIF still fires per-frame SOF (SOF is at the pipe *input*), but
   with the demosaic/FD/DISP/DS/stats all bypassed there's nothing undrained to
   overflow. RAW_DUMP is post-CAMIF raw Bayer — the **same** data the RDI tap gives —
   so it feeds BPS unchanged. This sidesteps the entire demosaic-geometry RE.

### Update 3 (2026-07-02): BREAKTHROUGH — per-frame SOF on the UNMODIFIED stock kernel

The `OP9_FD_SOF` (demosaic keep-alive) path was abandoned — a single IFE can't run
the full HAL pixel pipe without overflow, and disabling its dozen output modules
one-by-one didn't converge. The **`OP9_RAWSOF`** path (RAW_DUMP with the demosaic
pipe **bypassed**) works and is far simpler:

- `ife.h build_rawdump_bypass()`: `core_cfg_0 = 0x78002b00` (CAMIF on, demosaic/
  CSC/scalers/FD/DISP/stats **all off**) + demux + CAMIF `module_cfg 0x2660` +
  epoch `0x2680=0x001402ed` + full-frame crop `0xe10`. Single IFE (`OP9_SINGLE_IFE`),
  acquire **only** RAW_DUMP (`0x3003`, a pixel out → `is_rdi_only_context=false`),
  `reserved=0`.
- **Result on the STOCK module (0 op9 params):** acquire `single IFE[2] [1 pix]`,
  and the **CAMIF fires per-frame SOF ×215 + EPOCH ×215** over a 15 s run — with a
  clean bypassed pipe (no demosaic/overflow). The SOF carries `res_id=0` (CAMIF), so
  it **dodges the OPLUS `res_id==RDI0` hijack** and flows to the normal state machine
  (`__cam_isp_ctx_sof_in_epoch`, `frame_id` increments). **This is the core blocker
  — "stock delivers no per-frame SOF for a raw context" — SOLVED.** RAW_DUMP is
  post-CAMIF raw Bayer = the same data the RDI tap gives, so it feeds BPS unchanged.

- **The one remaining wall (well-scoped):** buf_done never fires, so `active_req_cnt`
  climbs, the ctx stops notifying the CRM after 2 frames, and it `sof_freeze`s. Cause:
  the **RAW_DUMP write-master image-size violation**. The WM is line-based at height
  **3001** but the CAMIF emits **3000**. The kernel's CSID **IPP crop reserve**
  computes `height:3001 line_stop:3000` from our config, whereas the HAL's IPP is
  exactly `height:3000 line_stop:2999` (same kernel). RAW_DUMP is a *processed/pixel*
  WM so frame-based mode is rejected (`"Frame based illegal programming"`, constraint
  `0x1000`) — it can't skip the 2D check like the RDI WM does; the geometry must match.
  **Fix (found + verified): `OP9_SENSOR_CROP_H=3000`.** The deployed `imx689.cc` sets
  `frame_height = 3001` on purpose (a demosaic-even trick for the *custom* kernel's
  `op9_y_h`); `OP9_SENSOR_CROP_H=3000` (or `OP9_SENSOR_CROP_W`) forces the real 3000.
  With that, the RAW_DUMP WM = CAMIF = CSID = **3000**, matching the sensor's real
  3000-line output.

### Update 4 (2026-07-02): STOCK camerad STREAMS — SOF → apply → buf_done, no module patch

With `OP9_RAWSOF=1 OP9_SINGLE_IFE=1 OP9_SENSOR_CROP_H=3000` on the **unmodified stock
`camera.ko`** (0 op9 params), a throttled run (`debug_mdl=0xffff`, which serializes
the startup) shows the **complete chain working**:
`apply=3, buf_done=2, EPOCH=4, freeze=0, violations=0`. i.e. the CAMIF SOF drives the
normal CRM apply → reg_update → **buf_done** → next request, entirely on the stock
kernel. **The stock-kernel goal is functionally demonstrated.**

**Last item — a startup race at full speed.** With `debug_mdl` off (production speed)
camerad's open-time burst (`enqueue_frame` ×`ife_buf_depth` back-to-back before
`START_DEV`) races the ISP ctx state → per-frame `config_dev` returns `-EINVAL`/`-ENOMEM`
→ `config_ife` `assert(ret==0)`. Throttling hides it; it needs sequencing, e.g.:
- enqueue **fewer** frames at open (or enqueue after START_DEV), and/or
- make `config_ife` **tolerate** a transient `config_dev` failure (retry/skip instead of
  `assert`) during the initial ramp, like a real driver would.

This is camerad-side request/state sequencing, **not** more kernel or pixel-pipe RE.

**Precise root cause (traced with `debug_mdl=0x1080018` = ISP|CRM|CTXT|REQ):**
config_dev is rejected with `Received update req N in wrong state:4` — state **4 =
`CAM_CTX_FLUSHED`**. The ISP ctx gets flushed mid-ramp by a **cascade from the BPS/ICP**:
```
cam_context_flush_ctx_to_hw: [cam-icp] E: NRT flush ctx
cam_icp_mgr_hw_flush: ctx_id 0 Flush type 1 last_flush_req 0
cam_req_mgr_process_flush_req: link_hdl <isp+sensor link> req_id 0 type 0   ← flushes the WHOLE link
```
Chain: an early/unstable SOF trips camerad's `validateEvent` (request_id 0 or a
frame-id skip during ramp) → `clearAndRequeue` → `clear_req_queue` issues a
`CAM_FLUSH_REQ` to the ICP **and** a `CAM_REQ_MGR_FLUSH_REQ` on the link → the ICP
NRT flush + the link flush drop the ISP ctx to FLUSHED → every later `config_dev`
is rejected → the stream can't recover → `sof_freeze`. `OP9_NO_FLUSH` skips
`clear_req_queue`, but then the no-flush recovery (destroy fence + re-enqueue)
doesn't re-stabilise the ramp either — so the real fix is to **stop the ramp from
tripping `validateEvent` in the first place** (SOF request_id continuity) and/or
**not submit BPS jobs until the IFE/RAW_DUMP stream is stable** (decouple the ICP
so its flush can't cascade onto the ISP link). Both are camerad-side ramp
engineering; the stock kernel itself is not the blocker.

**Update 5 (2026-07-02) — ramp fixes applied, one stubborn flush remains.** Applied
and kept: (a) `config_ife` skips a transient `config_dev` failure instead of
`assert`ing; (b) `validateEvent` no longer `clearAndRequeue`s on the benign
`request_id==0` idle SOF for RAWSOF (that reaction was a death-spiral: flush →
FLUSHED → can-never-apply → request_id stuck 0). Result is deterministic: the ctx
processes **exactly one frame** ("camera 0 synced"), then a **CRM link flush fires
~220 ms after the first SOF** (`cam_req_mgr_process_flush_req link req_id 0 type 0`)
→ ISP FLUSHED → 7× `config_dev wrong state:4` → `sof_freeze`. This flush **persists
even with `OP9_NO_FLUSH` and even with BPS disabled**, and `clearAndRequeue`'s
`op9requeue` log does NOT fire — so it is **not** coming from the paths we've
guarded. Next debugging step: find the unguarded flush source — instrument every
`clear_req_queue`/`CAM_REQ_MGR_FLUSH_REQ` caller (and check for a CRM-internal
watchdog flush) to see who issues the `req_id 0 type 0` link flush ~220 ms in; the
likely culprit is a `waitForFrameReady` timeout path or a CRM watchdog, not the
request-continuity checks. Once that single flush is suppressed, the one-frame
success should extend to continuous streaming.

### The working stock recipe (env)
```
OP9_BPS=1 OP9_RAWSOF=1 OP9_SINGLE_IFE=1 OP9_SENSOR_CROP_H=3000 OP9_GAIN=512
```
`OP9_RAWSOF` = RAW_DUMP keep-alive with the demosaic pipe bypassed (`build_rawdump_bypass`
in `ife.h`): CAMIF runs → per-frame SOF on stock (res_id=0 dodges the OPLUS RDI hijack) →
normal CRM apply; RAW_DUMP raw Bayer feeds BPS unchanged. Init-only pixel config; per-frame
sends just the reg_update. Single IFE + `frame_height=3000` so the RAW_DUMP WM geometry
matches (line-based; frame-based is illegal on a processed WM).

### Env levers added to `spectra.cc` / `ife.h` for this work
- `OP9_NO_RDISOF` — force acquire `reserved=0` (use_rdi_sof=FALSE) on the RDI path.
- `OP9_FD_SOF` — acquire a 2nd pixel keep-alive out port (now FULL_DISP 0x3013)
  so the CAMIF drives per-frame SOF; adds the matching HFR(framedrop=0)/BW/
  VFE_OUT/io_cfg + programs the pixel pipe (was empty for pure RDI).
- `OP9_WM_MEASURE` — pre-arm bus WM debug-measure counters for ground-truth dims.
- `OP9_CROP_W`/`OP9_CROP_H`, `OP9_NVCDT` — CAMIF crop / vc-dt count for pixel-pipe
  bandwidth + clean frame geometry.

## Decision point

1. **Finish the pixel-keepalive (fully stock, no module patch).** Match the
   keep-alive WM to the base graft's native 4000×3000 demosaic output (or re-capture
   the HAL pixel-pipe at the exact keep-alive geometry) to clear the last CCIF
   violation. Highest-value, but a real RE session.
2. **Ship a minimal kernel patch (~8 lines).** Keep only the per-frame RDI SOF
   route in `cam_ife_csid_irq` + the RDI-drain apply in `cam_isp_context.c`; drop
   all debug/tolerance code. Defeats "unmodified stock" but is tiny and proven.

### Update 6 (2026-07-02) — SUSTAINED STREAMING SOLVED

The one-frame-then-flush death spiral (Update 5) is fixed. **Root cause:** after the
first frame, the CAMIF increments `frame_id` on *every* SOF — including the idle
`request_id=0` frames camerad now skips — so `frame_id_raw` is legitimately
non-contiguous between the frames we actually process (`frame ID skipped, 1 -> 4`).
`validateEvent`'s frame-id continuity check read that benign gap as a dropped frame →
`clearAndRequeue` → `clear_req_queue` → **CRM link flush** (`cam_req_mgr_process_flush_req
req_id 0`) → ISP ctx `CAM_CTX_FLUSHED` → every `config_dev` rejected → `sof_freeze`.

**Fix (kept in `spectra.cc` `validateEvent`):** for RAWSOF, on a `frame_id`/`request_id`
gap, **resync** (`skip_expected = true`, return true) instead of `clearAndRequeue` — the
gap is expected on this path, so never flush on it. Combined with the Update-3/4/5 fixes
(RAWSOF bypass, `frame_height=3000`, skip idle `request_id=0`, `config_ife` skip-on-fail).

**Result:** `op9fps` climbs to 330+ over the run, `clearing=0`, `sof_freeze=0`,
`config_dev wrong-state=0`; the BPS NV12 dump is a real demosaiced color scene. The
stock-kernel goal is met. Remaining is ordinary tuning (exposure/AE, image quality),
not stock-kernel enablement.
