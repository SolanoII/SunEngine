# Archetipi "da guerra" delle armi da fuoco e a energia: sagome massicce e cattive invece di tubi lunghi e stretti.
#
# Linguaggio comune: castelli alti e squadrati con smussi, pannelli incassati, feritoie e piastre imbullonate;
# camicie e freni di bocca enormi; caricatori spessi o a tamburo; calci pieni; baionette seghettate e punte.
# Le misure che contano (impugnature, bocca della canna) restano quelle del modello a blocchi; lo spessore cresce.
#
#   'k' Regno: acciaio rivettato, noce massiccio, ottone; tamburi, camicie di raffreddamento, baionette da trincea
#   'v' Vanguardia: gusci d'acciaio lavorati a macchina, piastre rosso sangue, strisce di pericolo, ottiche, lanciagranate
#   'x' Vrepigh: lamiere sovrapposte e saldate, punte, lame da sega, catene e filo
import math
import numpy as np
from mathutils import Matrix
from kit import *
from arch import pistol_grip, trigger_group, wire_wrap, rivets_line

METAL = {'k': 'steel', 'v': 'gunmetal', 'x': 'scrap'}
PLATE = {'k': 'steel', 'v': 'paint_v', 'x': 'scrap_red'}


# ---------------------------------------------------------------------------------------------- forme
def zprofile(sec, z0, z1, bev=0.0, segs=2):
    """sezione simmetrica [(x, y)] estrusa lungo z da z0 a z1"""
    bm = profile(sec, z0, z1, bev, segs)
    return transform(bm, Matrix.Rotation(math.radians(-90), 4, 'Y'))


def sec(w, yb, yt, cht, chb=None):
    """sezione squadrata con smussi (sopra cht, sotto chb)"""
    chb = cht * 0.5 if chb is None else chb
    h = w / 2
    return [(-h, yb + chb), (-h + chb, yb), (h - chb, yb), (h, yb + chb), (h, yt - cht), (h - cht, yt), (-h + cht, yt), (-h, yt - cht)]


def slab(B, x, pts, th, mat, bev=0.0008):
    """piastra sottile (sagoma laterale) appoggiata al fianco x"""
    s = 1 if x >= 0 else -1
    B.add(profile(pts, x, x + s * th, bev, 1), mat)


def hazard(B, x, z0, z1, y0, y1, n=4):
    """fascia di strisce di pericolo sul fianco x (gialla e nera, inclinate)"""
    s = 1 if x >= 0 else -1
    slab(B, x, [(z0, y0), (z1, y0), (z1, y1), (z0, y1)], 0.0012, 'paint_k')
    L = z1 - z0; sw = L / (2 * n); dy = (y1 - y0) * 0.6
    for i in range(n):
        za = z0 + i * 2 * sw + sw * 0.3
        pts = [(za, y0 + 0.001), (za + sw, y0 + 0.001), (min(za + sw + dy, z1 - 0.001), y1 - 0.001), (min(za + dy, z1 - 0.001), y1 - 0.001)]
        slab(B, x + s * 0.0012, pts, 0.0006, 'hazard', 0)


def bolts(B, pts, side, r=0.0035, mat='steel'):
    for p in pts: B.add(rivet(p, r, (side, 0, 0)), mat)


def weld(B, pts, mat='scrap'):
    B.add(tube(pts, 0.0022, segs=6), mat)


def spike(B, base, tip, w=0.008, mat='steel'):
    d = np.array(tip, float) - np.array(base, float)
    B.add(wedge(base, (w, w), tip, d), mat)


# ---------------------------------------------------------------------------------------------- pezzi
def receiver(B, st, zf, zr, yb, yt, w, rng, port=True):
    """castello squadrato; stile: rivetti e piastre (k), pannelli incassati e feritoie (v), lamiere saldate (x)"""
    m = METAL[st]
    pts = [(zr, yb + 0.012), (zr, yt - 0.024), (zr - 0.026, yt), (zf + 0.035, yt), (zf, yt - 0.03), (zf, yb + 0.02), (zf + 0.02, yb)]
    if st == 'x':
        pts = [(zr, yb + 0.004), (zr + 0.006, yt - 0.01), (zr - 0.01, yt + 0.004), (zf + 0.02, yt), (zf - 0.006, yt - 0.016), (zf, yb + 0.006), (zf + 0.012, yb - 0.004)]
    rec = profile(pts, -w / 2, w / 2, 0.005 if st != 'x' else 0.002, 2 if st != 'x' else 1)
    cuts = []
    L = zr - zf; H = yt - yb
    if port: cuts.append(box((w / 2, yt - H * 0.32, zf + L * 0.55), (0.02, H * 0.2, L * 0.22)))
    if st == 'v':
        for s in (1, -1):   # pannelli incassati
            cuts.append(box((s * w / 2, yb + H * 0.36, zf + L * 0.42), (0.006, H * 0.34, L * 0.48)))
            cuts.append(box((s * w / 2, yt - H * 0.18, zr - L * 0.16), (0.006, H * 0.12, L * 0.14)))
    for i in range(4):     # feritoie di sfogo sul retro
        cuts.append(box((0, yt - H * 0.3, zr - 0.02 - i * 0.013), (w + 0.02, H * 0.18, 0.0055)))
    B.add(boolean_bm(rec, cuts), m)
    if st == 'k':
        for s in (1, -1):
            slab(B, s * w / 2, [(zf + L * 0.12, yb + H * 0.12), (zf + L * 0.48, yb + H * 0.12), (zf + L * 0.48, yb + H * 0.62), (zf + L * 0.12, yb + H * 0.62)], 0.003, 'steel', 0.0012)
            bolts(B, [(s * (w / 2 + 0.003), yb + H * a, zf + L * b) for a in (0.2, 0.54) for b in (0.16, 0.44)], s, 0.0032, 'brass')
            bolts(B, [(s * w / 2, yb + 0.008, zf + L * t) for t in (0.6, 0.75, 0.9)], s, 0.003)
    elif st == 'v':
        hazard(B, w / 2, zr - L * 0.3, zr - L * 0.04, yb + H * 0.12, yb + H * 0.34, 3)
        for s in (1, -1):
            bolts(B, [(s * w / 2, y, z) for y in (yb + H * 0.12, yt - H * 0.08) for z in (zf + L * 0.08, zr - L * 0.06)], s, 0.0026, 'gunmetal')
    else:
        for s in (1, -1):
            a = rng.uniform(0.05, 0.2); b = rng.uniform(0.5, 0.7)
            p = [(zf + L * a, yb + H * rng.uniform(0.05, 0.2)), (zf + L * b, yb + H * 0.1), (zf + L * (b + 0.12), yb + H * 0.5), (zf + L * b, yt - H * 0.1), (zf + L * a, yt - H * rng.uniform(0.15, 0.3))]
            slab(B, s * w / 2, p, 0.004, 'scrap_red' if s > 0 else 'scrap', 0.001)
            weld(B, [(s * (w / 2 + 0.002), y, z) for z, y in p] + [(s * (w / 2 + 0.002), p[0][1], p[0][0])])
            bolts(B, [(s * (w / 2 + 0.004), y, z) for z, y in p[:4]], s, 0.004, 'scrap')
    return m


