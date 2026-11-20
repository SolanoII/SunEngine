# Attrezzi di geometria per la forgia: mesh da array, tagli con piani, lisciature, spessore, distanza dal bordo,
# occlusione ambientale nei vertici, pesi delle ossa ed esportazione nel formato del gioco.
import bpy, bmesh, math
import numpy as np
from mathutils import Vector
from scipy.sparse import coo_matrix, csr_matrix
from scipy.sparse.csgraph import dijkstra
import gf_io
from gf_body import b2g


# ------------------------------------------------------------------------------------------------- base
def link(obj):
    bpy.context.scene.collection.objects.link(obj)
    return obj


def obj_from(name, P, F):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(p) for p in P], [], [tuple(int(i) for i in f) for f in F])
    me.validate(); me.update()
    ob = link(bpy.data.objects.new(name, me))
    for p in me.polygons: p.use_smooth = True
    return ob


def verts(obj):
    me = obj.data
    a = np.zeros(len(me.vertices) * 3); me.vertices.foreach_get('co', a)
    return a.reshape(-1, 3)


def set_verts(obj, P):
    obj.data.vertices.foreach_set('co', np.asarray(P, np.float64).ravel())
    obj.data.update()


def normals(obj):
    me = obj.data
    a = np.zeros(len(me.vertices) * 3); me.vertices.foreach_get('normal', a)
    return a.reshape(-1, 3)


def edges(obj):
    me = obj.data
    a = np.zeros(len(me.edges) * 2, np.int64); me.edges.foreach_get('vertices', a)
    return a.reshape(-1, 2)


def faces(obj):
    return [list(p.vertices) for p in obj.data.polygons]


def adjacency(obj, weighted=False):
    E = edges(obj); n = len(obj.data.vertices)
    if weighted:
        P = verts(obj); w = np.linalg.norm(P[E[:, 0]] - P[E[:, 1]], axis=1) + 1e-9
    else:
        w = np.ones(len(E))
    A = coo_matrix((np.r_[w, w], (np.r_[E[:, 0], E[:, 1]], np.r_[E[:, 1], E[:, 0]])), shape=(n, n)).tocsr()
    return A


def boundary_verts(obj):
    bm = bmesh.new(); bm.from_mesh(obj.data)
    b = np.zeros(len(bm.verts), bool)
    for e in bm.edges:
        if e.is_boundary:
            b[e.verts[0].index] = b[e.verts[1].index] = True
    bm.free()
    return b


def apply_mods(obj):
    bpy.context.view_layer.objects.active = obj
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


def join(objs, name):
    objs = [o for o in objs if o is not None and len(o.data.vertices)]
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    objs[0].name = name
    return objs[0]


def delete(obj):
    if obj is not None:
        bpy.data.objects.remove(obj, do_unlink=True)


def copy(obj, name):
    o = obj.copy(); o.data = obj.data.copy(); o.name = name
    return link(o)


# ------------------------------------------------------------------------------------------------- dal corpo
def region_obj(body, mask, name, dilate=0):
    """Facce del corpo con tutti i vertici nella zona (zona allargata di 'dilate' anelli)"""
    m = mask.copy()
    if dilate:
        T = body.T
        for _ in range(dilate):
            hit = m[T].any(1)
            m2 = m.copy(); m2[T[hit].ravel()] = True; m = m2
    T = body.T[m[body.T].all(1)]
    used = np.unique(T)
    remap = -np.ones(len(body.P), np.int64); remap[used] = np.arange(len(used))
    ob = obj_from(name, body.P[used], remap[T])
    ob['src'] = used.tolist() if len(used) < 200000 else []
    return ob


def keep_largest(obj, min_frac=0.02):
    """toglie le isole minuscole (pezzetti di zona staccati)"""
    bm = bmesh.new(); bm.from_mesh(obj.data); bm.verts.ensure_lookup_table()
    seen = set(); islands = []
    for v in bm.verts:
        if v.index in seen: continue
        stack = [v]; isl = []
        seen.add(v.index)
        while stack:
            x = stack.pop(); isl.append(x)
            for e in x.link_edges:
                o = e.other_vert(x)
                if o.index not in seen: seen.add(o.index); stack.append(o)
        islands.append(isl)
    total = len(bm.verts)
    kill = [v for isl in islands if len(isl) < min_frac * total for v in isl]
    if kill: bmesh.ops.delete(bm, geom=kill, context='VERTS')
    bm.to_mesh(obj.data); bm.free(); obj.data.update()


