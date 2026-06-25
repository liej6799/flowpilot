import json
H = json.load(open('/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/nv12/hal_regs.json'))
ife = {int(k,16): int(v,16) for k,v in H['0xc3000'].items()}  # IFE1 (master)
# the pixel-pipe + demosaic/IQ modules camerad is missing (skip CAMIF/WM/stats/top)
pipe = {o:v for o,v in ife.items() if 0x2700 <= o < 0x9000}
offs = sorted(pipe)
print(f"// [op9] HAL pixel-pipe + demosaic/IQ config: {len(offs)} regs, 0x{offs[0]:x}..0x{offs[-1]:x}")
print("// captured from the stock HAL's IFE1 per-frame CDM (op9_dumpreq), NV12 1280x720 stream")
# emit as write_random pairs for camerad build_initial_config
lines=[]
for o in offs:
    lines.append(f"0x{o:x}, 0x{pipe[o]:08x},")
# chunk into write_random calls (kernel accepts up to ~N pairs per cont)
print("dst += write_random(dst, {")
for i in range(0,len(lines),4):
    print("  " + " ".join(lines[i:i+4]))
print("});")
# also save raw
open('/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/nv12/pipe_cfg.txt','w').write(
    "\n".join(f"0x{o:05x} 0x{pipe[o]:08x}" for o in offs))
# module-group summary
from collections import defaultdict
grp=defaultdict(int)
for o in offs: grp[o & 0xFF00]+=1
print(f"\n// module groups (base -> count): " + ", ".join(f"0x{g:x}:{c}" for g,c in sorted(grp.items())))
