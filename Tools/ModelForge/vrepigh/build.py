# Modello d'autore del Vrepigh: corpo dal campo di distanza (sdf.py), parti aggiunte (zanne, orecchie, occhi,
# criniera di setole, artigli, ciuffo della coda), scheletro del gioco, pesi, texture cotta, esportazione GLB.
import sys, os, json, math, random
import numpy as np
import bpy, bmesh
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)
import sdf, variants
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]


def arg(name, default=None):
    return ARGV[ARGV.index(name) + 1] if name in ARGV else default


VAR = arg('--variant', variants.BASE)
CFG = variants.get(VAR)
CHARS = os.path.abspath(os.path.join(HERE, '../../../Sandbox/GameData/Models/Characters/vrepigh'))
OUT = os.path.abspath(arg('--out', os.path.join(CHARS, 'model.glb') if VAR == variants.BASE else os.path.join(CHARS, 'variants', 'vrepighs_' + VAR + '.glb')))
TEX = 2048
random.seed(CFG.get('seed', 7)); np.random.seed(CFG.get('seed', 7))
sdf.configure(CFG)
cf = sdf.cf
R = json.load(open('rig.json'))
J = {k: np.array(v) for k, v in R['joints'].items()}
PAR = dict(R['parent'])
PAR.update({'Jaw': 'Head', 'LeftEar': 'Head', 'RightEar': 'Head', 'Tail': 'Hips'})
H, HS, HOFF = sdf.H, sdf.HS, sdf.HOFF


def hw(p):
    """punto nelle coordinate della testa (quelle di sdf.head) -> mondo"""
    return H + HOFF + (np.asarray(p, float) - H) * HS


def hl(off):
    o = np.array(off, float)
    u = -o[..., 1]                                    # in avanti: il grugno si allunga con 'snout'
    o[..., 1] = np.where(u > 0.10, -(0.10 + (u - 0.10) * cf('snout')), o[..., 1])
    return hw(H + o)


def head_local(V):
    """mondo -> coordinate della testa senza l'allungamento del grugno (come le vede sdf.head)"""
    P = H + (V - H - HOFF) / HS
    fw = np.clip(-(P[..., 1] - H[1]) - 0.10, 0, None); P[..., 1] += fw * (1 - 1 / cf('snout'))
    return P


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
key = json.dumps({k: v for k, v in CFG.items() if k in variants.SHAPE_KEYS}, sort_keys=True)
shp = f'shape_{VAR}.npz'
if not os.path.exists(shp) or str(np.load(shp)['key']) != key:
    v_, f_ = sdf.build(); np.savez(shp, v=v_, f=f_, key=key)
d = np.load(shp)
body = link('Body', d['v'], d['f'])
activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.remove_doubles(threshold=1e-5); bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')
m = body.modifiers.new('dec', 'DECIMATE'); m.ratio = 0.17
bpy.ops.object.modifier_apply(modifier='dec')
m = body.modifiers.new('sm', 'SMOOTH'); m.factor = 0.6; m.iterations = 5
bpy.ops.object.modifier_apply(modifier='sm')
for p in body.data.polygons: p.use_smooth = True
print('corpo', len(body.data.vertices), len(body.data.polygons))

bm = bmesh.new(); bm.from_mesh(body.data); bm.faces.ensure_lookup_table()
BVH = BVHTree.FromBMesh(bm)


def surf(p):
    loc, nrm, _, _ = BVH.find_nearest(Vector(p))
    return np.array(loc), np.array(nrm)


