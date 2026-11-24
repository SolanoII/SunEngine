# Archetipi delle armi: ogni generatore costruisce un'arma vera (sagome, canne tornite, fori, rivetti) sulle misure
# e sui punti d'appoggio del suo modello a blocchi, cosi' mani e bocca della canna restano dove il gioco le aspetta.
#
# Stili per fazione:
#   'k' Regno (Kingship): armi da trincea, acciaio fosfatato, legno di noce, cuoio, ottone delle munizioni
#   'v' Vanguardia: pesanti e lavorate a macchina, piastre rosso sangue scurissimo, rifiniture d'ottone
#   'x' Vrepigh: armi di rottami, lamiere rivettate e arrugginite, tubi, filo avvolto, punte saldate
import math, random
import numpy as np
from kit import *


class Lay:
    """misure di un'arma dal suo modello a blocchi (spazio dell'arma: canna verso -z, alto +y)"""

    def __init__(self, j):
        a = j.get('anchors', {})
        P = j['parts']
        lo = np.array([1e9] * 3); hi = -lo
        for p in P:
            o = np.array(p['offset'], float); s = np.abs(np.array(p['size'], float)) / 2
            lo = np.minimum(lo, o - s); hi = np.maximum(hi, o + s)
        self.lo, self.hi = lo, hi
        self.gR = np.array(a.get('grip_right', [0, lo[1] + 0.02, (lo[2] + hi[2]) / 2]), float)
        self.gL = np.array(a['grip_left'], float) if 'grip_left' in a else None
        self.M = np.array(a['muzzle'], float) if 'muzzle' in a else np.array([0, (lo[1] + hi[1]) / 2, lo[2]])
        self.bore = float(self.M[1])
        self.front = float(self.M[2])
        self.rear = float(hi[2])
        self.L = self.rear - self.front
        self.parts = P

    def emissive_color(self):
        cols = [p['color'][:3] for p in self.parts if p.get('emissive', 0) > 0.05]
        if not cols: return None
        c = np.mean(cols, 0)
        return tuple(float(x) for x in c)


def R(rng, a, b):
    return rng.uniform(a, b)


# ==================================================================================================== pezzi comuni
def muzzle_brake(B, z, r, length, mat, slots=3):
    """freno di bocca tornito con feritoie laterali"""
    p = [(z, r * 0.62), (z, r), (z + length, r), (z + length, r * 0.85), (z + length + 0.004, r * 0.7)]
    bm = lathe([(t, rr) for t, rr in p], 'z', 18)
    cuts = []
    for i in range(slots):
        zz = z + length * (0.25 + 0.55 * i / max(slots - 1, 1))
        cuts.append(box((0, 0, zz), (r * 3, r * 0.55, length / (slots * 2.2))))
    bm = boolean_bm(bm, cuts)
    # foro della canna
    bm = boolean_bm(bm, [cyl((0, 0, z + length / 2), r * 0.38, length * 1.5, 'z', 12)])
    B.add(translate(bm, (0, B.bore, 0)), mat)


def barrel(B, z0, z1, r, mat, bore_hole=True):
    """canna da z0 (davanti) a z1 (dietro)"""
    bm = lathe([(z0, r * 0.98), (z0 + 0.003, r), (z1, r)], 'z', 18)
    if bore_hole: bm = boolean_bm(bm, [cyl((0, 0, z0), r * 0.42, 0.03, 'z', 12)])
    B.add(translate(bm, (0, B.bore, 0)), mat)


def iron_sight_front(B, z, h, mat):
    B.add(box((0, B.bore + h * 0.5, z), (0.004, h, 0.006), 0.0008), mat)
    B.add(box((0, B.bore + h * 0.25, z), (0.016, h * 0.5, 0.01), 0.0015), mat)


def trigger_group(B, zg, yg, mat_guard, grip_rake=0.3):
    """grilletto e ponticello davanti all'impugnatura"""
    zt = zg - 0.035; top = yg + 0.05
    pts = [(zt + 0.022, top), (zt + 0.022, top - 0.004), (zt - 0.03, top - 0.004), (zt - 0.036, top - 0.012),
           (zt - 0.034, top - 0.03), (zt - 0.02, top - 0.034), (zt + 0.022, top - 0.03), (zt + 0.026, top - 0.026),
           (zt - 0.018, top - 0.029), (zt - 0.028, top - 0.026), (zt - 0.029, top - 0.012), (zt - 0.025, top - 0.008), (zt + 0.022, top - 0.008)]
    B.add(profile(pts, -0.004, 0.004, 0.0012), mat_guard)
    B.add(profile([(zt + 0.002, top), (zt - 0.006, top), (zt - 0.01, top - 0.016), (zt - 0.004, top - 0.022), (zt - 0.002, top - 0.012)],
                  -0.0025, 0.0025, 0.0008), 'bare')


def pistol_grip(B, zg, yg, top_y, mat, width=0.03, rake=0.28, length=0.1, checker=False):
    """impugnatura a pistola inclinata all'indietro"""
    hb = length / 2
    ztop0, ztop1 = zg - 0.022 + rake * (top_y - yg) * 0.0, zg + 0.026
    bot = yg - hb + 0.012
    dz = rake * (top_y - bot)
    pts = [(zg - 0.024 - dz * 0.0, top_y), (zg + 0.028, top_y), (zg + 0.034 + dz * 0.55, (top_y + bot) / 2),
           (zg + 0.03 + dz, bot + 0.006), (zg + 0.022 + dz, bot), (zg - 0.02 + dz, bot), (zg - 0.026 + dz, bot + 0.008),
           (zg - 0.022 + dz * 0.45, (top_y + bot) / 2 + 0.008), (zg - 0.02, top_y - 0.012)]
    pts = [(z - dz * 0.5, y) for z, y in pts]
    bm = profile(pts, -width / 2, width / 2, width * 0.32, 3)
    B.add(bm, mat)


def sling_swivel(B, z, y, mat='steel'):
    B.add(lathe([(0, 0.006), (0.0015, 0.007), (0.003, 0.006)], 'x', 10, (0, y, z)), mat)
    B.add(tube([(0, y - 0.004, z - 0.008), (0, y - 0.012, z - 0.004), (0, y - 0.013, z + 0.004), (0, y - 0.004, z + 0.008)], 0.0018, segs=6), mat)


def rivets_line(B, pts, r, side=1, mat='steel'):
    for p in pts:
        B.add(rivet(p, r, (side, 0, 0)), mat)


def wire_wrap(B, c, r, length, turns, wire=0.0018, axis='z', mat='steel'):
    B.add(helix(c, r + wire * 0.6, length / turns, length, wire, axis), mat)


