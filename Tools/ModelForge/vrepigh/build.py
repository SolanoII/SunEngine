# Modello d'autore del Vrepigh: razza violenta, affamata e bellicosa.
# Corpo dal campo di distanza (sdf.py) in due risoluzioni: quella alta (5 mm) porta muscoli, solchi, costole e rughe
# e viene dipinta per vertice (cicatrici, vene, verruche, sangue rappreso, fango, pitture di guerra); quella bassa
# va nel gioco e riceve tutto in texture (colore, normali, ruvidita') cotto dall'alta. Parti aggiunte: zanne e
# denti, occhi, orecchie sbrindellate, criniera di setole, artigli uncinati, coda. Scheletro del gioco, pesi, GLB.
import sys, os, json, math, random
import numpy as np
import bpy, bmesh
from mathutils import Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)
import sdf, variants
sys.path.insert(0, os.path.join(HERE, '..'))
import photo_lib
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]


def arg(name, default=None):
    return ARGV[ARGV.index(name) + 1] if name in ARGV else default


VAR = arg('--variant', variants.BASE)
CFG = variants.get(VAR)
CHARS = os.path.abspath(os.path.join(HERE, '../../../Sandbox/GameData/Models/Characters/vrepigh'))
OUT = os.path.abspath(arg('--out', os.path.join(CHARS, 'model.glb') if VAR == variants.BASE else os.path.join(CHARS, 'variants', 'vrepighs_' + VAR + '.glb')))
TEXDIR = arg('--textures')                      # cartella dove copiare anche le texture (per chi le vuole a parte)
FAST = '--fast' in ARGV
TEX = 1024 if FAST else 2048
STEP = 0.007 if FAST else 0.005
LOW_FACES = 42000
random.seed(CFG.get('seed', 7)); np.random.seed(CFG.get('seed', 7))
sdf.configure(CFG)
cf = sdf.cf
R = json.load(open('rig.json'))
J = {k: np.array(v) for k, v in R['joints'].items()}
PAR = dict(R['parent'])
PAR.update({'Jaw': 'Head', 'LeftEar': 'Head', 'RightEar': 'Head', 'Tail': 'Hips'})
H, HS, HOFF = sdf.H, sdf.HS, sdf.HOFF
PREFIX = 'mixamorig1:'


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


def sstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
col = sc.collection


def link(name, verts, faces):
    me = bpy.data.meshes.new(name); me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces]); me.update()
    ob = bpy.data.objects.new(name, me); col.objects.link(ob)
    return ob


def activate(ob):
    for o in bpy.data.objects:
        if o.name in bpy.context.view_layer.objects: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


def clean(ob):
    activate(ob)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=1e-5); bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode='OBJECT')


def apply_mod(ob, kind, **kw):
    activate(ob)
    m = ob.modifiers.new(kind.lower(), kind)
    for k, v in kw.items(): setattr(m, k, v)
    bpy.ops.object.modifier_apply(modifier=m.name)


# ------------------------------------------------------------------ corpo: alta e bassa risoluzione
import hashlib
key = json.dumps({k: v for k, v in CFG.items() if k in variants.SHAPE_KEYS}, sort_keys=True) + f'|{STEP}|' + hashlib.md5(open('sdf.py', 'rb').read()).hexdigest()[:10]
shp = f'shape_{VAR}.npz'
if not os.path.exists(shp) or str(np.load(shp)['key']) != key:
    v_, f_ = sdf.build(STEP); np.savez(shp, v=v_, f=f_, key=key)
d = np.load(shp)
hi = link('BodyHi', d['v'], d['f'])
clean(hi)
apply_mod(hi, 'SMOOTH', factor=0.5, iterations=1)
for p in hi.data.polygons: p.use_smooth = True
body = hi.copy(); body.data = hi.data.copy(); body.name = 'Body'; col.objects.link(body)
apply_mod(body, 'DECIMATE', ratio=min(1.0, LOW_FACES / len(body.data.polygons)))
apply_mod(body, 'SMOOTH', factor=0.4, iterations=1)
for p in body.data.polygons: p.use_smooth = True
print('corpo: alto', len(hi.data.polygons), 'basso', len(body.data.polygons), flush=True)

bm = bmesh.new(); bm.from_mesh(hi.data); bm.faces.ensure_lookup_table()
BVH = BVHTree.FromBMesh(bm)


def surf(p):
    loc, nrm, _, _ = BVH.find_nearest(Vector(p))
    return np.array(loc), np.array(nrm)


# ------------------------------------------------------------------ parti
def tube(name, pts, r0, r1=0.0, sides=10, rings=None, flat=1.0):
    """tubo lungo una polilinea (Catmull-Rom), raggio da r0 a r1, punta chiusa. UV: u attorno, v lungo (0 radice, 1 punta).
    flat < 1 schiaccia la sezione (zanne a lama)"""
    pts = [np.asarray(p, float) for p in pts]
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
    V, F, UV = [], [], []
    nprev = np.cross(T[0], up); nprev /= np.linalg.norm(nprev)
    for i in range(n):
        nn = nprev - T[i] * (nprev @ T[i]); nn /= np.linalg.norm(nn); nprev = nn
        bb = np.cross(T[i], nn)
        s = i / (n - 1)
        r = r0 + (r1 - r0) * s ** 1.2
        if i == n - 1: r = max(r1, 0.0)
        for k in range(sides + 1):
            a = 2 * math.pi * k / sides
            V.append(P[i] + r * (math.cos(a) * nn * flat + math.sin(a) * bb)); UV.append((k / sides, s))
    for i in range(n - 1):
        for k in range(sides):
            a, b = i * (sides + 1) + k, i * (sides + 1) + k + 1
            F.append((a, b, b + sides + 1, a + sides + 1))
    V.append(P[0]); UV.append((0.5, 0.0)); c = len(V) - 1
    for k in range(sides): F.append((c, k + 1, k))
    ob = link(name, V, F)
    uvl = ob.data.uv_layers.new(name='UVMap')
    for poly in ob.data.polygons:
        for li in poly.loop_indices:
            uvl.data[li].uv = UV[ob.data.loops[li].vertex_index]
    return ob


parts = []   # (oggetto, regola pesi)

