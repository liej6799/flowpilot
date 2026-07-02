#pragma once

#include "cdm.h"

#include "system/camerad/cameras/hw.h"
#include "system/camerad/sensors/sensor.h"

// [op9/stock] MINIMAL RAW_DUMP pixel pipe with the demosaic pipeline BYPASSED. core_cfg_0=0x78002b00
// keeps the CAMIF running (so it emits per-frame SOF -> normal CRM apply on the stock kernel) but
// turns OFF demosaic/CSC/scalers/FD/DISP/stats -> nothing downstream to back-pressure the IPP fifo
// (the full-pipe graft overflows a single IFE because ~12 outputs are enabled but undrained).
// RAW_DUMP taps post-CAMIF raw Bayer = the same data the RDI tap gives, so it feeds BPS unchanged.
int build_rawdump_bypass(uint8_t *dst, const SensorInfo *s) {
  uint8_t *start = dst;
  dst += write_random(dst, {
    0x2c, 0x78002b00,   // core_cfg_0: PIPELINE BYPASS (RAW_DUMP), CAMIF on, demosaic/CSC off
    0x30, 0x00000010,   // core_cfg_1
    0x34, 0xffffffff,   // reg_update triggers
    0x38, 0xffffffff,
    0x3c, 0xffffffff,
  });
  // demux cfg (Bayer RAW10) — same as the processed path's demux block
  dst += write_cont(dst, 0x560, {
    0x00000001, 0x04440444, 0x04450445, 0x04440444, 0x04450445, 0x000000ca, 0x0000009c,
  });
  // CAMIF module_cfg — enable the CAMIF frame generator (needed for SOF)
  dst += write_random(dst, { 0x2660, 0x02000101 });
  // CAMIF epoch config (0x2680): HAL ground-truth 0x001402ed. Needed so the CAMIF fires the
  // per-frame EPOCH IRQ that the normal pixel state machine applies requests on (without it the
  // ctx gets SOF but never advances to apply -> no reg_update -> no buf_done).
  dst += write_random(dst, { 0x2680, 0x001402ed });
  // black level offset (harmless, keeps raw sane)
  dst += write_cont(dst, 0x6b0, {
    ((uint32_t)(1 << 11) << 0xf) | (s->black_level << (14 - s->bits_per_pixel)), 0x0, 0x0,
  });
  // [op9/stock] CAMIF full-frame crop so the CAMIF output EXACTLY matches the RAW_DUMP WM
  // (frame_width x frame_height). Without it the CAMIF crop is at reset default and the post-CAMIF
  // RAW_DUMP WM's expected size != received -> "PIXEL RAW DUMP: Image Size violation" -> no buf_done
  // -> active_req_cnt climbs -> the ctx stops notifying the CRM -> apply stalls -> sof_freeze.
  // (The RDI tap is pre-CAMIF so it never needed this; RAW_DUMP is post-CAMIF and does.)
  dst += write_cont(dst, 0xe0c, { 0x00000e00 });                                   // full-frame crop line cfg (enable)
  dst += write_cont(dst, 0xe10, { s->frame_height - 1, s->frame_width - 1 });      // crop: (height-1, width-1)
  return dst - start;
}

int build_common_ife_bps(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches, bool ife) {
  uint8_t *start = dst;

  /*
    Common between IFE and BPS.
  */

  // IFE -> BPS addresses
  /*
  std::map<uint32_t, uint32_t> addrs = {
    {0xf30, 0x3468},
  };
  */

  // YUV
  dst += write_cont(dst, ife ? 0xf30 : 0x3468, {
    0x00680208,
    0x00000108,
    0x00400000,
    0x03ff0000,
    0x01c01ed8,
    0x00001f68,
    0x02000000,
    0x03ff0000,
    0x1fb81e88,
    0x000001c0,
    0x02000000,
    0x03ff0000,
  });

  return dst - start;
}

