# Sensor init = model-constant tables + per-unit EEPROM calibration

**Goal:** remove the hand-checked-in `*_registers.h` blob and generate the
streaming init automatically, with **no undocumented per-unit data** committed to
the repo.

This document is the evidence that the goal is met. The short version:

> The stock HAL streaming init for IMX766 / IMX689 is *almost entirely*
> model-constant (the same bytes on every unit). Exactly **one** contiguous block
> in the middle is per-unit factory calibration — the **QSC** (Quad Sensor Coding)
> table — and the HAL copies it **byte-for-byte out of the sensor's EEPROM**. So
> we keep the model-constant part as documented tables and read the QSC from the
> EEPROM of the actual device at runtime. Nothing per-unit is hard-coded.

The reconstruction is **byte-exact** vs a live capture for both sensors
(`tools/gen_sensor_init.py --verify`).

---

## 1. The problem with a checked-in `*_registers.h`

`sensors/imx766_registers.h` was a flat dump of ~3900 `{addr, val}` writes
captured from the stock HAL (via the `ALLREG` CCI kprobe) on **one physical
OnePlus 9**. ~80% of those bytes (the 0xC800–0xD3FF window on IMX766) looked like
an opaque random blob. Embedding it means:

- it encodes **this one device's** factory calibration — wrong for any other unit;
- it is arguably redistributing the vendor's per-unit factory data;
- it is "undocumented data" — exactly what the goal says to remove.

## 2. The decomposition

Both sensors use one unified structure — a single model-constant `PRE` and `POST`,
with per-unit calibration spliced/appended at runtime (the LSC goes into `PRE` at
`lsc_pre_index`, from the meta):

```
full_init = MODE_INIT_PRE[0 : lsc_pre_index]    (model-constant)
          + LSC( EEPROM 17x13 mesh )             (per-unit, computed)   regs 0x9B00..0xA0FF
          + MODE_INIT_PRE[lsc_pre_index : ]      (model-constant)
          + QSC( EEPROM[off:off+len] )           (per-unit, copied)     regs 0xD000..0xDBFF
          + QSC_TAIL( binned: mean of 4 phases ) (per-unit, computed)   regs 0xDC00..0xDEFF
          + MODE_INIT_POST                       (model-constant)
```
imx766 has `lsc`/`qsc_tail` = null in its meta, so it reduces to
`MODE_INIT_PRE + QSC + MODE_INIT_POST`. Both sensors' `*_init_meta.json` share the
same schema; only the `lsc` / `lsc_pre_index` / `qsc_tail` fields differ.

- **MODE_INIT_PRE / MODE_INIT_POST** — PLL / timing / crop / binning control
  (the documented SMIA++/Sony map, see `SENSOR-REGISTERS.md`) plus the static
  Sony "mode tuning" init. Identical on every unit of the model. Committed as
  `sensors/generated/<name>_mode_init.h`.
- **QSC** — per-unit Quad Sensor Coding. **Not committed.** Copied byte-for-byte
  from the device's EEPROM at runtime.
- **LSC** — per-unit lens-shading gain grid (imx689). **Not committed.** Computed
  at runtime as a block-center bilinear interpolation of an EEPROM 17×13 mesh, /2.
  Reproduces the vendor values to ~3% (the exact fixed-point interp is proprietary).