def shroud(B, st, z0, z1, bore, r, rng, kind='auto'):
    """camicia della canna da z0 (davanti) a z1 (dietro)"""
    L = z1 - z0
    if st == 'k':   # camicia tonda di raffreddamento con feritoie lunghe e anelli
        jk = lathe([(z0, r * 0.9), (z0 + 0.01, r), (z1, r)], 'z', 26)
        cuts = [cyl((0, 0, (z0 + z1) / 2), r * 0.82, L + 0.05, 'z', 22)]
        n = max(2, int(L / 0.06))
        for i in range(n):
            zc = z0 + 0.03 + (i + 0.5) * (L - 0.05) / n
            for a in np.linspace(0, math.pi, 4, endpoint=False):
                c = box((0, 0, zc), (r * 2.4, 0.006, (L - 0.05) / n * 0.62))
                cuts.append(transform(c, Matrix.Translation((0, 0, 0)) @ Matrix.Rotation(a, 4, 'Z')))
        jk = boolean_bm(jk, cuts)
        B.add(translate(jk, (0, bore, 0)), 'steel')
        for z in (z0 + 0.012, z1 - 0.01, (z0 + z1) / 2):
            B.add(translate(lathe([(z - 0.008, r * 1.08), (z + 0.008, r * 1.08)], 'z', 26), (0, bore, 0)), 'brass' if z == (z0 + z1) / 2 else 'steel')
        B.add(translate(cyl((0, 0, (z0 + z1) / 2), r * 0.42, L, 'z', 16), (0, bore, 0)), 'blued')
        return
    if st == 'v':   # guscio squadrato con fessure laterali, costola superiore
        w = r * 2.0
        sh = zprofile(sec(w, bore - r, bore + r * 0.95, r * 0.45, r * 0.3), z0, z1, 0.003)
        cuts = []
        n = max(2, int(L / 0.035))
        for i in range(n):
            z = z0 + 0.025 + i * (L - 0.04) / n
            cuts.append(box((0, bore + r * 0.15, z), (w + 0.02, r * 0.75, 0.008)))
        for i in range(max(1, int(L / 0.05))):
            cuts.append(cyl((0, bore + r, z0 + 0.03 + i * 0.05), r * 0.28, 0.03, 'y', 10))
        B.add(boolean_bm(sh, cuts), 'gunmetal')
        B.add(translate(cyl((0, 0, (z0 + z1) / 2), r * 0.42, L, 'z', 16), (0, bore, 0)), 'blued')
        B.add(box((0, bore + r * 0.95 + 0.004, (z0 + z1) / 2), (0.012, 0.008, L * 0.9), 0.002), 'gunmetal')        # costola
        for s in (1, -1):
            B.add(box((s * (w / 2 + 0.0015), bore - r * 0.5, (z0 + z1) / 2), (0.003, r * 0.4, L * 0.85), 0.001), PLATE['v'])
        return
    # rottami: tubo quadro con lamiere saldate e fori tagliati a fiamma
    w = r * 2.1
    sh = zprofile(sec(w, bore - r, bore + r, r * 0.2, r * 0.2), z0, z1, 0.0015, 1)
    cuts = [cyl((0, bore + rng.uniform(-r, r) * 0.3, z0 + 0.03 + i * (L - 0.05) / max(1, int(L / 0.045))), r * rng.uniform(0.3, 0.45), w + 0.02, 'x', 9)
            for i in range(max(1, int(L / 0.045)))]
    B.add(boolean_bm(sh, cuts), 'scrap')
    B.add(translate(cyl((0, 0, (z0 + z1) / 2), r * 0.45, L, 'z', 12), (0, bore, 0)), 'steel')
    for z in np.linspace(z0 + 0.02, z1 - 0.02, max(2, int(L / 0.09))):
        B.add(box((0, bore, z), (w + 0.008, r * 2 + 0.008, 0.012), 0.001), 'scrap')
        bolts(B, [(w / 2 + 0.004, bore, z)], 1, 0.004, 'scrap')


def brake(B, st, zf, bore, r, rng):
    """freno di bocca enorme che finisce esattamente alla bocca (zf)"""
    L = r * 3.2
    if st == 'k':
        p = [(zf, r * 0.95), (zf + 0.004, r * 1.05), (zf + L, r * 1.05), (zf + L + 0.008, r * 0.8)]
        bm = lathe(p, 'z', 22)
        cuts = [box((0, 0, zf + L * t), (r * 3, r * 0.7, L * 0.13)) for t in (0.28, 0.52, 0.76)]
        cuts.append(cyl((0, 0, zf), r * 0.42, L * 3, 'z', 14))
        B.add(translate(boolean_bm(bm, cuts), (0, bore, 0)), 'steel')
        return L
    if st == 'v':
        w = r * 2.3
        bm = zprofile(sec(w, bore - r * 1.05, bore + r * 1.05, r * 0.5, r * 0.5), zf, zf + L, 0.002)
        cuts = [box((0, bore, zf + L * t), (w + 0.02, r * 1.2, L * 0.12)) for t in (0.25, 0.47, 0.69)]
        cuts += [cyl((0, bore + r, zf + L * t), r * 0.25, 0.04, 'y', 10) for t in (0.3, 0.55, 0.8)]
        cuts.append(cyl((0, bore, zf), r * 0.42, L * 3, 'z', 14))
        B.add(boolean_bm(bm, cuts), 'gunmetal')
        return L
    # rottami: tubo con corona di denti
    bm = lathe([(zf + 0.01, r * 0.85), (zf + L, r * 0.85)], 'z', 14)
    bm = boolean_bm(bm, [cyl((0, 0, zf), r * 0.55, L * 3, 'z', 12)])
    B.add(translate(bm, (0, bore, 0)), 'scrap')
    for a in np.linspace(0, 2 * math.pi, 7, endpoint=False):
        base = (math.cos(a) * r * 0.72, bore + math.sin(a) * r * 0.72, zf + 0.012)
        B.add(wedge(base, (r * 0.35, r * 0.25), (math.cos(a) * r * 0.9, bore + math.sin(a) * r * 0.9, zf - rng.uniform(0.0, 0.012)), (0, 0, -1)), 'steel')
    return L


