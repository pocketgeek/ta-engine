"""List GAF sequences: name, frame count, frame w/h/offsets."""
import struct, sys
def entries(path):
    d=open(path,'rb').read()
    ver,n,_=struct.unpack_from('<3I',d,0)
    out=[]
    for i in range(n):
        p=struct.unpack_from('<I',d,12+4*i)[0]
        nf,u1,u2=struct.unpack_from('<HHI',d,p); name=d[p+8:p+40].split(b'\0')[0].decode('latin1')
        fr=[]
        for j in range(nf):
            fp,=struct.unpack_from('<I',d,p+40+8*j)
            w,h,xo,yo=struct.unpack_from('<HHhh',d,fp); fr.append((w,h,xo,yo))
        out.append((name,fr))
    return out
if __name__=='__main__':
    for f in sys.argv[1:]:
        print('==',f.split('/')[-1])
        for name,fr in entries(f):
            dims=sorted(set(fr)); print('  %-24s n=%d %s'%(name,len(fr),dims[:4]))
