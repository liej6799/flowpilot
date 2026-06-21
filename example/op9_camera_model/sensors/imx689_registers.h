#pragma once

// Sony IMX689 (OnePlus 9 / LE2110, main wide = cam-sensor@0) register tables.
//
// Confirmed live from kernel dmesg at boot:
//   CAM-SENSOR: Probe success, slot:0, slave_addr:0x34, sensor_id:0x689
// Sony IMX: WORD addr, BYTE data. Sensor ID at 0x0016/0x0017 (WORD) == 0x0689.
// Stream on/off at 0x0100.
//
// NOTE: init_array_imx689 below is a MINIMAL bring-up stub (software reset only).
// The full streaming-mode register table (PLL/clocks, MIPI, output size,
// binning, line/frame length, ...) must be extracted from the OnePlus 9 camera
// HAL sensor lib (/odm/lib64/camera/com.qti.sensor.imx689.lemonade.so) or an
// ftrace kprobe on camera_io_dev_write while the HAL opens the wide camera.
// See docs/SENSOR-EXTRACTION.md. Without it the sensor PROBES but does not
// stream a valid frame.

const struct i2c_random_wr_payload start_reg_array_imx689[] = {{0x0100, 0x01}};
const struct i2c_random_wr_payload stop_reg_array_imx689[]  = {{0x0100, 0x00}};

const struct i2c_random_wr_payload init_array_imx689[] = {
  {0x0103, 0x01},  // software reset
  // TODO(op9/imx689): full IMX689 streaming init register table goes here.
  // Extract via: ftrace kprobe on camera_io_dev_write while the Android HAL
  // opens the wide camera (slot 0 / CSIPHY 1), or parse
  // /odm/lib64/camera/com.qti.sensormodule.lemonade.imx689.bin (Qualcomm CSL
  // sensor-module format).
};
