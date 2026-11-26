# Terza forgia: armi "vere". Ogni arma e' costruita come un'arma da fuoco reale, pezzo per pezzo, con le misure delle
# armi storiche a cui somiglia per ruolo (lunghezza totale, distanza impugnatura-calciolo, diametro della canna):
#
#   ruolo                      riferimento reale (lunghezza)                  qui
#   fucile automatico          M1918 BAR 1.19 m, Thompson M1928 0.85 m        1.00 m
#   fucile pesante             FG42 0.98 m, M1 Garand 1.10 m                  1.12 m
#   fucile a otturatore        SMLE 1.13 m, Mauser 98k 1.11 m                 1.15 m (precisione), 1.25 m (cecchino)
#   fucile a pompa da trincea  Winchester 1897 trench gun 1.00 m              1.00 m
#   mitragliatrice             MG 42 1.22 m (canna 0.53 m), Lewis 1.28 m      1.22 m
#   pistola automatica         M1911 0.216 m, Luger P08 0.22 m                0.23 m
#   revolver pesante           Webley Mk VI 0.286 m                           0.29 m
#   distanza impugnatura-calciolo (length of pull) ~0.33-0.36 m dal grilletto -> 0.30 m dal centro dell'impugnatura
#
# Le armi della Vanguardia sono per giganti corazzati: lunghe 1.15 volte e grosse 1.3 volte (calibri enormi).
# I Vrepigh usano armi di tubi e lamiere saldate. Le armi a energia sono macchine: celle, bobine, alette, cavi, quadranti.
# Dalle opere di riferimento (40k, Fallout, Judge Dredd, Doom, Trench Crusade) si prendono solo proprieta' generali
# (massa, funzione leggibile, usura, materiali veri), mai disegni riconoscibili.
#
# Spazio dell'arma come i modelli a blocchi: canna verso -z, alto +y, metri. Restano fissi gli appoggi delle mani;
# la bocca della canna si sposta dove la vuole la lunghezza vera (B.muzzle: la forgia la riscrive nel JSON).
import math
import numpy as np
import bmesh
from mathutils import Matrix, Vector
from kit import *
from arch import sling_swivel

SC = {'k': 1.0, 'v': 1.3, 'x': 1.15}            # grossezza delle sezioni
OALX = {'k': 1.0, 'v': 1.15, 'x': 1.02}         # lunghezza
MET = {'k': 'parkerized', 'v': 'gunmetal', 'x': 'scrap'}
BAR = {'k': 'blued', 'v': 'blued', 'x': 'steel'}
WOOD = {'k': 'wood', 'v': None, 'x': 'wood_dark'}
PAINT = {'k': 'paint_k', 'v': 'paint_v', 'x': 'scrap_red'}


# ==================================================================================================== utensili
def P(pts, w, bev=0.002, segs=2, x=0.0, angle=25.0):
    return profile(pts, x - w / 2, x + w / 2, bev, segs, angle)


def at_bore(bm, B):
    return translate(bm, (0, B.bore, 0))


def ring(z, r_out, r_in, length, segs=24, y=0.0, sy=1.0):
    """anello (fascetta) attorno all'asse z; sy schiaccia in altezza"""
    bm = lathe([(z - length / 2, r_out), (z + length / 2, r_out)], 'z', segs)
    if r_in > 0: bm = boolean_bm(bm, [cyl((0, 0, z), r_in, length * 2, 'z', segs)])
    transform(bm, Matrix.Diagonal((1, sy, 1, 1)))
    return translate(bm, (0, y, 0))


def strap(B, pts, w, t, mat):
    """cinghia piatta (larga w lungo x, spessa t) lungo una polilinea nel piano yz"""
    P_ = [np.array(p, float) for p in pts]
    n = len(P_)
    verts, faces = [], []
    for i in range(n):
        d = P_[min(i + 1, n - 1)] - P_[max(i - 1, 0)]; d /= np.linalg.norm(d) + 1e-9
        nn = np.array([0.0, -d[2], d[1]])         # normale nel piano yz
        for sx, sn in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            verts.append(tuple(P_[i] + np.array([sx * w / 2, 0, 0]) + nn * sn * t / 2))
    for i in range(n - 1):
        for k in range(4):
            a, b = i * 4 + k, i * 4 + (k + 1) % 4
            faces.append([a, b, b + 4, a + 4])
    faces.append([3, 2, 1, 0]); faces.append([(n - 1) * 4 + k for k in range(4)])
    bm = bmesh.new()
    vs = [bm.verts.new(v) for v in verts]
    for f in faces:
        try: bm.faces.new([vs[i] for i in f])
        except ValueError: pass
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    B.add(bm, mat)


def sag(p0, p1, depth, n=14):
    """punti di una cinghia che pende fra p0 e p1"""
    p0, p1 = np.array(p0, float), np.array(p1, float)
    return [tuple(p0 + (p1 - p0) * t - np.array([0, depth * math.sin(math.pi * t), 0])) for t in np.linspace(0, 1, n)]


def screw(B, c, r, side=1, mat='blued'):
    """vite a testa tonda con il taglio"""
    B.add(rivet(c, r, (side, 0, 0)), mat)
    B.add(box((c[0] + side * r * 0.45, c[1], c[2]), (r * 0.5, r * 0.32, r * 1.9)), 'blued')


def screws(B, pts, r, side=1, mat='blued'):
    for p in pts: screw(B, p, r, side, mat)


def band(B, z, y, rx, ry, length, mat, bolt=True):
    """fascetta d'acciaio che stringe guardamano e canna"""
    bm = lathe([(z - length / 2, 1.0), (z + length / 2, 1.0)], 'z', 28)
    transform(bm, Matrix.Diagonal((rx, ry, 1, 1)))
    bm = boolean_bm(bm, [transform(lathe([(z - length, 1.0), (z + length, 1.0)], 'z', 28), Matrix.Diagonal((rx - 0.0025, ry - 0.0025, 1, 1)))])
    B.add(translate(bm, (0, y, 0)), mat)
    if bolt: B.add(cyl((0, y - ry - 0.003, z), 0.0032, length * 0.9, 'z', 8), mat)


def trigger_guard(B, zt, top, S, mat, trig='bare'):
    """ponticello e grilletto; zt = grilletto, top = fondo del castello"""
    h, L = 0.026 * S, 0.06 * S
    o = [(zt + L * 0.45, top), (zt + L * 0.5, top - h * 0.4), (zt + L * 0.3, top - h), (zt - L * 0.45, top - h), (zt - L * 0.6, top - h * 0.6), (zt - L * 0.55, top)]
    i_ = [(zt + L * 0.3, top - 0.0001), (zt + L * 0.36, top - h * 0.4), (zt + L * 0.22, top - h + 0.004), (zt - L * 0.38, top - h + 0.004), (zt - L * 0.46, top - h * 0.6), (zt - L * 0.42, top - 0.0001)]
    g = P(o, 0.008 * S, 0.0012)
    g = boolean_bm(g, [P(i_, 0.03, 0.0)])
    B.add(g, mat)
    B.add(P([(zt + 0.004 * S, top), (zt - 0.004 * S, top), (zt - 0.008 * S, top - h * 0.5), (zt - 0.002 * S, top - h * 0.75), (zt + 0.001 * S, top - h * 0.45)], 0.005 * S, 0.0008), trig)


def grip(B, zg, yg, top, S, mat, rake=0.3, panels=None, grooves=True):
    """impugnatura a pistola inclinata; panels = materiale delle guancette (sui fianchi), grooves = scanalature davanti"""
    hgt = top - (yg - 0.055 * S)
    dz = rake * hgt
    wt, wb = 0.036 * S, 0.034 * S
    pts = [(zg - 0.018 * S - dz * 0.35, top), (zg + 0.024 * S - dz * 0.35, top), (zg + 0.03 * S + dz * 0.25, top - hgt * 0.55),
           (zg + 0.028 * S + dz * 0.65, top - hgt + 0.004), (zg + 0.018 * S + dz * 0.65, top - hgt), (zg - 0.02 * S + dz * 0.65, top - hgt),
           (zg - 0.026 * S + dz * 0.6, top - hgt + 0.006), (zg - 0.022 * S + dz * 0.1, top - hgt * 0.5), (zg - 0.022 * S - dz * 0.3, top - 0.01)]
    g = P(pts, 0.028 * S, 0.008 * S, 3)
    cuts = []
    if grooves:
        for k in range(4):
            t = 0.25 + k * 0.17
            zc = zg - 0.024 * S + dz * (t - 0.3); yc = top - hgt * t
            cuts.append(transform(box((0, 0, 0), (0.05, 0.004 * S, 0.008 * S)), Matrix.Translation((0, yc, zc)) @ Matrix.Rotation(-0.3, 4, 'X')))
    B.add(boolean_bm(g, cuts) if cuts else g, mat)
    if panels:
        inner = [(z * 0.8 + (zg + dz * 0.2) * 0.2, y * 0.82 + (top - hgt * 0.5) * 0.18) for z, y in pts]
        for s in (1, -1):
            pnl = P(inner, 0.004, 0.0015, 2, s * 0.0155 * S)
            B.add(pnl, panels)
            screw(B, (s * (0.0175 * S + 0.002), top - hgt * 0.45, zg + dz * 0.15), 0.0028 * S, s)


# ==================================================================================================== pezzi di canna
def barrel(B, z0, z1, rb, mat, taper=True):
    p = [(z0, rb * 0.96), (z0 + 0.003, rb)]
    if taper: p += [(z1 - (z1 - z0) * 0.35, rb * 1.1), (z1 - 0.05, rb * 1.25), (z1 - 0.045, rb * 1.42), (z1, rb * 1.42)]
    else: p += [(z1, rb)]
    bm = lathe(p, 'z', 24)
    bm = boolean_bm(bm, [cyl((0, 0, z0), rb * 0.45, 0.06, 'z', 16)])      # anima
    B.add(at_bore(bm, B), mat)


