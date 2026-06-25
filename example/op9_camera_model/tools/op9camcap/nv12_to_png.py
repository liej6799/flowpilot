import sys, zlib, struct
path=sys.argv[1]; w=int(sys.argv[2]); h=int(sys.argv[3]); out=sys.argv[4]
d=open(path,'rb').read()
Y=d[0:w*h]; UV=d[w*h:w*h+w*h//2]
def clamp(v): return 0 if v<0 else (255 if v>255 else v)
rows=[]
ymin=min(Y); ymax=max(Y)
for y in range(h):
    row=bytearray()
    for x in range(w):
        yv=Y[y*w+x]
        ci=((y//2)*(w//2)+(x//2))*2
        u=UV[ci]-128; v=UV[ci+1]-128
        # BT.601 video-range-ish
        r=clamp(int(yv + 1.402*v))
        g=clamp(int(yv - 0.344*u - 0.714*v))
        b=clamp(int(yv + 1.772*u))
        row += bytes((r,g,b))
    rows.append(b'\x00'+bytes(row))
raw=b''.join(rows)
def chunk(t,data):
    return struct.pack('>I',len(data))+t+data+struct.pack('>I',zlib.crc32(t+data)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'
png+=chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
png+=chunk(b'IDAT',zlib.compress(raw,6))
png+=chunk(b'IEND',b'')
open(out,'wb').write(png)
# stats
nz=sum(1 for c in Y if c!=0)
print(f"Y range {ymin}..{ymax}, nonzero {100*nz//len(Y)}%, mean {sum(Y)//len(Y)}")
print(f"wrote {out}")
