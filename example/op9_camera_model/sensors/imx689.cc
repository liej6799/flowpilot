#include <cmath>

#include "system/camerad/sensors/sensor.h"

// Sony IMX689 (OnePlus 9, main wide = cam-sensor@0, CSIPHY 1, slot 0).
// Confirmed by kernel dmesg: sensor_id 0x689, slave_addr 0x34, slot 0.
// Modeled on the IMX766 driver (same Sony IMX register conventions); ISP
// tuning params are placeholders until tuned for the IMX689. Probe-capable.
//
// TODO(op9/imx689): mipi_data_rate / mipi_settle are placeholders carried over
// from IMX766's 2x2-binned mode -- capture the real CSIPHY 1 values from a HAL
// open of the wide camera (CAM_START_PHYDEV dmesg line). Without the real
// streaming register table in imx689_registers.h the sensor will probe but not
// emit frames.

namespace {

const float sensor_analog_gains_IMX689[] = {
    1.0, 1.0625, 1.125, 1.1875, 1.25, 1.3125, 1.375, 1.4375, 1.5, 1.5625, 1.6875,
    1.8125, 1.9375, 2.0, 2.125, 2.25, 2.375, 2.5, 2.625, 2.75, 2.875, 3.0,
    3.125, 3.375, 3.625, 3.875, 4.0, 4.25, 4.5, 4.75, 5.0, 5.25, 5.5,
    5.75, 6.0, 6.25, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0,
    10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 13.5, 14.0, 14.5, 15.0, 15.5};

}  // namespace

IMX689::IMX689() {
  image_sensor = cereal::FrameData::ImageSensor::IMX689;
  bayer_pattern = CAM_ISP_PATTERN_BAYER_RGRGRG;  // IMX689 RGGB; verify on tune
  pixel_size_mm = 0.0012;  // IMX689 native 1.2um cell (placeholder)
  data_word = false;       // Sony IMX: WORD addr, BYTE data (same as IMX766)

  // 2x2-binned 4000x3000 RAW10 (placeholder, matches IMX766 binned mode until
  // the real IMX689 mode register table is extracted). out_scale=1 for first
  // bring-up (no IFE scaler).
  out_scale = 1;
  frame_width = 4000;
  frame_height = 3000;
  frame_stride = frame_width * 2;  // [op9] PLAIN16_10 output (2B/px, 16-byte aligned) for legal RDI/RAW WM packer

  extra_height = 0;
  frame_offset = 0;

  start_reg_array.assign(std::begin(start_reg_array_imx689), std::end(start_reg_array_imx689));
  init_reg_array.assign(std::begin(init_array_imx689), std::end(init_array_imx689));
  apply_init_exposure = true;  // [op9] initial exposure before stream-on (Sony IMX)

  // Sony IMX689 sensor ID: 0x0016/0x0017 (WORD) == 0x0689 (confirmed dmesg).
  probe_reg_addr = 0x0016;
  probe_expected_data = 0x0689;

  bits_per_pixel = 10;
  mipi_format = CAM_FORMAT_MIPI_RAW_10;
  frame_data_type = CSI_RAW10;
  mclk_frequency = 19200000;       // 19.2 MHz (OnePlus 9 DT clock-rates)
  // TODO(op9/imx689): capture real CSIPHY 1 datarate/settle from a HAL wide-cam
  // open (CAM_START_PHYDEV dmesg). IMX766 binned values used as placeholder.
  mipi_data_rate = 1925500000ULL;
  mipi_settle = 2800000000ULL;     // 2.8 us
  mipi_cphy = true;  // [op9] stock IMX689 = C-PHY 3-trio (CSID lane_type:1 lane_num:3); was defaulting to D-PHY -> CSIPHY could not decode -> no MIPI

  readout_time_ns = 11000000;

  // --- exposure / gain (placeholders, reuse IMX766-style) ---
  ev_scale = 150.0;
  dc_gain_factor = 1;
  dc_gain_min_weight = 1;
  dc_gain_max_weight = 1;
  dc_gain_on_grey = 0.9;
  dc_gain_off_grey = 1.0;
  exposure_time_min = 2;
  exposure_time_max = 2352;
  analog_gain_min_idx = 0x0;
  analog_gain_rec_idx = 0x0;
  analog_gain_max_idx = 0x28;
  analog_gain_cost_delta = -1;
  analog_gain_cost_low = 0.4;
  analog_gain_cost_high = 6.4;
  for (int i = 0; i <= analog_gain_max_idx; i++) {
    sensor_analog_gains[i] = sensor_analog_gains_IMX689[i];
  }
  min_ev = exposure_time_min * sensor_analog_gains[analog_gain_min_idx];
  max_ev = exposure_time_max * dc_gain_factor * sensor_analog_gains[analog_gain_max_idx];
  target_grey_factor = 0.01;

  // --- ISP params (placeholders) ---
  black_level = 64;  // RAW10 typical
  color_correct_matrix = {
    0x000000c2, 0x00000fe0, 0x00000fde,
    0x00000fa7, 0x000000d9, 0x00001000,
    0x00000fca, 0x00000fef, 0x000000c7,
  };
  gamma_lut_rgb = {};
  for (int i = 0; i < 65; i++) {
    float fx = i / 64.0;
    gamma_lut_rgb.push_back((uint32_t)((10*fx)/(1+9*fx)*1023.0 + 0.5));
  }
  prepare_gamma_lut();

  linearization_lut = std::vector<uint32_t>(36, 0);
  linearization_pts = {0x0fff0fff, 0x0fff0fff, 0x0fff0fff, 0x0fff0fff};
  vignetting_lut = std::vector<uint32_t>(221, 0);
}

std::vector<i2c_random_wr_payload> IMX689::getExposureRegisters(int exposure_time, int new_exp_g, bool dc_gain_enabled) const {
  // Sony IMX coarse integration time @ 0x0202/0x0203, analog gain @ 0x0204/0x0205.
  uint32_t e = std::max(exposure_time, 2);
  return {
    {0x0202, (uint16_t)((e >> 8) & 0xff)},
    {0x0203, (uint16_t)(e & 0xff)},
    {0x0204, (uint16_t)((new_exp_g >> 8) & 0xff)},
    {0x0205, (uint16_t)(new_exp_g & 0xff)},
  };
}

float IMX689::getExposureScore(float desired_ev, int exp_t, int exp_g_idx, float exp_gain, int gain_idx) const {
  float score = std::abs(desired_ev - (exp_t * exp_gain));
  score += std::abs(exp_g_idx - analog_gain_rec_idx) * analog_gain_cost_low;
  return score;
}

int IMX689::getSlaveAddress(int port) const {
  // OnePlus 9: IMX689 i2c slave (8-bit) = 0x34 (confirmed dmesg slot 0).
  // One entry per port.
  return 0x34;
}