def box_mag(B, st, z, ytop, h, curve, w=0.036, l=0.06):
    n = 10; f, b = [], []
    for i in range(n + 1):
        t = i / n; y = ytop - h * t; off = -curve * t * t
        f.append((z - l / 2 + off, y)); b.append((z + l / 2 + off * 0.9, y))
    mag = profile(f + b[::-1], -w / 2, w / 2, 0.004, 2)
    cuts = [box((s * w / 2, ytop - h * 0.5, z - curve * 0.3), (0.004, h * 0.6, l * 0.22)) for s in (1, -1)]   # nervature
    B.add(boolean_bm(mag, cuts), {'k': 'steel', 'v': 'gunmetal', 'x': 'scrap'}[st])
    B.add(box((0, ytop - h - 0.006, z - curve + 0.002), (w + 0.008, 0.014, l + 0.01), 0.004), 'steel' if st != 'v' else 'rubber')
    if st == 'x':
        weld(B, [(w / 2 + 0.002, ytop - h * 0.3, z - l / 2), (w / 2 + 0.002, ytop - h * 0.35, z + l / 2)])


def drum(B, z, yc, r, w, mat):
    d = lathe([(-w / 2, r * 0.9), (-w / 2 + 0.006, r), (w / 2 - 0.006, r), (w / 2, r * 0.9)], 'x', 30, (0, yc, z))
    d = boolean_bm(d, [cyl((s * w / 2, yc, z), r * 0.65, 0.008, 'x', 24) for s in (1, -1)])
    B.add(d, mat)
    B.add(cyl((0, yc, z), r * 0.3, w + 0.012, 'x', 16, 0.003), 'brass')
    B.add(box((w / 2 + 0.006, yc, z), (0.006, r * 1.2, 0.012), 0.002), 'steel')     # leva di carica


def chunky_guard(B, zg, yb, m):
    """ponticello massiccio sotto il castello, davanti all'impugnatura"""
    zt = zg - 0.04
    pts = [(zt + 0.024, yb), (zt - 0.04, yb), (zt - 0.05, yb - 0.012), (zt - 0.046, yb - 0.036), (zt - 0.03, yb - 0.042), (zt + 0.03, yb - 0.04),
           (zt + 0.03, yb - 0.032), (zt - 0.026, yb - 0.034), (zt - 0.038, yb - 0.028), (zt - 0.04, yb - 0.012), (zt - 0.032, yb - 0.007), (zt + 0.024, yb - 0.007)]
    B.add(profile(pts, -0.007, 0.007, 0.002), m)
    B.add(profile([(zt + 0.004, yb), (zt - 0.008, yb), (zt - 0.014, yb - 0.022), (zt - 0.006, yb - 0.03), (zt - 0.002, yb - 0.016)], -0.003, 0.003, 0.001), 'bare')


def stock(B, st, z0, zr, bore, yb, rng, solid=True):
    """calcio: noce massiccio con calciolo d'ottone e punta (k), guscio armato (v), lamiere e tondino (x)"""
    sb = bore - 0.14; stp = bore + 0.022
    if st == 'k':
        pts = [(z0, yb + 0.006), (z0 + 0.06, yb - 0.012), (zr - 0.01, sb), (zr, sb + 0.008), (zr, stp - 0.008), (zr - 0.012, stp),
               (z0 + 0.12, stp), (z0 + 0.07, bore + 0.012), (z0, bore + 0.02)]
        B.add(profile(pts, -0.027, 0.027, 0.014, 3), 'wood')
        B.add(box((0, (sb + stp) / 2, zr + 0.004), (0.058, stp - sb + 0.006, 0.01), 0.003), 'brass')
        B.add(profile([(zr - 0.03, sb + 0.004), (zr, sb + 0.004), (zr + 0.004, sb - 0.012)], -0.004, 0.004, 0.001), 'steel')
        spike(B, (0, sb + 0.01, zr + 0.006), (0, sb - 0.012, zr + 0.05), 0.012, 'steel')
        return
    if st == 'v':
        pts = [(z0, yb + 0.004), (zr - 0.02, sb), (zr, sb + 0.01), (zr, stp - 0.01), (zr - 0.016, stp), (z0, bore + 0.03)]
        s = profile(pts, -0.03, 0.03, 0.006, 2)
        hole = profile([(z0 + 0.05, yb - 0.01), (zr - 0.06, sb + 0.03), (zr - 0.06, stp - 0.03), (z0 + 0.05, bore + 0.008)], -0.012, 0.012)
        s = boolean_bm(s, [translate(hole, (0.03, 0, 0)), translate(hole.copy(), (-0.03, 0, 0))])
        B.add(s, 'paint_v')
        B.add(box((0, (sb + stp) / 2, zr + 0.008), (0.064, stp - sb + 0.008, 0.018), 0.006), 'rubber')
        hazard(B, 0.03, zr - 0.075, zr - 0.02, sb + 0.035, sb + 0.058, 3)
        return
    # rottami: due lamiere, tondino piegato, punte
    for s in (1, -1):
        p = [(z0, yb + 0.002), (zr, sb + rng.uniform(0, 0.02)), (zr + 0.006, stp), (z0, bore + 0.02)]
        B.add(profile(p, s * 0.02, s * 0.026, 0.001, 1), 'scrap' if s > 0 else 'scrap_red')
    B.add(tube([(0, yb - 0.01, z0 + 0.01), (0, sb + 0.01, zr - 0.02), (0, stp - 0.01, zr - 0.01), (0, bore + 0.01, z0 + 0.04)], 0.006, segs=8), 'steel')
    B.add(box((0, (sb + stp) / 2, zr + 0.008), (0.06, stp - sb + 0.01, 0.014), 0.002), 'scrap')
    for y in np.linspace(sb + 0.02, stp - 0.01, 3):
        spike(B, (0, y, zr + 0.012), (0, y + rng.uniform(-0.01, 0.01), zr + rng.uniform(0.04, 0.06)), 0.012, 'steel')


