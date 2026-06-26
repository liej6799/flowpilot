cfg = open('/tmp/imx689_bps_cfg_multi4096.bin','rb').read()       # 1248 multi-output, OUT_FULL stride 4096
strip = open('/tmp/imx689_bps_striping_21736.bin','rb').read()   # 21736 multi-output striping (captured)
sett = open('/tmp/imx689_bps_settings.bin','rb').read()          # 4096
assert len(cfg)==1248 and len(strip)==21736 and len(sett)==4096, (len(cfg),len(strip),len(sett))
def emit(name, size, slot_idx, data, nslots=6):
    lines = ["unsigned char %s[%d][%d] = {" % (name, nslots, size)]
    for i in range(nslots):
        if i == slot_idx:
            body = ",".join("0x%02x"%x for x in data)
            lines.append("  { /* imx689 (num=5) */ %s }," % body)
        else:
            lines.append("  { 0 },")
    lines.append("};")
    return "\n".join(lines)
out = ["#pragma once",
"// [op9] BPS firmware blobs captured live from OP9 stock HAL (imx689 4000x3000 RAW10 -> BPS -> NV12)",
"// MULTI-OUTPUT cfg (input+OUT_FULL+DS4/DS16/DS64) matching the captured multi-output striping (hdr 14,21,2).",
"// Slot index = ImageSensor enum; only imx689 (slot 5) uses ISP_BPS_PROCESSED.",
"",
emit("bps_cfg", 1248, 5, cfg), "",
emit("bps_striping_output", 21736, 5, strip), "",
emit("bps_settings", 4096, 5, sett), ""]
open('/tmp/bps_blobs_multi.h','w').write("\n".join(out))
print("wrote /tmp/bps_blobs_multi.h (%d bytes)"%len(open('/tmp/bps_blobs_multi.h').read()))
