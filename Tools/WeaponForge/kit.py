# Attrezzi di modellazione per la forgia delle armi (Blender, bpy/bmesh).
#
# Convenzione: si lavora nello spazio dell'arma del gioco (x destra, y alto, z dietro: la canna punta verso -z) e
# ogni pezzo nasce gia' li'. All'esportazione si passa a Blender (g2b) e il glTF torna al gioco.
# Ogni pezzo porta il suo materiale (MATS) negli attributi dei vertici; la cottura (bake.py) ne ricava colore,
# ruvidita', metallo e rilievo (normal map).
import math, random
import numpy as np
import bpy, bmesh
from mathutils import Vector, Matrix

# materiale -> colore lineare, ruvidita', metallo, "nudo sotto" (spigoli che si consumano fino al metallo),
#              rilievo (0 niente, 1 venatura del legno, 2 metallo butterato, 3 lamiera martellata, 4 cuoio/tela),
#              ruggine (quanta)
MATS = {
    'steel':    ((0.022, 0.022, 0.024), 0.48, 0.9, 1, 2, 0.15),   # acciaio fosfatato, nero-grigio
    'blued':    ((0.016, 0.018, 0.026), 0.38, 0.95, 1, 2, 0.1),   # brunito
    'bare':     ((0.16, 0.158, 0.152), 0.32, 1.0, 1, 2, 0.2),     # acciaio nudo (lame, otturatori)
    'wood':     ((0.03, 0.012, 0.005), 0.55, 0.0, 0, 1, 0.0),     # noce oliato
    'wood_dark':((0.013, 0.0065, 0.0035), 0.5, 0.0, 0, 1, 0.0),
    'leather':  ((0.026, 0.016, 0.01), 0.62, 0.0, 0, 4, 0.0),
    'rubber':   ((0.014, 0.014, 0.014), 0.85, 0.0, 0, 4, 0.0),
    'brass':    ((0.22, 0.14, 0.045), 0.42, 1.0, 1, 2, 0.05),
    'copper':   ((0.26, 0.09, 0.04), 0.4, 1.0, 1, 2, 0.05),
    'paint_k':  ((0.045, 0.043, 0.03), 0.72, 0.25, 1, 2, 0.2),   # Regno: verde-oliva spento
    'paint_v':  ((0.03, 0.012, 0.012), 0.6, 0.35, 1, 2, 0.1),    # Vanguardia: rosso sangue scurissimo
    'scrap':    ((0.055, 0.032, 0.02), 0.7, 0.75, 1, 3, 0.75),   # lamiera dei Vrepigh, arrugginita
    'scrap_red':((0.09, 0.018, 0.01), 0.75, 0.3, 1, 3, 0.55),    # lamiera dipinta di rosso, scrostata
    'cloth':    ((0.05, 0.042, 0.028), 0.92, 0.0, 0, 4, 0.0),
    'bone':     ((0.32, 0.27, 0.19), 0.6, 0.0, 0, 2, 0.0),
    'glow':     ((0.2, 0.05, 0.02), 0.3, 0.2, 0, 0, 0.0),
    'hazard':   ((0.16, 0.1, 0.012), 0.7, 0.2, 1, 2, 0.15),    # strisce di pericolo gialle, sporche
    'ceramic':  ((0.2, 0.17, 0.13), 0.35, 0.0, 0, 0, 0.0),     # isolatori di ceramica
    'gunmetal': ((0.035, 0.035, 0.038), 0.42, 0.95, 1, 2, 0.08),
}


class Builder:
    """raccoglie i pezzi (bmesh) con il loro materiale; build() li fonde in un oggetto"""

    def __init__(self, name):
        self.name = name
        self.parts = []           # (bmesh, materiale)
        self.glow = []            # bmesh luminosi
        self.cutters = []         # (bmesh da sottrarre, filtro sui pezzi)
        self.glow_color = (1.0, 0.3, 0.1)

    def add(self, bm, mat):
        if mat == 'glow': self.glow.append(bm)
        else: self.parts.append((bm, mat))
        return bm

    def cut(self, bm):
        self.cutters.append(bm)


# ---------------------------------------------------------------------------------------------- primitive
def _bm_from(verts, faces):
    bm = bmesh.new()
    vs = [bm.verts.new(v) for v in verts]
    for f in faces:
        try: bm.faces.new([vs[i] for i in f])
        except ValueError: pass
    bm.normal_update()
    return bm