# ------------------------------------------------------------------ parti
def tube(name, pts, r0, r1=0.0, sides=10, rings=None):
    """tubo lungo una polilinea, raggio da r0 a r1, punta chiusa"""
    pts = [np.asarray(p, float) for p in pts]
    # ricampiona con una spline di Catmull-Rom
    P = []
    seg = rings or 6
    ext = [pts[0] * 2 - pts[1]] + pts + [pts[-1] * 2 - pts[-2]]
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for k in range(seg):
            t = k / seg
            P.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    P.append(pts[-1]); P = np.array(P)
    n = len(P)
    T = np.gradient(P, axis=0); T /= np.linalg.norm(T, axis=1)[:, None]
    up = np.array([0, 0, 1.0]) if abs(T[0][2]) < 0.9 else np.array([1.0, 0, 0])
    V, F = [], []
    nprev = np.cross(T[0], up); nprev /= np.linalg.norm(nprev)
    for i in range(n):
        nn = nprev - T[i] * (nprev @ T[i]); nn /= np.linalg.norm(nn); nprev = nn
        bb = np.cross(T[i], nn)
        s = i / (n - 1)
        r = r0 + (r1 - r0) * s ** 1.2
        if i == n - 1: r = max(r1, 0.0)
        for k in range(sides):
            a = 2 * math.pi * k / sides
            V.append(P[i] + r * (math.cos(a) * nn + math.sin(a) * bb))
    for i in range(n - 1):
        for k in range(sides):
            a, b = i * sides + k, i * sides + (k + 1) % sides
            F.append((a, b, b + sides, a + sides))
    V.append(P[0]); c = len(V) - 1
    for k in range(sides): F.append((c, (k + 1) % sides, k))
    return link(name, V, F)


parts = []   # (oggetto, regola pesi)

# zanne inferiori: dalla mascella verso l'alto, in fuori e indietro
for s in (1, -1):
    side = 'L' if s > 0 else 'R'
    tl, tr, curl = cf('tusk'), cf('tusk_r'), cf('tusk_curl')
    b = hl([0.043 * s, -0.285, -0.03])
    pts = [b, b + np.array([0.016 * s, -0.018, 0.065]) * HS * tl, b + np.array([0.055 * s, -0.004 * curl, 0.125]) * HS * tl,
           b + np.array([0.085 * s, 0.045 * curl, 0.155 - 0.02 * (curl - 1)]) * HS * tl]
    r1 = 0.0015
    if CFG.get('tusk_broken') == side:                # zanna spezzata: moncone smussato
        pts = pts[:2] + [pts[1] + (pts[2] - pts[1]) * 0.45]; r1 = 0.021 * HS * tr * 0.55
    parts.append((tube('Tusk_' + side, pts, 0.021 * HS * tr, r1, 12, 6), 'Jaw'))
    # zanne superiori, piu' corte, che escono dal labbro e si piegano verso l'alto
    tu = cf('tusk_up')
    if tu > 0.05:
        b = hl([0.04 * s, -0.255, 0.0])
        pts = [b, b + np.array([0.022 * s, -0.004, -0.005]) * HS * tu, b + np.array([0.04 * s, 0.006, 0.025]) * HS * tu,
               b + np.array([0.046 * s, 0.022, 0.05]) * HS * tu]
        parts.append((tube('TuskUp_' + side, pts, 0.011 * HS * min(tu, 1.4) * tr, 0.001, 10, 5), 'Head'))


# orecchie: foglie appuntite, un poco a coppa, dritte e piegate in fuori
def ear(s):
    base = hl([0.072 * s, 0.005, 0.165])
    dr = cf('ear_droop', 0.0)                                                # orecchie che ricadono in fuori
    ax = np.array([(0.55 + 0.6 * dr) * s, 0.35 + 0.2 * dr, 0.78 - 1.1 * dr]); ax /= np.linalg.norm(ax)        # lunghezza
    wd = np.array([0.25 * s, -1.0, 0.25]); wd -= ax * (wd @ ax); wd /= np.linalg.norm(wd)   # larghezza
    nn = np.cross(ax, wd) * s
    Lh, W = 0.16 * HS * cf('ear'), 0.09 * HS * cf('ear')
    if CFG.get('ear_torn') == ('L' if s > 0 else 'R'): Lh *= 0.62          # orecchio mozzato
    nu, nv = 9, 7
    V, F = [], []
    for side in (0, 1):
        for i in range(nu):
            u = i / (nu - 1)
            w = W * (math.sin(math.pi * min(u * 1.15, 1.0)) ** 0.7) * (1 - u) ** 0.15 * 0.5 + 0.004
            for j in range(nv):
                v = j / (nv - 1) * 2 - 1
                cup = (1 - v * v) * 0.018 * HS * math.sin(math.pi * u)    # coppa
                tip = 0.02 * HS * u * u                                     # punta piegata in fuori
                p = base + ax * (Lh * u) + wd * (w * v) + nn * (cup * (1 if side == 0 else 1) - tip + (0.006 if side else -0.006) * (1 - u))
                V.append(p)
    off = nu * nv
    for i in range(nu - 1):
        for j in range(nv - 1):
            a = i * nv + j
            F.append((a, a + 1, a + nv + 1, a + nv))
            F.append((off + a + nv, off + a + nv + 1, off + a + 1, off + a))
    for i in range(nu - 1):          # bordi
        for j in (0, nv - 1):
            a, b = i * nv + j, (i + 1) * nv + j
            F.append((a, b, off + b, off + a) if j == 0 else (b, a, off + a, off + b))
    for j in range(nv - 1):
        a = j
        F.append((a + 1, a, off + a, off + a + 1))
    return link('Ear_L' if s > 0 else 'Ear_R', V, F)


