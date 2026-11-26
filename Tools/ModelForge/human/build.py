# Corpo umano d'autore (Regno, recluta/fante/cadetto e aspirante): anatomia vera da MakeHuman (CC0), adattata allo
# scheletro del gioco (footman_master.fbx), con volto, occhi e pelle cotti in texture. Vedi README.md.
#
#   python3 build.py [--fast] [--tex 2048] [--out file.glb] [--copy-candidate]
#
# Passi:
#   1. mh_body.npz (prepare_mh.py): maschio giovane muscoloso, in posa A, con le articolazioni di MakeHuman
#   2. scheletro provvisorio sulle articolazioni di MakeHuman, pesi dal calore di Blender
#   3. ogni osso porta la sua parte di pelle dalla posa A alla posa T del gioco (direzione e lunghezza dell'osso):
#      skinning lineare con trasformazioni calcolate qui, cosi' le articolazioni cadono esattamente su quelle del gioco
#   4. pelle sullo scheletro vero del gioco con gli stessi pesi, occhi sull'osso della testa
#   5. pelle cotta con Cycles: pori, chiazze, labbra, sopracciglia, barba e capelli rasati (mappa di dettaglio che il
#      gioco moltiplica per la carnagione scelta nel garage: "tint" in model.json), occhi con iride e cornea
import sys, os, json, math, shutil
import numpy as np
import bpy, bmesh
from mathutils import Vector, Matrix

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
def arg(n, d=None): return ARGV[ARGV.index(n) + 1] if n in ARGV else d
CHARS = os.path.join(ROOT, 'Sandbox/GameData/Models/Characters')
OUT = os.path.abspath(arg('--out', os.path.join(CHARS, 'footman', 'model.glb')))
TEX = int(arg('--tex', '2048'))
FAST = '--fast' in ARGV
PREFIX = 'mixamorig1:'

bpy.ops.wm.read_factory_settings(use_empty=True)
SC = bpy.context.scene
COL = SC.collection


def activate(ob):
    bpy.context.view_layer.update()
    for o in bpy.context.view_layer.objects:
        if o: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


# ------------------------------------------------------------------ 1. corpo di MakeHuman
D = np.load(os.path.join(HERE, 'mh_body.npz'))
MV = D['V'].copy()
J = {str(n): p.copy() for n, p in zip(D['joint_names'], D['joint_pos'])}
ground = J['ground'][2]
MV[:, 2] -= ground
for k in J: J[k][2] -= ground

# ------------------------------------------------------------------ scheletro del gioco
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=os.path.join(CHARS, 'footman_master.fbx'))
new = [o for o in bpy.data.objects if o not in before]
ARM = next(o for o in new if o.type == 'ARMATURE')
for o in new:
    if o.name != ARM.name: bpy.data.objects.remove(o, do_unlink=True)
activate(ARM)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
ARM.name = 'Armature'
ARM.animation_data_clear()
for pb in ARM.pose.bones: pb.matrix_basis = Matrix()
GB = {b.name[len(PREFIX):]: (np.array(b.head_local[:]), np.array(b.tail_local[:])) for b in ARM.data.bones}
GPAR = {b.name[len(PREFIX):]: (b.parent.name[len(PREFIX):] if b.parent else None) for b in ARM.data.bones}

# scala: stessa altezza degli occhi del corpo del gioco (la testa del gioco e' a GB['Head'])
s = (GB['HeadTop_End'][0][2]) / (J['head-2'][2])
MV *= s
for k in J: J[k] = J[k] * s

# ------------------------------------------------------------------ articolazioni: MakeHuman -> ossa del gioco
FING = {1: 'Thumb', 2: 'Index', 3: 'Middle', 4: 'Ring', 5: 'Pinky'}
MAP = {'Hips': 'pelvis', 'Spine': 'spine-3', 'Spine1': 'spine-2', 'Spine2': 'spine-1', 'Neck': 'neck', 'Head': 'head',
       'HeadTop_End': 'head-2'}
for side, s_ in (('Left', 'l'), ('Right', 'r')):
    MAP.update({f'{side}Shoulder': f'{s_}-clavicle', f'{side}Arm': f'{s_}-shoulder', f'{side}ForeArm': f'{s_}-elbow',
                f'{side}Hand': f'{s_}-hand', f'{side}UpLeg': f'{s_}-upper-leg', f'{side}Leg': f'{s_}-knee',
                f'{side}Foot': f'{s_}-ankle', f'{side}ToeBase': f'{s_}-foot-1', f'{side}Toe_End': f'{s_}-foot-2'})
    for k, nm in FING.items():
        for j in range(1, 5): MAP[f'{side}Hand{nm}{j}'] = f'{s_}-finger-{k}-{j}'
MAP = {b: m for b, m in MAP.items() if b in GB and m in J}
MAIN_CHILD = {'Hips': 'Spine', 'Spine2': 'Neck', 'LeftHand': 'LeftHandMiddle1', 'RightHand': 'RightHandMiddle1'}


