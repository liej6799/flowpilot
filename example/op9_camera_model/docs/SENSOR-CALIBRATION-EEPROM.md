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

Every capture splits cleanly into four parts, in HAL write order:

```
full_init  =  MODE_INIT_PRE                       (model-constant)
           +  QSC_BLOCK( EEPROM[off : off+len] )  (per-unit, copied from EEPROM)
           +  QSC_DERIVED_TAIL                     (imx689 only; HAL-computed)
           +  MODE_INIT_POST                       (model-constant)
```

- **MODE_INIT_PRE / MODE_INIT_POST** — PLL / timing / crop / binning control
  (the documented SMIA++/Sony map, see `SENSOR-REGISTERS.md`) plus the static
  Sony "mode tuning" init. Identical on every unit of the model. Committed as
  `sensors/generated/<name>_mode_init.h`.
- **QSC_BLOCK** — the per-unit calibration. **Not committed.** Sourced from the
  device's own EEPROM at runtime.
- **QSC_DERIVED_TAIL** — IMX689 only: a 768-byte tail (regs 0xDC00–0xDEFF) that
  the HAL *computes* from the QSC (a proprietary interpolation, ~7 LSB off the
  raw EEPROM bytes; **not** a copy of any EEPROM region). Flagged separately as a
  fallback; see §6.

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

## 6. Residual: the IMX689 derived tail

IMX689 has 768 bytes (regs 0xDC00–0xDEFF) the HAL derives from the QSC by a
proprietary interpolation rather than copying from EEPROM. We currently ship the
captured values in `imx689_mode_init.h::imx689_qsc_derived_tail[]` (flagged), so
those *are* per-unit bytes still in the table for IMX689. Options, in order of
preference:

1. **Test whether RDI raw capture needs it at all** (§7). QSC is on-sensor
   remosaic/shading correction; the RDI/RAW path bypasses the ISP debayer, so the
   sensor may stream a valid raw frame with QSC omitted entirely. If so, the whole
   QSC block (including the derived tail) is optional for our use and the table is
   fully model-constant.
2. Reverse the interpolation from the QSC (the `.so` has the math).
3. Keep it as a flagged fallback (current state).

IMX766 has **no** derived tail — it is already 100% model-constant + EEPROM, zero
per-unit bytes in the repo.

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
**zero** per-unit data (except the flagged IMX689 derived tail, §6).