for s in (1, -1):
    parts.append((ear(s), 'LeftEar' if s > 0 else 'RightEar'))

# occhi piccoli e infossati
for s in (1, -1):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=12, ring_count=8, radius=0.016 * HS, location=tuple(hl((sdf.EYE - H) * [s, 1, 1] + [0, -0.004, 0])))
    o = bpy.context.active_object; o.name = 'Eye_L' if s > 0 else 'Eye_R'
    parts.append((o, 'Head'))


# artigli: coni corti in punta alle dita
def claws():
    objs = []
    for side, pre in ((1, 'Left'), (-1, 'Right')):
        for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
            b = J[pre + 'Hand' + f + '2']; c = J[pre + 'Hand' + f + '3']
            tip = c + (c - b) * 0.9
            dirv = (c - b) / np.linalg.norm(c - b)
            loc, nrm = surf(tip + dirv * 0.03)
            p0 = loc - dirv * 0.006
            down = np.array([0, 0, -1.0])
            pts = [p0, p0 + dirv * 0.014 + down * 0.002, p0 + dirv * 0.026 + down * 0.008]
            objs.append(tube('Claw', pts, 0.008, 0.0008, 8, 3))
    return objs


for o in claws():
    parts.append((o, 'near'))


# criniera: setole lungo la cresta, dalla fronte alla schiena, e ciuffi sulle guance
def bristles():
    path_head = [hl([0, -0.19, 0.155]), hl([0, -0.12, 0.185]), hl([0, -0.04, 0.2]), hl([0, 0.04, 0.19])]
    path_back = [np.array([0, 0.03, 1.53]), np.array([0, 0.11, 1.47]), np.array([0, 0.19, 1.36]),
                 np.array([0, 0.22, 1.22]), np.array([0, 0.21, 1.08]), np.array([0, 0.17, 0.95])]
    path = path_head + path_back
    pts = []
    for i in range(len(path) - 1):
        for k in range(20):
            t = k / 14
            pts.append((path[i] * (1 - t) + path[i + 1] * t, path[i + 1] - path[i], i / (len(path) - 1)))
    V, F = [], []
    for p, tg, u in pts:
        tg = tg / np.linalg.norm(tg)
        hump = math.exp(-((u - 0.5) / 0.2) ** 2)
        spread = (0.025 + 0.06 * hump) * cf('mane_spread')
        for _ in range(int(round(4 * cf('mane_n')))):
            q = p + np.array([random.uniform(-spread, spread), random.uniform(-0.01, 0.01), 0])
            loc, n = surf(q)
            L = (0.045 + 0.085 * hump) * random.uniform(0.7, 1.2) * (0.6 if u > 0.85 else 1.0) * cf('mane')
            dvec = n * 0.55 + tg * 0.85 + np.random.normal(0, 0.18, 3)
            dvec /= np.linalg.norm(dvec)
            r = 0.0055 + 0.003 * hump
            spike(V, F, loc - n * 0.004, dvec, L, r)
    # guance: ciuffi che scendono all'indietro
    for s in (1, -1):
        for _ in range(int(45 * cf('cheek'))):
            q = hl([0.1 * s + random.uniform(-0.01, 0.01), random.uniform(-0.12, -0.01), random.uniform(-0.06, 0.02)])
            loc, n = surf(q)
            dvec = n * 0.5 + np.array([0.2 * s, 0.8, -0.45]) + np.random.normal(0, 0.15, 3); dvec /= np.linalg.norm(dvec)
            spike(V, F, loc - n * 0.003, dvec, random.uniform(0.04, 0.07) * cf('cheek_len'), 0.0045)
    return link('Mane', V, F)