# ------------------------------------------------------------------------------------------------- tagli
def cut(obj, co, no, fill=False):
    """Taglia con un piano e tiene il lato verso cui punta 'no' (orlo dritto e pulito)"""
    bm = bmesh.new(); bm.from_mesh(obj.data)
    geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
    bmesh.ops.bisect_plane(bm, geom=geom, plane_co=Vector(co), plane_no=Vector(no), clear_inner=True, dist=1e-6)
    if fill:
        pass
    bm.to_mesh(obj.data); bm.free(); obj.data.update()


def cut_fn(obj, keep_fn):
    """Toglie le facce il cui centro non soddisfa keep_fn(centro) (taglio grossolano, poi si liscia il bordo)"""
    bm = bmesh.new(); bm.from_mesh(obj.data)
    kill = [f for f in bm.faces if not keep_fn(np.array(f.calc_center_median()))]
    bmesh.ops.delete(bm, geom=kill, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    bm.to_mesh(obj.data); bm.free(); obj.data.update()


def subdivide(obj, cuts=1):
    bm = bmesh.new(); bm.from_mesh(obj.data)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=cuts, use_grid_fill=True, smooth=0.0)
    bmesh.ops.triangulate(bm, faces=bm.faces[:]) if False else None
    bm.to_mesh(obj.data); bm.free(); obj.data.update()


def remesh_quads(obj, size):
    """rifa la mesh a quadrati regolari (buona per stoffa e piastre lisce)"""
    m = obj.modifiers.new('rm', 'REMESH'); m.mode = 'VOXEL'; m.voxel_size = size
    apply_mods(obj)


def decimate(obj, ratio):
    m = obj.modifiers.new('dec', 'DECIMATE'); m.ratio = ratio
    apply_mods(obj)


# ------------------------------------------------------------------------------------------------- lisciature
def smooth(obj, iters=5, lam=0.5, pin=None, boundary=True):
    """Lisciatura laplaciana. pin = vertici fermi; boundary=False tiene fermo il bordo"""
    P = verts(obj); A = adjacency(obj)
    deg = np.asarray(A.sum(1)).ravel(); deg[deg == 0] = 1
    fixed = np.zeros(len(P), bool)
    if pin is not None: fixed |= pin
    if not boundary: fixed |= boundary_verts(obj)
    for _ in range(iters):
        avg = (A @ P) / deg[:, None]
        P = np.where(fixed[:, None], P, P + lam * (avg - P))
    set_verts(obj, P)


def taubin(obj, iters=10, lam=0.5, mu=-0.53, pin=None):
    """lisciatura che non restringe (toglie i dettagli del corpo, tiene il volume)"""
    P = verts(obj); A = adjacency(obj)
    deg = np.asarray(A.sum(1)).ravel(); deg[deg == 0] = 1
    fixed = pin if pin is not None else np.zeros(len(P), bool)
    for _ in range(iters):
        for f in (lam, mu):
            avg = (A @ P) / deg[:, None]
            P = np.where(fixed[:, None], P, P + f * (avg - P))
    set_verts(obj, P)


def smooth_boundary(obj, iters=20, lam=0.5, plane=None):
    """Raddrizza il bordo: ogni vertice del bordo verso la media dei due vicini sul bordo (niente scalini)"""
    bm = bmesh.new(); bm.from_mesh(obj.data); bm.verts.ensure_lookup_table()
    nb = {}
    for e in bm.edges:
        if e.is_boundary:
            a, b = e.verts[0].index, e.verts[1].index
            nb.setdefault(a, []).append(b); nb.setdefault(b, []).append(a)
    bm.free()
    if not nb: return
    P = verts(obj)
    ids = np.array([k for k, v in nb.items() if len(v) == 2])
    if not len(ids): return
    n1 = np.array([nb[k][0] for k in ids]); n2 = np.array([nb[k][1] for k in ids])
    for _ in range(iters):
        P[ids] += lam * ((P[n1] + P[n2]) * 0.5 - P[ids])
    set_verts(obj, P)