def front_sight(B, z, h, S, mat, hood=True):
    """mirino a lama con la sua base a fascetta e il tunnel di protezione"""
    B.add(at_bore(ring(z, 0.014 * S, 0.0, 0.018 * S, 20), B), mat)
    B.add(box((0, B.bore + 0.012 * S + h / 2, z), (0.0035 * S, h, 0.008 * S), 0.0006), mat)
    if hood:
        hd = lathe([(z - 0.011 * S, 0.012 * S), (z + 0.011 * S, 0.012 * S)], 'z', 20)
        hd = boolean_bm(hd, [cyl((0, 0, z), 0.0105 * S, 0.05, 'z', 20), box((0, -0.012 * S, z), (0.04, 0.02 * S, 0.05))])
        B.add(translate(hd, (0, B.bore + 0.012 * S + h * 0.45, 0)), mat)


def ladder_sight(B, z, top, S, mat):
    """alzo a ritto graduato (fucili del Regno)"""
    B.add(box((0, top + 0.003 * S, z), (0.022 * S, 0.006 * S, 0.04 * S), 0.0015), mat)
    for s in (1, -1): B.add(box((s * 0.009 * S, top + 0.009 * S, z + 0.012 * S), (0.003 * S, 0.008 * S, 0.012 * S), 0.0008), mat)
    leaf = box((0, 0, 0), (0.016 * S, 0.003 * S, 0.045 * S), 0.0006)
    leaf = boolean_bm(leaf, [box((0, 0, -0.008 * S), (0.008 * S, 0.01, 0.022 * S))])
    transform(leaf, Matrix.Translation((0, top + 0.012 * S, z + 0.012 * S)) @ Matrix.Rotation(0.35, 4, 'X') @ Matrix.Translation((0, 0, -0.022 * S)))
    B.add(leaf, mat)
    B.add(box((0, top + 0.014 * S, z - 0.002 * S), (0.02 * S, 0.006 * S, 0.006 * S), 0.0008), 'bare')


def rail(B, z0, z1, y, S, mat, w=0.021):
    """slitta a denti per ottiche"""
    w *= S
    B.add(box((0, y + 0.003 * S, (z0 + z1) / 2), (w, 0.006 * S, z1 - z0), 0.0008), mat)
    n = int((z1 - z0) / (0.01 * S))
    for i in range(n):
        B.add(box((0, y + 0.0075 * S, z0 + (i + 0.5) * (z1 - z0) / n), (w, 0.003 * S, (z1 - z0) / n * 0.55), 0.0005), mat)


def scope(B, z0, z1, y, S, mat, big=False):
    """cannocchiale: campane, tubo, torrette, anelli e lenti"""
    r = (0.0125 if not big else 0.016) * S
    ro, re = r * (1.75 if big else 1.55), r * 1.4
    p = [(z0, ro * 0.92), (z0 + 0.004, ro), (z0 + 0.05 * S, ro), (z0 + 0.08 * S, r), (z1 - 0.09 * S, r), (z1 - 0.06 * S, re), (z1 - 0.004, re), (z1, re * 0.9)]
    t = lathe(p, 'z', 32)
    t = boolean_bm(t, [cyl((0, 0, z0), ro * 0.82, 0.02, 'z', 28), cyl((0, 0, z1), re * 0.8, 0.02, 'z', 28)])
    B.add(translate(t, (0, y, 0)), mat)
    for z, rr in ((z0 + 0.004, ro * 0.82), (z1 - 0.004, re * 0.8)):
        B.add(cyl((0, y, z), rr, 0.002, 'z', 28), 'glass')
    zm = (z0 + z1) / 2 + 0.01 * S
    B.add(cyl((0, y + r + 0.008 * S, zm), r * 0.62, 0.016 * S, 'y', 18, 0.001), mat)          # torretta d'elevazione
    B.add(cyl((r + 0.008 * S, y, zm), r * 0.62, 0.016 * S, 'x', 18, 0.001), mat)              # deriva
    for k in range(10):
        a = k / 10 * 2 * math.pi
        B.add(box((math.cos(a) * r * 0.62, y + r + 0.009 * S, zm + math.sin(a) * r * 0.62), (0.0016, 0.012 * S, 0.0016)), mat)
    B.add(box((0, y, zm), (r * 2.25, r * 2.25, 0.022 * S), 0.002), mat)                       # sella
    for z in (z0 + 0.075 * S, z1 - 0.085 * S):                                                  # anelli
        B.add(translate(ring(z, r * 1.22, r, 0.012 * S, 24), (0, y, 0)), mat)
        B.add(box((0, (y + B.top) / 2, z), (r * 1.6, y - B.top, 0.012 * S), 0.0012), mat)
        for s in (1, -1): screw(B, (s * r * 0.95, y - r * 0.4, z), 0.0022 * S, s)


def red_dot(B, z, top, S, mat):
    """mirino olografico squadrato con vetro"""
    w, h, L = 0.036 * S, 0.032 * S, 0.05 * S
    hood = box((0, top + 0.004 * S + h / 2, z), (w, h, L), 0.003)
    hood = boolean_bm(hood, [box((0, top + 0.008 * S + h * 0.55, z), (w - 0.008 * S, h - 0.012 * S, L + 0.02))])
    B.add(hood, mat)
    B.add(box((0, top + 0.008 * S + h * 0.55, z - L * 0.2), (w - 0.008 * S, h - 0.012 * S, 0.002)), 'glass')
    B.add(box((w / 2 + 0.003, top + h * 0.6, z + L * 0.15), (0.006, 0.01 * S, 0.012 * S), 0.0012), mat)


def bipod(B, z, S, mat):
    """bipiede ripiegato sotto la canna, gambe verso la bocca"""
    y = B.bore - 0.018 * S
    B.add(box((0, y, z), (0.03 * S, 0.012 * S, 0.02 * S), 0.002), mat)
    for s in (1, -1):
        a = (s * 0.012 * S, y - 0.004, z - 0.004)
        b = (s * 0.016 * S, y - 0.014 * S, z - 0.24 * S)
        B.add(tube([a, b], 0.0045 * S, segs=10), mat)
        B.add(tube([(b[0], b[1], b[2] + 0.06 * S), (b[0], b[1], b[2] + 0.002)], 0.0058 * S, segs=10), 'bare')
        B.add(wedge((b[0], b[1] - 0.002, b[2] - 0.004), (0.012 * S, 0.006 * S), (b[0], b[1] - 0.004, b[2] - 0.022 * S), (0, 0, -1)), mat)


def bayonet(B, z_lug, S, mat):
    """baionetta da trincea sotto la canna, lama oltre la bocca"""
    y = B.bore - 0.018 * S
    L = 0.24 * S
    B.add(box((0, y, z_lug), (0.016 * S, 0.02 * S, 0.03 * S), 0.002), mat)                 # elsa
    B.add(ring(z_lug - 0.012 * S, 0.0105 * S, 0.0085 * S, 0.008 * S, 18, B.bore), mat)    # anello sulla canna
    bl = [(z_lug - 0.015 * S, y + 0.007 * S), (z_lug - 0.015 * S - L * 0.85, y + 0.006 * S), (z_lug - 0.015 * S - L, y - 0.002 * S),
          (z_lug - 0.015 * S - L * 0.8, y - 0.011 * S), (z_lug - 0.015 * S, y - 0.011 * S)]
    B.add(P(bl, 0.006 * S, 0.0012, 1), 'bare')
    B.add(P([(z_lug - 0.015 * S - L * 0.1, y + 0.0035 * S), (z_lug - 0.015 * S - L * 0.7, y + 0.003 * S), (z_lug - 0.015 * S - L * 0.7, y + 0.001 * S), (z_lug - 0.015 * S - L * 0.1, y + 0.001 * S)], 0.0075 * S, 0.0003, 1), 'steel')   # sguscio


# ==================================================================================================== bocche
def muzzle_k(B, zm, rb, S, mat):
    """compensatore a feritoie superiori (le vampe vanno in alto e tengono giu' la canna)"""
    L = 0.055 * S
    r = rb * 1.75
    bm = lathe([(zm, r * 0.9), (zm + 0.004, r), (zm + L, r), (zm + L + 0.01, rb * 1.1)], 'z', 24)
    cuts = [cyl((0, 0, zm), rb * 0.75, L * 2.2, 'z', 18)]
    for i in range(4):
        zc = zm + 0.01 * S + i * 0.011 * S
        cuts.append(box((0, r, zc), (r * 1.2, r * 1.1, 0.0045 * S)))
    B.add(at_bore(boolean_bm(bm, cuts), B), mat)
    return L


def muzzle_v(B, zm, rb, S, mat):
    """freno di bocca squadrato a camere (calibri enormi)"""
    L = 0.085 * S
    w, h = rb * 3.4, rb * 2.8
    bm = zbox_chamfer(w, h, zm, zm + L, rb * 0.5, B.bore)
    cuts = [cyl((0, B.bore, zm + L / 2), rb * 0.8, L * 2, 'z', 18)]
    for i in range(3):
        zc = zm + 0.014 * S + i * 0.024 * S
        cuts.append(box((0, B.bore, zc), (w * 1.5, h * 0.62, 0.011 * S)))
    for i in range(3):
        cuts.append(cyl((0, B.bore + h / 2, zm + 0.015 * S + i * 0.024 * S), rb * 0.28, 0.03, 'y', 10))
    B.add(boolean_bm(bm, cuts), mat)
    for s in (1, -1): screw(B, (s * w / 2, B.bore - h * 0.3, zm + L - 0.008 * S), 0.0025 * S, s, mat)
    return L


def muzzle_x(B, zm, rb, S, mat, rng):
    """bocca di tubo allargata, fascetta stringitubo"""
    L = 0.035 * S
    B.add(at_bore(lathe([(zm, rb * 1.55), (zm + 0.006, rb * 1.5), (zm + L, rb * 1.12)], 'z', 18), B), mat)
    hose_clamp(B, zm + L + 0.02, rb * 1.12, S)
    return L


