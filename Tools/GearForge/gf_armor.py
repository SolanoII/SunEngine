# Armature e cuoio: elmi misurati sul cranio, piastre e lame sugli arti, giubbetti antischegge e corazze, stivali,
# cinturoni. Le forme nascono da griglie regolari proiettate sul corpo (raggi da fuori verso l'osso), cosi' i bordi
# sono puliti e le superfici lisce come lamiera stampata; poi spessore, bordi smussati, rivetti e cinghie.
import bpy, bmesh, math
import numpy as np
from mathutils import Vector
from scipy.ndimage import gaussian_filter, maximum_filter
import gf_geom as G

FRONT = np.array([0.0, -1.0, 0.0])   # davanti del corpo in Blender
UP = np.array([0.0, 0.0, 1.0])


def unit(v):
    v = np.asarray(v, float); n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def frame(axis, ref):
    """terna (asse, riferimento ortogonale, terzo) per le coordinate cilindriche attorno a un osso"""
    a = unit(axis)
    r = np.asarray(ref, float) - a * np.dot(ref, a)
    if np.linalg.norm(r) < 1e-6:
        r = np.array([1.0, 0, 0]) - a * a[0]
    r = unit(r)
    return a, r, np.cross(a, r)


# ------------------------------------------------------------------------------------------------- griglie
def grid_faces(nr, nc, wrap=False):
    F = []
    cols = nc if wrap else nc - 1
    for i in range(nr - 1):
        for j in range(cols):
            j2 = (j + 1) % nc
            F.append((i * nc + j, i * nc + j2, (i + 1) * nc + j2, (i + 1) * nc + j))
    return F


def radii_fill(R):
    """riempie i raggi mancati (nan) con quelli vicini"""
    R = R.copy()
    for _ in range(50):
        bad = np.isnan(R)
        if not bad.any(): break
        Rp = np.pad(R, 1, mode='edge')
        stack = np.stack([Rp[:-2, 1:-1], Rp[2:, 1:-1], Rp[1:-1, :-2], Rp[1:-1, 2:]])
        m = np.nanmean(np.where(np.isnan(stack), np.nan, stack), axis=0)
        R[bad] = m[bad]
    R[np.isnan(R)] = np.nanmean(R) if np.isfinite(np.nanmean(R)) else 0.05
    return R


def envelope(R, sigma, grow=3, wrap=False):
    """inviluppo liscio dei raggi: mai dentro il corpo, niente muscoli o dita"""
    mode = ('nearest', 'wrap') if wrap else 'nearest'
    sig = np.broadcast_to(np.asarray(sigma, float), (2,))
    Rm = maximum_filter(R, size=grow, mode=mode)
    Rs = gaussian_filter(Rm, sig, mode=mode)
    Rs = np.maximum(Rs, gaussian_filter(R, sig * 0.5, mode=mode))
    return gaussian_filter(Rs, sig * 0.5, mode=mode)


def cyl_shell(body, origin_fn, axis, ref, ts, angles, allowed, max_d=0.4):
    """Raggi del corpo in coordinate cilindriche: per ogni t (lungo l'asse) e angolo, distanza dall'asse della pelle"""
    a, r, s = frame(axis, ref)
    R = np.full((len(ts), len(angles)), np.nan)
    for i, t in enumerate(ts):
        o = origin_fn(t)
        for j, al in enumerate(angles):
            d = r * math.cos(al) + s * math.sin(al)
            hit = body.ray_out(o, d, allowed, max_d)
            if hit is not None:
                R[i, j] = np.dot(hit - o, d)
    return R, (a, r, s)


def cyl_points(origin_fn, fr, ts, angles, R):
    a, r, s = fr
    P = np.zeros((len(ts), len(angles), 3))
    for i, t in enumerate(ts):
        o = origin_fn(t)
        for j, al in enumerate(angles):
            d = r * math.cos(al) + s * math.sin(al)
            P[i, j] = o + d * R[i, j]
    return P


def plate_from_grid(P, name, wrap=False):
    nr, nc = P.shape[:2]
    return G.obj_from(name, P.reshape(-1, 3), grid_faces(nr, nc, wrap))


def finish_plate(ob, thick, bevel=0.0018, segs=2, edge=True, sub=0):
    """spessore, bordo smussato, distanza dal bordo per l'usura degli spigoli"""
    if sub: G.subsurf(ob, sub)
    if edge: G.store(ob, 'edge', G.edge_distance(ob, 0.05))
    G.solidify(ob, thick, offset=1.0)
    if bevel > 0: G.bevel_sharp(ob, bevel, segs, angle=40)
    return ob


def rounded_angles(nt, a0, a1, round_frac=0.25, power=4):
    """per ogni fila lungo l'osso, l'apertura angolare (ridotta alle estremita': angoli arrotondati)"""
    u = np.linspace(-1, 1, nt)
    k = (1 - np.abs(u) ** power) ** (1 / power)
    k = (1 - round_frac) + round_frac * k
    mid = 0.5 * (a0 + a1); half = 0.5 * (a1 - a0)
    return mid - half * k, mid + half * k


def rivets(points, normals, size, name):
    """borchie: semisfere schiacciate sulla piastra"""
    objs = []
    for p, n in zip(points, normals):
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=5, radius=size)
        for v in list(bm.verts):
            if v.co.z < -size * 0.15: bm.verts.remove(v)
        for v in bm.verts: v.co.z *= 0.55
        me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
        o = G.link(bpy.data.objects.new(name, me))
        z = unit(n); x = unit(np.cross(z, [0.31, 0.47, 0.83])); y = np.cross(z, x)
        M = np.eye(4); M[:3, 0] = x; M[:3, 1] = y; M[:3, 2] = z; M[:3, 3] = p
        from mathutils import Matrix
        o.data.transform(Matrix(M.tolist())); o.data.update()
        for f in o.data.polygons: f.use_smooth = True
        objs.append(o)
    return objs


def band(origin_fn, fr, t, angles, R, width, thick, name, wrap=True):
    """cinghia: fascia sottile attorno all'arto (cuoio)"""
    ts = [t - width / 2, t + width / 2]
    RR = np.stack([R, R])
    P = cyl_points(origin_fn, fr, ts, angles, RR)
    ob = plate_from_grid(P, name, wrap=wrap)
    finish_plate(ob, thick, bevel=0.0008, segs=1)
    return ob