# ==================================================================================================== fucili
def rifle(B, lay, style, rng, kind='auto'):
    """fucile: 'auto' (automatico con caricatore curvo), 'bolt' (precisione, otturatore girevole, ottica),
    'shotgun' (a pompa), 'heavy' (proiettili pesanti, Vanguardia), 'crude' (rottami)"""
    B.bore = lay.bore
    gR, gL = lay.gR, lay.gL if lay.gL is not None else lay.gR + np.array([0, 0.04, -0.3])
    zf, zr = lay.front, lay.rear
    heavy = kind == 'heavy' or style == 'v'
    crude = style == 'x'
    wood = 'wood' if style == 'k' else ('wood_dark' if style == 'x' else None)
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    plate = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    bh = 0.012 if kind != 'shotgun' else 0.014                       # raggio della canna
    if heavy: bh = 0.016
    if crude: bh = 0.017

    # --- castello ---
    rz0 = gR[2] + (0.09 if heavy else 0.07)                           # dietro
    rz1 = min(gR[2] - 0.13, gL[2] + (0.12 if kind != 'shotgun' else 0.2))   # davanti
    if kind == 'bolt': rz1 = gR[2] - 0.17
    rtop = lay.bore + (0.032 if heavy else 0.024)
    rbot = lay.bore - (0.05 if heavy else 0.036)
    rw = 0.058 if heavy else (0.06 if crude else 0.046)
    if crude:
        rec = box((0, (rtop + rbot) / 2, (rz0 + rz1) / 2), (rw, rtop - rbot, rz0 - rz1), 0.002, 1)
    else:
        pts = [(rz1, rbot), (rz1, rtop - 0.008), (rz1 + 0.01, rtop), (rz0 - 0.03, rtop), (rz0 - 0.022, rtop + 0.006),
               (rz0 - 0.004, rtop + 0.006), (rz0, rtop - 0.004), (rz0, rbot + 0.004), (rz0 - 0.006, rbot)]
        rec = profile(pts, -rw / 2, rw / 2, 0.004 if not heavy else 0.006, 2)
    cuts = [box((rw / 2, lay.bore + 0.004, (rz0 + rz1) / 2 + 0.01), (0.03, 0.016, 0.055))]            # finestra d'espulsione
    if heavy:
        for i in range(4):                                                                              # alleggerimenti
            cuts.append(box((-rw / 2, rbot + 0.014, rz1 + 0.03 + i * 0.03), (0.012, 0.01, 0.018), 0.002))
    rec = boolean_bm(rec, cuts)
    B.add(rec, metal)
    if heavy:   # piastre di fazione sul fianco, coi rivetti
        for s in (1, -1):
            B.add(box((s * (rw / 2 + 0.002), (rtop + rbot) / 2 + 0.004, (rz0 + rz1) / 2 - 0.01), (0.004, (rtop - rbot) * 0.6, (rz0 - rz1) * 0.55), 0.0015), plate)
            rivets_line(B, [(s * (rw / 2 + 0.004), (rtop + rbot) / 2 + 0.004 + dy, (rz0 + rz1) / 2 - 0.01 + dz)
                            for dy in (-0.012, 0.014) for dz in (-(rz0 - rz1) * 0.22, (rz0 - rz1) * 0.22)], 0.0028, s, 'brass')
    if crude:   # lamiere saldate e rivettate, storte
        for s in (1, -1):
            pl = box((s * (rw / 2 + 0.003), (rtop + rbot) / 2 + R(rng, -0.004, 0.004), (rz0 + rz1) / 2 + R(rng, -0.02, 0.02)),
                     (0.004, (rtop - rbot) * R(rng, 0.7, 0.95), (rz0 - rz1) * R(rng, 0.45, 0.7)), 0.001, 1)
            transform(pl, Matrix.Rotation(R(rng, -0.08, 0.08), 4, 'X'))
            B.add(pl, 'scrap_red' if s > 0 else 'scrap')
            zs = np.linspace(rz1 + 0.02, rz0 - 0.02, 5)
            rivets_line(B, [(s * (rw / 2 + 0.006), rtop - 0.008, z) for z in zs] + [(s * (rw / 2 + 0.006), rbot + 0.008, z) for z in zs], 0.0035, s, 'scrap')
    # leva d'armamento / otturatore
    if kind == 'bolt':
        B.add(tube([(rw / 2, lay.bore + 0.006, rz0 - 0.03), (rw / 2 + 0.03, lay.bore - 0.012, rz0 - 0.03)], 0.004, segs=8), 'bare')
        B.add(lathe([(0, 0.008), (0.006, 0.009), (0.012, 0.0)], 'x', 12, (rw / 2 + 0.028, lay.bore - 0.012, rz0 - 0.03)), 'bare')
    else:
        B.add(box((rw / 2 + 0.006, lay.bore + 0.006, (rz0 + rz1) / 2 - 0.035), (0.012, 0.008, 0.012), 0.002), 'bare')

    # --- canna, gas, freno, mirini ---
    zb1 = rz1 + 0.01
    brake_len = 0.045 if kind != 'shotgun' else 0.0
    if crude: brake_len = 0.0
    barrel(B, zf + brake_len, zb1, bh, 'blued' if style != 'x' else 'scrap')
    if brake_len: muzzle_brake(B, zf, bh * 1.55, brake_len, metal, 3 if not heavy else 4)
    elif crude:   # bocca allargata a mano, con un anello saldato
        B.add(translate(lathe([(zf, bh * 1.3), (zf + 0.02, bh * 1.25), (zf + 0.024, bh * 1.05)], 'z', 16), (0, lay.bore, 0)), 'scrap')
    if kind in ('auto', 'heavy'):
        gz = gL[2] - 0.1
        B.add(translate(cyl((0, 0.018, gz), 0.0125, 0.03, 'z', 14, 0.002), (0, lay.bore, 0)), metal)                  # blocco del gas
        B.add(translate(lathe([(gz, 0.0065), (rz1, 0.0065)], 'z', 10), (0, lay.bore + 0.019, 0)), metal)             # tubo del gas
    if kind != 'bolt':
        iron_sight_front(B, zf + brake_len + 0.03, 0.03, metal)
        B.add(box((0, rtop + 0.008, rz0 - 0.03), (0.022, 0.016, 0.012), 0.002), metal)                              # tacca di mira
    if kind == 'shotgun':   # serbatoio tubolare sotto la canna e pompa di legno
        B.add(translate(lathe([(zf + 0.03, 0.011), (zf + 0.034, 0.012), (rz1, 0.012)], 'z', 16), (0, lay.bore - 0.026, 0)), metal)
        pz = gL[2]
        pump = lathe([(pz - 0.07, 0.016), (pz - 0.066, 0.02), (pz + 0.066, 0.02), (pz + 0.07, 0.016)], 'z', 20)
        pump = boolean_bm(pump, [box((0, 0, pz - 0.05 + i * 0.012), (0.06, 0.06, 0.004)) for i in range(9)] if False else [])
        B.add(translate(pump, (0, lay.bore - 0.026, 0)), wood or 'rubber')
        for i in range(8):
            B.add(translate(lathe([(pz - 0.05 + i * 0.014, 0.0205), (pz - 0.047 + i * 0.014, 0.0205)], 'z', 20), (0, lay.bore - 0.026, 0)), 'wood_dark')

    # --- guardamano ---
    if kind in ('auto', 'bolt', 'heavy'):
        hz0, hz1 = gL[2] - 0.11, min(gL[2] + 0.1, rz1 + 0.005)
        if kind == 'bolt': hz0, hz1 = zf + 0.25, rz1 + 0.005
        hb, ht = lay.bore - 0.034, lay.bore + 0.014
        if wood and not heavy:
            pts = [(hz0, hb + 0.01), (hz0 + 0.012, hb), (hz1, hb), (hz1, ht), (hz0 + 0.02, ht), (hz0, ht - 0.01)]
            hg = profile(pts, -0.024, 0.024, 0.008, 3)
            hg = boolean_bm(hg, [cyl((0, lay.bore, (hz0 + hz1) / 2), bh * 1.15, abs(hz1 - hz0) + 0.05, 'z', 14)])
            B.add(hg, wood)
            B.add(translate(lathe([(hz0 + 0.004, 0.026), (hz0 + 0.016, 0.026)], 'z', 18), (0, lay.bore - 0.01, 0)), metal)   # fascetta
        else:
            # guardamano a tubo forato (pesante) o lamiera ripiegata (rottami)
            sh = lathe([(hz0, 0.024), (hz1, 0.024)], 'z', 16)
            sh = boolean_bm(sh, [cyl((0, 0, (hz0 + hz1) / 2), 0.019, abs(hz1 - hz0) + 0.05, 'z', 16)] +
                            [cyl((0, 0, hz0 + 0.025 + i * 0.03), 0.007, 0.06, 'x', 8) for i in range(int(abs(hz1 - hz0) / 0.03) - 1)] +
                            [cyl((0, 0, hz0 + 0.04 + i * 0.03), 0.007, 0.06, 'y', 8) for i in range(int(abs(hz1 - hz0) / 0.03) - 1)])
            B.add(translate(sh, (0, lay.bore, 0)), metal if not crude else 'scrap')
        if heavy and lay.gL is not None and kind != 'bolt':   # impugnatura anteriore verticale
            fy = lay.bore - 0.021
            B.add(box((0, fy - 0.004, gL[2]), (0.03, 0.012, 0.05), 0.003), metal)          # fascetta sul tubo
            B.add(profile([(gL[2] - 0.018, fy - 0.008), (gL[2] + 0.018, fy - 0.008), (gL[2] + 0.02, min(gL[1] - 0.05, fy - 0.06)), (gL[2] - 0.016, min(gL[1] - 0.055, fy - 0.065))], -0.015, 0.015, 0.006, 2), 'rubber')

    # --- impugnatura, ponticello, caricatore ---
    pistol_grip(B, gR[2], gR[1], rbot + 0.004, wood if (wood and kind != 'heavy') else ('rubber' if style == 'v' else 'wood_dark'),
                0.03 if not heavy else 0.034, 0.3, 0.1 + (gR[1] - (rbot - 0.05)) * 0.2)
    trigger_group(B, gR[2], gR[1], metal)
    if kind in ('auto', 'heavy'):
        mz = gR[2] - 0.075
        mh = 0.17 if kind == 'auto' else 0.13
        cur = 0.055 if kind == 'auto' else 0.02
        n = 9; pts_f, pts_b = [], []
        for i in range(n + 1):
            t = i / n
            y = rbot - mh * t
            off = -cur * t * t
            pts_f.append((mz - 0.024 + off, y)); pts_b.append((mz + 0.024 + off * 0.9, y))
        mag = profile(pts_f + pts_b[::-1], -0.014 if not heavy else -0.02, 0.014 if not heavy else 0.02, 0.003, 2)
        B.add(mag, 'steel' if style != 'x' else 'scrap')
        B.add(box((0, rbot - mh + 0.006, mz - cur + 0.0), (0.034 if not heavy else 0.046, 0.012, 0.056), 0.003), metal)   # fondello
    if kind == 'bolt':   # ottica lunga con anelli
        oz0, oz1 = rz1 - 0.06, rz0 - 0.02
        sc = lathe([(oz0, 0.019), (oz0 + 0.04, 0.019), (oz0 + 0.07, 0.012), (oz1 - 0.06, 0.012), (oz1 - 0.035, 0.016), (oz1, 0.016)], 'z', 20)
        sc = boolean_bm(sc, [cyl((0, 0, oz0), 0.015, 0.012, 'z', 16), cyl((0, 0, oz1), 0.012, 0.012, 'z', 16)])
        B.add(translate(sc, (0, rtop + 0.03, 0)), 'blued')
        for zz in (oz0 + 0.09, oz1 - 0.08):
            B.add(box((0, rtop + 0.012, zz), (0.016, 0.02, 0.014), 0.002), metal)
            B.add(translate(lathe([(zz - 0.007, 0.015), (zz + 0.007, 0.015)], 'z', 16), (0, rtop + 0.03, 0)), metal)
        B.add(translate(cyl((0.018, 0, (oz0 + oz1) / 2 + 0.02), 0.007, 0.014, 'x', 12), (0, rtop + 0.03, 0)), metal)       # torretta

    # --- calcio ---
    if zr > rz0 + 0.06:
        sb = lay.bore - (0.13 if kind != 'heavy' else 0.12)
        st = lay.bore + 0.008
        if wood and kind != 'heavy':
            pts = [(rz0 - 0.005, rbot + 0.006), (rz0 + 0.05, rbot - 0.004), (zr - 0.012, sb), (zr, sb + 0.008), (zr, st - 0.004),
                   (zr - 0.012, st), (rz0 + 0.07, st - 0.004), (rz0 + 0.03, rtop - 0.006), (rz0 - 0.005, rtop - 0.01)]
            if crude: pts = [(rz0 - 0.005, rbot + 0.004), (zr, sb + 0.01), (zr, st), (rz0 - 0.005, rtop - 0.008)]
            stock = profile(pts, -0.021, 0.021, 0.01 if not crude else 0.003, 3 if not crude else 1)
            B.add(stock, wood)
            B.add(box((0, (sb + st) / 2, zr + 0.003), (0.044, st - sb + 0.004, 0.008), 0.0025), metal)               # calciolo
            if crude:   # cinghie di cuoio che tengono insieme il calcio
                for zz in (rz0 + 0.05, zr - 0.07):
                    yb = sb + (st - sb) * 0.1 + (rbot - sb) * (1 - (zz - rz0) / (zr - rz0)) * 0.0
                    B.add(box((0, (rbot + st) / 2 - 0.02, zz), (0.048, (st - sb) * 0.9, 0.014), 0.002), 'leather')
        else:
            # calcio scheletrato d'acciaio con calciolo di gomma
            out = [(rz0 - 0.005, rbot + 0.004), (zr - 0.01, sb), (zr, sb + 0.006), (zr, st - 0.004), (zr - 0.01, st), (rz0 - 0.005, rtop - 0.008)]
            stock = profile(out, -0.018, 0.018, 0.005, 2)
            hole = profile([(rz0 + 0.05, rbot - 0.01), (zr - 0.04, sb + 0.025), (zr - 0.04, st - 0.02), (rz0 + 0.05, rtop - 0.02)], -0.05, 0.05)
            stock = boolean_bm(stock, [hole])
            B.add(stock, plate if style == 'v' else metal)
            B.add(box((0, (sb + st) / 2, zr + 0.004), (0.04, st - sb + 0.004, 0.012), 0.004), 'rubber')
    # baionetta (Regno) o punta saldata (Vrepigh)
    if kind in ('auto', 'bolt') and style == 'k':
        bz = zf + brake_len + 0.012
        B.add(box((0, lay.bore - 0.022, bz + 0.03), (0.012, 0.014, 0.05), 0.002), metal)
        B.add(profile([(bz + 0.006, lay.bore - 0.016), (bz - 0.17, lay.bore - 0.024), (bz + 0.006, lay.bore - 0.034)], -0.0035, 0.0035, 0.0008), 'bare')
    if crude:
        B.add(wedge((0, lay.bore - 0.03, zf + 0.04), (0.012, 0.012), (0, lay.bore - 0.04, zf - 0.09), (0, -0.05, -1)), 'bare')


