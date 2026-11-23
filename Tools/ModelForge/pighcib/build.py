# Modello d'autore del Pighcib: un porcellino d'India guastato dalla pietra, grottesco come un goblin. Corpo gobbo
# con costole, vertebre e bubboni sopra una pancia gonfia (sdf.py); testa scavata con una bocca larga piena di denti
# a spillo e incisivi gialli, orecchie sbrindellate, occhi rossi in fondo alle orbite; pelle rognosa con chiazze di pelo
# sporco e infeltrito; schegge di pietra verdastra che bucano spalla, schiena, cranio e braccio, con vene scure attorno.
# Scheletro del gioco (rig.json, quello del pighcib_master.fbx) + Jaw e orecchie. Uscita:
# Sandbox/GameData/Models/Characters/pighcib/model.glb
import sys, os, json, math, random
import numpy as np
import bpy, bmesh
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)
import sdf
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
OUT = os.path.abspath(ARGV[ARGV.index('--out') + 1]) if '--out' in ARGV else \
    os.path.abspath(os.path.join(HERE, '../../../Sandbox/GameData/Models/Characters/pighcib/model.glb'))
TEX = 2048
random.seed(5); np.random.seed(5)
PREFIX = 'mixamorig1:'

R = json.load(open('rig.json'))
J = {k: np.array(v) for k, v in R['joints'].items()}
PAR = {k: v for k, v in R['parent'].items()}
PAR.update({'Jaw': 'Head', 'LeftEar': 'Head', 'RightEar': 'Head'})
H, HS = sdf.H, sdf.HS


def hl(off):
    """punto nelle coordinate della testa (attorno all'osso Head, prima della scala HS) -> mondo"""
    return H + np.asarray(off, float) * HS


bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
col = sc.collection


def link(name, verts, faces):
    me = bpy.data.meshes.new(name); me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces]); me.update()
    ob = bpy.data.objects.new(name, me); col.objects.link(ob)
    return ob


def activate(ob):
    for o in bpy.context.view_layer.objects: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


# ------------------------------------------------------------------ corpo
if not os.path.exists('shape.npz'):
    v_, f_ = sdf.build(); np.savez('shape.npz', v=v_, f=f_, key='')
d = np.load('shape.npz')
body = link('Body', d['v'], d['f'])
activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.remove_doubles(threshold=1e-5); bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')
m = body.modifiers.new('dec', 'DECIMATE'); m.ratio = 0.12
bpy.ops.object.modifier_apply(modifier='dec')
m = body.modifiers.new('sm', 'SMOOTH'); m.factor = 0.6; m.iterations = 4
bpy.ops.object.modifier_apply(modifier='sm')
for p in body.data.polygons: p.use_smooth = True
print('corpo', len(body.data.vertices), len(body.data.polygons))
bm = bmesh.new(); bm.from_mesh(body.data)
BVH = BVHTree.FromBMesh(bm)


def surf(p):
    loc, nrm, _, _ = BVH.find_nearest(Vector(p))
    return np.array(loc), np.array(nrm)


def tube(name, pts, r0, r1=0.0, sides=10, seg=5):
    pts = [np.asarray(p, float) for p in pts]
    P = []
    ext = [pts[0] * 2 - pts[1]] + pts + [pts[-1] * 2 - pts[-2]]
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for k in range(seg):
            t = k / seg
            P.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    P.append(pts[-1]); P = np.array(P); n = len(P)
    T = np.gradient(P, axis=0); T /= np.linalg.norm(T, axis=1)[:, None]
    up = np.array([0, 0, 1.0]) if abs(T[0][2]) < 0.9 else np.array([1.0, 0, 0])
    V, F = [], []
    nprev = np.cross(T[0], up); nprev /= np.linalg.norm(nprev)
    for i in range(n):
        nn = nprev - T[i] * (nprev @ T[i]); nn /= np.linalg.norm(nn); nprev = nn
        bb = np.cross(T[i], nn)
        r = r0 + (r1 - r0) * (i / (n - 1)) ** 1.2
        for k in range(sides):
            a = 2 * math.pi * k / sides
            V.append(P[i] + r * (math.cos(a) * nn + math.sin(a) * bb))
    for i in range(n - 1):
        for k in range(sides):
            a, b = i * sides + k, i * sides + (k + 1) % sides
            F.append((a, b, b + sides, a + sides))
    V.append(P[0]); c0 = len(V) - 1
    for k in range(sides): F.append((c0, (k + 1) % sides, k))
    return link(name, V, F)