def spike(V, F, base, dirv, L, r, sides=5):
    """setola: cono con un anello a meta', piegato appena verso il basso"""
    up = np.array([0, 0, 1.0]) if abs(dirv[2]) < 0.9 else np.array([1.0, 0, 0])
    a = np.cross(dirv, up); a /= np.linalg.norm(a); b = np.cross(dirv, a)
    i0 = len(V)
    sag = np.array([0, 0, -L * 0.12])
    for rr, f, dz in ((r, 0.0, 0.0), (r * 0.55, 0.5, 0.3)):
        c = base + dirv * L * f + sag * dz
        for k in range(sides):
            t = 2 * math.pi * k / sides
            V.append(c + rr * (math.cos(t) * a + math.sin(t) * b))
    V.append(base + dirv * L + sag)
    tip = len(V) - 1
    for k in range(sides):
        k1 = (k + 1) % sides
        F.append((i0 + k, i0 + k1, i0 + sides + k1, i0 + sides + k))
        F.append((i0 + sides + k, i0 + sides + k1, tip))


parts.append((bristles(), 'near'))

# coda: esce sopra il cinturone, sottile, con un ciuffo in fondo (osso Tail alla radice)
tb, _ = surf(np.array([0, 0.25, 1.0]))
TAIL_BASE = tb + np.array([0, -0.01, 0])
tp = [TAIL_BASE, TAIL_BASE + [0, 0.06, 0.01], TAIL_BASE + [0, 0.11, -0.03], TAIL_BASE + [0, 0.13, -0.09]]
parts.append((tube('Tail', tp, 0.017, 0.007, 8, 4), 'Tail'))
t = tp[-1]
V, F = [], []
for _ in range(30):
    dvec = np.array([0, 0.6, -0.8]) + np.random.normal(0, 0.35, 3); dvec /= np.linalg.norm(dvec)
    spike(V, F, t + np.random.normal(0, 0.006, 3), dvec, random.uniform(0.05, 0.09), 0.004)
parts.append((link('TailTuft', V, F), 'Tail'))

# ------------------------------------------------------------------ scheletro
arm_data = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', arm_data); col.objects.link(arm)
activate(arm)
bpy.ops.object.mode_set(mode='EDIT')
EXTRA = {'Tail': TAIL_BASE, 'Jaw': hl([0, -0.06, 0.0]), 'LeftEar': hl([0.072, 0.005, 0.165]), 'RightEar': hl([-0.072, 0.005, 0.165])}
if VAR == variants.BASE:
    json.dump({k: [float(x) for x in v] for k, v in EXTRA.items()}, open('rig_extra.json', 'w'), indent=1)
else:
    EXTRA = {k: np.array(v) for k, v in json.load(open('rig_extra.json')).items()}
POS = dict(J); POS.update(EXTRA)
CH = {}
for c, p in PAR.items():
    if p: CH.setdefault(p, []).append(c)