# ==================================================================================================== pistole
def pistol(B, lay, style, rng, kind='auto'):
    """pistola: 'auto' (carrello), 'energy' (bobina e cella), 'heavy' (revolver pesante), 'crude' (tubo e lamiere)"""
    B.bore = lay.bore
    gR = lay.gR
    zf, zr = lay.front, min(lay.rear, gR[2] + 0.06)
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    top = lay.bore + 0.016
    fb = lay.bore - 0.018
    if kind == 'heavy':
        # revolver pesante: tamburo, canna con nervatura, cane
        cz = gR[2] - 0.065
        frame = profile([(cz - 0.04, fb - 0.012), (cz - 0.04, top - 0.006), (zr - 0.02, top + 0.004), (zr, top - 0.01), (zr, fb - 0.006), (gR[2] + 0.02, fb - 0.02)],
                        -0.014, 0.014, 0.003)
        frame = boolean_bm(frame, [box((0, lay.bore - 0.004, cz), (0.06, 0.05, 0.052))])
        B.add(frame, metal)
        drum = lathe([(cz - 0.024, 0.02), (cz - 0.02, 0.023), (cz + 0.02, 0.023), (cz + 0.024, 0.02)], 'z', 18)
        drum = boolean_bm(drum, [cyl((0.016 * math.cos(a), 0.016 * math.sin(a), cz - 0.03), 0.0045, 0.02, 'z', 10) for a in np.linspace(0, 2 * math.pi, 6, endpoint=False)] +
                          [box((0.023 * math.cos(a), 0.023 * math.sin(a), cz), (0.006, 0.006, 0.028)) for a in np.linspace(0.5, 2 * math.pi + 0.5, 6, endpoint=False)])
        B.add(translate(drum, (0, lay.bore - 0.004, 0)), 'blued')
        barrel(B, zf, cz - 0.03, 0.009, metal)
        B.add(box((0, lay.bore + 0.011, (zf + cz) / 2), (0.008, 0.008, cz - zf - 0.03), 0.002), metal)                     # nervatura
        B.add(profile([(zr - 0.012, top), (zr + 0.008, top + 0.02), (zr + 0.016, top + 0.018), (zr + 0.004, top - 0.004)], -0.004, 0.004, 0.001), 'bare')   # cane
    elif kind == 'crude':
        cz = gR[2] - 0.05
        B.add(box((0, lay.bore - 0.006, (zr + cz - 0.04) / 2), (0.034, 0.05, zr - cz + 0.05), 0.002, 1), 'scrap')
        B.add(box((0.019, lay.bore - 0.006, (zr + cz) / 2), (0.004, 0.044, 0.07), 0.0008, 1), 'scrap_red')
        rivets_line(B, [(0.022, lay.bore + 0.012, z) for z in (cz - 0.02, cz + 0.02, zr - 0.02)] + [(0.022, lay.bore - 0.024, z) for z in (cz - 0.02, cz + 0.02)], 0.003, 1, 'scrap')
        p = lathe([(zf, 0.014), (zf + 0.006, 0.013), (cz - 0.04, 0.012)], 'z', 14)
        p = boolean_bm(p, [cyl((0, 0, zf), 0.008, 0.03, 'z', 12)])
        B.add(translate(p, (0, lay.bore, 0)), 'scrap')
        wire_wrap(B, (0, lay.bore, (zf + cz) / 2 - 0.02), 0.013, 0.04, 6, 0.0015, 'z', 'steel')
        # baionetta saldata: fascetta attorno alla canna e lama a cuneo che sporge oltre la bocca
        B.add(translate(lathe([(zf + 0.012, 0.0155), (zf + 0.03, 0.0155)], 'z', 14), (0, lay.bore, 0)), 'scrap')
        B.add(box((0, lay.bore - 0.017, zf + 0.022), (0.008, 0.008, 0.024), 0.001), 'scrap')
        B.add(wedge((0, lay.bore - 0.019, zf + 0.012), (0.004, 0.012), (0, lay.bore - 0.024, zf - 0.07), (0, -0.07, -1)), 'bare')
    else:
        # carrello corto con zigrinature; davanti la canna esce in una camicia forata (pistole lunghe da trincea)
        zs = zf if (kind == 'energy' or zr - zf < 0.26) else max(zf, gR[2] - 0.15)
        sl_pts = [(zs + 0.006, lay.bore - 0.012), (zs, lay.bore - 0.008), (zs, top - 0.004), (zs + 0.006, top), (zr - 0.008, top), (zr, top - 0.006), (zr, lay.bore - 0.012)]
        sl = profile(sl_pts, -0.013, 0.013, 0.003, 2)
        cuts = [box((0, top - 0.008, zr - 0.015 - i * 0.006), (0.04, 0.02, 0.0022)) for i in range(7)]
        cuts += [box((0, top - 0.008, zs + 0.012 + i * 0.006), (0.04, 0.02, 0.0022)) for i in range(4)]
        cuts.append(box((0.014, lay.bore + 0.004, (zs + zr) / 2 + 0.01), (0.012, 0.01, 0.035)))
        if kind == 'energy':
            cuts += [box((0, lay.bore + 0.002, zf + 0.025 + i * 0.016), (0.04, 0.006, 0.007)) for i in range(4)]   # feritoie di raffreddamento
        sl = boolean_bm(sl, cuts)
        B.add(sl, metal if kind != 'energy' else ('paint_k' if style == 'k' else 'paint_v'))
        B.add(profile([(zs + 0.01, lay.bore - 0.012), (zr, lay.bore - 0.012), (zr, fb - 0.004), (gR[2] - 0.05, fb - 0.004), (zs + 0.035, lay.bore - 0.026), (zs + 0.01, lay.bore - 0.024)],
                      -0.012, 0.012, 0.0025), metal)
        if zs > zf + 0.01:
            jk = lathe([(zf + 0.004, 0.0105), (zf + 0.012, 0.0125), (zs + 0.004, 0.0125)], 'z', 16)
            jk = boolean_bm(jk, [cyl((0, 0, zf + 0.025 + i * 0.022), 0.0045, 0.05, 'x', 8) for i in range(int((zs - zf - 0.03) / 0.022))] +
                            [cyl((0, 0, zf), 0.006, 0.03, 'z', 12)])
            B.add(translate(jk, (0, lay.bore, 0)), metal)
            B.add(translate(lathe([(zf + 0.006, 0.0135), (zf + 0.014, 0.0135)], 'z', 16), (0, lay.bore, 0)), metal)
        if kind == 'energy':
            col = lay.emissive_color() or (1.0, 0.2, 0.05)
            B.glow_color = col
            B.add(translate(cyl((0, 0, zf - 0.002), 0.006, 0.006, 'z', 14), (0, lay.bore, 0)), 'glow')
            B.add(helix((0, lay.bore, zf + 0.04), 0.0105, 0.004, 0.05, 0.0013), 'copper')
        else:
            B.add(translate(lathe([(zf - 0.004, 0.0065), (zf + 0.01, 0.0065)], 'z', 12), (0, lay.bore, 0)), 'blued')
        B.add(box((0, top + 0.004, zf + 0.008), (0.003, 0.006, 0.004), 0.0006), metal)
        B.add(box((0, top + 0.004, zr - 0.008), (0.016, 0.006, 0.005), 0.0008), metal)
    pistol_grip(B, gR[2], gR[1], fb, 'wood' if style == 'k' else ('rubber' if style == 'v' else 'wood_dark'), 0.03 if kind != 'heavy' else 0.032, 0.3, 0.095)
    trigger_group(B, gR[2] + 0.006, gR[1] - 0.002, metal)
    if kind == 'energy':   # cella d'energia nel calcio dell'impugnatura
        B.add(box((0, gR[1] - 0.036, gR[2] + 0.012), (0.033, 0.014, 0.05), 0.003), metal)
        B.add(box((0.0166, gR[1] - 0.036, gR[2] + 0.012), (0.002, 0.006, 0.03), 0.0005), 'glow')


