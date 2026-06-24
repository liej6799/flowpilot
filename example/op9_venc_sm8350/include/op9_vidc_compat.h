#pragma once
// [op9] comma three's v4l_encoder.cc uses legacy Qualcomm Venus VIDC V4L2 controls.
// The OnePlus 9 (SM8350) kernel does NOT define most of them (HEVC profile/level moved to the
// upstream V4L2_CID_MPEG_VIDEO_HEVC_* controls; the rest are gone). These shims let the code
// COMPILE so loggerd (which never touches the encoder) can build/run. WARNING: the encoder
// control programming below is NOT valid for the SM8350 Venus — encoderd needs a real port.
#include <linux/v4l2-controls.h>

// HEVC profile/level: map to upstream controls (these exist on SM8350)
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_HEVC_PROFILE
#define V4L2_CID_MPEG_VIDC_VIDEO_HEVC_PROFILE             V4L2_CID_MPEG_VIDEO_HEVC_PROFILE
#endif
#ifndef V4L2_MPEG_VIDC_VIDEO_HEVC_PROFILE_MAIN
#define V4L2_MPEG_VIDC_VIDEO_HEVC_PROFILE_MAIN            V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_HEVC_TIER_LEVEL
#define V4L2_CID_MPEG_VIDC_VIDEO_HEVC_TIER_LEVEL          V4L2_CID_MPEG_VIDEO_HEVC_LEVEL
#endif
#ifndef V4L2_MPEG_VIDC_VIDEO_HEVC_LEVEL_HIGH_TIER_LEVEL_5
#define V4L2_MPEG_VIDC_VIDEO_HEVC_LEVEL_HIGH_TIER_LEVEL_5 V4L2_MPEG_VIDEO_HEVC_LEVEL_5
#endif
#ifndef V4L2_MPEG_VIDEO_H264_LEVEL_UNKNOWN
#define V4L2_MPEG_VIDEO_H264_LEVEL_UNKNOWN               0
#endif

// legacy VIDC-only controls absent on SM8350 — stubbed (compile-only; not functional here)
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_VUI_TIMING_INFO
#define V4L2_CID_MPEG_VIDC_VIDEO_VUI_TIMING_INFO         (V4L2_CID_MPEG_BASE + 1000)
#endif
#ifndef V4L2_MPEG_VIDC_VIDEO_VUI_TIMING_INFO_ENABLED
#define V4L2_MPEG_VIDC_VIDEO_VUI_TIMING_INFO_ENABLED     1
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_H264_CABAC_MODEL
#define V4L2_CID_MPEG_VIDC_VIDEO_H264_CABAC_MODEL        (V4L2_CID_MPEG_BASE + 1001)
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_H264_CABAC_MODEL_0
#define V4L2_CID_MPEG_VIDC_VIDEO_H264_CABAC_MODEL_0      0
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_NUM_P_FRAMES
#define V4L2_CID_MPEG_VIDC_VIDEO_NUM_P_FRAMES            (V4L2_CID_MPEG_BASE + 1002)
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES
#define V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES            (V4L2_CID_MPEG_BASE + 1003)
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_RATE_CONTROL
#define V4L2_CID_MPEG_VIDC_VIDEO_RATE_CONTROL            (V4L2_CID_MPEG_BASE + 1004)
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_RATE_CONTROL_VBR_CFR
#define V4L2_CID_MPEG_VIDC_VIDEO_RATE_CONTROL_VBR_CFR    0
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_PRIORITY
#define V4L2_CID_MPEG_VIDC_VIDEO_PRIORITY                (V4L2_CID_MPEG_BASE + 1005)
#endif
#ifndef V4L2_MPEG_VIDC_VIDEO_PRIORITY_REALTIME_DISABLE
#define V4L2_MPEG_VIDC_VIDEO_PRIORITY_REALTIME_DISABLE   0
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_IDR_PERIOD
#define V4L2_CID_MPEG_VIDC_VIDEO_IDR_PERIOD              (V4L2_CID_MPEG_BASE + 1006)
#endif

// [op9] Iris2 framerate control (S_PARM unsupported on this venc) — value is Q16 (fps<<16)
#ifndef V4L2_CID_MPEG_MSM_VIDC_BASE
#define V4L2_CID_MPEG_MSM_VIDC_BASE          (V4L2_CTRL_CLASS_MPEG | 0x2000)
#endif
#ifndef V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE
#define V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE  (V4L2_CID_MPEG_MSM_VIDC_BASE + 119)
#endif