# zanne inferiori: grosse, a lama, escono dalla mandibola e si piegano in alto, in fuori e indietro
for s in (1, -1):
    side = 'L' if s > 0 else 'R'
    tl, tr, curl = cf('tusk'), cf('tusk_r'), cf('tusk_curl')
    b = hl([0.043 * s, -0.287, -0.032])
    pts = [b, b + np.array([0.014 * s, -0.02, 0.06]) * HS * tl, b + np.array([0.05 * s, -0.012 * curl, 0.12]) * HS * tl,
           b + np.array([0.085 * s, 0.035 * curl, 0.158 - 0.02 * (curl - 1)]) * HS * tl,
           b + np.array([0.098 * s, 0.075 * curl, 0.16 - 0.04 * (curl - 1)]) * HS * tl]
    r1 = 0.0012
    if CFG.get('tusk_broken') == side:                # zanna spezzata: moncone scheggiato
        pts = pts[:2] + [pts[1] + (pts[2] - pts[1]) * 0.5]; r1 = 0.024 * HS * tr * 0.55
    parts.append((tube('Tusk_' + side, pts, 0.024 * HS * tr, r1, 14, 7, flat=0.78), 'Jaw'))
    # zanne superiori (affilatoi), piu' corte, escono dal labbro gonfio e si piegano in alto
    tu = cf('tusk_up')
    if tu > 0.05:
        b = hl([0.046 * s, -0.262, 0.006])
        pts = [b, b + np.array([0.024 * s, -0.006, -0.004]) * HS * tu, b + np.array([0.044 * s, 0.004, 0.026]) * HS * tu,
               b + np.array([0.05 * s, 0.024, 0.056]) * HS * tu]
        parts.append((tube('TuskUp_' + side, pts, 0.013 * HS * min(tu, 1.4) * tr, 0.001, 10, 5), 'Head'))

# denti: file di zanne corte e storte nel labbro di sopra e nella mandibola, scoperte dal ringhio
rng_t = np.random.RandomState(CFG.get('seed', 7) + 11)
for jaw_row in (0, 1):
    for s in (1, -1):
        for k in range(4):
            yl = -0.365 + k * 0.058 + rng_t.uniform(-0.01, 0.01)
            z_up, z_lo, t = sdf.mouth_lines(np.array(yl))
            xw = 0.022 + 0.034 * float(t) + rng_t.uniform(-0.003, 0.003)
            ln = (0.03 - 0.01 * float(t)) * rng_t.uniform(0.6, 1.3) * (1.3 if k == 0 else 1.0)
            if jaw_row == 0:
                lean_ = rng_t.uniform(-0.008, 0.008); b = hl([xw * s, yl, float(z_up) + 0.012]); tip = hl([(xw + 0.004) * s + lean_, yl - 0.004 + lean_, float(z_up) + 0.012 - ln - 0.008])
            else:
                lean_ = rng_t.uniform(-0.008, 0.008); b = hl([xw * s, yl, float(z_lo) - 0.012]); tip = hl([(xw + 0.004) * s + lean_, yl - 0.006 + lean_, float(z_lo) - 0.012 + ln + 0.008])
            mid = (b + tip) / 2 + np.array([0, -0.003, 0])
            o = tube('Tooth', [b, mid, tip], 0.0085 * HS * (1.2 if k == 0 else 1.0), 0.0012, 8, 3)
            parts.append((o, 'Head' if jaw_row == 0 else 'Jaw'))


# orecchie: foglie appuntite, piu' spesse, con l'orlo strappato a morsi
def ear(s):
    base = hl([0.072 * s, 0.005, 0.165])
    dr = cf('ear_droop', 0.0)
    ax = np.array([(0.75 + 0.5 * dr) * s, 0.55 + 0.2 * dr, 0.45 - 1.0 * dr]); ax /= np.linalg.norm(ax)
    wd = np.array([0.25 * s, -1.0, 0.25]); wd -= ax * (wd @ ax); wd /= np.linalg.norm(wd)
    nn = np.cross(ax, wd) * s
    Lh, W = 0.17 * HS * cf('ear'), 0.11 * HS * cf('ear')
    torn = CFG.get('ear_torn') == ('L' if s > 0 else 'R')
    if torn: Lh *= 0.62
    rs = np.random.RandomState(CFG.get('seed', 7) + (5 if s > 0 else 9))
    notch_u = rs.uniform(0.35, 0.8, 2)
    nu, nv = 11, 7
    V, F = [], []
    for side in (0, 1):
        for i in range(nu):
            u = i / (nu - 1)
            w = W * (math.sin(math.pi * min(u * 1.12, 1.0)) ** 0.7) * (1 - u) ** 0.12 * 0.5 + 0.004
            for j in range(nv):
                v = j / (nv - 1) * 2 - 1
                ww = w
                if v > 0.6 and any(abs(u - q) < 0.06 for q in notch_u): ww *= 0.55          # morsi sull'orlo
                cup = (1 - v * v) * 0.016 * HS * math.sin(math.pi * u)
                tip = 0.024 * HS * u * u
                p = base + ax * (Lh * u) + wd * (ww * v) + nn * (cup - tip + (0.008 if side else -0.008) * (1 - u * 0.7))
                V.append(p)
    off = nu * nv
    for i in range(nu - 1):
        for j in range(nv - 1):
            a = i * nv + j
            F.append((a, a + 1, a + nv + 1, a + nv))
            F.append((off + a + nv, off + a + nv + 1, off + a + 1, off + a))
    for i in range(nu - 1):
        for j in (0, nv - 1):
            a, b = i * nv + j, (i + 1) * nv + j
            F.append((a, b, off + b, off + a) if j == 0 else (b, a, off + a, off + b))
    for j in range(nv - 1):
        a = j
        F.append((a + 1, a, off + a, off + a + 1))
    return link('Ear_L' if s > 0 else 'Ear_R', V, F)


for s in (1, -1):
    parts.append((ear(s), 'LeftEar' if s > 0 else 'RightEar'))

# occhi piccoli, infossati sotto l'arcata
for s in (1, -1):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=14, ring_count=9, radius=0.0135 * HS, location=tuple(hl((sdf.EYE - H) * [s, 1, 1] + [-0.002 * s, 0.003, -0.001])))
    o = bpy.context.active_object; o.name = 'Eye_L' if s > 0 else 'Eye_R'
    parts.append((o, 'Head'))


# artigli: uncini lunghi e neri in punta alle dita
def claws():
    objs = []
    for side, pre in ((1, 'Left'), (-1, 'Right')):
        for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
            b = J[pre + 'Hand' + f + '2']; c = J[pre + 'Hand' + f + '3']
            tip = c + (c - b) * 0.8
            dirv = (c - b) / np.linalg.norm(c - b)
            loc, nrm = surf(tip + dirv * 0.03)
            p0 = loc - dirv * 0.012
            down = np.array([0, 0, -1.0])
            L_ = 0.042 * cf('claw', 1.0)
            pts = [p0, p0 + dirv * L_ * 0.45 + down * L_ * 0.05, p0 + dirv * L_ * 0.8 + down * L_ * 0.25, p0 + dirv * L_ * 0.95 + down * L_ * 0.55]
            objs.append(tube('Claw', pts, 0.0105, 0.0008, 8, 3, flat=0.75))
    return objs