def hose_clamp(B, z, r, S, y=None):
    y = B.bore if y is None else y
    B.add(translate(ring(z, r + 0.0018, r - 0.0005, 0.009 * S, 22), (0, y, 0)), 'steel')
    B.add(box((0, y + r + 0.004, z), (0.008, 0.008, 0.011 * S), 0.001), 'steel')
    B.add(cyl((0.006, y + r + 0.004, z), 0.0025, 0.012, 'x', 8), 'bare')


def zbox_chamfer(w, h, z0, z1, ch, y):
    """scatola lungo z con gli spigoli lunghi smussati"""
    hw, hh = w / 2, h / 2
    sec = [(-hw, -hh + ch), (-hw + ch, -hh), (hw - ch, -hh), (hw, -hh + ch), (hw, hh - ch), (hw - ch, hh), (-hw + ch, hh), (-hw, hh - ch)]
    bm = profile([(xx, yy) for xx, yy in sec], z0, z1, 0.0012, 1)
    # profile estrude lungo x: ruota perche' l'estrusione vada lungo z
    transform(bm, Matrix.Rotation(math.radians(-90), 4, 'Y'))
    # dopo la rotazione: (x, y, z) <- (-z, y, x): rimette la sezione al suo posto
    return translate(bm, (0, y, 0))


# ==================================================================================================== caricatori
def box_mag(B, z, top, S, mat, length, curve=0.12, thick=0.024, width=0.022):
    L, t = length * S, thick * S
    n = 6
    front, back = [], []
    for i in range(n + 1):
        u = i / n
        dz = curve * L * u * u
        y = top - L * u
        front.append((z - t / 2 - dz, y)); back.append((z + t / 2 - dz, y))
    pts = front + back[::-1]
    B.add(P(pts, width * S, 0.0025, 2), mat)
    for i in range(1, n):                                                  # costole di rinforzo
        u = i / n * 0.9
        dz = curve * L * u * u
        B.add(box((0, top - L * u, z - dz), (width * S + 0.002, 0.003 * S, t * 0.92), 0.0008), mat)
    dz = curve * L
    B.add(box((0, top - L - 0.004 * S, z - dz), (width * S + 0.004, 0.008 * S, t + 0.008 * S), 0.0018), mat)   # fondello
    return top - L


def drum_mag(B, z, top, S, mat, r=0.06):
    r *= S
    c = (0, top - r - 0.012 * S, z)
    d = cyl(c, r, 0.05 * S, 'x', 40, 0.004)
    d = boolean_bm(d, [cyl((0.025 * S, c[1], z), r * 0.75, 0.004, 'x', 40)])
    B.add(d, mat)
    B.add(cyl((0.026 * S, c[1], z), r * 0.18, 0.01 * S, 'x', 20, 0.001), 'bare')                # chiave di carica
    B.add(box((0.03 * S, c[1], z), (0.01 * S, 0.008 * S, r * 0.32), 0.001), 'bare')
    for k in range(12):
        a = k / 12 * 2 * math.pi
        B.add(box((0.0255 * S, c[1] + math.sin(a) * r * 0.86, z + math.cos(a) * r * 0.86), (0.003, 0.006 * S, 0.006 * S), 0.0008), mat)
    B.add(box((0, top - 0.008 * S, z), (0.026 * S, 0.02 * S, 0.03 * S), 0.002), mat)            # collo


def belt(B, start, end, S, n=10):
    """nastro di cartucce che pende dalla mitragliatrice nella cassetta"""
    pts = sag(start, end, 0.05 * S, n)
    for p in pts[:-1]:
        x, y, z = p
        B.add(cyl((x, y, z), 0.0052 * S, 0.06 * S, 'x', 10), 'brass')
        B.add(lathe([(x + 0.03 * S, 0.0052 * S), (x + 0.042 * S, 0.0035 * S), (x + 0.046 * S, 0.0)], 'x', 10, (0, y, z)), 'copper')
        B.add(box((x - 0.012 * S, y, z), (0.008 * S, 0.012 * S, 0.012 * S), 0.0008), 'steel')


# ==================================================================================================== fucili
SPEC = {   # lunghezza totale, castello, raggio canna, caricatore
    'auto':    (1.00, 0.20, 0.0105),
    'heavy':   (1.12, 0.22, 0.011),
    'bolt':    (1.15, 0.21, 0.0098),
    'sniper':  (1.25, 0.22, 0.0105),
    'shotgun': (1.00, 0.20, 0.0105),
    'mg':      (1.22, 0.30, 0.011),
    'crude':   (1.02, 0.20, 0.0115),
}


