import re
from collections import OrderedDict
lines=open('/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/nv12/nv12_iow.trace').read().splitlines()
rx=re.compile(r'data=0x([0-9a-f]+) addr=0x([0-9a-f]+)')
IFE=0xffffffc02a9d0000
# last-written value per offset within the IFE block (config = final state)
last=OrderedDict(); count={}
for ln in lines:
    m=rx.search(ln)
    if not m: continue
    data=int(m.group(1),16); addr=int(m.group(2),16)
    if addr & ~0xFFFF != IFE: continue
    off=addr & 0xFFFF
    last[off]=data; count[off]=count.get(off,0)+1

# WM clients: base 0xAC00 + n*0x100 (vfe480 bus_ver3)
WMREG={0x00:'cfg',0x04:'img_addr',0x08:'frame_incr',0x0c:'img_cfg0(h/w)',0x10:'img_cfg1',
       0x14:'img_cfg2(stride)',0x18:'packer_cfg',0x1c:'?',0x20:'bw_limit',0x60:'fd_period',
       0x64:'fd_pattern',0x68:'irq_subsmpl',0x6c:'mode?',0x70:'burst'}
print("=== ENABLED WM clients (cfg bit0=1) and their format config ===")
for n in range(0,30):
    b=0xAC00+n*0x100
    cfg=last.get(b)
    if cfg is None: continue
    en = cfg & 1
    tag = "ENABLED" if en else "off"
    if not en and (b not in last): continue
    fields=[]
    for r in sorted(WMREG):
        if b+r in last:
            fields.append(f"{WMREG[r]}=0x{last[b+r]:x}")
    print(f"  WM{n:2d} @0x{b:04x} cfg=0x{cfg:08x} [{tag}]  " + " ".join(fields))

print("\n=== IFE top/core/CAMIF config (final values, key offsets) ===")
KEY={0x0000:'hw_version',0x001c:'?',0x0020:'core_cfg?',0x002c:'core_cfg_0',0x0030:'core_cfg_1',
     0x0034:'reg_update_cmd',0x0038:'?',0x2660:'camif_cfg',0x2664:'camif_cfg1',0x2668:'camif_skip',
     0x2680:'camif_epoch',0x2684:'camif_w',0x2688:'camif_h'}
for off in sorted(last):
    if off<0x3000:
        nm=KEY.get(off,'')
        print(f"  0x{off:04x} = 0x{last[off]:08x}  (writes={count[off]}) {nm}")
