#!/usr/bin/env python3
"""
gen_sensor_init.py -- build a sensor streaming-init register array from
*documented* inputs only, with NO per-unit calibration baked into the repo.

Background
----------
The stock Qualcomm HAL streaming init for these Sony sensors is a long list of
i2c register writes.  Almost all of it is model-constant (identical for every
unit of a given sensor model) -- PLL/timing/crop/binning control plus a static
"mode tuning" block.  A single contiguous chunk in the middle, however, is the
QSC (Quad Sensor Coding) calibration: a per-unit factory measurement that the
HAL copies *verbatim* out of the sensor's on-board EEPROM into a register
window.  Embedding that chunk in a header file would hard-code ONE physical
device's calibration into the repo (wrong for any other unit, and arguably
redistributing factory data).

This tool reconstructs the full init at build/runtime from:

  1. sensors/generated/<name>_mode_init.h   -- the model-constant tables
     (mode_init_pre[], mode_init_post[], and for imx689 a small HAL-derived
     qsc tail).  These are documented, unit-independent, and committed.
  2. sensors/generated/<name>_init_meta.json -- where the QSC window lives in
     the EEPROM and in the register map.
  3. an EEPROM image of THE ACTUAL UNIT being used (read at runtime over i2c, or
     from the on-device cache /mnt/vendor/persist/camera/eeprom_<name>.bin).
     This is NOT committed -- it is per-unit data that belongs to the device.

  full_init = mode_init_pre
            + QSC_block( EEPROM[qsc_off : qsc_off+qsc_len] )
            + qsc_derived_tail            (imx689 only; HAL-computed)
            + mode_init_post

The result is byte-for-byte identical to what the stock HAL streams (proven by
--verify against a live ALLREG capture), but the repo carries zero per-unit data.

Usage
-----
  # emit sensors/imx766_registers.h from a runtime EEPROM image
  ./gen_sensor_init.py --sensor imx766 --eeprom eeprom_imx766.bin \
      --out ../sensors/imx766_registers.h

  # prove the reconstruction is byte-exact vs a captured reference
  ./gen_sensor_init.py --sensor imx766 --eeprom eeprom_imx766.bin \
      --verify capture_imx766.txt

The capture file (verify only) is one "0xADDR 0xVAL" per line, in HAL write
order, as produced by the ALLREG CCI kprobe.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN_DIR = os.path.normpath(os.path.join(HERE, "..", "sensors", "generated"))


def parse_mode_init_h(path):
    """Return {array_name: [(addr, val), ...]} for every i2c_random_wr_payload
    array declared in a generated mode-init header."""
    with open(path) as f:
        text = f.read()
    arrays = {}
    # match:  const struct i2c_random_wr_payload NAME[] = { ... };
    for m in re.finditer(
        r"i2c_random_wr_payload\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\};", text, re.S
    ):
        name, body = m.group(1), m.group(2)
        pairs = re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}", body)
        arrays[name] = [(int(a, 16), int(v, 16)) for a, v in pairs]
    return arrays


def load_capture(path):
    """Load an ALLREG capture (HAL write order): list of (addr, val)."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            a, v = line.split()
            out.append((int(a, 16), int(v, 16) & 0xFF))
    return out


def build_init(sensor, eeprom_path):
    """Reconstruct the full (addr, val) init list for `sensor` using its
    committed model-constant tables + the supplied per-unit EEPROM image."""
    meta_path = os.path.join(GEN_DIR, f"{sensor}_init_meta.json")
    hdr_path = os.path.join(GEN_DIR, f"{sensor}_mode_init.h")
    with open(meta_path) as f:
        meta = json.load(f)
    arrays = parse_mode_init_h(hdr_path)

    pre = arrays[f"{sensor}_mode_init_pre"]
    post = arrays[f"{sensor}_mode_init_post"]
    derived_tail = arrays.get(f"{sensor}_qsc_derived_tail", [])

    qsc_off = meta["eeprom_qsc_offset"]
    qsc_len = meta["eeprom_qsc_len"]
    qsc_reg_lo = meta["qsc_reg_lo"]
    qsc_reg_count = meta["qsc_reg_count"]
    derived_tail_count = meta["derived_tail_count"]

    # sanity: tables match the metadata they were generated with
    assert len(pre) == meta["pre_count"], (
        f"{sensor}: pre table has {len(pre)} regs, meta says {meta['pre_count']}"
    )
    assert len(post) == meta["post_count"], (
        f"{sensor}: post table has {len(post)} regs, meta says {meta['post_count']}"
    )
    assert len(derived_tail) == derived_tail_count, (
        f"{sensor}: derived tail has {len(derived_tail)} regs, "
        f"meta says {derived_tail_count}"
    )

    with open(eeprom_path, "rb") as f:
        eeprom = f.read()
    if len(eeprom) < qsc_off + qsc_len:
        raise SystemExit(
            f"EEPROM image too small: need {qsc_off + qsc_len} bytes, "
            f"got {len(eeprom)} ({eeprom_path})"
        )
    qsc_bytes = eeprom[qsc_off : qsc_off + qsc_len]

    # QSC register window: a contiguous auto-increment burst starting at
    # qsc_reg_lo.  First qsc_len regs come straight from the EEPROM; any
    # remaining regs are the HAL-derived tail (imx689 interpolation).
    qsc_block = []
    for i in range(qsc_len):
        qsc_block.append((qsc_reg_lo + i, qsc_bytes[i]))
    for j, (addr, val) in enumerate(derived_tail):
        expect_addr = qsc_reg_lo + qsc_len + j
        assert addr == expect_addr, (
            f"{sensor}: derived tail addr 0x{addr:x} != expected 0x{expect_addr:x}"
        )
        qsc_block.append((addr, val))
    assert len(qsc_block) == qsc_reg_count, (
        f"{sensor}: built {len(qsc_block)} QSC regs, meta says {qsc_reg_count}"
    )

    return pre + qsc_block + post, meta