def rifle3(B, lay, st, rng, kind='auto', drum=False):
    S = SC[st]
    B.bore = bore = lay.bore
    gR = lay.gR
    gL = lay.gL if lay.gL is not None else gR + np.array([0, 0.05, -0.3])
    gz, gy = float(gR[2]), float(gR[1])
    oal, Lrec, rb = SPEC[kind]
    oal *= OALX[st]; Lrec *= S; rb *= S
    zb = gz + 0.30 * min(S, 1.15)                 # calciolo
    zm = zb - oal                                 # bocca
    B.muzzle = (0.0, bore, zm)
    met, bar, wood, paint = MET[st], BAR[st], WOOD[st], PAINT[st]
    rtop, rbot = bore + 0.022 * S, bore - 0.03 * S
    if kind == 'mg': rtop += 0.008 * S
    rw = (0.032 if st == 'k' else 0.036) * S
    zr0 = gz + 0.055 * S
    zr1 = min(zr0 - Lrec, float(gL[2]) + 0.06)
    B.top = rtop

    # ------------------------------------------------------------------ castello
    ch = 0.006 * S
    if st == 'x':
        pts = [(zr1, rbot + 0.002), (zr1 - 0.004, rtop - 0.004), (zr1 + 0.01, rtop + 0.002), (zr0 - 0.008, rtop), (zr0 + 0.004, rtop - 0.012), (zr0, rbot + 0.006), (zr1 + 0.02, rbot - 0.002)]
        rec = P(pts, rw, 0.0015, 1)
    else:
        pts = [(zr1, rbot + ch), (zr1, rtop - ch), (zr1 + ch, rtop), (zr0 - 0.012 * S, rtop), (zr0, rtop - 0.01 * S), (zr0, rbot), (zr1 + ch, rbot)]
        rec = P(pts, rw, 0.0035 * S, 2)
    zp = zr1 + Lrec * 0.52                        # finestra d'espulsione
    cuts = [box((rw / 2, bore + 0.006 * S, zp), (0.014 * S, 0.014 * S, 0.06 * S), 0.0015),
            box((rw / 2, bore - 0.008 * S, zr1 + Lrec * 0.45), (0.008 * S, 0.0045 * S, Lrec * 0.5))]      # feritoia della leva
    if st == 'v':
        for s in (1, -1):
            for i in range(3):
                cuts.append(box((s * rw / 2, rbot + 0.01 * S, zr1 + 0.03 * S + i * 0.035 * S), (0.006 * S, 0.008 * S, 0.026 * S), 0.0015))
    B.add(boolean_bm(rec, cuts), met)
    # otturatore che si vede dalla finestra
    B.add(box((rw / 2 - 0.004 * S, bore + 0.006 * S, zp), (0.006 * S, 0.012 * S, 0.056 * S), 0.0012), 'bare')
    B.add(box((rw / 2 - 0.001 * S, bore + 0.009 * S, zp + 0.012 * S), (0.003, 0.003 * S, 0.02 * S), 0.0005), 'bare')
    # deflettore dei bossoli dietro la finestra
    B.add(P([(zp + 0.03 * S, bore + 0.013 * S), (zp + 0.042 * S, bore + 0.013 * S), (zp + 0.042 * S, bore - 0.001 * S)], 0.006 * S, 0.0008, 1, rw / 2 + 0.003 * S), met)
    # leva d'armamento
    if kind in ('bolt', 'sniper'):
        zh = zr0 - 0.035 * S
        B.add(lathe([(zh - 0.035 * S, 0.0075 * S), (zh + 0.03 * S, 0.0075 * S)], 'z', 18, (0, bore + 0.004 * S, 0)), 'bare')   # corpo dell'otturatore
        B.add(tube([(rw / 2 - 0.002, bore + 0.004 * S, zh), (rw / 2 + 0.018 * S, bore - 0.006 * S, zh + 0.006 * S), (rw / 2 + 0.03 * S, bore - 0.02 * S, zh + 0.01 * S)], 0.0032 * S, segs=10), 'bare')
        B.add(lathe([(-0.009 * S, 0.0), (-0.007 * S, 0.006 * S), (0.0, 0.0085 * S), (0.007 * S, 0.006 * S), (0.009 * S, 0.0)], 'y', 16, (rw / 2 + 0.031 * S, bore - 0.026 * S, zh + 0.01 * S)), 'bare')
    elif kind != 'shotgun':
        zh = zr1 + Lrec * 0.66
        B.add(box((rw / 2 + 0.004 * S, bore - 0.008 * S, zh), (0.008 * S, 0.006 * S, 0.012 * S), 0.0012), 'bare')
        B.add(cyl((rw / 2 + 0.012 * S, bore - 0.008 * S, zh), 0.0065 * S, 0.014 * S, 'x', 16, 0.0015), 'bare')
    # viti e perni sui fianchi
    for s in (1, -1):
        screws(B, [(s * rw / 2, rbot + 0.008 * S, zr1 + Lrec * t) for t in (0.15, 0.85)], 0.0026 * S, s)
        B.add(cyl((s * rw / 2, rbot + 0.016 * S, zr0 - 0.02 * S), 0.0028 * S, 0.003, 'x', 12), 'bare')
    if st == 'v':   # targhetta del numero di serie e strisce di fazione
        B.add(box((-rw / 2 - 0.001, rtop - 0.014 * S, zr1 + Lrec * 0.5), (0.002, 0.01 * S, 0.05 * S), 0.0006), 'brass')
        for s in (1, -1):
            B.add(box((s * (rw / 2 + 0.0012), rbot + 0.006 * S, zr1 + Lrec * 0.3), (0.0024, 0.006 * S, Lrec * 0.45), 0.0005), paint)
    if st == 'x':   # lamiere saldate e chiodi
        for s in (1, -1):
            pl = box((s * (rw / 2 + 0.002), bore + rng.uniform(-0.006, 0.004), zr1 + Lrec * rng.uniform(0.35, 0.65)), (0.003, 0.03 * S, Lrec * rng.uniform(0.35, 0.55)), 0.0008, 1)
            B.add(pl, 'scrap_red' if s > 0 else 'scrap')
            for k in range(5):
                B.add(rivet((s * (rw / 2 + 0.0045), bore + rng.uniform(-0.012, 0.012) * S, zr1 + Lrec * rng.uniform(0.2, 0.8)), 0.0032 * S, (s, 0, 0)), 'scrap')

    # ------------------------------------------------------------------ canna, bocca, mirini
    if st == 'k': md = muzzle_k(B, zm, rb, S, met) if kind not in ('shotgun', 'bolt') else 0.0
    elif st == 'v': md = muzzle_v(B, zm, rb, S, met) if kind != 'shotgun' else 0.0
    else: md = muzzle_x(B, zm, rb, S, 'scrap', rng)
    if kind == 'bolt' and st == 'k':
        md = 0.0
        B.add(at_bore(lathe([(zm, rb * 0.95), (zm + 0.002, rb * 1.05), (zm + 0.02, rb * 1.05)], 'z', 22), B), bar)
    barrel(B, zm + md, zr1, rb, bar if st != 'x' else 'steel', taper=(st != 'x'))
    if st == 'x':
        for z in np.linspace(zm + md + 0.06, zr1 - 0.05, 3): hose_clamp(B, z, rb, S)

    zhg0 = max(float(gL[2]) - 0.22, zm + md + 0.16 * S)    # davanti del guardamano
    if st == 'v' and kind != 'mg': zhg0 = zm + md + 0.035 * S   # Vanguardia: la camicia copre la canna fino al freno

    # ------------------------------------------------------------------ guardamano / camicia
    if kind == 'mg':
        jr = rb * 2.4
        jk = lathe([(zhg0 - 0.04, jr * 0.85), (zhg0 - 0.03, jr), (zr1, jr)], 'z', 28)
        cuts = [cyl((0, 0, (zhg0 + zr1) / 2), jr - 0.0025, abs(zr1 - zhg0) + 0.2, 'z', 28)]
        n = int((zr1 - zhg0 + 0.02) / (0.026 * S))
        for i in range(n):
            zc = zhg0 - 0.015 + (i + 0.5) * (zr1 - zhg0 + 0.01) / n
            cuts.append(cyl((0, 0, zc), jr * 0.32, jr * 3, 'x', 12))
            cuts.append(cyl((0, 0, zc + 0.013 * S), jr * 0.32, jr * 3, 'y', 12))
        B.add(at_bore(boolean_bm(jk, cuts), B), met)
        # maniglia di trasporto sulla camicia
        hz = (zhg0 + zr1) / 2 + 0.03
        B.add(box((0, bore + jr + 0.004, hz), (0.014 * S, 0.008 * S, 0.02 * S), 0.0015), met)
        B.add(tube([(0, bore + jr + 0.006, hz - 0.004), (0.03 * S, bore + jr + 0.04 * S, hz - 0.02 * S), (0.03 * S, bore + jr + 0.045 * S, hz + 0.06 * S), (0.0, bore + jr + 0.008, hz + 0.06 * S)], 0.005 * S, segs=12), met)
        B.add(tube([(0.03 * S, bore + jr + 0.042 * S, hz - 0.01 * S), (0.03 * S, bore + jr + 0.046 * S, hz + 0.05 * S)], 0.009 * S, segs=14), wood or 'rubber')
        bipod(B, zhg0 - 0.02, S, met)
    elif st == 'v':
        hw, hh = 0.046 * S, 0.044 * S
        sh = zbox_chamfer(hw, hh, zhg0, zr1 + 0.004, 0.012 * S, bore - 0.004 * S)
        cuts = []
        n = int((zr1 - zhg0) / (0.03 * S))
        for i in range(n):
            zc = zhg0 + 0.02 * S + i * (zr1 - zhg0 - 0.03 * S) / max(n, 1)
            cuts.append(box((0, bore + 0.004 * S, zc), (hw * 1.5, 0.012 * S, 0.012 * S), 0.002))
        cuts.append(cyl((0, bore, (zhg0 + zr1) / 2), rb * 1.5, zr1 - zhg0 + 0.1, 'z', 20))
        B.add(boolean_bm(sh, cuts), met)
        # impugnatura anteriore verticale
        fz, fy = float(gL[2]), bore - 0.004 * S - hh / 2
        B.add(P([(fz - 0.018 * S, fy), (fz + 0.018 * S, fy), (fz + 0.022 * S, fy - 0.09 * S), (fz - 0.014 * S, fy - 0.09 * S)], 0.03 * S, 0.006 * S, 2), 'rubber')
        for k in range(4): B.add(box((0, fy - 0.025 * S - k * 0.018 * S, fz + 0.002 * S), (0.032 * S, 0.004 * S, 0.034 * S), 0.0012), met)
        if kind in ('heavy', 'sniper'): bipod(B, zhg0 + 0.03, S, met)
    elif st == 'k':
        if kind == 'shotgun':
            # scudo termico forato sopra la canna, serbatoio tubolare, pompa
            hs = lathe([(zhg0 - 0.02, rb * 2.0), (zr1, rb * 2.0)], 'z', 28)
            cuts = [cyl((0, 0, 0), rb * 1.85, 2, 'z', 28), box((0, -rb * 2.2, 0), (0.1, rb * 3, 2))]
            for i in range(int((zr1 - zhg0) / 0.022)):
                for a in (-0.7, 0.0, 0.7):
                    c = cyl((0, 0, 0), 0.0042, 0.06, 'y', 10)
                    transform(c, Matrix.Translation((0, 0, zhg0 + 0.004 + i * 0.022)) @ Matrix.Rotation(a, 4, 'Z'))
                    cuts.append(c)
            B.add(at_bore(boolean_bm(hs, cuts), B), met)
            ty = bore - rb - 0.011 * S
            B.add(lathe([(zm + 0.03, 0.009 * S), (zm + 0.034, 0.0105 * S), (zr1, 0.0105 * S)], 'z', 20, (0, ty, 0)), bar)
            pz = float(gL[2])
            pump = lathe([(pz - 0.08, 0.016 * S), (pz - 0.072, 0.02 * S), (pz + 0.072, 0.02 * S), (pz + 0.08, 0.016 * S)], 'z', 28)
            pump = boolean_bm(pump, [ring(pz - 0.06 + i * 0.012, 0.03, 0.0195 * S, 0.0045, 28) for i in range(11)])
            B.add(translate(pump, (0, ty, 0)), wood)
            bayonet(B, zm + 0.04, S, met)
        else:
            # guardamano di legno inferiore e superiore, con le fascette
            hb, ht = bore - 0.026 * S, bore + 0.006 * S
            lo = P([(zhg0, hb + 0.012), (zhg0 + 0.012, hb), (zr1 + 0.002, hb), (zr1 + 0.002, ht), (zhg0, ht)], 0.038 * S, 0.008 * S, 3)
            lo = boolean_bm(lo, [cyl((0, bore, (zhg0 + zr1) / 2), rb * 1.4, abs(zr1 - zhg0) + 0.1, 'z', 20)])
            B.add(lo, wood)
            up = P([(zhg0 + 0.05, bore + 0.004), (zhg0 + 0.06, bore + rb * 1.9), (zr1 - 0.004, bore + rb * 1.9), (zr1 - 0.004, bore + 0.004)], 0.03 * S, 0.006 * S, 3)
            up = boolean_bm(up, [cyl((0, bore, (zhg0 + zr1) / 2), rb * 1.25, abs(zr1 - zhg0) + 0.1, 'z', 20)] +
                            [box((0, bore + rb * 1.9, zhg0 + 0.08 + i * 0.03), (0.04, 0.008, 0.012), 0.002) for i in range(int((zr1 - zhg0 - 0.1) / 0.03))])
            B.add(up, wood)
            band(B, zhg0 + 0.016, bore - 0.01 * S, 0.021 * S, 0.02 * S, 0.012 * S, met)
            band(B, (zhg0 + zr1) / 2 + 0.03, bore - 0.01 * S, 0.021 * S, 0.02 * S, 0.01 * S, met)
            if kind == 'auto':     # impugnatura anteriore di legno, verticale
                fz, fy = float(gL[2]), hb
                B.add(P([(fz - 0.02, fy + 0.004), (fz + 0.02, fy + 0.004), (fz + 0.024, fy - 0.035), (fz + 0.018, fy - 0.085), (fz - 0.016, fy - 0.085), (fz - 0.02, fy - 0.04)], 0.032, 0.009, 3), wood)
                B.add(ring(fz + 0.004, 0.0175, 0.0, 0.008, 20, fy - 0.08), met)
                for k in range(5):
                    B.add(box((0, fy - 0.02 - k * 0.012, fz - 0.019), (0.034, 0.003, 0.004), 0.0006), 'wood_dark')
            if kind in ('auto', 'heavy'):
                bayonet(B, zm + md + 0.03, S, met)
            # cinghia di cuoio dalla fascetta al calcio
            sw0 = (0.0, bore - 0.034 * S, zhg0 + 0.02)
            sw1 = (0.0, rbot - 0.105, zb - 0.09)
            sling_swivel(B, sw0[2], sw0[1] + 0.006, met)
            strap(B, sag(sw0, sw1, 0.028, 16), 0.024, 0.0035, 'leather')
    else:   # rottami: tubo con straccio avvolto e filo
        B.add(at_bore(lathe([(zhg0, rb * 2.1), (zr1, rb * 2.1)], 'z', 14), B), 'scrap')
        B.add(at_bore(helix((0, 0, (zhg0 + zr1) / 2), rb * 2.25, 0.012, (zr1 - zhg0) * 0.7, 0.004, 'z', 8), B), 'cloth')
        for z in (zhg0 + 0.01, zr1 - 0.01): B.add(at_bore(helix((0, 0, z), rb * 2.4, 0.003, 0.012, 0.0012), B), 'steel')
        fz, fy = float(gL[2]), bore - rb * 2.1
        B.add(P([(fz - 0.018, fy + 0.004), (fz + 0.018, fy + 0.004), (fz + 0.02, fy - 0.08), (fz - 0.016, fy - 0.08)], 0.03, 0.002, 1), 'wood_dark')
        for k in range(3): B.add(rivet((0.016, fy - 0.02 - k * 0.022, fz), 0.003, (1, 0, 0)), 'scrap')

    # gas
    if kind in ('auto', 'heavy', 'mg') and st != 'x':
        gzz = zhg0 + 0.01 if kind != 'mg' else zm + md + 0.05
        B.add(at_bore(box((0, rb * 1.9, gzz), (rb * 2.4, rb * 2.2, 0.026 * S), 0.0025), B), met)
        B.add(at_bore(lathe([(gzz, 0.005 * S), (zr1, 0.005 * S)], 'z', 14, (0, rb * 2.2, 0)), B), met)
    # mirini
    if kind in ('sniper',) or (kind == 'bolt' and st != 'x'):
        if kind == 'bolt' and st == 'k': front_sight(B, zm + 0.02, 0.012, S, met)
        scope(B, zr1 + 0.02, zr0 + 0.02, rtop + 0.03 * S, S, met if st != 'k' else 'blued', big=(st == 'v'))
    elif st == 'v':
        rail(B, zr1 + 0.01, zr0 - 0.02, rtop, S, met)
        red_dot(B, (zr1 + zr0) / 2, rtop + 0.009 * S, S, met)
    elif st == 'k':
        if kind == 'mg':
            ladder_sight(B, zr1 + 0.03, rtop, S, met)
            front_sight(B, zhg0 - 0.03, 0.016, S, met)
        else:
            ladder_sight(B, zr0 - 0.05 * S, rtop, S, met)
            front_sight(B, zm + md + 0.018, 0.012 if kind != 'shotgun' else 0.004, S, met, hood=(kind != 'shotgun'))
    else:
        B.add(tube([(0, rtop, zr0 - 0.04), (0, rtop + 0.014, zr0 - 0.045), (0.004, rtop + 0.02, zr0 - 0.04)], 0.0018, segs=6), 'steel')   # chiodo piegato
        B.add(box((0, bore + rb + 0.006, zm + md + 0.02), (0.004, 0.012, 0.006), 0.0005), 'scrap')

    # ------------------------------------------------------------------ alimentazione
    zmag = zr1 + Lrec * 0.3
    if kind == 'mg':
        # coperchio d'alimentazione incernierato, nastro che entra da sinistra, cassetta appesa
        B.add(box((0, rtop + 0.006 * S, zr1 + Lrec * 0.42), (rw + 0.004, 0.012 * S, Lrec * 0.55), 0.003), met)
        B.add(cyl((0, rtop + 0.006 * S, zr1 + Lrec * 0.7), 0.0045 * S, rw + 0.01, 'x', 12), 'bare')
        B.add(box((-rw / 2 - 0.012 * S, bore + 0.002, zr1 + Lrec * 0.42), (0.024 * S, 0.016 * S, 0.06 * S), 0.002), met)   # finestra del nastro
        bx, by = -rw / 2 - 0.07 * S, rbot - 0.09 * S
        cb = box((bx, by, zr1 + Lrec * 0.4), (0.1 * S, 0.12 * S, 0.17 * S), 0.004)
        B.add(cb, met if st != 'k' else 'paint_k')
        B.add(box((bx, by + 0.062 * S, zr1 + Lrec * 0.4), (0.104 * S, 0.008 * S, 0.174 * S), 0.002), met)
        B.add(tube([(bx, by + 0.066 * S, zr1 + Lrec * 0.4 - 0.05 * S), (bx, by + 0.085 * S, zr1 + Lrec * 0.4 - 0.03 * S), (bx, by + 0.085 * S, zr1 + Lrec * 0.4 + 0.03 * S), (bx, by + 0.066 * S, zr1 + Lrec * 0.4 + 0.05 * S)], 0.004 * S, segs=8), met)
        B.add(box((bx + 0.051 * S, by + 0.03 * S, zr1 + Lrec * 0.4), (0.004, 0.024 * S, 0.03 * S), 0.001), 'bare')   # chiusura
        belt(B, (-rw / 2 - 0.02 * S, bore - 0.002, zr1 + Lrec * 0.42), (bx + 0.02 * S, by + 0.06 * S, zr1 + Lrec * 0.42), S, 9)
    elif kind == 'shotgun':
        if st == 'v': box_mag(B, zmag, rbot, S, met, 0.12, 0.0, 0.044, 0.04)
        else:   # finestra di caricamento e cane esterno
            B.add(box((0, rbot - 0.002, zr1 + Lrec * 0.4), (rw * 0.7, 0.006, Lrec * 0.4), 0.002), 'bare')
            B.add(P([(zr0 - 0.01, rtop - 0.008), (zr0 + 0.004, rtop + 0.006), (zr0 + 0.012, rtop + 0.008), (zr0 + 0.008, rtop - 0.004), (zr0 + 0.002, rtop - 0.014)], 0.007, 0.001, 1), 'bare')
    elif kind in ('bolt', 'sniper'):
        B.add(box((0, rbot - 0.006 * S, zmag), (rw * 0.9, 0.014 * S, 0.075 * S), 0.003), met)      # serbatoio interno
        if st == 'v': box_mag(B, zmag, rbot, S, met, 0.075, 0.0, 0.03, 0.026)
    elif drum:
        drum_mag(B, zmag, rbot, S, met)
    elif st == 'v':
        box_mag(B, zmag, rbot, S, met, 0.13, 0.04, 0.036, 0.03)
    elif st == 'k':
        box_mag(B, zmag, rbot, S, met, 0.13, 0.14, 0.036, 0.025)
    else:
        B.add(cyl((-rw / 2 - 0.045 * S, bore, zmag), 0.04 * S, 0.06 * S, 'y', 20, 0.003), 'scrap_red')      # barattolo laterale
        B.add(box((-rw / 2 - 0.01 * S, bore, zmag), (0.03 * S, 0.02 * S, 0.03 * S), 0.0015), 'scrap')
    if kind not in ('mg', 'shotgun', 'bolt', 'sniper') and not drum:
        B.add(box((rw / 2 + 0.002, rbot - 0.004, zmag + 0.03 * S), (0.004, 0.008 * S, 0.01 * S), 0.0008), 'bare')   # sgancio

    # ------------------------------------------------------------------ grilletto, impugnatura, calcio
    zt = gz - 0.045 * S
    trigger_guard(B, zt, rbot, S, met)
    gmat = {'k': wood, 'v': 'rubber', 'x': 'wood_dark'}[st]
    grip(B, gz, gy, rbot + 0.002, S, gmat, 0.28, panels=None, grooves=(st != 'x'))
    stock(B, st, kind, zr0, zb, rtop, rbot, gz, S, wood, met, rng)
    return B