# ==================================================================================================== armi a energia e pesanti
def energy_rifle(B, lay, style, rng, kind='impulse'):
    """fucile a energia: 'impulse' (camicia con alette di raffreddamento e lente), 'discharge' (bobine di rame
    scoperte), 'fusion' (due canne e serbatoio), 'beam' (Vrepigh: tubo di rottami pieno di cavi)"""
    B.bore = lay.bore
    gR = lay.gR; gL = lay.gL if lay.gL is not None else gR + np.array([0, 0.05, -0.32])
    zf, zr = lay.front, lay.rear
    col = lay.emissive_color() or ((1.0, 0.25, 0.08) if kind == 'impulse' else (0.35, 0.6, 1.0))
    B.glow_color = col
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    plate = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    big = style == 'x' or kind == 'beam'
    rz0, rz1 = gR[2] + 0.07, gR[2] - (0.16 if not big else 0.22)
    rtop, rbot = lay.bore + (0.03 if not big else 0.06), lay.bore - (0.04 if not big else 0.07)
    rw = 0.052 if not big else 0.1
    pts = [(rz1, rbot + 0.01), (rz1, rtop - 0.012), (rz1 + 0.02, rtop), (rz0 - 0.02, rtop), (rz0, rtop - 0.015), (rz0, rbot), (rz1 + 0.02, rbot)]
    rec = profile(pts, -rw / 2, rw / 2, 0.006 if not big else 0.008, 2)
    rec = boolean_bm(rec, [box((s * rw / 2, (rtop + rbot) / 2 + 0.004, (rz0 + rz1) / 2 + i * 0.022 - 0.03), (0.012, (rtop - rbot) * 0.45, 0.008)) for s in (1, -1) for i in range(4)])
    B.add(rec, plate if style != 'x' else 'scrap')
    # finestrelle accese dietro le feritoie
    B.add(box((0, (rtop + rbot) / 2 + 0.004, (rz0 + rz1) / 2 - 0.0), (rw - 0.008, (rtop - rbot) * 0.4, 0.1)), 'glow')
    # camicia della canna
    sz1 = rz1 + 0.01
    if kind in ('impulse', 'beam'):
        r = 0.022 if not big else 0.034
        sh = lathe([(zf + 0.02, r * 0.75), (zf + 0.03, r), (sz1, r)], 'z', 20)
        B.add(translate(sh, (0, lay.bore, 0)), metal)
        nf = int((sz1 - zf - 0.05) / 0.018)
        for i in range(nf):                                                                     # alette
            z = zf + 0.045 + i * 0.018
            B.add(translate(lathe([(z, r), (z + 0.002, r * 1.5), (z + 0.006, r * 1.5), (z + 0.008, r)], 'z', 20), (0, lay.bore, 0)), metal)
        B.add(translate(cyl((0, 0, zf + 0.02), r * 0.5, 0.008, 'z', 16), (0, lay.bore, 0)), 'glow')     # lente
        B.add(translate(lathe([(zf + 0.014, r * 0.8), (zf + 0.02, r * 0.85)], 'z', 18), (0, lay.bore, 0)), 'brass' if style == 'v' else metal)
        if kind == 'beam':   # cavi che corrono dalla cella alla bocca
            for s in (1, -1):
                B.add(tube([(s * r * 0.9, lay.bore + r * 0.6, zf + 0.06), (s * r * 1.4, lay.bore + r * 1.2, (zf + sz1) / 2), (s * rw * 0.45, rtop - 0.01, rz1 + 0.04)], 0.004, segs=8), 'rubber')
    elif kind == 'discharge':
        r = 0.016 if not big else 0.026
        B.add(translate(lathe([(zf, r * 0.7), (sz1, r * 0.7)], 'z', 14), (0, lay.bore, 0)), metal)
        B.add(helix((0, lay.bore, (zf + 0.04 + sz1) / 2), r * 1.15, 0.008, sz1 - zf - 0.06, 0.0028), 'copper')
        for z in (zf + 0.02, sz1 - 0.01, (zf + sz1) / 2):
            B.add(translate(lathe([(z - 0.006, r * 1.6), (z + 0.006, r * 1.6)], 'z', 18), (0, lay.bore, 0)), metal)
        for s in (1, -1):   # due guide sopra e sotto
            B.add(box((0, lay.bore + s * r * 1.6, (zf + sz1) / 2), (0.01, 0.006, sz1 - zf - 0.01), 0.0015), metal)
        B.add(translate(cyl((0, 0, zf + 0.005), r * 0.55, 0.006, 'z', 14), (0, lay.bore, 0)), 'glow')
    else:   # fusion: due canne sovrapposte
        for dy in (0.012, -0.014):
            B.add(translate(lathe([(zf + 0.01, 0.011), (sz1, 0.011)], 'z', 14), (0, lay.bore + dy, 0)), metal)
            B.add(translate(cyl((0, 0, zf + 0.008), 0.007, 0.006, 'z', 12), (0, lay.bore + dy, 0)), 'glow')
        B.add(translate(lathe([(zf + 0.02, 0.028), (zf + 0.04, 0.028), (zf + 0.045, 0.024)], 'z', 18), (0, lay.bore, 0)), metal)
        B.add(translate(cyl((0, 0.05, (rz0 + rz1) / 2), 0.02, 0.1, 'z', 16, 0.004), (rw * 0.55, lay.bore - 0.01, 0)), 'brass')   # serbatoio
    # cella d'energia sotto il castello
    cz = gR[2] - 0.075
    B.add(box((0, rbot - 0.04, cz), (0.044 if not big else 0.07, 0.08, 0.05), 0.004), metal)
    B.add(box((0.023 if not big else 0.036, rbot - 0.04, cz), (0.002, 0.05, 0.008)), 'glow')
    # impugnatura, ponticello, impugnatura anteriore, calcio
    pistol_grip(B, gR[2], gR[1], rbot + 0.006, 'rubber' if style != 'k' else 'wood', 0.032, 0.3, 0.1)
    trigger_group(B, gR[2], gR[1], metal)
    if lay.gL is not None:
        B.add(profile([(gL[2] - 0.02, rbot + 0.004), (gL[2] + 0.02, rbot + 0.004), (gL[2] + 0.02, gL[1] - 0.045), (gL[2] - 0.018, gL[1] - 0.05)], -0.014, 0.014, 0.006, 2), 'rubber')
        if gL[2] < rz1:   # sostegno sotto la camicia
            B.add(box((0, rbot + 0.006, (gL[2] + rz1) / 2), (0.03, 0.012, abs(rz1 - gL[2]) + 0.04), 0.003), metal)
    if zr > rz0 + 0.06:
        sb, st = lay.bore - 0.12, lay.bore + 0.01
        out = [(rz0 - 0.005, rbot + 0.004), (zr - 0.01, sb), (zr, sb + 0.006), (zr, st - 0.004), (zr - 0.01, st), (rz0 - 0.005, rtop - 0.01)]
        stock = profile(out, -0.02, 0.02, 0.006, 2)
        stock = boolean_bm(stock, [profile([(rz0 + 0.05, rbot - 0.012), (zr - 0.045, sb + 0.025), (zr - 0.045, st - 0.022), (rz0 + 0.05, rtop - 0.025)], -0.05, 0.05)])
        B.add(stock, 'wood' if style == 'k' else (plate if style == 'v' else 'scrap'))
        B.add(box((0, (sb + st) / 2, zr + 0.004), (0.042, st - sb + 0.004, 0.012), 0.004), 'rubber')
    if style == 'x':
        rivets_line(B, [(s * (rw / 2 + 0.002), y, z) for s in (1, -1) for y in (rtop - 0.01, rbot + 0.01) for z in np.linspace(rz1 + 0.02, rz0 - 0.02, 4)], 0.004, 1, 'scrap')