eb = {}
PREFIX = 'mixamorig1:'
for name, p in POS.items():
    b = arm_data.edit_bones.new(PREFIX + name)
    b.head = Vector(p)
    kids = [k for k in CH.get(name, []) if k in POS and k not in ('Jaw', 'LeftEar', 'RightEar', 'Tail', 'LeftShoulder', 'RightShoulder', 'LeftUpLeg', 'RightUpLeg')]
    if name == 'Spine2': kids = ['Neck']
    if name == 'Hips': kids = ['Spine']
    if name == 'LeftHand' or name == 'RightHand': kids = [name + 'Middle1']
    if kids:
        tail = np.mean([POS[k] for k in kids], 0)
    elif name == 'Head':
        tail = p + np.array([0, 0, 0.18])
    elif name == 'Jaw':
        tail = hl([0, -0.28, -0.03])
    elif name.endswith('Ear'):
        tail = hl([0.12 * (1 if name.startswith('Left') else -1), 0.05, 0.27])
    elif name == 'Tail':
        tail = p + np.array([0, 0.12, -0.06])
    elif 'ToeBase' in name:
        tail = p + np.array([0, -0.07, 0])
    else:
        par = POS[PAR[name]]
        dv = p - par; tail = p + dv / max(np.linalg.norm(dv), 1e-6) * 0.025
    if np.linalg.norm(tail - p) < 1e-3: tail = p + np.array([0, 0, 0.02])
    b.tail = Vector(tail)
    eb[name] = b
for name, b in eb.items():
    pn = PAR.get(name)
    if pn and pn in eb: b.parent = eb[pn]
bpy.ops.object.mode_set(mode='OBJECT')
print('ossa', len(arm_data.bones))

# ------------------------------------------------------------------ pesi del corpo: calore di Blender, poi correzioni
activate(body); arm.select_set(True); bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
vg = {g.name.split(':')[-1]: g for g in body.vertex_groups}
print('gruppi', len(vg))

# testa e mascella definite dalla forma: dentro la testa = Head, sotto la linea della bocca = Jaw
V = np.array([v.co[:] for v in body.data.vertices])
Hl = head_local(V)          # coordinate della testa
dh = sdf.head(V); db = sdf.body(V)
in_head = (dh < db - 0.004) & (Hl[:, 1] < 0.0)
# bocca: sotto la retta dalla punta (y=-0.355, z=-0.022) all'angolo (y=-0.10, z=-0.005)
zline = -0.022 + (Hl[:, 1] + 0.355) / (0.255) * 0.017
jaw = in_head & (Hl[:, 2] < zline) & (Hl[:, 1] < -0.06)
blend = np.clip((db - dh) / 0.03, 0, 1)    # transizione morbida col collo


def setw(i, wmap):
    for g in body.vertex_groups:
        try: g.remove([i])
        except RuntimeError: pass
    for n, w in wmap.items():
        if w > 1e-4: vg[n].add([i], float(w), 'REPLACE')


for i in range(len(V)):
    if in_head[i]:
        t = blend[i]
        if jaw[i]: setw(i, {'Jaw': 1.0})
        elif t >= 1: setw(i, {'Head': 1.0})
        else:
            setw(i, {'Head': t, 'Neck': 1 - t})
# fuori dalla testa: niente peso su Head, Jaw, orecchie (gobba e garrese seguono il tronco)
bad = [vg[n] for n in ('Head', 'Jaw', 'LeftEar', 'RightEar') if n in vg]
for i in np.where(~in_head)[0]:
    v = body.data.vertices[int(i)]
    mv = 0.0
    for g in v.groups:
        if body.vertex_groups[g.group] in bad: mv += g.weight
    if mv > 0:
        for g in bad:
            try: g.remove([int(i)])
            except RuntimeError: pass
        vg['Neck'].add([int(i)], mv, 'ADD')

# ------------------------------------------------------------------ pesi delle parti
BV = np.array([v.co[:] for v in body.data.vertices])
from scipy.spatial import cKDTree
KD = cKDTree(BV)
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
    for p in ob.data.polygons: p.use_smooth = rule not in ('near',) or ob.name != 'Mane'