# ------------------------------------------------------------------------------------------------- spessore e bordo
def offset_normals(obj, d):
    """sposta lungo le normali (d scalare o per vertice)"""
    P = verts(obj); N = normals(obj)
    d = np.asarray(d, float)
    set_verts(obj, P + N * (d[:, None] if d.ndim else d))


def clearance(obj, body, min_d, iters=3):
    """nessun vertice piu' vicino di min_d alla pelle (o dentro il corpo)"""
    for _ in range(iters):
        P = verts(obj)
        moved = False
        for i, q in enumerate(P):
            loc, nrm, _, dist = body.bvh.find_nearest(Vector(q))
            if loc is None: continue
            s = (Vector(q) - loc).dot(nrm)
            if s < min_d:
                P[i] = P[i] + np.array(nrm) * (min_d - s)
                moved = True
        set_verts(obj, P)
        if not moved: break


def min_edge(obj):
    P = verts(obj); E = edges(obj)
    return float(np.linalg.norm(P[E[:, 0]] - P[E[:, 1]], axis=1).min()) if len(E) else 0.0


def drape(obj, body, gap, rounds=12, iters=3, lam=0.6, pin=None):
    """Stoffa tesa: lisciatura (che restringe) e poi fuori dalla pelle di 'gap'. La stoffa scavalca gli incavi
    (addominali, solco della schiena, sterno) invece di seguirli come una tuta"""
    A = adjacency(obj); deg = np.asarray(A.sum(1)).ravel(); deg[deg == 0] = 1
    fixed = (pin.copy() if pin is not None else np.zeros(len(obj.data.vertices), bool)) | boundary_verts(obj)
    gap = np.broadcast_to(np.asarray(gap, float), (len(fixed),))
    for _ in range(rounds):
        P = verts(obj)
        for _ in range(iters):
            P = np.where(fixed[:, None], P, P + lam * ((A @ P) / deg[:, None] - P))
        for i, q in enumerate(P):
            loc, nrm, _, dist = body.bvh.find_nearest(Vector(q))
            if loc is None: continue
            s = (Vector(q) - loc).dot(nrm)
            if s < gap[i]:
                P[i] = P[i] + np.array(nrm) * (gap[i] - s)   # spinta lungo la normale: niente vertici schiacciati
        set_verts(obj, P)


def edge_distance(obj, max_d=0.08):
    """distanza (geodetica sui lati) di ogni vertice dal bordo aperto"""
    b = boundary_verts(obj)
    n = len(b)
    if not b.any(): return np.full(n, max_d)
    A = adjacency(obj, weighted=True)
    d = dijkstra(A, directed=False, indices=np.where(b)[0], min_only=True, limit=max_d)
    d[~np.isfinite(d)] = max_d
    return np.minimum(d, max_d)


def store(obj, name, values):
    me = obj.data
    if name in me.attributes: me.attributes.remove(me.attributes[name])
    a = me.attributes.new(name, 'FLOAT', 'POINT')
    a.data.foreach_set('value', np.asarray(values, np.float32))


def fetch(obj, name, default=0.0):
    me = obj.data
    if name not in me.attributes: return np.full(len(me.vertices), default, np.float32)
    a = np.zeros(len(me.vertices), np.float32); me.attributes[name].data.foreach_get('value', a)
    return a


def solidify(obj, thick, offset=-1.0, rim=True, even=False):
    """spessore vero (gli attributi dei vertici si copiano sul rovescio e sulla fascia del bordo)"""
    m = obj.modifiers.new('sol', 'SOLIDIFY'); m.thickness = thick; m.offset = offset
    m.use_rim = rim; m.use_even_offset = even; m.use_quality_normals = False
    apply_mods(obj)


def bevel_sharp(obj, width, segs=2, angle=50):
    m = obj.modifiers.new('bev', 'BEVEL'); m.width = width; m.segments = segs
    m.limit_method = 'ANGLE'; m.angle_limit = math.radians(angle); m.harden_normals = False
    apply_mods(obj)


def subsurf(obj, levels=1):
    m = obj.modifiers.new('sub', 'SUBSURF'); m.levels = levels; m.render_levels = levels
    apply_mods(obj)


def triangulate(obj):
    bm = bmesh.new(); bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(obj.data); bm.free(); obj.data.update()


