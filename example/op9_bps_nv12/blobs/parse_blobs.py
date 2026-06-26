import re, struct, sys
DUMP="/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/bps_blobs/bps_reboot_dump.txt"
OUT="/tmp/claude-0/-root/e65abdf1-0b1a-43e1-af5f-00a48bc0ce6f/scratchpad/bps_blobs/"
lines=open(DUMP,encoding='utf-8',errors='replace').read().splitlines()
hdr_re=re.compile(r'BLOB SUBP hdl=([0-9a-f]+) iova=([0-9a-f]+) bytes=(\d+)')
subp_re=re.compile(r'\[op9bps\] SUBP\s+(\d+):\s+([0-9a-f ]+)$')
# collect first complete instance of each handle
blobs={}  # hdl -> {size, words{idx:val}}
cur=None
for ln in lines:
    m=hdr_re.search(ln)
    if m:
        hdl=m.group(1); size=int(m.group(3))
        # start a fresh capture for this hdl only if not yet complete
        if hdl not in blobs or len(blobs[hdl]['words'])*4 < blobs[hdl]['size']:
            blobs.setdefault(hdl,{'size':size,'words':{}})
            cur=hdl
        else:
            cur=None
        continue
    if cur:
        s=subp_re.search(ln)
        if s:
            base=int(s.group(1))
            words=s.group(2).split()
            for i,w in enumerate(words):
                try: blobs[cur]['words'][base+i]=int(w,16)
                except: pass
        elif '[op9bps]' in ln and 'PATCH' in ln:
            cur=None  # blob dump ended
for hdl,b in blobs.items():
    size=b['size']; nwords=size//4
    buf=bytearray(size)
    have=0
    for idx,val in b['words'].items():
        if idx<nwords:
            struct.pack_into('<I',buf,idx*4,val&0xffffffff); have+=1
    fn=OUT+f"blob_{hdl}_{size}.bin"
    open(fn,'wb').write(buf)
    print(f"{hdl}: size={size} words_captured={have}/{nwords} -> {fn}")