# ------------------------------------------------------------------ materiali
def mat(name, color, rough, metal=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    bs = m.node_tree.nodes['Principled BSDF']
    bs.inputs['Base Color'].default_value = (*color, 1)
    bs.inputs['Roughness'].default_value = rough
    bs.inputs['Metallic'].default_value = metal
    return m


for ob, rule in parts:
    n = ob.name
    if n.startswith('Tusk'): ob.data.materials.append(mat('Tusk_Ivory', tuple(CFG.get('tusk_col', (0.62, 0.55, 0.42))), 0.45))
    elif n.startswith('Eye'): ob.data.materials.append(mat('Eye', tuple(CFG.get('eye', (0.12, 0.02, 0.01))), 0.15))
    elif n.startswith('Ear'): pass
    elif n.startswith('Claw'): ob.data.materials.append(mat('Claw_Horn', (0.06, 0.05, 0.045), 0.4))
    elif n in ('Mane', 'TailTuft'): ob.data.materials.append(mat('Fur_Bristle', tuple(CFG.get('mane_col', (0.035, 0.028, 0.024))), 0.75))
    elif n == 'Tail': ob.data.materials.append(mat('Skin_Tail', tuple(np.array(CFG.get('base', (0.075, 0.055, 0.044))) * 0.65), 0.7))

# pelle: le orecchie si uniscono al corpo (stessa texture, pesi propri)
ears = [ob for ob, r in parts if ob.name.startswith('Ear')]
claws_o = [ob for ob, r in parts if ob.name.startswith('Claw')]
activate(claws_o[0])
for o in claws_o[1:]: o.select_set(True)
bpy.ops.object.join(); claws_o[0].name = 'Claws'

activate(body)
for o in ears: o.select_set(True)
bpy.ops.object.join()
body.name = 'Body'

# colore per vertice (dipinto qui), poi cotto in una texture
me = body.data
V = np.array([v.co[:] for v in me.vertices]); N = np.array([v.normal[:] for v in me.vertices])
Hl = head_local(V)
base = np.array(CFG.get('base', (0.075, 0.055, 0.044)))          # cuoio bruno-grigio
belly = np.array(CFG.get('belly_col', (0.12, 0.085, 0.068)))
snout = np.array(CFG.get('snout_col', (0.17, 0.105, 0.088)))
dark = np.array(CFG.get('dark', (0.035, 0.026, 0.021)))
hoof = np.array([0.022, 0.018, 0.016])
C = np.tile(base, (len(V), 1))
# ventre e gola piu' chiari (normali verso il basso/davanti nel torso)
tor = (V[:, 2] > 0.8) & (V[:, 2] < 1.3) & (np.abs(V[:, 0]) < 0.25)
k = np.clip(-N[:, 1] * 0.8 - N[:, 2] * 0.6, 0, 1) * tor
C = C * (1 - k[:, None] * 0.6) + belly * (k[:, None] * 0.6)
# schiena e gobba scure
k = np.clip(N[:, 1] * 0.9 + N[:, 2] * 0.5, 0, 1) * (V[:, 2] > 0.9)
C = C * (1 - k[:, None] * 0.55) + dark * (k[:, None] * 0.55)
# grugno, labbra, disco del naso
k = np.clip((-Hl[:, 1] - 0.2) / 0.12, 0, 1) * (np.linalg.norm(V - hl([0, -0.3, 0]), axis=1) < 0.25)
C = C * (1 - k[:, None]) + snout * k[:, None]
nose = np.clip(1 - np.abs(((Hl - sdf.NOSE) @ sdf.NOSE_N)) / 0.02, 0, 1) * (np.linalg.norm(Hl - sdf.NOSE, axis=1) < 0.07)
C = C * (1 - nose[:, None] * 0.6) + np.array([0.12, 0.07, 0.07]) * nose[:, None] * 0.6
# avambracci, mani e stinchi scuri, zoccoli quasi neri
fa = np.zeros(len(V))
for pre in ('Left', 'Right'):
    for a, b in (('ForeArm', 'Hand'), ('Leg', 'Foot')):
        A, B = J[pre + a], J[pre + b]
        t = np.clip(((V - A) @ (B - A)) / ((B - A) @ (B - A)), 0, 1.4)
        fa = np.maximum(fa, np.clip((t - 0.2) / 0.8, 0, 1) * (np.linalg.norm(V - (A + t[:, None] * (B - A)), axis=1) < 0.2))
    hd = np.linalg.norm(V - J[pre + 'Hand'], axis=1) < 0.17
    fa = np.maximum(fa, hd * 0.9)
C = C * (1 - fa[:, None] * 0.55) + dark * (fa[:, None] * 0.55)
hv = (V[:, 2] < 0.06) & (V[:, 1] < 0.02)
C[hv] = hoof
# orecchie: interno rosato
earv = np.zeros(len(V), bool)
for s in (1, -1):
    earv |= np.linalg.norm(V - hl([0.11 * s, 0.03, 0.23]), axis=1) < 0.1 * HS
C[earv] = C[earv] * 0.5 + np.array([0.3, 0.17, 0.15]) * 0.5
# striature dei giovani: bande chiare lungo i fianchi e la schiena
if cf('stripes', 0.0) > 0:
    ang = np.arctan2(V[:, 0], V[:, 1] - 0.02)
    band = (np.cos(ang * 9) > 0.55) & (V[:, 2] > 0.75) & (V[:, 2] < 1.45) & (np.abs(V[:, 0]) < 0.32)
    C[band] = C[band] * (1 - 0.5 * cf('stripes')) + np.array([0.16, 0.12, 0.09]) * 0.5 * cf('stripes')
# vecchiaia: grugno, arcate e guance ingrigiti
g = cf('grizzle', 0.0)
if g > 0:
    kk = np.clip((-Hl[:, 1] - 0.05) / 0.2, 0, 1) * (np.linalg.norm(V - hl([0, -0.2, 0.05]), axis=1) < 0.3)
    kk = np.maximum(kk, ((np.abs(Hl[:, 0]) > 0.06) & (Hl[:, 2] < 0.05) & (np.linalg.norm(V - hl([0, -0.1, 0]), axis=1) < 0.2)) * 0.7)
    grey = np.array([0.25, 0.23, 0.21])
    C = C * (1 - kk[:, None] * g) + grey * (kk[:, None] * g)
# cicatrici: solchi chiari e rosati su testa, spalle e braccia
rng = np.random.RandomState(CFG.get('seed', 7) + 3)
for _ in range(int(cf('scars', 0.0))):
    ctr = rng.choice(['head', 'chest', 'arm'], p=[0.45, 0.3, 0.25])
    if ctr == 'head': p0 = hl([rng.uniform(-0.09, 0.09), rng.uniform(-0.3, -0.05), rng.uniform(0.0, 0.16)])
    elif ctr == 'chest': p0 = np.array([rng.uniform(-0.25, 0.25), -0.22, rng.uniform(1.0, 1.35)])
    else:
        sgn = rng.choice([-1, 1]); p0 = J['LeftArm'] * [sgn, 1, 1] + (J['LeftForeArm'] - J['LeftArm']) * [sgn, 1, 1] * rng.uniform(0, 1.2)
    dvec = rng.normal(0, 1, 3); dvec /= np.linalg.norm(dvec)
    p0, _n = surf(p0); p1 = p0 + dvec * rng.uniform(0.06, 0.16)
    t = np.clip(((V - p0) @ (p1 - p0)) / ((p1 - p0) @ (p1 - p0)), 0, 1)
    dd = np.linalg.norm(V - (p0 + t[:, None] * (p1 - p0)), axis=1)
    kk = np.clip(1 - dd / 0.012, 0, 1)
    C = C * (1 - kk[:, None] * 0.8) + np.array([0.26, 0.13, 0.11]) * (kk[:, None] * 0.8)
# macchie e cicatrici: rumore a bassa frequenza
ph = np.random.rand(6, 3) * 10
nz = sum(np.sin(V @ (np.random.normal(0, 9, 3)) + ph[i, 0]) for i in range(6)) / 6
C *= (1 + 0.18 * nz)[:, None]
C = np.clip(C, 0, 1)
ca = me.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
for i, c in enumerate(C): ca.data[i].color = (*c, 1)

# UV e cottura
activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(55), island_margin=0.004)
bpy.ops.object.mode_set(mode='OBJECT')
img = bpy.data.images.new('Skin_BaseColor', TEX, TEX)
skin = bpy.data.materials.new('Skin'); skin.use_nodes = True
nt = skin.node_tree; bs = nt.nodes['Principled BSDF']
attr = nt.nodes.new('ShaderNodeAttribute'); attr.attribute_name = 'Col'
# pori e rughe fini: rumore moltiplicato sul colore
noise = nt.nodes.new('ShaderNodeTexNoise'); noise.inputs['Scale'].default_value = 180.0; noise.inputs['Detail'].default_value = 6
ramp = nt.nodes.new('ShaderNodeMapRange'); ramp.inputs['From Min'].default_value = 0.3; ramp.inputs['From Max'].default_value = 0.7
ramp.inputs['To Min'].default_value = 0.78; ramp.inputs['To Max'].default_value = 1.1
mix = nt.nodes.new('ShaderNodeVectorMath'); mix.operation = 'MULTIPLY'
nt.links.new(noise.outputs['Fac'], ramp.inputs['Value'])
tc = nt.nodes.new('ShaderNodeTexCoord'); mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 1.0, 0.12)
hair = nt.nodes.new('ShaderNodeTexNoise'); hair.inputs['Scale'].default_value = 420.0; hair.inputs['Detail'].default_value = 2
hr = nt.nodes.new('ShaderNodeMapRange'); hr.inputs['From Min'].default_value = 0.45; hr.inputs['From Max'].default_value = 0.62
hr.inputs['To Min'].default_value = 1.0; hr.inputs['To Max'].default_value = 0.55
nt.links.new(tc.outputs['Object'], mp.inputs['Vector']); nt.links.new(mp.outputs['Vector'], hair.inputs['Vector']); nt.links.new(hair.outputs['Fac'], hr.inputs['Value'])
mul2 = nt.nodes.new('ShaderNodeMath'); mul2.operation = 'MULTIPLY'
nt.links.new(ramp.outputs['Result'], mul2.inputs[0]); nt.links.new(hr.outputs['Result'], mul2.inputs[1])
nt.links.new(attr.outputs['Color'], mix.inputs[0]); nt.links.new(mul2.outputs['Value'], mix.inputs[1])
emit = nt.nodes.new('ShaderNodeEmission'); nt.links.new(mix.outputs['Vector'], emit.inputs['Color'])
out = nt.nodes['Material Output']
nt.links.new(emit.outputs['Emission'], out.inputs['Surface'])
tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = img; nt.nodes.active = tn
body.data.materials.clear(); body.data.materials.append(skin)
sc.render.engine = 'CYCLES'; sc.cycles.samples = 1; sc.cycles.device = 'CPU'
sc.render.bake.margin = 8
bpy.ops.object.bake(type='EMIT')
img.filepath_raw = os.path.abspath('Skin_BaseColor.png'); img.file_format = 'PNG'; img.save()
# materiale finale: texture -> colore base
for n in list(nt.nodes):
    if n.type not in ('BSDF_PRINCIPLED', 'OUTPUT_MATERIAL', 'TEX_IMAGE'): nt.nodes.remove(n)