def child_of(b):
    if b in MAIN_CHILD: return MAIN_CHILD[b]
    kids = [c for c, p in GPAR.items() if p == b and c in MAP]
    return kids[0] if kids else None


DEFORM = [b for b in MAP if not b.endswith('_End') and not b.endswith('4')]

# ------------------------------------------------------------------ mesh del corpo e degli occhi
def make_mesh(name, key):
    fv, ft, fn = D[key + '_fv'], D[key + '_ft'], D[key + '_fn']
    used = np.unique(fv); remap = {int(o): i for i, o in enumerate(used)}
    faces, k = [], 0
    for n in fn: faces.append([remap[int(i)] for i in fv[k:k + n]]); k += n
    me = bpy.data.meshes.new(name); me.from_pydata([tuple(MV[i]) for i in used], [], faces); me.update()
    uv = me.uv_layers.new(name='UVMap')
    k = 0
    for poly in me.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = tuple(D['VT'][int(ft[k])]); k += 1
    ob = bpy.data.objects.new(name, me); COL.objects.link(ob)
    return ob, used

body, body_idx = make_mesh('Body', 'body')
for p in body.data.polygons: p.use_smooth = True

# ------------------------------------------------------------------ 2. scheletro provvisorio e pesi dal calore
tmp = bpy.data.armatures.new('MHArm'); TA = bpy.data.objects.new('MHArm', tmp); COL.objects.link(TA)
activate(TA); bpy.ops.object.mode_set(mode='EDIT')
for b in DEFORM:
    eb = tmp.edit_bones.new(b)
    eb.head = Vector(J[MAP[b]])
    c = child_of(b)
    if c and c in MAP: tail = J[MAP[c]]
    else: tail = J[MAP[b]] + (J[MAP[b]] - J[MAP[GPAR[b]]]) * 0.6
    if np.linalg.norm(tail - J[MAP[b]]) < 1e-3: tail = J[MAP[b]] + np.array([0, 0, 0.02])
    eb.tail = Vector(tail)
for b in DEFORM:
    p = GPAR[b]
    while p and p not in DEFORM: p = GPAR[p]
    if p: tmp.edit_bones[b].parent = tmp.edit_bones[p]
bpy.ops.object.mode_set(mode='OBJECT')
activate(body); TA.select_set(True); bpy.context.view_layer.objects.active = TA
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
nv = len(body.data.vertices)
W = np.zeros((nv, len(DEFORM)))
gi = {g.index: DEFORM.index(g.name) for g in body.vertex_groups if g.name in DEFORM}
for v in body.data.vertices:
    for g in v.groups:
        if g.group in gi: W[v.index, gi[g.group]] = g.weight
miss = W.sum(1) < 1e-6
print('vertici senza peso:', int(miss.sum()))
if miss.any():   # i pochi vertici senza peso prendono quelli del vicino pesato
    from scipy.spatial import cKDTree
    P = np.array([v.co[:] for v in body.data.vertices])
    kd = cKDTree(P[~miss]); _, nn = kd.query(P[miss])
    W[miss] = W[~miss][nn]
W /= W.sum(1, keepdims=True)

# ------------------------------------------------------------------ 3. posa A -> posa T del gioco (skinning lineare)
def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9: return np.eye(3) if c > 0 else -np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1.0 / (1.0 + c))


# Busto, collo e testa restano come in MakeHuman (stessa altezza del gioco gia' con la scala): ruotarli sulle ossa
# del gioco piegherebbe la faccia in giu' (l'osso della testa del gioco e' inclinato in avanti). Si rimettono in
# posa solo braccia, mani e gambe.
KEEP = {'Hips', 'Spine', 'Spine1', 'Spine2', 'Neck', 'Head', 'LeftShoulder', 'RightShoulder'}
MT = []
for b in DEFORM:
    if b in KEEP:
        MT.append(np.eye(4)); continue
    h0 = J[MAP[b]]; t0 = np.array(TA.data.bones[b].tail_local[:])
    h1 = GB[b][0]
    c = child_of(b)
    t1 = GB[c][0] if (c and c in GB) else GB[b][1]
    d0, d1 = t0 - h0, t1 - h1
    R = rot_between(d0, d1)
    sc = np.linalg.norm(d1) / max(np.linalg.norm(d0), 1e-6)
    sc = np.clip(sc, 0.6, 1.6)
    n = d0 / np.linalg.norm(d0)
    S = np.eye(3) + (sc - 1.0) * np.outer(n, n)   # allunga solo lungo l'osso
    A = R @ S
    M = np.eye(4); M[:3, :3] = A; M[:3, 3] = h1 - A @ h0
    MT.append(M)
