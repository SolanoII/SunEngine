# Modello "indianpig": il porcellino d'India dei Vrepigh, prima versione del Pighcib, tenuta da parte per altri usi.
# Corpo a pera dal campo di distanza (sdf.py), testa lunga e tonda senza collo, disco del naso dei Vrepigh, orecchie
# a petalo, occhietti sporgenti, zannette, pelo a rosette in tre colori. Scheletro del Pighcib (rig.json) + Jaw e orecchie.
# Uscita: Sandbox/GameData/Models/Extra/indianpig/model.glb (il gioco non lo usa ancora).
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
    os.path.abspath(os.path.join(HERE, '../../../Sandbox/GameData/Models/Extra/indianpig/model.glb'))
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


parts = []

# zannette: due punte corte dagli angoli della bocca
for s in (1, -1):
    b = hl([0.028 * s, -0.172, 0.004])
    pts = [b, b + np.array([0.005 * s, -0.005, 0.016]) * HS, b + np.array([0.012 * s, 0.0, 0.03]) * HS]
    parts.append((tube('Tusk_' + ('L' if s > 0 else 'R'), pts, 0.0065 * HS, 0.0008, 8, 3), 'Jaw'))


# orecchie a petalo, tonde, che ricadono in fuori
def ear(s):
    base = hl([0.088 * s, -0.025, 0.15])
    ax = np.array([0.85 * s, 0.3, 0.42]); ax /= np.linalg.norm(ax)
    wd = np.array([0.0, -1.0, 0.25]); wd -= ax * (wd @ ax); wd /= np.linalg.norm(wd)
    nn = np.cross(ax, wd) * s
    Lh, W = 0.075 * HS, 0.075 * HS
    nu, nv = 9, 9
    V, F = [], []
    for side in (0, 1):
        for i in range(nu):
            u = i / (nu - 1)
            w = W * 0.5 * max(math.sin(math.pi * (0.12 + 0.88 * u)), 0.0) ** 0.55 + 0.003
            for j in range(nv):
                v = j / (nv - 1) * 2 - 1
                cup = (1 - v * v) * 0.012 * HS * math.sin(math.pi * u)
                droop = -0.025 * HS * u * u                                  # la punta ricade
                p = base + ax * (Lh * u) + wd * (w * v) + nn * (cup + (0.004 if side else -0.004) * (1 - 0.6 * u)) + np.array([0, 0, droop])
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
        F.append((j + 1, j, off + j, off + j + 1))
        a = (nu - 1) * nv + j
        F.append((a, a + 1, off + a + 1, off + a))
    return link('Ear_' + ('L' if s > 0 else 'R'), V, F)


for s in (1, -1):
    parts.append((ear(s), 'LeftEar' if s > 0 else 'RightEar'))

# occhietti neri e lucidi, sporgenti ai lati della testa
for s in (1, -1):
    loc, n = surf(hl((sdf.EYE - H) * [s, 1, 1]))
    bpy.ops.mesh.primitive_uv_sphere_add(segments=14, ring_count=10, radius=0.017 * HS, location=tuple(loc - n * 0.006))
    o = bpy.context.active_object; o.name = 'Eye_' + ('L' if s > 0 else 'R')
    parts.append((o, 'Head'))

# unghiette
for side, pre in ((1, 'Left'), (-1, 'Right')):
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        b = J[pre + 'Hand' + f + '2']; c = J[pre + 'Hand' + f + '3']
        dirv = (c - b) / np.linalg.norm(c - b)
        loc, _ = surf(b + (c - b) * 0.4 + dirv * 0.02)
        p0 = loc - dirv * 0.003
        parts.append((tube('Claw', [p0, p0 + dirv * 0.007, p0 + dirv * 0.012 + np.array([0, 0, -0.003])], 0.004, 0.0006, 6, 2), 'near'))

# ------------------------------------------------------------------ colori a chiazze (crema, fulvo, scuro)
me = body.data
V = np.array([v.co[:] for v in me.vertices]); N = np.array([v.normal[:] for v in me.vertices])
Hl = H + (V - H) / HS
CREAM = np.array([0.46, 0.37, 0.25])
GINGER = np.array([0.34, 0.14, 0.045])
DARK = np.array([0.035, 0.025, 0.02])
ph = np.random.rand(5) * 6.28
dirs = np.random.normal(0, 1, (5, 3)); dirs /= np.linalg.norm(dirs, axis=1)[:, None]
val = sum(np.sin(V @ dirs[i] * 11.0 + ph[i]) for i in range(5)) / 5
val2 = sum(np.sin(V @ dirs[(i + 2) % 5] * 7.0 + ph[i] * 1.7) for i in range(5)) / 5
region = np.where(val > 0.18, 1, np.where(val2 < -0.22, 2, 0))           # 0 crema, 1 fulvo, 2 scuro
# testa: una mascherina scura attorno agli occhi da un lato, fulva dall'altro, lista bianca in mezzo al muso
inh = (sdf.head(V) < sdf.body(V)) & (Hl[:, 2] > H[2] - 0.02)
region[inh & (V[:, 0] > 0.02)] = 2
region[inh & (V[:, 0] < -0.02)] = 1
region[inh & (np.abs(V[:, 0]) < 0.028 + 0.02 * np.clip(-(Hl[:, 1] - H[1]) - 0.1, 0, 1) * 4)] = 0
COLS = np.array([CREAM, GINGER, DARK])
C = COLS[region].copy()
# grugno, naso e zampe rosati
nose = np.linalg.norm(Hl - sdf.NOSE, axis=1) < 0.03
C[nose] = np.array([0.32, 0.17, 0.15])
paws = (V[:, 2] < 0.05) | (np.linalg.norm(V - J['LeftHand'], axis=1) < 0.07) | (np.linalg.norm(V - J['RightHand'], axis=1) < 0.07)
C[paws] = C[paws] * 0.4 + np.array([0.3, 0.19, 0.16]) * 0.6
nz = sum(np.sin(V @ (np.random.normal(0, 30, 3)) + i) for i in range(6)) / 6
C *= (1 + 0.12 * nz)[:, None]
C = np.clip(C, 0, 1)


