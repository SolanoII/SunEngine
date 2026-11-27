# Modello d'autore della Vanguard Armor: un umano potenziato chiuso in un'armatura pesante a piastre.
#
#   python Tools/ModelForge/vanguard/build.py                    -> Models/Characters/vanguard/model.glb (armatura generica)
#   python Tools/ModelForge/vanguard/build.py --variant ordo     -> Models/Characters/vanguard/variants/vanguard_konrad.glb
#
# Com'e' fatto (vedi README.md):
#   - tuta sotto (campo di distanza -> marching cubes): si vede solo ai giunti, pesi di Blender (calore)
#   - armatura a piastre rigide: lamiere spesse a sfaccettature (loft di sezioni ottagonali con la chiglia), pieghe
#     arrotondate e bordi smussati netti, ognuna sul suo osso (le piastre non si stirano, scorrono una sull'altra);
#     spallacci a cupola con lame, collare alto, zaino-reattore, pistoni idraulici, cintura con tasche
#   - nucleo di pietra sul petto: ossidiana viola (generica) o nera incastonata nella pietra bianca (Ordo)
#   - texture cotte con Cycles: colore (usura sugli spigoli, sporco negli incavi), rugosita'/metallo, normali fini
import sys, os, json, math
import numpy as np
import bpy, bmesh
from mathutils import Vector
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import photo_lib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
def arg(n, d=None): return ARGV[ARGV.index(n) + 1] if n in ARGV else d

VAR = arg('--variant', 'generic')
ORDO = VAR == 'ordo'
CHARS = os.path.join(ROOT, 'Sandbox/GameData/Models/Characters/vanguard')
OUT = os.path.abspath(arg('--out', os.path.join(CHARS, 'variants', 'vanguard_konrad.glb') if ORDO else os.path.join(CHARS, 'model.glb')))
TEX = int(arg('--tex', '2048'))
FAST = '--fast' in ARGV          # senza cottura (anteprime veloci)

R = json.load(open(os.path.join(HERE, 'rig.json')))
J = {k: np.array(v, float) for k, v in R['joints'].items()}
PAR = R['parent']
PREFIX = R['prefix']

bpy.ops.wm.read_factory_settings(use_empty=True)
SC = bpy.context.scene
COL = SC.collection
PARTS = []      # (oggetto, pesi {osso: peso} o 'near', materiale)


def activate(ob):
    for o in bpy.context.view_layer.objects: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


def link(name, verts, faces):
    me = bpy.data.meshes.new(name); me.from_pydata([tuple(map(float, v)) for v in verts], [], [tuple(int(i) for i in f) for f in faces]); me.update()
    ob = bpy.data.objects.new(name, me); COL.objects.link(ob)
    return ob


def bake_modifiers(ob):
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    old = ob.data; ob.modifiers.clear(); ob.data = me; bpy.data.meshes.remove(old)


def norm(v): v = np.asarray(v, float); return v / max(np.linalg.norm(v), 1e-9)


def frame(z, hint=(0, 0, 1)):
    """terna ortonormale con ez lungo z"""
    ez = norm(z); h = np.asarray(hint, float)
    if abs(ez @ norm(h)) > 0.95: h = np.array([0, -1.0, 0]) if abs(ez[1]) < 0.9 else np.array([1.0, 0, 0])
    ex = norm(np.cross(h, ez)); ey = np.cross(ez, ex)
    return ex, ey, ez


AXES = (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0]))


def sp(t, e): return np.sign(t) * np.abs(t) ** e


# ------------------------------------------------------------------ primitive: superellissoide
def superq(center, axes, r, pu=3.0, pv=3.0, u=(-math.pi, math.pi), v=(-math.pi / 2, math.pi / 2), nu=48, nv=24, taper=(0.0, 0.0), keel=0.0, flare=0.0):
    """griglia di punti sulla superficie (u attorno all'asse z locale, v dal basso in alto). Ritorna V, F, chiuso_u"""
    ex, ey, ez = axes; a, b, c = r
    full = abs((u[1] - u[0]) - 2 * math.pi) < 1e-6
    U = np.linspace(u[0], u[1], nu, endpoint=not full)
    W = np.linspace(v[0], v[1], nv + 1)
    V = []
    for w in W:
        cw = sp(math.cos(w), 2 / pv); sw = sp(math.sin(w), 2 / pv)
        for uu in U:
            lx = a * cw * sp(math.cos(uu), 2 / pu); ly = b * cw * sp(math.sin(uu), 2 / pu); lz = c * sw
            t = lz / c                                         # -1 in basso, +1 in alto
            lx *= 1 + taper[0] * t + flare * t * t; ly *= 1 + taper[1] * t + flare * t * t
            if keel and ly < 0: ly -= keel * max(0.0, 1 - abs(lx) / a) ** 1.3 * (1 - 0.6 * t * t)   # prua in avanti (-y locale)
            V.append(center + ex * lx + ey * ly + ez * lz)
    F = []
    n = len(U)
    for i in range(nv):
        for j in range(n if full else n - 1):
            j2 = (j + 1) % n
            F.append((i * n + j, i * n + j2, (i + 1) * n + j2, (i + 1) * n + j))
    return np.array(V), F, full


def orient_out(ob, center):
    """normali verso l'esterno (rispetto al centro del guscio)"""
    me = ob.data; me.update()
    s = 0.0
    for p in me.polygons: s += (Vector(p.center) - Vector(center)).dot(p.normal) * p.area
    if s < 0:
        bm = bmesh.new(); bm.from_mesh(me); bmesh.ops.reverse_faces(bm, faces=bm.faces[:]); bm.to_mesh(me); bm.free()


def finish(ob, thick=0.0, bevel=0.0, smooth=0.6, inward=True):
    if thick > 0:
        m = ob.modifiers.new('s', 'SOLIDIFY'); m.thickness = thick; m.offset = -1.0 if inward else 0.0; m.use_even_offset = True; m.use_rim = True
    if bevel > 0:
        m = ob.modifiers.new('b', 'BEVEL'); m.width = bevel; m.segments = 3; m.limit_method = 'ANGLE'; m.angle_limit = math.radians(30); m.profile = 0.6
    bake_modifiers(ob)
    for p in ob.data.polygons: p.use_smooth = True
    activate(ob)
    try: bpy.ops.object.shade_smooth_by_angle(angle=smooth)
    except Exception: pass


def shell(name, bone, center, axes, r, pu=3, pv=3, u=(-math.pi, math.pi), v=(-math.pi / 2, math.pi / 2), thick=0.025, bevel=0.005,
          mat='Plate', nu=40, nv=20, closed=False, weights=None, taper=(0.0, 0.0), keel=0.0, flare=0.0):
    center = np.asarray(center, float)
    V, F, full = superq(center, axes, r, pu, pv, u, v, nu, nv, taper, keel, flare)
    ob = link(name, V, F)
    bm = bmesh.new(); bm.from_mesh(ob.data); bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5); bm.to_mesh(ob.data); bm.free()
    orient_out(ob, center)
    finish(ob, 0.0 if closed else thick, bevel)
    PARTS.append((ob, weights or {bone: 1.0}, mat))
    return ob


def block(name, bone, center, axes, r, p=8.0, mat='Trim', weights=None, bevel=0.0, taper=(0.0, 0.0), keel=0.0):
    """blocco chiuso dagli spigoli arrotondati"""
    return shell(name, bone, center, axes, r, pu=p, pv=p, closed=True, bevel=bevel, mat=mat, nu=28, nv=14, weights=weights, taper=taper, keel=keel)


def limb_axes(a, b, hint=(0, 0, 1)):
    ex, ey, ez = frame(np.asarray(b) - np.asarray(a), hint)
    return ex, ey, ez


def mid(a, b, t=0.5): return np.asarray(a) * (1 - t) + np.asarray(b) * t


# ------------------------------------------------------------------ tuta sotto (campo di distanza)
def sdf_body(P):
    def caps(p, a, b, ra, rb):
        a = np.asarray(a); b = np.asarray(b); pa = p - a; ba = b - a
        h = np.clip((pa @ ba) / (ba @ ba), 0, 1)
        return np.linalg.norm(pa - h[:, None] * ba, axis=1) - (ra + (rb - ra) * h)
    def ell(p, c, r):
        q = (p - np.asarray(c)) / np.asarray(r)
        return (np.linalg.norm(q, axis=1) - 1) * min(r)
    def smin(a, b, k=0.05):
        h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1); return b * (1 - h) + a * h - k * h * (1 - h)
    d = ell(P, (0, 0.0, 1.66), (0.27, 0.17, 0.20))
    for prim in (ell(P, (0, 0.0, 1.48), (0.22, 0.15, 0.20)), ell(P, (0, -0.01, 1.27), (0.20, 0.15, 0.14)),
                 caps(P, (0, 0.03, 1.78), (0, 0.02, 1.96), 0.085, 0.08), ell(P, (0, 0.005, 2.02), (0.105, 0.125, 0.13))):
        d = smin(d, prim, 0.06)
    for s in (1, -1):
        L = lambda n: J[n] * np.array([s, 1, 1])
        for prim in (caps(P, L('LeftShoulder') + [0.06 * s, 0, -0.02], L('LeftArm'), 0.09, 0.09), caps(P, L('LeftArm'), L('LeftForeArm'), 0.085, 0.072),
                     caps(P, L('LeftForeArm'), L('LeftHand'), 0.07, 0.052), caps(P, L('LeftHand'), L('LeftHandMiddle1') + [0.02 * s, 0, 0], 0.045, 0.04),
                     caps(P, L('LeftUpLeg') + [0, 0, 0.04], L('LeftLeg'), 0.12, 0.085), caps(P, L('LeftLeg'), L('LeftFoot'), 0.08, 0.058),
                     caps(P, L('LeftFoot') + [0, 0.02, -0.06], L('LeftToeBase') + [0, -0.03, 0.03], 0.058, 0.045)):
            d = smin(d, prim, 0.04)
        # dita: tubi sottili lungo le falangi
        for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
            ch = [L(f'LeftHand{f}{k}') for k in (1, 2, 3)]
            tip = ch[2] + (ch[2] - ch[1]) * 0.9
            for a, b in zip(ch + [], ch[1:] + [tip]): d = smin(d, caps(P, a, b, 0.017, 0.015), 0.012)
    return d


def build_suit():
    from skimage import measure
    h = 0.011
    lo = np.array([-1.25, -0.30, -0.02]); hi = np.array([1.25, 0.32, 2.22])
    n = np.ceil((hi - lo) / h).astype(int)
    xs, ys, zs = [lo[i] + np.arange(n[i]) * h for i in range(3)]
    vol = np.empty(n, np.float32)
    for i, x in enumerate(xs):
        Y, Z = np.meshgrid(ys, zs, indexing='ij')
        P = np.stack([np.full(Y.size, x), Y.ravel(), Z.ravel()], 1)
        vol[i] = sdf_body(P).reshape(Y.shape)
    v, f, _, _ = measure.marching_cubes(vol, 0.0, spacing=(h, h, h))
    v += lo
    ob = link('Suit', v, f[:, ::-1])
    activate(ob)
    m = ob.modifiers.new('d', 'DECIMATE'); m.ratio = 0.22
    m = ob.modifiers.new('sm', 'SMOOTH'); m.factor = 0.5; m.iterations = 4
    bake_modifiers(ob)
    for p in ob.data.polygons: p.use_smooth = True
    print('tuta', len(ob.data.vertices), len(ob.data.polygons))
    return ob