def heavy_gun(B, lay, style, rng, kind='mg'):
    """mitragliatrice: canna pesante in camicia forata, bipiede, maniglia, nastro o caricatore a tamburo"""
    rifle(B, lay, style, rng, 'heavy' if style == 'v' else ('crude' if style == 'x' else 'auto'))
    gR = lay.gR
    zf = lay.front
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    # camicia forata attorno alla canna
    z0, z1 = zf + 0.07, gR[2] - 0.2
    if z1 - z0 > 0.1:
        jk = lathe([(z0, 0.028), (z0 + 0.01, 0.03), (z1, 0.03)], 'z', 22)
        holes = [cyl((0, 0, z0 + 0.03 + i * 0.035), 0.008, 0.08, 'x', 10) for i in range(int((z1 - z0 - 0.04) / 0.035))]
        holes += [cyl((0, 0, z0 + 0.047 + i * 0.035), 0.008, 0.08, 'y', 10) for i in range(int((z1 - z0 - 0.06) / 0.035))]
        holes.append(cyl((0, 0, (z0 + z1) / 2), 0.025, z1 - z0 + 0.05, 'z', 18))
        B.add(translate(boolean_bm(jk, holes), (0, lay.bore, 0)), metal)
    # bipiede ripiegato sotto la canna
    for s in (1, -1):
        B.add(tube([(s * 0.012, lay.bore - 0.03, z0 + 0.02), (s * 0.018, lay.bore - 0.042, z0 + 0.2)], 0.004, segs=8), metal)
    # maniglia di trasporto
    hz = gR[2] - 0.17
    B.add(tube([(0, lay.bore + 0.035, hz + 0.05), (0, lay.bore + 0.075, hz + 0.03), (0, lay.bore + 0.075, hz - 0.05), (0, lay.bore + 0.035, hz - 0.07)], 0.006, segs=10), metal)
    # tamburo o cassetta delle munizioni a sinistra
    bz = gR[2] - 0.08
    if style == 'x':
        B.add(box((-0.06, lay.bore - 0.06, bz), (0.07, 0.1, 0.12), 0.004, 2), 'scrap_red')
        rivets_line(B, [(-0.096, lay.bore - 0.06 + dy, bz + dz) for dy in (-0.035, 0.035) for dz in (-0.045, 0.045)], 0.004, -1, 'scrap')
    else:
        B.add(translate(cyl((0, 0, 0), 0.055, 0.05, 'x', 26, 0.006), (-0.055, lay.bore - 0.06, bz)), metal)
        # nastro di cartucce d'ottone che entra nel castello
        for i in range(7):
            B.add(translate(lathe([(-0.01, 0.0055), (0.02, 0.0055), (0.026, 0.0035)], 'z', 8), (-0.03 + i * 0.0, lay.bore - 0.005 - i * 0.012, bz + 0.03 - i * 0.004)), 'brass')


def flamer(B, lay, style, rng):
    B.bore = lay.bore
    gR = lay.gR; gL = lay.gL if lay.gL is not None else gR + np.array([0, 0, -0.27])
    zf, zr = lay.front, lay.rear
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    B.glow_color = (1.0, 0.45, 0.1)
    rz0, rz1 = gR[2] + 0.08, gR[2] - 0.16
    import arch2
    arch2.receiver(B, style, rz1, rz0, lay.bore - 0.06, lay.bore + 0.045, 0.07, rng, port=False)
    if zr > rz0 + 0.06: arch2.stock(B, style, rz0 - 0.006, zr, lay.bore, lay.bore - 0.06, rng)
    # tubo d'erogazione con ugello e fiammella pilota
    B.add(translate(lathe([(zf + 0.04, 0.018), (rz1, 0.018)], 'z', 18), (0, lay.bore, 0)), metal)
    noz = lathe([(zf, 0.026), (zf + 0.014, 0.03), (zf + 0.06, 0.03), (zf + 0.066, 0.02)], 'z', 22)
    noz = boolean_bm(noz, [cyl((0, 0, zf), 0.016, 0.03, 'z', 16)] + [box((0, 0, zf + 0.03), (0.08, 0.008, 0.02))])
    B.add(translate(noz, (0, lay.bore, 0)), 'blued')
    B.add(translate(cyl((0, 0, zf + 0.02), 0.005, 0.03, 'z', 10), (0, lay.bore - 0.026, 0)), 'brass')
    B.add(translate(cyl((0, 0, zf + 0.003), 0.003, 0.004, 'z', 10), (0, lay.bore - 0.026, 0)), 'glow')
    # due serbatoi sotto, con valvola e tubo
    for i, s in enumerate((1, -1)):
        tz0, tz1 = gL[2] - 0.12, gR[2] - 0.05
        B.add(translate(lathe([(tz0, 0.016), (tz0 + 0.016, 0.034), (tz1 - 0.016, 0.034), (tz1, 0.016)], 'z', 22), (s * 0.036, lay.bore - 0.1, 0)), 'paint_k' if style == 'k' else ('paint_v' if style == 'v' else 'scrap_red'))
    B.add(translate(cyl((0, 0, 0), 0.012, 0.03, 'y', 14), (0, lay.bore - 0.035, gL[2] - 0.13)), 'brass')
    B.add(tube([(0, lay.bore - 0.035, gL[2] - 0.13), (0, lay.bore - 0.01, gL[2] - 0.2), (0, lay.bore - 0.003, zf + 0.1)], 0.005, segs=8), 'rubber')
    pistol_grip(B, gR[2], gR[1], lay.bore - 0.04, 'wood' if style == 'k' else 'rubber', 0.03, 0.3, 0.1)
    trigger_group(B, gR[2], gR[1], metal)
    B.add(profile([(gL[2] - 0.02, lay.bore - 0.075), (gL[2] + 0.02, lay.bore - 0.075), (gL[2] + 0.018, gL[1] - 0.04), (gL[2] - 0.018, gL[1] - 0.045)], -0.014, 0.014, 0.006), 'wood' if style == 'k' else 'rubber')
    # scudo termico forato sopra il tubo
    sh = box((0, lay.bore + 0.02, (zf + rz1) / 2 + 0.02), (0.034, 0.006, rz1 - zf - 0.08), 0.002)
    sh = boolean_bm(sh, [cyl((0, lay.bore + 0.02, zf + 0.08 + i * 0.03), 0.006, 0.03, 'y', 10) for i in range(int((rz1 - zf - 0.12) / 0.03))])
    B.add(sh, metal)


def launcher(B, lay, style, rng, kind='grenade'):
    """lanciagranate (tamburo girevole) o lanciarazzi (tubo con razzo in canna)"""
    B.bore = lay.bore
    gR = lay.gR; gL = lay.gL if lay.gL is not None else gR + np.array([0, 0, -0.3])
    zf, zr = lay.front, lay.rear
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    plate = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    if kind == 'rocket':
        r = 0.045
        tb = lathe([(zf, r + 0.006), (zf + 0.03, r + 0.006), (zf + 0.032, r), (zr - 0.03, r), (zr - 0.028, r + 0.008), (zr, r + 0.008)], 'z', 24)
        tb = boolean_bm(tb, [cyl((0, 0, (zf + zr) / 2), r - 0.004, zr - zf + 0.1, 'z', 22)])
        B.add(translate(tb, (0, lay.bore, 0)), plate)
        for z in np.linspace(zf + 0.12, zr - 0.12, 4):   # fasce rivettate
            B.add(translate(lathe([(z - 0.012, r + 0.003), (z + 0.012, r + 0.003)], 'z', 24), (0, lay.bore, 0)), metal)
        # testata del razzo che sporge, con punta e alette
        B.add(translate(lathe([(zf - 0.12, 0.0), (zf - 0.11, 0.012), (zf - 0.06, 0.036), (zf + 0.02, 0.038), (zf + 0.05, 0.03)], 'z', 20), (0, lay.bore, 0)), 'paint_k' if style == 'k' else 'scrap_red')
        B.add(translate(lathe([(zf - 0.12, 0.004), (zf - 0.13, 0.0)], 'z', 10), (0, lay.bore, 0)), 'brass')
        # mirino ribaltabile, impugnatura, appoggio spalla
        B.add(box((0.05, lay.bore + 0.04, gR[2] - 0.1), (0.004, 0.05, 0.03), 0.001), metal)
        pistol_grip(B, gR[2], gR[1], lay.bore - r + 0.004, 'wood_dark' if style != 'v' else 'rubber', 0.03, 0.25, 0.1)
        trigger_group(B, gR[2], gR[1], metal)
        B.add(profile([(gL[2] - 0.02, lay.bore - r), (gL[2] + 0.02, lay.bore - r), (gL[2] + 0.018, gL[1] - 0.045), (gL[2] - 0.018, gL[1] - 0.05)], -0.014, 0.014, 0.006), 'wood_dark' if style != 'v' else 'rubber')
        B.add(box((0, lay.bore - r - 0.01, (gR[2] + zr) / 2 + 0.04), (0.03, 0.03, 0.12), 0.006), 'leather')
        if style == 'x':
            rivets_line(B, [(r * math.cos(a), lay.bore + r * math.sin(a), z) for a in np.linspace(0, 2 * math.pi, 6, endpoint=False) for z in (zf + 0.15, zr - 0.15)], 0.004, 1, 'scrap')
        return
    # lanciagranate a tamburo
    import arch2
    arch2.war_rifle(B, lay, style, rng, 'shotgun')
    dz = gR[2] - 0.12
    drum = lathe([(dz - 0.05, 0.048), (dz - 0.044, 0.054), (dz + 0.044, 0.054), (dz + 0.05, 0.048)], 'z', 28)
    drum = boolean_bm(drum, [cyl((0.032 * math.cos(a), 0.032 * math.sin(a), dz - 0.06), 0.014, 0.03, 'z', 14) for a in np.linspace(0, 2 * math.pi, 6, endpoint=False)] +
                      [box((0.054 * math.cos(a), 0.054 * math.sin(a), dz), (0.012, 0.012, 0.07)) for a in np.linspace(0.52, 2 * math.pi + 0.52, 6, endpoint=False)])
    B.add(translate(drum, (0, lay.bore - 0.03, 0)), metal)
    B.add(translate(lathe([(lay.front, 0.024), (dz - 0.05, 0.024)], 'z', 20), (0, lay.bore - 0.005, 0)), metal)