def trench_bayonet(B, st, z_mount, bore, r, rng):
    """baionetta seghettata (k) o lama da sega saldata (x) sotto la bocca"""
    yb = bore - r * 1.1
    if st == 'k':
        B.add(box((0, yb - 0.006, z_mount + 0.03), (0.016, 0.014, 0.06), 0.003), 'steel')
        L = 0.2
        teeth = [(z_mount - 0.02 - i * 0.018 - (0.009 if j else 0), yb - 0.004 + (0.006 if j else 0)) for i in range(6) for j in (0, 1)]
        pts = [(z_mount + 0.01, yb - 0.004)] + teeth + [(z_mount - L * 0.75, yb - 0.006), (z_mount - L, yb - 0.022), (z_mount - L * 0.7, yb - 0.036), (z_mount + 0.01, yb - 0.034)]
        bl = profile(pts, -0.0035, 0.0035, 0.0006, 1)
        for v in bl.verts:
            if v.co.y < yb - 0.026: v.co.x *= 0.3
        B.add(bl, 'bare')
    else:
        B.add(box((0, yb - 0.008, z_mount + 0.03), (0.02, 0.018, 0.06), 0.001), 'scrap')
        L = 0.17
        teeth = []
        for i in range(7):
            z = z_mount - 0.01 - i * L / 7
            teeth += [(z, yb - 0.05), (z - L / 14, yb - 0.064)]
        pts = [(z_mount + 0.01, yb - 0.004), (z_mount - L, yb - 0.012)] + teeth[::-1] + [(z_mount + 0.01, yb - 0.05)]
        B.add(profile(pts, -0.003, 0.003, 0.0006, 1), 'bare')
        bolts(B, [(0.004, yb - 0.02, z_mount - 0.02), (0.004, yb - 0.02, z_mount - 0.06)], 1, 0.004, 'scrap')


def top_spikes(B, z0, z1, y, n, rng):
    for z in np.linspace(z0, z1, n):
        spike(B, (0, y, z), (0, y + rng.uniform(0.025, 0.04), z + 0.012), 0.012, 'steel')


def optic_block(B, z0, z1, y, red=(1.0, 0.1, 0.05)):
    """mirino elettronico squadrato (Vanguardia) con lente accesa"""
    w = 0.04
    o = zprofile(sec(w, y, y + 0.045, 0.014, 0.004), z0, z1, 0.003)
    o = boolean_bm(o, [box((0, y + 0.024, z1), (w * 0.6, 0.022, 0.012)), box((0, y + 0.024, z0), (w * 0.7, 0.026, 0.012))])
    B.add(o, 'gunmetal')
    B.add(box((0, y + 0.024, z1 - 0.004), (w * 0.55, 0.018, 0.002)), 'glow')
    B.add(box((0, y - 0.004, (z0 + z1) / 2), (0.026, 0.012, (z1 - z0) * 0.8), 0.002), 'gunmetal')
    B.add(box((w / 2 + 0.002, y + 0.03, (z0 + z1) / 2), (0.004, 0.012, 0.016), 0.001), 'rubber')


def scope(B, st, z0, z1, y):
    r = 0.02 if st != 'x' else 0.024
    sc = lathe([(z0, r * 1.25), (z0 + 0.05, r * 1.25), (z0 + 0.08, r * 0.85), (z1 - 0.07, r * 0.85), (z1 - 0.04, r * 1.05), (z1, r * 1.05)], 'z', 22)
    sc = boolean_bm(sc, [cyl((0, 0, z0), r * 1.0, 0.02, 'z', 18), cyl((0, 0, z1), r * 0.8, 0.02, 'z', 18)])
    B.add(translate(sc, (0, y, 0)), 'blued' if st != 'x' else 'scrap')
    B.add(translate(cyl((0, 0, z0 + 0.006), r * 0.95, 0.002, 'z', 18), (0, y, 0)), 'glow')
    for zz in (z0 + 0.1, z1 - 0.09):
        B.add(box((0, y - r - 0.008, zz), (0.022, 0.024, 0.02), 0.003), METAL[st])
        B.add(translate(lathe([(zz - 0.01, r * 0.95), (zz + 0.01, r * 0.95)], 'z', 20), (0, y, 0)), METAL[st])
    B.add(translate(cyl((0.024, 0, (z0 + z1) / 2), 0.009, 0.016, 'x', 12, 0.002), (0, y, 0)), METAL[st])
    B.add(translate(cyl((0, 0.024, (z0 + z1) / 2), 0.009, 0.016, 'y', 12, 0.002), (0, y, 0)), METAL[st])


def bipod(B, z, bore, r, m):
    B.add(box((0, bore - r - 0.008, z), (0.03, 0.016, 0.024), 0.003), m)
    for s in (1, -1):
        B.add(tube([(s * 0.012, bore - r - 0.012, z), (s * 0.016, bore - r - 0.02, z - 0.22)], 0.005, segs=8), m)
        B.add(box((s * 0.016, bore - r - 0.02, z - 0.225), (0.014, 0.014, 0.014), 0.003), 'rubber')


