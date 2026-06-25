import re, sys
L = open('/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/nv12/hal_cdm.log').read().splitlines()
# isolate ONE request dump (req 2) to get the per-frame IFE config
start=end=None
for i,ln in enumerate(L):
    if 'op9dbg] DUMP CFG req 2 ' in ln and start is None: start=i
    elif start is not None and 'op9dbg] DUMP CFG req 3' in ln: end=i; break
seg = L[start:end] if end else L[start:]
print(f"req2 segment: {len(seg)} lines")
base=None; regs={}  # (base,off)->val ; pending REG_CONT
cont_off=cont_n=cont_i=None
rx_base=re.compile(r'CHANGE_BASE: 0x([0-9A-Fa-f]+)')
rx_cont=re.compile(r'REG_CONT: COUNT: (\d+) OFFSET: 0x([0-9A-Fa-f]+)')
rx_data=re.compile(r'DATA_(\d+): 0x([0-9A-Fa-f]+)')
rx_rand=re.compile(r'OFFSET_(\d+): 0x([0-9A-Fa-f]+) DATA_\d+: 0x([0-9A-Fa-f]+)')
for ln in seg:
    m=rx_base.search(ln)
    if m: base=int(m.group(1),16); continue
    m=rx_cont.search(ln)
    if m:
        cont_n=int(m.group(1)); cont_off=int(m.group(2),16); cont_i=0; continue
    m=rx_rand.search(ln)
    if m:
        regs[(base,int(m.group(2),16))]=int(m.group(3),16); cont_off=None; continue
    m=rx_data.search(ln)
    if m and cont_off is not None:
        idx=int(m.group(1)); regs[(base,cont_off+4*idx)]=int(m.group(2),16)
# group by base
from collections import defaultdict
byb=defaultdict(dict)
for (b,o),v in regs.items(): byb[b][o]=v
print(f"total distinct regs: {len(regs)}  bases: {[hex(b) for b in sorted(byb)]}")
for b in sorted(byb):
    offs=sorted(byb[b])
    print(f"\n=== BASE 0x{b:x}: {len(offs)} regs, range 0x{offs[0]:x}..0x{offs[-1]:x} ===")
# Save the main IFE base regs (the one with CAMIF 0x2660)
ife_base=None
for b in byb:
    if 0x2660 in byb[b]: ife_base=b; break
if ife_base is not None:
    o=byb[ife_base]
    print(f"\n=== IFE block 0x{ife_base:x} key regs ===")
    for k in [0x2c,0x30,0x2660,0x2668,0x2670,0x2678,0x3090,0x3260,0x560,0x40,0x44,0x48,0x4c,0xe0c,0xe2c,0x6fc,0x6b0]:
        if k in o: print(f"  0x{k:04x} = 0x{o[k]:08x}")
    # dump pixel-pipe module range 0x0-0x1000 + 0x3000-0x3400
    print("  --- modules 0x0-0x1000 ---")
    for k in sorted(o):
        if k < 0x1000: print(f"    0x{k:04x}=0x{o[k]:08x}", end='')
    print()
import json
json.dump({hex(b):{hex(o):hex(v) for o,v in d.items()} for b,d in byb.items()},
          open('/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/nv12/hal_regs.json','w'))
print("\nsaved hal_regs.json")
