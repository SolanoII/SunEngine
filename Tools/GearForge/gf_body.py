# Corpo del gioco dentro Blender: mesh saldata, ossa (origini e direzioni), pesi, BVH per raggi e punti vicini,
# selezione delle zone con le stesse regole dei gusci del gioco (GarmentShell.h).
import bpy, bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from scipy.spatial import cKDTree
import gf_io

# Catene dello scheletro mixamo: osso -> osso figlio (per la direzione)
CHILD = {
    'Hips': 'Spine', 'Spine': 'Spine1', 'Spine1': 'Spine2', 'Spine2': 'Neck', 'Neck': 'Head', 'Head': 'HeadTop_End',
    'LeftShoulder': 'LeftArm', 'LeftArm': 'LeftForeArm', 'LeftForeArm': 'LeftHand', 'LeftHand': 'LeftHandMiddle1',
    'RightShoulder': 'RightArm', 'RightArm': 'RightForeArm', 'RightForeArm': 'RightHand', 'RightHand': 'RightHandMiddle1',
    'LeftUpLeg': 'LeftLeg', 'LeftLeg': 'LeftFoot', 'LeftFoot': 'LeftToeBase', 'LeftToeBase': 'LeftToe_End',
    'RightUpLeg': 'RightLeg', 'RightLeg': 'RightFoot', 'RightFoot': 'RightToeBase', 'RightToeBase': 'RightToe_End',
}


def suffix(n):
    return n.rsplit(':', 1)[-1]


def g2b(p):
    """Assi del gioco (Y in alto, Z davanti) -> Blender (Z in alto, -Y davanti)"""
    p = np.asarray(p, dtype=np.float64)
    return np.stack([p[..., 0], -p[..., 2], p[..., 1]], -1)


def b2g(p):
    p = np.asarray(p, dtype=np.float64)
    return np.stack([p[..., 0], p[..., 2], -p[..., 1]], -1)