def stock(B, st, kind, zr0, zb, rtop, rbot, gz, S, wood, met, rng):
    th = 0.14 * min(S, 1.15)          # altezza del calciolo
    if st == 'k':
        top0 = rtop - 0.004
        pts = [(zr0 - 0.004, top0), (zb - 0.02, top0 - 0.016), (zb, top0 - 0.02), (zb, top0 - 0.02 - th), (zb - 0.03, top0 - 0.02 - th + 0.006),
               (gz + 0.07, rbot - 0.03), (zr0 - 0.004, rbot)]
        s = P(pts, 0.044, 0.016, 4)
        B.add(s, wood)
        bp = P([(zb, top0 - 0.019), (zb + 0.008, top0 - 0.021), (zb + 0.008, top0 - 0.02 - th + 0.002), (zb, top0 - 0.02 - th + 0.002)], 0.042, 0.003, 2)
        B.add(bp, met)                                                                                          # calciolo d'acciaio
        for y in (top0 - 0.03, top0 - th + 0.006): B.add(rivet((0, y, zb + 0.008), 0.003, (0, 0, 1)), 'bare')
        sling_swivel(B, zb - 0.09, rbot - 0.098, met)
        # disco del reparto incassato nel calcio
        B.add(cyl((0.0215, top0 - 0.06, zb - 0.12), 0.012, 0.002, 'x', 24), 'brass')
    elif st == 'v':
        top0 = rtop - 0.004 * S
        pts = [(zr0 - 0.004, top0), (zb - 0.03 * S, top0 - 0.006 * S), (zb - 0.03 * S, top0 - th * 1.05), (gz + 0.08 * S, rbot - 0.025 * S), (zr0 - 0.004, rbot)]
        s = P(pts, 0.04 * S, 0.006 * S, 2)
        s = boolean_bm(s, [P([(zr0 + 0.05 * S, top0 - 0.022 * S), (zb - 0.06 * S, top0 - 0.026 * S), (zb - 0.06 * S, top0 - th * 0.72), (zr0 + 0.08 * S, top0 - th * 0.42)], 0.1, 0.0, 1)])
        B.add(s, met)
        B.add(P([(zb - 0.03 * S, top0 - 0.004 * S), (zb, top0 - 0.004 * S), (zb + 0.004, top0 - th * 0.5), (zb, top0 - th * 1.07), (zb - 0.03 * S, top0 - th * 1.07)], 0.042 * S, 0.008 * S, 3), 'rubber')
        B.add(box((0, top0 + 0.008 * S, zb - 0.1 * S), (0.034 * S, 0.016 * S, 0.11 * S), 0.004), 'rubber')         # appoggio per la guancia
        for s_ in (1, -1): screws(B, [(s_ * 0.0205 * S, top0 - 0.01 * S, zb - z) for z in (0.06 * S, 0.14 * S)], 0.0028 * S, s_)
    else:   # asse di legno inchiodata a un tubo
        B.add(lathe([(zr0 - 0.01, 0.009 * S), (zb - 0.02, 0.009 * S)], 'z', 12, (0, rtop - 0.02 * S, 0)), 'scrap')
        y0 = rtop - 0.02 * S
        pl = P([(zr0 + 0.03, y0 + 0.012), (zb - 0.02, y0 + 0.004), (zb - 0.02, y0 - 0.04), (zr0 + 0.03, y0 - 0.02)], 0.03 * S, 0.0015, 1)
        transform(pl, Matrix.Rotation(rng.uniform(-0.02, 0.02), 4, 'X'))
        B.add(pl, 'wood_dark')                                                                    # asse
        B.add(P([(zb - 0.024, y0 + 0.012), (zb + 0.004, y0 + 0.01), (zb + 0.006, y0 - th), (zb - 0.022, y0 - th + 0.004)], 0.04 * S, 0.0015, 1), 'wood_dark')   # calciolo
        B.add(tube([(0, y0 - th + 0.01, zb - 0.02), (0, rbot - 0.04, gz + 0.05)], 0.007 * S, segs=10), 'scrap')    # puntone
        for p_ in ((0, y0 - th + 0.01, zb - 0.02), (0, rbot - 0.04, gz + 0.05)): B.add(rivet((0.016 * S, p_[1], p_[2]), 0.004, (1, 0, 0)), 'scrap')
        for z in np.linspace(zr0 + 0.1, zb - 0.03, 4):
            for s_ in (1, -1): B.add(rivet((s_ * 0.0165 * S, rtop - 0.02 * S, z), 0.0035, (s_, 0, 0)), 'scrap')
        B.add(helix((0, rtop - 0.02 * S, zr0 + 0.04), 0.012 * S, 0.0035, 0.03, 0.0014), 'steel')


