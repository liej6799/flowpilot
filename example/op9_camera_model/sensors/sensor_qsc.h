#pragma once
//
// Runtime per-unit calibration splice — builds a sensor's streaming init from the
// committed model-constant tables (sensors/generated/<name>_mode_init.h) plus the
// per-unit calibration read/derived from THIS device's EEPROM at runtime. No
// per-unit data is compiled into the binary; one camerad build runs on any unit.
//
// Three kinds of per-unit calibration are handled, all sourced from the EEPROM:
//   * QSC      — Quad Sensor Coding, copied byte-for-byte from EEPROM.
//   * QSC tail — binned-mode QSC = mean of the 4 quad-phases per position
//                (a documented reduction of the EEPROM QSC; imx689 only).
//   * LSC      — lens-shading gain grid, bilinear (block-center) interpolation of
//                an EEPROM 17x13 control-point mesh, /2 (imx689 only).
//
// The QSC tail and LSC reproduce the vendor HAL's values to ~7 LSB / ~3% (the
// exact fixed-point interpolation is proprietary); for a raw/RDI capture this is
// negligible. See docs/SENSOR-CALIBRATION-EEPROM.md.
//
// Recipe (imx689):  pre_a + LSC + pre_b + QSC + QSC_tail + post
// Recipe (imx766):  pre  + QSC + post           (no LSC, no pre_b, no tail)

#include <cstdint>
#include <cstdio>
#include <vector>

#include "system/camerad/sensors/sensor.h"  // i2c_random_wr_payload

// LSC: bilinear interp of an EEPROM 17x13 mesh -> a channels x rows x cols grid.
// channels==0 disables.
struct LscSpec {
  uint16_t reg_lo = 0;
  int channels = 0, rows = 0, cols = 0;
  size_t mesh_off = 0;          // EEPROM byte offset of the mesh block header
  int hdr_bytes = 0;            // header bytes before the mesh data
  int mesh_w = 0, mesh_h = 0;   // control-point mesh dimensions (e.g. 17x13)
  int div = 1;                  // output scale divisor
};

struct SensorInitSpec {
  const i2c_random_wr_payload *pre = nullptr;    size_t pre_n = 0;     // pre_a
  const i2c_random_wr_payload *pre_b = nullptr;  size_t pre_b_n = 0;   // optional
  const i2c_random_wr_payload *post = nullptr;   size_t post_n = 0;
  uint16_t qsc_reg_lo = 0;       // QSC register window base
  size_t   qsc_len = 0;          // QSC bytes copied from EEPROM
  size_t   eeprom_qsc_off = 0;   // QSC offset inside the EEPROM image
  LscSpec  lsc;                  // channels==0 -> no LSC
  bool     qsc_tail = false;     // compute binned QSC tail after QSC
  uint16_t qsc_tail_reg_lo = 0;
  const char *eeprom_path = nullptr;
};

namespace sensor_qsc_detail {
inline uint16_t le16(const std::vector<uint8_t>& e, size_t o) {
  return (uint16_t)(e[o] | (e[o + 1] << 8));
}
// LSC: out[ch][r][c] = round( blockcenter(mesh) / div ), written as BE16 byte pairs.
inline void append_lsc(std::vector<i2c_random_wr_payload>& out,
                       const std::vector<uint8_t>& e, const LscSpec& s) {
  uint16_t addr = s.reg_lo;
  auto M = [&](int r, int c) {
    return (int)le16(e, s.mesh_off + s.hdr_bytes + 2 * (r * s.mesh_w + c));
  };
  for (int ch = 0; ch < s.channels; ch++)
    for (int r = 0; r < s.rows; r++)
      for (int c = 0; c < s.cols; c++) {
        int v = (M(r, c) + M(r, c + 1) + M(r + 1, c) + M(r + 1, c + 1) + (2 * s.div)) /
                (4 * s.div);
        out.push_back({addr++, (uint16_t)((v >> 8) & 0xff)});
        out.push_back({addr++, (uint16_t)(v & 0xff)});
      }
}
// QSC tail: binned-mode QSC = mean of the 4 quad-phases per position.
inline void append_qsc_tail(std::vector<i2c_random_wr_payload>& out,
                            const std::vector<uint8_t>& qsc, uint16_t reg_lo) {
  size_t n = qsc.size() / 4;
  for (size_t i = 0; i < n; i++) {
    int sum = qsc[4 * i] + qsc[4 * i + 1] + qsc[4 * i + 2] + qsc[4 * i + 3];
    out.push_back({(uint16_t)(reg_lo + i), (uint16_t)((sum + 2) / 4)});
  }
}
}  // namespace sensor_qsc_detail

// Build pre_a + [LSC] + pre_b + QSC + [QSC_tail] + post, reading per-unit data
// from the EEPROM. If the EEPROM can't be read, the per-unit segments are omitted
// (raw/RDI still streams; cooked shading/remosaic degraded) and a warning logged.
inline std::vector<i2c_random_wr_payload> build_sensor_init(const SensorInitSpec &s) {
  using namespace sensor_qsc_detail;
  std::vector<i2c_random_wr_payload> out;
  out.reserve(s.pre_n + s.pre_b_n + s.post_n + s.qsc_len + 4096);

  std::vector<uint8_t> eeprom;
  if (FILE *f = std::fopen(s.eeprom_path, "rb")) {
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (sz > 0) { eeprom.resize(sz); if (std::fread(eeprom.data(), 1, sz, f) != (size_t)sz) eeprom.clear(); }
    std::fclose(f);
  }
  bool have = eeprom.size() >= s.eeprom_qsc_off + s.qsc_len;
  if (!have)
    std::fprintf(stderr, "[sensor_qsc] WARNING: could not read EEPROM %s — "
        "streaming without per-unit calibration (raw OK, cooked degraded)\n", s.eeprom_path);

  out.insert(out.end(), s.pre, s.pre + s.pre_n);
  if (have && s.lsc.channels > 0) append_lsc(out, eeprom, s.lsc);
  if (s.pre_b) out.insert(out.end(), s.pre_b, s.pre_b + s.pre_b_n);

  if (have) {
    std::vector<uint8_t> qsc(eeprom.begin() + s.eeprom_qsc_off,
                             eeprom.begin() + s.eeprom_qsc_off + s.qsc_len);
    for (size_t i = 0; i < s.qsc_len; i++)
      out.push_back({(uint16_t)(s.qsc_reg_lo + i), qsc[i]});
    if (s.qsc_tail) append_qsc_tail(out, qsc, s.qsc_tail_reg_lo);
    std::fprintf(stderr, "[sensor_qsc] EEPROM %s: QSC %zu regs @0x%04x%s%s\n",
        s.eeprom_path, s.qsc_len, s.qsc_reg_lo,
        s.lsc.channels ? " + LSC(computed)" : "", s.qsc_tail ? " + QSC_tail(computed)" : "");
  }

  out.insert(out.end(), s.post, s.post + s.post_n);
  return out;
}
