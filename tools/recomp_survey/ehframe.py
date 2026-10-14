# Function list from the eboot's .eh_frame_hdr (PT_GNU_EH_FRAME): one "start length" line per
# FDE, sorted. usage: ehframe.py image.bin funcs.txt [eh_frame_hdr image offset, hex]
import struct,sys
img=open(sys.argv[1],'rb').read()
HDR=int(sys.argv[3],16) if len(sys.argv)>3 else 0x4f41510  # Bloodborne 1.09
def enc_read(enc,pos,base_pc):
    fmt=enc&0x0f; app=enc&0x70
    if fmt==0x0b: v=struct.unpack_from('<i',img,pos)[0]; n=4
    elif fmt==0x03: v=struct.unpack_from('<I',img,pos)[0]; n=4
    elif fmt==0x04: v=struct.unpack_from('<Q',img,pos)[0]; n=8
    elif fmt==0x0c: v=struct.unpack_from('<q',img,pos)[0]; n=8
    elif fmt==0x02: v=struct.unpack_from('<H',img,pos)[0]; n=2
    elif fmt==0x0a: v=struct.unpack_from('<h',img,pos)[0]; n=2
    else: raise Exception('enc %x'%enc)
    if app==0x10: v+=base_pc
    elif app==0x30: v+=HDR
    elif app==0: pass
    else: raise Exception('app %x'%enc)
    return v,n
ver,ehp_enc,cnt_enc,tab_enc=img[HDR:HDR+4]
p=HDR+4
eh,n=enc_read(ehp_enc,p,p); p+=n
cnt,n=enc_read(cnt_enc,p,p); p+=n
print('eh_frame at',hex(eh),'fde count',cnt,file=sys.stderr)
def uleb(pos):
    r=0;s=0
    while True:
        b=img[pos];pos+=1;r|=(b&0x7f)<<s;s+=7
        if b<0x80: return r,pos
cie_cache={}
def cie_enc(cie):
    if cie in cie_cache: return cie_cache[cie]
    length=struct.unpack_from('<I',img,cie)[0]; q=cie+8
    ver=img[q]; q+=1
    aug=b''
    while img[q]: aug+=bytes([img[q]]); q+=1
    q+=1
    _,q=uleb(q); # code align
    while img[q]&0x80: q+=1
    q+=1 # data align sleb
    if ver==1: q+=1
    else: _,q=uleb(q)
    enc=0
    if aug.startswith(b'z'):
        _,q=uleb(q)
        for c in aug[1:]:
            if c==ord('R'): enc=img[q]; q+=1
            elif c==ord('P'): pe=img[q]; q+=1; _,n=enc_read(pe,q,q); q+=n
            elif c==ord('L'): q+=1
    cie_cache[cie]=enc; return enc
out=[]
for i in range(cnt):
    loc,n1=enc_read(tab_enc,p,p); p+=4
    fde,n2=enc_read(tab_enc,p,p); p+=4
    ciep=struct.unpack_from('<I',img,fde+4)[0]; cie=fde+4-ciep
    enc=cie_enc(cie)
    q=fde+8
    start,n=enc_read(enc,q,q); q+=n
    rng,n=enc_read(enc&0x0f,q,q)
    out.append((start,rng))
out.sort()
with open(sys.argv[2],'w') as f:
    for s,r in out: f.write('%x %x\n'%(s,r))
tot=sum(r for s,r in out)
print('functions',len(out),'bytes covered',tot,'min',hex(out[0][0]),'max',hex(out[-1][0]+out[-1][1]),file=sys.stderr)
