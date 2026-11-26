# Prepara il corpo base da MakeHuman (asset CC0: base.obj e target "macrodetails") in mh_body.npz.
#
#   python3 prepare_mh.py <cartella makehuman/data>
#
# Il corpo: maschio, 25 anni, muscoloso, peso medio (soldato). Si salvano vertici, coordinate texture, facce
# per gruppo (corpo, occhi) e le articolazioni (gruppi "joint-*" della base, media dei loro vertici).
# Coordinate di MakeHuman: decimetri, Y in alto, faccia verso +Z -> qui metri, Z in alto, faccia verso -Y.
import sys, os, re
import numpy as np

DATA = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
TG = os.path.join(DATA, 'targets', 'macrodetails')

V, VT, groups = [], [], {}
cur = None
for line in open(os.path.join(DATA, '3dobjs', 'base.obj')):
    if line.startswith('v '): V.append([float(x) for x in line.split()[1:4]])
    elif line.startswith('vt '): VT.append([float(x) for x in line.split()[1:3]])
    elif line.startswith('g '): cur = line.split()[1]; groups.setdefault(cur, [])
    elif line.startswith('f '):
        groups[cur].append([tuple(int(i) - 1 if i else -1 for i in (p.split('/') + ['', ''])[:2]) for p in line.split()[1:]])
V = np.array(V); VT = np.array(VT)


def target(name, w):
    global V
    if w == 0: return
    p = os.path.join(TG, name)
    for line in open(p):
        if not line[:1].isdigit(): continue
        i, dx, dy, dz = line.split()
        V[int(i)] += w * np.array([float(dx), float(dy), float(dz)])


# miscela dei target come fa MakeHuman per: maschio 1, eta' 25 (young), muscoli 0.8, peso 0.5, caucasico
mus = 0.85   # 0 = medio, 1 = massimo (muscoli 0.8 della scala 0.5..1)
target('caucasian-male-young.target', 1.0)
target('universal-male-young-averagemuscle-averageweight.target', 1.0 - mus)
target('universal-male-young-maxmuscle-averageweight.target', mus)

joints = {}
for g, faces in groups.items():
    if g.startswith('joint-'):
        idx = sorted({vi for f in faces for vi, _ in f})
        joints[g[6:]] = V[idx].mean(0)

conv = lambda p: np.array([p[..., 0], -p[..., 2], p[..., 1]]).transpose(list(range(1, p.ndim)) + [0]) * 0.1 if p.ndim > 1 else np.array([p[0], -p[2], p[1]]) * 0.1
Vb = np.stack([V[:, 0], -V[:, 2], V[:, 1]], 1) * 0.1
J = {k: np.array([v[0], -v[2], v[1]]) * 0.1 for k, v in joints.items()}


def pack(gname):
    fv, ft, fn = [], [], []
    for f in groups.get(gname, []):
        fn.append(len(f)); fv.extend(i for i, _ in f); ft.extend(t for _, t in f)
    return np.array(fv, np.int32), np.array(ft, np.int32), np.array(fn, np.int32)

out = {'V': Vb, 'VT': VT}
for g in ('body', 'helper-l-eye', 'helper-r-eye'):
    fv, ft, fn = pack(g)
    k = g.replace('helper-', '').replace('-', '_')
    out[k + '_fv'] = fv; out[k + '_ft'] = ft; out[k + '_fn'] = fn
out['joint_names'] = np.array(list(J.keys()))
out['joint_pos'] = np.array(list(J.values()))
np.savez_compressed(os.path.join(HERE, 'mh_body.npz'), **out)
print('vertici', len(Vb), 'facce corpo', len(out['body_fn']), 'articolazioni', len(J), 'altezza', Vb[:, 2].max() - Vb[:, 2].min())
