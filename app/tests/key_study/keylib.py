# Oracle diagnostic: key-finding on GuitarSet's ground-truth notes (no audio errors) vs. the annotated key_mode.
import json, glob, re, math, collections, sys, os
PC = {'C':0,'C#':1,'Db':1,'D':2,'D#':3,'Eb':3,'E':4,'F':5,'F#':6,'Gb':6,'G':7,'G#':8,'Ab':8,'A':9,'A#':10,'Bb':10,'B':11}
PROF = {
 'temperley': ([0.748,0.060,0.488,0.082,0.670,0.460,0.096,0.715,0.104,0.366,0.057,0.400],[0.712,0.084,0.474,0.618,0.049,0.460,0.105,0.747,0.404,0.067,0.133,0.330]),
 'kk': ([6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88],[6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17]),
 'as': ([0.238,0.006,0.111,0.006,0.137,0.094,0.016,0.214,0.009,0.080,0.008,0.081],[0.220,0.006,0.104,0.123,0.019,0.103,0.012,0.214,0.062,0.022,0.061,0.052]),
}
def corr(h, p, r):
    x=[h[(i+r)%12] for i in range(12)]; mx=sum(x)/12; mp=sum(p)/12
    sxy=sum((a-mx)*(b-mp) for a,b in zip(x,p)); sxx=sum((a-mx)**2 for a in x); syy=sum((b-mp)**2 for b in p)
    return sxy/math.sqrt(sxx*syy) if sxx>0 else 0
def ks(h, prof):
    M,m=PROF[prof]; best=max(((corr(h,M,r),r,0) for r in range(12)), key=lambda t:t[0]); bm=max(((corr(h,m,r),r,1) for r in range(12)), key=lambda t:t[0])
    s=max(best,bm); return s[1], s[2]
def score(pred, gt):
    (pr,pm),(gr,gm)=pred,gt
    if pr==gr and pm==gm: return 'exact'
    if pm==gm and (pr-gr)%12 in (5,7): return 'fifth'
    if pm!=gm and ((gm==0 and pr==(gr+9)%12) or (gm==1 and pr==(gr+3)%12)): return 'relative'
    if pr==gr: return 'parallel'
    return 'other'
W={'exact':1,'fifth':.5,'relative':.3,'parallel':.2,'other':0}
def load(f):
    d=json.load(open(f)); notes=[]; chords=[]
    for a in d['annotations']:
        if a['namespace']=='note_midi':
            for o in a['data']: notes.append((o['time'], o['duration'], o['value']))
        if a['namespace']=='chord' and 'Semi' not in (a['annotation_metadata'].get('data_source') or ''): chords=[(o['time'],o['duration'],o['value']) for o in a['data']]
    k=[o['value'] for a in d['annotations'] if a['namespace']=='key_mode' for o in a['data']][0]
    r,mo=k.split(':'); return notes, chords, (PC[r], 0 if mo=='major' else 1)
files=sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "..", "..", "work", "midi", "dl", "gset_ann", "*.jams")))
data={f:load(f) for f in files}
def report(name, fn):
    c=collections.Counter(); ws=0; bystyle=collections.defaultdict(collections.Counter)
    for f,(notes,chords,gt) in data.items():
        s=score(fn(notes,chords),gt); c[s]+=1; ws+=W[s]; st=re.match(r'\d+_([A-Za-z]+)',f.split('/')[-1]).group(1); bystyle[st][s]+=1
    n=len(data); print(f"{name:34s} exact {100*c['exact']/n:5.1f}%  weighted {100*ws/n:5.1f}%  | fifth {c['fifth']} rel {c['relative']} par {c['parallel']} other {c['other']}  |", ' '.join(f"{k}:{100*v['exact']/sum(v.values()):.0f}" for k,v in sorted(bystyle.items())))
def hist(notes, w='dur'):
    h=[0]*12
    for t,d,m in notes: h[int(round(m))%12]+= d if w=='dur' else 1
    return h

# ---- Temperley Bayesian (pc presence per segment) ----
def bayes(notes, seg=1.0, prof='temperley'):
    if not notes: return (0,0)
    T=max(t+d for t,d,m in notes); nseg=int(T/seg)+1
    pres=[set() for _ in range(nseg)]
    for t,d,m in notes:
        for s in range(int(t/seg), min(nseg, int((t+d)/seg)+1)): pres[s].add(int(round(m))%12)
    M,m=PROF[prof]; sM=max(M); sm=max(m)
    P=[[min(0.97,v/ (1 if prof=='temperley' else sM)) for v in M],[min(0.97,v/(1 if prof=='temperley' else sm)) for v in m]]
    best=None
    for mode in (0,1):
        for r in range(12):
            ll=0
            for s in pres:
                if not s: continue
                for pc in range(12):
                    p=P[mode][(pc-r)%12]; ll+= math.log(p) if pc in s else math.log(1-p)
            if best is None or ll>best[0]: best=(ll,r,mode)
    return best[1],best[2]