# ------------------------------------------------------------------ carapace
def cross_pattee(name, bone, center, axes, size, depth=0.008, mat='Cross'):
    """croce patente (bracci che si allargano), in rilievo sulla piastra: asse x/y nel piano, z verso fuori"""
    ex, ey, ez = axes; s = size
    out = []
    for k in range(4):
        a = k * math.pi / 2
        dx = ex * math.cos(a) + ey * math.sin(a); dy = -ex * math.sin(a) + ey * math.cos(a)
        out += [center + dy * 0.10 * s, center + dx * 0.5 * s + dy * 0.24 * s, center + dx * 0.5 * s - dy * 0.24 * s, center - dy * 0.10 * s]
    pts = np.array(out)
    V = list(pts) + list(pts + ez * depth)
    n = len(pts)
    F = [tuple(range(n))[::-1], tuple(range(n, 2 * n))] + [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    ob = link(name, V, F)
    finish(ob, 0, 0.0015)
    PARTS.append((ob, {bone: 1.0}, mat))


def gem(name, bone, center, axes, r, mat='Core'):
    """pietra sfaccettata (bipiramide ottagonale) incastonata"""
    ex, ey, ez = axes
    V = [center + ez * r * 0.55, center - ez * r * 0.25]
    ring = []
    for k in range(8):
        a = k * math.pi / 4 + math.pi / 8
        ring.append(center + (ex * math.cos(a) + ey * math.sin(a)) * r * (1.25 if k % 2 == 0 else 1.0) + ez * r * 0.05)
    V += ring
    F = [(0, 2 + k, 2 + (k + 1) % 8) for k in range(8)] + [(1, 2 + (k + 1) % 8, 2 + k) for k in range(8)]
    ob = link(name, V, F)
    finish(ob, 0, 0, smooth=0.1)
    PARTS.append((ob, {bone: 1.0}, mat))
    return ob


def ring(name, bone, center, axes, r, tube, mat='Trim', nu=40):
    """anello (toro) nel piano xy delle assi"""
    ex, ey, ez = axes; V = []; F = []; nv = 10
    for i in range(nu):
        a = 2 * math.pi * i / nu; d = ex * math.cos(a) + ey * math.sin(a)
        for j in range(nv):
            b = 2 * math.pi * j / nv
            V.append(center + d * (r + tube * math.cos(b)) + ez * tube * math.sin(b))
    for i in range(nu):
        for j in range(nv):
            F.append((i * nv + j, i * nv + (j + 1) % nv, ((i + 1) % nu) * nv + (j + 1) % nv, ((i + 1) % nu) * nv + j))
    ob = link(name, V, F); finish(ob, 0, 0); PARTS.append((ob, {bone: 1.0}, mat))


def rivets(name, bone, pts, normals, r=0.0085, weights=None, mat='Trim'):
    """chiodi a testa tonda (mezze sfere schiacciate) nei punti dati"""
    V, F = [], []
    for p_, n_ in zip(pts, normals):
        ex, ey, ez = frame(n_)
        base = len(V)
        for i in range(4):
            vv = (i / 3) * math.pi / 2
            for k in range(8):
                a_ = 2 * math.pi * k / 8
                V.append(p_ + (ex * math.cos(a_) + ey * math.sin(a_)) * r * math.cos(vv) + ez * r * 0.6 * math.sin(vv))
        for i in range(3):
            for k in range(8):
                F.append((base + i * 8 + k, base + i * 8 + (k + 1) % 8, base + (i + 1) * 8 + (k + 1) % 8, base + (i + 1) * 8 + k))
    ob = link(name, V, F)
    bm = bmesh.new(); bm.from_mesh(ob.data); bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6); bm.to_mesh(ob.data); bm.free()
    finish(ob, 0, 0)
    PARTS.append((ob, weights or {bone: 1.0}, mat))


def rivet_row(name, bone, center, axes, r, u0, u1, n, v, weights=None, pu=3, pv=3, taper=(0.0, 0.0), out=0.004):
    """fila di chiodi lungo il bordo di un guscio (stessa forma del guscio, alla quota v)"""
    pts, nrm = [], []
    for k in range(n):
        uu = u0 + (u1 - u0) * (k + 0.5) / n
        V, _, _ = superq(np.asarray(center, float), axes, r, pu, pv, (uu, uu + 1e-3), (v, v + 1e-3), 2, 1, taper)
        p_ = V[0]; d_ = norm(p_ - np.asarray(center, float))
        pts.append(p_ + d_ * out); nrm.append(d_)
    rivets(name, bone, pts, nrm, weights=weights)


def tube(name, pts, r, weights='near', mat='Trim', sides=10, ribs=0):
    """tubo lungo una polilinea (cavi, tubi del fluido); ribs = anelli di rinforzo"""
    pts = [np.asarray(q, float) for q in pts]
    P = []
    for a_, b_ in zip(pts[:-1], pts[1:]):
        for t in np.linspace(0, 1, 6, endpoint=False): P.append(a_ * (1 - t) + b_ * t)
    P.append(pts[-1]); P = np.array(P)
    V, F = [], []
    for i, c in enumerate(P):
        tng = norm(P[min(i + 1, len(P) - 1)] - P[max(i - 1, 0)])
        ex, ey, _ = frame(tng)
        rr = r * (1.25 if ribs and i % ribs == 0 else 1.0)
        for k in range(sides):
            a_ = 2 * math.pi * k / sides
            V.append(c + (ex * math.cos(a_) + ey * math.sin(a_)) * rr)
    for i in range(len(P) - 1):
        for k in range(sides):
            F.append((i * sides + k, i * sides + (k + 1) % sides, (i + 1) * sides + (k + 1) % sides, (i + 1) * sides + k))
    ob = link(name, V, F); finish(ob, 0, 0)
    PARTS.append((ob, weights, mat))


def chain(name, pts, link_r=0.016, weights='near'):
    """catena: anelli alternati lungo una polilinea"""
    pts = [np.asarray(q, float) for q in pts]
    P = []
    for a_, b_ in zip(pts[:-1], pts[1:]):
        n_ = max(1, int(np.linalg.norm(b_ - a_) / (link_r * 1.6)))
        for t in np.linspace(0, 1, n_, endpoint=False): P.append(a_ * (1 - t) + b_ * t)
    for i, c in enumerate(P):
        tng = norm(P[min(i + 1, len(P) - 1)] - P[max(i - 1, 0)])
        ex, ey, ez = frame(tng)
        ax_ = (ez, ex if i % 2 else ey, ey if i % 2 else ex)
        ring(f'{name}{i}', None, c, ax_, link_r, link_r * 0.28, mat='Trim', nu=12)
        PARTS[-1] = (PARTS[-1][0], weights, 'Trim')


# ------------------------------------------------------------------ piastre sfaccettate (armatura pesante)
# Le piastre sono lamiere spesse piegate a sfaccettature (sezione ottagonale con la chiglia davanti), con spigoli
# smussati netti: niente cuscini arrotondati. Ogni pezzo e' un loft di anelli (o di archi aperti) con spessore vero.
FWD = np.array([0, -1.0, 0]); RIGHT = np.array([1.0, 0, 0]); UP = np.array([0, 0, 1.0])
FRONT_IDX = [7, 8, 9, 0, 1, 2, 3]     # da un fianco, per il davanti, all'altro fianco
BACK_IDX = [3, 4, 5, 6, 7]


def octo(a, b, c=0.36, keel=0.0):
    """sezione a rettangolo smussato con la chiglia: 0 = centro davanti (-y), poi in senso +x"""
    ca, cb = a * c, b * c
    return np.array([(0, -b - keel), (a - ca, -b), (a, -b + cb), (a, b - cb), (a - ca, b), (0, b),
                     (-a + ca, b), (-a, b - cb), (-a, -b + cb), (-a + ca, -b)], float)


def sect(c, kd, sd, a, b, cc=0.36, keel=0.0):
    c = np.asarray(c, float)
    return np.array([c + sd * x - kd * y for x, y in octo(a, b, cc, keel)])


def hard(ob, thick, bevel, smooth=0.35, rnd=0.0):
    """pieghe della lamiera arrotondate (rnd), spessore verso l'interno, smusso netto (due segmenti) sui bordi"""
    if rnd > 0:
        m = ob.modifiers.new('r', 'BEVEL'); m.width = rnd; m.segments = 3; m.limit_method = 'ANGLE'; m.angle_limit = math.radians(18)
        m.profile = 0.5; m.use_clamp_overlap = True
    if thick > 0:
        m = ob.modifiers.new('s', 'SOLIDIFY'); m.thickness = thick; m.offset = -1.0; m.use_even_offset = True; m.use_rim = True
        bevel = min(bevel, thick * 0.3)
    if bevel > 0:
        m = ob.modifiers.new('b', 'BEVEL'); m.width = bevel; m.segments = 2; m.limit_method = 'ANGLE'; m.angle_limit = math.radians(25 if rnd <= 0 else 50)
        m.profile = 0.7; m.miter_outer = 'MITER_ARC'
    bake_modifiers(ob)
    for p in ob.data.polygons: p.use_smooth = True
    activate(ob)
    try: bpy.ops.object.shade_smooth_by_angle(angle=smooth)
    except Exception: pass


def loft(name, bone, rings, closed=True, thick=0.03, bevel=0.004, mat='Plate', weights=None, center=None, cap_top=False, smooth=0.35, rnd=0.014):
    rings = [np.asarray(r, float) for r in rings]; n = len(rings[0]); nr = len(rings)
    V = np.concatenate(rings); F = []
    for i in range(nr - 1):
        for j in range(n if closed else n - 1):
            j2 = (j + 1) % n
            F.append((i * n + j, i * n + j2, (i + 1) * n + j2, (i + 1) * n + j))
    if cap_top and closed: F.append(tuple(range((nr - 1) * n, nr * n)))
    ob = link(name, V, F)
    orient_out(ob, np.asarray(center, float) if center is not None else V.mean(0))
    hard(ob, thick, bevel, smooth, rnd)
    PARTS.append((ob, weights or {bone: 1.0}, mat))
    return ob


def limb(name, bone, P0, P1, kd, specs, idx=None, cc=0.36, thick=0.03, bevel=0.004, mat='Plate', weights=None, rnd=0.012):
    """piastra lungo un arto: specs = [(t lungo l'arto, mezza larghezza, mezza profondita', chiglia)]"""
    P0 = np.asarray(P0, float); P1 = np.asarray(P1, float); ax = norm(P1 - P0)
    kd = np.asarray(kd, float); kd = norm(kd - ax * (kd @ ax)); sd = np.cross(ax, kd)
    rings = []
    for t, a, b, k in specs:
        pts = sect(P0 + (P1 - P0) * t, kd, sd, a, b, cc, k)
        rings.append(pts if idx is None else pts[idx])
    return loft(name, bone, rings, closed=idx is None, thick=thick, bevel=bevel, mat=mat, weights=weights, center=mid(P0, P1), rnd=rnd)


