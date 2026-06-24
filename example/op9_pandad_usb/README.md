# op9_pandad_usb

Re-adds **USB panda support** to openpilot `pandad` (v0.11.2), which upstream dropped when the
comma three moved to an SPI-only panda. This lets pandad talk to a classic **USB panda**
(`bbaa:ddcc`) — e.g. a black/grey/white panda or a comma body — over USB‑C in host mode.

## Status / honesty

✅ **Works.** Verified end-to-end on a OnePlus 9 (LeMonade, SM8350) running openpilot 0.11.2 in an
Ubuntu 24.04 proot, with a **black panda** on USB‑C (host mode):

```
pandad.cc: connecting to panda: 30001d000351323437393931
panda.cc:  connected to 30001d000351323437393931 over USB
```

Live `pandaState` confirmed flowing over msgq: `pandaType=blackPanda voltage=3641mV safetyModel=silent
faultStatus=none harnessStatus=notConnected` — all sane for a bench panda.

## Why upstream dropped it

In 0.11.2, `selfdrive/pandad/panda.cc` is SPI-only:

```cpp
std::vector<std::string> Panda::list() { return PandaSpiHandle::list(); }
```

There is **no `PandaUsbHandle`** anywhere — the comma three wires its panda over SPI. A USB panda
enumerates fine in libusb (`lsusb` shows `bbaa:ddcc comma.ai panda`) but pandad never looks for it,
so it prints `no pandas found, exiting`.

## What this does

Reintroduces the abstract `PandaCommsHandle` base class (which upstream collapsed) and adds a
libusb-based `PandaUsbHandle` alongside the existing `PandaSpiHandle`:

- `panda_comms.h` — abstract `PandaCommsHandle` base (hw_serial / connected / comms_healthy + the 4
  transfer virtuals + cleanup); `PandaSpiHandle` and new `PandaUsbHandle` both inherit it.
- `usb.cc` — `PandaUsbHandle`: classic libusb impl (open/list `bbaa:ddcc`, control + bulk transfers).
- `panda.cc` — `Panda()` tries USB then falls back to SPI; `Panda::list()` returns USB **+** SPI serials.
- `panda.h` — `handle` widened from `unique_ptr<PandaSpiHandle>` to `unique_ptr<PandaCommsHandle>`.
- `SConscript` — compiles `usb.cc`, links `usb-1.0`.

This is the **portable** part — it should apply almost verbatim to mainline openpilot.

## Porting to mainline openpilot

```bash
git apply example/op9_pandad_usb/patches/pandad-usb-v0.11.2.patch
# build dep: libusb-1.0-0-dev   (scons links -lusb-1.0)
scons -j$(nproc) selfdrive/pandad/pandad
```

`PandaUsbHandle` is lifted from openpilot's own pre-SPI history, so this is really a "revert the SPI-only
collapse" — the cleanest upstream form would be a `boardd`-style abstract base kept permanently.

## Running on the OnePlus 9 (notes specific to this device)

- USB‑C must be in **host mode** (`/sys/class/typec/port0/data_role` = `[host]`). The panda enumerates at
  `/dev/bus/usb/<bus>/<dev>`.
- The node is `root:usb 660`; the proot's real uid isn't in the `usb` group, so libusb `open()` needs
  `chmod 666` on the node (a udev rule / group membership makes it permanent).
- The panda firmware won't match this build's signature → run with `BOARDD_SKIP_FW_CHECK=1`
  (the firmware-version assert; `panda/board/obj/panda.bin.signed` isn't even built here).

## hardware-tici-default-op9.patch (OP9-only, NOT for mainline)

`pandad` (and other daemons) call `HardwareTici::get_device_type()`, which `assert`s the model is in
`{tici, tizi, mici}`. The OnePlus 9 model string isn't, so it aborts. This companion patch defaults
unrecognized hardware to `TICI` instead of asserting. It only matters when running openpilot on
non-comma hardware — do not send it upstream.

## Layout

```
patches/pandad-usb-v0.11.2.patch        portable USB-panda support (5 files, +251)
patches/hardware-tici-default-op9.patch OP9-only device-type fallback
src/usb.cc                              PandaUsbHandle implementation (reference copy)
src/panda_comms.h                       abstract base + Spi/Usb handles (reference copy)
```