def spike(V, F, base, dirv, L, r, sides=5):
    up = np.array([0, 0, 1.0]) if abs(dirv[2]) < 0.9 else np.array([1.0, 0, 0])
    a = np.cross(dirv, up); a /= np.linalg.norm(a); b = np.cross(dirv, a)
    i0 = len(V)
    sag = np.array([0, 0, -L * 0.1])
    for rr, f, dz in ((r, 0.0, 0.0), (r * 0.6, 0.5, 0.3)):
        cc = base + dirv * L * f + sag * dz
        for k in range(sides):
            t = 2 * math.pi * k / sides
            V.append(cc + rr * (math.cos(t) * a + math.sin(t) * b))
    V.append(base + dirv * L + sag)
    tip = len(V) - 1
    for k in range(sides):
        k1 = (k + 1) % sides
        F.append((i0 + k, i0 + k1, i0 + sides + k1, i0 + sides + k))
        F.append((i0 + sides + k, i0 + sides + k1, tip))


def mir(P):
    Q = np.array(P, float); Q[..., 0] = np.abs(Q[..., 0]); return Q


parts = []


def hw(p):
    """punto nelle coordinate della testa di sdf.head (prima della scala) -> mondo"""
    return H + (np.asarray(p, float) - H) * HS


# ------------------------------------------------------------------ bocca: incisivi da roditore e file di denti a spillo
mf, mb = sdf.MOUTH_FRONT, sdf.MOUTH_BACK
for s in (1, -1):
    # incisivi superiori lunghi, gialli, che scendono oltre il labbro (sopra = Head)
    b0 = hw(mf + [0.007 * s, 0.006, 0.01])
    parts.append((tube('TeethUp', [b0, b0 + np.array([0.001 * s, -0.008, -0.022]) * HS, b0 + np.array([0.0, -0.004, -0.045]) * HS],
                       0.0055 * HS, 0.0012, 6, 4), 'Head'))
    # incisivi inferiori (mandibola)
    b0 = hw(mf + [0.006 * s, 0.012, -0.016])
    parts.append((tube('TeethLow', [b0, b0 + np.array([0.0, -0.006, 0.014]) * HS, b0 + np.array([0.002 * s, -0.004, 0.026]) * HS],
                       0.0045 * HS, 0.001, 6, 3), 'Jaw'))
    # denti a spillo storti lungo tutta la bocca, sopra e sotto
    for k in range(8):
        t = 0.12 + 0.8 * k / 7
        p = mf + (mb * [s, 1, 1] - mf) * t
        for up, rule in ((1, 'Head'), (-1, 'Jaw')):
            L = random.uniform(0.008, 0.017) * (1.2 - 0.5 * t)
            base = hw(p + [0.004 * s, 0.0, 0.009 * up])
            tip = base + np.array([random.uniform(-0.004, 0.004) + 0.002 * s, random.uniform(-0.004, 0.003), -L * up]) * HS
            parts.append((tube('TeethUp' if up > 0 else 'TeethLow', [base, (base + tip) / 2 + np.random.normal(0, 0.001, 3), tip],
                               random.uniform(0.0022, 0.0032) * HS, 0.0004, 5, 2), rule))
    # zannette dei Vrepigh dalla mandibola, spezzate e storte
    b0 = hw(sdf.JAW_B + [0.024 * s, 0.03, 0.006])
    parts.append((tube('Tusk', [b0, b0 + np.array([0.008 * s, -0.004, 0.02]) * HS, b0 + np.array([0.016 * s, 0.004, 0.034]) * HS],
                       0.006 * HS, 0.0018 if s > 0 else 0.0035, 7, 3), 'Jaw'))