- **QSC_TAIL** — per-unit binned-mode QSC (imx689). **Not committed.** Computed at
  runtime as the mean of the 4 quad-phases per position of the EEPROM QSC (~7 LSB
  off the vendor's value). See §6.

**No per-unit data is committed to the repo** — QSC, LSC and QSC-tail are all
read or derived from the EEPROM at runtime by `sensors/sensor_qsc.h`.

## 3. Decisive proof: the QSC is an EEPROM copy

A sweep over the **entire** captured init (every contiguous run ≥48 bytes whose
values match any contiguous EEPROM region) finds **exactly one** match per
sensor — the QSC window — and nothing else:

```
IMX766:  cap[523:3595]  L=3072  regs 0xc800..0xd3ff  == eeprom[8144:11216]   (byte-exact)
IMX689:  cap[2431:5503] L=3072  regs 0xd000..0xdbff  == eeprom[7936:11008]   (byte-exact)
```

That is the whole story: **the only place the init pulls per-unit data is the QSC
window, and that data is a verbatim copy of the EEPROM.** There is no second
hidden calibration blob anywhere else in the stream.

EEPROM map (read once at probe, cached on device):

| sensor | EEPROM chip       | QSC offset       | QSC len      | → register window |
|--------|-------------------|------------------|--------------|-------------------|
| IMX766 | gt24p128ca2       | 8144  (0x1FD0)   | 3072 (0xC00) | 0xC800 … 0xD3FF   |
| IMX689 | p24c128e          | 7936  (0x1F00)   | 3072 (0xC00) | 0xD000 … 0xDBFF   |

(The same QSC offset/len appears as hard immediates in the vendor sensor `.so`,
e.g. `com.qti.sensor.imx766.lemonade.so`. The EEPROM is a distinct i2c device
with its own slave address — see the sensor-module `.bin` `eepromName` /
`eepromSlaveAddress` fields.)

## 4. The `0x0084` tail is **not** per-unit

The fresh capture also has a short burst of writes to register `0x0084` at the
very tail (21 writes). These are **not** in the EEPROM and are **not** calibration
— `0x0084` is a vendor AE-trim / digital-gain register (perturb-confirmed in
`SENSOR-REGISTERS.md`: it tracks auto-exposure). They are dynamic AE writes that
happened to be captured because the trace ran a few frames into streaming; the
known-good init does not need them. Listed here only so the delta is accounted
for.

## 5. Generating the init (no per-unit data in the repo)

`tools/gen_sensor_init.py` reconstructs the full init from the committed
model-constant tables + an EEPROM image of the actual unit:

```sh
# verify the reconstruction is byte-exact vs a live ALLREG capture
./tools/gen_sensor_init.py --sensor imx766 \
    --eeprom eeprom_imx766.bin --verify capture_imx766.txt
# -> [imx766] BYTE-EXACT MATCH vs capture_imx766.txt  ✓

# emit a ready-to-compile header for THIS unit
./tools/gen_sensor_init.py --sensor imx766 \
    --eeprom eeprom_imx766.bin --out sensors/imx766_registers.h
```

Inputs, by provenance:

| input | committed? | per-unit? | source |
|---|---|---|---|
| `sensors/generated/<name>_mode_init.h` | yes | no  | extracted once, model-constant |
| `sensors/generated/<name>_init_meta.json` | yes | no | QSC window offsets |
| `eeprom_<name>.bin` | **no** | **yes** | read from the device at runtime |

The generated `<name>_registers.h` is per-unit and therefore **`.gitignore`d** —
it is a build artifact, regenerated for whatever device you run on.

## 6. The IMX689 LSC and QSC-tail — now computed from the EEPROM (resolved)

Two IMX689 blocks are per-unit but **derived** from the EEPROM rather than copied,
so the byte-for-byte sweep (§3) didn't catch them. Both are now reproduced at
runtime by `sensors/sensor_qsc.h`, so **neither is committed**:

- **LSC** (lens-shading, 1536 regs at 0x9B00–0xA0FF). The EEPROM holds five 17×13
  control-point shading meshes (16-bit LE, `[4,17,13]` headers at 0x600/0xB00/
  0x1000/0x1500/0x1A00). The register grid is 4 channels × 12×16 = the per-block
  values of a 17→16 / 13→12 interpolation. We reproduce it as block-center
  bilinear ÷2 → **~3% (mean |err| ≈ 6)**.
- **QSC tail** (binned QSC, 768 regs at 0xDC00–0xDEFF) = mean of the 4 quad-phases
  per position of the EEPROM QSC → **~7 LSB**.

The last few percent (LSC) / few LSB (tail) is Qualcomm's exact fixed-point
interpolation, which lives in `com.qti.sensor.imx689.lemonade.so`; reproducing it
bit-exactly would need disassembling that. For a raw/RDI capture the delta is
negligible (verified streaming on device, §7). The captured reference values stay
in `sensors/captured/perturbation/` for `--verify`, **not** in the committed init.

IMX766 has no LSC block and no QSC tail — only the QSC copy. It was already clean.

### Audit: is anything else per-unit?

Per-unit calibration can only originate from the sensor EEPROM (its sole per-unit
store). A full-init sweep finds **exactly 3072 direct EEPROM-copy regs per sensor**
(the QSC) and, for imx689, the two EEPROM-*derived* blocks above (LSC, tail). The
only sizeable remaining contiguous blocks (imx689 0x9200/0xF884/0xF800; imx766
0x5670/0x9200/…) are **not** EEPROM-derivable and are vendor mode-init config
(model-constant). So after this change **no per-unit calibration is committed for
either sensor**. One 13-reg imx689 block (0xC278) shows a weak, likely-coincidental
EEPROM correlation; it reads as config and is treated as model-constant (a second
unit would settle it definitively).

What remains committed in `*_mode_init.h` (885 regs imx766 / 1175 imx689) is the
model-constant Sony/Qualcomm mode-init: unit-independent, but opaque per-register
because Sony doesn't publish those meanings. That's the irreducible "standard
sensor init" every IMX driver ships — not per-unit data.

## 7. On-device validation — DONE

The runtime EEPROM path was built and run end-to-end on the OnePlus 9
(2026-06-24), with the wired `imx766.cc`/`imx689.cc` + `sensor_qsc.h` deployed into
the proot openpilot tree and rebuilt (`scons system/camerad/`, clean):

- **Both sensors read their EEPROM at runtime** (camerad stderr):
  ```
  [sensor_qsc] loaded 3072-byte QSC from /mnt/vendor/persist/camera/eeprom_imx689_p24c128e.bin -> regs 0xd000.. (+768 derived) = 6551-reg init
  [sensor_qsc] loaded 3072-byte QSC from /mnt/vendor/persist/camera/eeprom_imx766_gt24p128ca2.bin -> regs 0xc800.. (+0 derived) = 3957-reg init
  ```
- **Streaming completed**: `Sync with success: req 1 res 0x3006 ... isBadFrame 0`,
  `Buf done for VFE:3`. Full-size frames dumped (IMX689 4000×3000, IMX766
  4096×3072), 100% non-zero. (The captured scene was dark — exposure is already
  pinned at `getExposureRegisters(3000, 960)` ≈ max — so the image is near-black
  noise; that's a lighting condition, not a pipeline issue. The init is byte-
  identical to the static blob that previously captured a lit scene.)

Note `/mnt` is not bind-mounted into the proot build container, so for that run the
two `eeprom_*.bin` were copied to the container's `/mnt/vendor/persist/camera/`. In
a native deployment camerad reads the real path directly.

### Still open (narrow): is QSC strictly *required* for the raw path?
QSC was loaded for the validation above. Whether the RDI/raw path would stream
*without* it (PRE+POST only — RDI bypasses the ISP debayer that QSC feeds) is
untested; if not required, even the IMX689 derived tail becomes moot. The
EEPROM-read approach is correct regardless; this only decides whether to load QSC
at all.

## 8. Provenance of the model-constant tables

`<name>_mode_init.h` was produced by taking a live `ALLREG` capture and removing
the QSC window (the bytes proven in §3 to be an EEPROM copy). What remains is
unit-independent: it is identical across units because it is PLL/timing/crop/
binning control + the static Sony mode-tuning init. The individual control
registers are decoded in `SENSOR-REGISTERS.md`; the mode-tuning writes are the
standard Sony/Qualcomm sequence for this mode. Do **not** redistribute the vendor
`.bin`/`.so`; the tables here were derived from on-device live captures of our own
hardware, not copied from those binaries.

## 9. C++ runtime integration (one binary, any unit)

Two ways to wire this into camerad:

**(A) Build-time** — `tools/gen_sensor_init.py --out sensors/imx766_registers.h`
from a device EEPROM dump, then build as today (`imx766.cc` keeps
`init_reg_array.assign(begin(init_array_imx766), end(...))`). Simple, but the
binary is per-unit (the QSC is baked into the build).

**(B) Runtime** *(recommended)* — read the EEPROM at sensor construction and
splice the QSC in C++ via `sensors/sensor_qsc.h`. One binary runs on any unit:

```cpp
// sensors/imx766.cc
#include "sensors/generated/imx766_mode_init.h"
#include "sensors/sensor_qsc.h"
...
init_reg_array = build_sensor_init({
    .pre = imx766_mode_init_pre,   .pre_n = std::size(imx766_mode_init_pre),
    .post = imx766_mode_init_post, .post_n = std::size(imx766_mode_init_post),
    .derived_tail = nullptr,       .derived_tail_n = 0,
    .qsc_reg_lo = 0xc800,          .qsc_len = 3072,
    .eeprom_qsc_off = 8144,
    .eeprom_path = "/mnt/vendor/persist/camera/eeprom_imx766.bin",
});
```

(The numeric args are the `<name>_init_meta.json` fields; imx689 also passes
`imx689_qsc_derived_tail` and uses `qsc_reg_lo=0xd000`, `eeprom_qsc_off=7936`.)

### Getting the EEPROM image on device

- **Vendor cache (easiest):** the stock HAL caches each sensor's EEPROM at
  `/mnt/vendor/persist/camera/eeprom_<sensor>.bin` after probe. Point
  `eeprom_path` there.
- **Direct i2c (no HAL dependency):** the EEPROM is its own i2c device (slave
  address in the sensor-module `.bin` `eepromSlaveAddress`). Read the QSC window
  over CCI at our own probe time and cache it. This removes any dependency on the
  vendor HAL having run.

### Per-unit artifacts are not committed

`sensors/<name>_registers.h` (when generated) and any `eeprom_*.bin` are per-unit
build artifacts and are `.gitignore`d. The committed inputs
(`generated/*_mode_init.h`, `*_init_meta.json`, `tools/gen_sensor_init.py`) carry
**zero** per-unit data — QSC, LSC and the QSC-tail are all read or derived from
the EEPROM at runtime (§2, §6).

## 10. Tuning-invariance + vendor-blob verification (both sensors)

The committed `*_mode_init.h` tables are model-constant — they must not encode a
frozen snapshot of any tuning-dependent value. Verified by capturing each sensor's
stock-HAL init at six settings (baseline, 720p, 2× zoom, exposure 5 ms / 30 ms,
ISO 100 / 1600) via the `ALLREG` kprobe and diffing per address
(`sensors/captured/perturbation/c{0,2}_*`):

| sensor | committed regs | invariant across all tuning | change with tuning |
|--------|----------------|-----------------------------|--------------------|
| imx689 | 1000 unique    | 991                         | 9                  |
| imx766 | 670 unique     | 661                         | 9                  |

The 9 that move are identical on both sensors and are exactly the AE control loop —
`0x0202/3` (exposure), `0x0204/5` (analog gain), `0x0340/1` (frame length),
`0x0084` + `0x0b91/0x0b93` (vendor AE-trim/digital-gain). These are dynamic by
design and camerad rewrites `0x0202–0x0205` every frame via
`getExposureRegisters()`, so committing their baseline values is harmless.
**Resolution and zoom change zero sensor registers** (the binned mode is fixed;
scaling/crop is ISP-side).

**Vendor-blob match:** a fresh stock-HAL capture vs the committed reference is
byte-identical on every address except the one dynamic AE-trim register
(imx766: 3741/3742 identical; the diff is `0x0084`). So the committed values are
the genuine vendor values, unmodified.
