# OnePlus 9 camera sensor register map (IMX689 + IMX766)

Both sensors are Sony IMX (quad-bayer) parts on a shared Qualcomm CCI bus and follow the
**Sony / SMIA++ register convention**. This map was built by capturing the **stock HAL's** live
register writes (the `ALLREG` `cam_cci_data_queue` kprobe in a stock+ALLREG `camera.ko`) while a
Camera2 app streamed each camera, then **perturbing one setting at a time** and diffing the writes.

## Perturbation findings (what each control actually does)

| change made (via Camera2 request) | sensor registers that changed | conclusion |
|---|---|---|
| `SENSOR_EXPOSURE_TIME` 5 ms → 30 ms | `0x0202:0203` 0x01D0→0x0AE0 (464→2784 lines, 6×) | **exposure = coarse integration in lines** |
| `SENSOR_SENSITIVITY` ISO 100 → 1600 | `0x0204:0205` 0x0000→0x03C0 (code 960 = 16×) | **gain = analog-gain code**, gain=1024/(1024−code) |
| ImageReader 4096×3072 → 1280×720 | *none* | **resolution is ISP-side** (sensor always streams full binned mode; IFE/ISP downscales) |
| `CONTROL_ZOOM_RATIO` 1× → 2× | *none* | **zoom is ISP-side** (digital crop+scale in the ISP, not the sensor) |
| (`0x0084`, `0x0B91`, `0x0B93` also track AE — vendor AE-trim/digital-gain registers) | | |

> Aperture is **fixed** on both OnePlus 9 rear cameras (no variable-aperture register); brightness is
> controlled purely by exposure (`0x0202`) + analog gain (`0x0204`) + digital gain (vendor `0x0Bxx`).

## IMX689 — main wide (camera 0, CSIPHY 1)

### Mode: 8000×6000 array → 2×2-binned → 4000×3000 RAW10, C-PHY 3-trio
Full pixel array 8000×6000 (`X/Y_ADDR_END`=7999/5999), 2×2 binning (`0x0901`=0x22) → 4000×3000 output.

_6376 unique registers; control registers decoded below, the rest are vendor tuning + per-unit QSC calibration._

| addr | value | name | meaning |
|---|---|---|---|
| `0x0112:0113` | 0x0a0a | CSI_DATA_FORMAT | input/output bit depth (0x0A0A = RAW10/RAW10) |
| `0x0202:0203` | 6188 | COARSE_INTEGRATION_TIME | exposure in LINES  [perturb-confirmed] |
| `0x0204:0205` | 1008 | ANALOG_GAIN | gain code, gain=1024/(1024-code)  [perturb-confirmed] |
| `0x0306:0307` | 172 | PLL_VT_MPY | VT (pixel) PLL multiplier |
| `0x030e:030f` | 351 | PLL_OP_MPY | OP (MIPI) PLL multiplier |
| `0x0340:0341` | 6252 | FRAME_LENGTH_LINES | total lines/frame incl vblank (sets frame rate) |
| `0x0342:0343` | 11856 | LINE_LENGTH_PCK | total pixel-clocks/line incl hblank |
| `0x0344:0345` | 0 | X_ADDR_START | analog crop left (px, in full-array coords) |
| `0x0346:0347` | 0 | Y_ADDR_START | analog crop top |
| `0x0348:0349` | 7999 | X_ADDR_END | analog crop right |
| `0x034a:034b` | 5999 | Y_ADDR_END | analog crop bottom |
| `0x034c:034d` | 4000 | X_OUTPUT_SIZE | output frame width (px) |
| `0x034e:034f` | 3000 | Y_OUTPUT_SIZE | output frame height (px) |
| `0x0408:0409` | 0 | DIG_CROP_X_OFFSET | digital crop x offset |
| `0x040a:040b` | 0 | DIG_CROP_Y_OFFSET | digital crop y offset |
| `0x040c:040d` | 4000 | DIG_CROP_IMAGE_WIDTH | digital crop width |
| `0x040e:040f` | 3000 | DIG_CROP_IMAGE_HEIGHT | digital crop height |
| `0x0100` | 0x01 | MODE_SELECT | stream: 0x01=streaming, 0x00=standby |
| `0x0104` | 0x00 | GROUPED_PARAMETER_HOLD | 0x01=hold updates / 0x00=release (atomic apply of a group) |
| `0x0106` | 0x01 | CCI_ADDRESS_AUTO_INCR | auto-increment I2C address on burst writes |
| `0x0114` | 0x02 | CSI_LANE_MODE | 0x03=4-lane D-PHY, 0x02=3-trio C-PHY |
| `0x0136` | 0x13 | EXTCLK_MHZ (8.8 fixed) | input MCLK; 0x1333 = 19.2 MHz |
| `0x0216` | 0x00 | SHORT_ANALOG_GAIN | HDR short-exposure analog gain |
| `0x0218` | 0x01 | SHORT_INTEGRATION_TIME | HDR short exposure (lines) |
| `0x0301` | 0x08 | VTPXCK_DIV | video-timing pixel-clock divider |
| `0x0303` | 0x02 | VTSYCK_DIV | video-timing system-clock divider |
| `0x0305` | 0x03 | PREPLLCK_VT_DIV | pre-PLL divider (VT path) |
| `0x030b` | 0x04 | OPSYCK_DIV | output system-clock divider |
| `0x030d` | 0x02 | PREPLLCK_OP_DIV | pre-PLL divider (OP/MIPI path) |
| `0x0310` | 0x01 | PLL_MODE | dual/single PLL mode |
| `0x0900` | 0x01 | BINNING_MODE | 0x01=binning enabled |
| `0x0901` | 0x22 | BINNING_TYPE | 0x22=2x2, 0x44=4x4, 0x11=none |
| `0x0902` | 0x08 | BINNING_WEIGHTING | binning weight/averaging mode |

