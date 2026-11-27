# Forma del Vrepigh come campo di distanza: primitive fuse con unioni morbide, solchi tra i muscoli, poi marching cubes.
# Linea: predatore affamato e rabbioso. Testa a cuneo bassa con arcate aggrottate a V, occhi piccoli incassati,
# grugno corrugato dal ringhio, bocca aperta (labbro arricciato: si vedono gengive e denti), mascella pesante.
# Corpo asciutto e duro: gobba e scudo delle spalle, trapezi enormi, pettorali e addominali separati, costole sui
# fianchi (fame), braccia nodose, mani grandi da artigli.
import json, os
import numpy as np
from skimage import measure

R = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rig.json')))
J = {k: np.array(v) for k, v in R['joints'].items()}

# Varianti (variants.py): moltiplicatori della forma. configure() li imposta prima di build().
C = {}


def cf(k, d=1.0):
    return C.get(k, d)


def configure(cfg):
    global HS, HOFF
    C.clear(); C.update(cfg)
    HS = BASE_HS * cf('head')
    HOFF = BASE_HOFF + np.array([0, -0.03 * (cf('head') - 1) - cf('head_fwd', 0.0), cf('head_up', 0.0)])


def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b * (1 - h) + a * h - k * h * (1 - h)


def smax(a, b, k):
    return -smin(-a, -b, k)


def capsule(P, a, b, ra, rb):
    a = np.asarray(a, float); b = np.asarray(b, float)
    ba = b - a; t = np.clip(((P - a) @ ba) / (ba @ ba), 0, 1)
    q = a + t[..., None] * ba
    return np.linalg.norm(P - q, axis=-1) - (ra + (rb - ra) * t)


def ellip(P, c, r):
    r = np.asarray(r, float)
    q = (P - np.asarray(c, float)) / r
    k0 = np.linalg.norm(q, axis=-1)
    k1 = np.linalg.norm(q / r, axis=-1)
    return k0 * (k0 - 1) / np.maximum(k1, 1e-9)


def disc(P, c, n, r, h):
    n = np.asarray(n, float); n = n / np.linalg.norm(n)
    d = P - np.asarray(c, float)
    ax = d @ n
    rad = np.linalg.norm(d - ax[..., None] * n, axis=-1)
    q = np.stack([rad - r, np.abs(ax) - h], -1)
    return np.minimum(np.maximum(q[..., 0], q[..., 1]), 0) + np.linalg.norm(np.maximum(q, 0), axis=-1) - 0.012


def groove(d, P, a, b, r, depth_k=0.012):
    """solco tra due masse: sottrae un tubo sottile"""
    return smax(d, -capsule(P, a, b, r, r), depth_k)


def mir(P):
    Q = P.copy(); Q[..., 0] = np.abs(Q[..., 0]); return Q


def L(n):
    return J['Left' + n]


def ext(a, b, f):
    return b + (b - a) * f


def sx(P, k):
    Q = P.copy(); Q[..., 0] = Q[..., 0] * k; return Q


def sstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------- testa (posizioni fisse, attorno all'osso Head)
H = J['Head']


def o(x, y, z):
    return H + np.array([x, y, z], float)


NOSE = o(0, -0.396, 0.012)
SNOUT_A = o(0, -0.05, 0.095)
SNOUT_B = o(0, -0.35, 0.022)
NOSE_N = (SNOUT_B - SNOUT_A) / np.linalg.norm(SNOUT_B - SNOUT_A)
EYE = o(0.066, -0.158, 0.122)
# bocca aperta nel ringhio (coordinate della testa): labbro superiore e mandibola si separano dalla punta all'angolo
MOUTH_TIP_Y, MOUTH_CORNER_Y = -0.39, -0.095


def mouth_lines(yl):
    """quote (z) del labbro superiore e del bordo della mandibola lungo la bocca, e t (0 punta, 1 angolo)"""
    t = np.clip((yl - MOUTH_TIP_Y) / (MOUTH_CORNER_Y - MOUTH_TIP_Y), 0, 1)
    sn = cf('snarl')
    z_up = -0.004 + 0.013 * sn * (1 - t) ** 1.6                      # il labbro sale davanti: si scoprono i denti
    z_lo = -0.008 - 0.026 * sn * (1 - t) ** 1.6                      # la mandibola cala appena
    return z_up, z_lo, t


BASE_HS = 1.22
BASE_HOFF = np.array([0, -0.06, 0.045])
HS, HOFF = BASE_HS, BASE_HOFF.copy()


def head(P0):
    P = H + (P0 - H - HOFF) / HS
    sn = cf('snout')                                  # grugno piu' lungo o piu' corto
    P = P.copy(); fw = np.clip(-(P[..., 1] - H[1]) - 0.10, 0, None); P[..., 1] += fw * (1 - 1 / sn)
    Q = mir(P)
    xl, yl, zl = P[..., 0] - H[0], P[..., 1] - H[1], P[..., 2] - H[2]
    br = cf('brow')
    # cuneo: cranio basso e lungo che scende dritto nel grugno, piu' alto che largo
    d = capsule(sx(P, 1.25), SNOUT_A, SNOUT_B, 0.125, 0.058)
    d = smin(d, ellip(P, o(0, 0.01, 0.1), [0.105, 0.13, 0.105]), 0.06)                        # cranio
    d = smin(d, capsule(P, o(0, -0.12, 0.2), o(0, 0.05, 0.205), 0.008, 0.014), 0.035)          # cresta sagittale
    # arcate aggrottate a V: l'interno piu' basso e piu' avanti, nodo di rabbia in mezzo
    d = smin(d, capsule(Q, o(0.016, -0.205, 0.148), o(0.072, -0.165, 0.162), 0.018 * br, 0.022 * br), 0.035)
    d = smin(d, ellip(P, o(0, -0.2, 0.152), np.array([0.026, 0.022, 0.016]) * br), 0.03)
    d = smin(d, capsule(Q, o(0.078, -0.17, 0.088), o(0.098, -0.06, 0.095), 0.016, 0.02), 0.035)   # zigomi
    d = smin(d, ellip(Q, o(0.085, -0.075, 0.012 - 0.02 * (cf('jowl') - 1)), np.array([0.074, 0.1, 0.085]) * cf('jowl')), 0.05)  # giogaie / masseteri
    d = smin(d, ellip(Q, o(0.052, -0.265, 0.016), [0.031, 0.042, 0.03]), 0.03)                # labbro gonfio sopra la zanna
    d = smin(d, disc(P, NOSE, NOSE_N, 0.052, 0.016), 0.025)                                    # disco del grugno
    d = smin(d, capsule(sx(P, 1.1), o(0, -0.06, -0.01), o(0, -0.33, -0.062), 0.085, 0.042), 0.03)  # mandibola pesante
    d = smin(d, ellip(P, o(0, -0.3, -0.078), [0.042, 0.05, 0.03]), 0.02)                       # mento
    d = smin(d, ellip(P, o(0, -0.02, -0.06), [0.09, 0.11, 0.075]), 0.05)                       # gola
    # bocca aperta: taglio tra labbro e mandibola, dalla punta fino all'angolo
    z_up, z_lo, t = mouth_lines(yl)
    zc = (z_up + z_lo) / 2; hh = (z_up - z_lo) / 2
    cav = np.maximum(np.maximum(np.abs(zl - zc) - hh, yl - MOUTH_CORNER_Y), np.abs(xl) - (0.062 + 0.02 * t))
    d = smax(d, -cav, 0.01)
    d = smin(d, ellip(P, o(0, -0.2, -0.034 - 0.008 * cf('snarl')), [0.032, 0.11, 0.012]), 0.008)   # lingua sul fondo
    d = smax(d, -ellip(Q, NOSE + NOSE_N * 0.024 + np.array([0.022, 0, 0.004]), [0.013, 0.026, 0.019]), 0.006)   # narici
    d = smax(d, -ellip(Q, EYE, [0.025, 0.022, 0.019]), 0.008)                                  # orbite infossate
    # rughe del ringhio: poche pieghe profonde alla radice del grugno e solchi verticali tra le arcate
    ztop = 0.22 + (yl + 0.05) / (-0.3) * (0.08 - 0.22)
    mz = sstep(ztop - 0.045, ztop - 0.015, zl) * sstep(0.075, 0.05, np.abs(xl)) * sstep(-0.29, -0.26, yl) * sstep(-0.16, -0.19, yl)
    yw = yl - 2.5 * xl * xl
    d = d + 0.0035 * cf('snarl') * mz * (0.5 + 0.5 * np.cos(2 * np.pi * (yw + 0.2) / 0.03)) ** 3
    mb = sstep(-0.232, -0.218, yl) * sstep(-0.168, -0.182, yl) * sstep(0.115, 0.13, zl)
    d = d + 0.0035 * mb * (np.exp(-((np.abs(xl) - 0.01) / 0.004) ** 2) + 0.6 * np.exp(-((np.abs(xl) - 0.028) / 0.004) ** 2))
    return d * HS


# ---------------------------------------------------------------- corpo
def ribs(P, d):
    """costole sui fianchi (la fame): solchi obliqui tra le costole, piu' marcati in basso sul fianco"""
    lean = cf('lean')
    if lean <= 0.05: return d
    x, y, z = np.abs(P[..., 0]), P[..., 1], P[..., 2]
    m = sstep(0.1, 0.17, x) * sstep(0.97, 1.01, z) * sstep(1.15, 1.1, z) * sstep(-0.2, -0.15, y) * sstep(0.02, -0.04, y)
    u = z - 0.4 * y
    wave = (0.5 + 0.5 * np.cos(2 * np.pi * u / 0.042)) ** 2
    return d + 0.003 * lean * m * wave


def body(P):
    Q = mir(P)
    lean = cf('lean')
    belly = cf('belly')
    d = ellip(P, [0, 0.0, 0.80], np.array([0.17, 0.14, 0.125]) * cf('hips'))                                     # bacino
    d = smin(d, ellip(P, [0, -0.035 - 0.04 * (belly - 1), 0.955 - cf('belly_drop', 0.0)], np.array([0.155, 0.13, 0.16]) * belly), 0.07)   # ventre asciutto
    # gabbia toracica un poco ritirata: costole e pettorali vi stanno sopra
    d = smin(d, ellip(P, [0, -0.05, 1.15], np.array([0.25, 0.18, 0.17]) * cf('chest') * 0.97), 0.06)
        # addominali: tre file di placche
    for zr, yy in ((0.885, 0.0), (0.955, -0.004), (1.03, -0.008)):
        yb = -0.14 - 0.045 * (belly - 1) + yy - 0.03 * max(belly - 1, 0)
        d = smin(d, ellip(Q, [0.046, yb + 0.006, zr - cf('belly_drop', 0.0) * 0.5], np.array([0.044, 0.024, 0.034]) * (0.9 + 0.1 * lean)), 0.03)
    d = groove(d, P, [0, -0.2, 0.84], [0, -0.215, 1.08], 0.006 * lean, 0.01)                                    # linea alba
    # gobba e scudo delle spalle: la massa dei cinghiali maschi
    hump = cf('hump')
    d = smin(d, ellip(P, [0, 0.05 + 0.03 * (hump - 1), 1.3 + 0.03 * (hump - 1)], np.array([0.27, 0.165, 0.22]) * hump), 0.12)
    d = smin(d, ellip(P, [0, 0.05, 1.445 + 0.05 * (hump - 1)], np.array([0.15, 0.13, 0.11]) * hump), 0.07)                # garrese
    d = smin(d, ellip(Q, [0.16, 0.05, 1.15], [0.1, 0.1, 0.17]), 0.1)                                                # dorsali
    d = smin(d, ellip(Q, [0.125, 0.15, 1.25], [0.09, 0.045, 0.1]), 0.04)                                              # scapole
    d = groove(d, P, [0, 0.2, 0.9], [0, 0.22, 1.2], 0.008, 0.014)                                                 # solco della spina
    d = smin(d, capsule(P, [0, -0.02, 1.22], H + [0, -0.07, 0.10], 0.16 * cf('neck'), 0.135 * cf('neck')), 0.06)   # collo taurino
    d = smin(d, capsule(Q, [0.0, 0.03, 1.38], L('Arm') + [-0.05, 0.03, 0.07], 0.105 * cf('neck'), 0.085 * cf('arm')), 0.06)   # trapezi
    # pettorali: due lastre con il solco in mezzo e l'orlo basso netto
    ch = cf('chest')
    for k in (0, 1):
        d = smin(d, ellip(Q, [0.115, -0.168 - 0.03 * (ch - 1) - 0.01 * k, 1.215 - cf('chest_sag', 0.0) - 0.03 * k],
                          np.array([0.13, 0.072, 0.085]) * ch * (1 - 0.18 * k)), 0.03)
    d = groove(d, P, [0, -0.25, 1.12], [0, -0.255, 1.31], 0.011, 0.018)
    # deltoidi, con il solco verso il pettorale
    arm = cf('arm')
    d = smin(d, ellip(Q, L('Arm') + [0.015, 0.0, 0.015], np.array([0.13, 0.125, 0.115]) * arm), 0.04)
    d = groove(d, Q, L('Arm') + [-0.07, -0.12, 0.08], L('Arm') + [0.05, -0.115, -0.07], 0.008, 0.012)
    # braccia: bicipite davanti, tricipite dietro, solco tra i due
    d = smin(d, capsule(Q, L('Arm'), L('ForeArm'), 0.1 * arm, 0.08 * arm), 0.05)
    mid = L('Arm') * 0.52 + L('ForeArm') * 0.48
    d = smin(d, ellip(Q, mid + [0, -0.03, -0.005], np.array([0.12, 0.068, 0.072]) * arm), 0.03)        # bicipite
    d = smin(d, ellip(Q, mid + [-0.04, 0.035, 0.0], np.array([0.13, 0.065, 0.075]) * arm), 0.03)       # tricipite
    d = groove(d, Q, mid + [-0.1, 0.0, -0.075 * arm], mid + [0.1, 0.0, -0.07 * arm], 0.007, 0.01)
    d = groove(d, Q, mid + [-0.1, 0.0, 0.08 * arm], mid + [0.1, 0.0, 0.075 * arm], 0.007, 0.01)
    # avambracci nodosi: massa sotto il gomito, polso stretto
    fa = cf('forearm')
    d = smin(d, capsule(Q, L('ForeArm'), L('Hand'), 0.088 * fa, 0.055 * fa), 0.04)
    d = smin(d, ellip(Q, (L('ForeArm') * 0.72 + L('Hand') * 0.28) + [0, -0.01, 0.015], np.array([0.12, 0.075, 0.075]) * fa), 0.03)
    d = smin(d, ellip(Q, (L('ForeArm') * 0.6 + L('Hand') * 0.4) + [0, 0.02, -0.02], np.array([0.1, 0.06, 0.06]) * fa), 0.03)
    # mani grandi e nocchiute
    hand = J['LeftHand']; midf = J['LeftHandMiddle1']; hs = cf('hand')
    d = smin(d, ellip(Q, hand * 0.45 + midf * 0.55, np.array([0.075, 0.06, 0.032]) * hs), 0.03)
    for f, r0 in (('Index', 0.021), ('Middle', 0.022), ('Ring', 0.021), ('Pinky', 0.018), ('Thumb', 0.024)):
        r0 *= hs
        a = J['LeftHand' + f + '1']; b = J['LeftHand' + f + '2']; c = J['LeftHand' + f + '3']
        tip = ext(b, c, 0.8)
        base = hand if f == 'Thumb' else a
        d = smin(d, capsule(Q, base, b, r0, r0 * 0.92), 0.012)
        d = smin(d, ellip(Q, b, np.array([1.0, 1.0, 1.0]) * r0 * 1.12), 0.006)                        # nocca
        d = smin(d, capsule(Q, b, c, r0 * 0.92, r0 * 0.82), 0.008)
        d = smin(d, capsule(Q, c, tip, r0 * 0.82, r0 * 0.66), 0.006)
    # gambe: quadricipiti davanti, ginocchio, polpacci, stinco secco
    lg = cf('leg')
    d = smin(d, capsule(Q, L('UpLeg'), L('Leg'), 0.112 * lg, 0.078 * lg), 0.06)
    d = smin(d, ellip(Q, L('UpLeg') * 0.55 + L('Leg') * 0.45 + [0.01, -0.035, 0], np.array([0.095, 0.09, 0.15]) * lg), 0.035)   # quadricipite
    d = smin(d, ellip(Q, L('UpLeg') * 0.25 + L('Leg') * 0.75 + [-0.035, -0.06, 0.01], np.array([0.05, 0.045, 0.07]) * lg), 0.03)  # vasto interno
    d = smin(d, ellip(Q, L('UpLeg') * 0.5 + L('Leg') * 0.5 + [0.0, 0.05, 0], np.array([0.085, 0.075, 0.14]) * lg), 0.035)     # bicipite femorale
    d = smin(d, ellip(Q, L('Leg') + [0, -0.045, 0.0], np.array([0.05, 0.04, 0.045]) * lg), 0.02)                       # ginocchio
    d = smin(d, capsule(Q, L('Leg'), L('Foot'), 0.072 * lg, 0.05 * lg), 0.035)
    d = smin(d, ellip(Q, L('Leg') * 0.68 + L('Foot') * 0.32 + [0, 0.035, 0], np.array([0.068, 0.07, 0.1]) * lg), 0.03)   # polpaccio
    # piede tozzo: caviglia e pianta, unghioni fessi
    foot = L('Foot'); toe = L('ToeBase')
    d = smin(d, capsule(Q, foot + [0, 0.02, -0.04], toe + [0, 0.035, 0.035], 0.068, 0.058), 0.04)
    for sgn in (1, -1):
        t = J['LeftToeBase'] * [sgn, 1, 1]
        g = capsule(P, t + [0, -0.03, -0.02], t + [0, -0.1, 0.06], 0.007, 0.007)
        d = smax(d, -g, 0.006)
    d = ribs(P, d)
    d = np.maximum(d, -(P[..., 2] - 0.0))                                              # niente sotto il suolo
    return d


def field(P):
    return smin(body(P), head(P), 0.035)


def build(step=0.005, lo=(-1.24, -0.88, -0.01), hi=(1.24, 0.5, 1.9)):
    lo = np.array(lo, float); hi = np.array(hi, float)
    n = np.ceil((hi - lo) / step).astype(int) + 1
    xs = [lo[i] + np.arange(n[i]) * step for i in range(3)]
    F = np.empty(n, np.float32)
    Y, Z = np.meshgrid(xs[1], xs[2], indexing='ij')
    for i, x in enumerate(xs[0]):
        P = np.stack([np.full_like(Y, x), Y, Z], -1)
        F[i] = field(P)
    v, f, _, _ = measure.marching_cubes(F, 0.0, spacing=(step,) * 3)
    return v + lo, f[:, ::-1]


if __name__ == '__main__':
    v, f = build()
    np.savez('shape.npz', v=v, f=f)
    print(len(v), len(f))
