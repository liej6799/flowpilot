#include <cmath>
#include <iterator>

#include "system/camerad/sensors/sensor.h"
#include "system/camerad/sensors/generated/imx766_mode_init.h"  // model-constant tables (no per-unit data)
#include "system/camerad/sensors/sensor_qsc.h"                  // runtime QSC splice from EEPROM

// Sony IMX766 (OnePlus 9). Modeled on the os04c10 driver; ISP tuning params are
// placeholders (reuse os04c10 values) until tuned for the IMX766. Probe-capable.

namespace {

const float sensor_analog_gains_IMX766[] = {
    1.0, 1.0625, 1.125, 1.1875, 1.25, 1.3125, 1.375, 1.4375, 1.5, 1.5625, 1.6875,
    1.8125, 1.9375, 2.0, 2.125, 2.25, 2.375, 2.5, 2.625, 2.75, 2.875, 3.0,
    3.125, 3.375, 3.625, 3.875, 4.0, 4.25, 4.5, 4.75, 5.0, 5.25, 5.5,
    5.75, 6.0, 6.25, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0,
    10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 13.5, 14.0, 14.5, 15.0, 15.5};

}  // namespace

IMX766::IMX766() {
  image_sensor = cereal::FrameData::ImageSensor::IMX766;
  bayer_pattern = CAM_ISP_PATTERN_BAYER_RGRGRG;  // IMX766 RGGB; verify on tune
  pixel_size_mm = 0.0016;  // 2x2-binned cell ~1.6um (0.8um native x2)
  data_word = false;       // Sony IMX: WORD addr, BYTE data (confirmed by HAL capture)

  // Mode: 2x2-binned full-FOV 4096x3072 RAW10 C-PHY (the mode the HAL actually
  // streams; init built below from generated/imx766_mode_init.h + EEPROM QSC).
  // out_scale=1 for first bring-up; switch to out_scale=2 -> 2048x1536 once frames are confirmed.
  out_scale = 1;
  frame_width = 4096;   // [op9] stock ultrawide mode 0x034C=0x1000
  frame_height = 3072;  // [op9] 0x034E=0x0C00
  frame_stride = frame_width * 2;  // [op9] PLAIN16_10 output (2B/px, 16-byte aligned) for RDI WM

  extra_height = 0;
  frame_offset = 0;

  // [op9] Stream on/off is the SMIA++ standard 0x0100 write (not per-unit).
  start_reg_array = {{0x0100, 0x01}};

  // [op9] Build the streaming init from model-constant tables + THIS unit's QSC
  // calibration, read from the sensor EEPROM at runtime. NO per-unit data is
  // compiled into the binary. The QSC window (regs 0xc800..0xd3ff =
  // eeprom[8144:11216]) is spliced between the pre/post tables. If the EEPROM
  // can't be read, init streams without QSC (raw OK; cooked shading degraded).
  // See docs/SENSOR-CALIBRATION-EEPROM.md.
  SensorInitSpec qsc{};
  qsc.pre = imx766_mode_init_pre;   qsc.pre_n = std::size(imx766_mode_init_pre);
  qsc.post = imx766_mode_init_post; qsc.post_n = std::size(imx766_mode_init_post);
  qsc.derived_tail = nullptr;       qsc.derived_tail_n = 0;
  qsc.qsc_reg_lo = 0xc800;          qsc.qsc_len = 3072;
  qsc.eeprom_qsc_off = 8144;
  qsc.eeprom_path = "/mnt/vendor/persist/camera/eeprom_imx766_gt24p128ca2.bin";
  init_reg_array = build_sensor_init(qsc);

  // [op9] Apply as the HAL's semantic groups (BASE_INIT / QSC / RES), each its
  // own CONFIG_DEV (one giant i2c packet overflows the kernel CDM/CCI path).
  // Sums to init_reg_array.size() only when QSC loaded; otherwise spectra.cc
  // falls back to fixed-size chunking.
  init_group_sizes = {(int)std::size(imx766_mode_init_pre),
                      (int)(qsc.qsc_len + qsc.derived_tail_n),
                      (int)std::size(imx766_mode_init_post)};
  apply_init_exposure = true;
  mipi_cphy = true;  // [op9] IMX766 streams C-PHY 3-trio (HAL: is_3phase=1, lane_cnt=3)

  // Sony IMX766 sensor ID: 0x0016/0x0017 (WORD) == 0x0766
  probe_reg_addr = 0x0016;
  probe_expected_data = 0x0766;

  bits_per_pixel = 10;
  mipi_format = CAM_FORMAT_MIPI_RAW_10;
  frame_data_type = CSI_RAW10;
  mclk_frequency = 19200000;       // 19.2 MHz (OnePlus 9 DT clock-rates)
  mipi_data_rate = 1925500000ULL;  // [op9] 4000x3000 mode (PLL-derived)
  mipi_settle = 2800000000ULL;     // 2.8 us (HAL CSIPHY log)

  readout_time_ns = 11000000;

  // --- exposure / gain (placeholders, reuse os04c10-style) ---
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
    sensor_analog_gains[i] = sensor_analog_gains_IMX766[i];
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

std::vector<i2c_random_wr_payload> IMX766::getExposureRegisters(int exposure_time, int new_exp_g, bool dc_gain_enabled) const {
  // Sony IMX coarse integration time @ 0x0202/0x0203, analog gain @ 0x0204/0x0205.
  uint32_t e = std::max(exposure_time, 2);
  return {
    {0x0202, (uint16_t)((e >> 8) & 0xff)},
    {0x0203, (uint16_t)(e & 0xff)},
    {0x0204, (uint16_t)((new_exp_g >> 8) & 0xff)},
    {0x0205, (uint16_t)(new_exp_g & 0xff)},
  };
}

float IMX766::getExposureScore(float desired_ev, int exp_t, int exp_g_idx, float exp_gain, int gain_idx) const {
  float score = std::abs(desired_ev - (exp_t * exp_gain));
  score += std::abs(exp_g_idx - analog_gain_rec_idx) * analog_gain_cost_low;
  return score;
}

int IMX766::getSlaveAddress(int port) const {
  // OnePlus 9: IMX766 i2c slave (8-bit) = 0x34 (from dmesg). One entry per port.
  return 0x34;
}