for o in claws():
    parts.append((o, 'near'))


def spike(V, F, UV, base, dirv, L, r, uu, sides=5):
    """setola: cono con un anello a meta', piegato appena verso il basso. UV: u = setola, v lungo"""
    up = np.array([0, 0, 1.0]) if abs(dirv[2]) < 0.9 else np.array([1.0, 0, 0])
    a = np.cross(dirv, up); a /= np.linalg.norm(a); b = np.cross(dirv, a)
    i0 = len(V)
    sag = np.array([0, 0, -L * 0.14])
    for rr, f, dz in ((r, 0.0, 0.0), (r * 0.5, 0.55, 0.35)):
        c = base + dirv * L * f + sag * dz
        for k in range(sides):
            t = 2 * math.pi * k / sides
            V.append(c + rr * (math.cos(t) * a + math.sin(t) * b)); UV.append((uu, f))
    V.append(base + dirv * L + sag); UV.append((uu, 1.0))
    tip = len(V) - 1
    for k in range(sides):
        k1 = (k + 1) % sides
        F.append((i0 + k, i0 + k1, i0 + sides + k1, i0 + sides + k))
        F.append((i0 + sides + k, i0 + sides + k1, tip))


def with_uv(ob, UV):
    uvl = ob.data.uv_layers.new(name='UVMap')
    for poly in ob.data.polygons:
        for li in poly.loop_indices:
            uvl.data[li].uv = UV[ob.data.loops[li].vertex_index]
    return ob


# criniera: cresta irta dalla fronte alla schiena, mantello di setole sulla gobba, ciuffi su guance e mento
def bristles():
    path_head = [hl([0, -0.2, 0.16]), hl([0, -0.12, 0.2]), hl([0, -0.04, 0.215]), hl([0, 0.04, 0.205])]
    path_back = [np.array([0, 0.03, 1.55]), np.array([0, 0.11, 1.49]), np.array([0, 0.19, 1.38]),
                 np.array([0, 0.22, 1.24]), np.array([0, 0.21, 1.1]), np.array([0, 0.17, 0.96])]
    path = path_head + path_back
    pts = []
    for i in range(len(path) - 1):
        for k in range(22):
            t = k / 22
            pts.append((path[i] * (1 - t) + path[i + 1] * t, path[i + 1] - path[i], i / (len(path) - 1)))
    V, F, UV = [], [], []
    for p, tg, u in pts:
        tg = tg / np.linalg.norm(tg)
        hump = math.exp(-((u - 0.5) / 0.22) ** 2)
        spread = (0.025 + 0.09 * hump) * cf('mane_spread')
        for _ in range(int(round(5 * cf('mane_n')))):
            q = p + np.array([random.uniform(-spread, spread), random.uniform(-0.012, 0.012), 0])
            loc, n = surf(q)
            L = (0.05 + 0.11 * hump) * random.uniform(0.6, 1.25) * (0.6 if u > 0.88 else 1.0) * cf('mane')
            dvec = n * 0.6 + tg * 0.8 + np.random.normal(0, 0.22, 3)
            dvec /= np.linalg.norm(dvec)
            spike(V, F, UV, loc - n * 0.005, dvec, L, 0.0055 + 0.0035 * hump, random.random())
    # mantello sulla gobba e sulle spalle: setole che cadono all'indietro e in fuori
    for _ in range(int(320 * cf('mane_n'))):
        q = np.array([random.uniform(-0.3, 0.3), random.uniform(0.0, 0.2), random.uniform(1.25, 1.52)])
        loc, n = surf(q)
        if loc[1] < -0.02: continue
        dvec = n * 0.5 + np.array([0.3 * np.sign(loc[0]), 0.5, -0.7]) + np.random.normal(0, 0.2, 3); dvec /= np.linalg.norm(dvec)
        spike(V, F, UV, loc - n * 0.004, dvec, random.uniform(0.05, 0.1) * cf('mane'), 0.0045, random.random())
    # guance e mento: ciuffi irti che scendono all'indietro
    for s in (1, -1):
        for _ in range(int(60 * cf('cheek'))):
            q = hl([0.11 * s + random.uniform(-0.012, 0.012), random.uniform(-0.13, -0.01), random.uniform(-0.07, 0.03)])
            loc, n = surf(q)
            dvec = n * 0.5 + np.array([0.25 * s, 0.8, -0.45]) + np.random.normal(0, 0.18, 3); dvec /= np.linalg.norm(dvec)
            spike(V, F, UV, loc - n * 0.003, dvec, random.uniform(0.045, 0.08) * cf('cheek_len'), 0.0045, random.random())
    for _ in range(int(35 * cf('cheek'))):
        q = hl([random.uniform(-0.04, 0.04), random.uniform(-0.3, -0.12), -0.1])
        loc, n = surf(q)
        dvec = n * 0.4 + np.array([0, 0.3, -1.0]) + np.random.normal(0, 0.2, 3); dvec /= np.linalg.norm(dvec)
        spike(V, F, UV, loc - n * 0.003, dvec, random.uniform(0.03, 0.06) * cf('cheek_len'), 0.004, random.random())
    return with_uv(link('Mane', V, F), UV)


parts.append((bristles(), 'near'))

# coda: esce sopra il cinturone, sottile, con un ciuffo in fondo (osso Tail alla radice)
tb, _ = surf(np.array([0, 0.25, 1.0]))
TAIL_BASE = tb + np.array([0, -0.01, 0])
tp = [TAIL_BASE, TAIL_BASE + [0, 0.06, 0.01], TAIL_BASE + [0, 0.11, -0.03], TAIL_BASE + [0, 0.13, -0.09]]
parts.append((tube('Tail', tp, 0.017, 0.007, 8, 4), 'Tail'))
t = tp[-1]
V, F, UV = [], [], []
for _ in range(34):
    dvec = np.array([0, 0.6, -0.8]) + np.random.normal(0, 0.35, 3); dvec /= np.linalg.norm(dvec)
    spike(V, F, UV, t + np.random.normal(0, 0.006, 3), dvec, random.uniform(0.05, 0.09), 0.004, random.random())
parts.append((with_uv(link('TailTuft', V, F), UV), 'Tail'))

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
print('ossa', len(arm_data.bones), flush=True)

# ------------------------------------------------------------------ pesi del corpo: calore di Blender, poi correzioni
activate(body); arm.select_set(True); bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
vg = {g.name.split(':')[-1]: g for g in body.vertex_groups}
for n in ('Head', 'Jaw', 'Neck', 'LeftEar', 'RightEar'):
    if n not in vg: vg[n] = body.vertex_groups.new(name=PREFIX + n)