# ---- chord-based ----
QI={'maj':(0,4,7),'min':(0,3,7),'7':(0,4,7,10),'maj7':(0,4,7,11),'min7':(0,3,7,10),'dim':(0,3,6),'hdim7':(0,3,6,10),'dim7':(0,3,6,9),'aug':(0,4,8),'sus2':(0,2,7),'sus4':(0,5,7),'maj6':(0,4,7,9),'min6':(0,3,7,9),'9':(0,4,7,10),'min9':(0,3,7,10),'maj9':(0,4,7,11)}
def parse(ch):
    if ch in ('N','X'): return None
    r,_,q=ch.partition(':'); q=q.split('/')[0].split('(')[0] or 'maj'
    return PC[r], q if q in QI else ('min' if q.startswith('min') else 'maj')
# diatonic chord-function weights (degree, triad-quality-class) for major / minor keys
FUNC_MAJ={(0,'M'):3.0,(5,'M'):1.6,(7,'M'):2.0,(9,'m'):1.2,(2,'m'):1.2,(4,'m'):0.8,(11,'d'):0.6,(10,'M'):0.4,(5,'m'):0.3}
FUNC_MIN={(0,'m'):3.0,(5,'m'):1.6,(7,'M'):1.6,(7,'m'):1.0,(3,'M'):1.0,(8,'M'):1.2,(10,'M'):1.0,(2,'d'):0.8,(5,'M'):0.4}
def qclass(q): return 'm' if q.startswith('min') else 'd' if q in ('dim','hdim7','dim7') else 'M'
def chordkey(chords, w_first=0.0, w_last=0.0):
    S=[[0]*12 for _ in range(2)]
    cs=[(t,d,parse(v)) for t,d,v in chords]; cs=[x for x in cs if x[2]]
    for i,(t,d,(r,q)) in enumerate(cs):
        qc=qclass(q)
        for k in range(12):
            deg=(r-k)%12
            S[0][k]+=d*FUNC_MAJ.get((deg,qc),-0.5)
            S[1][k]+=d*FUNC_MIN.get((deg,qc),-0.5)
            if i==0: S[0][k]+=w_first*(deg==0 and qc=='M'); S[1][k]+=w_first*(deg==0 and qc=='m')
            if i==len(cs)-1: S[0][k]+=w_last*(deg==0 and qc=='M'); S[1][k]+=w_last*(deg==0 and qc=='m')
    best=max(((S[m][k],k,m) for m in (0,1) for k in range(12)))
    return best[1],best[2]
def firstroot(n,c):
    cs=[parse(v) for t,d,v in c]; cs=[x for x in cs if x]; r,q=cs[0]; return r, 1 if qclass(q)=='m' else 0

# ---- split comp / solo, and chords recognised from the played notes (beat windows) ----
TEMPL=[(q,iv) for q,iv in [('maj',(0,4,7)),('min',(0,3,7)),('7',(0,4,7,10)),('maj7',(0,4,7,11)),('min7',(0,3,7,10)),('dim',(0,3,6)),('hdim7',(0,3,6,10)),('sus4',(0,5,7)),('sus2',(0,2,7))]]
def chords_from_notes(notes, win=0.5):
    if not notes: return []
    T=max(t+d for t,d,m in notes); out=[]
    t0=0.0
    while t0<T:
        h=[0]*12; low=None
        for t,d,m in notes:
            ov=min(t+d,t0+win)-max(t,t0)
            if ov>0:
                h[int(round(m))%12]+=ov
                if low is None or m<low[0]: low=(m,ov)
        tot=sum(h)
        if tot>0 and sum(1 for v in h if v>0.15*max(h))>=2:
            best=None
            for r in range(12):
                for q,iv in TEMPL:
                    s=sum(h[(r+i)%12] for i in iv)/tot - 0.12*len(iv)/3 + (0.1 if low and int(round(low[0]))%12==r else 0)
                    if best is None or s>best[0]: best=(s,r,q)
            if best[0]>0.5: out.append((t0,win,f"{['C','C#','D','Eb','E','F','F#','G','Ab','A','Bb','B'][best[1]]}:{best[2]}"))
        t0+=win
    return out