def tring(z, y, a, b, keel, cc=0.38, idx=None, x=0.0):
    pts = sect((x, y, z), FWD, RIGHT, a, b, cc, keel)
    return pts if idx is None else pts[idx]


def arc(x, cy, zc, ry, rz, th0=200.0, th1=-25.0, n=8, ridge=0.0):
    """arco sfaccettato nel piano y-z (spallacci): da davanti in basso, sopra, a dietro in basso"""
    out = []
    for k in range(n):
        th = math.radians(th0 + (th1 - th0) * k / (n - 1))
        out.append((x, cy + ry * math.cos(th), zc + rz * math.sin(th) + ridge * max(0.0, math.cos(th - math.pi / 2)) ** 14))
    return np.array(out)


def edge_rivets(name, bone, pts, center, weights=None, r=0.0075, out=0.006):
    pts = np.asarray(pts, float); c = np.asarray(center, float)
    nrm = [norm(p - c) for p in pts]
    rivets(name, bone, [p + n_ * out for p, n_ in zip(pts, nrm)], nrm, r=r, weights=weights, mat='Steel')


def build_armor_left():
    """piastre del lato sinistro (poi specchiate) e del centro"""
    j = J; PI = math.pi
    # ---- corazza: botte sfaccettata con la chiglia, larga alle spalle, stretta in vita
    CU = [(1.50, 0.0, 0.24, 0.185, 0.03), (1.60, -0.005, 0.28, 0.205, 0.045), (1.72, -0.01, 0.315, 0.22, 0.06),
          (1.84, 0.0, 0.315, 0.215, 0.05), (1.935, 0.02, 0.235, 0.175, 0.02)]
    loft('Cuirass', 'Spine2', [tring(*s) for s in CU], thick=0.036, bevel=0.005, center=(0, 0.0, 1.7))
    # pettorali sovrapposti, divisi da una fessura sulla chiglia
    PE = [(1.62, -0.005, 0.30, 0.23, 0.05), (1.73, -0.01, 0.335, 0.245, 0.065), (1.86, 0.0, 0.325, 0.235, 0.05)]
    for s_ in (1, -1):
        rings = []
        for i_, (z, y, a, b, k) in enumerate(PE):
            p = tring(z, y, a, b, k)
            q = p[[0, 1, 2, 3]] if s_ > 0 else p[[0, 9, 8, 7]]
            r_ = np.array([q[0] + (q[1] - q[0]) * 0.13, q[1], q[2], q[2] + (q[3] - q[2]) * 0.3])
            if i_ == 0: r_[:, 2] += np.array([-0.045, -0.02, 0.025, 0.04])     # bordo inferiore a V: punta in basso verso il centro
            rings.append(r_)
        loft('Pec' + ('L' if s_ > 0 else 'R'), 'Spine2', rings, closed=False, thick=0.026, bevel=0.004, center=(0, 0.05, 1.74))
        edge_rivets('RivPec' + str(s_), 'Spine2', [r_[3] + (r_[2] - r_[3]) * 0.12 for r_ in rings] , (0, 0.05, 1.74))
    # nucleo: alloggiamento d'acciaio sulla chiglia, anello, pietra, due sbarre di protezione
    gax = (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, -1.0, 0]))
    gc = np.array([0, -0.345, 1.725])
    block('CoreHousing', 'Spine2', gc + [0, 0.03, 0], AXES, (0.078, 0.035, 0.078), p=10, mat='Plate' if ORDO else 'Steel', bevel=0.003)
    ring('CoreRing', 'Spine2', gc + [0, -0.008, 0], gax, 0.06, 0.011, mat='Trim')
    gem('Core', 'Spine2', gc + [0, -0.01, 0], gax, 0.046, mat='Core')
    if not ORDO:
        for x_ in (-0.024, 0.024): block(f'CoreBar{x_}', 'Spine2', gc + [x_, -0.03, 0], AXES, (0.0055, 0.006, 0.07), p=8, mat='Steel')
    # fianchi sotto le ascelle
    for s_ in (1, -1):
        loft('Flank' + str(s_), 'Spine2', [tring(z, 0.02, 0.33, 0.2, 0.0, idx=([2, 3, 4] if s_ > 0 else [8, 7, 6])) for z in (1.56, 1.76)],
             closed=False, thick=0.02, bevel=0.003, mat='Steel', center=(0, 0.02, 1.66))
    # addome: tre lame a embrice davanti, una piastra dietro
    for k, (z0, bn) in enumerate(((1.535, 'Spine1'), (1.47, 'Spine1'), (1.405, 'Spine'))):
        loft(f'Ab{k}', bn, [tring(z0, 0.0, 0.215 - 0.01 * k, 0.175, 0.03, idx=FRONT_IDX), tring(z0 - 0.078, 0.0, 0.228 - 0.01 * k, 0.185, 0.036, idx=FRONT_IDX)],
             closed=False, thick=0.02, bevel=0.003, center=(0, 0.03, z0))
    loft('AbBack', 'Spine1', [tring(1.53, 0.0, 0.22, 0.175, 0.0, idx=BACK_IDX), tring(1.37, 0.0, 0.215, 0.17, 0.0, idx=BACK_IDX)],
         closed=False, thick=0.022, bevel=0.003, mat='Steel', center=(0, 0.0, 1.45))
    # cintura pesante, fibbia, tasche
    loft('Belt', 'Hips', [tring(1.335, -0.01, 0.25, 0.2, 0.02, cc=0.42), tring(1.245, -0.01, 0.255, 0.205, 0.022, cc=0.42)], thick=0.03, bevel=0.004,
         mat='Steel', center=(0, 0, 1.29))
    if not ORDO: block('Buckle', 'Hips', (0, -0.238, 1.29), AXES, (0.07, 0.018, 0.042), p=10, mat='Trim', bevel=0.003)
    for x_, y_ in ((0.205, -0.15), (0.258, 0.02), (0.13, 0.2), (-0.205, -0.15), (-0.258, 0.02), (-0.13, 0.2)):
        r_ = norm((x_, y_, 0)); t_ = np.cross(UP, r_)
        c_ = np.array([x_, y_, 1.0]) + r_ * 0.045; c_[2] = 1.27
        block(f'Pouch{x_}{y_}', 'Hips', c_ - r_ * 0.008, (t_, r_, UP), (0.038, 0.024, 0.042), p=10, mat='Leather', bevel=0.003)
        block(f'PouchLid{x_}{y_}', 'Hips', c_ + UP * 0.038, (t_, r_, UP), (0.041, 0.027, 0.01), p=10, mat='Leather')
    loft('Cod', 'Hips', [tring(1.235, -0.01, 0.1, 0.2, 0.04, idx=[9, 0, 1]), tring(1.08, 0.0, 0.065, 0.17, 0.03, idx=[9, 0, 1])],
         closed=False, thick=0.026, bevel=0.004, center=(0, 0, 1.16))
    if not ORDO:
        for side, y, sg in (('F', -0.255, -1), ('B', 0.235, 1)):
            V, F = [], []
            nz, nx = 12, 7
            for i in range(nz + 1):
                z = 1.23 - i * (0.42 / nz)
                for k in range(nx + 1):
                    x = -0.12 + 0.24 * k / nx
                    tear = 0.04 * math.sin(k * 2.7 + 1.3) if i == nz else 0.0
                    V.append((x * (1 + 0.15 * i / nz), y + sg * (0.01 + 0.02 * math.cos(math.pi * (k / nx - 0.5)) + 0.03 * i / nz), z + tear))
            for i in range(nz):
                for k in range(nx): F.append((i * (nx + 1) + k, i * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k))
            ob = link('Loin' + side, np.array(V), F); orient_out(ob, (0, 0, 1.0)); finish(ob, 0.006, 0.0)
            PARTS.append((ob, {'Hips': 1.0}, 'Cloth'))
        chain('BeltChain', [(-0.24, -0.17, 1.24), (-0.12, -0.25, 1.2), (0.05, -0.255, 1.21), (0.2, -0.2, 1.24)])
    # collare alto: basso davanti, sale ai lati e dietro a proteggere l'elmo; bordo d'ottone
    bot = tring(1.885, 0.02, 0.215, 0.19, 0.0)
    top = tring(1.95, 0.02, 0.2, 0.185, 0.0)
    top[:, 2] = 1.945 + 0.095 * np.clip((top[:, 1] + 0.17) / 0.32, 0, 1)
    loft('Collar', 'Spine2', [bot, top], thick=0.03, bevel=0.005, center=(0, 0.02, 1.9))
    tube('CollarRim', list(top) + [top[0]], 0.011, weights={'Spine2': 1.0}, mat='Trim', sides=8)
    shell('NeckGuard', 'Neck', (0, 0.03, 1.915), AXES, (0.12, 0.12, 0.045), pu=3, pv=4, v=(-1.0, 1.0), thick=0.02, bevel=0.004, mat='Steel')
    # zaino: alloggiamento, cappa alta dietro l'elmo, alette, scarichi; reattore in gabbia (generica)
    block('Pack', 'Spine2', (0, 0.33, 1.67), AXES, (0.24, 0.12, 0.23), p=10, mat='Plate', bevel=0.004, taper=(0.08, 0.0))
    if not ORDO: block('PackCowl', 'Spine2', (0, 0.31, 1.925), AXES, (0.2, 0.1, 0.06), p=10, mat='Plate', bevel=0.004)
    for s_ in ((1, -1) if not ORDO else ()):
        for k in range(6): block(f'Fin{k}{s_}', 'Spine2', (0.255 * s_, 0.33, 1.52 + 0.05 * k), AXES, (0.03, 0.095, 0.008), p=10, mat='Steel')
        a0 = np.array([0.15 * s_, 0.39, 1.9]); a1 = np.array([0.185 * s_, 0.47, 2.03])
        tube('Exhaust' + str(s_), [a0, a1], 0.036, weights={'Spine2': 1.0}, mat='Steel', sides=10, ribs=3)
        ring('ExhaustLip' + str(s_), 'Spine2', a1, frame(a1 - a0), 0.036, 0.009, mat='Trim', nu=16)
    if not ORDO:
        rc = np.array([0, 0.47, 1.67])
        tube('Reactor', [rc - UP * 0.16, rc + UP * 0.16], 0.075, weights={'Spine2': 1.0}, mat='Steel', sides=12, ribs=0)
        for z_ in (-0.16, 0.16):
            block(f'ReactorCap{z_}', 'Spine2', rc + UP * z_, AXES, (0.08, 0.08, 0.02), p=3, mat='Steel')
            ring(f'ReactorBand{z_}', 'Spine2', rc + UP * z_ * 0.85, AXES, 0.082, 0.012, mat='Trim', nu=24)
        block('ReactorWindow', 'Spine2', rc + [0, 0.072, 0], AXES, (0.026, 0.008, 0.11), p=8, mat='Glow')
        for th in (-0.75, -0.25, 0.25, 0.75):
            block(f'Cage{th}', 'Spine2', rc + [math.sin(th) * 0.092, math.cos(th) * 0.092, 0], AXES, (0.007, 0.007, 0.14), p=8, mat='Steel')
    # ---- spallaccio sinistro: cupola a sfaccettature, bordo d'ottone, due lame sotto, chiodi
    PW = {'LeftArm': 0.75, 'LeftShoulder': 0.25}
    PR = [(0.17, 0.04, 1.80, 0.2, 0.11), (0.25, 0.04, 1.79, 0.28, 0.21), (0.35, 0.045, 1.785, 0.305, 0.235),
          (0.44, 0.045, 1.78, 0.295, 0.22), (0.49, 0.05, 1.78, 0.285, 0.2)]
    PRG = (0.015, 0.04, 0.05, 0.045, 0.035)
    pr = [arc(*r_, n=9, ridge=g_) for r_, g_ in zip(PR, PRG)]
    pcen = (0.36, 0.045, 1.76)
    loft('Pauldron', None, pr, closed=False, thick=0.042, bevel=0.006, weights=PW, center=pcen)
    tube('PauldronRimO', list(pr[-1]), 0.013, weights=PW, mat='Trim', sides=8)
    tube('PauldronRimF', [r_[0] for r_ in pr], 0.012, weights=PW, mat='Trim', sides=8)
    tube('PauldronRimB', [r_[-1] for r_ in pr], 0.012, weights=PW, mat='Trim', sides=8)
    edge_rivets('RivPauldron', None, [p for p in pr[-1][1:-1]] + [r_[0] for r_ in pr[1:-1]] + [r_[-1] for r_ in pr[1:-1]], pcen, weights=PW, out=0.016)
    for k, (x0, x1, ry, rz) in enumerate(((0.43, 0.52, 0.2, 0.155), (0.49, 0.575, 0.185, 0.14))):
        loft(f'PauldronLame{k}', None, [arc(x0, 0.05, 1.80, ry, rz, 205, -30), arc(x1, 0.05, 1.80, ry - 0.01, rz - 0.01, 205, -30)], closed=False,
             thick=0.022, bevel=0.004, weights={'LeftArm': 0.9 + 0.1 * k, 'LeftShoulder': 0.1 - 0.1 * k}, center=(0.5, 0.05, 1.8))
    # ---- braccio
    A, E, W, Hn = j['LeftArm'], j['LeftForeArm'], j['LeftHand'], j['LeftHandMiddle1']
    limb('Rerebrace', 'LeftArm', A, E, UP, [(0.45, 0.1, 0.1, 0.015), (0.95, 0.09, 0.095, 0.01)], thick=0.03, bevel=0.004)
    ax = limb_axes(A, E, (0, 0, 1))
    tube('ArmPiston', [mid(A, E, 0.45) + [0, -0.108, -0.02], mid(A, E, 0.86) + [0, -0.108, -0.02]], 0.016, weights={'LeftArm': 1.0}, mat='Steel', sides=10)
    tube('ArmRod', [mid(A, E, 0.86) + [0, -0.108, -0.02], mid(A, E, 1.0) + [0, -0.105, -0.02]], 0.008, weights={'LeftArm': 1.0}, mat='Trim', sides=8)
    ring('ElbowRing', 'LeftArm', mid(A, E, 0.95), ax, 0.08, 0.014, mat='Steel')
    shell('Couter', 'LeftForeArm', E + [0.0, 0.06, 0.0], frame((0, 1.0, 0.0), (0, 0, 1)), (0.085, 0.08, 0.07), pu=8, pv=2.4, v=(0.0, PI / 2), thick=0.024,
          bevel=0.004, mat='Plate')
    limb('Vambrace', 'LeftForeArm', E, W, UP, [(0.08, 0.085, 0.09, 0.01), (0.5, 0.105, 0.105, 0.03), (0.96, 0.105, 0.1, 0.02)], thick=0.033, bevel=0.005)
    limb('VambraceTop', 'LeftForeArm', E, W, UP, [(0.2, 0.116, 0.116, 0.06), (0.85, 0.117, 0.112, 0.05)], idx=[8, 9, 0, 1, 2], thick=0.016, bevel=0.003)
    wm = mid(E, W, 0.62) + [0, -0.118, 0]
    block('WristBox', 'LeftForeArm', wm, AXES, (0.055, 0.022, 0.032), p=10, mat='Steel', bevel=0.002)
    hax = limb_axes(W, Hn, (0, 0, 1))
    shell('Cuff', 'LeftHand', W + (W - E) * 0.05, hax, (0.08, 0.085, 0.04), pu=6, pv=6, v=(-1.2, 1.2), thick=0.018, bevel=0.003, mat='Steel')
    block('HandBack', 'LeftHand', mid(W, Hn, 0.55) + [0, 0, 0.028], hax, (0.062, 0.03, 0.082), p=10, mat='Plate', bevel=0.003)
    block('Knuckles', 'LeftHand', Hn + [-0.01, 0, 0.024], hax, (0.064, 0.022, 0.024), p=10, mat='Steel', bevel=0.002)
    block('Palm', 'LeftHand', mid(W, Hn, 0.55) + [0, 0, -0.012], hax, (0.055, 0.022, 0.075), p=3.5, mat='Suit')
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        chn = [j[f'LeftHand{f}{k}'] for k in (1, 2, 3)]
        tip = chn[2] + (chn[2] - chn[1]) * 0.9
        pts = chn + [tip]
        for k in range(3):
            a_, b_ = pts[k], pts[k + 1]
            fa = limb_axes(a_, b_, (0, 0, 1)); L_ = np.linalg.norm(b_ - a_)
            block(f'{f}{k}', f'LeftHand{f}{k + 1}', mid(a_, b_), fa, (0.022, 0.021, L_ * 0.5 + 0.005), p=8, mat='Steel' if k == 0 else 'Plate', bevel=0.0015)
    # ---- gamba
    Hp, K, F, T = j['LeftUpLeg'], j['LeftLeg'], j['LeftFoot'], j['LeftToeBase']
    TW = {'LeftUpLeg': 0.7, 'Hips': 0.3}
    for k, (z0, z1, a0, a1) in enumerate(((1.27, 1.15, 0.275, 0.29), (1.18, 1.06, 0.29, 0.305))):
        loft(f'Tasset{k}', None, [tring(z0, -0.01, a0, 0.215, 0.03, idx=[1, 2, 3]), tring(z1, -0.01, a1, 0.225, 0.035, idx=[1, 2, 3])], closed=False,
             thick=0.024, bevel=0.004, weights=TW, center=(0.05, 0, 1.17))
    limb('Cuisse', 'LeftUpLeg', Hp, K, FWD, [(0.08, 0.155, 0.155, 0.02), (0.5, 0.15, 0.15, 0.035), (0.9, 0.12, 0.12, 0.02)], thick=0.035, bevel=0.005)
    limb('CuisseOuter', 'LeftUpLeg', Hp, K, FWD, [(0.2, 0.172, 0.168, 0.0), (0.72, 0.158, 0.152, 0.0)], idx=[9, 8, 7, 6], thick=0.016, bevel=0.003, mat='Steel')
    shell('Poleyn', 'LeftLeg', K + [0, -0.098, 0.012], frame((0, -1.0, 0), (0, 0, 1)), (0.105, 0.11, 0.085), pu=8, pv=2.6, v=(0.0, PI / 2), thick=0.03, bevel=0.005,
          keel=0.035)
    block('KneeRidge', 'LeftLeg', K + [0, -0.19, 0.012], AXES, (0.014, 0.016, 0.07), p=10, mat='Steel')
    gax2 = limb_axes(K, F, (0, 1, 0))
    ring('KneeRing', 'LeftLeg', K + (F - K) * 0.12, gax2, 0.09, 0.013, mat='Steel')
    limb('Greave', 'LeftLeg', K, F, FWD, [(0.08, 0.115, 0.12, 0.01), (0.45, 0.13, 0.135, 0.05), (0.92, 0.1, 0.105, 0.02)], thick=0.036, bevel=0.005)
    limb('GreaveRidge', 'LeftLeg', K, F, FWD, [(0.14, 0.14, 0.148, 0.065), (0.85, 0.118, 0.125, 0.036)], idx=[9, 0, 1], thick=0.015, bevel=0.003)
    tube('CalfPiston', [mid(K, F, 0.15) + [0, 0.145, 0], mid(K, F, 0.72) + [0, 0.128, 0]], 0.018, weights={'LeftLeg': 1.0}, mat='Steel', sides=10)
    tube('CalfRod', [mid(K, F, 0.72) + [0, 0.128, 0], mid(K, F, 0.9) + [0, 0.12, 0]], 0.009, weights={'LeftLeg': 1.0}, mat='Trim', sides=8)
    block('Boot', 'LeftFoot', (F[0], F[1] - 0.04, 0.105), AXES, (0.1, 0.17, 0.1), p=10, mat='Plate', bevel=0.004, taper=(-0.06, 0.0))
    block('Sole', 'LeftFoot', (F[0], F[1] - 0.05, 0.024), AXES, (0.108, 0.2, 0.024), p=12, mat='Steel', bevel=0.002)
    block('ToeCap', 'LeftToeBase', (T[0], T[1] - 0.07, 0.068), AXES, (0.094, 0.085, 0.062), p=10, mat='Plate', bevel=0.004, taper=(0.0, -0.1), keel=0.035)
    block('ToeLame', 'LeftToeBase', (T[0], T[1] + 0.01, 0.125), AXES, (0.096, 0.04, 0.016), p=10, mat='Steel', bevel=0.002)
    shell('AnkleGuard', 'LeftFoot', F + [0, 0, 0.05], gax2, (0.105, 0.115, 0.055), pu=8, pv=5, v=(-1.1, 1.1), thick=0.02, bevel=0.003, mat='Steel')
    build_helmet()