print('gruppi', len(vg), flush=True)

V = np.array([v.co[:] for v in body.data.vertices])
Hl = head_local(V)
dh = sdf.head(V); db = sdf.body(V)
in_head = (dh < db - 0.004) & (Hl[:, 1] - H[1] < 0.0)
yl, zl = Hl[:, 1] - H[1], Hl[:, 2] - H[2]
z_up, z_lo, tt = sdf.mouth_lines(yl)
zmid = np.where(yl < sdf.MOUTH_CORNER_Y, (z_up + z_lo) / 2, -0.004 + (yl - sdf.MOUTH_CORNER_Y) * 0.05)
jaw = in_head & (zl < zmid) & (yl < -0.05)
blend = np.clip((db - dh) / 0.03, 0, 1)


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
        else: setw(i, {'Head': t, 'Neck': 1 - t})
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
from scipy.spatial import cKDTree
BV = np.array([v.co[:] for v in body.data.vertices])
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
    for p in ob.data.polygons: p.use_smooth = ob.name not in ('Mane', 'TailTuft')


# ------------------------------------------------------------------ pittura della pelle (sul modello alto)
def seg_dist(P, a, b):
    ab = b - a; t = np.clip(((P - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
    return np.linalg.norm(P - (a + t[:, None] * ab), axis=1), t


def poly_dist(P, pts):
    d = np.full(len(P), 1e9); tt = np.zeros(len(P))
    L = np.cumsum([0] + [np.linalg.norm(pts[i + 1] - pts[i]) for i in range(len(pts) - 1)])
    for i in range(len(pts) - 1):
        di, ti = seg_dist(P, pts[i], pts[i + 1])
        m = di < d; d[m] = di[m]; tt[m] = (L[i] + ti[m] * (L[i + 1] - L[i])) / max(L[-1], 1e-9)
    return d, tt


def vnoise(P, freq, seed, octaves=3):
    rs = np.random.RandomState(seed); acc = 0; amp = 1; tot = 0
    for o_ in range(octaves):
        for _ in range(3):
            k = rs.normal(0, 1, 3); k /= np.linalg.norm(k)
            acc = acc + amp * np.sin(P @ (k * freq * (2 ** o_)) + rs.uniform(0, 6.3))
            tot += amp
        amp *= 0.55
    return acc / tot


me = hi.data
V = np.array([v.co[:] for v in me.vertices]); N = np.array([v.normal[:] for v in me.vertices])
Hl = head_local(V); xl, yl, zl = Hl[:, 0] - H[0], Hl[:, 1] - H[1], Hl[:, 2] - H[2]
dh = sdf.head(V); db = sdf.body(V)
in_head = (dh < db + 0.01) & (yl < 0.03)
seed = CFG.get('seed', 7)
nz1 = vnoise(V, 9.0, seed); nz2 = vnoise(V, 31.0, seed + 1); nz3 = vnoise(V, 75.0, seed + 2, 2)
base = np.array(CFG.get('base', (0.05, 0.034, 0.025)))
belly = np.array(CFG.get('belly_col', (0.085, 0.058, 0.042)))
snout = np.array(CFG.get('snout_col', (0.045, 0.032, 0.028)))
dark = np.array(CFG.get('dark', (0.026, 0.02, 0.017)))
# tono: la pelle sta nella banda media-bassa (0.06-0.12 lineare), cosi' muscoli e muso si leggono anche in ombra
TONE = 1.5
base, belly, snout, dark = base * TONE, belly * TONE, snout * TONE, dark * TONE
HGT = np.zeros(len(V)); RGH = np.full(len(V), 0.8)
C = np.tile(base, (len(V), 1))
# ventre e gola un poco piu' chiari e caldi
tor = sstep(0.75, 0.85, V[:, 2]) * sstep(1.35, 1.25, V[:, 2]) * sstep(0.28, 0.2, np.abs(V[:, 0]))
k = np.clip(-N[:, 1] * 0.8 - N[:, 2] * 0.5, 0, 1) * tor
C = C * (1 - k[:, None] * 0.55) + belly * (k[:, None] * 0.55)
# schiena, gobba, avambracci, stinchi scuri
k = np.clip(N[:, 1] * 0.9 + N[:, 2] * 0.6, 0, 1) * (V[:, 2] > 0.9)
C = C * (1 - k[:, None] * 0.6) + dark * (k[:, None] * 0.6)
fa = np.zeros(len(V))
for pre in ('Left', 'Right'):
    for a, b in (('ForeArm', 'Hand'), ('Leg', 'Foot')):
        A, B = J[pre + a], J[pre + b]
        dd, t = seg_dist(V, A, B + (B - A) * 0.4)
        fa = np.maximum(fa, sstep(0.1, 0.7, t) * (dd < 0.2))
    fa = np.maximum(fa, (np.linalg.norm(V - J[pre + 'Hand'], axis=1) < 0.18) * 0.9)
C = C * (1 - fa[:, None] * 0.5) + dark * (fa[:, None] * 0.5)
# grugno: pelle nuda e grinzosa, piu' grigia e scura; disco del naso quasi nero e umido
mu = sstep(-0.2, -0.27, yl) * in_head
C = C * (1 - mu[:, None] * 0.7) + snout * (mu[:, None] * 0.7)
nose = np.clip(1 - np.abs((Hl - sdf.NOSE) @ sdf.NOSE_N) / 0.022, 0, 1) * (np.linalg.norm(Hl - sdf.NOSE, axis=1) < 0.075)
C = C * (1 - nose[:, None] * 0.8) + np.array([0.03, 0.02, 0.02]) * nose[:, None] * 0.8
RGH = RGH * (1 - mu * 0.2) - nose * 0.2
# bocca: gengive e interno rosso scuro, lingua violacea, bagnati
z_up, z_lo, tt = sdf.mouth_lines(yl)
inside = in_head & (yl < sdf.MOUTH_CORNER_Y + 0.01) & (zl < z_up + 0.006) & (zl > z_lo - 0.006) & (np.abs(xl) < 0.075)
gum = in_head & (yl < sdf.MOUTH_CORNER_Y + 0.01) & (((zl > z_up - 0.002) & (zl < z_up + 0.016) & (N[:, 2] < -0.25)) | ((zl < z_lo + 0.002) & (zl > z_lo - 0.016) & (N[:, 2] > 0.25))) & (np.abs(xl) < 0.07)
C[gum] = np.array([0.045, 0.012, 0.011]); RGH[gum] = 0.5
C[inside] = np.array([0.018, 0.005, 0.005]); RGH[inside] = 0.45
tongue = inside & (zl < z_lo + 0.012) & (np.abs(xl) < 0.035)
C[tongue] = np.array([0.032, 0.011, 0.013])
# arcate e orbite: ombra profonda attorno agli occhi (sguardo cattivo)
ey = np.minimum(np.linalg.norm(Hl - sdf.EYE, axis=1), np.linalg.norm(Hl - sdf.EYE * [-1, 1, 1] - [2 * H[0], 0, 0], axis=1))
k = np.clip(1 - ey / 0.045, 0, 1) * in_head
C = C * (1 - k[:, None] * 0.6)
# chiazze e mottle del cuoio
C *= (1 + 0.22 * nz1 + 0.1 * nz2)[:, None]
# verruche del cinghiale: bitorzoli sotto gli occhi e sui fianchi del grugno
rs = np.random.RandomState(seed + 21)
for _ in range(int(26 * cf('warts', 1.0))):
    s = rs.choice([-1, 1])
    p = hl([s * rs.uniform(0.06, 0.1), rs.uniform(-0.24, -0.1), rs.uniform(0.02, 0.1)])
    p, _n = surf(p)
    dd = np.linalg.norm(V - p, axis=1); r = rs.uniform(0.006, 0.012)
    HGT += np.exp(-(dd / r) ** 2) * 1.2
# vene gonfie su avambracci e bicipiti
for pre, s in (('Left', 1), ('Right', -1)):
    for a, b, n_ in (('Arm', 'ForeArm', 2), ('ForeArm', 'Hand', 3)):
        A, B = J[pre + a], J[pre + b]
        for vv in range(n_):
            ang = rs.uniform(-1.2, 1.2) + (math.pi if vv % 2 else 0)
            pts = []
            for k_ in range(9):
                t = 0.1 + 0.85 * k_ / 8
                c = A + (B - A) * t
                off = np.array([0, math.cos(ang + 0.5 * math.sin(k_ * 1.3)), math.sin(ang + 0.5 * math.sin(k_ * 1.3))])
                p, _n = surf(c + off * 0.2)
                pts.append(p)
            dd, _t = poly_dist(V, pts)
            HGT += np.exp(-(dd / 0.0042) ** 2) * 0.9
# cicatrici: solchi rialzati, pelle chiara e rosata, niente pelo
rs = np.random.RandomState(seed + 3)
for _ in range(int(cf('scars', 3.0)) + 2):
    where = rs.choice(['head', 'chest', 'arm', 'back'], p=[0.35, 0.3, 0.2, 0.15])
    if where == 'head': p0 = hl([rs.uniform(-0.1, 0.1), rs.uniform(-0.3, -0.05), rs.uniform(0.0, 0.17)])
    elif where == 'chest': p0 = np.array([rs.uniform(-0.25, 0.25), -0.25, rs.uniform(0.95, 1.35)])
    elif where == 'back': p0 = np.array([rs.uniform(-0.25, 0.25), 0.25, rs.uniform(1.0, 1.45)])
    else:
        sgn = rs.choice([-1, 1]); p0 = J['LeftArm'] * [sgn, 1, 1] + (J['LeftForeArm'] - J['LeftArm']) * [sgn, 1, 1] * rs.uniform(0, 1.6)
    dvec = rs.normal(0, 1, 3); dvec /= np.linalg.norm(dvec)
    ln = rs.uniform(0.07, 0.2)
    pts = [surf(p0 + dvec * ln * (k_ / 5) + rs.normal(0, 0.006, 3))[0] for k_ in range(6)]
    dd, t = poly_dist(V, pts)
    w = 0.006 + 0.004 * np.sin(t * math.pi)
    k = np.clip(1 - dd / w, 0, 1)
    C = C * (1 - k[:, None] * 0.75) + np.array([0.2, 0.1, 0.085]) * (k[:, None] * 0.75)
    HGT += k * 1.4 - np.exp(-((dd - w * 1.2) / 0.003) ** 2) * 0.5
    RGH = RGH * (1 - k * 0.3)
# pitture di guerra: graffi d'ocra rossa sul muso (sopra l'occhio sinistro) e strisce sulle spalle e sul petto
pc = np.array(CFG.get('paint_col', (0.2, 0.035, 0.02)))
pa = cf('paint', 1.0)
if pa > 0:
    paint = np.zeros(len(V))
    for k_ in range(3):
        a = hl([0.035 + 0.018 * k_, -0.25 + 0.012 * k_, 0.17]); b = hl([0.07 + 0.022 * k_, -0.1 + 0.01 * k_, -0.01])
        dd, t = seg_dist(V, a, b)
        paint = np.maximum(paint, np.clip(1 - dd / (0.009 * (1 - 0.5 * t)), 0, 1) * in_head * (Hl[:, 0] - H[0] > 0))
    for s in (1, -1):
        for k_ in range(2):
            a = np.array([0.3 * s, -0.1, 1.38 - 0.05 * k_]); b = np.array([0.08 * s, -0.27, 1.18 - 0.05 * k_])
            dd, t = seg_dist(V, a, b)
            paint = np.maximum(paint, np.clip(1 - dd / 0.016, 0, 1) * (N[:, 1] < 0.2))
    paint *= np.clip(0.75 + 0.6 * nz3 + 0.3 * nz2, 0, 1) * pa
    C = C * (1 - paint[:, None] * 0.85) + pc * (paint[:, None] * 0.85)
    RGH = RGH * (1 - paint * 0.15) + paint * 0.1
# sangue rappreso: muso, mento, gola, colature sul petto, mani e avambracci
bl = np.array([0.045, 0.006, 0.004])
bk = cf('blood', 1.0)
blood = np.zeros(len(V))
lip = in_head & (yl < sdf.MOUTH_CORNER_Y + 0.03) & (zl < 0.03) & (zl > -0.11)
blood = np.maximum(blood, lip * sstep(-0.1, -0.2, yl) * np.clip(0.6 + 0.8 * nz2, 0, 1))
rs = np.random.RandomState(seed + 31)
for _ in range(7):
    x0 = rs.uniform(-0.12, 0.12); top = hl([x0 * 0.6, rs.uniform(-0.3, -0.15), -0.08])
    bot = np.array([x0 * 1.4, -0.27, top[2] - rs.uniform(0.25, 0.55)])
    dd, t = seg_dist(V, top, bot)
    blood = np.maximum(blood, np.clip(1 - dd / (0.012 * (1 - 0.7 * t)), 0, 1) * (N[:, 1] < 0.3))
for pre in ('Left', 'Right'):
    A, B = J[pre + 'ForeArm'], J[pre + 'Hand']
    dd, t = seg_dist(V, A, B + (B - A) * 0.6)
    blood = np.maximum(blood, sstep(0.35, 0.95, t) * (dd < 0.16) * np.clip(0.55 + 0.9 * nz2, 0, 1))
blood = np.clip(blood * bk, 0, 1)
C = C * (1 - blood[:, None] * 0.9) + bl * (blood[:, None] * 0.9)
RGH = RGH * (1 - blood * 0.5) + blood * 0.25
# fango secco dal basso: piedi, stinchi, ginocchia
mud = sstep(0.5, 0.15, V[:, 2]) * np.clip(0.7 + 0.6 * nz1 + 0.4 * nz3, 0, 1)
mud = np.maximum(mud, sstep(0.1, 0.0, np.abs(V[:, 2] - J['LeftLeg'][2])) * 0.5 * np.clip(0.5 + nz2, 0, 1))
C = C * (1 - mud[:, None] * 0.75) + np.array([0.06, 0.05, 0.038]) * (mud[:, None] * 0.75)
RGH = RGH * (1 - mud) + 0.92 * mud
# zoccoli: corno quasi nero
hv = (V[:, 2] < 0.07) & (V[:, 1] < 0.03)
C[hv] = np.array([0.018, 0.015, 0.013])
# striature dei giovani, vecchiaia
if cf('stripes', 0.0) > 0:
    ang = np.arctan2(V[:, 0], V[:, 1] - 0.02)
    band = sstep(0.35, 0.7, np.cos(ang * 9)) * (V[:, 2] > 0.75) * (V[:, 2] < 1.45) * (np.abs(V[:, 0]) < 0.32)
    C = C * (1 - band[:, None] * 0.45 * cf('stripes')) + np.array([0.13, 0.095, 0.07]) * (band[:, None] * 0.45 * cf('stripes'))
g = cf('grizzle', 0.0)
if g > 0:
    kk = sstep(-0.05, -0.2, yl) * in_head
    grey = np.array([0.22, 0.21, 0.2])
    C = C * (1 - kk[:, None] * g) + grey * (kk[:, None] * g)
# orecchie: interno rosato scuro (sono parti, la pittura del corpo non le tocca: colore dato dopo)
C = np.clip(C, 0, 1); RGH = np.clip(RGH, 0.08, 1)
HGT = np.clip(HGT, -1, 2)
ca = me.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
ca.data.foreach_set('color', np.c_[C, np.ones(len(C))].ravel())
ra = me.attributes.new('rgh', 'FLOAT', 'POINT'); ra.data.foreach_set('value', RGH)
ha = me.attributes.new('hgt', 'FLOAT', 'POINT'); ha.data.foreach_set('value', HGT)
print('pittura fatta', flush=True)

# ------------------------------------------------------------------ orecchie nel corpo (stessa texture) e copia alta
ears = [ob for ob, r in parts if ob.name.startswith('Ear')]
for e in ears:
    e2 = e.copy(); e2.data = e.data.copy(); col.objects.link(e2)
    e2.parent = None; e2.modifiers.clear()
    me2 = e2.data
    n = len(me2.vertices)
    EV = np.array([v.co[:] for v in me2.vertices])
    ec = np.tile(np.array([0.05, 0.035, 0.03]), (n, 1)) * (1 + 0.2 * vnoise(EV, 30, seed + 4))[:, None]
    me2.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT').data.foreach_set('color', np.c_[ec, np.ones(n)].ravel())
    me2.attributes.new('rgh', 'FLOAT', 'POINT').data.foreach_set('value', np.full(n, 0.7))
    me2.attributes.new('hgt', 'FLOAT', 'POINT').data.foreach_set('value', np.zeros(n))
    activate(hi); e2.select_set(True); bpy.ops.object.join()
activate(body)
for o in ears: o.select_set(True)
bpy.ops.object.join()
body.name = 'Body'

# ------------------------------------------------------------------ UV e cottura alto -> basso
activate(body)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(58), island_margin=0.003)
bpy.ops.uv.pack_islands(rotate=True, margin=0.004)
bpy.ops.object.mode_set(mode='OBJECT')
sc.render.engine = 'CYCLES'; sc.cycles.samples = 1; sc.cycles.device = 'CPU'
sc.render.bake.margin = 8
sc.render.bake.use_selected_to_active = True
sc.render.bake.cage_extrusion = 0.012
sc.render.bake.max_ray_distance = 0.04

img_c = bpy.data.images.new('Skin_BaseColor', TEX, TEX)
img_o = bpy.data.images.new('Skin_ORM', TEX // 2, TEX // 2); img_o.colorspace_settings.name = 'Non-Color'
img_n = bpy.data.images.new('Skin_Normal', TEX, TEX); img_n.colorspace_settings.name = 'Non-Color'

# materiale del modello alto: colore (attributo x pori x pelo x cuoio fotografato), ruvidita', rilievo
hm = bpy.data.materials.new('HiSkin'); hm.use_nodes = True
nt = hm.node_tree; NN = nt.nodes.new; LL = nt.links.new
bs = nt.nodes['Principled BSDF']; outn = nt.nodes['Material Output']
attr = NN('ShaderNodeAttribute'); attr.attribute_name = 'Col'
ar = NN('ShaderNodeAttribute'); ar.attribute_name = 'rgh'
ah = NN('ShaderNodeAttribute'); ah.attribute_name = 'hgt'
tc = NN('ShaderNodeTexCoord')
pores = NN('ShaderNodeTexVoronoi'); pores.inputs['Scale'].default_value = 260.0
LL(tc.outputs['Object'], pores.inputs['Vector'])
pr = NN('ShaderNodeMapRange'); pr.inputs['From Min'].default_value = 0.0; pr.inputs['From Max'].default_value = 0.35
pr.inputs['To Min'].default_value = 0.85; pr.inputs['To Max'].default_value = 1.0
LL(pores.outputs['Distance'], pr.inputs['Value'])
# pelo ispido: rumore stirato lungo il corpo (setole corte che scendono)
mp = NN('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 1.0, 0.1)
LL(tc.outputs['Object'], mp.inputs['Vector'])
hair = NN('ShaderNodeTexNoise'); hair.inputs['Scale'].default_value = 520.0; hair.inputs['Detail'].default_value = 3
LL(mp.outputs['Vector'], hair.inputs['Vector'])
hr = NN('ShaderNodeMapRange'); hr.inputs['From Min'].default_value = 0.42; hr.inputs['From Max'].default_value = 0.66
hr.inputs['To Min'].default_value = 1.12; hr.inputs['To Max'].default_value = 0.5
LL(hair.outputs['Fac'], hr.inputs['Value'])
m1 = NN('ShaderNodeMath'); m1.operation = 'MULTIPLY'; LL(pr.outputs['Result'], m1.inputs[0]); LL(hr.outputs['Result'], m1.inputs[1])
cm = NN('ShaderNodeVectorMath'); cm.operation = 'MULTIPLY'; LL(attr.outputs['Color'], cm.inputs[0]); LL(m1.outputs['Value'], cm.inputs[1])
col_out = cm.outputs['Vector']
photo_lib.ensure(['fabric_leather_02'])
P = photo_lib.nodes(nt, tc.outputs['Object'], 'fabric_leather_02', 1.6)
if P is not None:
    pm = NN('ShaderNodeMix'); pm.data_type = 'RGBA'; pm.blend_type = 'MULTIPLY'; pm.inputs['Factor'].default_value = 0.5
    LL(col_out, pm.inputs['A']); LL(P['rel'], pm.inputs['B']); col_out = pm.outputs['Result']
emit = NN('ShaderNodeEmission')
hm_body = hm
hi.data.materials.clear(); hi.data.materials.append(hm)


def bake_to(image, kind='EMIT'):
    bm_ = bpy.data.materials.get('LowBake') or bpy.data.materials.new('LowBake')
    bm_.use_nodes = True
    t = bm_.node_tree.nodes.get('img') or bm_.node_tree.nodes.new('ShaderNodeTexImage')
    t.name = 'img'; t.image = image; bm_.node_tree.nodes.active = t
    body.data.materials.clear(); body.data.materials.append(bm_)
    activate(body); hi.select_set(True); bpy.context.view_layer.objects.active = body
    bpy.ops.object.bake(type=kind)


# 1) colore
LL(col_out, emit.inputs['Color']); LL(emit.outputs['Emission'], outn.inputs['Surface'])
bake_to(img_c)
# 2) ORM: R=1 (occlusione nella luce del gioco), G=ruvidita' (attributo + cuoio + pelo), B=0
comb = NN('ShaderNodeCombineColor'); comb.inputs['Red'].default_value = 1.0; comb.inputs['Blue'].default_value = 0.0
ra_ = NN('ShaderNodeMath'); ra_.operation = 'ADD'; ra_.use_clamp = True
LL(ar.outputs['Fac'], ra_.inputs[0]); ra_.inputs[1].default_value = 0.0
if P is not None and 'rough' in P:
    rs_ = NN('ShaderNodeMath'); rs_.operation = 'MULTIPLY'; LL(P['rough'], rs_.inputs[0]); rs_.inputs[1].default_value = 0.5
    LL(rs_.outputs[0], ra_.inputs[1])
LL(ra_.outputs[0], comb.inputs['Green']); LL(comb.outputs[0], emit.inputs['Color'])
bake_to(img_o)
# 3) normali: forma alta + rilievo (cicatrici, vene, verruche, pori, pelo, pieghe del cuoio)
hsum = NN('ShaderNodeMath'); hsum.operation = 'MULTIPLY_ADD'
LL(ah.outputs['Fac'], hsum.inputs[0]); hsum.inputs[1].default_value = 1.0
pd = NN('ShaderNodeMath'); pd.operation = 'MULTIPLY_ADD'; LL(pores.outputs['Distance'], pd.inputs[0]); pd.inputs[1].default_value = 0.3
hd = NN('ShaderNodeMath'); hd.operation = 'MULTIPLY_ADD'; LL(hair.outputs['Fac'], hd.inputs[0]); hd.inputs[1].default_value = 0.25
LL(hd.outputs[0], pd.inputs[2])
if P is not None and 'disp' in P:
    dd_ = NN('ShaderNodeMath'); dd_.operation = 'MULTIPLY_ADD'; LL(P['disp'], dd_.inputs[0]); dd_.inputs[1].default_value = 0.8
    LL(pd.outputs[0], dd_.inputs[2]); LL(dd_.outputs[0], hsum.inputs[2])
else:
    LL(pd.outputs[0], hsum.inputs[2])
bump = NN('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.8; bump.inputs['Distance'].default_value = 0.002
LL(hsum.outputs[0], bump.inputs['Height']); LL(bump.outputs['Normal'], bs.inputs['Normal'])
LL(bs.outputs['BSDF'], outn.inputs['Surface'])
sc.render.bake.normal_space = 'TANGENT'
bake_to(img_n, 'NORMAL')
print('cottura fatta', flush=True)
sc.render.bake.use_selected_to_active = False

# materiale finale della pelle
skin = bpy.data.materials.new('Skin'); skin.use_nodes = True
st = skin.node_tree; sb = st.nodes['Principled BSDF']
tn = st.nodes.new('ShaderNodeTexImage'); tn.image = img_c
tn_o = st.nodes.new('ShaderNodeTexImage'); tn_o.image = img_o
tn_n = st.nodes.new('ShaderNodeTexImage'); tn_n.image = img_n
st.links.new(tn.outputs['Color'], sb.inputs['Base Color'])
sp_ = st.nodes.new('ShaderNodeSeparateColor'); st.links.new(tn_o.outputs['Color'], sp_.inputs[0])
st.links.new(sp_.outputs['Green'], sb.inputs['Roughness']); st.links.new(sp_.outputs['Blue'], sb.inputs['Metallic'])
nm_ = st.nodes.new('ShaderNodeNormalMap'); st.links.new(tn_n.outputs['Color'], nm_.inputs['Color']); st.links.new(nm_.outputs['Normal'], sb.inputs['Normal'])
body.data.materials.clear(); body.data.materials.append(skin)
for im in (img_c, img_o, img_n): im.pack()
bpy.data.objects.remove(hi)


# ------------------------------------------------------------------ texture delle parti (fatte qui con numpy)
def np_image(name, arr, noncolor=False):
    h, w, _ = arr.shape
    im = bpy.data.images.new(name, w, h, alpha=False)
    if noncolor: im.colorspace_settings.name = 'Non-Color'
    rgba = np.concatenate([arr, np.ones((h, w, 1))], -1).astype(np.float32)
    im.pixels.foreach_set(rgba.ravel()); im.pack()
    return im


def lin(c): return np.asarray(c, float)


rs = np.random.RandomState(seed + 41)
u = np.linspace(0, 1, 256)[None, :, None]; v = np.linspace(0, 1, 512)[:, None, None]
# avorio: radice bruna e sporca, corpo giallastro con anelli di crescita e crepe, punta consumata, sangue verso la punta
ivory = lin(CFG.get('tusk_col', (0.42, 0.36, 0.25)))
root = lin((0.09, 0.06, 0.035))
tv = np.clip((v - 0.05) / 0.35, 0, 1)
A = root * (1 - tv) + ivory * tv
rings = 0.5 + 0.5 * np.sin(v * 140 + 2.0 * np.sin(u * 6.28 * 2))
A = A * (0.97 + 0.03 * rings)
streak = np.sin(u * 6.28 * 7 + rs.uniform(0, 6)) * 0.5 + 0.5
A = A * (0.9 + 0.1 * streak)
crack = (np.abs(np.sin(u * 6.28 * 3 + v * 9)) < 0.02) * (v > 0.25)
A = A * (1 - 0.5 * crack)
bmask = np.clip((v - 0.55) / 0.3, 0, 1) * (0.5 + 0.5 * np.sin(u * 6.28 + 1.3)) * cf('blood', 1.0)
bmask = np.clip(bmask * (0.7 + 0.5 * rs.rand(512, 256, 1)), 0, 1)
A = A * (1 - bmask * 0.85) + lin((0.07, 0.008, 0.006)) * bmask * 0.85
img_iv = np_image('Ivory_BaseColor', np.clip(A, 0, 1)[::-1].copy())
Ro = np.ones_like(A); Ro[..., 1] = (0.45 + 0.15 * rings[..., 0] - 0.15 * bmask[..., 0] + 0.35 * (1 - tv[..., 0]))[...]; Ro[..., 2] = 0
img_ivo = np_image('Ivory_ORM', np.clip(Ro, 0, 1)[::-1].copy(), True)
# normali dell'avorio: anelli e crepe
hgt = 0.05 * rings[..., 0] - 1.2 * crack[..., 0]
gy, gx = np.gradient(hgt)
Nn = np.stack([-gx * 2.0, -gy * 2.0, np.ones_like(hgt)], -1); Nn /= np.linalg.norm(Nn, axis=-1, keepdims=True)
img_ivn = np_image('Ivory_Normal', (Nn * 0.5 + 0.5)[::-1].copy(), True)
# artigli: corno nero lucido, piu' chiaro e consumato in punta, sporco di sangue
cl = root * 0.25 * (1 - v) + lin((0.05, 0.042, 0.035)) * v
cl = cl * (0.85 + 0.15 * np.sin(u * 6.28 * 5))
cb = np.clip((v - 0.4) / 0.4, 0, 1) * cf('blood', 1.0) * (0.6 + 0.4 * rs.rand(512, 256, 1))
cl = cl * (1 - cb * 0.6) + lin((0.06, 0.006, 0.005)) * cb * 0.6
img_cl = np_image('Claw_BaseColor', np.clip(cl, 0, 1)[::-1].copy())


def tex_mat(name, img_col, img_orm=None, img_nrm=None, rough=0.6):
    m = bpy.data.materials.new(name); m.use_nodes = True
    t_ = m.node_tree; b_ = t_.nodes['Principled BSDF']
    tc_ = t_.nodes.new('ShaderNodeTexImage'); tc_.image = img_col; t_.links.new(tc_.outputs['Color'], b_.inputs['Base Color'])
    b_.inputs['Roughness'].default_value = rough
    if img_orm is not None:
        to = t_.nodes.new('ShaderNodeTexImage'); to.image = img_orm
        sp = t_.nodes.new('ShaderNodeSeparateColor'); t_.links.new(to.outputs['Color'], sp.inputs[0])
        t_.links.new(sp.outputs['Green'], b_.inputs['Roughness']); t_.links.new(sp.outputs['Blue'], b_.inputs['Metallic'])
    if img_nrm is not None:
        tn_ = t_.nodes.new('ShaderNodeTexImage'); tn_.image = img_nrm
        nm = t_.nodes.new('ShaderNodeNormalMap'); t_.links.new(tn_.outputs['Color'], nm.inputs['Color']); t_.links.new(nm.outputs['Normal'], b_.inputs['Normal'])
    return m


def mat(name, color, rough, metal=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    bs_ = m.node_tree.nodes['Principled BSDF']
    bs_.inputs['Base Color'].default_value = (*color, 1)
    bs_.inputs['Roughness'].default_value = rough
    bs_.inputs['Metallic'].default_value = metal
    return m


M_IVORY = tex_mat('Tusk_Ivory', img_iv, img_ivo, img_ivn)
M_CLAW = tex_mat('Claw_Horn', img_cl, rough=0.35)
# setole a tinta unita: col materiale "capelli" (ritaglio con l'alfa) una texture costa al gioco centinaia di MB per modello
M_FUR = mat('Fur_Bristle', tuple(lin(CFG.get('mane_col', (0.02, 0.016, 0.014))) * 1.4), 0.75)
M_EYE = mat('Eye', tuple(CFG.get('eye', (0.11, 0.02, 0.004))), 0.1)
M_TAIL = mat('Skin_Tail', tuple(np.array(CFG.get('base', (0.06, 0.045, 0.036))) * 0.6), 0.75)

def _alive(ob):
    try: return ob.name in bpy.data.objects
    except ReferenceError: return False


alive = [(ob, r) for ob, r in parts if _alive(ob)]
for ob, rule in alive:
    n = ob.name
    if n.startswith('Tusk') or n.startswith('Tooth'): ob.data.materials.append(M_IVORY)
    elif n.startswith('Eye'): ob.data.materials.append(M_EYE)
    elif n.startswith('Claw'): ob.data.materials.append(M_CLAW)
    elif n in ('Mane', 'TailTuft'): ob.data.materials.append(M_FUR)
    elif n == 'Tail': ob.data.materials.append(M_TAIL)

# zanne e denti in un solo oggetto, artigli in un solo oggetto
for prefix, newname in (('Claw', 'Claws'), ('Tooth', 'Teeth')):
    group = [ob for ob, r in parts if _alive(ob) and ob.name.startswith(prefix)]
    if not group: continue
    activate(group[0])
    for o in group[1:]: o.select_set(True)
    bpy.ops.object.join(); group[0].name = newname

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
print('orecchie', len(earo.data.vertices), flush=True)

# texture anche a parte (per ritoccarle o metterle in un altro programma)
if TEXDIR:
    td = os.path.join(TEXDIR, 'vrepighs_' + VAR); os.makedirs(td, exist_ok=True)
    for im in (img_c, img_o, img_n, img_iv, img_ivo, img_ivn, img_cl):
        im.filepath_raw = os.path.join(td, im.name + '.png'); im.file_format = 'PNG'; im.save()

# ------------------------------------------------------------------ esporta
for o in bpy.data.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(f'vrepigh_{VAR}.blend'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=os.path.abspath(OUT), export_format='GLB', use_selection=True,
                          export_skins=True, export_animations=False, export_apply=False, export_yup=True,
                          export_image_format='JPEG', export_jpeg_quality=92)
print('esportato', OUT, flush=True)