# ---------------------------------------------------------------------------------------------- fucili
def war_rifle(B, lay, st, rng, kind='auto'):
    """fucile da guerra: 'auto', 'heavy', 'bolt' (precisione), 'shotgun', 'mg'"""
    bore = B.bore = lay.bore
    gR = lay.gR; gL = lay.gL if lay.gL is not None else gR + np.array([0, 0.05, -0.32])
    zf, zr = lay.front, lay.rear
    big = kind in ('heavy', 'mg') or st == 'x'
    rz0 = gR[2] + (0.1 if big else 0.085)
    rz1 = gR[2] - (0.24 if kind != 'bolt' else 0.2)
    yt = bore + (0.05 if big else 0.044)
    yb = bore - (0.07 if big else 0.06)
    if kind == 'shotgun': yt += 0.006; yb -= 0.006
    w = {'k': 0.064, 'v': 0.07, 'x': 0.078}[st] + (0.006 if big else 0)
    m = receiver(B, st, rz1, rz0, yb, yt, w, rng)
    r = 0.03 if kind != 'bolt' else 0.026
    if kind == 'shotgun': r = 0.028
    if big: r += 0.004
    bl = 0
    if kind == 'shotgun':
        # due canne sovrapposte: canna grossa e serbatoio tubolare, con scudo termico forato sopra
        zb0 = zf
        B.add(translate(boolean_bm(lathe([(zf, 0.022), (zf + 0.008, 0.024), (rz1 + 0.01, 0.024)], 'z', 22), [cyl((0, 0, zf), 0.015, 0.04, 'z', 16)]), (0, bore, 0)), m if st != 'v' else 'blued')
        B.add(translate(lathe([(zf + 0.03, 0.019), (zf + 0.04, 0.02), (rz1 + 0.01, 0.02)], 'z', 20), (0, bore - 0.045, 0)), m)
        for z in (zf + 0.04, (zf + rz1) / 2):
            B.add(box((0, bore - 0.022, z), (0.05, 0.09, 0.016), 0.004), m)
        hs = zprofile([(-0.028, bore + 0.006), (0.028, bore + 0.006), (0.028, bore + 0.018), (0.016, bore + 0.03), (-0.016, bore + 0.03), (-0.028, bore + 0.018)], zf + 0.06, rz1 + 0.01, 0.002)
        hs = boolean_bm(hs, [cyl((0, bore + 0.03, z), 0.006, 0.04, 'y', 10) for z in np.arange(zf + 0.08, rz1 - 0.01, 0.028)])
        B.add(hs, m)
        # pompa enorme
        pz0, pz1 = gL[2] - 0.08, gL[2] + 0.08
        pm = zprofile(sec(0.06, bore - 0.078, bore - 0.02, 0.012, 0.012), pz0, pz1, 0.005)
        pm = boolean_bm(pm, [box((s * 0.03, bore - 0.05, pz0 + 0.02 + i * 0.016), (0.008, 0.06, 0.006)) for s in (1, -1) for i in range(8)])
        B.add(pm, 'wood' if st == 'k' else ('rubber' if st == 'v' else 'wood_dark'))
        if st == 'v':   # tamburo al posto del serbatoio
            drum(B, gR[2] - 0.11, yb - 0.06, 0.06, 0.06, 'gunmetal')
        if st == 'x':
            top_spikes(B, zf + 0.08, rz1, bore + 0.03, 4, rng)
    else:
        bl = brake(B, st, zf, bore, r * 0.8 if kind != 'bolt' else r * 0.85, rng)
        sz1 = rz1 + 0.01
        shroud(B, st, zf + bl - 0.004, sz1, bore, r, rng, kind)
        if kind == 'bolt': bipod(B, zf + bl + 0.06, bore, r, m)
        if st == 'k' and kind in ('auto', 'bolt'): trench_bayonet(B, st, zf + bl + 0.012, bore, r, rng)
        if st == 'x': trench_bayonet(B, st, zf + bl + 0.012, bore, r, rng); top_spikes(B, zf + bl + 0.03, sz1 - 0.02, bore + r, 3, rng)
        if st == 'k':   # mirino da trincea
            B.add(profile([(zf + bl + 0.01, bore + r), (zf + bl + 0.03, bore + r), (zf + bl + 0.024, bore + r + 0.03), (zf + bl + 0.016, bore + r + 0.03)], -0.003, 0.003, 0.0008), 'steel')
            B.add(box((0, bore + r + 0.006, zf + bl + 0.02), (0.026, 0.012, 0.02), 0.003), 'steel')
        if st == 'v' and kind in ('auto', 'heavy'):   # lanciagranate sotto la canna
            lz0, lz1 = zf + bl + 0.04, max(gL[2] + 0.02, zf + bl + 0.2)
            lt = lathe([(lz0, 0.024), (lz0 + 0.008, 0.026), (lz1, 0.026)], 'z', 22)
            lt = boolean_bm(lt, [cyl((0, 0, lz0), 0.019, 0.04, 'z', 18)])
            B.add(translate(lt, (0, bore - r - 0.03, 0)), 'gunmetal')
            B.add(box((0, bore - r - 0.008, (lz0 + lz1) / 2), (0.022, 0.016, lz1 - lz0 - 0.02), 0.003), 'gunmetal')
            hazard(B, 0.026, lz0 + 0.02, lz0 + 0.07, bore - r - 0.042, bore - r - 0.02, 3)
    # cima
    if kind == 'bolt':
        scope(B, st, rz1 - 0.05, rz0 - 0.01, yt + 0.035)
    elif st == 'v':
        optic_block(B, rz1 + 0.04, rz0 - 0.04, yt)
    elif kind == 'mg' or (kind == 'heavy' and st == 'k'):
        hz = gR[2] - 0.15
        B.add(tube([(0, yt, hz + 0.06), (0, yt + 0.045, hz + 0.04), (0, yt + 0.045, hz - 0.06), (0, yt, hz - 0.08)], 0.007, segs=10), m)
    elif st == 'k':
        B.add(box((0, yt + 0.008, rz0 - 0.04), (0.03, 0.016, 0.03), 0.003), 'steel')
        B.add(box((0, yt + 0.02, rz0 - 0.04), (0.006, 0.016, 0.012), 0.001), 'steel')
    elif st == 'x':
        top_spikes(B, rz1 + 0.04, rz0 - 0.04, yt, 3, rng)
    # caricatore
    mz = gR[2] - 0.1
    if kind == 'mg':
        bz = gR[2] - 0.11
        if st == 'x':
            B.add(box((-w / 2 - 0.045, bore - 0.05, bz), (0.09, 0.12, 0.13), 0.004, 2), 'scrap_red')
            bolts(B, [(-w / 2 - 0.09, bore - 0.05 + dy, bz + dz) for dy in (-0.04, 0.04) for dz in (-0.05, 0.05)], -1, 0.0045, 'scrap')
        else:
            B.add(box((-w / 2 - 0.045, bore - 0.06, bz), (0.09, 0.12, 0.14), 0.005, 2), 'paint_k' if st == 'k' else 'paint_v')
            B.add(tube([(-w / 2 - 0.05, bore - 0.06 + 0.07, bz - 0.04), (-w / 2 - 0.05, bore - 0.06 + 0.07, bz + 0.04)], 0.006, segs=8), 'steel')
            for i in range(6):   # nastro di cartucce
                B.add(translate(lathe([(-0.014, 0.0065), (0.022, 0.0065), (0.03, 0.004)], 'z', 8), (-w / 2 - 0.004 + i * 0.0, bore - 0.0 - i * 0.012, bz + 0.04 - i * 0.005)), 'brass')
        bipod(B, lay.front + 0.12, bore, r, m)
    elif kind == 'bolt':
        box_mag(B, st, mz, yb + 0.004, 0.05, 0.0, 0.034, 0.07)
    elif kind != 'shotgun':
        if st == 'k' and kind == 'auto':
            B.add(box((0, yb - 0.012, mz), (0.03, 0.03, 0.05), 0.003), 'steel')
            drum(B, mz, yb - 0.075, 0.07, 0.06, 'steel')
        else:
            box_mag(B, st, mz, yb + 0.004, 0.16 if not big else 0.14, 0.04 if st == 'x' else 0.02, 0.038 if not big else 0.044, 0.064 if not big else 0.07)
    # impugnatura, ponticello, impugnatura anteriore
    pistol_grip(B, gR[2], gR[1], yb + 0.004, 'wood' if st == 'k' else ('rubber' if st == 'v' else 'wood_dark'), 0.038, 0.32, 0.11 + max(0, gR[1] - (yb - 0.06)) * 0.2)
    chunky_guard(B, gR[2], yb, m)
    if kind not in ('shotgun',):
        fy = bore - r
        if st == 'k':   # astina di noce sotto la camicia
            fz0, fz1 = gL[2] - 0.09, min(gL[2] + 0.09, rz1 + 0.01)
            fo = zprofile([(-0.026, fy - 0.03), (0.026, fy - 0.03), (0.03, fy - 0.01), (0.03, fy + 0.008), (-0.03, fy + 0.008), (-0.03, fy - 0.01)], fz0, fz1, 0.008)
            fo = boolean_bm(fo, [translate(cyl((0, 0, (fz0 + fz1) / 2), r * 1.02, fz1 - fz0 + 0.04, 'z', 22), (0, bore, 0))] +
                            [box((s * 0.03, fy - 0.012, z), (0.006, 0.01, 0.004)) for s in (1, -1) for z in np.arange(fz0 + 0.02, fz1 - 0.02, 0.012)])
            B.add(fo, 'wood')
        elif st == 'v':
            B.add(box((0, fy - 0.006, gL[2]), (0.034, 0.014, 0.06), 0.003), 'gunmetal')
            B.add(profile([(gL[2] - 0.02, fy - 0.012), (gL[2] + 0.024, fy - 0.012), (gL[2] + 0.032, min(gL[1] - 0.05, fy - 0.08)), (gL[2] - 0.006, min(gL[1] - 0.055, fy - 0.085))], -0.017, 0.017, 0.007, 2), 'rubber')
        else:
            B.add(tube([(0, fy - 0.004, gL[2]), (0, min(gL[1] - 0.05, fy - 0.08), gL[2] + 0.012)], 0.016, segs=12), 'scrap')
            B.add(box((0, fy - 0.003, gL[2]), (0.05, 0.01, 0.05), 0.001), 'scrap')
    # calcio
    if zr > rz0 + 0.06:
        stock(B, st, rz0 - 0.006, zr, bore, yb, rng)