def cut(ob, boxes):
    """fori nella lamiera: differenza booleana con scatole (centro, mezze misure)"""
    bm = bmesh.new()
    for c, h in boxes:
        r_ = bmesh.ops.create_cube(bm, size=1.0)
        for v in r_['verts']: v.co = Vector((c[0] + v.co.x * 2 * h[0], c[1] + v.co.y * 2 * h[1], c[2] + v.co.z * 2 * h[2]))
    me = bpy.data.meshes.new('cutter'); bm.to_mesh(me); bm.free()
    co = bpy.data.objects.new('cutter', me); COL.objects.link(co)
    m = ob.modifiers.new('cut', 'BOOLEAN'); m.operation = 'DIFFERENCE'; m.object = co; m.solver = 'EXACT'
    bake_modifiers(ob); bpy.data.objects.remove(co, do_unlink=True)
    for p in ob.data.polygons: p.use_smooth = True


def horn(name, bone, pts, r0, r1, mat='Cross', sides=12):
    """corno: anelli che si stringono lungo una curva"""
    pts = [np.asarray(p, float) for p in pts]; rings = []
    for i, c in enumerate(pts):
        t = i / (len(pts) - 1); r = r0 + (r1 - r0) * t ** 0.85
        tng = norm(pts[min(i + 1, len(pts) - 1)] - pts[max(i - 1, 0)]); ex, ey, _ = frame(tng)
        rings.append([c + (ex * math.cos(a) + ey * math.sin(a)) * r for a in np.linspace(0, 2 * math.pi, sides, endpoint=False)])
    ob = loft(name, bone, rings, thick=0.0, bevel=0.0, mat=mat, center=pts[len(pts) // 2], rnd=0.0, smooth=1.2)
    return ob


def bezier(p0, p1, p2, n=14):
    p0, p1, p2 = (np.asarray(p, float) for p in (p0, p1, p2))
    return [(1 - t) ** 2 * p0 + 2 * (1 - t) * t * p1 + t * t * p2 for t in np.linspace(0, 1, n)]


def build_helmet():
    PI = math.pi
    hc = np.array([0, 0.0, 2.025])
    if ORDO:
        # elmo a secchio teutonico: fianchi quasi dritti, cima piatta, croce nera sul viso (banda della fronte + nasale),
        # due feritoie, fori di respirazione, chiodi sul bordo, cimiero a corna nere
        def hr(dz, y, a, b, k, idx=None): return tring(hc[2] + dz, y, a, b, k, cc=0.28, idx=idx)
        HR = [(-0.205, -0.025, 0.228, 0.232, 0.045), (-0.15, -0.008, 0.172, 0.187, 0.03), (-0.06, 0.0, 0.153, 0.168, 0.026), (0.06, 0.0, 0.151, 0.166, 0.026),
              (0.15, 0.0, 0.143, 0.158, 0.02), (0.176, 0.0, 0.12, 0.134, 0.012), (0.186, 0.0, 0.055, 0.065, 0.0)]
        helm = loft('Helm', 'Head', [hr(*h) for h in HR], thick=0.032, bevel=0.005, cap_top=True, center=hc, rnd=0.012)
        # feritoie e fori di respirazione tagliati davvero nella lamiera: la luce sta dentro, in fondo alla visiera
        holes = [((0.062 * s_, -0.18, hc[2] + 0.03), (0.042, 0.035, 0.0065)) for s_ in (1, -1)]
        for s_ in (1, -1):
            for k in range(3):
                for q in range(2 if k != 1 else 3):
                    dx = (0.0 if k != 1 else -0.014) + 0.014 * q
                    holes.append((((0.075 + dx) * s_, -0.17, hc[2] - 0.06 - 0.022 * k), (0.0045, 0.035, 0.0045)))
        cut(helm, holes)
        rim = hr(-0.205, -0.025, 0.228, 0.232, 0.045)
        tube('HelmRim', list(rim) + [rim[0]], 0.011, weights={'Head': 1.0}, mat='Trim', sides=8)
        band = hr(-0.15, -0.008, 0.177, 0.192, 0.031)
        tube('HelmBand', list(band) + [band[0]], 0.009, weights={'Head': 1.0}, mat='Cross', sides=8)
        loft('BrowBand', 'Head', [hr(0.052, 0.0, 0.158, 0.173, 0.03, FRONT_IDX), hr(0.088, 0.0, 0.157, 0.172, 0.03, FRONT_IDX)], closed=False,
             thick=0.01, bevel=0.002, mat='Cross', center=hc, rnd=0.004)
        block('Nasal', 'Head', hc + [0, -0.198, -0.045], AXES, (0.019, 0.008, 0.135), p=8, mat='Cross', bevel=0.002)
        for s_ in (1, -1):
            block('Slit' + str(s_), 'Head', hc + [0.062 * s_, -0.152, 0.03], AXES, (0.044, 0.004, 0.0075), p=8, mat='Glow')
            hp = bezier(hc + [0.115 * s_, 0.0, 0.15], hc + [0.29 * s_, -0.02, 0.15], hc + [0.37 * s_, -0.07, 0.31])
            horn('Horn' + str(s_), 'Head', hp, 0.045, 0.004)
            ring('HornBand' + str(s_), 'Head', hp[1], frame(hp[2] - hp[0]), 0.04, 0.007, mat='Trim', nu=18)
        rim2 = hr(-0.178, -0.017, 0.2, 0.21, 0.038)
        edge_rivets('RivHelm', 'Head', [rim2[i] for i in range(len(rim2))], hc + [0, 0, -0.1], out=0.006)
        cross_pattee('CrossHelm', 'Head', hc + [0, 0.0, 0.189], (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])), 0.11)
        return
    # elmo da assalto, cattivo: calotta bassa e sfaccettata, fronte a becco che scende sul viso, due feritoie inclinate
    # (piu' basse all'interno) incassate in ombra, mascella a prua, griglie di sfiato sui fianchi, guance, lame sulla nuca
    def hr(dz, y, a, b, k, idx=None): return tring(hc[2] + dz, y, a, b, k, cc=0.4, idx=idx)
    loft('Dome', 'Head', [hr(-0.05, 0.01, 0.15, 0.168, 0.03), hr(0.04, 0.01, 0.158, 0.178, 0.05), hr(0.10, 0.015, 0.14, 0.165, 0.045),
                          hr(0.14, 0.02, 0.095, 0.125, 0.03), hr(0.157, 0.02, 0.04, 0.055, 0.0)], thick=0.03, bevel=0.005, cap_top=True,
         center=hc + [0, 0.01, 0.0], rnd=0.006)
    b0 = hr(0.03, 0.0, 0.17, 0.19, 0.065, FRONT_IDX); b1 = hr(0.075, 0.0, 0.165, 0.184, 0.05, FRONT_IDX)
    b0[3, 2] -= 0.042; b0[2, 2] -= 0.017; b0[4, 2] -= 0.017
    loft('Brow', 'Head', [b0, b1], closed=False, thick=0.032, bevel=0.005, center=hc, rnd=0.004)
    block('VisorPit', 'Head', hc + [0, -0.15, -0.01], AXES, (0.112, 0.02, 0.032), p=10, mat='Steel', bevel=0.002)
    for s_ in (1, -1):
        a_ = math.radians(17); ex_ = np.array([math.cos(a_), 0, s_ * math.sin(a_)]); ey_ = np.array([0, 1.0, 0]); ez_ = np.cross(ex_, ey_)
        block('Eye' + str(s_), 'Head', hc + [0.05 * s_, -0.171, -0.012], (ex_, ey_, ez_), (0.034, 0.004, 0.0042), p=12, mat='Glow')
    loft('Jaw', 'Head', [hr(-0.025, 0.0, 0.15, 0.172, 0.045, FRONT_IDX), hr(-0.10, -0.01, 0.122, 0.18, 0.1, FRONT_IDX),
                         hr(-0.178, 0.0, 0.06, 0.125, 0.065, FRONT_IDX)], closed=False, thick=0.028, bevel=0.005, center=hc + [0, 0.02, -0.06], rnd=0.004)
    for s_ in (1, -1):
        ex_ = norm((0.5 * s_, 0.86, 0)); ey_ = norm((0.86 * s_, -0.5, 0))
        for k in range(4):
            block(f'Slat{k}{s_}', 'Head', hc + [0.112 * s_, -0.158, -0.05 - 0.024 * k], (ex_, ey_, UP), (0.04, 0.006, 0.0065), p=10, mat='Steel')
        block('Cheek' + str(s_), 'Head', hc + [0.166 * s_, 0.005, -0.015], AXES, (0.03, 0.09, 0.085), p=10, mat='Plate', bevel=0.003, keel=0.02)
        block('CheekFin' + str(s_), 'Head', hc + [0.19 * s_, 0.06, 0.03], frame((0, 0.5, 1.0)), (0.01, 0.03, 0.06), p=10, mat='Steel', bevel=0.002)
        tube('Hose' + str(s_), [hc + [0.10 * s_, 0.15, -0.08], (0.15 * s_, 0.27, 1.99), (0.14 * s_, 0.35, 1.95)], 0.016, ribs=3, mat='Steel')
    block('Crest', 'Head', hc + [0, -0.01, 0.152], AXES, (0.01, 0.12, 0.012), p=10, mat='Plate')
    for k, (z0, z1) in enumerate(((-0.06, -0.14), (-0.12, -0.19))):
        loft(f'Nape{k}', 'Head', [hr(z0, 0.0, 0.152 - 0.008 * k, 0.172 - 0.008 * k, 0.0, BACK_IDX), hr(z1, 0.0, 0.145 - 0.008 * k, 0.165 - 0.008 * k, 0.0, BACK_IDX)],
             closed=False, thick=0.024, bevel=0.004, center=hc)