def emit_header(sensor, init, meta, out_path, eeprom_path):
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"// {sensor.upper()} streaming init -- GENERATED by tools/gen_sensor_init.py")
    lines.append("// DO NOT EDIT BY HAND.  This file mixes model-constant tables")
    lines.append(f"// (sensors/generated/{sensor}_mode_init.h) with the per-unit QSC")
    lines.append(f"// calibration read from this device's EEPROM.  Regenerate per unit.")
    lines.append("//")
    lines.append(f"//   mode:        {meta['mode']}")
    lines.append(f"//   eeprom:      {os.path.basename(eeprom_path)}")
    lines.append(f"//   qsc window:  eeprom[{meta['eeprom_qsc_offset']}:"
                 f"{meta['eeprom_qsc_offset'] + meta['eeprom_qsc_len']}] "
                 f"-> regs 0x{meta['qsc_reg_lo']:04x}..0x"
                 f"{meta['qsc_reg_lo'] + meta['qsc_reg_count'] - 1:04x}")
    lines.append(f"//   total regs:  {len(init)} "
                 f"({meta['pre_count']} pre + {meta['qsc_reg_count']} qsc + "
                 f"{meta['post_count']} post)")
    lines.append("")
    lines.append(f"const struct i2c_random_wr_payload start_reg_array_{sensor}[] = "
                 "{{0x0100, 0x01}};")
    lines.append(f"const struct i2c_random_wr_payload stop_reg_array_{sensor}[]  = "
                 "{{0x0100, 0x00}};")
    lines.append("")
    lines.append(f"const struct i2c_random_wr_payload init_array_{sensor}[] = {{")
    for addr, val in init:
        lines.append(f"  {{0x{addr:04x}, 0x{val:02x}}},")
    lines.append("};")
    lines.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {out_path}  ({len(init)} regs)")


def verify(sensor, init, meta, capture_path):
    cap = load_capture(capture_path)
    # The capture is the full HAL write stream up to stream-on.  Our reconstruction
    # should reproduce it exactly, in order.
    ok = init == cap
    print(f"[{sensor}] reconstructed {len(init)} regs, capture has {len(cap)} regs")
    if ok:
        print(f"[{sensor}] BYTE-EXACT MATCH vs {os.path.basename(capture_path)}  ✓")
        return True
    # diagnose
    n = min(len(init), len(cap))
    first_diff = next((i for i in range(n) if init[i] != cap[i]), None)
    if len(init) != len(cap):
        print(f"[{sensor}] LENGTH MISMATCH: gen {len(init)} vs cap {len(cap)}")
    if first_diff is not None:
        g, c = init[first_diff], cap[first_diff]
        print(f"[{sensor}] first diff at idx {first_diff}: "
              f"gen (0x{g[0]:04x},0x{g[1]:02x}) vs cap (0x{c[0]:04x},0x{c[1]:02x})")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sensor", required=True, choices=["imx766", "imx689"])
    ap.add_argument("--eeprom", required=True,
                    help="per-unit EEPROM image (runtime i2c read or on-device cache)")
    ap.add_argument("--out", help="write generated <sensor>_registers.h here")
    ap.add_argument("--verify", metavar="CAPTURE",
                    help="byte-compare reconstruction against an ALLREG capture")
    args = ap.parse_args()

    init, meta = build_init(args.sensor, args.eeprom)

    if args.out:
        emit_header(args.sensor, init, meta, args.out, args.eeprom)

    if args.verify:
        if not verify(args.sensor, init, meta, args.verify):
            sys.exit(1)

    if not args.out and not args.verify:
        # default: just report the breakdown
        print(f"[{args.sensor}] {len(init)} regs = "
              f"{meta['pre_count']} pre + {meta['qsc_reg_count']} qsc + "
              f"{meta['post_count']} post "
              f"(qsc = {meta['eeprom_qsc_len']} eeprom + "
              f"{meta['derived_tail_count']} derived)")


if __name__ == "__main__":
    main()