def transform(bm, M):
    bmesh.ops.transform(bm, matrix=M, verts=bm.verts)
    return bm


def translate(bm, t):
    return transform(bm, Matrix.Translation(Vector(t)))


def bevel_edges(bm, width, segs=2, angle=30.0):
    """smussa gli spigoli piu' vivi di 'angle' gradi"""
    if width <= 1e-6: return bm
    bm.edges.ensure_lookup_table()
    edges = [e for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0.0) > math.radians(angle)]
    if edges:
        bmesh.ops.bevel(bm, geom=edges, offset=width, segments=segs, profile=0.5, affect='EDGES', clamp_overlap=True)
    return bm


def box(c, size, bev=0.0, segs=2):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    bevel_edges(bm, bev, segs)
    return translate(bm, c)


def profile(points, x0, x1, bev=0.0, segs=2, angle=25.0):
    """sagoma laterale (punti (z, y) nello spazio dell'arma) estrusa da x0 a x1"""
    pts = list(points)
    # orientamento antiorario visto da +x
    area = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))
    if area > 0: pts = pts[::-1]
    n = len(pts)
    verts = [(x0, y, z) for z, y in pts] + [(x1, y, z) for z, y in pts]
    faces = [list(range(n))[::-1], list(range(n, 2 * n))]
    for i in range(n):
        j = (i + 1) % n
        faces.append([i, j, n + j, n + i])
    bm = _bm_from(verts, faces)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    # una sagoma concava va triangolata prima della smussatura
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4 and abs(f.normal.x) > 0.9])
    bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(1.0), verts=bm.verts[:], edges=bm.edges[:])
    bevel_edges(bm, bev, segs, angle)
    return bm


def lathe(prof, axis='z', segs=24, c=(0, 0, 0), cap=True):
    """solido di rotazione: prof = [(posizione lungo l'asse, raggio), ...]"""
    verts, faces = [], []
    n = len(prof)
    for i, (t, r) in enumerate(prof):
        for k in range(segs):
            a = 2 * math.pi * k / segs
            u, v = r * math.cos(a), r * math.sin(a)
            if axis == 'z': verts.append((u, v, t))
            elif axis == 'x': verts.append((t, u, v))
            else: verts.append((u, t, v))
    for i in range(n - 1):
        for k in range(segs):
            k1 = (k + 1) % segs
            faces.append([i * segs + k, i * segs + k1, (i + 1) * segs + k1, (i + 1) * segs + k])
    if cap:
        if prof[0][1] > 1e-6: faces.append([k for k in range(segs)][::-1])
        if prof[-1][1] > 1e-6: faces.append([(n - 1) * segs + k for k in range(segs)])
    bm = _bm_from(verts, faces)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    return translate(bm, c)


def cyl(c, r, length, axis='z', segs=20, bev=0.0):
    h = length / 2
    p = [(-h, r), (h, r)] if bev <= 0 else [(-h, r - bev), (-h + bev, r), (h - bev, r), (h, r - bev)]
    return lathe(p, axis, segs, c)


def tube(points, r0, r1=None, segs=10, closed_ends=True):
    """tubo lungo una polilinea (cavi, fili, tubi del carburante)"""
    r1 = r0 if r1 is None else r1
    P = [np.array(p, float) for p in points]
    n = len(P)
    T = [P[min(i + 1, n - 1)] - P[max(i - 1, 0)] for i in range(n)]
    T = [t / (np.linalg.norm(t) + 1e-9) for t in T]
    up = np.array([0, 1.0, 0]) if abs(T[0][1]) < 0.9 else np.array([1.0, 0, 0])
    verts, faces = [], []
    nprev = np.cross(T[0], up); nprev /= np.linalg.norm(nprev)
    for i in range(n):
        nn = nprev - T[i] * (nprev @ T[i]); nn /= np.linalg.norm(nn); nprev = nn
        bb = np.cross(T[i], nn)
        r = r0 + (r1 - r0) * i / max(n - 1, 1)
        for k in range(segs):
            a = 2 * math.pi * k / segs
            verts.append(tuple(P[i] + r * (math.cos(a) * nn + math.sin(a) * bb)))
    for i in range(n - 1):
        for k in range(segs):
            k1 = (k + 1) % segs
            faces.append([i * segs + k, i * segs + k1, (i + 1) * segs + k1, (i + 1) * segs + k])
    if closed_ends:
        faces.append(list(range(segs))[::-1]); faces.append([(n - 1) * segs + k for k in range(segs)])
    return _bm_from(verts, faces)