def cape_y(x, z):
    yb = np.interp(z, [0.58, 1.0, 1.6, 1.86, 1.92, 1.96], [0.53, 0.48, 0.50, 0.49, 0.40, 0.22])
    return yb - 0.09 * (x / 0.45) ** 2


def cape_w(z): return np.interp(z, [0.58, 1.5, 1.85, 1.96], [0.47, 0.39, 0.34, 0.2])


def cape_weights(p):
    z = p[2]
    if z > 1.55: return {'Spine2': 1.0}
    if z > 1.25:
        t = (1.55 - z) / 0.3; return {'Spine2': 1 - t, 'Spine1': t}
    t = min(1.0, (1.25 - z) / 0.4); return {'Spine1': 1 - t * 0.6, 'Hips': t * 0.6}


def surface_grid(name, xr, zr, yfun, off, nx, nz, mat, weights, jag=0.0):
    V, F = [], []
    zs = np.linspace(zr[0], zr[1], nz + 1)
    for i, z in enumerate(zs):
        x0, x1 = xr(z) if callable(xr) else xr
        for k in range(nx + 1):
            x = x0 + (x1 - x0) * k / nx
            zz = z + (jag * abs(math.sin(k * 1.9 + 0.4)) if (jag and i == 0) else 0.0)
            V.append((x, yfun(x, zz) + off, zz))
    for i in range(nz):
        for k in range(nx): F.append((i * (nx + 1) + k, i * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k))
    ob = link(name, np.array(V), F)
    orient_out(ob, (0, 0.0, 1.3))
    return ob


def build_cape():
    """cappa bianca dell'Ordo, dalle spalle ai polpacci, con la croce nera sulla schiena (segue la piega della stoffa)"""
    ob = surface_grid('Cape', lambda z: (-cape_w(z), cape_w(z)), (0.58, 1.96), cape_y, 0.0, 24, 40, 'Cloth', cape_weights, jag=0.035)
    finish(ob, 0.008, 0.0)
    PARTS.append((ob, cape_weights, 'Cloth'))
    for nm, xr, zr, off in (('CapeCrossV', (-0.06, 0.06), (0.92, 1.58), 0.004), ('CapeCrossH', (-0.27, 0.27), (1.29, 1.41), 0.0047)):
        o2 = surface_grid(nm, xr, zr, cape_y, off, 12, 18, 'Cross', cape_weights)
        finish(o2, 0.0, 0.0)
        PARTS.append((o2, cape_weights, 'Cross'))