MT = np.array(MT)
P = np.array([v.co[:] for v in body.data.vertices])
Ph = np.c_[P, np.ones(len(P))]
blend = np.einsum('vb,bij->vij', W, MT)
P2 = np.einsum('vij,vj->vi', blend, Ph)[:, :3]
body.modifiers.clear(); body.parent = None
for v, p in zip(body.data.vertices, P2): v.co = p
for g in list(body.vertex_groups): body.vertex_groups.remove(g)
bpy.data.objects.remove(TA, do_unlink=True)
bpy.ops.object.select_all(action='DESELECT')

# lisciatura leggera dove lo skinning lineare stringe (spalle, ascelle)
sm = body.modifiers.new('S', 'CORRECTIVE_SMOOTH'); sm.factor = 0.5; sm.iterations = 4; sm.use_only_smooth = True
sm.rest_source = 'ORCO'
activate(body); bpy.ops.object.modifier_apply(modifier='S')

# ------------------------------------------------------------------ occhi: sfere con cornea, sull'osso della testa
MHead = MT[DEFORM.index('Head')]
eyes = []
for side, s_ in (('L', 'l'), ('R', 'r')):
    c0 = np.r_[J[f'{s_}-eye'], 1.0]
    c = (MHead @ c0)[:3]
    tgt = (MHead @ np.r_[J[f'{s_}-eye-target'], 1.0])[:3]
    fwd = (tgt - c); fwd /= np.linalg.norm(fwd)
    # raggio dagli occhi di MakeHuman (globo oculare ~12 mm)
    ev = D['V'][np.unique(D[f'{s_}_eye_fv'])] * 1.0
    r = 0.5 * np.ptp(ev[:, 0]) * s * 0.97   # mh_body.npz e' gia' in metri
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=r, location=tuple(c))
    e = bpy.context.active_object; e.name = 'Eye' + side
    # polo della sfera verso lo sguardo: le coordinate oggetto dicono dov'e' l'iride
    e.rotation_euler = Vector((0, 0, 1)).rotation_difference(Vector(fwd)).to_euler()
    for p in e.data.polygons: p.use_smooth = True
    eyes.append(e)

import skin
skin.SC = SC
skin.FAST = FAST
skin.apply_eyes(eyes, 512)   # cuoce l'occhio e applica le trasformazioni (prima dei pesi)

# ------------------------------------------------------------------ 4. scheletro del gioco e pesi
for i, b in enumerate(DEFORM):
    g = body.vertex_groups.new(name=PREFIX + b)
    idx = np.nonzero(W[:, i] > 1e-4)[0]
    for vi in idx: g.add([int(vi)], float(W[vi, i]), 'REPLACE')
for e in eyes:
    g = e.vertex_groups.new(name=PREFIX + 'Head'); g.add(list(range(len(e.data.vertices))), 1.0, 'REPLACE')
for ob in [body] + eyes:
    ob.parent = ARM
    m = ob.modifiers.new('Armature', 'ARMATURE'); m.object = ARM

# segni utili alla pelle (posizioni nella posa T, coordinate oggetto = mondo)
LM = {k: (MHead @ np.r_[J[k], 1.0])[:3] for k in ('l-eye', 'r-eye', 'mouth', 'jaw', 'head', 'head-2', 'neck',
                                                  'l-upperlid', 'r-upperlid')}
LM['hips'] = GB['Hips'][0]
json.dump({k: v.tolist() for k, v in LM.items()}, open(os.path.join(HERE, 'landmarks.json'), 'w'), indent=1)
print('punti del volto', {k: np.round(v, 3).tolist() for k, v in LM.items()})

# ------------------------------------------------------------------ 5. materiali
skin.LM = {k: np.array(v) for k, v in LM.items()}
skin.apply_skin(body, TEX)

# ------------------------------------------------------------------ esportazione
for im in list(bpy.data.images):   # texture del vecchio corpo dell'FBX: non servono
    if im.users == 0 or im.name.startswith('file'): bpy.data.images.remove(im)
activate(ARM)
for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, 'human.blend'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_skins=True, export_animations=False,
                          export_apply=False, export_yup=True, export_image_format='JPEG', export_jpeg_quality=92)
json.dump({"comment": "Corpo umano d'autore (Tools/ModelForge/human): anatomia MakeHuman CC0. La pelle e' una mappa di dettaglio "
                      "moltiplicata per la carnagione scelta nel garage.",
           "materials": {"Skin": {"tint": True}}},
          open(os.path.join(os.path.dirname(OUT), 'model.json'), 'w'), indent=1)
if '--copy-candidate' in ARGV:   # l'aspirante e' anche lui un umano: stesso corpo
    dst = os.path.join(CHARS, 'candidate'); os.makedirs(dst, exist_ok=True)
    shutil.copy(OUT, os.path.join(dst, 'model.glb')); shutil.copy(os.path.join(os.path.dirname(OUT), 'model.json'), os.path.join(dst, 'model.json'))
print('esportato', OUT, len(body.data.vertices), 'vertici')
