#pragma once

#include "common/util.h"
#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/visionipc/visionipc_server.h"

#include "media/cam_isp_ife.h"


typedef enum {
  ISP_RAW_OUTPUT,   // raw frame from sensor
  ISP_IFE_PROCESSED,  // fully processed image through the IFE
  ISP_BPS_PROCESSED,  // fully processed image through the BPS
} SpectraOutputType;

// For the comma 3X three camera platform

struct CameraConfig {
  int camera_num;
  VisionStreamType stream_type;
  float focal_len;  // millimeters
  const char *publish_name;
  cereal::FrameData::Builder (cereal::Event::Builder::*init_camera_state)();
  bool enabled;
  uint32_t phy;
  int csiphy_index;  // [op9] CSIPHY subdev index (may differ from camera_num)
  bool vignetting_correction;
  SpectraOutputType output_type;
  bool staggered_sof;  // SOF is staggered (half-period offset) from other cameras
};

// NOTE: to be able to disable road and wide road, we still have to configure the sensor over i2c
// If you don't do this, the strobe GPIO is an output (even in reset it seems!)
const CameraConfig WIDE_ROAD_CAMERA_CONFIG = {
  .camera_num = 0,
  .stream_type = VISION_STREAM_WIDE_ROAD,
  .focal_len = 1.71,
  .publish_name = "wideRoadCameraState",
  .init_camera_state = &cereal::Event::Builder::initWideRoadCameraState,
  .enabled = !getenv("DISABLE_WIDE_ROAD"),
  .phy = CAM_ISP_IFE_IN_RES_PHY_1,  // [op9/imx689] main wide IN_RES_PHY_1 matches CSIPHY 1 (was PHY_2 BUG)
  .csiphy_index = 1,
  .vignetting_correction = false,
  .output_type = ISP_RAW_OUTPUT,
  .staggered_sof = false,
};

const CameraConfig ROAD_CAMERA_CONFIG = {
  .camera_num = 1,
  .stream_type = VISION_STREAM_ROAD,
  .focal_len = 8.0,
  .publish_name = "roadCameraState",
  .init_camera_state = &cereal::Event::Builder::initRoadCameraState,
  .enabled = !getenv("DISABLE_ROAD"),
  .phy = CAM_ISP_IFE_IN_RES_PHY_2,  // [op9] IMX766 on CSIPHY 2
  .csiphy_index = 2,
  .vignetting_correction = false,
  // [op9] camX/HAL bpftrace (VFE_OUT_CONFIG capture) PROVES the HAL streams the IMX766
  // wide via the RDI raw path (WM port 0x3007 = CAM_ISP_IFE_OUT_RES_RDI_1), NOT the
  // debayer/pixel pipe -- which is why VFE:2's pixel modules read 0 under the HAL and
  // why ISP_IFE_PROCESSED hangs (SM8350 debayer regs differ from tici). Use RAW like
  // the HAL; the WM is armed via the per-frame VFE_OUT_CONFIG blob (virtual_frame_en=0).
  .output_type = ISP_RAW_OUTPUT,
  .staggered_sof = false,
};

const CameraConfig DRIVER_CAMERA_CONFIG = {
  .camera_num = 2,
  .stream_type = VISION_STREAM_DRIVER,
  .focal_len = 1.71,
  .publish_name = "driverCameraState",
  .init_camera_state = &cereal::Event::Builder::initDriverCameraState,
  .enabled = !getenv("DISABLE_DRIVER"),
  .phy = CAM_ISP_IFE_IN_RES_PHY_2,
  .csiphy_index = 5,
  .vignetting_correction = false,
  .output_type = ISP_BPS_PROCESSED,
  .staggered_sof = true,
};

const CameraConfig ALL_CAMERA_CONFIGS[] = {WIDE_ROAD_CAMERA_CONFIG, ROAD_CAMERA_CONFIG, DRIVER_CAMERA_CONFIG};