BACK_ = (0.25, math.pi - 0.25)


def ordo_extras():
    """Ordo: croci patenti, ossa, teschio, sopravveste"""
    ch = AXES
    # croci nere: sul petto sopra il nucleo, sul fianco alto degli spallacci; sulla schiena c'e' quella della cappa
    cross_pattee('CrossChest', 'Spine2', np.array([0, -0.318, 1.84]), (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, -1.0, 0])), 0.11)
    th = math.radians(138); n = np.array([0, math.cos(th), math.sin(th)]); ax_ = np.array([1.0, 0, 0]); ay_ = np.cross(n, ax_)
    cross_pattee('CrossPauldronL', 'LeftArm', np.array([0.37, 0.045 + 0.305 * math.cos(th), 1.783 + 0.235 * math.sin(th)]) + n * 0.006, (ax_, ay_, n), 0.12)
    PARTS[-1] = (PARTS[-1][0], {'LeftArm': 0.75, 'LeftShoulder': 0.25}, 'Cross')
    # scanalature gotiche: costole in rilievo che seguono la curva degli spallacci e scendono sui pettorali
    PR = [(0.17, 0.04, 1.80, 0.2, 0.11), (0.25, 0.04, 1.79, 0.28, 0.21), (0.35, 0.045, 1.785, 0.305, 0.235),
          (0.44, 0.045, 1.78, 0.295, 0.22), (0.49, 0.05, 1.78, 0.285, 0.2)]
    PRG = (0.015, 0.04, 0.05, 0.045, 0.035)
    for xr in (0.29, 0.39, 0.46):
        prm = [np.interp(xr, [p[0] for p in PR], [p[i] for p in PR]) for i in range(1, 5)]
        g_ = np.interp(xr, [p[0] for p in PR], PRG)
        ap = arc(xr, prm[0], prm[1], prm[2] + 0.004, prm[3] + 0.004, n=9, ridge=g_)[1:-1]
        tube(f'Flute{xr}', list(ap), 0.0055, weights={'LeftArm': 0.75, 'LeftShoulder': 0.25}, mat='Plate', sides=6)
    PE = [(1.62, -0.005, 0.30, 0.23, 0.05), (1.73, -0.01, 0.335, 0.245, 0.065), (1.86, 0.0, 0.325, 0.235, 0.05)]
    for s_ in (1, -1):
        for t_ in (0.35, 0.7):
            pts = []
            for i_, (z, y, a, b, k) in enumerate(PE):
                p = tring(z, y, a, b, k); q = p[[0, 1, 2]] if s_ > 0 else p[[0, 9, 8]]
                c_ = q[1] + (q[2] - q[1]) * t_ if t_ > 0.5 else q[0] + (q[1] - q[0]) * (0.25 + t_)
                if i_ == 0: c_[2] += -0.022 if t_ < 0.5 else 0.012
                pts.append(c_ + norm(c_ - np.array([0, 0.05, c_[2]])) * 0.004)
            tube(f'PecFlute{s_}{t_}', pts, 0.005, weights={'Spine2': 1.0}, mat='Plate', sides=6)
    build_cape()
    sk = np.array([0, -0.265, 1.268])
    shell('Skull_', 'Hips', sk + [0, 0, 0.012], ch, (0.046, 0.05, 0.056), pu=2, pv=2.3, closed=True, mat='Bone', nu=24, nv=12)
    block('SkullCheek', 'Hips', sk + [0, -0.012, -0.03], ch, (0.04, 0.036, 0.02), p=2.6, mat='Bone')
    block('SkullJaw', 'Hips', sk + [0, -0.02, -0.058], ch, (0.03, 0.03, 0.014), p=3, mat='Bone')
    for t in range(6): block(f'Tooth{t}', 'Hips', sk + [-0.0175 + t * 0.007, -0.05, -0.046], ch, (0.0028, 0.006, 0.006), p=3, mat='Bone')
    block('Nose', 'Hips', sk + [0, -0.05, -0.016], ch, (0.007, 0.008, 0.01), p=2, mat='Cross')
    for s in (1, -1):
        block('Eye' + str(s), 'Hips', sk + [0.018 * s, -0.044, 0.006], ch, (0.013, 0.012, 0.012), p=2, mat='Cross')
        fa = frame((s, 0, 0.42))
        c0 = sk + [0, 0.012, -0.075]
        block('Femur' + str(s), 'Hips', c0, (fa[0], fa[1], fa[2]), (0.011, 0.011, 0.1), p=2.4, mat='Bone')
        for e in (1, -1):
            for q in (1, -1): block(f'FemurEnd{s}{e}{q}', 'Hips', c0 + fa[2] * 0.1 * e + fa[0] * 0.008 * q, ch, (0.012, 0.012, 0.012), p=2, mat='Bone')
    chain('BeltChain', [(-0.24, -0.17, 1.24), (-0.12, -0.25, 1.2), (0.05, -0.255, 1.21), (0.2, -0.2, 1.24)])
    # sopravveste: davanti e dietro, dalla cintura al ginocchio, con la croce nera (pesi dalla tuta)
    for side, y, sgn in (('F', -0.262, -1), ('B', 0.245, 1)):
        V, F = [], []
        nz, nx = 14, 9
        for i in range(nz + 1):
            z = 1.22 - i * (0.56 / nz)
            w = 0.19 + i * 0.004
            for k in range(nx + 1):
                x = -w + 2 * w * k / nx
                bulge = 0.03 * math.cos(math.pi * (k / nx - 0.5))
                tear = 0.05 * abs(math.sin(k * 2.3 + 0.7)) if i == nz else 0.0
                V.append((x, y + sgn * (0.012 + bulge) + sgn * 0.02 * (i / nz), z + tear))
        for i in range(nz):
            for k in range(nx):
                F.append((i * (nx + 1) + k, i * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k))
        ob = link('Tabard' + side, np.array(V), F)
        orient_out(ob, (0, 0, 1.0)); finish(ob, 0.006, 0.0)
        PARTS.append((ob, {'Hips': 1.0}, 'Cloth'))
        cross_pattee('CrossTabard' + side, 'Hips', np.array([0, y + sgn * 0.05, 1.05]), (np.array([1.0 * -sgn, 0, 0]), np.array([0, 0, 1.0]), np.array([0, sgn * 1.0, 0])), 0.2, depth=0.004)
        PARTS[-1] = (PARTS[-1][0], {'Hips': 1.0}, 'Cross')


# ------------------------------------------------------------------ specchio e scheletro
def mirror_left():
    out = []
    for ob, w, mat in list(PARTS):
        if not any(k in ob.name for k in ()) and not (isinstance(w, dict) and any('Left' in b for b in w if b)):
            continue
        me = ob.data.copy(); me.name = ob.name + '_R'
        for v in me.vertices: v.co.x = -v.co.x
        bm = bmesh.new(); bm.from_mesh(me); bmesh.ops.reverse_faces(bm, faces=bm.faces[:]); bm.to_mesh(me); bm.free()
        o2 = bpy.data.objects.new(ob.name + '_R', me); COL.objects.link(o2)
        w2 = {b.replace('Left', 'Right'): x for b, x in w.items()}
        out.append((o2, w2, mat))
    PARTS.extend(out)


def build_armature():
    """lo scheletro originale del corpo Vanguard (vanguard_master.fbx): stesse ossa, stesse rotazioni di riposo che le
    clip del gioco si aspettano (uno ricostruito dalle sole posizioni piega il busto nelle animazioni)"""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'Sandbox/GameData/Models/Characters/vanguard_master.fbx'))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == 'ARMATURE')
    for o in new:
        if o is not arm: bpy.data.objects.remove(o, do_unlink=True)
    activate(arm)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    arm.name = 'Armature'
    print('scheletro', len(arm.data.bones), 'ossa dal FBX')
    return arm


# ------------------------------------------------------------------ materiali (nodi per la cottura)
PAL = {
    # generica: vernice da campo grigio-oliva scuro (scheggiata fino all'acciaio), finiture d'ottone brunito, meccanica in acciaio scuro
    'generic': dict(plate=(0.034, 0.039, 0.029), plate_metal=0.1, plate_rough=0.6, edge=(0.36, 0.355, 0.34), trim=(0.26, 0.16, 0.055),
                    trim_edge=(0.72, 0.52, 0.24), tarnish=(0.03, 0.035, 0.02), steel=(0.03, 0.03, 0.032),
                    core=(0.12, 0.02, 0.22), core_emit=(0.45, 0.06, 0.85), glow=(0.55, 0.12, 0.95), rust=(0.2, 0.075, 0.025)),
    'ordo': dict(plate=(0.74, 0.72, 0.68), plate_metal=0.0, plate_rough=0.6, edge=(0.82, 0.8, 0.76), trim=(0.02, 0.02, 0.022),
                 trim_edge=(0.3, 0.29, 0.28), tarnish=(0.2, 0.15, 0.09), steel=(0.02, 0.02, 0.022),
                 core=(0.8, 0.79, 0.76), core_emit=(1.0, 0.95, 0.86), glow=(1.0, 0.78, 0.42), rust=(0.2, 0.15, 0.09)),
}[VAR if VAR in ('ordo',) else 'generic']


def node(nt, t, **kw):
    n = nt.nodes.new(t)
    for k, v in kw.items():
        if k in n.inputs: n.inputs[k].default_value = v
        else: setattr(n, k, v)
    return n


def _sock(n, ident):
    for x in n.inputs:
        if x.identifier == ident: return x
    raise KeyError(ident)


def _set(nt, sock, val):
    if hasattr(val, 'node') and hasattr(val, 'is_output'): nt.links.new(val, sock)
    elif isinstance(val, (tuple, list)): sock.default_value = (*val[:3], 1) if len(val) == 3 else tuple(val)
    else: sock.default_value = val


def mixc(nt, fac, a, b, blend='MIX'):
    n = nt.nodes.new('ShaderNodeMix'); n.data_type = 'RGBA'; n.blend_type = blend
    _set(nt, _sock(n, 'Factor_Float'), fac); _set(nt, _sock(n, 'A_Color'), a); _set(nt, _sock(n, 'B_Color'), b)
    return next(o for o in n.outputs if o.identifier == 'Result_Color')


def mixf(nt, fac, a, b):
    n = nt.nodes.new('ShaderNodeMix'); n.data_type = 'FLOAT'
    _set(nt, _sock(n, 'Factor_Float'), fac); _set(nt, _sock(n, 'A_Float'), a); _set(nt, _sock(n, 'B_Float'), b)
    return next(o for o in n.outputs if o.identifier == 'Result_Float')


def mrange(nt, val, fmin, fmax, tmin=0.0, tmax=1.0):
    n = nt.nodes.new('ShaderNodeMapRange')
    n.inputs['From Min'].default_value = fmin; n.inputs['From Max'].default_value = fmax
    n.inputs['To Min'].default_value = tmin; n.inputs['To Max'].default_value = tmax
    _set(nt, n.inputs['Value'], val)
    return n.outputs['Result']