**Calibration/tuning blocks (6326 regs).** The 256-entry pages are per-unit LUTs (QSC quad-bayer remosaic + lens-shading + linearisation), calibrated per sensor at the factory — data, not control. Large blocks: `0x9b00`×256, `0x9c00`×256, `0x9d00`×256, `0x9e00`×256, `0x9f00`×256, `0xa000`×256, `0xc000`×71, `0xc100`×71, `0xc200`×79, `0xd000`×256, `0xd100`×256, `0xd200`×256, `0xd300`×256, `0xd400`×256, `0xd500`×256, `0xd600`×256, `0xd700`×256, `0xd800`×256, `0xd900`×256, `0xda00`×256, `0xdb00`×256, `0xdc00`×256, `0xdd00`×256, `0xde00`×256, `0xf800`×144.

## IMX766 — ultra-wide (camera 2, CSIPHY 2)

### Mode: 8192×6144 array → 2×2-binned → 4096×3072 RAW10, C-PHY 3-trio
Full pixel array 8192×6144 (`X/Y_ADDR_END`=8191/6143), 2×2 binning → 4096×3072 output. **This is the mode the camerad port must match** — using 4000×3000 here caused the CSID 'bad frame timings' CCIF.

_3742 unique registers; control registers decoded below, the rest are vendor tuning + per-unit QSC calibration._

