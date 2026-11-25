# Modello d'autore della Vanguard Armor: un umano potenziato chiuso in un carapace spesso e aderente.
#
#   python Tools/ModelForge/vanguard/build.py                    -> Models/Characters/vanguard/model.glb (armatura generica)
#   python Tools/ModelForge/vanguard/build.py --variant ordo     -> Models/Characters/vanguard/variants/vanguard_konrad.glb
#
# Com'e' fatto (vedi README.md):
#   - tuta sotto (campo di distanza -> marching cubes): si vede solo ai giunti, pesi di Blender (calore)
#   - carapace a piastre rigide: gusci a superellissoide con spessore e smussi, ognuno sul suo osso (come un'armatura
#     vera: le piastre non si stirano, scorrono una sull'altra)
#   - nucleo di pietra sul petto: ossidiana viola (generica) o nera incastonata nella pietra bianca (Ordo)
#   - texture cotte con Cycles: colore (usura sugli spigoli, sporco negli incavi), rugosita'/metallo, normali fini
import sys, os, json, math
import numpy as np
import bpy, bmesh
from mathutils import Vector

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


def build_armor_left():
    """piastre del lato sinistro (poi specchiate) e del centro"""
    j = J; ch = AXES; PI = math.pi
    FRONT, BACK = (-PI + 0.2, -0.2), (0.2, PI - 0.2)
    # ---- torso: corazza a prua, pettorale sovrapposto, schiena larga (sagoma a V)
    shell('Cuirass_F', 'Spine2', (0, -0.02, 1.64), ch, (0.30, 0.20, 0.25), pu=7, pv=5, u=FRONT, v=(-0.95, 1.1), thick=0.04, bevel=0.008, keel=0.05, taper=(0.14, 0.05))
    shell('Cuirass_B', 'Spine2', (0, 0.01, 1.65), ch, (0.31, 0.21, 0.25), pu=7, pv=5, u=BACK, v=(-0.9, 1.15), thick=0.04, bevel=0.008, taper=(0.14, 0.04))
    shell('Plastron', 'Spine2', (0, -0.065, 1.745), ch, (0.27, 0.165, 0.115), pu=7, pv=4.5, u=(-PI + 0.3, -0.3), v=(-0.85, 1.0), thick=0.026, bevel=0.006, keel=0.05, taper=(0.1, 0.0))
    for s_ in (1, -1):
        shell('Side' + ('L' if s_ > 0 else 'R'), 'Spine2', (0.25 * s_, 0.0, 1.62), ch, (0.07, 0.15, 0.17), pu=3, pv=4, u=((-0.9, 0.9) if s_ > 0 else (PI - 0.9, PI + 0.9)),
              v=(-1.0, 1.0), thick=0.025, bevel=0.005, mat='Trim')
    # nucleo di pietra: incastonatura, anello, gemma
    gax = (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, -1.0, 0]))
    gc = np.array([0, -0.262, 1.70])
    block('CoreSocket', 'Spine2', gc + [0, 0.02, 0], (AXES[0], AXES[2], AXES[1]), (0.08, 0.08, 0.03), p=3.2, mat='Plate' if ORDO else 'Trim', bevel=0.003)
    ring('CoreRing', 'Spine2', gc + [0, -0.012, 0], gax, 0.066, 0.012, mat='Trim')
    gem('Core', 'Spine2', gc + [0, -0.016, 0], gax, 0.05, mat='Core')
    rivet_row('RivCuirass', 'Spine2', (0, -0.02, 1.64), ch, (0.30, 0.20, 0.25), -PI + 0.45, -0.45, 12, -0.92, pu=7, pv=5, taper=(0.14, 0.05))
    rivet_row('RivPlastron', 'Spine2', (0, -0.065, 1.745), ch, (0.27, 0.165, 0.115), -PI + 0.5, -0.5, 9, 0.95, pu=7, pv=4.5, taper=(0.1, 0.0))
    if not ORDO:
        tube('Strap', [(0.30, -0.05, 1.86), (0.2, -0.235, 1.80), (0.0, -0.275, 1.62), (-0.18, -0.24, 1.42), (-0.26, -0.12, 1.30)], 0.02, weights={'Spine2': 0.7, 'Spine1': 0.3}, mat='Leather', sides=6)
        block('StrapBuckle', 'Spine2', (0.11, -0.272, 1.73), ch, (0.03, 0.012, 0.035), p=6, mat='Plate')
        for side, y, sg in (('F', -0.2, -1), ('B', 0.2, 1)):
            V, F = [], []
            nz, nx = 12, 7
            for i in range(nz + 1):
                z = 1.23 - i * (0.42 / nz)
                for k in range(nx + 1):
                    x = -0.13 + 0.26 * k / nx
                    tear = 0.04 * math.sin(k * 2.7 + 1.3) if i == nz else 0.0
                    V.append((x * (1 + 0.15 * i / nz), y + sg * (0.02 + 0.03 * math.cos(math.pi * (k / nx - 0.5)) + 0.03 * i / nz), z + tear))
            for i in range(nz):
                for k in range(nx): F.append((i * (nx + 1) + k, i * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k + 1, (i + 1) * (nx + 1) + k))
            ob = link('Loin' + side, np.array(V), F); orient_out(ob, (0, 0, 1.0)); finish(ob, 0.006, 0.0)
            PARTS.append((ob, {'Hips': 1.0}, 'Cloth'))   # rigida sul bacino: dai pesi delle gambe si strapperebbe
        chain('BeltChain', [(-0.24, -0.16, 1.24), (-0.12, -0.235, 1.19), (0.05, -0.24, 1.2), (0.2, -0.19, 1.24)])
    # dorso: zaino d'energia, prese d'aria, alette di raffreddamento
    block('PowerPack', 'Spine2', (0, 0.29, 1.64), ch, (0.22, 0.1, 0.21), p=5, mat='Plate', bevel=0.006, taper=(0.1, 0.0))
    block('PackCap', 'Spine2', (0, 0.27, 1.86), ch, (0.18, 0.09, 0.045), p=6, mat='Trim', bevel=0.003)
    for s_ in (1, -1):
        cax = frame((0.25 * s_, 0.35, 1.0))
        shell('Vent' + ('L' if s_ > 0 else 'R'), 'Spine2', (0.13 * s_, 0.35, 1.86), cax, (0.05, 0.05, 0.1), pu=2, pv=8, v=(-1.3, 1.4), thick=0.014, bevel=0.002, mat='Trim', nu=24, nv=6)
        for k in range(5): block(f'Fin{k}' + ('L' if s_ > 0 else 'R'), 'Spine2', (0.225 * s_, 0.37, 1.50 + k * 0.045), ch, (0.03, 0.03, 0.009), p=6, mat='Trim')
    # vita: tre lame sull'addome, fascia sotto, cintura piatta
    shell('Waist', 'Spine1', (0, 0.0, 1.44), ch, (0.215, 0.165, 0.12), pu=3, pv=4, v=(-1.2, 1.2), thick=0.02, bevel=0.004, mat='Trim')
    for k, (z, bn) in enumerate(((1.515, 'Spine1'), (1.445, 'Spine1'), (1.375, 'Spine'))):
        shell(f'Ab{k}', bn, (0, -0.02, z), ch, (0.205 - 0.008 * k, 0.17, 0.05), pu=4, pv=6, u=(-PI + 0.45, -0.45), v=(-1.25, 1.25), thick=0.022, bevel=0.005, keel=0.03)
    shell('Belt', 'Hips', (0, -0.01, 1.27), ch, (0.255, 0.195, 0.045), pu=5, pv=8, v=(-1.3, 1.3), thick=0.03, bevel=0.004, mat='Trim')
    block('Buckle', 'Hips', (0, -0.215, 1.27), ch, (0.075, 0.022, 0.048), p=6, mat='Plate', bevel=0.003)
    block('Cod', 'Hips', (0, -0.17, 1.16), ch, (0.085, 0.05, 0.075), p=4, mat='Plate', bevel=0.004, taper=(0.25, 0.0))
    # goletta alta e collare dietro
    shell('Gorget', 'Spine2', (0, 0.03, 1.85), ch, (0.185, 0.17, 0.07), pu=5, pv=4, v=(-1.0, 1.15), thick=0.032, bevel=0.006, mat='Trim', taper=(-0.22, -0.2))
    shell('Collar', 'Spine2', (0, 0.08, 1.9), ch, (0.21, 0.13, 0.09), pu=5, pv=4, u=BACK, v=(-1.0, 1.2), thick=0.03, bevel=0.006, taper=(0.1, 0.0))
    shell('NeckGuard', 'Neck', (0, 0.03, 1.915), ch, (0.12, 0.12, 0.045), pu=3, pv=4, v=(-1.0, 1.0), thick=0.02, bevel=0.004, mat='Plate')
    build_helmet()
    # ---- braccio sinistro
    A, E, W, Hn = j['LeftArm'], j['LeftForeArm'], j['LeftHand'], j['LeftHandMiddle1']
    ax = limb_axes(A, E, (0, 0, 1))
    pc = np.array([A[0] + 0.075, A[1] + 0.005, A[2] + 0.055])
    pax = frame((1.0, 0, 0.6), (0, -1, 0))
    PW = {'LeftArm': 0.75, 'LeftShoulder': 0.25}
    shell('Pauldron', None, pc, pax, (0.2, 0.23, 0.17), pu=4, pv=3, v=(-0.3, PI / 2), thick=0.05, bevel=0.008, weights=PW, keel=0.0)
    shell('PauldronRim', None, pc, pax, (0.21, 0.24, 0.17), pu=4, pv=3, v=(-0.38, -0.05), thick=0.02, bevel=0.004, mat='Trim', weights=PW)
    for k in range(2):
        shell(f'PauldronLame{k}', None, pc + pax[2] * (-0.08 - 0.06 * k), pax, (0.21 - 0.012 * k, 0.235 - 0.012 * k, 0.1), pu=4, pv=3.4,
              v=(-0.45, 0.25), thick=0.022, bevel=0.004, weights={'LeftArm': 0.9, 'LeftShoulder': 0.1})
    rivet_row('RivPauldron', None, pc, pax, (0.2, 0.23, 0.17), -PI, PI, 14, -0.22, weights=PW, pu=4, pv=3)
    shell('Rerebrace', 'LeftArm', mid(A, E, 0.58), ax, (0.1, 0.105, 0.125), pu=6, pv=7, v=(-1.25, 1.25), thick=0.03, bevel=0.005, taper=(0.06, 0.06))
    ring('ElbowRing', 'LeftArm', mid(A, E, 0.95), ax, 0.08, 0.016, mat='Trim')
    shell('Couter', 'LeftForeArm', E + [0.0, 0.055, 0.0], frame((0, 1.0, 0.0), (0, 0, 1)), (0.08, 0.075, 0.065), pu=3, pv=2.6, v=(0.0, PI / 2), thick=0.022, bevel=0.005, keel=0.0)
    fax = limb_axes(E, W, (0, 0, 1))
    shell('Vambrace', 'LeftForeArm', mid(E, W, 0.52), fax, (0.097, 0.1, 0.13), pu=6, pv=7, v=(-1.25, 1.25), thick=0.032, bevel=0.005, taper=(0.12, 0.12))
    shell('VambraceTop', 'LeftForeArm', mid(E, W, 0.55), fax, (0.105, 0.11, 0.1), pu=4, pv=5, u=(0.45, PI - 0.45), v=(-1.1, 1.1),
          thick=0.016, bevel=0.003, mat='Trim', taper=(0.12, 0.12))
    hax = limb_axes(W, Hn, (0, 0, 1))
    shell('Cuff', 'LeftHand', W + (W - E) * 0.05, hax, (0.08, 0.085, 0.04), pu=3.5, pv=6, v=(-1.2, 1.2), thick=0.018, bevel=0.003, mat='Trim')
    block('HandBack', 'LeftHand', mid(W, Hn, 0.55) + [0, 0, 0.026], hax, (0.06, 0.03, 0.08), p=4, mat='Plate', bevel=0.003)
    block('Knuckles', 'LeftHand', Hn + [-0.01, 0, 0.022], hax, (0.06, 0.02, 0.022), p=5, mat='Trim', bevel=0.002)
    block('Palm', 'LeftHand', mid(W, Hn, 0.55) + [0, 0, -0.012], hax, (0.055, 0.022, 0.075), p=3.5, mat='Suit')
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        chn = [j[f'LeftHand{f}{k}'] for k in (1, 2, 3)]
        tip = chn[2] + (chn[2] - chn[1]) * 0.9
        pts = chn + [tip]
        for k in range(3):
            a_, b_ = pts[k], pts[k + 1]
            fa = limb_axes(a_, b_, (0, 0, 1)); L_ = np.linalg.norm(b_ - a_)
            block(f'{f}{k}', f'LeftHand{f}{k + 1}', mid(a_, b_), fa, (0.022, 0.021, L_ * 0.5 + 0.005), p=4, mat='Trim' if k == 0 else 'Plate', bevel=0.0015)
    # ---- gamba sinistra
    Hp, K, F, T = j['LeftUpLeg'], j['LeftLeg'], j['LeftFoot'], j['LeftToeBase']
    tax = limb_axes(Hp, K, (0, 1, 0))
    shell('Cuisse', 'LeftUpLeg', mid(Hp, K, 0.5), tax, (0.14, 0.15, 0.2), pu=6, pv=7, v=(-1.2, 1.2), thick=0.034, bevel=0.006, taper=(-0.12, -0.1), keel=0.02)
    shell('CuisseOuter', 'LeftUpLeg', mid(Hp, K, 0.45), tax, (0.15, 0.16, 0.16), pu=4, pv=5, u=(-1.0, 1.0) , v=(-1.0, 1.1), thick=0.018, bevel=0.004, mat='Trim', taper=(-0.1, -0.1))
    shell('Tasset', None, Hp + [0.07, -0.06, -0.04], frame((1.0, -0.55, 0.15), (0, 0, 1)), (0.12, 0.16, 0.12), pu=3.5, pv=3, v=(-0.5, 0.8), thick=0.024, bevel=0.005,
          weights={'LeftUpLeg': 0.8, 'Hips': 0.2})
    shell('Poleyn', 'LeftLeg', K + [0, -0.085, 0.01], frame((0, -1.0, 0), (0, 0, 1)), (0.095, 0.1, 0.075), pu=3, pv=2.6, v=(0.0, PI / 2), thick=0.028, bevel=0.006, keel=0.0)
    gax2 = limb_axes(K, F, (0, 1, 0))
    ring('KneeRing', 'LeftLeg', K + (F - K) * 0.12, gax2, 0.09, 0.014, mat='Trim')
    shell('Greave', 'LeftLeg', mid(K, F, 0.5), gax2, (0.115, 0.125, 0.22), pu=6, pv=7, v=(-1.2, 1.2), thick=0.036, bevel=0.006, taper=(-0.12, -0.05), keel=0.035)
    rivet_row('RivGreave', 'LeftLeg', mid(K, F, 0.5), gax2, (0.115, 0.125, 0.22), -PI + 0.6, -0.6, 4, 1.1, pu=6, pv=7, taper=(-0.12, -0.05))
    shell('GreaveBack', 'LeftLeg', mid(K, F, 0.38), gax2, (0.12, 0.13, 0.13), pu=4, pv=5, u=BACK, v=(-1.0, 1.0), thick=0.018, bevel=0.004, mat='Trim', taper=(-0.1, -0.05))
    bax = AXES
    block('Boot', 'LeftFoot', (F[0], F[1] - 0.045, 0.09), bax, (0.092, 0.17, 0.09), p=4, mat='Plate', bevel=0.005, keel=0.0, taper=(-0.08, 0.0))
    block('Sole', 'LeftFoot', (F[0], F[1] - 0.05, 0.02), bax, (0.1, 0.185, 0.022), p=6, mat='Trim', bevel=0.002)
    block('ToeCap', 'LeftToeBase', (T[0], T[1] - 0.06, 0.06), bax, (0.085, 0.08, 0.055), p=3.6, mat='Plate', bevel=0.004)
    shell('AnkleGuard', 'LeftFoot', F + [0, 0, 0.045], gax2, (0.1, 0.11, 0.055), pu=4, pv=5, v=(-1.1, 1.1), thick=0.02, bevel=0.003, mat='Trim')