def mul(nt, a, b):
    n = nt.nodes.new('ShaderNodeMath'); n.operation = 'MULTIPLY'; _set(nt, n.inputs[0], a); _set(nt, n.inputs[1], b); return n.outputs[0]


def add(nt, a, b):
    n = nt.nodes.new('ShaderNodeMath'); n.operation = 'ADD'; _set(nt, n.inputs[0], a); _set(nt, n.inputs[1], b); return n.outputs[0]


def procedural(nt, kind):
    """ritorna (colore, rugosita', metallo, altezza del rilievo, forza del rilievo)"""
    L = nt.links.new
    tc = node(nt, 'ShaderNodeTexCoord')
    geo = node(nt, 'ShaderNodeNewGeometry')
    obj = tc.outputs['Object']
    nbig = node(nt, 'ShaderNodeTexNoise', Scale=3.0, Detail=4.0); L(obj, nbig.inputs['Vector'])
    nfine = node(nt, 'ShaderNodeTexNoise', Scale=60.0, Detail=8.0, Roughness=0.7); L(obj, nfine.inputs['Vector'])
    # spigoli e incavi dalla geometria vera (Bevel e occlusione), non dalla Pointiness: sui gusci fitti la Pointiness
    # cambia da un vertice all'altro e sporcava le piastre di puntini chiari
    bev = node(nt, 'ShaderNodeBevel', Radius=0.006); bev.samples = 8
    dp = node(nt, 'ShaderNodeVectorMath', operation='DOT_PRODUCT')
    L(bev.outputs['Normal'], dp.inputs[0]); L(geo.outputs['Normal'], dp.inputs[1])
    edge = mrange(nt, dp.outputs['Value'], 0.985, 0.94)    # spigoli vivi
    ao = node(nt, 'ShaderNodeAmbientOcclusion', Distance=0.05); ao.samples = 8
    cav = mrange(nt, ao.outputs['AO'], 0.85, 0.45)        # incavi
    nmid = node(nt, 'ShaderNodeTexNoise', Scale=14.0, Detail=3.0); L(obj, nmid.inputs['Vector'])
    wear = mul(nt, edge, mrange(nt, nmid.outputs['Fac'], 0.42, 0.62))
    scr = node(nt, 'ShaderNodeTexWave', wave_type='BANDS', bands_direction='DIAGONAL'); scr.inputs['Scale'].default_value = 90.0
    scr.inputs['Distortion'].default_value = 25.0; scr.inputs['Detail'].default_value = 3.0; L(obj, scr.inputs['Vector'])
    scratch = mrange(nt, scr.outputs['Fac'], 0.93, 1.0)
    p = PAL
    if kind == 'Plate':
        if ORDO:
            vein = node(nt, 'ShaderNodeTexWave', wave_type='BANDS'); vein.inputs['Scale'].default_value = 4.0
            vein.inputs['Distortion'].default_value = 14.0; vein.inputs['Detail'].default_value = 6.0; L(obj, vein.inputs['Vector'])
            c = mixc(nt, mul(nt, mrange(nt, vein.outputs['Fac'], 0.96, 1.0), 0.5), p['plate'], (0.5, 0.5, 0.5))
            c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.0, 0.15), c, (0.66, 0.63, 0.57))
            mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (22.0, 22.0, 1.6); L(obj, mp.inputs['Vector'])
            drip = node(nt, 'ShaderNodeTexNoise', Scale=1.0, Detail=5.0); L(mp.outputs['Vector'], drip.inputs['Vector'])
            c = mixc(nt, mul(nt, mrange(nt, drip.outputs['Fac'], 0.5, 0.62), 0.25), c, (0.35, 0.32, 0.27))
            zs = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, zs.inputs['Vector'])
            c = mixc(nt, mul(nt, mrange(nt, zs.outputs['Z'], 0.6, 0.05), 0.3), c, (0.3, 0.27, 0.22))
            chip = node(nt, 'ShaderNodeTexVoronoi'); chip.inputs['Scale'].default_value = 40.0; L(obj, chip.inputs['Vector'])
            c = mixc(nt, mul(nt, mul(nt, edge, mrange(nt, chip.outputs['Distance'], 0.18, 0.05)), 0.6), c, (0.45, 0.43, 0.4))
        else:
            c = mixc(nt, 0.35, p['plate'], mrange(nt, nbig.outputs['Fac'], 0, 1, 0.8, 1.2), 'MULTIPLY')
            # vernice scheggiata: le zone chiare di una foto di lamiera arrugginita decidono dove la vernice e' saltata,
            # molto di piu' sugli spigoli che sulle facce piane
            P2 = photo_lib.nodes(nt, obj, 'rusty_metal_04', 2.2) if not FAST else None
            if P2 is not None:
                chip = mrange(nt, mul(nt, P2['rel'], add(nt, 0.3, mul(nt, edge, 0.75))), 1.15, 1.3)
                mx = nt.nodes.new('ShaderNodeMath'); mx.operation = 'MAXIMUM'; _set(nt, mx.inputs[0], wear); _set(nt, mx.inputs[1], chip)
                wear = mx.outputs[0]
        rustm = mul(nt, add(nt, cav, mrange(nt, nbig.outputs['Fac'], 0.58, 0.78, 0.0, 0.5)), mrange(nt, nmid.outputs['Fac'], 0.35, 0.65))
        c = mixc(nt, wear, c, p['edge'])
        c = mixc(nt, mul(nt, rustm, 0.2 if ORDO else 0.75), c, p['rust'])
        c = mixc(nt, mul(nt, cav, 0.35 if ORDO else 0.6), c, (0.02, 0.018, 0.015))
        c = mixc(nt, mul(nt, scratch, 0.18), c, p['edge'])
        r = mixf(nt, wear, mrange(nt, nmid.outputs['Fac'], 0, 1, p['plate_rough'] - 0.08, p['plate_rough'] + 0.1), 0.25 if not ORDO else 0.45)
        r = mixf(nt, mul(nt, rustm, 0.75), r, 0.9)
        m = mixf(nt, wear, p['plate_metal'], 1.0 if not ORDO else 0.0)
        m = mixf(nt, mul(nt, rustm, 0.75), m, 0.15)
        return c, r, m, add(nt, mul(nt, nmid.outputs['Fac'], 0.5), mul(nt, scratch, 0.25)), 0.06
    if kind == 'Leather':
        c = mixc(nt, mrange(nt, nfine.outputs['Fac'], 0.3, 0.7), (0.035, 0.022, 0.015), (0.07, 0.045, 0.03))
        c = mixc(nt, mul(nt, wear, 0.6), c, (0.2, 0.15, 0.1))
        return c, mrange(nt, nfine.outputs['Fac'], 0, 1, 0.55, 0.8), 0.0, nmid.outputs['Fac'], 0.1
    if kind == 'Trim':
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.0, 0.35), p['trim'], p['tarnish'])
        c = mixc(nt, wear, c, p['trim_edge'])
        c = mixc(nt, mul(nt, mul(nt, cav, mrange(nt, nfine.outputs['Fac'], 0.3, 0.6)), 0.85), c, p['tarnish'])
        c = mixc(nt, mul(nt, scratch, 0.4), c, p['trim_edge'])
        return c, mrange(nt, nfine.outputs['Fac'], 0, 1, 0.3, 0.5), 1.0, add(nt, mul(nt, nmid.outputs['Fac'], 0.5), mul(nt, scratch, 0.25)), 0.06
    if kind == 'Steel':
        c = mixc(nt, wear, p['steel'], (0.3, 0.29, 0.28))
        c = mixc(nt, mul(nt, mul(nt, cav, mrange(nt, nfine.outputs['Fac'], 0.3, 0.6)), 0.8), c, p['rust'])
        c = mixc(nt, mul(nt, scratch, 0.4), c, (0.3, 0.3, 0.31))
        return c, mrange(nt, nfine.outputs['Fac'], 0, 1, 0.38, 0.55), 0.9, add(nt, mul(nt, nmid.outputs['Fac'], 0.5), mul(nt, scratch, 0.25)), 0.06
    if kind == 'Suit' and ORDO:
        # cotta di maglia: anelli d'acciaio (Voronoi: il bordo di ogni cella e' l'anello, il centro e' il buco)
        vor = node(nt, 'ShaderNodeTexVoronoi'); vor.inputs['Scale'].default_value = 330.0; L(obj, vor.inputs['Vector'])
        rng = mul(nt, mrange(nt, vor.outputs['Distance'], 0.16, 0.26), mrange(nt, vor.outputs['Distance'], 0.48, 0.38))
        c = mixc(nt, rng, (0.012, 0.012, 0.013), (0.2, 0.2, 0.21))
        c = mixc(nt, mul(nt, cav, 0.6), c, (0.05, 0.035, 0.02))
        return c, mixf(nt, rng, 0.8, 0.42), mixf(nt, rng, 0.3, 0.95), rng, 0.5
    if kind == 'Suit':
        rib = node(nt, 'ShaderNodeTexWave', wave_type='BANDS', bands_direction='Z'); rib.inputs['Scale'].default_value = 55.0; L(obj, rib.inputs['Vector'])
        c = mixc(nt, rib.outputs['Fac'], (0.028, 0.028, 0.032), (0.06, 0.06, 0.065))
        return c, 0.72, 0.15, rib.outputs['Fac'], 0.3
    if kind == 'Cloth':
        weave = node(nt, 'ShaderNodeTexNoise', Scale=420.0, Detail=2.0); L(obj, weave.inputs['Vector'])
        sep = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, sep.inputs['Vector'])
        cloth = (0.62, 0.6, 0.55) if ORDO else (0.075, 0.065, 0.05)
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.0, 0.5 if not ORDO else 0.7), cloth, (0.1, 0.08, 0.06))
        c = mixc(nt, mul(nt, mrange(nt, sep.outputs['Z'], 1.0 if not ORDO else 0.85, 0.8 if not ORDO else 0.66), 0.7), c, (0.08, 0.06, 0.04))
        return c, 0.92, 0.0, weave.outputs['Fac'], 0.08
    if kind == 'Bone':
        crack = node(nt, 'ShaderNodeTexVoronoi', feature='DISTANCE_TO_EDGE'); crack.inputs['Scale'].default_value = 35.0; L(obj, crack.inputs['Vector'])
        c = mixc(nt, mrange(nt, crack.outputs['Distance'], 0.0, 0.04), (0.25, 0.2, 0.14), (0.74, 0.67, 0.52))
        return c, 0.6, 0.0, crack.outputs['Distance'], 0.12
    raise ValueError(kind)


# Foto CC0 sotto i materiali (photo_lib): tipo -> (foto, scala, forza colore, ruvidita', rilievo)
PHOTO = {'Plate': ('marble_rock_01', 1.5, 0.45, 0.2, 0.5) if ORDO else ('green_metal_rust', 1.3, 0.85, 0.2, 0.45),
         'Trim': ('rusty_metal_sheet', 2.0, 0.5, 0.2, 0.3),
         'Steel': ('rusty_metal_sheet', 2.0, 0.8, 0.25, 0.4),
         'Leather': ('brown_leather', 1.0, 0.75, 0.3, 0.7),
         'Cloth': ('hessian_230', 1.5, 0.7, 0.2, 0.6),
         'Suit': ('rubberized_track', 3.0, 0.6, 0.3, 0.5)}