# orecchie da goblin: grandi, aperte all'indietro, sbrindellate, con buchi e tacche
def ear(s):
    base = hw(H + [0.075 * s, -0.01, 0.13])
    ax = np.array([0.62 * s, 0.6, 0.5]); ax /= np.linalg.norm(ax)
    wd = np.array([0.0, -0.35, 1.0]); wd -= ax * (wd @ ax); wd /= np.linalg.norm(wd)
    nn = np.cross(ax, wd) * s
    Lh, W = 0.17 * HS, 0.085 * HS
    nu, nv = 13, 9
    rng = np.random.RandomState(31 if s > 0 else 37)
    nick = np.zeros(nu, bool); nick[[4, 9]] = True                    # due tacche sul bordo
    V, F = [], []
    for side in (0, 1):
        for i in range(nu):
            u = i / (nu - 1)
            w = W * 0.5 * math.sin(math.pi * min(u * 1.12 + 0.04, 1.0)) ** 0.7 * (1 - u) ** 0.35 + 0.0015
            if nick[i]: w *= 0.55
            for j in range(nv):
                v = j / (nv - 1) * 2 - 1
                cup = (1 - v * v) * 0.012 * HS * math.sin(math.pi * u)
                droop = -0.045 * HS * u ** 2.0                                # la punta ricade
                p = base + ax * (Lh * u) + wd * (w * v) + nn * (cup + (0.003 if side else -0.003) * (1 - 0.6 * u)) + np.array([0, 0, droop])
                V.append(p)
    off = nu * nv
    holes = {(6, 3), (8, 5)}                                         # buchi
    for i in range(nu - 1):
        for j in range(nv - 1):
            if (i, j) in holes: continue
            a = i * nv + j
            F.append((a, a + 1, a + nv + 1, a + nv))
            F.append((off + a + nv, off + a + nv + 1, off + a + 1, off + a))
    for (i, j) in holes:                                                 # pareti dei buchi
        a = i * nv + j
        for e0, e1 in ((a, a + 1), (a + 1, a + nv + 1), (a + nv + 1, a + nv), (a + nv, a)):
            F.append((e1, e0, off + e0, off + e1))
    for i in range(nu - 1):
        for j in (0, nv - 1):
            a, b = i * nv + j, (i + 1) * nv + j
            F.append((a, b, off + b, off + a) if j == 0 else (b, a, off + a, off + b))
    for j in range(nv - 1):
        F.append((j + 1, j, off + j, off + j + 1))
        a = (nu - 1) * nv + j
        F.append((a, a + 1, off + a + 1, off + a))
    return link('Ear_' + ('L' if s > 0 else 'R'), V, F)


for s in (1, -1):
    parts.append((ear(s), 'LeftEar' if s > 0 else 'RightEar'))

# occhi piccoli in fondo alle orbite, con un riflesso rosso malato
for s in (1, -1):
    c = hw(sdf.EYE * [s, 1, 1] + [0.004 * s, 0.004, 0.0]) if s > 0 else hw(np.array([-sdf.EYE[0] - 0.004, sdf.EYE[1] + 0.004, sdf.EYE[2]]))
    bpy.ops.mesh.primitive_uv_sphere_add(segments=12, ring_count=8, radius=0.011 * HS, location=tuple(c))
    o = bpy.context.active_object; o.name = 'Eye_' + ('L' if s > 0 else 'R')
    parts.append((o, 'Head'))