def blunderbuss(B, lay, style, rng):
    B.bore = lay.bore
    gR = lay.gR; zf, zr = lay.front, lay.rear
    lock_z = gR[2] - 0.06
    bell = lathe([(zf, 0.05), (zf + 0.006, 0.052), (zf + 0.02, 0.044), (zf + 0.09, 0.024), (zf + 0.16, 0.019), (lock_z, 0.019)], 'z', 26)
    bell = boolean_bm(bell, [lathe([(zf - 0.01, 0.046), (zf + 0.02, 0.04), (zf + 0.09, 0.016), (lock_z + 0.01, 0.012)], 'z', 22)])
    B.add(translate(bell, (0, lay.bore, 0)), 'brass')
    for z in (zf + 0.2, lock_z - 0.09):
        B.add(translate(lathe([(z - 0.008, 0.023), (z + 0.008, 0.023)], 'z', 20), (0, lay.bore, 0)), 'scrap')
    # cassa di legno con la piastra dell'acciarino e il cane
    pts = [(zf + 0.2, lay.bore - 0.012), (lock_z + 0.03, lay.bore - 0.03), (gR[2] + 0.04, lay.bore - 0.04), (zr - 0.01, lay.bore - 0.13),
           (zr, lay.bore - 0.12), (zr, lay.bore - 0.04), (gR[2] + 0.05, lay.bore - 0.006), (zf + 0.2, lay.bore + 0.004)]
    B.add(profile(pts, -0.024, 0.024, 0.009, 3), 'wood_dark')
    B.add(box((0.026, lay.bore - 0.012, lock_z), (0.004, 0.03, 0.07), 0.0015), 'scrap')
    B.add(profile([(lock_z + 0.03, lay.bore + 0.0), (lock_z + 0.012, lay.bore + 0.03), (lock_z + 0.002, lay.bore + 0.032), (lock_z + 0.016, lay.bore - 0.002)], 0.024, 0.032, 0.001), 'scrap')
    pistol_grip(B, gR[2], gR[1], lay.bore - 0.03, 'wood_dark', 0.03, 0.4, 0.09)
    trigger_group(B, gR[2], gR[1], 'scrap')
    B.add(box((0, lay.bore - 0.125, zr - 0.004), (0.05, 0.03, 0.008), 0.002), 'scrap')
    wire_wrap(B, (0, lay.bore, lock_z - 0.05), 0.02, 0.05, 8, 0.0016, 'z', 'steel')


# ==================================================================================================== armi bianche
def knife(B, lay, style, rng):
    """coltello da trincea: lama con sguscio, guardia, impugnatura di legno/cuoio, pomolo"""
    gR = lay.gR; tip = lay.M
    zg = gR[2]; y0 = gR[1]
    zb = zg - 0.065                                   # inizio della lama
    L = zb - tip[2]
    w = 0.026 if style != 'x' else 0.032
    pts = [(zb, y0 - w * 0.45), (zb - L * 0.85, y0 - w * 0.4), (tip[2], tip[1] - 0.002), (zb - L * 0.82, y0 + w * 0.32), (zb - L * 0.35, y0 + w * 0.55), (zb, y0 + w * 0.55)]
    if style == 'x':   # lama seghettata sul dorso
        back = [(zb - L * t, y0 + w * 0.55 + (0.006 if i % 2 else 0)) for i, t in enumerate(np.linspace(0.1, 0.6, 9))]
        pts = pts[:4] + back[::-1] + [(zb, y0 + w * 0.55)]
    bl = profile(pts, -0.0035, 0.0035, 0.0008, 1, 40)
    # il filo: si assottiglia in basso
    for v in bl.verts:
        if v.co.y < y0 - w * 0.1: v.co.x *= 0.25
    bl = boolean_bm(bl, [box((0, y0 + w * 0.15, zb - L * 0.4), (0.0035, 0.004, L * 0.6))]) if style != 'x' else bl
    B.add(bl, 'bare')
    B.add(box((0, y0 + 0.002, zb + 0.004), (0.014, w * 1.6, 0.008), 0.002), 'brass' if style == 'v' else ('steel' if style == 'k' else 'scrap'))
    B.add(lathe([(zb + 0.008, 0.011), (zb + 0.015, 0.0125), (zg + 0.04, 0.0125), (zg + 0.048, 0.011)], 'z', 14, (0, y0, 0)), 'wood' if style == 'k' else 'leather')
    for i in range(5):
        B.add(lathe([(zb + 0.02 + i * 0.016, 0.0128), (zb + 0.024 + i * 0.016, 0.0128)], 'z', 14, (0, y0, 0)), 'wood_dark')
    B.add(lathe([(zg + 0.048, 0.013), (zg + 0.056, 0.014), (zg + 0.064, 0.009), (zg + 0.068, 0.0)], 'z', 14, (0, y0, 0)), 'steel' if style != 'x' else 'scrap')
    if style == 'k':   # tirapugni
        B.add(tube([(0, y0 - 0.006, zb + 0.004), (0, y0 - 0.032, zb + 0.015), (0, y0 - 0.036, zg + 0.02), (0, y0 - 0.012, zg + 0.055)], 0.003, segs=8), 'steel')


def cleaver(B, lay, style, rng, heavy=False):
    """mannaia dei Vrepigh: lama enorme e tacche nel filo, dorso spesso con punte saldate, rivetti, manico avvolto"""
    gR = lay.gR; tip = lay.M
    zg = gR[2]; yh = gR[1]
    lo, hi = lay.lo, lay.hi
    zb = zg - (0.09 if not heavy else 0.12)
    ztip = tip[2]
    H = min(hi[1] - lo[1] - 0.04, 0.12 if not heavy else 0.16)       # altezza della lama: mannaia, non lastra
    ytop = yh + 0.028
    ybot = ytop - H
    th = 0.012 if not heavy else 0.016
    zm = (zb + ztip) / 2
    pts = [(zb, yh + 0.012), (zb, ytop - 0.006), (zb - 0.02, ytop), (ztip + 0.05, ytop + 0.006), (ztip + 0.015, ytop - 0.004), (ztip, ytop - 0.03),
           (ztip - 0.004, ybot + H * 0.45), (ztip + 0.02, ybot + 0.012), (ztip + 0.06, ybot), (zm, ybot + 0.004),
           (zb - 0.07, ybot + H * 0.3), (zb - 0.025, yh - 0.03), (zb, yh - 0.016)]
    bl = profile(pts, -th / 2, th / 2, 0.0015, 1, 40)
    # filo affilato in basso e davanti, tacche
    for v in bl.verts:
        t = np.clip((ybot + 0.035 - v.co.y) / 0.035, 0, 1)
        v.co.x *= 1.0 - 0.8 * t
    nicks = [wedge((0, ybot - 0.002, z), (0.03, 0.012), (0, ybot + rng.uniform(0.012, 0.02), z + 0.004), (0, 1, 0)) for z in rng.uniform(ztip + 0.06, zb - 0.1, 3)]
    holes = [cyl((0, ytop - 0.035, zb - 0.06 - i * 0.07), 0.008, 0.05, 'x', 12) for i in range(2)]
    bl = boolean_bm(bl, nicks + holes)
    B.add(bl, 'scrap')
    # dorso rinforzato
    B.add(box((0, ytop - 0.006, (zb + ztip) / 2 + 0.01), (th * 1.6, 0.014, zb - ztip - 0.04), 0.003), 'scrap')
    for z in np.linspace(ztip + 0.05, zb - 0.03, 4 if not heavy else 6):
        B.add(wedge((0, ytop + 0.001, z), (th * 1.2, 0.02), (0, ytop + rng.uniform(0.03, 0.05), z - 0.006), (0, 1, 0)), 'steel')
    rivets_line(B, [(s * th / 2, ytop - 0.012, z) for s in (1, -1) for z in np.linspace(ztip + 0.06, zb - 0.02, 5)], 0.0035, 1, 'steel')
    # codolo, guardia, manico avvolto, pomolo
    hz1 = lay.hi[2] - 0.02
    B.add(box((0, yh, zb + 0.01), (th * 2.6, 0.06 if not heavy else 0.08, 0.02), 0.004), 'scrap')
    B.add(lathe([(zb + 0.02, 0.015 if not heavy else 0.019), (hz1, 0.015 if not heavy else 0.019)], 'z', 14, (0, yh, 0)), 'wood_dark')
    wire_wrap(B, (0, yh, (zb + 0.02 + hz1) / 2), 0.015 if not heavy else 0.019, hz1 - zb - 0.03, 10 if not heavy else 16, 0.0022, 'z', 'leather')
    B.add(lathe([(hz1, 0.02), (hz1 + 0.01, 0.022), (hz1 + 0.02, 0.012)], 'z', 10, (0, yh, 0)), 'scrap')
    B.add(wedge((0, yh, hz1 + 0.018), (0.012, 0.012), (0, yh, hz1 + 0.045), (0, 0, 1)), 'steel')