# ---------------------------------------------------------------------------------------------- pistole
def hand_cannon(B, lay, st, rng, kind='auto'):
    """pistola da guerra: 'auto' (carrello massiccio e compensatore), 'energy', 'heavy' (revolver enorme), 'crude' (doppia canna di rottami)"""
    bore = B.bore = lay.bore
    gR = lay.gR
    zf, zr = lay.front, min(lay.rear, gR[2] + 0.07)
    m = METAL[st]
    if kind == 'heavy':
        cz = gR[2] - 0.075
        top = bore + 0.03; fb = bore - 0.03
        fr = profile([(cz - 0.05, fb - 0.01), (cz - 0.05, top - 0.004), (zr - 0.03, top + 0.004), (zr, top - 0.016), (zr, fb - 0.012), (gR[2] + 0.02, fb - 0.022)], -0.019, 0.019, 0.004)
        fr = boolean_bm(fr, [box((0, bore - 0.002, cz), (0.08, 0.07, 0.066))])
        B.add(fr, m)
        cy = lathe([(cz - 0.03, 0.026), (cz - 0.026, 0.031), (cz + 0.026, 0.031), (cz + 0.03, 0.026)], 'z', 24)
        cy = boolean_bm(cy, [cyl((0.02 * math.cos(a), 0.02 * math.sin(a), cz - 0.04), 0.0065, 0.024, 'z', 12) for a in np.linspace(0, 2 * math.pi, 6, endpoint=False)] +
                        [cyl((0.032 * math.cos(a), 0.032 * math.sin(a), cz), 0.006, 0.05, 'z', 10) for a in np.linspace(math.pi / 6, 2 * math.pi + math.pi / 6, 6, endpoint=False)])
        B.add(translate(cy, (0, bore, 0)), 'blued' if st != 'x' else 'scrap')
        # canna con sottocanna pieno, costola ventilata, porte
        b = zprofile(sec(0.036, bore - 0.034, bore + 0.018, 0.008, 0.006), zf, cz - 0.03, 0.003)
        b = boolean_bm(b, [cyl((0, bore, zf), 0.008, 0.05, 'z', 14)] + [cyl((0, bore + 0.018, zf + 0.014 + i * 0.014), 0.004, 0.03, 'y', 8) for i in range(3)] +
                       [box((s * 0.018, bore - 0.014, (zf + cz) / 2), (0.006, 0.014, (cz - zf) * 0.6)) for s in (1, -1)])
        B.add(b, m)
        B.add(zprofile(sec(0.012, bore + 0.018, bore + 0.026, 0.002), zf + 0.004, cz - 0.03, 0.001), m)
        B.add(profile([(zr - 0.02, top - 0.004), (zr + 0.004, top + 0.02), (zr + 0.018, top + 0.022), (zr + 0.006, top - 0.006)], -0.006, 0.006, 0.002), m)   # cane
        if st == 'v': hazard(B, 0.018, zf + 0.06, cz - 0.04, bore - 0.03, bore - 0.016, 4)
        fbg = fb
    elif kind == 'crude':
        cz = gR[2] - 0.06
        B.add(box((0, bore - 0.01, (zr + cz - 0.05) / 2), (0.05, 0.07, zr - cz + 0.06), 0.002, 1), 'scrap')
        slab(B, 0.025, [(cz - 0.03, bore - 0.04), (zr - 0.01, bore - 0.035), (zr, bore + 0.02), (cz - 0.02, bore + 0.024)], 0.004, 'scrap_red')
        bolts(B, [(0.029, y, z) for y in (bore - 0.03, bore + 0.015) for z in (cz - 0.02, zr - 0.02)], 1, 0.004, 'scrap')
        for dy in (0.012, -0.022):   # due canne di tubo
            p = lathe([(zf, 0.016), (zf + 0.008, 0.015), (cz - 0.05, 0.015)], 'z', 14)
            p = boolean_bm(p, [cyl((0, 0, zf), 0.01, 0.03, 'z', 12)])
            B.add(translate(p, (0, bore + dy, 0)), 'scrap')
        for z in (zf + 0.03, (zf + cz) / 2):
            B.add(box((0, bore - 0.005, z), (0.04, 0.07, 0.014), 0.001), 'scrap')
        B.add(profile([(zf + 0.03, bore - 0.04), (zf - 0.06, bore - 0.05), (zf + 0.03, bore - 0.062)], -0.003, 0.003, 0.0006), 'bare')
        top_spikes(B, cz - 0.02, zr - 0.02, bore + 0.025, 2, rng)
        fbg = bore - 0.045
    else:
        top = bore + 0.03; sb = bore - 0.012; fb = bore - 0.034
        w = 0.04 if kind != 'energy' else 0.044
        sl = zprofile(sec(w, sb, top, 0.012, 0.003), zf + 0.03, zr, 0.003)
        cuts = [box((0, top - 0.01, zr - 0.014 - i * 0.007), (w + 0.02, 0.026, 0.003)) for i in range(6)]
        cuts += [box((w / 2, bore + 0.008, (zf + zr) / 2 + 0.03), (0.016, 0.016, 0.045))]
        if kind == 'energy':
            cuts += [box((0, bore + 0.012, zf + 0.06 + i * 0.02), (w + 0.02, 0.012, 0.009)) for i in range(5)]
        sl = boolean_bm(sl, cuts)
        B.add(sl, m if kind != 'energy' else PLATE[st] if st != 'k' else 'paint_k')
        # compensatore con porte in alto
        cm = zprofile(sec(w + 0.002, sb - 0.004, top - 0.002, 0.012, 0.004), zf, zf + 0.032, 0.002)
        cm = boolean_bm(cm, [cyl((0, bore, zf), 0.008, 0.08, 'z', 14)] + [box((0, top, zf + 0.008 + i * 0.009), (w * 0.6, 0.03, 0.004)) for i in range(3)])
        B.add(cm, m)
        # telaio con slitta sotto la canna
        B.add(profile([(zf + 0.01, sb), (zr, sb), (zr, fb - 0.006), (gR[2] - 0.05, fb - 0.006), (gR[2] - 0.075, fb), (zf + 0.04, fb + 0.002), (zf + 0.01, sb - 0.012)], -w / 2 + 0.002, w / 2 - 0.002, 0.003), m)
        B.add(box((0, fb - 0.004, zf + 0.07), (w - 0.006, 0.012, 0.08), 0.002), m)
        for z in np.arange(zf + 0.04, zf + 0.11, 0.012):
            B.add(box((0, fb - 0.012, z), (w - 0.004, 0.006, 0.006), 0.001), m)
        B.add(profile([(zr - 0.006, top - 0.006), (zr + 0.012, top + 0.006), (zr + 0.016, top - 0.004), (zr + 0.004, top - 0.016)], -0.005, 0.005, 0.0015), m)
        B.add(box((0, top + 0.005, zr - 0.012), (0.022, 0.01, 0.01), 0.002), m)
        B.add(box((0, top + 0.005, zf + 0.04), (0.006, 0.01, 0.008), 0.001), m)
        if kind == 'energy':
            B.glow_color = lay.emissive_color() or (1.0, 0.22, 0.05)
            B.add(box((0, bore + 0.012, zf + 0.1), (w - 0.006, 0.01, 0.1)), 'glow')
            B.add(translate(cyl((0, 0, zf - 0.001), 0.007, 0.004, 'z', 14), (0, bore, 0)), 'glow')
            for s in (1, -1):
                B.add(tube([(s * w / 2, bore - 0.004, zr - 0.02), (s * (w / 2 + 0.008), bore - 0.02, (zr + zf) / 2), (s * w / 2, fb, zf + 0.05)], 0.0035, segs=8), 'rubber')
        if st == 'v': hazard(B, w / 2, zf + 0.04, zf + 0.12, fb - 0.002, sb - 0.004, 4)
        if st == 'x': top_spikes(B, zf + 0.05, zr - 0.03, top, 2, rng)
        fbg = fb
    pistol_grip(B, gR[2], gR[1], fbg, 'wood' if st == 'k' else ('rubber' if st == 'v' else 'wood_dark'), 0.036, 0.27, 0.11)
    chunky_guard(B, gR[2] + 0.004, fbg, m)
    if kind == 'energy':
        B.add(box((0, gR[1] - 0.045, gR[2] + 0.016), (0.04, 0.018, 0.058), 0.004), m)
        B.add(box((0.0205, gR[1] - 0.045, gR[2] + 0.016), (0.002, 0.008, 0.036), 0.0005), 'glow')
    else:
        B.add(box((0, gR[1] - 0.05, gR[2] + 0.016), (0.04, 0.012, 0.052), 0.004), m)


