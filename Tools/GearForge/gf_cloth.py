# Vesti di stoffa: camicie e giubbe, pantaloni, calzoni, stracci. Zona presa dal corpo come nel gioco, orli
# tagliati dritti con dei piani, giro largo, poi la stoffa cade e si raggrinza sul corpo (simulazione di Blender).
import bpy, math
import numpy as np
from mathutils import Vector
import gf_geom as G


def simulate(cloth, body, pin, frames=30, shrink=-0.03, shrink_group=None, mass=0.3, bending=0.6, quality=6,
             gravity=1.0, distance_min=0.003):
    """La stoffa cade sul corpo. pin = vertici fermi (cuciture, colletto, cintura). shrink < 0 = tessuto in
    piu' che si raggrinza (pieghe), shrink_group = quanto per vertice (0..1)"""
    print(f"  stoffa {cloth.name}: {int(pin.sum())} vertici fermi su {len(pin)}, nan {int(np.isnan(G.verts(cloth)).sum())}")
    vg = cloth.vertex_groups.new(name="pin")
    vg.add([int(i) for i in np.where(pin)[0]], 1.0, 'REPLACE')
    if shrink_group is not None:
        sg = cloth.vertex_groups.new(name='shrink')
        for i, w in enumerate(shrink_group):
            if w > 0.001: sg.add([i], float(w), 'REPLACE')
    col = body.obj.modifiers.new('col', 'COLLISION')
    body.obj.collision.thickness_outer = 0.003
    body.obj.collision.cloth_friction = 8.0
    cm = cloth.modifiers.new('cloth', 'CLOTH'); s = cm.settings
    s.vertex_group_mass = 'pin'; s.quality = quality; s.mass = mass
    s.tension_stiffness = 15; s.compression_stiffness = 15; s.shear_stiffness = 8
    s.bending_stiffness = bending
    s.shrink_min = shrink
    if shrink_group is not None:
        s.vertex_group_shrink = 'shrink'; s.shrink_max = 0.0
    s.effector_weights.gravity = gravity
    cs = cm.collision_settings
    cs.distance_min = distance_min; cs.use_self_collision = False
    cs.collision_quality = 3
    sc = bpy.context.scene; sc.frame_start = 1; sc.frame_end = frames
    cm.point_cache.frame_end = frames
    P0 = G.verts(cloth)
    for f in range(1, frames + 1): sc.frame_set(f)
    bpy.context.view_layer.objects.active = cloth
    bpy.ops.object.modifier_apply(modifier='cloth')
    mv = np.linalg.norm(G.verts(cloth) - P0, axis=1)
    print(f'  stoffa {cloth.name}: spostamento medio {mv.mean()*1000:.1f} mm, massimo {mv.max()*1000:.1f} mm')
    body.obj.modifiers.remove(body.obj.modifiers['col'])
    sc.frame_set(1)
    for g in list(cloth.vertex_groups): cloth.vertex_groups.remove(g)


def near_plane(P, co, no, d):
    return np.abs((P - co) @ no) < d


def prepare(body, g, name, loosen=0.12, extra_h=(0.03, 0.01)):
    """zona del corpo un po' piu' larga di quella del gioco (i bordi veri li fanno i tagli)"""
    g2 = dict(g)
    g2['min_weight'] = max(0.08, g.get('min_weight', 0.5) - loosen)
    hr = g.get('height_range', [0, 1])
    g2['height_range'] = [hr[0] - extra_h[0], hr[1] + extra_h[1]]
    if 'side' in g2: g2.pop('side')
    mask = body.region(g2)
    ob = G.region_obj(body, mask, name, dilate=1)
    G.keep_largest(ob)
    return ob


def finish_cloth(ob, thick=0.0025, edge_store=True, target_faces=6000):
    G.taubin(ob, iters=3)                         # via le grinze minute, restano le pieghe
    n = len(ob.data.polygons)
    if n > target_faces: G.decimate(ob, target_faces / n)
    G.smooth_boundary(ob, iters=6, lam=0.4)
    if edge_store: G.store(ob, 'edge', G.edge_distance(ob))
    G.solidify(ob, thick, offset=-1.0)
    return ob


# ------------------------------------------------------------------------------------------------- giubba
def shirt(body, g, name, ease=0.010, sleeves_to=0.82, collar=True, frames=28, shrink=0.0, bending=2.0):
    H = body.H
    ob = prepare(body, g, name)
    hr = g.get('height_range', [0.5, 0.835])
    # orlo in basso dritto, scollo
    G.cut(ob, (0, 0, body.at_height(hr[0] + 0.035)), (0, 0, 1))
    neck = body.head('Neck') if body.has('Neck') else np.array([0, 0, body.at_height(hr[1])])
    zc = min(body.at_height(hr[1]), neck[2] + 0.01)
    G.cut(ob, (0, 0, zc), (0, 0, -1))
    # maniche: piano perpendicolare all'avambraccio
    for side in ('Left', 'Right'):
        fa = side + 'ForeArm'
        if body.has(fa) and fa in g.get('bones', []):
            a = body.head(fa); b = body.tail(fa)
            G.cut(ob, a + (b - a) * sleeves_to, -(b - a))
        elif body.has(side + 'Arm'):
            a = body.head(side + 'Arm'); b = body.tail(side + 'Arm')
            G.cut(ob, a + (b - a) * 0.75, -(b - a))
    G.keep_largest(ob)
    G.smooth_boundary(ob, iters=8)
    # giro largo, senza i muscoli del corpo
    N0 = G.normals(ob)
    P0 = G.verts(ob)
    arm = np.abs(P0[:, 0]) > body.head('LeftArm')[0] * 0.95 if body.has('LeftArm') else np.zeros(len(P0), bool)
    off = g.get('inflate', 0.0065) * H + np.where(arm, ease * 1.4, ease)
    G.offset_normals(ob, off)
    G.drape(ob, body, off * 0.8, rounds=20, iters=5)
    # cuciture ferme: spalle e colletto
    P = G.verts(ob)
    sh_z = body.head('LeftArm')[2] if body.has('LeftArm') else body.at_height(0.8)
    pin = (P[:, 2] > zc - 0.035) & (np.abs(P[:, 0]) < body.head('LeftArm')[0] * 1.05)
    if frames: simulate(ob, body, pin, frames=frames, shrink=shrink, mass=0.25, bending=bending)
    G.clearance(ob, body, 0.003)
    finish_cloth(ob)
    return ob