def toothsword(B, lay, style, rng):
    """spada a denti meccanici: barra con catena di denti che corre attorno, carter del motore, guardia, manico"""
    gR = lay.gR; tip = lay.M
    yh = gR[1]
    zb = gR[2] - 0.11                           # attacco della barra al carter
    if lay.gL is not None and lay.gL[2] > gR[2]: zb = gR[2] - 0.11
    zt = tip[2]
    bh = 0.034 if style != 'x' else 0.04
    metal = {'k': 'steel', 'v': 'blued', 'x': 'scrap'}[style]
    plate = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    bar = profile([(zb, yh - bh), (zt + bh, yh - bh), (zt, yh), (zt + bh, yh + bh), (zb, yh + bh)], -0.006, 0.006, 0.0015, 2)
    bar = boolean_bm(bar, [cyl((0, yh, z), 0.006, 0.03, 'x', 10) for z in np.linspace(zt + 0.07, zb - 0.06, 6)])
    B.add(bar, metal)
    # denti: una fila sopra e una sotto, con la catena
    n = int((zb - zt - 0.03) / 0.024)
    for i in range(n):
        z = zt + 0.03 + i * 0.024
        for s in (1, -1):
            B.add(wedge((0, yh + s * (bh + 0.004), z), (0.01, 0.014), (0, yh + s * (bh + 0.018), z - 0.008), (0, s, -0.4)), 'bare')
            B.add(box((0, yh + s * (bh + 0.002), z + 0.008), (0.009, 0.006, 0.016), 0.001), 'steel')
    # carter del motore con scarichi, guardia, impugnature
    cz0, cz1 = zb + 0.12, zb - 0.005
    car = profile([(cz1, yh - 0.05), (cz1, yh + 0.05), (cz0 - 0.03, yh + 0.058), (cz0, yh + 0.035), (cz0, yh - 0.04), (cz0 - 0.02, yh - 0.055)], -0.026, 0.026, 0.006, 2)
    car = boolean_bm(car, [box((s * 0.026, yh + 0.01 - i * 0.014, (cz0 + cz1) / 2), (0.01, 0.006, 0.06)) for s in (1, -1) for i in range(4)])
    B.add(car, plate)
    for s in (1, -1):
        B.add(translate(lathe([(0, 0.006), (0.03, 0.006), (0.034, 0.0075)], 'z', 10), (s * 0.016, yh + 0.058, cz0 - 0.04)), metal)
    B.add(box((0, yh, cz0 + 0.006), (0.06, 0.13, 0.012), 0.004), 'brass' if style == 'v' else metal)
    hz1 = lay.hi[2] - 0.02
    B.add(lathe([(cz0 + 0.012, 0.016), (hz1, 0.016)], 'z', 14, (0, yh, 0)), 'leather' if style != 'v' else 'rubber')
    for z in np.linspace(cz0 + 0.03, hz1 - 0.02, int((hz1 - cz0) / 0.025)):
        B.add(lathe([(z, 0.0168), (z + 0.004, 0.0168)], 'z', 14, (0, yh, 0)), metal)
    B.add(lathe([(hz1, 0.02), (hz1 + 0.012, 0.022), (hz1 + 0.024, 0.0)], 'z', 14, (0, yh, 0)), 'brass' if style == 'v' else metal)
    B.add(box((0, yh - 0.03, cz0 + 0.03), (0.01, 0.024, 0.012), 0.002), 'bare')     # leva d'avviamento


def wrench(B, lay, style, rng):
    gR = lay.gR; tip = lay.M
    yh = gR[1]; zt = tip[2]; hz1 = lay.hi[2] - 0.01
    B.add(profile([(zt + 0.07, yh - 0.012), (hz1, yh - 0.01), (hz1, yh + 0.01), (zt + 0.07, yh + 0.012)], -0.006, 0.006, 0.002), 'scrap')
    head = profile([(zt, yh - 0.03), (zt + 0.075, yh - 0.03), (zt + 0.075, yh + 0.035), (zt, yh + 0.035)], -0.008, 0.008, 0.003)
    head = boolean_bm(head, [box((0, yh + 0.012, zt), (0.03, 0.024, 0.06))])
    B.add(head, 'scrap')
    B.add(cyl((0, yh - 0.008, zt + 0.04), 0.01, 0.022, 'x', 12, 0.002), 'steel')
    wire_wrap(B, (0, yh, hz1 - 0.06), 0.012, 0.09, 12, 0.0018, 'z', 'leather')


# ==================================================================================================== equipaggiamento
def grenade(B, lay, style, rng, kind='frag'):
    c = np.array([0, (lay.lo[1] + lay.hi[1]) / 2, (lay.lo[2] + lay.hi[2]) / 2])
    h = lay.hi[1] - lay.lo[1]
    if kind == 'stick':   # granata a manico coi chiodi saldati
        L = lay.hi[2] - lay.lo[2]
        B.add(lathe([(lay.lo[2], 0.0), (lay.lo[2] + 0.005, 0.03), (lay.lo[2] + 0.09, 0.032), (lay.lo[2] + 0.1, 0.014)], 'z', 18, (0, c[1], 0)), 'scrap')
        B.add(lathe([(lay.lo[2] + 0.1, 0.013), (lay.hi[2] - 0.01, 0.012), (lay.hi[2], 0.009)], 'z', 12, (0, c[1], 0)), 'wood')
        for a in np.linspace(0, 2 * math.pi, 9, endpoint=False):
            for z in (lay.lo[2] + 0.025, lay.lo[2] + 0.06):
                p = np.array([math.cos(a) * 0.031, c[1] + math.sin(a) * 0.031, z])
                B.add(tube([p, p + np.array([math.cos(a), math.sin(a), rng.uniform(-0.3, 0.3)]) * 0.02], 0.0016, 0.0008, segs=5), 'steel')
        B.add(tube([(0, c[1] - 0.009, lay.hi[2] - 0.004), (0, c[1] - 0.02, lay.hi[2] + 0.01)], 0.0012, segs=5), 'cloth')
        return
    r = min(lay.hi[0] - lay.lo[0], lay.hi[2] - lay.lo[2]) * 0.48
    y0 = lay.lo[1] + 0.004
    body = lathe([(y0, 0.0), (y0 + 0.004, r * 0.55), (y0 + h * 0.18, r * 0.95), (y0 + h * 0.45, r), (y0 + h * 0.68, r * 0.8), (y0 + h * 0.74, r * 0.45)], 'y', 20, (0, 0, c[2]))
    if kind == 'frag':   # corpo a pigna
        cuts = []
        for i in range(5):
            y = y0 + h * (0.1 + 0.12 * i)
            cuts.append(lathe([(y - 0.002, r * 1.2), (y + 0.002, r * 1.2)], 'y', 20, (0, 0, c[2])) if False else box((0, y, c[2]), (r * 3, 0.0035, r * 3)))
        for a in np.linspace(0, math.pi, 4, endpoint=False):
            cuts.append(transform(box((0, y0 + h * 0.35, c[2]), (0.0035, h * 0.6, r * 3)), Matrix.Translation((0, 0, c[2])) @ Matrix.Rotation(a, 4, 'Y') @ Matrix.Translation((0, 0, -c[2]))))
        body = boolean_bm(body, [b for b in cuts])
    B.add(body, 'paint_k' if style == 'k' else ('paint_v' if style == 'v' else 'scrap'))
    if kind == 'scrap':   # granata di rottami: legata col filo, chiodi
        for a_ in np.linspace(0, 2 * math.pi, 10, endpoint=False):     # chiodi infilati nella carica
            for yy in (y0 + h * 0.25, y0 + h * 0.5):
                p = np.array([math.cos(a_) * r * 0.95, yy, c[2] + math.sin(a_) * r * 0.95])
                d = np.array([math.cos(a_), rng.uniform(-0.3, 0.3), math.sin(a_)])
                B.add(tube([p - d * 0.004, p + d * 0.018], 0.0014, 0.0007, segs=5), 'steel')
        B.add(lathe([(y0 + h * 0.36, r * 1.02), (y0 + h * 0.4, r * 1.02)], 'y', 20, (0, 0, c[2])), 'steel')
    # spoletta, leva, anello
    yt = y0 + h * 0.74
    B.add(lathe([(yt, r * 0.42), (yt + h * 0.1, r * 0.38), (yt + h * 0.14, r * 0.3)], 'y', 14, (0, 0, c[2])), 'steel')
    B.add(profile([(c[2] - r * 0.35, yt + h * 0.13), (c[2] + r * 0.4, yt + h * 0.12), (c[2] + r * 0.55, yt - h * 0.3), (c[2] + r * 0.5, yt - h * 0.32), (c[2] + r * 0.33, yt + h * 0.08), (c[2] - r * 0.35, yt + h * 0.1)],
                  -0.006, 0.006, 0.0008), 'steel')
    ring = [(0.012 * math.cos(a) - r * 0.5, yt + h * 0.12 + 0.012 * math.sin(a) * 0.3, c[2] - r * 0.2 + 0.012 * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 14)]
    B.add(tube(ring, 0.0012, segs=5, closed_ends=False), 'bare')


def mine(B, lay, style, rng):
    c = (lay.lo + lay.hi) / 2; r = (lay.hi[0] - lay.lo[0]) * 0.48; h = lay.hi[1] - lay.lo[1]
    y0 = lay.lo[1]
    B.add(lathe([(y0, r * 0.9), (y0 + 0.006, r), (y0 + h * 0.6, r), (y0 + h * 0.75, r * 0.7), (y0 + h * 0.78, r * 0.3)], 'y', 28, (c[0], 0, c[2])), 'scrap')
    B.add(lathe([(y0 + h * 0.76, r * 0.28), (y0 + h, r * 0.2)], 'y', 16, (c[0], 0, c[2])), 'steel')
    for a in np.linspace(0, 2 * math.pi, 8, endpoint=False):
        B.add(rivet((c[0] + math.cos(a) * r * 0.85, y0 + h * 0.64, c[2] + math.sin(a) * r * 0.85), 0.005, (0, 1, 0)), 'steel')
    for a in np.linspace(0, 2 * math.pi, 3, endpoint=False):
        B.add(wedge((c[0] + math.cos(a) * r * 0.15, y0 + h, c[2] + math.sin(a) * r * 0.15), (0.006, 0.006), (c[0] + math.cos(a) * r * 0.15, y0 + h + 0.02, c[2] + math.sin(a) * r * 0.15), (0, 1, 0)), 'bare')