# ------------------------------------------------------------------------------------------------- ossa e zone
def bone_mask(body, bones, minw=0.35):
    return (body.weight_of(bones) >= minw) & ~body.aux


def limb_info(body, bone):
    a = body.head(bone); b = body.tail(bone)
    return a, b, np.linalg.norm(b - a)


def facing_vec(g):
    f = g.get('facing')
    if not f: return None
    v = np.array([f[0], -f[2], f[1]], float)
    return unit(v) if np.linalg.norm(v) > 1e-6 else None


def angle_span(fr, g, default=(-math.pi, math.pi)):
    """apertura angolare dal 'facing' del JSON (es. solo davanti, solo sopra)"""
    fv = facing_vec(g)
    if fv is None: return default, False
    a, r, s = fr
    c = math.atan2(np.dot(fv, s), np.dot(fv, r))
    fmin = g.get('facing_min', -1)
    half = math.acos(max(-0.95, min(0.95, fmin))) + 0.25
    half = min(half, math.pi * 0.92)
    return (c - half, c + half), True


# ------------------------------------------------------------------------------------------------- arti
def limb_plate(body, g, name, kind, style):
    H = body.H
    side = 'Left' if (g.get('side', 0) >= 0 and ('_l' in name or 'Left' in ''.join(g.get('bones', [])))) else 'Right'
    if '_r' in name.split(name.rstrip('0123456789'))[0][-3:] or name.endswith('_r') or '_r_' in name:
        side = 'Right'
    bones = g.get('bones', [])
    off = g.get('inflate', 0.015) * H
    thick = 0.0035 if style in ('flak',) else 0.0045
    parts = []
    if kind == 'bracer':
        bone = side + 'ForeArm'
        a, b, L = limb_info(body, bone)
        allowed = bone_mask(body, [bone, side + 'Hand'], 0.3)
        o_fn = lambda t: a + (b - a) * t
        fr = frame(b - a, UP)
        ts = np.linspace(0.30, 0.86, 18)
        ang = np.linspace(-math.pi, math.pi, 40, endpoint=False)
        R, fr = cyl_shell(body, o_fn, b - a, UP, ts, ang, allowed, 0.25)
        R = envelope(radii_fill(R), (1.0, 2.0), wrap=True) + off * 0.7
        # apertura sotto (allacciatura): non un tubo chiuso
        ang2 = np.linspace(-math.pi * 0.86, math.pi * 0.86, 38)
        R2 = np.stack([np.interp(ang2, np.r_[ang - 2 * math.pi, ang, ang + 2 * math.pi], np.r_[row, row, row]) for row in R])
        ang2 = ang2 + math.pi   # l'apertura guarda in basso
        P = cyl_points(o_fn, fr, ts, ang2, R2)
        pl = plate_from_grid(P, name + '_p')
        finish_plate(pl, thick)
        parts.append(pl)
        for tt in (0.40, 0.74):
            i = int(np.argmin(np.abs(ts - tt)))
            parts.append(band(o_fn, fr, tt, ang, R[i] + thick + 0.0015, 0.018 * H / 1.75, 0.0025, name + '_cinghia'))
        ob = G.join(parts, name)
        ob['rigid'] = bone
        return ob

    if kind == 'arm_plate' and style in ('flak', 'vrepigh', 'scrap'):
        return shoulder_pad(body, g, name, side, style, off, thick)

    if kind == 'arm_plate':
        bone = side + 'Arm'
        a, b, L = limb_info(body, bone)
        allowed = bone_mask(body, [bone, side + 'Shoulder'], 0.3)
        sh = body.head(side + 'Shoulder') if body.has(side + 'Shoulder') else a
        o_fn = lambda t: a + (b - a) * t
        fr = frame(b - a, UP)
        (a0, a1), faced = angle_span(fr, g, (-math.pi * 0.62, math.pi * 0.62))
        if not faced: a0, a1 = -math.pi * 0.62, math.pi * 0.62
        a0 = max(a0, -math.pi * 0.7); a1 = min(a1, math.pi * 0.7)
        n_lames = 3 if style in ('vrepigh', 'scrap') else 2
        t_start, t_end = -0.06, 0.62
        ang_full = np.linspace(a0, a1, 34)
        tsf = np.linspace(t_start, t_end, 30)
        R, fr = cyl_shell(body, o_fn, b - a, UP, tsf, ang_full, allowed, 0.35)
        R = envelope(radii_fill(R), (2.0, 2.0)) + off
        edges_t = np.linspace(t_start, t_end, n_lames + 1)
        for k in range(n_lames):
            lt0 = edges_t[k] - (0.04 if k else 0.0); lt1 = edges_t[k + 1] + 0.02
            ts = np.linspace(lt0, lt1, 12)
            lo, hi = rounded_angles(len(ts), a0, a1, 0.18)
            Pk = np.zeros((len(ts), 22, 3))
            for i, t in enumerate(ts):
                angs = np.linspace(lo[i], hi[i], 22)
                rr = np.array([np.interp(t, tsf, R[:, int(np.argmin(np.abs(ang_full - al)))]) for al in angs])
                rr = rr + (n_lames - 1 - k) * 0.0045 + 0.004 * np.sin(np.linspace(0, math.pi, 22))   # lame sovrapposte, bombate
                Pk[i] = cyl_points(o_fn, fr, [t], angs, rr[None, :])[0]
            pl = plate_from_grid(Pk, f'{name}_lama{k}')
            finish_plate(pl, thick)
            parts.append(pl)
            # rivetti agli angoli della lama
            pts, nrm = [], []
            for (ii, jj) in ((1, 1), (1, -2), (-2, 1), (-2, -2)):
                p = Pk[ii, jj]; n = p - o_fn(ts[ii]); pts.append(p + unit(n) * thick); nrm.append(n)
            parts += rivets(pts, nrm, 0.0042 if style != 'flak' else 0.0032, name + '_riv')
        # cinghia attorno al braccio sotto le lame
        ang = np.linspace(-math.pi, math.pi, 36, endpoint=False)
        Rb, frb = cyl_shell(body, o_fn, b - a, UP, [0.55], ang, allowed, 0.3)
        Rb = envelope(radii_fill(Rb), (0.5, 2.0), wrap=True)[0] + 0.004
        parts.append(band(o_fn, frb, 0.55, ang, Rb, 0.02 * H / 1.75, 0.003, name + '_cinghia'))
        ob = G.join(parts, name)
        ob['rigid'] = bone
        return ob

    if kind == 'leg_plate':
        shin = side + 'Leg'; thigh = side + 'UpLeg'
        a, b, L = limb_info(body, shin)
        ua, ub, UL = limb_info(body, thigh)
        hr = g.get('height_range', [0.1, 0.42])
        z_lo, z_hi = body.at_height(hr[0]), body.at_height(hr[1])
        o_fn = lambda t: a + (b - a) * t
        fr = frame(b - a, FRONT)
        (a0, a1), faced = angle_span(fr, g, (-1.2, 1.2))
        a0 = max(a0, -1.25); a1 = min(a1, 1.25)
        allowed = bone_mask(body, [shin], 0.3)
        # schiniere: dal ginocchio fin dove dice il JSON
        t_lo = 0.10
        t_hi = min(0.92, max(0.4, (a[2] - z_lo) / max(a[2] - b[2], 1e-6)))
        ts = np.linspace(t_lo, t_hi, 26)
        angf = np.linspace(a0, a1, 30)
        R, fr = cyl_shell(body, o_fn, b - a, FRONT, ts, angf, allowed, 0.3)
        R = envelope(radii_fill(R), (2.0, 1.5)) + off
        lo, hi = rounded_angles(len(ts), a0, a1, 0.2)
        P = np.zeros((len(ts), 24, 3))
        for i, t in enumerate(ts):
            angs = np.linspace(lo[i], hi[i], 24)
            rr = np.interp(angs, angf, R[i]) + 0.003 * np.sin(np.linspace(0, math.pi, 24))
            P[i] = cyl_points(o_fn, fr, [t], angs, rr[None, :])[0]
        greave = plate_from_grid(P, name + '_schiniera')
        finish_plate(greave, thick)
        parts.append(greave)
        # ginocchiera: coppa sul ginocchio
        kc = a + (b - a) * 0.0
        ka = unit(b - a); kfr = frame(ka, FRONT)
        allowed_k = bone_mask(body, [shin, thigh], 0.2)
        tk = np.linspace(-0.11, 0.12, 14)
        angk = np.linspace(-1.1, 1.1, 22)
        knee_fn = lambda t: kc + ka * t * L
        Rk, kfr = cyl_shell(body, knee_fn, ka, FRONT, tk, angk, allowed_k, 0.3)
        Rk = envelope(radii_fill(Rk), (1.5, 1.5)) + off + thick + 0.004
        lo, hi = rounded_angles(len(tk), -1.1, 1.1, 0.5, 2)
        Pk = np.zeros((len(tk), 20, 3))
        for i, t in enumerate(tk):
            angs = np.linspace(lo[i], hi[i], 20)
            dome = 0.010 * np.cos(np.linspace(-1, 1, 20) * 1.2) * math.cos((i / (len(tk) - 1) - 0.5) * 2.2)
            rr = np.interp(angs, angk, Rk[i]) + dome
            Pk[i] = cyl_points(knee_fn, kfr, [t], angs, rr[None, :])[0]
        knee = plate_from_grid(Pk, name + '_ginocchio')
        finish_plate(knee, thick)
        parts.append(knee)
        pts = [Pk[len(tk) // 2, 1], Pk[len(tk) // 2, -2]]
        parts += rivets([p + unit(p - knee_fn(0)) * thick for p in pts], [p - knee_fn(0) for p in pts], 0.0045, name + '_riv')
        # piastra sulla coscia (se la zona sale fin li')
        if thigh in bones and z_hi > a[2] + 0.04:
            fn2 = lambda t: ua + (ub - ua) * t
            t2_hi = 0.93; t2_lo = max(0.35, 1.0 - (z_hi - a[2]) / max(UL, 1e-6))
            ts2 = np.linspace(t2_lo, t2_hi, 16)
            fr2 = frame(ub - ua, FRONT)
            (b0, b1), _ = angle_span(fr2, g, (-1.0, 1.0)); b0 = max(b0, -1.1); b1 = min(b1, 1.1)
            angt = np.linspace(b0, b1, 26)
            R2, fr2 = cyl_shell(body, fn2, ub - ua, FRONT, ts2, angt, bone_mask(body, [thigh], 0.3), 0.35)
            R2 = envelope(radii_fill(R2), (2.0, 1.5)) + off
            lo, hi = rounded_angles(len(ts2), b0, b1, 0.22)
            P2 = np.zeros((len(ts2), 22, 3))
            for i, t in enumerate(ts2):
                angs = np.linspace(lo[i], hi[i], 22)
                P2[i] = cyl_points(fn2, fr2, [t], angs, (np.interp(angs, angt, R2[i]) + 0.003 * np.sin(np.linspace(0, math.pi, 22)))[None, :])[0]
            cuisse = plate_from_grid(P2, name + '_coscia')
            finish_plate(cuisse, thick)
            parts.append(cuisse)
        # cinghie dietro al polpaccio
        ang = np.linspace(-math.pi, math.pi, 36, endpoint=False)
        for tt in (0.28, 0.6):
            Rb, frb = cyl_shell(body, o_fn, b - a, FRONT, [tt], ang, allowed, 0.3)
            Rb = envelope(radii_fill(Rb), (0.5, 2.0), wrap=True)[0] + 0.004
            parts.append(band(o_fn, frb, tt, ang, Rb, 0.018 * H / 1.75, 0.003, name + '_cinghia'))
        ob = G.join(parts, name)
        ob['rigid'] = 'dominant'
        return ob
    return None


def shoulder_pad(body, g, name, side, style, off, thick):
    """spallaccio: coppa sulla spalla (dal trapezio al deltoide) e lame sotto, sul lato esterno del braccio"""
    H = body.H
    sgn = 1.0 if side == 'Left' else -1.0
    arm = side + 'Arm'
    c = body.head(arm).copy()
    out = np.array([sgn, 0.0, 0.0])
    pole = unit(UP * 0.85 + out * 0.5)
    fr_a, fr_r, fr_s = frame(pole, out)          # r: verso fuori (giu' lungo il braccio), s: davanti/dietro
    allowed = bone_mask(body, [arm, side + 'Shoulder', 'Spine2'], 0.25)
    big = style != 'flak'
    def e_main(phi):
        # piu' bassa sul lato esterno, corta verso il collo
        return (0.85 if big else 0.78) * (0.75 + 0.25 * math.cos(phi))
    def ray(e, phi):
        d = unit(fr_a * math.cos(e) + (fr_r * math.cos(phi) + fr_s * math.sin(phi)) * math.sin(e))
        hit = body.ray_out(c, d, allowed, 0.35)
        return d, (np.dot(hit - c, d) if hit is not None else np.nan)
    n_e, n_p = 14, 40
    phis = np.linspace(-math.pi, math.pi, n_p, endpoint=False)
    D = np.zeros((n_e, n_p, 3)); R = np.full((n_e, n_p), np.nan)
    for j, ph in enumerate(phis):
        em = e_main(ph)
        for i in range(n_e):
            D[i, j], R[i, j] = ray(em * i / (n_e - 1), ph)
    R = envelope(radii_fill(R), (1.2, 2.0), grow=3) + off + (0.008 if big else 0.004)
    R = R * (1.0 + 0.04 * (1 - np.linspace(0, 1, n_e))[:, None])   # coppa bombata
    P = c + D * R[..., None]
    cap = G.obj_from(name + '_coppa', P.reshape(-1, 3), grid_faces(n_e, n_p, wrap=True))
    bm = bmesh.new(); bm.from_mesh(cap.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
    bm.to_mesh(cap.data); bm.free(); cap.data.update()
    G.smooth(cap, iters=2, lam=0.4, boundary=False)
    finish_plate(cap, thick, sub=1 if not big else 0)
    parts = [cap]
    # lame sul braccio, sotto la coppa: fasce che girano sopra, davanti e dietro
    a, b, L = limb_info(body, arm)
    o_fn = lambda t: a + (b - a) * t
    n_l = 3 if big else 2
    tsf = np.linspace(0.0, 0.42, 24)
    angf = np.linspace(-1.75, 1.75, 30)
    Rl, frl = cyl_shell(body, o_fn, b - a, UP, tsf, angf, bone_mask(body, [arm], 0.3), 0.3)
    Rl = envelope(radii_fill(Rl), (1.5, 1.5)) + off
    edges_t = np.linspace(0.06, 0.40, n_l + 1)
    for k in range(n_l):
        ts = np.linspace(edges_t[k] - 0.03, edges_t[k + 1] + 0.015, 9)
        lo, hi = rounded_angles(len(ts), -1.7, 1.7, 0.25)
        Pk = np.zeros((len(ts), 24, 3))
        for i, t in enumerate(ts):
            angs = np.linspace(lo[i], hi[i], 24)
            rr = np.array([np.interp(t, tsf, Rl[:, int(np.argmin(np.abs(angf - al)))]) for al in angs])
            rr = rr + (n_l - k) * 0.004 + 0.003 * np.sin(np.linspace(0, math.pi, 24))
            Pk[i] = cyl_points(o_fn, frl, [t], angs, rr[None, :])[0]
        lam_ = plate_from_grid(Pk, f'{name}_lama{k}')
        finish_plate(lam_, thick)
        parts.append(lam_)
        if big:
            pts = [Pk[4, 2], Pk[4, -3], Pk[4, 12]]
            parts += rivets([p + unit(p - o_fn(ts[4])) * thick for p in pts], [p - o_fn(ts[4]) for p in pts], 0.005, name + '_riv')
    # rivetti sulla coppa
    idx = np.linspace(0, n_p, 7 if big else 5, endpoint=False).astype(int)
    pts = [P[-2, j] for j in idx]
    parts += rivets([p + unit(p - c) * (thick + 0.0005) for p in pts], [p - c for p in pts], 0.0048 if big else 0.0034, name + '_riv')
    # cinghia attorno al braccio
    ang = np.linspace(-math.pi, math.pi, 36, endpoint=False)
    Rb, frb = cyl_shell(body, o_fn, b - a, UP, [0.46], ang, bone_mask(body, [arm], 0.3), 0.3)
    Rb = envelope(radii_fill(Rb), (0.5, 2.0), wrap=True)[0] + 0.004
    parts.append(band(o_fn, frb, 0.46, ang, Rb, 0.02 * H / 1.75, 0.003, name + '_cinghia'))
    ob = G.join(parts, name)
    ob['rigid'] = arm
    return ob


# ------------------------------------------------------------------------------------------------- elmi
def head_frame(body):
    """centro, cima e altezza degli occhi del cranio"""
    hm = bone_mask(body, ['Head', 'HeadTop_End'], 0.5)
    P = body.P[hm]
    top = P[:, 2].max()
    org = body.head('Head')
    # sopracciglia: circa a meta' tra l'attacco della testa e la cima (un po' sopra)
    brow = org[2] + (top - org[2]) * 0.52
    crown = P[P[:, 2] > brow]
    c = np.array([0.0, crown[:, 1].mean(), brow])
    return c, top, brow, hm


def helmet(body, g, name, style):
    H = body.H
    c, top, brow, hm = head_frame(body)
    allowed = bone_mask(body, ['Head', 'HeadTop_End', 'Neck'], 0.25) | body.aux
    pad = max(g.get('inflate', 0.014) * H * 0.55, 0.010)
    thick = 0.0032
    # griglia sferica: elevazione dal polo fin sotto la fronte, azimut tutto attorno
    n_el, n_az = 26, 56
    el = np.linspace(0.0, 1.0, n_el)      # 0 = cima, 1 = orlo
    az = np.linspace(-math.pi, math.pi, n_az, endpoint=False)   # 0 = davanti
    head_h = top - brow
    def rim_drop(azv):
        """quanto scende l'orlo sotto la fronte: poco davanti, sulle orecchie, molto dietro (paranuca)"""
        f = 0.5 - 0.5 * np.cos(azv)          # 0 davanti, 1 dietro
        deep = {'flak': -0.36, 'vrepigh': -0.12, 'scrap': -0.2}.get(style, -0.3)
        back = {'flak': 0.12, 'vrepigh': 0.45, 'scrap': 0.3}.get(style, 0.2)
        return head_h * (deep + (back - deep) * f ** 1.5)
    R = np.full((n_el, n_az), np.nan)
    D = np.zeros((n_el, n_az, 3))
    for j, a in enumerate(az):
        drop = rim_drop(a)
        horiz = np.array([math.sin(a), -math.cos(a), 0.0])   # az 0 = davanti (-Y)
        theta_rim = math.pi / 2 + math.atan2(drop, max(0.06, head_h * 0.9))
        for i, e in enumerate(el):
            th = theta_rim * e
            d = unit(UP * math.cos(th) + horiz * math.sin(th))
            D[i, j] = d
            hit = body.ray_out(c, d, allowed, 0.5)
            if hit is not None: R[i, j] = np.dot(hit - c, d)
    R = radii_fill(R)
    R = envelope(R, (1.6, 2.5), grow=5, wrap=False) + pad
    # calotta un po' piu' alta e tonda (non il cranio pari pari)
    R *= 1.0 + 0.05 * (1 - el[:, None]) ** 2
    P = c + D * R[..., None]
    # polo: un vertice solo
    Pm = P.reshape(-1, 3)
    shell = G.obj_from(name + '_calotta', Pm, grid_faces(n_el, n_az, wrap=True))
    bm = bmesh.new(); bm.from_mesh(shell.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-4)
    bm.to_mesh(shell.data); bm.free(); shell.data.update()
    G.smooth(shell, iters=3, lam=0.4, boundary=False)
    parts = []
    finish_plate(shell, thick, bevel=0.0015)
    parts.append(shell)
    rim = P[-1]
    rim_n = np.array([unit(np.r_[(p - c)[:2], 0.0]) for p in rim])
    # orlo arrotolato (cordone) e tesa
    if style == 'flak':
        brim_w = 0.030 * H / 1.75
        rows = []
        for k, (w, dz) in enumerate(((0.0, 0.0), (brim_w * 0.5, -0.006), (brim_w, -0.016))):
            rows.append(rim + rim_n * w + np.array([0, 0, dz]))
        bp = np.stack(rows)
        brim = plate_from_grid(bp, name + '_tesa', wrap=True)
        G.subsurf(brim, 1)
        finish_plate(brim, thick * 0.9, bevel=0.001)
        parts.append(brim)
    # cordone sull'orlo
    ring = np.stack([rim + rim_n * 0.002 + np.array([0, 0, 0.004]), rim + rim_n * 0.005, rim + rim_n * 0.002 - np.array([0, 0, 0.004])])
    bead = plate_from_grid(ring, name + '_cordone', wrap=True)
    finish_plate(bead, 0.002, bevel=0.0)
    parts.append(bead)
    # rivetti attorno all'orlo
    k_riv = {'flak': 10, 'vrepigh': 14, 'scrap': 9}.get(style, 10)
    idx = np.linspace(0, n_az, k_riv, endpoint=False).astype(int)
    rp = [P[-3, j] + unit(P[-3, j] - c) * (thick + 0.0005) for j in idx]
    parts += rivets(rp, [P[-3, j] - c for j in idx], 0.0035 if style == 'flak' else 0.0048, name + '_riv')
    # rinforzo a croce sulla calotta (vrepigh, rottami): due fasce
    if style in ('vrepigh', 'scrap'):
        for jj in (0, n_az // 4):
            for j0 in (jj, jj + n_az // 2):
                strip = []
                for i in range(1, n_el - 1):
                    p = P[i, j0 % n_az]; n = unit(p - c)
                    t = unit(np.cross(n, UP)) if abs(np.dot(n, UP)) < 0.98 else np.array([1.0, 0, 0])
                    strip.append([p + n * (thick + 0.001) - t * 0.012, p + n * (thick + 0.001) + t * 0.012])
                S = np.array(strip).transpose(1, 0, 2).transpose(1, 0, 2)
                sb = plate_from_grid(np.ascontiguousarray(S), name + '_fascia')
                finish_plate(sb, 0.0025, bevel=0.0008)
                parts.append(sb)
    # sottogola (umani)
    if style == 'flak' and body.name in ('footman', 'candidate'):
        for sgn in (-1, 1):
            j = int(np.argmin(np.abs(az - sgn * math.pi / 2)))
            p0 = P[-1, j]
            chin = np.array([0.0, c[1] - 0.045 * H / 1.75, brow - head_h * 0.95])
            pts = np.array([p0 + (chin - p0) * t for t in np.linspace(0, 1, 8)])
            # la cinghia segue la guancia: spinta fuori dalla pelle
            strap = []
            for p in pts:
                loc, nrm, _, dist = body.bvh.find_nearest(Vector(p))
                q = np.array(loc) + np.array(nrm) * 0.004 if loc is not None else p
                tdir = unit(np.cross(np.array(nrm), unit(chin - p0)))
                strap.append([q - tdir * 0.006, q + tdir * 0.006])
            S = np.array(strap)
            sb = plate_from_grid(S, name + '_sottogola')
            finish_plate(sb, 0.0018, bevel=0.0)
            parts.append(sb)
    ob = G.join(parts, name)
    ob['rigid'] = 'Head'
    return ob


def helmet_band(body, g, name):
    """fascia di stoffa attorno all'elmo (leva)"""
    H = body.H
    c, top, brow, hm = head_frame(body)
    allowed = bone_mask(body, ['Head', 'HeadTop_End'], 0.25)
    az = np.linspace(-math.pi, math.pi, 56, endpoint=False)
    head_h = top - brow
    pts = []
    for zf in (0.18, 0.42):
        row = []
        for a in az:
            horiz = np.array([math.sin(a), -math.cos(a), 0.0])
            th = math.pi / 2 - 0.55 * (1 - zf) + 0.2
            d = unit(UP * math.cos(th) * 0.0 + horiz)
            o = c + np.array([0, 0, head_h * zf])
            hit = body.ray_out(o, d, allowed, 0.4)
            r = np.dot(hit - o, d) if hit is not None else 0.09
            row.append(o + d * (r + max(g.get('inflate', 0.014) * H * 0.55, 0.010) + 0.006))
        pts.append(row)
    P = np.array(pts)
    ob = plate_from_grid(P, name, wrap=True)
    G.subsurf(ob, 1)
    finish_plate(ob, 0.003, bevel=0.0)
    ob['rigid'] = 'Head'
    return ob


# ------------------------------------------------------------------------------------------------- busto
def torso_center(body, z, mask):
    sel = mask & (np.abs(body.P[:, 2] - z) < 0.02)
    if sel.sum() < 5:
        return np.array([0.0, 0.0, z])
    return np.array([0.0, body.P[sel, 1].mean(), z])


def torso_armor(body, g, name, kind, style):
    H = body.H
    hr = g.get('height_range', [0.55, 0.835])
    z0 = body.at_height(hr[0]); z1 = body.at_height(hr[1])
    off = g.get('inflate', 0.015) * H
    tb = ['Hips', 'Spine', 'Spine1', 'Spine2', 'LeftShoulder', 'RightShoulder', 'Neck']
    allowed = bone_mask(body, tb, 0.35)
    neck = body.head('Neck') if body.has('Neck') else np.array([0, 0, z1])
    z1 = min(z1, max(neck[2] - 0.005, z0 + 0.75 * (z1 - z0)))   # sotto il collo, ma non troppo corta sui corpi curvi
    front_only = kind == 'chest_plate'
    (a0, a1), faced = (-1.45, 1.45), True
    if not front_only:
        a0, a1 = -math.pi, math.pi
    nz, na = 30, 64 if not front_only else 36
    zs = np.linspace(z0, z1, nz)
    angs = np.linspace(a0, a1, na, endpoint=front_only)
    centers = {float(z): torso_center(body, z, allowed) for z in zs}
    o_fn = lambda z: centers.get(float(z), torso_center(body, z, allowed))
    R = np.full((nz, na), np.nan)
    for i, z in enumerate(zs):
        o = o_fn(z)
        for j, al in enumerate(angs):
            d = np.array([math.sin(al), -math.cos(al), 0.0])   # 0 = davanti
            hit = body.ray_out(o, d, allowed, 0.6)
            if hit is not None: R[i, j] = np.dot(hit - o, d)
    R = envelope(radii_fill(R), (1.5, 2.0), grow=5, wrap=not front_only) + off
    # scalfi per le braccia: ai lati in alto la piastra si ritira (niente ascelle coperte)
    shoulder_x = abs(body.head('LeftArm')[0]) if body.has('LeftArm') else 0.18
    P = np.zeros((nz, na, 3))
    for i, z in enumerate(zs):
        o = o_fn(z)
        for j, al in enumerate(angs):
            d = np.array([math.sin(al), -math.cos(al), 0.0])
            P[i, j] = o + d * R[i, j]
    parts = []
    thick = 0.004 if style == 'flak' else 0.005
    if style == 'flak':
        # giubbetto imbottito: davanti e dietro, con scalfi; piastre in rilievo davanti
        vest = vest_from_grid(P, zs, angs, shoulder_x, neck, name)
        G.store(vest, 'edge', G.edge_distance(vest, 0.05))
        G.solidify(vest, 0.011, offset=-1.0)   # imbottitura spessa
        G.bevel_sharp(vest, 0.003, 2, angle=35)
        parts.append(vest)
        # piastre: petto, addome (due lame), dietro
        for (zz0, zz1, a_c, a_h, lift) in ((0.70, 0.98, 0.0, 0.62, 0.006), (0.40, 0.66, 0.0, 0.58, 0.005), (0.14, 0.38, 0.0, 0.56, 0.004),
                                            (0.45, 0.95, math.pi, 0.7, 0.005)):
            parts.append(raised_plate(P, zs, angs, R, o_fn, zz0, zz1, a_c, a_h, lift + 0.011, thick, name + '_piastra', wrap=True))
        for sgn in (-1, 1):   # spallacci imbottiti del giubbetto, sopra le spalle
            parts.append(shoulder_strap(body, sgn, P, zs, angs, R, o_fn, name + '_spalla', width=0.055 * H / 1.75, thick=0.010, lift=off * 0.9))
    else:
        # corazza di lamiera: fasce orizzontali sovrapposte (dal basso in alto), rivettate
        n_b = 3 if kind != 'heavy_torso' else 4
        cuts = np.linspace(0, 1, n_b + 1)
        for k in range(n_b):
            f0 = cuts[k] - (0.04 if k else 0); f1 = cuts[k + 1] + 0.02
            lift = (n_b - 1 - k) * 0.004 if k < n_b - 1 else 0.0
            lift = k * 0.0035
            if front_only:
                pl = raised_plate(P, zs, angs, R, o_fn, f0, f1, 0.0, 1.35, lift, thick, f'{name}_fascia{k}', wrap=False, round_frac=0.12)
            else:
                pl = vest_band(P, zs, angs, R, o_fn, f0, f1, lift, thick, shoulder_x, neck, f'{name}_fascia{k}')
            parts.append(pl)
        # rivetti lungo le fasce
        pts, nrm = [], []
        for k in range(n_b):
            zi = int(((cuts[k] + cuts[k + 1]) / 2) * (nz - 1))
            for al in (np.linspace(-1.1, 1.1, 5) if front_only else np.linspace(-math.pi, math.pi, 12, endpoint=False)):
                j = int(np.argmin(np.abs(angs - al)))
                d = np.array([math.sin(angs[j]), -math.cos(angs[j]), 0.0])
                if not front_only and abs(abs(angs[j]) - math.pi / 2) < 0.35 and zs[zi] > neck[2] - 0.25: continue
                pts.append(o_fn(zs[zi]) + d * (R[zi, j] + k * 0.0035 + thick + 0.001)); nrm.append(d)
        parts += rivets(pts, nrm, 0.005 if style == 'vrepigh' else 0.0045, name + '_riv')
        # spallacci di cuoio (cinghie sopra le spalle) per la piastra davanti
        if front_only:
            for sgn in (-1, 1):
                parts.append(shoulder_strap(body, sgn, P, zs, angs, R, o_fn, name + '_bretella'))
    ob = G.join(parts, name)
    ob['wsmooth'] = 8
    return ob


def armhole_keep(shoulder_x, zt):
    """scalfi (ellissi attorno all'attacco del braccio) e scollo (piu' basso davanti)"""
    def keep(cn):
        x, y, z = cn
        ax = shoulder_x * 1.10; az = zt - 0.05
        arm = ((abs(x) - ax) / (shoulder_x * 0.34)) ** 2 + ((z - az) / 0.11) ** 2 < 1.0
        nk = (x ** 2) / (0.072 ** 2) + ((z - (zt + 0.01)) ** 2) / ((0.075 if y < 0 else 0.03) ** 2) < 1.0
        return not arm and not nk
    return keep


def vest_from_grid(P, zs, angs, shoulder_x, neck, name):
    """giubbetto: griglia del busto con scalfi delle braccia e scollo tolti, bordo lisciato"""
    nz, na = P.shape[:2]
    ob = plate_from_grid(P, name + '_giubbetto', wrap=True)
    zt = zs[-1]
    keep = armhole_keep(shoulder_x, zt)
    G.cut_fn(ob, keep)
    G.keep_largest(ob)
    G.subsurf(ob, 1)
    G.smooth_boundary(ob, iters=25, lam=0.5)
    return ob


def raised_plate(P, zs, angs, R, o_fn, f0, f1, a_c, a_h, lift, thick, name, wrap=True, round_frac=0.3):
    """piastra in rilievo sulla griglia del busto, tra le frazioni d'altezza f0..f1, centrata sull'angolo a_c"""
    nz = len(zs)
    zz = np.linspace(zs[0] + (zs[-1] - zs[0]) * f0, zs[0] + (zs[-1] - zs[0]) * f1, 12)
    lo, hi = rounded_angles(len(zz), a_c - a_h, a_c + a_h, round_frac)
    Q = np.zeros((len(zz), 22, 3))
    for i, z in enumerate(zz):
        al = np.linspace(lo[i], hi[i], 22)
        alw = np.angle(np.exp(1j * al))
        rz = np.array([np.interp(z, zs, R[:, int(np.argmin(np.abs(np.angle(np.exp(1j * (angs - a))))))]) for a in alw])
        rz = rz + lift + 0.003 * np.sin(np.linspace(0, math.pi, 22))
        o = o_fn(zs[int(np.argmin(np.abs(zs - z)))])
        o = np.array([o[0], o[1], z])
        for j, a in enumerate(alw):
            Q[i, j] = o + np.array([math.sin(a), -math.cos(a), 0.0]) * rz[j]
    pl = plate_from_grid(Q, name)
    finish_plate(pl, thick, bevel=0.0018)
    return pl


def vest_band(P, zs, angs, R, o_fn, f0, f1, lift, thick, shoulder_x, neck, name):
    """fascia di corazza tutto attorno (aperta sotto le ascelle se e' in alto)"""
    nz = len(zs)
    i0 = int(max(0, f0) * (nz - 1)); i1 = int(min(1, f1) * (nz - 1))
    Q = P[i0:i1 + 1].copy()
    for i in range(Q.shape[0]):
        o = o_fn(zs[i0 + i])
        for j in range(Q.shape[1]):
            d = Q[i, j] - np.array([o[0], o[1], zs[i0 + i]]); d[2] = 0
            Q[i, j] += unit(d) * (lift + 0.002 * math.sin(math.pi * i / max(1, Q.shape[0] - 1)))
    ob = plate_from_grid(Q, name, wrap=True)
    zt = zs[-1]
    keep = armhole_keep(shoulder_x, zt)
    G.cut_fn(ob, keep)
    G.keep_largest(ob)
    G.smooth_boundary(ob, iters=15, lam=0.5)
    finish_plate(ob, thick, bevel=0.0018)
    return ob


def shoulder_strap(body, sgn, P, zs, angs, R, o_fn, name, width=0.032, thick=0.003, lift=0.012):
    """bretella dalla piastra davanti, sopra la spalla, fino dietro"""
    x = sgn * 0.095 * body.H / 1.75
    top = zs[-1]
    allowed = bone_mask(body, ['Spine2', 'Neck', 'LeftShoulder', 'RightShoulder'], 0.3)
    c = np.array([x, o_fn(zs[-1])[1], top - 0.04])
    pts = []
    for th in np.linspace(-0.15, math.pi + 0.15, 16):
        d = np.array([0.0, -math.cos(th), math.sin(th)])
        hit = body.ray_out(c, d, allowed, 0.4)
        if hit is None: continue
        loc, nrm, _, _ = body.bvh.find_nearest(Vector(hit))
        pts.append(np.array(hit) + d * lift)
    if len(pts) < 5:
        return None   # spalla non trovata (corpo molto curvo): niente bretella
    pts = np.array(pts)
    for _ in range(3): pts[1:-1] = 0.5 * pts[1:-1] + 0.25 * (pts[:-2] + pts[2:])
    side = np.array([1.0, 0, 0])
    S = np.stack([pts - side * width / 2, pts + side * width / 2], 1)
    ob = plate_from_grid(S, name)
    G.subsurf(ob, 1)
    finish_plate(ob, thick, bevel=min(0.003, thick * 0.3))
    return ob


# ------------------------------------------------------------------------------------------------- stivali e cinture
def boot(body, g, name, style):
    H = body.H
    side = 'Left' if (g.get('side', 1) > 0) else 'Right'
    hr = g.get('height_range', [0.0, 0.11])
    z_top = body.at_height(hr[1]) + 0.02
    gg = dict(g); gg['height_range'] = [-0.05, hr[1] + 0.035]; gg['min_weight'] = 0.2
    gg.pop('facing', None)
    # forma della scarpa: il corpo (chiuso) tagliato sopra la caviglia e rifuso a voxel grossi: le dita spariscono
    ob = G.copy(body.obj, name)
    for mm in list(ob.modifiers): ob.modifiers.remove(mm)
    sgn = 1 if side == 'Left' else -1
    bm = bmesh.new(); bm.from_mesh(ob.data)
    res = bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=Vector((0, 0, z_top + 0.03)),
                                 plane_no=Vector((0, 0, 1)), clear_outer=True)
    bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
    kill = [v for v in bm.verts if v.co.x * sgn < 0]
    bmesh.ops.delete(bm, geom=kill, context='VERTS')
    bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
    bm.to_mesh(ob.data); bm.free(); ob.data.update()
    foot = side + 'Foot'
    ank = body.head(foot)[2] if body.has(foot) else body.at_height(0.06)
    # del corpo resta solo il gambale (sopra la caviglia): il piede lo fa l'involucro
    bm = bmesh.new(); bm.from_mesh(ob.data)
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=Vector((0, 0, ank - 0.005)),
                           plane_no=Vector((0, 0, 1)), clear_inner=True)
    bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=0)
    bm.to_mesh(ob.data); bm.free(); ob.data.update()
    # piede: involucro convesso (punta tonda, niente dita), unito al gambale
    from scipy.spatial import ConvexHull
    fp = body.P[(body.P[:, 0] * sgn > 0.005) & (body.P[:, 2] < ank + 0.01) & ~body.aux]
    hull = ConvexHull(fp)
    hv = np.unique(hull.simplices); rm_ = -np.ones(len(fp), int); rm_[hv] = np.arange(len(hv))
    ho = G.obj_from(name + '_scarpa', fp[hv], rm_[hull.simplices])
    bm = bmesh.new(); bm.from_mesh(ho.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])   # i triangoli dell'involucro hanno versi a caso
    bm.to_mesh(ho.data); bm.free(); ho.data.update()
    ob = G.join([ob, ho], name)
    m = ob.modifiers.new('rm', 'REMESH'); m.mode = 'VOXEL'; m.voxel_size = 0.008 * H / 1.75
    G.apply_mods(ob)
    G.keep_largest(ob, 0.2)
    G.taubin(ob, iters=40)
    G.cut(ob, (0, 0, z_top), (0, 0, -1))
    G.keep_largest(ob)
    G.smooth_boundary(ob, iters=10)
    off = max(g.get('inflate', 0.007) * H, 0.006)
    G.offset_normals(ob, off * 0.6)
    G.clearance(ob, body, off * 0.7)
    P = G.verts(ob)
    floor = body.z0
    # suola: piatta, un po' piu' larga del piede (guardolo), tacco
    sole_h = 0.022 * H / 1.75
    low = P[:, 2] < floor + sole_h
    ctr = np.array([P[:, 0].mean(), P[:, 1].mean()])
    P[low, :2] += (P[low, :2] - ctr) * 0.04
    P[:, 2] = np.maximum(P[:, 2], floor - 0.002)
    P[P[:, 2] < floor + 0.004, 2] = floor - 0.002
    G.set_verts(ob, P)
    # risvolto in cima
    G.store(ob, 'edge', G.edge_distance(ob, 0.04))
    G.solidify(ob, 0.004, offset=-1.0)
    parts = [ob]
    # fibbie: due cinghie attorno al gambale
    leg = side + 'Leg'
    if body.has(leg):
        a, b, L = limb_info(body, leg)
        o_fn = lambda t: a + (b - a) * t
        ang = np.linspace(-math.pi, math.pi, 36, endpoint=False)
        for zz in (z_top - 0.03, z_top - 0.075):
            t = (a[2] - zz) / max(a[2] - b[2], 1e-6)
            Rb, frb = cyl_shell(body, o_fn, b - a, FRONT, [t], ang, bone_mask(body, [leg, side + 'Foot'], 0.2), 0.3)
            Rb = envelope(radii_fill(Rb), (0.5, 2.0), wrap=True)[0] + off * 0.6 + 0.0045
            parts.append(band(o_fn, frb, t, ang, Rb, 0.016 * H / 1.75, 0.003, name + '_fibbia'))
    out = G.join(parts, name)
    out['wsmooth'] = 6
    return out


def belt(body, g, name, style):
    """cinturone: fascia attorno ai fianchi con fibbia"""
    H = body.H
    hr = g.get('height_range', [0.5, 0.58])
    zc = body.at_height((hr[0] + hr[1]) / 2)
    width = max(0.04, (hr[1] - hr[0]) * H * 0.8)
    allowed = bone_mask(body, ['Hips', 'Spine', 'LeftUpLeg', 'RightUpLeg'], 0.3)
    angs = np.linspace(-math.pi, math.pi, 64, endpoint=False)
    rows = []
    for z in (zc - width / 2, zc + width / 2):
        o = torso_center(body, z, allowed)
        row = []
        for al in angs:
            d = np.array([math.sin(al), -math.cos(al), 0.0])
            hit = body.ray_out(o, d, allowed, 0.6)
            row.append(np.dot(hit - o, d) if hit is not None else np.nan)
        rows.append(row)
    R = envelope(radii_fill(np.array(rows)), (0.5, 2.0), wrap=True) + g.get('inflate', 0.012) * H
    P = np.zeros((2, len(angs), 3))
    for i, z in enumerate((zc - width / 2, zc + width / 2)):
        o = torso_center(body, z, allowed)
        for j, al in enumerate(angs):
            P[i, j] = np.array([o[0], o[1], z]) + np.array([math.sin(al), -math.cos(al), 0.0]) * R[i, j]
    ob = plate_from_grid(P, name, wrap=True)
    G.subsurf(ob, 1)
    finish_plate(ob, 0.005, bevel=0.0012)
    parts = [ob]
    # fibbia davanti
    o = torso_center(body, zc, allowed)
    fpos = np.array([o[0], o[1], zc]) + FRONT * (R[:, 0].mean() + 0.006)
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * 0.055, v.co.y * 0.008, v.co.z * width * 1.1)) + Vector(fpos)
    me = bpy.data.meshes.new(name + '_fibbia'); bm.to_mesh(me); bm.free()
    fb = G.link(bpy.data.objects.new(name + '_fibbia', me))
    G.bevel_sharp(fb, 0.002, 2, angle=30)
    parts.append(fb)
    out = G.join(parts, name)
    return out