def build_helmet():
    ch = AXES; PI = math.pi
    hc = np.array([0, 0.0, 2.025])
    if ORDO:
        # grande elmo a secchio: fianchi dritti, cima bombata, feritoia a croce col rinforzo verticale
        shell('Helm', 'Head', hc, ch, (0.145, 0.16, 0.17), pu=3.2, pv=7, v=(-1.35, 1.25), thick=0.032, bevel=0.006, keel=0.03)
        shell('HelmTop', 'Head', hc + [0, 0, 0.125], ch, (0.14, 0.155, 0.07), pu=3.2, pv=2.4, v=(0.0, PI / 2), thick=0.03, bevel=0.006)
        block('Slit', 'Head', hc + [0, -0.17, 0.025], ch, (0.115, 0.012, 0.011), p=6, mat='Glow')
        block('SlitBand', 'Head', hc + [0, -0.18, 0.05], ch, (0.135, 0.013, 0.014), p=6, mat='Trim', bevel=0.002)
        block('SlitBandLow', 'Head', hc + [0, -0.18, 0.0], ch, (0.135, 0.013, 0.012), p=6, mat='Trim', bevel=0.002)
        block('Rib', 'Head', hc + [0, -0.19, -0.075], ch, (0.018, 0.014, 0.075), p=6, mat='Trim', bevel=0.002)
        cross_pattee('CrossHelm', 'Head', hc + [0, -0.03, 0.198], (np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), np.array([0, 0, 1.0])), 0.1)
        for k in range(3):
            for s_ in (1, -1): block(f'Breath{k}{s_}', 'Head', hc + [0.05 * s_, -0.178, -0.06 - 0.03 * k], ch, (0.008, 0.008, 0.008), p=2, mat='Cross')
        return
    # elmo chiuso del carapace: calotta bassa, fronte pesante a sbalzo, maschera a cuneo, feritoie a V strette e
    # incassate, grata della bocca a denti, guance e nuca a lame, tubi verso lo zaino
    shell('Skull', 'Head', hc + [0, 0.02, 0.02], ch, (0.148, 0.168, 0.15), pu=5, pv=3.4, v=(-0.45, PI / 2), thick=0.032, bevel=0.006, keel=0.025)
    shell('Brow', 'Head', hc + [0, -0.045, 0.062], ch, (0.158, 0.135, 0.042), pu=7, pv=5, u=(-PI + 0.2, -0.2), v=(-1.0, 1.0), thick=0.03, bevel=0.006, keel=0.03, taper=(0.0, 0.15))
    shell('Mask', 'Head', hc + [0, -0.02, -0.05], ch, (0.13, 0.145, 0.11), pu=6, pv=4, u=(-PI + 0.35, -0.35), v=(-1.25, 0.45), thick=0.028, bevel=0.005,
          keel=0.035, taper=(0.3, 0.0))
    shell('Cheeks', 'Head', hc + [0, 0.01, -0.045], ch, (0.15, 0.162, 0.115), pu=6, pv=5, u=(-0.95, PI + 0.95), v=(-1.2, 0.6), thick=0.028, bevel=0.005, mat='Trim')
    shell('NapeGuard', 'Head', hc + [0, 0.07, -0.085], ch, (0.155, 0.12, 0.06), pu=6, pv=5, u=BACK_, v=(-1.0, 1.0), thick=0.026, bevel=0.004, mat='Plate', taper=(0.15, 0.0))
    for s_ in (1, -1):
        ang = math.radians(20) * s_
        vax = (np.array([math.cos(ang), 0, math.sin(ang)]), np.array([0, 1.0, 0]), np.array([-math.sin(ang), 0, math.cos(ang)]))
        block('VisorPit' + str(s_), 'Head', hc + [0.05 * s_, -0.185, 0.018], vax, (0.058, 0.024, 0.017), p=7, mat='Trim', bevel=0.002)
        block('Visor' + str(s_), 'Head', hc + [0.05 * s_, -0.205, 0.018], vax, (0.046, 0.006, 0.006), p=8, mat='Glow')
        block('Ear' + ('L' if s_ > 0 else 'R'), 'Head', hc + [0.155 * s_, 0.01, -0.015], frame((s_, 0, 0)), (0.055, 0.06, 0.025), p=6, mat='Trim', bevel=0.003)
        tube('Hose' + ('L' if s_ > 0 else 'R'), [hc + [0.1 * s_, -0.12, -0.11], hc + [0.14 * s_, -0.05, -0.15], (0.17 * s_, 0.02, 1.83), (0.15 * s_, 0.2, 1.80), (0.12 * s_, 0.31, 1.78)],
             0.016, ribs=3)
    block('MouthSlot', 'Head', hc + [0, -0.185, -0.1], ch, (0.06, 0.02, 0.03), p=7, mat='Trim')
    for k in range(7): block(f'Tooth{k}', 'Head', hc + [-0.048 + 0.016 * k, -0.205, -0.1], ch, (0.0055, 0.008, 0.03 - 0.004 * abs(k - 3)), p=4, mat='Plate')
    block('Chin', 'Head', hc + [0, -0.18, -0.15], ch, (0.04, 0.03, 0.022), p=6, mat='Trim', bevel=0.002)
    block('CrestRidge', 'Head', hc + [0, -0.01, 0.16], ch, (0.014, 0.14, 0.016), p=6, mat='Trim')