def grime(nt, kind, col, rough):
    """campo di battaglia (solo generica): fango secco sulle gambe, colature scure dall'alto, fuliggine negli incavi"""
    if ORDO or kind not in ('Plate', 'Steel', 'Trim', 'Leather', 'Cloth'): return col, rough
    L = nt.links.new
    obj = node(nt, 'ShaderNodeTexCoord').outputs['Object']
    zs = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, zs.inputs['Vector'])
    nb = node(nt, 'ShaderNodeTexNoise', Scale=6.0, Detail=6.0, Roughness=0.65); L(obj, nb.inputs['Vector'])
    mud = mul(nt, mrange(nt, zs.outputs['Z'], 0.6, 0.1), mrange(nt, nb.outputs['Fac'], 0.35, 0.6, 0.35, 1.0))
    col = mixc(nt, mul(nt, mud, 0.85), col, (0.05, 0.037, 0.025))
    rough = mixf(nt, mul(nt, mud, 0.8), rough, 0.92)
    mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (26.0, 26.0, 1.4); L(obj, mp.inputs['Vector'])
    st = node(nt, 'ShaderNodeTexNoise', Scale=1.0, Detail=5.0); L(mp.outputs['Vector'], st.inputs['Vector'])
    col = mixc(nt, mul(nt, mrange(nt, st.outputs['Fac'], 0.52, 0.64), 0.45), col, (0.012, 0.011, 0.009))
    ao = node(nt, 'ShaderNodeAmbientOcclusion', Distance=0.08); ao.samples = 8
    col = mixc(nt, mul(nt, mrange(nt, ao.outputs['AO'], 0.8, 0.35), 0.55), col, (0.008, 0.007, 0.006))
    return col, rough


def photo_layer(nt, kind, col, rough, height):
    """foto proiettata sulle coordinate oggetto: colore relativo, scarto di ruvidita', rilievo"""
    if kind not in PHOTO or (kind == 'Suit' and ORDO): return col, rough, height
    pid, mult, kc, kr, kh = PHOTO[kind]
    tc = node(nt, 'ShaderNodeTexCoord')
    P = photo_lib.nodes(nt, tc.outputs['Object'], pid, mult)
    if P is None: return col, rough, height
    col = mixc(nt, kc, col, P['rel'], 'MULTIPLY')
    if 'rough' in P:
        r = nt.nodes.new('ShaderNodeMath'); r.operation = 'ADD'; r.use_clamp = True
        _set(nt, r.inputs[0], rough); _set(nt, r.inputs[1], mul(nt, P['rough'], kr)); rough = r.outputs[0]
    if 'disp' in P: height = add(nt, height, mul(nt, P['disp'], kh))
    return col, rough, height


def flat_material(name, color, rough, metal, emit=None, strength=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    bs = m.node_tree.nodes['Principled BSDF']
    bs.inputs['Base Color'].default_value = (*color, 1); bs.inputs['Roughness'].default_value = rough; bs.inputs['Metallic'].default_value = metal
    if emit:
        bs.inputs['Emission Color'].default_value = (*emit, 1); bs.inputs['Emission Strength'].default_value = strength
    return m


def bake_material(ob, kind):
    """UV, cottura con Cycles (colore, ORM, normali) e materiale finale a texture"""
    name = 'VG_' + kind
    activate(ob)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(50), island_margin=0.003)
    bpy.ops.object.mode_set(mode='OBJECT')
    size = TEX if kind in ('Plate',) else TEX // 2
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; L = nt.links.new
    out = nt.nodes['Material Output']; bs = nt.nodes['Principled BSDF']
    col, rough, metal, height, strength = procedural(nt, kind)
    col, rough, height = photo_layer(nt, kind, col, rough, height)
    col, rough = grime(nt, kind, col, rough)
    emis = node(nt, 'ShaderNodeEmission')
    ob.data.materials.clear(); ob.data.materials.append(m)
    imgs = {}

    def target(tag, noncolor):
        im = bpy.data.images.new(f'{name}_{tag}', size, size, alpha=False, float_buffer=False)
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
        for n in nt.nodes: n.select = False
        tn.select = True; nt.nodes.active = tn
        imgs[tag] = (im, tn)
        return im

    SC.render.engine = 'CYCLES'; SC.cycles.samples = 8; SC.cycles.device = 'CPU'
    SC.render.bake.margin = 4
    # colore
    target('BaseColor', False)
    _set(nt, emis.inputs['Color'], col)
    L(emis.outputs[0], out.inputs['Surface'])
    bpy.ops.object.bake(type='EMIT')
    # ORM: R=1, G=rugosita', B=metallo
    comb = node(nt, 'ShaderNodeCombineColor'); comb.inputs['Red'].default_value = 1.0
    for ch_, src in (('Green', rough), ('Blue', metal)): _set(nt, comb.inputs[ch_], float(src) if isinstance(src, (int, float)) else src)
    L(comb.outputs[0], emis.inputs['Color'])
    target('ORM', True)
    bpy.ops.object.bake(type='EMIT')
    # normali fini (rilievo dal rumore e dai graffi)
    bump = node(nt, 'ShaderNodeBump', Strength=strength, Distance=0.002)
    _set(nt, bump.inputs['Height'], height)
    L(bump.outputs['Normal'], bs.inputs['Normal'])
    L(bs.outputs[0], out.inputs['Surface'])
    target('Normal', True)
    SC.render.bake.normal_space = 'TANGENT'
    bpy.ops.object.bake(type='NORMAL')
    # materiale finale: solo texture
    for n in list(nt.nodes):
        if n not in (out, bs) and n.bl_idname != 'ShaderNodeTexImage': nt.nodes.remove(n)
    for n in [n for n in nt.nodes if n.bl_idname == 'ShaderNodeTexImage']: nt.nodes.remove(n)
    def tex(tag, noncolor):
        im = imgs[tag][0]; im.pack()
        tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        return tn
    L(tex('BaseColor', False).outputs['Color'], bs.inputs['Base Color'])
    sep = nt.nodes.new('ShaderNodeSeparateColor'); L(tex('ORM', True).outputs['Color'], sep.inputs['Color'])
    L(sep.outputs['Green'], bs.inputs['Roughness']); L(sep.outputs['Blue'], bs.inputs['Metallic'])
    nm = nt.nodes.new('ShaderNodeNormalMap'); L(tex('Normal', True).outputs['Color'], nm.inputs['Color']); L(nm.outputs['Normal'], bs.inputs['Normal'])
    L(bs.outputs[0], out.inputs['Surface'])
    print('cotto', name, size)


def quick_material(kind):
    p = PAL
    return {'Plate': flat_material('VG_Plate', p['plate'], p['plate_rough'], p['plate_metal']),
            'Trim': flat_material('VG_Trim', p['trim'], 0.4, 1.0),
            'Steel': flat_material('VG_Steel', p['steel'], 0.45, 0.9),
            'Suit': flat_material('VG_Suit', (0.04, 0.04, 0.045), 0.72, 0.15),
            'Cloth': flat_material('VG_Cloth', (0.8, 0.77, 0.7) if ORDO else (0.15, 0.13, 0.1), 0.92, 0.0),
            'Bone': flat_material('VG_Bone', (0.72, 0.66, 0.52), 0.6, 0.0),
            'Leather': flat_material('VG_Leather', (0.1, 0.07, 0.045), 0.7, 0.0)}[kind]


# ------------------------------------------------------------------ costruzione
if not FAST: photo_lib.ensure(sorted({v[0] for v in PHOTO.values()} | {'rusty_metal_04'}))
suit = build_suit()
build_armor_left()
if ORDO: ordo_extras()
mirror_left()
arm = build_armature()

# pesi: tuta dal calore di Blender, piastre rigide sul loro osso, stoffa dalla tuta vicina
activate(suit); arm.select_set(True); bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
from scipy.spatial import cKDTree
SV = np.array([v.co[:] for v in suit.data.vertices]); KD = cKDTree(SV)
for ob, w, mat in PARTS:
    ob.parent = arm
    mod = ob.modifiers.new('Armature', 'ARMATURE'); mod.object = arm
    if callable(w):
        for v in ob.data.vertices:
            for b_, x_ in w(np.array(v.co[:])).items():
                gg = ob.vertex_groups.get(PREFIX + b_) or ob.vertex_groups.new(name=PREFIX + b_)
                gg.add([v.index], float(x_), 'REPLACE')
    elif w == 'near':
        pv = np.array([v.co[:] for v in ob.data.vertices]); _, nn = KD.query(pv)
        for vi, bi in enumerate(nn):
            for g in suit.data.vertices[int(bi)].groups:
                gname = suit.vertex_groups[g.group].name
                gg = ob.vertex_groups.get(gname) or ob.vertex_groups.new(name=gname)
                gg.add([vi], g.weight, 'REPLACE')
    else:
        for b, x in w.items():
            g = ob.vertex_groups.new(name=PREFIX + b); g.add(list(range(len(ob.data.vertices))), float(x), 'REPLACE')

# unisce per materiale (un oggetto per materiale: meno chiamate di disegno, una texture per tipo)
groups = {}
for ob, w, mat in PARTS: groups.setdefault(mat, []).append(ob)
groups.setdefault('Suit', []).append(suit)
merged = {}
for mat, obs in groups.items():
    for o in bpy.context.view_layer.objects: o.select_set(False)
    for o in obs: o.select_set(True)
    bpy.context.view_layer.objects.active = obs[0]
    if len(obs) > 1: bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active; ob.name = 'VG_' + mat
    if not ob.modifiers.get('Armature'):
        mod = ob.modifiers.new('Armature', 'ARMATURE'); mod.object = arm
    ob.parent = arm
    merged[mat] = ob
    print(mat, len(ob.data.vertices), 'vertici', len(ob.data.polygons), 'facce')

p = PAL
for mat, ob in merged.items():
    if mat == 'Cross': ob.data.materials.append(flat_material('VG_Cross', (0.012, 0.012, 0.014), 0.38, 0.55))
    elif mat == 'Core': ob.data.materials.append(flat_material('VG_Core', p['core'], 0.06, 0.0, p['core_emit'], 2.5 if not ORDO else 1.0))
    elif mat == 'Glow': ob.data.materials.append(flat_material('VG_Glow', tuple(x * 0.2 for x in p['glow']), 0.2, 0.0, p['glow'], 2.5))
    elif FAST: ob.data.materials.append(quick_material(mat))
    else: bake_material(ob, mat)

for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
blend = os.path.join(HERE, f'vanguard_{VAR}.blend')
bpy.ops.wm.save_as_mainfile(filepath=blend)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
for o in bpy.context.view_layer.objects: o.select_set(o.type in ('MESH', 'ARMATURE'))
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_skins=True, export_animations=False,
                          export_apply=False, export_yup=True, export_image_format='JPEG', export_jpeg_quality=92)
print('esportato', OUT)