# ------------------------------------------------------------------------------------------------- pantaloni
def pants(body, g, name, bottom_frac=None, ease=0.010, blouse=0.05, frames=28):
    """Una gamba (side del JSON). bottom_frac = dove finisce (dentro lo stivale)"""
    H = body.H
    side = g.get('side', 1)
    ob = prepare(body, g, name)
    hr = g.get('height_range', [0.07, 0.6])
    top = body.at_height(hr[1])
    bot = body.at_height(bottom_frac if bottom_frac is not None else hr[0])
    G.cut(ob, (0, 0, top), (0, 0, -1))
    G.cut(ob, (0, 0, bot), (0, 0, 1))
    G.cut(ob, (0, 0, 0), (side, 0, 0))          # cucitura al centro
    G.keep_largest(ob)
    G.smooth_boundary(ob, iters=6)
    P0 = G.verts(ob)
    # piu' largo alla coscia, stretto in vita e in fondo (sbuffo sopra lo stivale)
    hf = (P0[:, 2] - bot) / max(top - bot, 1e-6)
    ease_v = ease * (0.6 + 0.8 * np.sin(np.clip(hf, 0, 1) * math.pi) ** 0.7) * np.where(hf > 0.9, 0.4, 1.0)
    off = g.get('inflate', 0.0045) * H + ease_v
    G.offset_normals(ob, off)
    seam = np.abs(P0[:, 0]) < 0.004
    G.drape(ob, body, off * 0.8, rounds=16, iters=4, pin=seam)
    P = G.verts(ob)
    pin = (P[:, 2] > top - 0.03) | (np.abs(P[:, 0]) < 0.004) | (P[:, 2] < bot + 0.012)
    # tessuto in piu' in fondo alla gamba: si raggrinza sopra lo stivale
    hf = (P[:, 2] - bot) / max(top - bot, 1e-6)
    sg = np.clip(1.0 - hf / 0.35, 0, 1) + 0.35 * np.exp(-((hf - 0.55) / 0.08) ** 2)   # e un po' al ginocchio
    simulate(ob, body, pin, frames=frames, shrink=-blouse, shrink_group=np.clip(sg, 0, 1), mass=0.3, bending=0.5)
    G.clearance(ob, body, 0.003)
    finish_cloth(ob)
    return ob


def pants_bottom(body, g):
    """i pantaloni finiscono un po' sopra l'orlo del gioco: dentro lo stivale, sbuffati sopra"""
    return g.get('height_range', [0.07, 0.6])[0] + 0.06


# ------------------------------------------------------------------------------------------------- stracci
def _noise3(P, scale, seed=0):
    """rumore liscio (somma di onde) per buchi e orli sfilacciati"""
    rng = np.random.default_rng(seed)
    v = np.zeros(len(P))
    for k in range(6):
        d = rng.normal(size=3); d /= np.linalg.norm(d)
        f = scale * (1.0 + k * 0.7)
        v += np.sin(P @ d * f + rng.uniform(0, 6.28)) / (1 + k * 0.6)
    return v / 2.5


def rags(body, g, name, frames=26):
    """stracci: tela strappata con buchi, orli sfilacciati, lembi che pendono"""
    H = body.H
    ob = prepare(body, g, name)
    hr = g.get('height_range', [0.4, 0.7])
    G.cut(ob, (0, 0, body.at_height(hr[0])), (0, 0, 1))
    G.cut(ob, (0, 0, body.at_height(hr[1])), (0, 0, -1))
    if g.get('side'):
        G.cut(ob, (0, 0, 0), (g['side'], 0, 0))
    G.keep_largest(ob)
    seed = int(abs(hash(name)) % 1000)
    # buchi e orlo a denti: via le facce dove il rumore e' alto (piu' facile vicino all'orlo in basso)
    P = G.verts(ob)
    z0, z1 = body.at_height(hr[0]), body.at_height(hr[1])
    def keep(c):
        hf = (c[2] - z0) / max(z1 - z0, 1e-6)
        n = _noise3(c[None, :] / H, 7.0, seed)[0]
        return n < 0.75 - 1.2 * max(0.0, 0.2 - hf)   # buchi radi, orlo in basso a denti
    G.cut_fn(ob, keep)
    G.keep_largest(ob, 0.05)
    off = g.get('inflate', 0.01) * H + 0.006
    G.offset_normals(ob, off)
    G.drape(ob, body, off * 0.8, rounds=10, iters=4)
    P = G.verts(ob)
    pin = P[:, 2] > z1 - (z1 - z0) * 0.3          # la parte alta resta appoggiata (niente stracci che scivolano)
    if g.get('side'): pin |= np.abs(P[:, 0]) < 0.01
    simulate(ob, body, pin, frames=min(frames, 14), shrink=-0.03, mass=0.35, bending=0.3, gravity=0.5)
    G.clearance(ob, body, 0.003)
    finish_cloth(ob, thick=0.002, target_faces=4000)
    return ob