int build_update(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches) {
  uint8_t *start = dst;

  // init sequence
  // [op9/SM8350] core_cfg_0 (0x2c) and core_cfg_1 (0x30): openpilot's tici code wrote
  // 0xffffffff here, which CORRUPTS core_cfg_0 -- the CAMIF then has input_pp_fmt=3
  // (not 0=BAYER) and input_mux_sel_pp=3, so it never recognizes the CSID's Bayer input
  // -> 0 SOF. The HAL's vendor config leaves these clean. Values captured via bpftrace
  // (camif IRQ -> mem_base, VFE:2): core_cfg_0=0x60002b00, core_cfg_1=0x10.
  // 0x34/0x38/0x3c remain 0xffffffff (reg-update triggers).
  dst += write_random(dst, {
    0x2c, 0x60002b00,
    0x30, 0x10,
    0x34, 0xffffffff,
    0x38, 0xffffffff,
    0x3c, 0xffffffff,
  });

  // demux cfg
  dst += write_cont(dst, 0x560, {
    0x00000001,
    0x04440444,
    0x04450445,
    0x04440444,
    0x04450445,
    0x000000ca,
    0x0000009c,
  });

  // white balance
  dst += write_cont(dst, 0x6fc, {
    0x00800080,
    0x00000080,
    0x00000000,
    0x00000000,
  });

  // module config/enables (e.g. enable debayer, white balance, etc.)
  dst += write_cont(dst, 0x40, {
    0x00000c06 | ((uint32_t)(cam.vignetting_correction) << 8),
  });
  dst += write_cont(dst, 0x44, {
    0x00000000,
  });
  dst += write_cont(dst, 0x48, {
    (1 << 3) | (1 << 1),
  });
  dst += write_cont(dst, 0x4c, {
    0x00000019,
  });
  dst += write_cont(dst, 0xf00, {
    0x00000000,
  });

  // cropping
  dst += write_cont(dst, 0xe0c, {
    0x00000e00,
  });
  dst += write_cont(dst, 0xe2c, {
    0x00000e00,
  });

  // black level scale + offset
  dst += write_cont(dst, 0x6b0, {
    ((uint32_t)(1 << 11) << 0xf) | (s->black_level << (14 - s->bits_per_pixel)),
    0x0,
    0x0,
  });

  return dst - start;
}