# artigli lunghi e sporchi alle mani e ai piedi
for side, pre in ((1, 'Left'), (-1, 'Right')):
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        b = J[pre + 'Hand' + f + '2']; c = J[pre + 'Hand' + f + '3']
        dirv = (c - b) / np.linalg.norm(c - b)
        loc, _ = surf(c + (c - b) * 1.3 + dirv * 0.02)
        p0 = loc - dirv * 0.004
        down = np.array([0, 0, -1.0])
        parts.append((tube('Claw', [p0, p0 + dirv * 0.014 + down * 0.002, p0 + dirv * 0.026 + down * 0.009], 0.0042, 0.0005, 6, 3), 'near'))
    t = J[pre + 'ToeBase']
    for k in (-1, 0, 1):
        loc, _ = surf(t + [0.02 * k * side, -0.07, 0.0])
        p0 = loc + np.array([0, 0.004, 0])
        parts.append((tube('Claw', [p0, p0 + np.array([0.004 * k * side, -0.014, -0.002]), p0 + np.array([0.006 * k * side, -0.022, -0.009])], 0.0045, 0.0006, 6, 3), 'near'))

# ------------------------------------------------------------------ la pietra: schegge che bucano la pelle
CRYSTALS = []   # (centro sulla pelle, normale)


def shard(V, F, base, dirv, L, r, sides=6):
    up = np.array([0, 0, 1.0]) if abs(dirv[2]) < 0.9 else np.array([1.0, 0, 0])
    a = np.cross(dirv, up); a /= np.linalg.norm(a); b = np.cross(dirv, a)
    i0 = len(V)
    rot = random.uniform(0, 1)
    for f, rr in ((-0.25, r * 0.9), (0.72, r)):                        # prisma esagonale che affonda nella pelle
        for k in range(sides):
            t = 2 * math.pi * (k + rot) / sides
            V.append(base + dirv * L * f + rr * (math.cos(t) * a + math.sin(t) * b))
    V.append(base + dirv * L)
    tip = len(V) - 1
    for k in range(sides):
        k1 = (k + 1) % sides
        F.append((i0 + k, i0 + k1, i0 + sides + k1, i0 + sides + k))
        F.append((i0 + sides + k, i0 + sides + k1, tip))
    F.append(tuple(i0 + k for k in reversed(range(sides))))


SV, SF = [], []
CLUSTERS = [((-0.1, 0.06, 0.7), 5, 0.18), ((0.0, 0.12, 0.62), 3, 0.14), ((0.0, 0.115, 0.53), 2, 0.1),
            (tuple(hw(H + [0.055, -0.01, 0.14])), 3, 0.12), ((0.26, -0.03, 0.66), 2, 0.07), ((-0.12, -0.08, 0.46), 2, 0.08)]
for ctr, n, Lmax in CLUSTERS:
    loc, nrm = surf(np.array(ctr))
    CRYSTALS.append((loc, nrm))
    for _ in range(n):
        p, nn_ = surf(loc + np.random.normal(0, 0.014, 3))
        dv = nn_ + np.random.normal(0, 0.35, 3); dv /= np.linalg.norm(dv)
        Ls = random.uniform(0.45, 1.0) * Lmax
        shard(SV, SF, p - nn_ * 0.006, dv, Ls, random.uniform(0.1, 0.16) * Ls + 0.004)
parts.append((link('Stone_Shards', SV, SF), 'near'))

# ------------------------------------------------------------------ pelle: rogna, chiazze di pelo sporco, vene della pietra
me = body.data
V = np.array([v.co[:] for v in me.vertices]); N = np.array([v.normal[:] for v in me.vertices])
Hl = H + (V - H) / HS
SKIN = np.array([0.085, 0.062, 0.056])          # pelle malata, grigio-rosata
RUST = np.array([0.11, 0.05, 0.022])           # pelo fulvo sporco
SOOT = np.array([0.025, 0.021, 0.019])         # pelo nero di fuliggine
DIRTY = np.array([0.12, 0.1, 0.072])           # pelo crema ingiallito
ph = np.random.rand(6) * 6.28
dirs = np.random.normal(0, 1, (6, 3)); dirs /= np.linalg.norm(dirs, axis=1)[:, None]
fur = sum(np.sin(V @ dirs[i] * 17.0 + ph[i]) for i in range(6)) / 6         # dove resta il pelo
tone = sum(np.sin(V @ dirs[(i + 3) % 6] * 9.0 + ph[i] * 1.3) for i in range(6)) / 6
furcol = np.where(tone[:, None] > 0.15, RUST, np.where(tone[:, None] < -0.2, SOOT, DIRTY))
hasfur = fur > -0.05
C = np.where(hasfur[:, None], furcol, SKIN)
# piaghe e croste
for _ in range(26):
    c = V[np.random.randint(len(V))]
    k = np.clip(1 - np.linalg.norm(V - c, axis=1) / random.uniform(0.008, 0.02), 0, 1)
    C = C * (1 - k[:, None] * 0.85) + np.array([0.12, 0.025, 0.02]) * (k[:, None] * 0.85)