# ==================================================================================================== pistole
def pistol3(B, lay, st, rng, kind='auto'):
    """'auto' pistola semiautomatica a carrello, 'heavy' revolver pesante, 'energy' pistola a impulso, 'crude' tubo"""
    S = SC[st] * (1.1 if kind == 'heavy' and st == 'v' else 1.0)
    B.bore = bore = lay.bore
    gz, gy = float(lay.gR[2]), float(lay.gR[1])
    oal = {'auto': 0.23, 'heavy': 0.29, 'energy': 0.27, 'crude': 0.3}[kind] * OALX[st]
    zr = gz + 0.04 * S
    zm = zr - oal
    B.muzzle = (0.0, bore, zm)
    met = MET[st] if st != 'k' else 'blued'
    wood = WOOD[st] or 'rubber'
    B.top = bore + 0.016 * S
    if kind == 'auto':
        ys1, ys0, w = bore + 0.015 * S, bore - 0.011 * S, 0.023 * S
        sl = P([(zm, ys0), (zm, ys1 - 0.004 * S), (zm + 0.008 * S, ys1), (zr - 0.004, ys1), (zr, ys1 - 0.004 * S), (zr, ys0)], w, 0.0025 * S, 2)
        cuts = [box((w / 2, bore + 0.006 * S, zr - oal * 0.42), (0.012 * S, 0.012 * S, 0.034 * S), 0.001)]
        for s in (1, -1):
            for i in range(8): cuts.append(box((s * w / 2, bore + 0.002 * S, zr - 0.008 * S - i * 0.0045 * S), (0.003, 0.02 * S, 0.0018 * S)))
        cuts.append(cyl((0, bore, zm), 0.0065 * S, 0.01, 'z', 16))
        B.add(boolean_bm(sl, cuts), met)
        B.add(at_bore(lathe([(zm + 0.001, 0.0062 * S), (zm + 0.012, 0.0062 * S)], 'z', 18), B), 'bare')     # canna che si vede in bocca
        B.add(box((w / 2 - 0.003, bore + 0.006 * S, zr - oal * 0.42), (0.004, 0.01 * S, 0.032 * S), 0.0008), 'bare')
        fb = bore - 0.026 * S
        B.add(P([(zm + 0.012 * S, ys0), (zm + 0.012 * S, fb + 0.004), (zm + 0.02 * S, fb), (zr - 0.01, fb), (zr + 0.012 * S, ys0 - 0.002)], w * 0.92, 0.002 * S, 2), met)   # fusto
        B.add(P([(zr - 0.006, ys0 + 0.004), (zr + 0.006 * S, ys0 + 0.012 * S), (zr + 0.014 * S, ys0 + 0.01 * S), (zr + 0.01 * S, ys0 - 0.004)], 0.008 * S, 0.001, 1), 'bare')   # cane
        B.add(box((0, ys1 + 0.003 * S, zm + 0.008 * S), (0.003 * S, 0.006 * S, 0.006 * S), 0.0005), met)
        B.add(P([(zr - 0.012 * S, ys1), (zr - 0.002, ys1), (zr - 0.002, ys1 + 0.007 * S), (zr - 0.012 * S, ys1 + 0.006 * S)], 0.016 * S, 0.0008, 1), met)
        for s in (-1,):
            B.add(box((s * (w * 0.46 + 0.0015), fb + 0.006 * S, zr - oal * 0.45), (0.003, 0.005 * S, 0.03 * S), 0.0008), met)   # leva d'arresto
            B.add(box((s * (w * 0.46 + 0.0015), ys0 - 0.004 * S, zr - 0.01 * S), (0.003, 0.006 * S, 0.014 * S), 0.0008), met)   # sicura
        trigger_guard(B, gz - 0.04 * S, fb, S * 0.9, met)
        grip(B, gz, gy, fb + 0.002, S * 0.86, met, 0.2, panels=wood if st == 'k' else 'rubber', grooves=False)
        by = gy - 0.055 * S * 0.86
        B.add(box((0, by - 0.004, gz + 0.012 * S), (0.026 * S, 0.008 * S, 0.034 * S), 0.0015), met)     # fondello del caricatore
        if st == 'k': B.add(translate(transform(ring(0, 0.006, 0.0042, 0.003, 14), Matrix.Rotation(math.pi / 2, 4, 'X')), (0, by - 0.012, gz + 0.026)), 'steel')
        if st == 'v':
            B.add(box((0, ys0 - 0.008 * S, zm + 0.03 * S), (w * 0.9, 0.006 * S, 0.04 * S), 0.001), met)
            B.add(box((w / 2 + 0.001, ys1 - 0.006 * S, zm + 0.05 * S), (0.002, 0.004 * S, 0.05 * S), 0.0004), PAINT[st])
    elif kind == 'heavy':
        rc, Lc = 0.021 * S, 0.044 * S
        cz = gz - 0.035 * S                       # centro del tamburo
        cy = bore - rc * 0.62
        fb = cy - rc - 0.006 * S
        fr = P([(cz - Lc / 2 - 0.012 * S, bore + 0.008 * S), (cz - Lc / 2 - 0.012 * S, fb + 0.01 * S), (cz - Lc / 2 + 0.004, fb), (zr - 0.004, fb), (zr + 0.012 * S, cy), (zr + 0.004, bore + 0.014 * S), (zr - 0.02 * S, bore + 0.018 * S), (cz - Lc / 2, bore + 0.018 * S)], 0.026 * S, 0.003 * S, 2)
        fr = boolean_bm(fr, [box((0, cy, cz), (0.06, rc * 2.15, Lc + 0.002))])
        B.add(fr, met)
        cyl_ = lathe([(cz - Lc / 2, rc * 0.92), (cz - Lc / 2 + 0.004, rc), (cz + Lc / 2 - 0.003, rc), (cz + Lc / 2, rc * 0.94)], 'z', 36)
        cuts = []
        for k in range(6):
            a = k / 6 * 2 * math.pi + math.pi / 6
            cuts.append(cyl((math.cos(a) * rc * 1.12, math.sin(a) * rc * 1.12, cz + 0.006 * S), rc * 0.3, Lc * 0.62, 'z', 14))
            b = k / 6 * 2 * math.pi + math.pi / 2
            cuts.append(cyl((math.cos(b) * rc * 0.58, math.sin(b) * rc * 0.58, cz - Lc / 2), rc * 0.24, 0.016 * S, 'z', 14))
        B.add(translate(boolean_bm(cyl_, cuts), (0, cy, 0)), 'bare' if st == 'k' else met)
        for k in range(6):
            b = k / 6 * 2 * math.pi + math.pi / 2
            B.add(cyl((math.cos(b) * rc * 0.58, cy + math.sin(b) * rc * 0.58, cz - Lc / 2 + 0.006 * S), rc * 0.2, 0.004, 'z', 12), 'brass')   # fondelli dei colpi
        zb1 = cz - Lc / 2 - 0.012 * S
        B.add(at_bore(lathe([(zm, 0.0095 * S), (zm + 0.004, 0.0105 * S), (zb1, 0.0105 * S)], 'z', 24), B), met)
        B.add(at_bore(cyl((0, 0, zm), 0.0045 * S, 0.004, 'z', 14), B), 'blued')                                  # anima
        B.add(box((0, bore + 0.012 * S, (zm + zb1) / 2), (0.009 * S, 0.006 * S, zb1 - zm), 0.0015), met)            # bindella
        B.add(box((0, bore - 0.013 * S, (zm + zb1) / 2 + 0.01 * S), (0.016 * S, 0.012 * S, zb1 - zm - 0.02 * S), 0.003), met)   # copri-estrattore
        B.add(cyl((0, bore - 0.013 * S, zm + 0.005 * S), 0.003 * S, 0.012 * S, 'z', 10), 'bare')
        B.add(P([(zm + 0.004 * S, bore + 0.015 * S), (zm + 0.016 * S, bore + 0.015 * S), (zm + 0.012 * S, bore + 0.026 * S), (zm + 0.006 * S, bore + 0.026 * S)], 0.004 * S, 0.0006, 1), met)
        B.add(P([(zr - 0.006, bore + 0.01 * S), (zr + 0.008 * S, bore + 0.024 * S), (zr + 0.018 * S, bore + 0.024 * S), (zr + 0.012 * S, bore + 0.008 * S), (zr + 0.002, bore)], 0.008 * S, 0.001, 1), 'bare')   # cane
        B.add(box((-0.0135 * S, bore + 0.002, cz + Lc / 2 + 0.008 * S), (0.003, 0.008 * S, 0.012 * S), 0.0008), 'bare')   # leva del tamburo
        screws(B, [(0.013 * S, fb + 0.012 * S, cz + Lc / 2 + 0.014 * S), (0.013 * S, bore + 0.004, zr - 0.006 * S)], 0.0024 * S, 1)
        trigger_guard(B, gz - 0.04 * S, fb, S * 0.9, met)
        grip(B, gz + 0.006 * S, gy, fb + 0.002, S * 0.92, met, 0.36, panels=wood if st != 'v' else 'rubber', grooves=False)
        by = gy - 0.055 * S * 0.92
        B.add(translate(transform(ring(0, 0.0065, 0.0045, 0.003, 14), Matrix.Rotation(math.pi / 2, 4, 'X')), (0, by - 0.006, gz + 0.05 * S)), 'steel')
    elif kind == 'energy':
        e_pistol(B, lay, st, rng, S, zm, zr, gz, gy, met, wood)
    else:
        rb = 0.011 * S
        B.add(at_bore(lathe([(zm, rb * 1.3), (zm + 0.008, rb * 1.25), (zm + 0.012, rb), (zr - 0.02, rb), (zr - 0.02, rb * 1.4), (zr, rb * 1.4)], 'z', 16), B), 'scrap')
        B.add(at_bore(cyl((0, 0, zm), rb * 0.7, 0.01, 'z', 12), B), 'blued')
        hose_clamp(B, zm + 0.05, rb, S); hose_clamp(B, zr - 0.06, rb, S)
        blk = P([(zr - 0.09, bore - rb), (zr + 0.01, bore - rb), (zr + 0.02, bore - rb - 0.03), (zr - 0.08, bore - rb - 0.03)], 0.03 * S, 0.002, 1)
        B.add(blk, 'wood_dark')
        g = P([(gz - 0.02, bore - rb - 0.028), (gz + 0.024, bore - rb - 0.028), (gz + 0.04, gy - 0.05), (gz + 0.002, gy - 0.052)], 0.032 * S, 0.004, 1)
        B.add(g, 'wood_dark')
        B.add(tube([(0, bore - rb - 0.03, gz - 0.034), (0, bore - rb - 0.048, gz - 0.038), (0, bore - rb - 0.052, gz - 0.032)], 0.0025, segs=6), 'steel')   # grilletto di chiodo
        for k in range(4): B.add(rivet((0.017 * S, bore - rb - 0.015, zr - 0.08 + k * 0.025), 0.003, (1, 0, 0)), 'scrap')
        B.add(lathe([(-0.025, 0.02 * S), (0.025, 0.02 * S)], 'y', 12, (0, gy - 0.01, gz + 0.02)), 'cloth')
        B.top = bore + rb