class Body:
    def __init__(self, path, name, remesh=0.006):
        d = gf_io.read_body(path)
        self.name = name
        self.names = d['names']
        self.bone_ix = {suffix(n): i for i, n in enumerate(self.names) if n}
        v = d['verts']
        raw = v['pos'].astype(np.float64)
        main = ~d['aux']
        self.raw_min_y = raw[main, 1].min(); self.raw_max_y = raw[main, 1].max()
        raw_h = self.raw_max_y - self.raw_min_y
        self.scale = 0.01 if raw_h > 20 else 1.0          # Blender lavora in metri
        self.H = raw_h * self.scale                        # altezza come la misura il gioco (senza le parti aggiunte)
        self.z0 = self.raw_min_y * self.scale
        # saldatura per posizione (i file del gioco hanno i vertici doppi sulle cuciture)
        key = np.round(raw / (raw_h * 1e-5)).astype(np.int64)
        _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.ravel()
        self.P = g2b(raw[first]) * self.scale             # posizioni saldate (Blender, metri)
        n = len(first)
        nb = len(self.names)
        W = np.zeros((n, nb), np.float32)                  # pesi per osso (media dei doppioni)
        cnt = np.zeros(n)
        for k in range(4):
            b = v['bone'][:, k].astype(np.int64); w = v['w'][:, k]
            ok = (b >= 0) & (b < nb) & (w > 0)
            np.add.at(W, (inv[ok], b[ok]), w[ok])
        np.add.at(cnt, inv, 1)
        W /= np.maximum(cnt, 1)[:, None]
        W /= np.maximum(W.sum(1, keepdims=True), 1e-6)
        self.W = W
        self.aux = np.zeros(n, bool); self.aux[inv[d['aux']]] = True
        tris = inv[d['idx']]
        tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
        self.T = tris
        # ossa: origine in posa di riposo
        self.bone_pos = {}
        for s, i in self.bone_ix.items():
            m = np.linalg.inv(d['offsets'][i])
            self.bone_pos[s] = g2b(m[:3, 3]) * self.scale
        self._make_object(remesh)

    # --------------------------------------------------------------------------------------------- Blender
    def _make_object(self, remesh):
        me = bpy.data.meshes.new('body_' + self.name)
        me.from_pydata(self.P.tolist(), [], self.T.tolist())
        me.update()
        ob = bpy.data.objects.new('body_' + self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        if remesh:
            # Il corpo del gioco e' fatto di pezzi chiusi accostati (braccia, mani, collo...): per le vesti serve una
            # pelle sola, continua. Rifusione a voxel; pesi e parti aggiunte passano dal corpo originale.
            # i corpi adattati (BodyFit) hanno qualche buco: si chiudono, altrimenti la rifusione a voxel li svuota
            bm = bmesh.new(); bm.from_mesh(ob.data)
            bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=self.H * 2e-4)
            bnd = [e for e in bm.edges if e.is_boundary]
            if bnd: bmesh.ops.holes_fill(bm, edges=bnd, sides=0)
            bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
            bm.to_mesh(ob.data); bm.free(); ob.data.update()
            m = ob.modifiers.new('rm', 'REMESH'); m.mode = 'VOXEL'; m.voxel_size = remesh * self.H / 1.75
            m.use_smooth_shade = True
            bpy.context.view_layer.objects.active = ob
            bpy.ops.object.modifier_apply(modifier='rm')
            me = ob.data
            P = np.zeros(len(me.vertices) * 3); me.vertices.foreach_get('co', P); P = P.reshape(-1, 3)
            T = []
            for poly in me.polygons:
                v = list(poly.vertices)
                for k in range(1, len(v) - 1): T.append([v[0], v[k], v[k + 1]])
            kd0 = cKDTree(self.P)
            d, ix = kd0.query(P, k=4)
            wk = 1.0 / np.maximum(d, 1e-6) ** 2; wk /= wk.sum(1, keepdims=True)
            self.W = np.einsum('nk,nkb->nb', wk, self.W[ix])
            self.aux = self.aux[ix[:, 0]]
            self.P = P; self.T = np.array(T, np.int64)
        for p in me.polygons: p.use_smooth = True
        self.obj = ob
        me.calc_loop_triangles() if hasattr(me, 'calc_loop_triangles') else None
        self.N = np.array([vv.normal[:] for vv in me.vertices])
        self.bvh = BVHTree.FromPolygons(self.P.tolist(), self.T.tolist())
        self.kd = cKDTree(self.P)
        bm = self.P[~self.aux] if (~self.aux).any() else self.P
        self.bvh_main = BVHTree.FromPolygons(self.P.tolist(), self.T[~self.aux[self.T].any(1)].tolist())

    # --------------------------------------------------------------------------------------------- ossa
    def has(self, bone):
        return bone in self.bone_pos

    def head(self, bone):
        return np.array(self.bone_pos[bone])

    def tail(self, bone):
        c = CHILD.get(bone)
        if c and c in self.bone_pos:
            return np.array(self.bone_pos[c])
        # ultimo osso della catena: prosegue nella direzione del padre
        for p, ch in CHILD.items():
            if ch == bone and p in self.bone_pos:
                return self.head(bone) + (self.head(bone) - self.head(p)) * 0.6
        return self.head(bone) + np.array([0, 0, 0.1])

    def axis(self, bone):
        a = self.tail(bone) - self.head(bone)
        return a / max(np.linalg.norm(a), 1e-9)

    def weight_of(self, bones, idx=None):
        cols = [self.bone_ix[b] for b in bones if b in self.bone_ix]
        W = self.W if idx is None else self.W[idx]
        return W[:, cols].sum(1) if cols else np.zeros(len(W))

    def hfrac(self, P):
        """altezza in frazione del corpo (0 piedi, 1 testa), come nel gioco"""
        return (np.asarray(P)[..., 2] - self.z0) / self.H

    def at_height(self, f):
        return self.z0 + f * self.H

    # --------------------------------------------------------------------------------------------- zone
    def region(self, g):
        """Vertici della zona di uno strato (regole di GarmentBuildShell)"""
        P, N = self.P, self.N
        w = self.weight_of(g.get('bones', []))
        keep = w >= g.get('min_weight', 0.5)
        hy = self.hfrac(P)
        hr = g.get('height_range', [0, 1])
        keep &= (hy >= hr[0]) & (hy <= hr[1])
        side = g.get('side', 0)
        if side:
            keep &= P[:, 0] * side >= -0.01 * self.H
        if g.get('max_abs_x', 0) > 0:
            keep &= np.abs(P[:, 0]) / self.H <= g['max_abs_x']
        if g.get('min_abs_x', 0) > 0:
            keep &= np.abs(P[:, 0]) / self.H >= g['min_abs_x']
        f = g.get('facing')
        if f and np.dot(f, f) > 1e-6:
            fb = g2b(np.array(f, float)); fb /= np.linalg.norm(fb)
            keep &= N @ fb >= g.get('facing_min', -1)
        keep &= ~self.aux
        return keep

    # --------------------------------------------------------------------------------------------- pesi
    def weights_at(self, Q, smooth_k=6):
        """Pesi del corpo nei punti Q (media pesata dei vertici del corpo piu' vicini)"""
        d, ix = self.kd.query(Q, k=smooth_k)
        wk = 1.0 / np.maximum(d, 1e-5) ** 2
        wk /= wk.sum(1, keepdims=True)
        return np.einsum('nk,nkb->nb', wk, self.W[ix])

    def surface_dist(self, Q):
        """distanza (con segno: + fuori) dalla superficie del corpo"""
        out = np.zeros(len(Q))
        for i, q in enumerate(Q):
            loc, nrm, _, dist = self.bvh.find_nearest(Vector(q))
            if loc is None: out[i] = 1.0; continue
            out[i] = dist if (Vector(q) - loc).dot(nrm) >= 0 else -dist
        return out

    def ray_out(self, origin, direction, allowed=None, max_d=2.0):
        """Superficie esterna del corpo lungo un raggio che arriva da fuori verso 'origin' (prima superficie
        incontrata da fuori). allowed = vettore bool dei vertici accettati (es. solo quelli del busto)"""
        o = np.asarray(origin, float); dvec = np.asarray(direction, float)
        dvec = dvec / np.linalg.norm(dvec)
        start = o + dvec * max_d
        p = Vector(start); dirn = Vector(-dvec)
        travelled = 0.0
        while travelled < max_d * 1.05:
            loc, nrm, fi, dist = self.bvh.ray_cast(p, dirn, max_d * 1.1 - travelled)
            if loc is None:
                return None
            if allowed is None or allowed[self.T[fi]].any():
                return np.array(loc)
            p = loc + dirn * 1e-4; travelled += dist + 1e-4
        return None
