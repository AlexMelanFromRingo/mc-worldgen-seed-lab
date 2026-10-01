import math, sys
M48=(1<<48)-1
def s32(x): x&=0xFFFFFFFF; return x-(1<<32) if x>>31 else x
def s64(x): x&=(1<<64)-1; return x-(1<<64) if x>>63 else x
class Lcg:
    def __init__(s, seed): s.st=(seed ^ 0x5DEECE66D)&M48
    def next(s, bits):
        s.st=(s.st*0x5DEECE66D+0xB)&M48
        return s32(s.st>>(48-bits))
    def nextInt(s, bound):
        if bound&(bound-1)==0: return (bound*s.next(31))>>31
        while True:
            smp=s.next(31); m=smp%bound
            if s32(smp-m+(bound-1))>=0: return m
    def nextLong(s):
        hi=s.next(32); lo=s.next(32); return s64((hi<<32)+lo)
    def nextDouble(s):
        a=s.next(26); b=s.next(27); return ((a<<27)+b)*(2.0**-53)
    def fork(s): return Lcg(s.nextLong())
def is_pref(x,y,z):
    v=s32(s32(x*73856093)^s32(z*19349663)^s32(y*83492791))
    return ((v&0xFFFFFFFF)>>7)%5==0
def jround(x): return int(math.floor(x+0.5))
def find_biome_horizontal(ox,oy,oz,radius,pred,rnd):
    cx=ox>>2; cz=oz>>2; R=radius>>2; ny=oy>>2
    result=None; found=0
    for z in range(-R,R+1):
        for x in range(-R,R+1):
            nx=cx+x; nz=cz+z
            if pred(nx,ny,nz):
                if result is None or rnd.nextInt(found+1)==0:
                    result=(nx*4, nz*4)
                found+=1
    return result
def rings(seed, distance=32, spread=3, count=128, pred=is_pref):
    r=Lcg(seed)
    angle=r.nextDouble()*math.pi*2.0
    pic=0; circle=0; out=[]
    for i in range(count):
        dist=4*distance+distance*circle*6+(r.nextDouble()-0.5)*(distance*2.5)
        ix=jround(math.cos(angle)*dist); iz=jround(math.sin(angle)*dist)
        fk=r.fork()
        res=find_biome_horizontal(ix*16+8,0,iz*16+8,112,pred,fk)
        out.append((res[0]>>4,res[1]>>4) if res else (ix,iz))
        angle+=(math.pi*2.0)/spread
        pic+=1
        if pic==spread:
            circle+=1; pic=0
            spread+=2*spread//(circle+1)
            spread=min(spread,count-i)
            angle+=r.nextDouble()*math.pi*2.0
    return out
if __name__=='__main__':
    for line in open(sys.argv[1]):
        if ' seed ' not in line and not line.startswith('seed'): continue
        line=line[line.index('seed '):]
        head,rest=line.split(' : ')
        seed=int(head.split()[1])
        java=[tuple(map(int,t.split(','))) for t in rest.split()]
        py=rings(seed)
        bad=[i for i in range(len(py)) if py[i]!=java[i]]
        print(seed, len(java), 'MISMATCH at', bad[:10] if bad else 'ALL 128 MATCH')