# ------------------------------------------------------------------ pelo a rosette (vortici), del colore della chiazza
def rosettes():
    centers = [np.array([0.07, 0.15, 0.55]), np.array([-0.07, 0.15, 0.55]), np.array([0, 0.16, 0.42]),
               np.array([0.16, 0.02, 0.47]), np.array([-0.16, 0.02, 0.47]), np.array([0.1, 0.1, 0.66]),
               np.array([-0.1, 0.1, 0.66]), hl([0, -0.04, 0.19]), np.array([0.12, 0.12, 0.36]), np.array([-0.12, 0.12, 0.36])]
    KD = cKDTree(V)
    out = {0: ([], []), 1: ([], []), 2: ([], [])}
    for c in centers:
        cs, cn = surf(c)
        for _ in range(48):
            rad = 0.065 * math.sqrt(random.random())
            a = random.uniform(0, 2 * math.pi)
            t1 = np.cross(cn, [0, 0, 1.0]); t1 = t1 / (np.linalg.norm(t1) + 1e-9) if np.linalg.norm(t1) > 1e-3 else np.array([1.0, 0, 0])
            t2 = np.cross(cn, t1)
            q = cs + (t1 * math.cos(a) + t2 * math.sin(a)) * rad
            loc, n = surf(q)
            radial = loc - cs; radial -= n * (radial @ n)
            radial = radial / (np.linalg.norm(radial) + 1e-9)
            dvec = radial * 0.9 + n * 0.35 + np.random.normal(0, 0.12, 3); dvec /= np.linalg.norm(dvec)
            L = (0.012 + 0.016 * rad / 0.065) * random.uniform(0.8, 1.2)
            _, vi = KD.query(loc)
            Vv, Ff = out[int(region[vi])]
            spike(Vv, Ff, loc - n * 0.002, dvec, L, 0.0026)
    # guance: ciuffi all'indietro
    for s in (1, -1):
        for _ in range(40):
            q = hl([0.075 * s + random.uniform(-0.01, 0.01), random.uniform(-0.12, -0.04), random.uniform(-0.02, 0.04)])
            loc, n = surf(q)
            dvec = n * 0.5 + np.array([0.25 * s, 0.85, -0.35]) + np.random.normal(0, 0.12, 3); dvec /= np.linalg.norm(dvec)
            _, vi = KD.query(loc)
            Vv, Ff = out[int(region[vi])]
            spike(Vv, Ff, loc - n * 0.002, dvec, random.uniform(0.025, 0.04), 0.003)
    objs = []
    for k, name in ((0, 'Fur_Cream'), (1, 'Fur_Ginger'), (2, 'Fur_Dark')):
        if out[k][0]: objs.append(link(name, *out[k]))
    return objs


for o in rosettes():
    parts.append((o, 'near'))

# ------------------------------------------------------------------ scheletro
arm_data = bpy.data.armatures.new('Armature')
arm = bpy.data.objects.new('Armature', arm_data); col.objects.link(arm)
activate(arm)
bpy.ops.object.mode_set(mode='EDIT')
EXTRA = {'Jaw': hl([0, -0.07, -0.005]), 'LeftEar': hl([0.088, -0.025, 0.15]), 'RightEar': hl([-0.088, -0.025, 0.15])}
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
    elif name == 'Jaw': tail = hl([0, -0.16, -0.005])
    elif name.endswith('Ear'): tail = hl([0.15 * (1 if name.startswith('Left') else -1), 0.0, 0.17])
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
jaw = in_head & (Hl[:, 2] < H[2] + 0.004) & (Hl[:, 1] < H[1] - 0.09)
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


def mat(name, color, rough):
    m = bpy.data.materials.new(name); m.use_nodes = True
    bs = m.node_tree.nodes['Principled BSDF']
    bs.inputs['Base Color'].default_value = (*color, 1); bs.inputs['Roughness'].default_value = rough
    bs.inputs['Metallic'].default_value = 0.0
    return m


for ob, rule in parts:
    n = ob.name
    if n.startswith('Tusk'): ob.data.materials.append(mat('Tusk_Ivory', (0.62, 0.56, 0.44), 0.45))
    elif n.startswith('Eye'): ob.data.materials.append(mat('Eye', (0.01, 0.008, 0.008), 0.08))
    elif n.startswith('Claw'): ob.data.materials.append(mat('Claw_Horn', (0.2, 0.16, 0.13), 0.4))
    elif n == 'Fur_Cream': ob.data.materials.append(mat('Fur_Cream', tuple(CREAM * 0.9), 0.8))
    elif n == 'Fur_Ginger': ob.data.materials.append(mat('Fur_Ginger', tuple(GINGER * 0.9), 0.8))
    elif n == 'Fur_Dark': ob.data.materials.append(mat('Fur_Dark', tuple(DARK), 0.8))

claws_o = [ob for ob, r in parts if ob.name.startswith('Claw')]
ears = [ob for ob, r in parts if ob.name.startswith('Ear')]
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
        C2[i] = C2[i] * 0.45 + np.array([0.34, 0.19, 0.16]) * 0.55
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
bs.inputs['Roughness'].default_value = 0.8
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
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath('indianpig.blend'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_skins=True,
                          export_animations=False, export_apply=False, export_yup=True,
                          export_image_format='JPEG', export_jpeg_quality=92)
print('esportato', OUT)