int build_initial_config(uint8_t *dst, const CameraConfig cam, const SensorInfo *s, std::vector<uint32_t> &patches, uint32_t out_width, uint32_t out_height) {
  uint8_t *start = dst;

  // start with the every frame config
  dst += build_update(dst, cam, s, patches);

  // [op9/SM8350] CAMIF module_cfg (0x2660): openpilot's tici code writes NO register in
  // the CAMIF 0x26xx range, and the kernel's camif_ver3 driver only writes epoch_irq_cfg
  // + reg_update_cmd -- so module_cfg is left at its reset default and the CAMIF never
  // enables, generating 0 SOF (-> pixel pipeline never starts -> WM never writes -> HALT).
  // The QCOM HAL's vendor-binary CDM config writes this; value captured via bpftrace
  // (cam_vfe_camif_ver3_handle_irq_bottom_half -> mem_base+0x2660) during HAL streaming
  // of the same IMX766 4000x3000 mode.
  dst += write_random(dst, {
    0x2660, 0x2000101,
  });

  uint64_t addr;

  // setup
  dst += write_cont(dst, 0x478, {
    0x00000004,
    0x004000c0,
  });
  dst += write_cont(dst, 0x488, {
    0x00000000,
    0x00000000,
    0x00000f0f,
  });
  dst += write_cont(dst, 0x49c, {
    0x00000001,
  });
  dst += write_cont(dst, 0xce4, {
    0x00000000,
    0x00000000,
  });

  // linearization
  dst += write_cont(dst, 0x4dc, {
    0x00000000,
  });
  dst += write_cont(dst, 0x4e0, s->linearization_pts);
  dst += write_cont(dst, 0x4f0, s->linearization_pts);
  dst += write_cont(dst, 0x500, s->linearization_pts);
  dst += write_cont(dst, 0x510, s->linearization_pts);
  // TODO: this is DMI64 in the dump, does that matter?
  dst += write_dmi(dst, &addr, s->linearization_lut.size()*sizeof(uint32_t), 0xc24, 9);
  patches.push_back(addr - (uint64_t)start);

  // vignetting correction
  dst += write_cont(dst, 0x6bc, {
    0x0b3c0000,
    0x00670067,
    0xd3b1300c,
    0x13b1300c,
  });
  dst += write_cont(dst, 0x6d8, {
    0xec4e4000,
    0x0100c003,
  });
  dst += write_dmi(dst, &addr, s->vignetting_lut.size()*sizeof(uint32_t), 0xc24, 14); // GRR
  patches.push_back(addr - (uint64_t)start);
  dst += write_dmi(dst, &addr, s->vignetting_lut.size()*sizeof(uint32_t), 0xc24, 15); // GBB
  patches.push_back(addr - (uint64_t)start);

  // debayer
  dst += write_cont(dst, 0x6f8, {
    0x00000100,
  });
  dst += write_cont(dst, 0x71c, {
    0x00008000,
    0x08000066,
  });

  // color correction
  dst += write_cont(dst, 0x760, s->color_correct_matrix);

  // gamma
  dst += write_cont(dst, 0x798, {
    0x00000000,
  });
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 26);  // G
  patches.push_back(addr - (uint64_t)start);
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 28);  // B
  patches.push_back(addr - (uint64_t)start);
  dst += write_dmi(dst, &addr, s->gamma_lut_rgb.size()*sizeof(uint32_t), 0xc24, 30);  // R
  patches.push_back(addr - (uint64_t)start);

  // output size/scaling
  dst += write_cont(dst, 0xa3c, {
    0x00000003,
    ((out_width - 1) << 16) | (s->frame_width - 1),
    0x30036666,
    0x00000000,
    0x00000000,
    s->frame_width - 1,
    ((out_height - 1) << 16) | (s->frame_height - 1),
    0x30036666,
    0x00000000,
    0x00000000,
    s->frame_height - 1,
  });
  dst += write_cont(dst, 0xa68, {
    0x00000003,
    ((out_width / 2 - 1) << 16) | (s->frame_width - 1),
    0x3006cccc,
    0x00000000,
    0x00000000,
    s->frame_width - 1,
    ((out_height / 2 - 1) << 16) | (s->frame_height - 1),
    0x3006cccc,
    0x00000000,
    0x00000000,
    s->frame_height - 1,
  });

  // cropping
  dst += write_cont(dst, 0xe10, {
    out_height - 1,
    out_width - 1,
  });
  dst += write_cont(dst, 0xe30, {
    out_height / 2 - 1,
    out_width - 1,
  });
  dst += write_cont(dst, 0xe18, {
    0x0ff00000,
    0x00000016,
  });
  dst += write_cont(dst, 0xe38, {
    0x0ff00000,
    0x00000017,
  });

  dst += build_common_ife_bps(dst, cam, s, patches, true);

  // [op9] HAL SM8350 pixel-pipe + demosaic config (0x3000-0x8FFF) captured from the stock HAL's IFE
  // CDM. camerad's stock writes use comma's ISP offsets (wrong for SM8350) so the pipe never processed.
  // 182 regs.
  dst += write_random(dst, {
    0x3058, 0x00000000,
    0x305c, 0x00000000,
    0x3060, 0x00e21203,
    0x3068, 0x000003c0,
    0x306c, 0x01001000,
    0x3070, 0x00004040,
    0x3074, 0x00000000,
    0x3078, 0x000020a8,
    0x307c, 0x000014fb,
    0x3080, 0x000007d7,
    0x3084, 0x00000c34,
    0x3088, 0x00000000,
    0x308c, 0x0bb80fa0,
    0x3090, 0x3c003c01,
    0x3094, 0x10131013,
    0x3098, 0x10121012,
    0x309c, 0x10131013,
    0x30a0, 0x10121012,
    0x30a4, 0x000000ca,
    0x30a8, 0x0000009c,
    0x30ac, 0x00004040,
    0x30b0, 0x000003ff,
    0x30b4, 0x00000000,
    0x30b8, 0x00200080,
  });
  dst += write_random(dst, {
    0x30bc, 0x00000020,
    0x30c0, 0x00000000,
    0x30c4, 0x00000190,
    0x30c8, 0x00000180,
    0x30cc, 0x000000fa,
    0x30d0, 0x00003ffc,
    0x3260, 0x00000000,
    0x4460, 0x00000001,
    0x4464, 0x00000600,
    0x4468, 0x0bb80fa0,
    0x446c, 0xc0200000,
    0x4470, 0x00000000,
    0x4474, 0xc0200000,
    0x4478, 0x00000000,
    0x447c, 0x00000000,
    0x4480, 0x00000000,
    0x4600, 0x20010001,
    0x4660, 0x00000001,
    0x4664, 0x00000600,
    0x4668, 0x0bb80fa0,
    0x466c, 0xc0400000,
    0x4670, 0x00000000,
    0x4674, 0xc0400000,
    0x4678, 0x00000000,
    0x467c, 0x000007d0,
  });
  dst += write_random(dst, {
    0x4680, 0x000005dc,
    0x4868, 0x000001df,
    0x486c, 0x0000013f,
    0x4a60, 0x00000000,
    0x4a68, 0x000007cf,
    0x4a6c, 0x000005db,
    0x4a70, 0x00ff0000,
    0x4a74, 0x00000017,
    0x4a78, 0x00ff0000,
    0x4a7c, 0x00000017,
    0x4c00, 0x20010000,
    0x4c60, 0x00000000,
    0x4c64, 0x00000600,
    0x4c68, 0x0bb80fa0,
    0x4c6c, 0x00000000,
    0x4c70, 0x00000000,
    0x4c74, 0x00000000,
    0x4c78, 0x00000000,
    0x4c7c, 0x00000fa0,
    0x4c80, 0x00000bb8,
    0x4e60, 0x00000001,
    0x4e64, 0x00000600,
    0x4e68, 0x0bb80fa0,
    0x4e6c, 0xc0400000,
    0x4e70, 0x00000000,
    0x4e74, 0xc0400000,
    0x4e78, 0x00000000,
    0x4e7c, 0x000007d0,
    0x4e80, 0x000005dc,
  });
  dst += write_random(dst, {
    0x5268, 0x000005db,
    0x526c, 0x000007cf,
    0x5408, 0x00000307,
    0x540c, 0x09016c7d,
    0x5504, 0x00b404eb,
    0x5508, 0x00000449,
    0x5608, 0x00000307,
    0x5704, 0x005a0275,
    0x5708, 0x00000449,
    0x5868, 0x0000010d,
    0x586c, 0x000000ef,
    0x5a68, 0x00000086,
    0x5a6c, 0x00000077,
    0x5c08, 0x00000f07,
    0x5c0c, 0x09016c7d,
    0x5d04, 0x0000010d,
    0x5d08, 0x00000113,
    0x5e08, 0x00000f07,
    0x5f04, 0x00000086,
    0x5f08, 0x00000113,
    0x6068, 0x00000043,
    0x606c, 0x0000003f,
    0x6268, 0x00000021,
    0x6260, 0x00000000,
    0x6264, 0x00000000,
    0x6270, 0x03ff0000,
    0x6274, 0x00000007,
    0x6278, 0x03ff0000,
    0x627c, 0x00000007,
    0x5000, 0x10010001,
    0x5060, 0x00000e01,
    0x5070, 0x03ff0000,
    0x5074, 0x00000006,
    0x5078, 0x03ff0000,
    0x507c, 0x00000006,
  });
  dst += write_random(dst, {
    0x5200, 0x10010000,
    0x5260, 0x00003e01,
    0x5270, 0x03ff0000,
    0x5274, 0x00000007,
    0x5278, 0x03ff0000,
    0x527c, 0x00000007,
  });
  dst += write_random(dst, {
    0x3800, 0x30060001,
    0x3860, 0x00004001,
    0x3868, 0x062b0400,
    0x386c, 0x00000849,
    0x3878, 0x00000080,
    0x387c, 0x00800066,
  });
  dst += write_random(dst, {
    0x626c, 0x0000001f,
    0x7e60, 0xffff0001,
    0x7e68, 0x00000000,
    0x7e6c, 0x00110000,
    0x7e70, 0x0000007b,
    0x7e74, 0x00170000,
    0x7e78, 0x0000007b,
    0x7e7c, 0x00000000,
    0x7e80, 0x3fff3fff,
    0x7e84, 0x3fff3fff,
    0x7e88, 0x00000000,
    0x7e8c, 0x00000000,
    0x8060, 0x00000001,
    0x8068, 0x00000000,
    0x806c, 0x05db0477,
    0x8260, 0xffff0101,
    0x8264, 0x00000000,
    0x8268, 0x03c96260,
    0x826c, 0x00230000,
    0x8270, 0x0000003d,
    0x8274, 0x002f0000,
    0x8278, 0x0000003d,
    0x827c, 0x3e7f3e7f,
    0x8280, 0x3e7f3e7f,
  });
  dst += write_random(dst, {
    0x8284, 0x00000000,
    0x8288, 0x00000000,
    0x8460, 0xffff0201,
    0x8468, 0x00000000,
    0x846c, 0x00230000,
    0x8470, 0x0000003d,
    0x8474, 0x002f0000,
    0x8478, 0x0000003d,
    0x8480, 0x3c3f3c3f,
    0x8484, 0x3c3f3c3f,
    0x8488, 0x00000000,
    0x848c, 0x00000000,
    0x8660, 0x00000001,
    0x8668, 0x00000000,
    0x866c, 0x05db0477,
    0x8858, 0x00000000,
    0x885c, 0x00000000,
    0x8860, 0x00221201,
    0x8864, 0x05b801b4,
    0x8868, 0x00000093,
    0x886c, 0x00000000,
    0x8870, 0x00000000,
    0x8874, 0x00000000,
    0x8878, 0x00000000,
  });
  dst += write_random(dst, {
    0x887c, 0x00000b6e,
    0x8880, 0xec00f492,
    0x8884, 0xcc3d6e51,
    0x8888, 0x00001400,
    0x888c, 0xd8f94a10,
    0x8890, 0x00000b6e,
    0x8894, 0x0000f492,
    0x8898, 0xcc3d6e51,
    0x889c, 0x00001400,
    0x88a0, 0x0000ec00,
    0x88a4, 0xd8f94a10,
    0x88a8, 0x00000033,
    0x88ac, 0x0000007d,
    0x88b0, 0x00000000,
    0x88b4, 0x03081040,
    0x88b8, 0x0b247144,
    0x88bc, 0x0000040d,
    0x88c0, 0x0000007d,
    0x88c4, 0x00000000,
    0x88c8, 0x03081040,
    0x88cc, 0x0b247144,
    0x88d0, 0x0000040d,
    0x88d4, 0x00000010,
    0x88d8, 0x00000010,
  });
  dst += write_random(dst, {
    0x88dc, 0x00000000,
    0x88e0, 0x00000000,
    0x88e4, 0x00000000,
    0x88e8, 0x00000000,
    0x88ec, 0x00000000,
    0x88f0, 0x00200040,
    0x88f4, 0x0b9808b0,
    0x8a60, 0x00000701,
    0x8a68, 0x00000000,
    0x8a6c, 0x03ff0003,
    0x8a70, 0x0001023b,
    0x8e60, 0x00000c01,
    0x8e68, 0x00000000,
    0x8e6c, 0x05450477,
  });
  // [op9 binary-RE] Video-Full (FULL 0x3000) chroma crop, decoded from camx module table.
  dst += write_random(dst, {
    0x7c68, 0x00000001,
    0x7c74, 0x05db0477,
  });

  // [op9] FD/preview NV12 path (OP9_FD): the self-contained FD scaler (0x8860 + inline FIR 0x887c-0x88d8,
  // FD-Y out 0x8a60, FD-C out 0x8e60 — all already configured above) writes 4:2:0 NV12 to WM8/WM9 in
  // COMP_GRP_2, a SEPARATE comp from FULL_DISP (WM4/5). That sidesteps both the FULL_DISP odd-luma wall
  // and the DISP-C CCIF wall. Here we (a) turn on the FD crop/scaler (0x8858/0x885c, off by default), and
  // (b) turn OFF the FULL_DISP chroma out (0x5060/0x5260 -> WM4/5) which we no longer acquire, so its
  // chroma data can't back-pressure the pixel pipe.
  if (getenv("OP9_FD")) {
    // [op9/workflow whjq1f743] The 640x480 Y + 640x240 C (4:2:0) NV12 is produced by the FULL_DISP
    // MNDS path: luma-MNDS 0x4660 (downscale to an EVEN 640x480 -> no odd-luma wall) -> luma OUT
    // 0x4860 (out-id 0x16) + chroma OUT 0x4A60 (out-id 0x17). The half-height chroma comes from the
    // R2PD (Round-2-Plane-Down) bits[13:12] in chroma-OUT module_cfg 0x4A60 = 0x3E01 (the deployed
    // ife.h had 0x4A60=0 -> R2PD off -> full-height chroma -> WM Image-Size violation). Values from
    // ife1_hal.bin (stock HAL). The 0x88xx block is a luma-only stats downscaler, not the output.
    dst += write_random(dst, {
      // Y_FD MNDS 0x4460 (Module ID:14 = DOWNSCALE_MN_Y_FD, the FD luma scaler feeding OUT 0x4860) ->
      // 6.25x both axes (0xc0c80000) -> 640x480 for our 4000x3000 pipe. (NOT 0x4660 Y_DISP; input
      // 0x4468 is already 0x0bb80fa0 in the base config, just override the H/V ratios.)
      0x4460, 0x00000001, 0x4468, 0x0bb80fa0,
      0x446c, 0xc0c80000, 0x4470, 0x00000000, 0x4474, 0xc0c80000, 0x4478, 0x00000000,
      // luma OUT 0x4860 (out-id 0x16) crop 640x480
      0x4860, 0x00000e01, 0x4868, 0x000001df, 0x486c, 0x0000013f,
      0x4870, 0x00ff0000, 0x4874, 0x00000016, 0x4878, 0x00ff0000, 0x487c, 0x00000016,
      // chroma OUT 0x4A60 (out-id 0x17): module_cfg 0x3e01 = R2PD chroma 2-plane->NV12 FORMAT (NOT a
      // 2nd decimation — the C_FD MNDS above does the 12.5x size). Mirrors the prior WORKING chroma
      // OUT 0x5260=0x3e01. crop: 0x4a68=0xef(239->240 rows, unit=1 row); HORIZONTAL crop counts in
      // 2-SAMPLE UNITS (luma 640px -> 0x13f=320 units): chroma 320 samples -> 160 units -> 0x9f.
      // (The HAL programs 0x9f; the earlier "fix" to 0x13f passed 640 samples=1280B vs the WM's 640B
      // -> "FD C: Image Size violation" on every frame.)
      // [op9/stock] R2PD OFF (bits[13:12] of 0x4a60): the chroma stream is ALREADY 4:2:0 from the
      // CST (input decl 2000x1500 above) -- with R2PD on, the rows get halved AGAIN (240->120)
      // and the FD C WM (640B x 240) size-violates. 0x3e01 -> 0x0e01.
      0x4a60, 0x00000e01, 0x4a68, 0x000000ef, 0x4a6c, 0x0000009f,
      0x4a70, 0x00ff0000, 0x4a74, 0x00000017, 0x4a78, 0x00ff0000, 0x4a7c, 0x00000017,
      // C_FD MNDS 0x4c60 (Module ID:15, the FD chroma scaler feeding OUT 0x4A60): the CST emits
      // 4:2:0 chroma = 2000x1500 for the 4000x3000 pipe, so the MNDS INPUT DECLARATION must be
      // 2000x1500 (0x4c68 = (1500<<16)|2000 = 0x05dc07d0), NOT the luma 4000x3000 -- a wrong
      // declared input is exactly a "PIXEL PIPE VIOLATION Module ID:15" (in-ROI vs stream
      // mismatch inside the module). Ratios then 6.25x both (like luma): 2000->320 samples
      // (640B) and 1500->240 rows, matching the FD C WM (640B x 240) directly.
      0x4c60, 0x00000001, 0x4c68, 0x05dc07d0,
      0x4c6c, 0xc0c80000, 0x4c70, 0x00000000, 0x4c74, 0xc0c80000, 0x4c78, 0x00000000,
      0x4c7c, 0x00000000, 0x4c80, 0x00000000,
      // disable the OTHER (DISP-ladder) chroma scaler/outs we don't drain (avoid back-pressure)
      0x4e60, 0x00000000, 0x5060, 0x00000000, 0x5260, 0x00000000,
      // FD stats scaler off (not the output path)
      0x8858, 0x00000000, 0x885c, 0x00000000,
    });
    // [op9/stock] OP9_FD_Y: disable the FD CHROMA modules so the pixel pipe is LUMA-ONLY (no chroma WM,
    // no chroma image-size violation that hard-halts the IFE after frame 1 on stock). Keeps Y_FD MNDS
    // 0x4460 + FD-Y-out 0x4860. Later CDM write wins -> the FD output becomes a clean Y-only SOF source.
    if (getenv("OP9_FD_Y")) {
      dst += write_random(dst, {
        0x4a60, 0x00000000,                                            // FD-C-out OFF
        0x4c60, 0x00000000, 0x4c6c, 0x00000000, 0x4c74, 0x00000000,    // C_FD MNDS OFF
      });
    }
  }

  return dst - start;
}