def crate(B, lay, style, rng, kind='ammo'):
    """casse e scatole: lamiera stampata con nervature, coperchio, maniglia, fermi"""
    c = (lay.lo + lay.hi) / 2; s = lay.hi - lay.lo
    mat = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    body = box(c, s * np.array([0.96, 0.92, 0.96]), min(s) * 0.05, 2)
    ribs = [box((c[0], c[1] + dy, c[2]), (s[0] * 1.1, s[1] * 0.04, s[2] * 0.7)) for dy in (-s[1] * 0.2, s[1] * 0.05)] if False else []
    B.add(body, mat)
    for dy in (-s[1] * 0.22, s[1] * 0.05):   # nervature stampate
        B.add(box((c[0], c[1] + dy, c[2]), (s[0] * 0.985, s[1] * 0.05, s[2] * 0.985), 0.002), mat)
    B.add(box((c[0], lay.hi[1] - s[1] * 0.06, c[2]), (s[0] * 1.0, s[1] * 0.1, s[2] * 1.0), min(s) * 0.03), 'steel' if style != 'x' else 'scrap')
    hx = s[0] * 0.25
    B.add(tube([(c[0] - hx, lay.hi[1], c[2]), (c[0] - hx, lay.hi[1] + 0.02, c[2]), (c[0] + hx, lay.hi[1] + 0.02, c[2]), (c[0] + hx, lay.hi[1], c[2])], 0.004, segs=8), 'steel')
    for sx in (-1, 1):
        B.add(box((c[0] + sx * s[0] * 0.3, lay.hi[1] - s[1] * 0.13, lay.lo[2] + 0.002), (s[0] * 0.08, s[1] * 0.12, 0.006), 0.001), 'steel')
    if kind == 'backpack':   # cinghie e bandoliera
        for sx in (-1, 1):
            B.add(box((c[0] + sx * s[0] * 0.28, c[1], lay.hi[2] + 0.004), (0.035, s[1] * 0.95, 0.008), 0.002), 'leather')
        for i in range(8):
            B.add(translate(lathe([(-0.012, 0.005), (0.018, 0.005), (0.024, 0.003)], 'z', 8), (c[0] + s[0] * 0.5 + 0.006, lay.hi[1] - 0.02 - i * 0.03, c[2] + 0.01 * (i % 2))), 'brass')


def kit_box(B, lay, style, rng, kind='medkit'):
    """medikit, radio, rilevatore, zaino energetico, mantello arrotolato, bomba da demolizione"""
    c = (lay.lo + lay.hi) / 2; s = lay.hi - lay.lo
    if kind == 'medkit':
        B.add(box(c, s * 0.95, min(s) * 0.25, 3), 'cloth')
        B.add(box((c[0], c[1] + s[1] * 0.2, c[2]), (s[0] * 0.97, s[1] * 0.12, s[2] * 0.97), min(s) * 0.05), 'leather')
        B.add(box((c[0] + s[0] * 0.47, c[1], c[2]), (0.004, s[1] * 0.4, s[2] * 0.5), 0.001), 'bone')
        return
    if kind == 'cloak':
        B.add(cyl(c, min(s[1], s[2]) * 0.45, s[0] * 0.95, 'x', 18, 0.01), 'cloth')
        for dx in (-s[0] * 0.3, s[0] * 0.3):
            B.add(cyl((c[0] + dx, c[1], c[2]), min(s[1], s[2]) * 0.47, 0.016, 'x', 18), 'leather')
        return
    if kind == 'bomb':   # cariche legate, detonatore, fili
        for i, dx in enumerate((-s[0] * 0.25, 0, s[0] * 0.25)):
            B.add(cyl((c[0] + dx, c[1] - s[1] * 0.1, c[2]), min(s[0] * 0.14, s[1] * 0.4), s[2] * 0.9, 'z', 14, 0.004), 'paint_k' if style == 'k' else 'scrap_red')
        for dz in (-s[2] * 0.3, s[2] * 0.3):
            cr = min(s[0] * 0.14, s[1] * 0.4)
            B.add(box((c[0], c[1] - s[1] * 0.1, c[2] + dz), (s[0] * 0.5 + 2 * cr + 0.008, 2 * cr + 0.008, 0.014), 0.003), 'leather')
        cr0 = min(s[0] * 0.14, s[1] * 0.4); yd = c[1] - s[1] * 0.1 + cr0 + s[1] * 0.12
        B.add(box((c[0], yd, c[2]), (s[0] * 0.4, s[1] * 0.24, s[2] * 0.3), 0.004), 'steel')
        B.add(cyl((c[0], yd + s[1] * 0.12 + 0.002, c[2]), 0.006, 0.008, 'y', 10), 'glow')
        B.glow_color = (1.0, 0.1, 0.05)
        for dx in (-s[0] * 0.25, s[0] * 0.25):
            B.add(tube([(c[0] + dx, c[1] + s[1] * 0.15, c[2]), (c[0] + dx * 0.5, c[1] + s[1] * 0.4, c[2] + 0.02), (c[0], c[1] + s[1] * 0.42, c[2])], 0.0018, segs=6), 'rubber')
        return
    mat = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}[style]
    B.add(box(c, s * 0.92, min(s) * 0.1, 2), mat)
    B.add(box((c[0], c[1], lay.hi[2] - 0.004), (s[0] * 0.8, s[1] * 0.7, 0.006), 0.002), 'steel')
    if kind == 'radio':
        B.add(cyl((c[0] - s[0] * 0.25, lay.hi[1] + 0.12, c[2]), 0.003, 0.26, 'y', 8), 'steel')
        B.add(cyl((c[0] - s[0] * 0.25, lay.hi[1] + 0.005, c[2]), 0.008, 0.012, 'y', 12), 'brass')
        for i in range(3):
            B.add(cyl((c[0] + s[0] * 0.2, c[1] + s[1] * 0.2 - i * 0.03, lay.lo[2] - 0.003), 0.008, 0.008, 'z', 12), 'rubber')
        B.add(box((c[0] - s[0] * 0.1, c[1] - s[1] * 0.25, lay.lo[2] - 0.002), (s[0] * 0.5, s[1] * 0.2, 0.004), 0.001), 'glow')
        B.glow_color = (1.0, 0.55, 0.15)
    elif kind == 'scanner':
        B.add(box((c[0], c[1] + s[1] * 0.1, lay.lo[2] - 0.002), (s[0] * 0.6, s[1] * 0.45, 0.004), 0.001), 'glow')
        B.glow_color = (0.4, 1.0, 0.5)
        B.add(cyl((c[0], c[1], lay.lo[2] - 0.01), min(s[0], s[1]) * 0.2, 0.02, 'z', 16, 0.003), 'brass')
    elif kind == 'power':
        for dx in (-s[0] * 0.2, s[0] * 0.2):
            B.add(cyl((c[0] + dx, c[1], c[2]), s[2] * 0.3, s[1] * 0.95, 'y', 16, 0.004), 'steel')
            B.add(box((c[0] + dx, c[1], c[2] - s[2] * 0.3), (0.006, s[1] * 0.6, 0.004)), 'glow')
        B.glow_color = (0.3, 0.6, 1.0)


def pike(B, lay, style, rng):
    """picca del condannato: asta lunga di frassino con legature di ferro, punta a foglia con le ali sotto (per fermare
    chi carica), puntale in fondo all'asta"""
    gR = lay.gR; tip = lay.M
    yh = gR[1]; zt = tip[2]; zb = lay.hi[2]
    r = 0.017
    zh = zt + 0.3                                    # fine della punta, inizio del cannone
    B.add(lathe([(zh + 0.06, r * 0.95), (zb - 0.08, r), (zb - 0.06, r * 0.9)], 'z', 14, (0, yh, 0)), 'wood_dark')
    for z in np.linspace(zh + 0.25, zb - 0.3, 5):   # legature di ferro e pelle per le mani
        B.add(lathe([(z - 0.012, r * 1.12), (z + 0.012, r * 1.12)], 'z', 14, (0, yh, 0)), 'steel')
    for zc in (gR[2], lay.gL[2] if lay.gL is not None else gR[2] - 0.5):
        wire_wrap(B, (0, yh, zc), r * 1.05, 0.14, 12, 0.0022, 'z', 'leather')
    # cannone della punta con chiodi, ali, lama a foglia
    B.add(lathe([(zh - 0.01, r * 0.8), (zh + 0.02, r * 1.25), (zh + 0.1, r * 1.15), (zh + 0.12, r * 1.05)], 'z', 14, (0, yh, 0)), 'steel')
    rivets_line(B, [(r * 1.2, yh, zh + 0.05), (-r * 1.2, yh, zh + 0.07)], 0.004, 1, 'steel')
    for s in (1, -1):   # ali di arresto
        B.add(profile([(zh + 0.0, yh + s * 0.012), (zh - 0.02, yh + s * 0.09), (zh - 0.04, yh + s * 0.1), (zh - 0.03, yh + s * 0.012)], -0.006, 0.006, 0.001, 1), 'steel')
    L = zh - zt
    blade = profile([(zh, yh - 0.016), (zh - L * 0.35, yh - 0.04), (zt, yh), (zh - L * 0.35, yh + 0.04), (zh, yh + 0.016)], -0.006, 0.006, 0.0008, 1, 30)
    for v in blade.verts:
        k = min(1.0, abs(v.co.y - yh) / 0.04)
        v.co.x *= 1.0 - 0.75 * k                    # filo sottile ai bordi, costola al centro
    B.add(blade, 'bare')
    B.add(lathe([(zb - 0.07, r * 0.95), (zb - 0.04, r * 1.05), (zb + 0.02, r * 0.4), (zb + 0.04, 0.0)], 'z', 12, (0, yh, 0)), 'steel')