# vene scure attorno alla pietra: reticolo che si dirada allontanandosi
vn = np.abs(sum(np.sin(V @ (np.random.normal(0, 60, 3)) + i) for i in range(4)) / 4)
for loc, nrm in CRYSTALS:
    dist = np.linalg.norm(V - loc, axis=1)
    raw = np.clip(1 - dist / 0.04, 0, 1)                              # carne viva attorno alla scheggia
    vein = np.clip(1 - dist / 0.14, 0, 1) * (vn < 0.08)
    C = C * (1 - raw[:, None] * 0.8) + np.array([0.1, 0.012, 0.02]) * (raw[:, None] * 0.8)
    C = C * (1 - vein[:, None] * 0.85) + np.array([0.035, 0.01, 0.04]) * (vein[:, None] * 0.85)
# grugno e bocca a carne viva, orbite nere, gengive scure
nose = np.linalg.norm(Hl - sdf.NOSE, axis=1) < 0.028
C[nose] = np.array([0.24, 0.06, 0.05])
mt = np.clip(((mir(Hl) - sdf.MOUTH_FRONT) @ (sdf.MOUTH_BACK - sdf.MOUTH_FRONT)) / np.sum((sdf.MOUTH_BACK - sdf.MOUTH_FRONT) ** 2), 0, 1)
md = np.linalg.norm(mir(Hl) - (sdf.MOUTH_FRONT + mt[:, None] * (sdf.MOUTH_BACK - sdf.MOUTH_FRONT)), axis=1)
k = np.clip(1 - (md - 0.008) / 0.012, 0, 1)
C = C * (1 - k[:, None]) + np.array([0.08, 0.012, 0.012]) * k[:, None]
ed = np.linalg.norm(mir(Hl) - sdf.EYE, axis=1)
k = np.clip(1 - (ed - 0.01) / 0.012, 0, 1)
C = C * (1 - k[:, None] * 0.9) + np.array([0.02, 0.01, 0.01]) * (k[:, None] * 0.9)
# mani e piedi neri di sporco
dk = np.zeros(len(V))
for pre in ('Left', 'Right'):
    dk = np.maximum(dk, np.clip(1 - np.linalg.norm(V - J[pre + 'Hand'], axis=1) / 0.12, 0, 1))
dk = np.maximum(dk, np.clip((0.12 - V[:, 2]) / 0.1, 0, 1))
C = C * (1 - dk[:, None] * 0.6) + np.array([0.03, 0.025, 0.02]) * (dk[:, None] * 0.6)
C *= (1 + 0.15 * (sum(np.sin(V @ (np.random.normal(0, 25, 3)) + i) for i in range(5)) / 5))[:, None]
C = np.clip(C, 0, 1)


# ------------------------------------------------------------------ pelo infeltrito, a ciuffi radi, del colore della chiazza
def mange():
    KD = cKDTree(V)
    out = {}
    areas = [((0.08, 0.06, 0.69), 0.07), ((-0.04, 0.1, 0.66), 0.06), (tuple(hw(H + [0.0, 0.02, 0.15])), 0.05),
             ((0.09, 0.07, 0.44), 0.06), ((-0.1, 0.06, 0.42), 0.05), ((0.0, 0.11, 0.57), 0.04)]
    for ctr, rad in areas:
        cs, cn = surf(np.array(ctr))
        for _ in range(int(900 * rad)):
            loc, n = surf(cs + np.random.normal(0, rad * 0.5, 3))
            _, vi = KD.query(loc)
            if not hasfur[vi]: continue
            key = 'Fur_Rust' if tone[vi] > 0.15 else ('Fur_Soot' if tone[vi] < -0.2 else 'Fur_Dirty')
            dvec = n * 0.45 + np.array([0, 0.45, -0.75]) + np.random.normal(0, 0.25, 3); dvec /= np.linalg.norm(dvec)
            out.setdefault(key, ([], []))
            spike(*out[key], loc - n * 0.002, dvec, random.uniform(0.012, 0.03), 0.0028, 4)
    return [link(k, *vf) for k, vf in out.items()]