nt.links.new(tn.outputs['Color'], bs.inputs['Base Color'])
nt.links.new(bs.outputs['BSDF'], out.inputs['Surface'])
bs.inputs['Roughness'].default_value = 0.72
img.pack()

# orecchie di nuovo a parte (stessa texture, materiale "Skin_Ears"): per la forgia non sono cranio
activate(body)
gi = {body.vertex_groups[n].index for n in (PREFIX + 'LeftEar', PREFIX + 'RightEar') if n in body.vertex_groups}
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='DESELECT'); bpy.ops.object.mode_set(mode='OBJECT')
for v in body.data.vertices:
    v.select = any(g.group in gi and g.weight > 0.99 for g in v.groups)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_linked(); bpy.ops.mesh.separate(type='SELECTED'); bpy.ops.object.mode_set(mode='OBJECT')
earo = [o for o in bpy.context.selected_objects if o != body][0]
earo.name = 'Ears'
em = skin.copy(); em.name = 'Skin_Ears'
earo.data.materials.clear(); earo.data.materials.append(em)
print('orecchie', len(earo.data.vertices))

# ------------------------------------------------------------------ esporta
for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(f'vrepigh_{VAR}.blend'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=os.path.abspath(OUT), export_format='GLB', use_selection=True,
                          export_skins=True, export_animations=False, export_apply=False, export_yup=True,
                          export_image_format='JPEG', export_jpeg_quality=92)
print('esportato', OUT)