def cage(B, c, z0, z1, r, S, n=6):
    """gabbia di barrette e anelli d'ottone attorno a un nucleo acceso"""
    for k in range(n):
        a = (k + 0.5) / n * 2 * math.pi
        B.add(cyl((c[0] + math.cos(a) * r, c[1] + math.sin(a) * r, (z0 + z1) / 2), 0.0012 * S, z1 - z0, 'z', 6), 'brass')
    for z in np.linspace(z0, z1, 3)[1:-1]:
        B.add(translate(ring(z, r + 0.0015, r - 0.0008, 0.003 * S, 24), (c[0], c[1], 0)), 'brass')


def e_pistol(B, lay, st, rng, S, zm, zr, gz, gy, met, wood):
    bore = B.bore
    r = 0.016 * S
    body = lathe([(zm + 0.03, r * 0.8), (zm + 0.05, r), (zr - 0.05, r * 1.3), (zr - 0.02, r * 1.45), (zr, r * 1.2), (zr + 0.008, r * 0.5)], 'z', 32)
    B.add(at_bore(body, B), met)
    for k in range(6):   # alette
        z = zm + 0.055 + k * 0.011 * S
        B.add(at_bore(ring(z, r * 1.55, r * 0.9, 0.0025 * S, 32), B), met)
    B.add(at_bore(ring(zm + 0.03, r * 1.2, r * 0.7, 0.008 * S, 32), B), 'copper')
    em = lathe([(zm, r * 1.25), (zm + 0.006, r * 1.3), (zm + 0.03, r * 0.75)], 'z', 32)
    em = boolean_bm(em, [cyl((0, 0, zm), r * 0.95, 0.012, 'z', 28)])
    B.add(at_bore(em, B), 'copper')
    B.add(at_bore(cyl((0, 0, zm + 0.006), r * 0.9, 0.002, 'z', 28), B), 'glow')
    # cella sopra, nel vetro
    cz0, cz1 = zr - 0.075 * S, zr - 0.015 * S
    cy = bore + r * 1.35 + 0.008 * S
    B.add(cyl((0, cy, (cz0 + cz1) / 2), 0.0075 * S, cz1 - cz0, 'z', 16), 'glow')
    cage(B, (0, cy, 0), cz0, cz1, 0.0085 * S, S)
    for z in (cz0, cz1): B.add(cyl((0, cy, z), 0.0105 * S, 0.006 * S, 'z', 24, 0.001), 'brass')
    B.add(box((0, (cy + bore + r) / 2, (cz0 + cz1) / 2), (0.008 * S, cy - bore - r + 0.004, 0.012 * S), 0.0015), met)
    # quadrante
    B.add(cyl((r * 1.2 + 0.002, bore, zr - 0.04 * S), 0.009 * S, 0.006 * S, 'x', 24, 0.001), 'brass')
    B.add(cyl((r * 1.2 + 0.0055, bore, zr - 0.04 * S), 0.0075 * S, 0.001, 'x', 24), 'glass')
    B.add(tube([(0, bore - r * 1.1, zr - 0.07 * S), (0.014 * S, bore - r * 1.5, zr - 0.1 * S), (0.0, bore - r * 1.05, zm + 0.06)], 0.0025 * S, segs=8), 'rubber')
    fb = bore - r * 1.3
    trigger_guard(B, gz - 0.04 * S, fb, S * 0.9, met)
    grip(B, gz, gy, fb + 0.002, S * 0.86, met, 0.24, panels=wood if wood != 'rubber' else 'rubber', grooves=False)


