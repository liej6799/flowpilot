#pragma once

// Sony IMX766 (OnePlus 9 / LE2110) register tables.
//
// Sony IMX sensors: ID at 0x0016/0x0017 (WORD) = 0x0766; stream on 0x0100=0x01.
//
// NOTE: init_array_imx766 below is a MINIMAL bring-up stub (software reset only).
// The full streaming-mode register table (PLL/clocks, MIPI, output size,
// binning, line/frame length, ...) must be extracted from the OnePlus 9 camera
// HAL sensor lib or an i2c trace (see docs/SENSOR-EXTRACTION.md). Without it the
// sensor PROBES but does not stream a valid frame.

const struct i2c_random_wr_payload start_reg_array_imx766[] = {{0x0100, 0x01}};
const struct i2c_random_wr_payload stop_reg_array_imx766[]  = {{0x0100, 0x00}};

const struct i2c_random_wr_payload init_array_imx766[] = {
  {0x0103, 0x01},  // software reset
  // TODO(op9): full IMX766 streaming init register table goes here.
  // Extract via: i2c trace of the Android camera HAL while it streams cam 2,
  // or from /vendor/lib64/camera/components/com.qti.sensor.imx766.so.
};