| addr | value | name | meaning |
|---|---|---|---|
| `0x0112:0113` | 0x0a0a | CSI_DATA_FORMAT | input/output bit depth (0x0A0A = RAW10/RAW10) |
| `0x0202:0203` | 6622 | COARSE_INTEGRATION_TIME | exposure in LINES  [perturb-confirmed] |
| `0x0204:0205` | 16128 | ANALOG_GAIN | gain code, gain=1024/(1024-code)  [perturb-confirmed] |
| `0x0306:0307` | 303 | PLL_VT_MPY | VT (pixel) PLL multiplier |
| `0x030e:030f` | 224 | PLL_OP_MPY | OP (MIPI) PLL multiplier |
| `0x0340:0341` | 6670 | FRAME_LENGTH_LINES | total lines/frame incl vblank (sets frame rate) |
| `0x0342:0343` | 15616 | LINE_LENGTH_PCK | total pixel-clocks/line incl hblank |
| `0x0344:0345` | 0 | X_ADDR_START | analog crop left (px, in full-array coords) |
| `0x0346:0347` | 0 | Y_ADDR_START | analog crop top |
| `0x0348:0349` | 8191 | X_ADDR_END | analog crop right |
| `0x034a:034b` | 6143 | Y_ADDR_END | analog crop bottom |
| `0x034c:034d` | 4096 | X_OUTPUT_SIZE | output frame width (px) |
| `0x034e:034f` | 3072 | Y_OUTPUT_SIZE | output frame height (px) |
| `0x0408:0409` | 0 | DIG_CROP_X_OFFSET | digital crop x offset |
| `0x040a:040b` | 0 | DIG_CROP_Y_OFFSET | digital crop y offset |
| `0x040c:040d` | 4096 | DIG_CROP_IMAGE_WIDTH | digital crop width |
| `0x040e:040f` | 3072 | DIG_CROP_IMAGE_HEIGHT | digital crop height |
| `0x0100` | 0x01 | MODE_SELECT | stream: 0x01=streaming, 0x00=standby |
| `0x0104` | 0x00 | GROUPED_PARAMETER_HOLD | 0x01=hold updates / 0x00=release (atomic apply of a group) |
| `0x0106` | 0x01 | CCI_ADDRESS_AUTO_INCR | auto-increment I2C address on burst writes |
| `0x0114` | 0x02 | CSI_LANE_MODE | 0x03=4-lane D-PHY, 0x02=3-trio C-PHY |
| `0x0136` | 0x13 | EXTCLK_MHZ (8.8 fixed) | input MCLK; 0x1333 = 19.2 MHz |
| `0x0216` | 0x00 | SHORT_ANALOG_GAIN | HDR short-exposure analog gain |
| `0x0218` | 0x01 | SHORT_INTEGRATION_TIME | HDR short exposure (lines) |
| `0x0301` | 0x05 | VTPXCK_DIV | video-timing pixel-clock divider |
| `0x0303` | 0x04 | VTSYCK_DIV | video-timing system-clock divider |
| `0x0305` | 0x03 | PREPLLCK_VT_DIV | pre-PLL divider (VT path) |
| `0x030b` | 0x02 | OPSYCK_DIV | output system-clock divider |
| `0x030d` | 0x02 | PREPLLCK_OP_DIV | pre-PLL divider (OP/MIPI path) |
| `0x0900` | 0x01 | BINNING_MODE | 0x01=binning enabled |
| `0x0901` | 0x22 | BINNING_TYPE | 0x22=2x2, 0x44=4x4, 0x11=none |
| `0x0902` | 0x08 | BINNING_WEIGHTING | binning weight/averaging mode |

**Calibration/tuning blocks (3693 regs).** The 256-entry pages are per-unit LUTs (QSC quad-bayer remosaic + lens-shading + linearisation), calibrated per sensor at the factory — data, not control. Large blocks: `0x5600`×64, `0x5e00`×110, `0xc800`×256, `0xc900`×256, `0xca00`×256, `0xcb00`×256, `0xcc00`×256, `0xcd00`×256, `0xce00`×256, `0xcf00`×256, `0xd000`×256, `0xd100`×256, `0xd200`×256, `0xd300`×256.

## Reproducing / extending this map

1. Build a **stock + ALLREG-only** `camera.ko` (revert all camerad-port kernel patches, keep only the
   `cam_cci_data_queue` `CAM_INFO ALLREG` log) so the vendor HAL runs *and* every CCI write is logged.
2. Boot it, start `vendor.camera-provider-2-4`, open the target camera (0=IMX689, 2=IMX766) with the
   `com.op9.camcap` app: `am start -n com.op9.camcap/.MainActivity --es logical <id> --ei w W --ei h H`
   `[--es aeoff 1 --el exp <ns> --ei iso <iso>] [--ef zoom <z>]`.
3. `dmesg | grep ALLREG` → `0x<addr> 0x<data>` pairs. Diff two captures that differ by one setting to
   attribute registers. Full raw captures: `regs/c0_*.txt`, `regs/c2_*.txt`.