# ------------------------------------------------------------------------------------------------- occlusione
def bake_ao(objs, body, samples=24, distance=None):
    """occlusione ambientale nei vertici di ogni pezzo (con il corpo e gli altri pezzi in scena)"""
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = samples
    if sc.world is None:
        sc.world = bpy.data.worlds.new('gf_world')
    sc.world.light_settings.distance = distance if distance is not None else 0.12 * body.H / 1.75   # solo le ombre vicine
    res = {}
    for o in objs:
        me = o.data
        for a in [a.name for a in me.color_attributes]: me.color_attributes.remove(me.color_attributes[a])
        ca = me.color_attributes.new('ao', 'FLOAT_COLOR', 'POINT')
        me.color_attributes.active_color = ca
        mat = bpy.data.materials.new('aomat'); mat.use_nodes = True
        me.materials.clear(); me.materials.append(mat)
        bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active = o
        bpy.ops.object.bake(type='AO', target='VERTEX_COLORS', use_clear=True)
        c = np.zeros(len(me.vertices) * 4, np.float32); ca.data.foreach_get('color', c)
        res[o.name] = c.reshape(-1, 4)[:, 0].copy()
    return res


# ------------------------------------------------------------------------------------------------- pesi
def skin_weights(obj, body, rigid=None, smooth_iters=4, k=8, max_inf=4):
    """Pesi presi dal corpo sotto ogni vertice (lisciati sul pezzo), oppure tutto su un osso (pezzi rigidi).
    rigid: nome di un osso, oppure funzione(posizioni) -> nome dell'osso per vertice"""
    P = verts(obj); n = len(P); nb = len(body.names)
    if rigid == 'dominant':
        # ogni pezzo staccato (lama, piastra, cinghia) tutto sull'osso che lo regge di piu'
        from scipy.sparse.csgraph import connected_components
        Wb = body.weights_at(P, k)
        ncomp, lab = connected_components(adjacency(obj), directed=False)
        W = np.zeros((n, nb), np.float32)
        for c in range(ncomp):
            sel = lab == c
            W[sel, int(np.argmax(Wb[sel].sum(0)))] = 1.0
    elif rigid is not None:
        W = np.zeros((n, nb), np.float32)
        if callable(rigid):
            bones = rigid(P)
            for i, b in enumerate(bones): W[i, body.bone_ix[b]] = 1.0
        else:
            W[:, body.bone_ix[rigid]] = 1.0
    else:
        W = body.weights_at(P, k).astype(np.float32)
        if smooth_iters:
            A = adjacency(obj); deg = np.asarray(A.sum(1)).ravel(); deg[deg == 0] = 1
            for _ in range(smooth_iters):
                W = 0.5 * W + 0.5 * (A @ W) / deg[:, None]
    # le 4 ossa piu' forti, normalizzate
    top = np.argsort(-W, axis=1)[:, :max_inf]
    tw = np.take_along_axis(W, top, 1)
    tw[tw < 0.01] = 0
    s = tw.sum(1, keepdims=True); s[s == 0] = 1
    tw = tw / s
    top = np.where(tw > 0, top, -1)
    return top.astype(np.int16), tw.astype(np.float32)


# ------------------------------------------------------------------------------------------------- esportazione
def export(obj, body, path, edge=None, ao=None, rigid=None, smooth_iters=4):
    """Scrive il pezzo nel formato del gioco (unita' e assi del corpo esportato)"""
    triangulate(obj)
    me = obj.data
    P = verts(obj)
    n = len(P)
    # normali lisce per vertice (le normali per angolo le rifa' il gioco? no: si usano queste)
    N = normals(obj)
    bones, w = skin_weights(obj, body, rigid=rigid, smooth_iters=smooth_iters)
    uv = np.zeros((n, 2), np.float32)
    uv[:, 0] = (edge if edge is not None else fetch(obj, 'edge', 0.08)) / body.H   # frazione dell'altezza, come nel gioco
    uv[:, 1] = ao if ao is not None else 1.0
    tris = np.array([list(p.vertices) for p in me.polygons], np.int64)
    pos_g = b2g(P) / body.scale
    nrm_g = b2g(N)
    gf_io.write_gear(path, body.names, pos_g, nrm_g, uv, bones, w, tris)
    return len(tris)
