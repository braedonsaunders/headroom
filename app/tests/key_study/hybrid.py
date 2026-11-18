import sys, os, math, itertools
KS_DIR=os.environ.get('KEY_STUDY_DIR', '/tmp/key_study')
sys.path.insert(0, KS_DIR)
import keylib as K
TX=f'{KS_DIR}/tx'
def txnotes(f):
    p=f"{TX}/{os.path.basename(f).replace('.jams','')}.csv"
    return [(float(a[0]), float(a[1])-float(a[0]), float(a[2])) for a in (l.strip().split(',') for l in open(p)) if len(a)>=3]
TEMPL=K.TEMPL
def feats(notes):
    """per-key scores: KS corr (24), chord-function (24, normalised by chord time), chord time"""
    h=K.hist(notes); M,m=K.PROF['temperley']
    ks=[K.corr(h,M,r) for r in range(12)]+[K.corr(h,m,r) for r in range(12)]
    cs=K.chords_from_notes(notes)
    S=[0.0]*24; tot=0
    for t,d,v in cs:
        r,q=K.parse(v); qc=K.qclass(q)
        for k in range(12):
            deg=(r-k)%12
            S[k]+=d*K.FUNC_MAJ.get((deg,qc),-0.5); S[12+k]+=d*K.FUNC_MIN.get((deg,qc),-0.5)
        tot+=d
    T=max((t+d for t,d,m in notes), default=1)
    return ks, [x/tot if tot else 0 for x in S], tot/T
def predict(F, w, frac_full):
    ks,cf,frac=F; a=w*min(1,frac/frac_full)
    best=max(range(24), key=lambda k: ks[k]+a*cf[k]); return best%12, best//12
def evaluate(FS, w, ff):
    ex=0; ws=0
    for f,(F,gt) in FS.items():
        s=K.score(predict(F,w,ff),gt); ex+=s=='exact'; ws+=K.W[s]
    return 100*ex/len(FS), 100*ws/len(FS)
train={f:(feats(v[0]),v[2]) for f,v in K.data.items() if os.path.basename(f)[:2] not in ('00','03')}
test={}
for f,v in K.data.items():
    if os.path.basename(f)[:2] in ('00','03'): test[f]=(feats(txnotes(f)),v[2])
best=None
for w,ff in itertools.product((0,0.05,0.1,0.15,0.2,0.3,0.5),(0.2,0.4,0.6,1.0)):
    e,ws=evaluate(train,w,ff)
    if best is None or ws>best[0]: best=(ws,e,w,ff)
print('tuned on 240 true-note takes: weighted %.1f exact %.1f  w=%s frac_full=%s'%best)
w,ff=best[2],best[3]
for kind in ('comp','solo',''):
    sub={f:v for f,v in test.items() if '_'+kind in f} if kind else test
    print(f'TEST transcribed {kind or "all"} ({len(sub)}): exact %.1f%% weighted %.1f%%' % evaluate(sub,w,ff))

# ---- z-scored blend driven by "chordalness" (fraction of time with recognised chords) ----
def z(v):
    m=sum(v)/len(v); s=math.sqrt(sum((x-m)**2 for x in v)/len(v)) or 1; return [(x-m)/s for x in v]
def predict2(F, lo, hi, base):
    ks,cf,frac=F
    a=min(1,max(0,(frac-lo)/(hi-lo))) if hi>lo else (1 if frac>=lo else 0)
    a=base+(1-base)*a if any(cf) else 0
    zk,zc=z(ks),z(cf) if any(cf) else [0]*24
    best=max(range(24), key=lambda k:(1-a)*zk[k]+a*zc[k]); return best%12,best//12
def evaluate2(FS,*p):
    ex=ws=0
    for f,(F,gt) in FS.items():
        s=K.score(predict2(F,*p),gt); ex+=s=='exact'; ws+=K.W[s]
    return 100*ex/len(FS),100*ws/len(FS)
best=None
for lo,hi,base in itertools.product((0.05,0.1,0.2,0.3,0.4),(0.2,0.3,0.4,0.5,0.7),(0,0.2,0.4)):
    if hi<lo: continue
    e,ws=evaluate2(train,lo,hi,base)
    if best is None or ws>best[0]: best=(ws,e,lo,hi,base)
print('blend tuned on 240: weighted %.1f exact %.1f lo=%s hi=%s base=%s'%best)
for kind in ('comp','solo',''):
    sub={f:v for f,v in test.items() if '_'+kind in f} if kind else test
    print(f'TEST transcribed {kind or "all"} ({len(sub)}): exact %.1f%% weighted %.1f%%' % evaluate2(sub,*best[2:]))
print('chordal fraction comp vs solo (test):', sum(v[0][2] for f,v in test.items() if '_comp' in f)/60, sum(v[0][2] for f,v in test.items() if '_solo' in f)/60)

# ---- chords only from windows where >=3 notes sound simultaneously; chordalness = that fraction ----
def poly_windows(notes, win=0.5, need=3):
    T=max((t+d for t,d,m in notes), default=0); out=[]
    ev=sorted(notes)
    t0=0.0
    while t0<T:
        mid=[t0+win*f for f in (0.25,0.5,0.75)]
        poly=max(sum(1 for t,d,m in ev if t<=x<t+d) for x in mid)
        out.append(poly>=need); t0+=win
    return out
def feats3(notes, need=3):
    h=K.hist(notes); M,m=K.PROF['temperley']
    ks=[K.corr(h,M,r) for r in range(12)]+[K.corr(h,m,r) for r in range(12)]
    pw=poly_windows(notes,0.5,need)
    cs=K.chords_from_notes(notes,0.5)
    S=[0.0]*24; tot=0
    for t,d,v in cs:
        i=int(t/0.5+1e-6)
        if i<len(pw) and pw[i]:
            r,q=K.parse(v); qc=K.qclass(q)
            for k in range(12):
                deg=(r-k)%12; S[k]+=d*K.FUNC_MAJ.get((deg,qc),-0.5); S[12+k]+=d*K.FUNC_MIN.get((deg,qc),-0.5)
            tot+=d
    return ks,[x/tot if tot else 0 for x in S],(sum(pw)/len(pw) if pw else 0)
for need in (2,3):
    train3={f:(feats3(v[0],need),v[2]) for f,v in K.data.items() if os.path.basename(f)[:2] not in ('00','03')}
    test3={f:(feats3(txnotes(f),need),v[2]) for f,v in K.data.items() if os.path.basename(f)[:2] in ('00','03')}
    print(f'need={need} polyphonic fraction: comp %.2f solo %.2f' % (sum(v[0][2] for f,v in test3.items() if '_comp' in f)/60, sum(v[0][2] for f,v in test3.items() if '_solo' in f)/60))
    best=None
    for lo,hi,base in itertools.product((0.02,0.05,0.1,0.2,0.3,0.4),(0.1,0.2,0.3,0.5,0.7),(0,0.2,0.4)):
        if hi<lo: continue
        e,ws=evaluate2(train3,lo,hi,base)
        if best is None or ws>best[0]: best=(ws,e,lo,hi,base)
    print('  blend tuned on 240: weighted %.1f exact %.1f lo=%s hi=%s base=%s'%best)
    for kind in ('comp','solo',''):
        sub={f:v for f,v in test3.items() if '_'+kind in f} if kind else test3
        print(f'  TEST transcribed {kind or "all"} ({len(sub)}): exact %.1f%% weighted %.1f%%' % evaluate2(sub,*best[2:]))