for o in mange():
    parts.append((o, 'near'))

# ------------------------------------------------------------------ scheletro
arm_data = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', arm_data); col.objects.link(arm)
activate(arm)
bpy.ops.object.mode_set(mode='EDIT')
EXTRA = {'Jaw': hw(sdf.JAW_A + [0, -0.01, 0.01]), 'LeftEar': hw(H + [0.075, -0.01, 0.13]), 'RightEar': hw(H + [-0.075, -0.01, 0.13])}
POS = {k: v for k, v in J.items() if k not in EXTRA}; POS.update(EXTRA)
CH = {}
for k, p in PAR.items():
    if p: CH.setdefault(p, []).append(k)
eb = {}
for name, p in POS.items():
    b = arm_data.edit_bones.new(PREFIX + name)
    b.head = Vector(p)
    kids = [k for k in CH.get(name, []) if k in POS and k not in ('Jaw', 'LeftEar', 'RightEar', 'LeftShoulder', 'RightShoulder', 'LeftUpLeg', 'RightUpLeg')]
    if name == 'Spine2': kids = ['Neck']
    if name == 'Hips': kids = ['Spine']
    if name in ('LeftHand', 'RightHand'): kids = [name + 'Middle1']
    if kids: tail = np.mean([POS[k] for k in kids], 0)
    elif name == 'Head': tail = p + np.array([0, 0, 0.12])
    elif name == 'Jaw': tail = hw(sdf.JAW_B)
    elif name.endswith('Ear'): tail = hw(H + [0.17 * (1 if name.startswith('Left') else -1), 0.07, 0.17])
    elif 'ToeBase' in name: tail = p + np.array([0, -0.04, 0])
    else:
        par = POS[PAR[name]]; dv = p - par; tail = p + dv / max(np.linalg.norm(dv), 1e-6) * 0.015
    if np.linalg.norm(tail - p) < 1e-3: tail = p + np.array([0, 0, 0.015])
    b.tail = Vector(tail); eb[name] = b
for name, b in eb.items():
    pn = PAR.get(name)
    if pn and pn in eb: b.parent = eb[pn]
bpy.ops.object.mode_set(mode='OBJECT')

# pesi del corpo: calore, poi testa e mascella dalla forma
activate(body); arm.select_set(True); bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
vg = {g.name.split(':')[-1]: g for g in body.vertex_groups}
dh = sdf.head(V); db = sdf.body(V)
in_head = (dh < db - 0.003) & (V[:, 2] > H[2] - 0.03)
mt_ = np.clip(((mir(Hl) - sdf.MOUTH_FRONT) @ (sdf.MOUTH_BACK - sdf.MOUTH_FRONT)) / np.sum((sdf.MOUTH_BACK - sdf.MOUTH_FRONT) ** 2), 0, 1)
zline = (sdf.MOUTH_FRONT + mt_[:, None] * (sdf.MOUTH_BACK - sdf.MOUTH_FRONT))[:, 2]
jaw = in_head & (Hl[:, 2] < zline) & (Hl[:, 1] < H[1] - 0.05)
blend = np.clip((db - dh) / 0.04, 0, 1)


def setw(i, wmap):
    for g in body.vertex_groups:
        try: g.remove([i])
        except RuntimeError: pass
    for n, w in wmap.items():
        if w > 1e-4: vg[n].add([i], float(w), 'REPLACE')