def helix(c, r, pitch, length, wire, axis='z', turns_seg=10):
    """spirale (bobine, filo avvolto)"""
    pts = []
    turns = length / pitch
    nseg = max(8, int(turns * turns_seg))
    for i in range(nseg + 1):
        t = i / nseg
        a = 2 * math.pi * turns * t
        z = -length / 2 + length * t
        if axis == 'z': pts.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a), c[2] + z))
        else: pts.append((c[0] + z, c[1] + r * math.cos(a), c[2] + r * math.sin(a)))
    return tube(pts, wire, segs=6)


def rivet(c, r, normal=(1, 0, 0)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=5, radius=r)
    for v in bm.verts: v.co.z = max(v.co.z, -r * 0.1) * 0.55
    n = Vector(normal).normalized()
    R = n.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    return transform(bm, Matrix.Translation(Vector(c)) @ R)


def wedge(base_c, base_size, tip, axis_out):
    """punta/dente: base rettangolare, punta verso 'tip'"""
    bx, by = base_size
    a = np.array(axis_out, float); a /= np.linalg.norm(a)
    u = np.cross(a, [0, 0, 1.0]) if abs(a[2]) < 0.9 else np.cross(a, [1.0, 0, 0]); u /= np.linalg.norm(u)
    v = np.cross(a, u)
    b = np.array(base_c, float)
    verts = [tuple(b + u * sx * bx / 2 + v * sy * by / 2) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))] + [tuple(tip)]
    faces = [[3, 2, 1, 0], [0, 1, 4], [1, 2, 4], [2, 3, 4], [3, 0, 4]]
    bm = _bm_from(verts, faces)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    return bm


def mirror_x(bm):
    c = bm.copy()
    transform(c, Matrix.Scale(-1, 4, (1, 0, 0)))
    bmesh.ops.reverse_faces(c, faces=c.faces[:])
    return c


def merge(bms):
    out = bmesh.new()
    for b in bms:
        me = bpy.data.meshes.new('t'); b.to_mesh(me); out.from_mesh(me); bpy.data.meshes.remove(me)
    return out


# ---------------------------------------------------------------------------------------------- oggetti
def to_object(name, bm):
    me = bpy.data.meshes.new(name); bm.to_mesh(me)
    ob = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(ob)
    return ob


def apply_boolean(target, cutter_objs):
    """sottrae i tagli (fori, feritoie, finestre d'espulsione) dall'oggetto"""
    if not cutter_objs: return
    bpy.context.view_layer.objects.active = target
    for c in cutter_objs:
        m = target.modifiers.new('cut', 'BOOLEAN'); m.operation = 'DIFFERENCE'; m.object = c; m.solver = 'EXACT'
        try: bpy.ops.object.modifier_apply(modifier=m.name)
        except RuntimeError: target.modifiers.remove(m)
    for c in cutter_objs: bpy.data.objects.remove(c)


def boolean_bm(bm, cutters, solver='EXACT'):
    """bm meno i tagli (lista di bmesh): ritorna un bmesh nuovo"""
    if not cutters: return bm
    tgt = to_object('bool_t', bm)
    cobs = [to_object('bool_c', c) for c in cutters]
    bpy.context.view_layer.objects.active = tgt
    for c in cobs:
        m = tgt.modifiers.new('cut', 'BOOLEAN'); m.operation = 'DIFFERENCE'; m.object = c; m.solver = solver
        try: bpy.ops.object.modifier_apply(modifier=m.name)
        except RuntimeError: tgt.modifiers.remove(m)
    out = bmesh.new(); out.from_mesh(tgt.data)
    for o in [tgt] + cobs:
        me = o.data; bpy.data.objects.remove(o); bpy.data.meshes.remove(me)
    return out


def array_bm(make, n, step):
    """n copie di make(i), ognuna spostata di step*i"""
    out = []
    for i in range(n):
        b = make(i); translate(b, tuple(np.array(step) * i)); out.append(b)
    return merge(out)