BACK_ = (0.25, math.pi - 0.25)


def ordo_extras():
    """Ordo: croci patenti, ossa, teschio, sopravveste"""
    ch = AXES
    # croci: sul petto sopra il nucleo, sulla schiena, sugli spallacci
    cross_pattee('CrossChest', 'Spine2', np.array([0, -0.252, 1.815]), (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, -1.0, 0])), 0.11)
    cross_pattee('CrossBack', 'Spine2', np.array([0, 0.362, 1.66]), (np.array([-1.0, 0, 0]), np.array([0, 0, 1.0]), np.array([0, 1.0, 0])), 0.22)
    A = J['LeftArm']
    pc = np.array([A[0] + 0.05, A[1] + 0.005, A[2] + 0.03]) + [0.02, 0, 0.03]
    n = norm((1.0, 0, 0.55)); ax_ = norm(np.cross((0, 0, 1.0), n)); ay_ = np.cross(n, ax_)
    cross_pattee('CrossPauldronL', 'LeftArm', pc + n * 0.172, (ax_, ay_, n), 0.13)
    PARTS[-1] = (PARTS[-1][0], {'LeftArm': 0.75, 'LeftShoulder': 0.25}, 'Cross')
    # ossa: costole d'avorio sulla goletta, teschio sulla fibbia con due femori incrociati
    for k in range(7):
        a = -0.9 + k * 0.3
        c = np.array([math.sin(a) * 0.205, -math.cos(a) * 0.19 + 0.03, 1.84])
        block(f'Rib{k}', 'Spine2', c, frame((math.sin(a), -math.cos(a), 0.0), (0, 0, 1)), (0.016, 0.018, 0.06), p=2.4, mat='Bone')
    sk = np.array([0, -0.25, 1.265])
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
    chain('BeltChain', [(-0.24, -0.16, 1.24), (-0.12, -0.235, 1.19), (0.05, -0.24, 1.2), (0.2, -0.19, 1.24)])
    # sopravveste: davanti e dietro, dalla cintura al ginocchio, con la croce nera (pesi dalla tuta)
    for side, y, sgn in (('F', -0.215, -1), ('B', 0.215, 1)):
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
    'generic': dict(plate=(0.085, 0.082, 0.08), plate_metal=0.8, plate_rough=0.5, edge=(0.5, 0.49, 0.47), trim=(0.025, 0.024, 0.024),
                    core=(0.12, 0.02, 0.22), core_emit=(0.45, 0.06, 0.85), glow=(0.55, 0.12, 0.95), rust=(0.2, 0.075, 0.025)),
    'ordo': dict(plate=(0.30, 0.28, 0.24), plate_metal=0.0, plate_rough=0.66, edge=(0.46, 0.43, 0.38), trim=(0.02, 0.02, 0.022),
                 core=(0.006, 0.006, 0.008), core_emit=(0.0, 0.0, 0.0), glow=(0.55, 0.12, 0.95), rust=(0.2, 0.15, 0.09)),
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
    pt = geo.outputs['Pointiness']
    edge = mrange(nt, pt, 0.6, 0.7)                    # spigoli vivi
    cav = mrange(nt, pt, 0.49, 0.44)                      # incavi
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
            c = mixc(nt, mul(nt, mrange(nt, vein.outputs['Fac'], 0.96, 1.0), 0.7), p['plate'], (0.40, 0.39, 0.38))
            c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.0, 0.25), c, (0.42, 0.38, 0.31))
            mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (22.0, 22.0, 1.6); L(obj, mp.inputs['Vector'])
            drip = node(nt, 'ShaderNodeTexNoise', Scale=1.0, Detail=5.0); L(mp.outputs['Vector'], drip.inputs['Vector'])
            c = mixc(nt, mul(nt, mrange(nt, drip.outputs['Fac'], 0.5, 0.62), 0.75), c, (0.1, 0.085, 0.06))
            zs = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, zs.inputs['Vector'])
            c = mixc(nt, mul(nt, mrange(nt, zs.outputs['Z'], 1.3, 0.1), 0.6), c, (0.09, 0.075, 0.055))
            chip = node(nt, 'ShaderNodeTexVoronoi'); chip.inputs['Scale'].default_value = 40.0; L(obj, chip.inputs['Vector'])
            c = mixc(nt, mul(nt, mul(nt, edge, mrange(nt, chip.outputs['Distance'], 0.18, 0.05)), 0.9), c, (0.12, 0.11, 0.1))
        else:
            c = mixc(nt, 0.35, p['plate'], mrange(nt, nbig.outputs['Fac'], 0, 1, 0.8, 1.2), 'MULTIPLY')
        rustm = mul(nt, add(nt, cav, mrange(nt, nbig.outputs['Fac'], 0.58, 0.78, 0.0, 0.5)), mrange(nt, nmid.outputs['Fac'], 0.35, 0.65))
        c = mixc(nt, wear, c, p['edge'])
        c = mixc(nt, mul(nt, rustm, 0.75), c, p['rust'])
        c = mixc(nt, mul(nt, cav, 0.6), c, (0.02, 0.018, 0.015))
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
        c = mixc(nt, wear, p['trim'], (0.3, 0.29, 0.28))
        c = mixc(nt, mul(nt, mul(nt, cav, mrange(nt, nfine.outputs['Fac'], 0.3, 0.6)), 0.8), c, p['rust'])
        c = mixc(nt, mul(nt, scratch, 0.4), c, (0.3, 0.3, 0.31))
        return c, mrange(nt, nfine.outputs['Fac'], 0, 1, 0.38, 0.55), 0.9, add(nt, mul(nt, nmid.outputs['Fac'], 0.5), mul(nt, scratch, 0.25)), 0.06
    if kind == 'Suit':
        rib = node(nt, 'ShaderNodeTexWave', wave_type='BANDS', bands_direction='Z'); rib.inputs['Scale'].default_value = 55.0; L(obj, rib.inputs['Vector'])
        c = mixc(nt, rib.outputs['Fac'], (0.028, 0.028, 0.032), (0.06, 0.06, 0.065))
        return c, 0.72, 0.15, rib.outputs['Fac'], 0.3
    if kind == 'Cloth':
        weave = node(nt, 'ShaderNodeTexNoise', Scale=420.0, Detail=2.0); L(obj, weave.inputs['Vector'])
        sep = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, sep.inputs['Vector'])
        cloth = (0.42, 0.39, 0.33) if ORDO else (0.075, 0.065, 0.05)
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.0, 0.5 if not ORDO else 0.7), cloth, (0.1, 0.08, 0.06))
        c = mixc(nt, mul(nt, mrange(nt, sep.outputs['Z'], 1.0 if not ORDO else 0.85, 0.8 if not ORDO else 0.66), 0.7), c, (0.08, 0.06, 0.04))
        return c, 0.92, 0.0, weave.outputs['Fac'], 0.08
    if kind == 'Bone':
        crack = node(nt, 'ShaderNodeTexVoronoi', feature='DISTANCE_TO_EDGE'); crack.inputs['Scale'].default_value = 35.0; L(obj, crack.inputs['Vector'])
        c = mixc(nt, mrange(nt, crack.outputs['Distance'], 0.0, 0.04), (0.25, 0.2, 0.14), (0.74, 0.67, 0.52))
        return c, 0.6, 0.0, crack.outputs['Distance'], 0.12
    raise ValueError(kind)


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

    SC.render.engine = 'CYCLES'; SC.cycles.samples = 1; SC.cycles.device = 'CPU'
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
            'Trim': flat_material('VG_Trim', p['trim'], 0.45, 0.9),
            'Suit': flat_material('VG_Suit', (0.04, 0.04, 0.045), 0.72, 0.15),
            'Cloth': flat_material('VG_Cloth', (0.8, 0.77, 0.7) if ORDO else (0.15, 0.13, 0.1), 0.92, 0.0),
            'Bone': flat_material('VG_Bone', (0.72, 0.66, 0.52), 0.6, 0.0),
            'Leather': flat_material('VG_Leather', (0.1, 0.07, 0.045), 0.7, 0.0)}[kind]


# ------------------------------------------------------------------ costruzione
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
    if w == 'near':
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
    elif mat == 'Glow': ob.data.materials.append(flat_material('VG_Glow', (0.1, 0.03, 0.2), 0.2, 0.0, p['glow'], 4.0))
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