for i in range(len(V)):
    if in_head[i]:
        if jaw[i]: setw(i, {'Jaw': 1.0})
        elif blend[i] >= 1: setw(i, {'Head': 1.0})
        else: setw(i, {'Head': blend[i], 'Neck': 1 - blend[i]})
bad = [vg[n] for n in ('Head', 'Jaw', 'LeftEar', 'RightEar') if n in vg]
for i in np.where(~in_head)[0]:
    v = body.data.vertices[int(i)]
    mv = sum(g.weight for g in v.groups if body.vertex_groups[g.group] in bad)
    if mv > 0:
        for g in bad:
            try: g.remove([int(i)])
            except RuntimeError: pass
        vg['Neck'].add([int(i)], mv, 'ADD')

KD = cKDTree(V)
for ob, rule in parts:
    ob.parent = arm
    mod = ob.modifiers.new('Armature', 'ARMATURE'); mod.object = arm
    if rule == 'near':
        pv = np.array([v.co[:] for v in ob.data.vertices])
        _, nn = KD.query(pv)
        for vi, bi in enumerate(nn):
            for g in body.data.vertices[int(bi)].groups:
                name = body.vertex_groups[g.group].name
                gg = ob.vertex_groups.get(name) or ob.vertex_groups.new(name=name)
                gg.add([vi], g.weight, 'REPLACE')
    else:
        g = ob.vertex_groups.new(name=PREFIX + rule)
        g.add(list(range(len(ob.data.vertices))), 1.0, 'REPLACE')
    for p in ob.data.polygons: p.use_smooth = True


