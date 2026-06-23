#pragma once
//
// Runtime QSC splice — builds a sensor's streaming init from the committed
// model-constant tables (sensors/generated/<name>_mode_init.h) plus the per-unit
// QSC calibration read from THIS device's EEPROM. No per-unit data is compiled
// into the binary; one camerad build runs on any unit.
//
// See docs/SENSOR-CALIBRATION-EEPROM.md for the proof that the QSC window is a
// verbatim EEPROM copy.
//
// Usage in a sensor ctor (e.g. sensors/imx766.cc). Plain member assignment is
// used (not C++20 designated initializers) so it builds under any C++ standard:
//
//   #include "system/camerad/sensors/generated/imx766_mode_init.h"
//   #include "system/camerad/sensors/sensor_qsc.h"
//   ...
//   SensorInitSpec spec{};
//   spec.pre = imx766_mode_init_pre;   spec.pre_n = std::size(imx766_mode_init_pre);
//   spec.post = imx766_mode_init_post; spec.post_n = std::size(imx766_mode_init_post);
//   spec.derived_tail = nullptr;       spec.derived_tail_n = 0;
//   spec.qsc_reg_lo = 0xc800;          spec.qsc_len = 3072;
//   spec.eeprom_qsc_off = 8144;
//   spec.eeprom_path = "/mnt/vendor/persist/camera/eeprom_imx766_gt24p128ca2.bin";
//   init_reg_array = build_sensor_init(spec);
//
// (imx689 passes imx689_qsc_derived_tail / std::size(...) for the HAL-derived
// tail, qsc_reg_lo = 0xd000, eeprom_qsc_off = 7936.)
//
// The numeric constants above come straight from
// sensors/generated/<name>_init_meta.json; keep them in sync (or codegen the
// ctor call from the JSON).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "system/camerad/sensors/sensor.h"  // i2c_random_wr_payload

struct SensorInitSpec {
  const i2c_random_wr_payload *pre;   size_t pre_n;
  const i2c_random_wr_payload *post;  size_t post_n;
  const i2c_random_wr_payload *derived_tail;  size_t derived_tail_n;  // nullable
  uint16_t qsc_reg_lo;   // first QSC register (auto-increment window base)
  size_t   qsc_len;      // QSC bytes copied from EEPROM
  size_t   eeprom_qsc_off;  // byte offset of QSC inside the EEPROM image
  const char *eeprom_path;  // on-device EEPROM cache (or i2c-dumped image)
};

// Read the per-unit EEPROM and splice the QSC window into the model-constant
// init. Returns pre + QSC(from EEPROM) + derived_tail + post.
//
// If the EEPROM cannot be read, logs and returns pre + derived_tail + post with
// the QSC OMITTED. The RDI/raw path bypasses the ISP debayer that QSC feeds, so
// a raw frame should still stream (validate per docs/SENSOR-CALIBRATION-EEPROM.md
// §7). For the cooked/NV12 path a missing QSC degrades shading/remosaic only.
inline std::vector<i2c_random_wr_payload> build_sensor_init(const SensorInitSpec &s) {
  std::vector<i2c_random_wr_payload> out;
  out.reserve(s.pre_n + s.qsc_len + s.derived_tail_n + s.post_n);

  out.insert(out.end(), s.pre, s.pre + s.pre_n);

  std::vector<uint8_t> qsc(s.qsc_len, 0);
  bool have_qsc = false;
  if (FILE *f = std::fopen(s.eeprom_path, "rb")) {
    if (std::fseek(f, (long)s.eeprom_qsc_off, SEEK_SET) == 0 &&
        std::fread(qsc.data(), 1, s.qsc_len, f) == s.qsc_len) {
      have_qsc = true;
    }
    std::fclose(f);
  }

  if (have_qsc) {
    for (size_t i = 0; i < s.qsc_len; i++) {
      out.push_back({(uint16_t)(s.qsc_reg_lo + i), qsc[i]});
    }
    if (s.derived_tail && s.derived_tail_n) {
      out.insert(out.end(), s.derived_tail, s.derived_tail + s.derived_tail_n);
    }
    std::fprintf(stderr,
        "[sensor_qsc] loaded %zu-byte QSC from %s -> regs 0x%04x.. (+%zu derived) "
        "= %zu-reg init\n", s.qsc_len, s.eeprom_path, s.qsc_reg_lo,
        s.derived_tail_n, out.size() + s.post_n);
  } else {
    std::fprintf(stderr,
        "[sensor_qsc] WARNING: could not read QSC from %s — streaming without "
        "per-unit calibration (raw OK, cooked degraded)\n", s.eeprom_path);
  }

  out.insert(out.end(), s.post, s.post + s.post_n);
  return out;
}