# ---------------------------------------------------------------------------------------------- energia
def war_energy(B, lay, st, rng, kind='impulse'):
    """armi a energia: 'impulse' (dissipatori a lamelle), 'discharge' (bobine di rame e isolatori, punte di scarica),
    'fusion' (camere gemelle e serbatoio), 'beam' (Vrepigh: rottami, cavi, bobine scoperte)"""
    bore = B.bore = lay.bore
    gR = lay.gR; gL = lay.gL if lay.gL is not None else gR + np.array([0, 0.05, -0.3])
    zf, zr = lay.front, lay.rear
    B.glow_color = lay.emissive_color() or ((1.0, 0.25, 0.06) if kind in ('impulse', 'fusion') else (0.35, 0.65, 1.0))
    big = st == 'x' or kind == 'beam'
    rz0 = gR[2] + 0.09
    rz1 = gR[2] - 0.26
    yt = bore + (0.055 if big else 0.05); yb = bore - (0.07 if big else 0.064)
    w = 0.074 if not big else 0.084
    m = receiver(B, st, rz1, rz0, yb, yt, w, rng, port=False)
    # finestre accese ai lati
    for s in (1, -1):
        B.add(box((s * (w / 2 - 0.001), bore - 0.012, rz1 + 0.08), (0.004, 0.02, 0.06), 0.001), 'glow')
    # capacitore a cilindro col vetro acceso sopra il castello
    cz0, cz1 = rz1 + 0.05, rz0 - 0.05
    ca = lathe([(cz0, 0.018), (cz0 + 0.006, 0.024), (cz1 - 0.006, 0.024), (cz1, 0.018)], 'z', 20)
    ca = boolean_bm(ca, [box((0, 0.024, (cz0 + cz1) / 2 + d), (0.02, 0.02, 0.03)) for d in (-0.035, 0.0, 0.035)])
    B.add(translate(ca, (0, yt + 0.02, 0)), 'brass' if st != 'x' else 'scrap')
    B.add(translate(cyl((0, 0, (cz0 + cz1) / 2), 0.019, cz1 - cz0 - 0.02, 'z', 16), (0, yt + 0.02, 0)), 'glow')
    for z in (cz0 + 0.02, cz1 - 0.02):
        B.add(box((0, yt + 0.004, z), (0.02, 0.012, 0.012), 0.002), m)
    sz1 = rz1 + 0.01
    if kind == 'impulse':
        r = (0.032 if not big else 0.04) + max(0.0, sz1 - zf - 0.35) * 0.05     # canne lunghe: dissipatori piu' grossi
        core = zprofile(sec(r * 1.4, bore - r * 0.7, bore + r * 0.7, r * 0.3, r * 0.3), zf + 0.04, sz1, 0.003)
        B.add(core, 'gunmetal' if st == 'v' else METAL[st])
        n = int((sz1 - zf - 0.08) / 0.016)
        for i in range(n):   # lamelle dei dissipatori
            z = zf + 0.07 + i * 0.016
            B.add(zprofile(sec(r * 2.3, bore - r * 1.05, bore + r * 1.05, r * 0.6, r * 0.6), z, z + 0.005, 0.0008, 1), METAL[st])
        B.add(box((0, bore - r * 0.75, (zf + 0.04 + sz1) / 2), (r * 0.6, 0.004, sz1 - zf - 0.05)), 'glow')
        em = lathe([(zf, r * 0.55), (zf + 0.008, r * 0.75), (zf + 0.045, r * 0.75), (zf + 0.05, r * 0.6)], 'z', 22)
        em = boolean_bm(em, [cyl((0, 0, zf), r * 0.42, 0.012, 'z', 18)])
        B.add(translate(em, (0, bore, 0)), 'brass' if st == 'v' else METAL[st])
        B.add(translate(cyl((0, 0, zf + 0.006), r * 0.42, 0.004, 'z', 18), (0, bore, 0)), 'glow')
    elif kind in ('discharge', 'beam'):
        r = 0.014 if not big else 0.018
        B.add(translate(lathe([(zf + 0.02, r), (sz1, r)], 'z', 14), (0, bore, 0)), 'copper')
        n = int((sz1 - zf - 0.06) / 0.05)
        for i in range(n + 1):   # isolatori di ceramica e anelli di rame
            z = zf + 0.05 + i * (sz1 - zf - 0.07) / max(n, 1)
            B.add(translate(lathe([(z - 0.01, r * 1.2), (z - 0.006, r * 2.6), (z - 0.002, r * 2.2), (z + 0.002, r * 2.6), (z + 0.006, r * 2.2), (z + 0.01, r * 1.2)], 'z', 22), (0, bore, 0)), 'ceramic')
            if i < n:
                B.add(helix((0, bore, z + (sz1 - zf - 0.07) / max(n, 1) / 2), r * 1.5, 0.005, (sz1 - zf - 0.07) / max(n, 1) - 0.026, 0.0024), 'copper')
        # punte di scarica a forca e sfera accesa
        for a in np.linspace(math.pi / 2, math.pi / 2 + 2 * math.pi, 3, endpoint=False):
            p0 = np.array([math.cos(a) * r * 1.5, bore + math.sin(a) * r * 1.5, zf + 0.05])
            p1 = np.array([math.cos(a) * r * 3.0, bore + math.sin(a) * r * 3.0, zf + 0.02])
            p2 = np.array([math.cos(a) * r * 1.4, bore + math.sin(a) * r * 1.4, zf - 0.005])
            B.add(tube([p0, p1, p2], 0.0032, 0.0018, segs=8), 'copper' if st != 'v' else 'brass')
        B.add(translate(lathe([(zf + 0.004, 0.0), (zf + 0.008, r * 0.9), (zf + 0.02, r * 0.9), (zf + 0.024, 0.0)], 'z', 16), (0, bore, 0)), 'glow')
        # cavi dal capacitore alla canna
        for s in (1, -1):
            B.add(tube([(s * 0.012, yt + 0.02, cz0 + 0.004), (s * 0.02, yt + 0.02, rz1 - 0.02), (s * r * 2.0, bore + r * 1.6, zf + 0.06)], 0.004, segs=8), 'rubber')
    else:   # fusione: due camere con anelli di raffreddamento
        for dx in (0.018, -0.018):
            B.add(translate(lathe([(zf, 0.013), (zf + 0.006, 0.016), (sz1, 0.016)], 'z', 16), (dx, bore, 0)), 'blued' if st != 'x' else 'scrap')
            B.add(translate(cyl((0, 0, zf + 0.003), 0.008, 0.004, 'z', 12), (dx, bore, 0)), 'glow')
        for z in np.arange(zf + 0.03, sz1 - 0.02, 0.03):
            B.add(zprofile(sec(0.072, bore - 0.028, bore + 0.028, 0.012, 0.012), z, z + 0.008, 0.002), METAL[st])
        B.add(box((0, bore - 0.03, (zf + sz1) / 2 + 0.02), (0.02, 0.012, sz1 - zf - 0.06)), 'glow')
        B.add(translate(cyl((0, 0, (rz1 + gL[2]) / 2), 0.024, abs(rz1 - gL[2]) + 0.06, 'z', 18, 0.006), (w / 2 + 0.02, bore - 0.03, 0)), 'brass')
    # cella sotto e impugnature
    cz = gR[2] - 0.1
    B.add(box((0, yb - 0.045, cz), (0.05 if not big else 0.07, 0.09, 0.07), 0.006, 2), METAL[st])
    B.add(box((0.026 if not big else 0.036, yb - 0.045, cz), (0.003, 0.06, 0.012)), 'glow')
    if st == 'v': hazard(B, 0.026, cz - 0.03, cz + 0.03, yb - 0.085, yb - 0.065, 3)
    pistol_grip(B, gR[2], gR[1], yb + 0.004, 'wood' if st == 'k' else ('rubber' if st == 'v' else 'wood_dark'), 0.038, 0.32, 0.11)
    chunky_guard(B, gR[2], yb, m)
    fy = bore - 0.035
    B.add(box((0, fy - 0.004, gL[2]), (0.04, 0.016, 0.07), 0.003), m)
    B.add(profile([(gL[2] - 0.02, fy - 0.01), (gL[2] + 0.024, fy - 0.01), (gL[2] + 0.03, min(gL[1] - 0.05, fy - 0.08)), (gL[2] - 0.008, min(gL[1] - 0.055, fy - 0.085))], -0.017, 0.017, 0.007, 2),
          'wood' if st == 'k' else 'rubber')
    if zr > rz0 + 0.06:
        stock(B, st, rz0 - 0.006, zr, bore, yb, rng)