def mat(name, color, rough, emit=None, strength=1.0, metal=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    bs = m.node_tree.nodes['Principled BSDF']
    bs.inputs['Base Color'].default_value = (*color, 1); bs.inputs['Roughness'].default_value = rough
    bs.inputs['Metallic'].default_value = metal
    if emit is not None:
        bs.inputs['Emission Color'].default_value = (*emit, 1); bs.inputs['Emission Strength'].default_value = strength
    return m


MATS = {}


def shared(name, *a, **k):
    if name not in MATS: MATS[name] = mat(name, *a, **k)
    return MATS[name]


FURCOL = {'Fur_Rust': RUST, 'Fur_Soot': SOOT, 'Fur_Dirty': DIRTY}
for ob, rule in parts:
    n = ob.name
    if n.startswith('Teeth'): ob.data.materials.append(shared('Teeth_Bone', (0.34, 0.27, 0.13), 0.55))
    elif n.startswith('Tusk'): ob.data.materials.append(shared('Tusk_Ivory', (0.38, 0.31, 0.18), 0.5))
    elif n.startswith('Eye'): ob.data.materials.append(shared('Eye', (0.02, 0.003, 0.0), 0.08, emit=(0.3, 0.03, 0.005), strength=0.6))
    elif n.startswith('Claw'): ob.data.materials.append(shared('Claw_Horn', (0.03, 0.025, 0.02), 0.45))
    elif n.startswith('Stone'): ob.data.materials.append(shared('Stone_Shard', (0.012, 0.016, 0.012), 0.12, emit=(0.06, 0.11, 0.02), strength=0.45, metal=0.35))
    elif n.startswith('Fur_'): ob.data.materials.append(shared(n.split('.')[0], tuple(FURCOL[n.split('.')[0]] * 0.9), 0.85))

ears = [ob for ob, r in parts if ob.name.startswith('Ear')]
claws_o = [ob for ob, r in parts if ob.name.startswith('Claw')]
groups = [[ob for ob, r in parts if ob.name.startswith(pre)] for pre in ('TeethUp', 'TeethLow')]
for grp, pre in zip(groups, ('TeethUp', 'TeethLow')):
    activate(grp[0])
    for o in grp[1:]: o.select_set(True)
    bpy.ops.object.join(); grp[0].name = pre
activate(claws_o[0])
for o in claws_o[1:]: o.select_set(True)
bpy.ops.object.join(); claws_o[0].name = 'Claws'

# orecchie nella pelle per la cottura (interno rosato), poi di nuovo a parte
activate(body)
for o in ears: o.select_set(True)
bpy.ops.object.join()
body.name = 'Body'
me = body.data
V2 = np.array([v.co[:] for v in me.vertices])
_, nn = KD.query(V2)
C2 = C[nn]
gi = {body.vertex_groups[n].index for n in (PREFIX + 'LeftEar', PREFIX + 'RightEar') if n in body.vertex_groups}
for i, v in enumerate(me.vertices):
    if any(g.group in gi and g.weight > 0.99 for g in v.groups):
        C2[i] = C2[i] * 0.55 + np.array([0.1, 0.035, 0.035]) * 0.45
ca = me.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
for i, cc in enumerate(C2): ca.data[i].color = (*cc, 1)

activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(55), island_margin=0.004)
bpy.ops.object.mode_set(mode='OBJECT')
img = bpy.data.images.new('Skin_BaseColor', TEX, TEX)
skin = bpy.data.materials.new('Skin'); skin.use_nodes = True
nt = skin.node_tree; bs = nt.nodes['Principled BSDF']
attr = nt.nodes.new('ShaderNodeAttribute'); attr.attribute_name = 'Col'
# pelo: striature fini nel verso del pelo (all'indietro e in basso)
tc = nt.nodes.new('ShaderNodeTexCoord'); mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 0.15, 0.4)
hair = nt.nodes.new('ShaderNodeTexNoise'); hair.inputs['Scale'].default_value = 700.0; hair.inputs['Detail'].default_value = 2
hr = nt.nodes.new('ShaderNodeMapRange'); hr.inputs['From Min'].default_value = 0.42; hr.inputs['From Max'].default_value = 0.62
hr.inputs['To Min'].default_value = 1.08; hr.inputs['To Max'].default_value = 0.7
nt.links.new(tc.outputs['Object'], mp.inputs['Vector']); nt.links.new(mp.outputs['Vector'], hair.inputs['Vector']); nt.links.new(hair.outputs['Fac'], hr.inputs['Value'])
mix = nt.nodes.new('ShaderNodeVectorMath'); mix.operation = 'MULTIPLY'
nt.links.new(attr.outputs['Color'], mix.inputs[0]); nt.links.new(hr.outputs['Result'], mix.inputs[1])
emit = nt.nodes.new('ShaderNodeEmission'); nt.links.new(mix.outputs['Vector'], emit.inputs['Color'])
out = nt.nodes['Material Output']; nt.links.new(emit.outputs['Emission'], out.inputs['Surface'])
tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = img; nt.nodes.active = tn
body.data.materials.clear(); body.data.materials.append(skin)
sc.render.engine = 'CYCLES'; sc.cycles.samples = 1; sc.cycles.device = 'CPU'; sc.render.bake.margin = 8
bpy.ops.object.bake(type='EMIT')
for n in list(nt.nodes):
    if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL', 'TEX_IMAGE'): nt.nodes.remove(n)
nt.links.new(tn.outputs['Color'], bs.inputs['Base Color']); nt.links.new(bs.outputs['BSDF'], out.inputs['Surface'])
bs.inputs['Roughness'].default_value = 0.6
img.pack()

activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='DESELECT'); bpy.ops.object.mode_set(mode='OBJECT')
for v in body.data.vertices:
    v.select = any(g.group in gi and g.weight > 0.99 for g in v.groups)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_linked(); bpy.ops.mesh.separate(type='SELECTED'); bpy.ops.object.mode_set(mode='OBJECT')
earo = [o for o in bpy.context.selected_objects if o != body][0]
earo.name = 'Ears'
em = skin.copy(); em.name = 'Skin_Ears'
earo.data.materials.clear(); earo.data.materials.append(em)

for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath('pighcib.blend'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_skins=True,
                          export_animations=False, export_apply=False, export_yup=True,
                          export_image_format='JPEG', export_jpeg_quality=92)
print('esportato', OUT)