# ==================================================================================================== energia
def energy3(B, lay, st, rng, kind='impulse'):
    """fucili a energia: corpo con la cella, emettitore con alette e bobine, cavi, quadranti, capacitore di vetro"""
    S = SC[st]
    big = kind == 'cannon'
    if big: S *= 1.45
    B.bore = bore = lay.bore
    gR = lay.gR
    gL = lay.gL if lay.gL is not None else gR + np.array([0, 0.05, -0.3])
    gz, gy = float(gR[2]), float(gR[1])
    oal = {'impulse': 0.98, 'discharge': 1.02, 'fusion': 0.98, 'beam': 1.1, 'cannon': 1.55}[kind] * OALX[st]
    zb = gz + 0.30 * min(S, 1.15)
    if big: zb = gz + 0.12
    zm = zb - oal
    B.muzzle = (0.0, bore, zm)
    met, wood = MET[st], WOOD[st]
    rtop, rbot = bore + 0.03 * S, bore - 0.034 * S
    rw = 0.046 * S
    zr0 = gz + 0.055 * S
    zr1 = zr0 - 0.26 * S
    B.top = rtop
    col = lay.emissive_color()
    if col: B.glow_color = tuple(min(1.0, c * 1.4) for c in col)
    else: B.glow_color = {'impulse': (0.3, 0.7, 1.0), 'discharge': (0.6, 0.8, 1.0), 'fusion': (1.0, 0.45, 0.1), 'beam': (0.4, 1.0, 0.3), 'cannon': (0.3, 0.7, 1.0)}[kind]

    # ------------------------------------------------------------------ corpo con la cella
    body = zbox_chamfer(rw, rtop - rbot, zr1, zr0, 0.014 * S, (rtop + rbot) / 2)
    cuts = [box((rw / 2, bore + 0.002, (zr1 + zr0) / 2 - 0.02 * S), (0.01 * S, 0.026 * S, 0.11 * S), 0.004)]     # finestra della cella
    for i in range(7): cuts.append(box((0, rtop, zr1 + 0.03 * S + i * 0.012 * S), (rw * 0.6, 0.008 * S, 0.005 * S)))   # sfoghi
    B.add(boolean_bm(body, cuts), met)
    cz = (zr1 + zr0) / 2 - 0.02 * S
    B.add(cyl((rw / 2 - 0.012 * S, bore + 0.002, cz), 0.011 * S, 0.1 * S, 'z', 24), 'glow')
    for z in (cz - 0.04 * S, cz, cz + 0.04 * S): B.add(box((rw / 2 - 0.001, bore + 0.002, z), (0.004, 0.028 * S, 0.004 * S), 0.0006), met)
    for s in (1, -1):
        screws(B, [(s * rw / 2, y, z) for y in (rbot + 0.008 * S, rtop - 0.008 * S) for z in (zr1 + 0.01 * S, zr0 - 0.012 * S)], 0.0026 * S, s)
    # quadrante a lancetta sul fianco sinistro
    dz, dy = zr0 - 0.05 * S, rtop - 0.024 * S
    B.add(cyl((-rw / 2 - 0.003 * S, dy, dz), 0.014 * S, 0.008 * S, 'x', 28, 0.0015), 'brass')
    B.add(cyl((-rw / 2 - 0.0074 * S, dy, dz), 0.0115 * S, 0.0012, 'x', 28), 'glass')
    B.add(box((-rw / 2 - 0.0068 * S, dy + 0.004 * S, dz - 0.002 * S), (0.0008, 0.009 * S, 0.0012), 0.0), 'paint_v')
    # capacitore di vetro sul dorso
    kz0, kz1 = zr1 - 0.02 * S, zr0 - 0.08 * S
    ky = rtop + 0.016 * S
    B.add(cyl((0, ky, (kz0 + kz1) / 2), 0.009 * S, kz1 - kz0, 'z', 20), 'glow')
    cage(B, (0, ky, 0), kz0, kz1, 0.0105 * S, S)
    for z in (kz0, kz1):
        B.add(cyl((0, ky, z), 0.0135 * S, 0.01 * S, 'z', 28, 0.0015), 'brass')
        B.add(box((0, (ky + rtop) / 2, z), (0.012 * S, ky - rtop, 0.012 * S), 0.0015), met)
    # ------------------------------------------------------------------ emettitore
    re = 0.013 * S
    ze1 = zr1
    if kind in ('impulse', 'cannon'):
        B.add(at_bore(lathe([(zm + 0.03, re), (ze1, re)], 'z', 24), B), 'blued')
        n = int((ze1 - zm - 0.12 * S) / (0.011 * S))
        for i in range(n):
            B.add(at_bore(ring(zm + 0.09 * S + i * 0.011 * S, re * 2.1, re * 0.95, 0.003 * S, 32), B), met)     # alette
        for i in range(3):
            B.add(at_bore(ring(zm + 0.05 * S + i * 0.012 * S, re * 1.6, re * 0.95, 0.007 * S, 32), B), 'copper')
        em = lathe([(zm, re * 1.9), (zm + 0.004, re * 2.0), (zm + 0.03 * S, re * 1.3)], 'z', 32)
        B.add(at_bore(boolean_bm(em, [cyl((0, 0, zm), re * 1.5, 0.02, 'z', 28)]), B), met)
        B.add(at_bore(cyl((0, 0, zm + 0.008), re * 1.45, 0.002, 'z', 28), B), 'glow')
    elif kind == 'discharge':
        B.add(at_bore(lathe([(zm + 0.06, re * 0.9), (ze1, re * 0.9)], 'z', 20), B), 'blued')
        B.add(at_bore(helix((0, 0, (zm + 0.09 * S + ze1 - 0.06 * S) / 2), re * 1.35, 0.0055 * S, ze1 - zm - 0.17 * S, 0.0022 * S), B), 'copper')
        for z in (zm + 0.07 * S, ze1 - 0.04 * S):   # isolatori di ceramica a dischi
            prof = []
            for k in range(6): prof += [(z + k * 0.006 * S, re * 1.2), (z + k * 0.006 * S + 0.003 * S, re * 1.9)]
            B.add(at_bore(lathe(prof, 'z', 28), B), 'ceramic')
        for s in (1, -1):   # forcella
            B.add(tube([(s * re * 1.2, bore, zm + 0.07 * S), (s * re * 1.6, bore + 0.004, zm + 0.03 * S), (s * re * 1.3, bore + 0.002, zm)], 0.0035 * S, segs=10), 'copper')
            B.add(cyl((s * re * 1.3, bore + 0.002, zm), 0.0045 * S, 0.006 * S, 'z', 12), 'glow')
        B.add(tube([(-re * 1.25, bore + 0.002, zm + 0.002), (0, bore + 0.006, zm - 0.004), (re * 1.25, bore + 0.002, zm + 0.002)], 0.0012, segs=6), 'glow')
    elif kind == 'fusion':
        for s in (1, -1):   # camere gemelle di vetro con il nucleo acceso
            c = (s * re * 1.1, bore, (zm + ze1) / 2 + 0.02 * S)
            L = ze1 - zm - 0.08 * S
            B.add(cyl(c, re * 0.8, L, 'z', 20), 'glow')
            cage(B, (c[0], c[1], 0), c[2] - L / 2, c[2] + L / 2, re * 0.92, S, 4)
        for z in np.linspace(zm + 0.05 * S, ze1, 5):
            B.add(at_bore(transform(ring(z, 1.0, 0.0, 0.01 * S, 32), Matrix.Diagonal((re * 2.6, re * 1.55, 1, 1))), B), met)
        for a in (0.6, 2.5, 3.8, 5.7):
            B.add(at_bore(lathe([(zm + 0.05 * S, 0.0028 * S), (ze1, 0.0028 * S)], 'z', 10, (math.cos(a) * re * 2.4, math.sin(a) * re * 1.4, 0)), B), 'bare')
        em = zbox_chamfer(re * 4.6, re * 2.6, zm, zm + 0.045 * S, re * 0.6, bore)
        em = boolean_bm(em, [cyl((s * re * 1.1, bore, zm), re * 0.85, 0.06, 'z', 20) for s in (1, -1)])
        B.add(em, met)
        for s in (1, -1): B.add(cyl((s * re * 1.1, bore, zm + 0.01), re * 0.8, 0.002, 'z', 20), 'glow')
    else:   # raggio dei Vrepigh: tubo di rottami con barattoli di vetro accesi e bobine di filo
        B.add(at_bore(lathe([(zm, re * 1.6), (zm + 0.02, re * 1.4), (ze1, re * 1.4)], 'z', 14), B), 'scrap')
        for i, z in enumerate(np.linspace(zm + 0.1 * S, ze1 - 0.08 * S, 3)):
            jy = bore + re * 1.4 + 0.022 * S
            B.add(cyl((0, jy, z), 0.017 * S, 0.048 * S, 'y', 16), 'glow')
            for a in np.linspace(0, 2 * math.pi, 5, endpoint=False):
                B.add(cyl((math.cos(a) * 0.019 * S, jy, z + math.sin(a) * 0.019 * S), 0.0016, 0.05 * S, 'y', 6), 'scrap')
            B.add(cyl((0, jy + 0.027 * S, z), 0.022 * S, 0.006 * S, 'y', 18), 'scrap')
            B.add(at_bore(helix((0, 0, z + 0.04 * S), re * 1.55, 0.004, 0.03 * S, 0.0016), B), 'copper')
        for z in np.linspace(zm + 0.03, ze1 - 0.02, 4): hose_clamp(B, z, re * 1.4, S)
    # cavi dal corpo all'emettitore
    for s in (1, -1):
        a = (s * rw * 0.42, rbot + 0.01 * S, zr1 + 0.01)
        b = (s * re * 1.7, bore - re * 1.2, zm + 0.12 * S)
        pts = sag(a, b, 0.018 * S, 10)
        B.add(tube(pts, 0.0034 * S, segs=10), 'rubber')
        for p in (pts[0], pts[-1]): B.add(cyl(p, 0.0052 * S, 0.012 * S, 'z', 12), 'brass')
    # ------------------------------------------------------------------ guardamano, impugnature, calcio
    zhg0 = max(float(gL[2]) - 0.1, zm + 0.18 * S)
    if st == 'v' or big:
        sh = zbox_chamfer(0.05 * S, 0.03 * S, zhg0, zr1 + 0.002, 0.008 * S, bore - re - 0.016 * S)
        B.add(sh, met)
        fz, fy = float(gL[2]), bore - re - 0.031 * S
        B.add(P([(fz - 0.018 * S, fy), (fz + 0.018 * S, fy), (fz + 0.022 * S, fy - 0.085 * S), (fz - 0.014 * S, fy - 0.085 * S)], 0.03 * S, 0.006 * S, 2), 'rubber')
    elif st == 'k':
        hb = bore - re - 0.03 * S
        lo = P([(zhg0, hb + 0.01), (zhg0 + 0.012, hb), (zr1 + 0.002, hb), (zr1 + 0.002, bore - re * 0.6), (zhg0, bore - re * 0.6)], 0.042 * S, 0.008 * S, 3)
        B.add(lo, wood)
        band(B, zhg0 + 0.02, (hb + bore - re * 0.6) / 2, 0.023 * S, (bore - re * 0.6 - hb) / 2 + 0.002, 0.01 * S, met, bolt=False)
    else:
        fz, fy = float(gL[2]), bore - re * 1.4
        B.add(P([(fz - 0.018, fy + 0.004), (fz + 0.018, fy + 0.004), (fz + 0.02, fy - 0.08), (fz - 0.016, fy - 0.08)], 0.03, 0.002, 1), 'wood_dark')
    zt = gz - 0.045 * S
    trigger_guard(B, zt, rbot, S, met)
    grip(B, gz, gy, rbot + 0.002, S, {'k': wood, 'v': 'rubber', 'x': 'wood_dark'}[st], 0.28)
    # cella di ricambio sotto, come un caricatore
    zc = zr1 + 0.06 * S
    B.add(box((0, rbot - 0.03 * S, zc), (0.03 * S, 0.06 * S, 0.05 * S), 0.004), met)
    B.add(box((0.016 * S, rbot - 0.03 * S, zc), (0.002, 0.04 * S, 0.008 * S)), 'glow')
    B.add(box((0, rbot - 0.062 * S, zc), (0.034 * S, 0.006 * S, 0.054 * S), 0.0015), 'brass')
    if big:   # cannone: niente calcio, maniglie e piastra di culatta
        B.add(box((0, (rtop + rbot) / 2, zr0 + 0.015), (rw * 1.1, rtop - rbot, 0.03), 0.004), met)
        for s in (1, -1):
            B.add(tube([(s * rw * 0.5, rtop - 0.01, zr0 + 0.03), (s * rw * 0.9, rtop + 0.03, zr0 + 0.06), (s * rw * 0.9, rtop + 0.03, zr0 - 0.06), (s * rw * 0.5, rtop - 0.01, zr0 - 0.08)], 0.008, segs=12), met)
        return B
    stock(B, st, 'auto', zr0, zb, rtop, rbot, gz, S, wood, met, rng)
    return B
