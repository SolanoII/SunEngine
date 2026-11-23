# Forma del Vrepigh come campo di distanza: primitive fuse con unioni morbide, poi marching cubes.
import json, numpy as np
from skimage import measure

import os
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


def mir(P):
    Q = P.copy(); Q[..., 0] = np.abs(Q[..., 0]); return Q


def L(n):
    return J['Left' + n]


def ext(a, b, f):
    return b + (b - a) * f


# ---------------------------------------------------------------- testa (posizioni fisse, attorno all'osso Head)
H = J['Head']
NOSE = H + [0, -0.385, 0.0]
SNOUT_A = H + [0, -0.06, 0.10]
SNOUT_B = H + [0, -0.36, 0.005]
NOSE_N = (SNOUT_B - SNOUT_A) / np.linalg.norm(SNOUT_B - SNOUT_A)
JAW_A = H + [0, -0.06, 0.02]
JAW_B = H + [0, -0.31, -0.035]
EYE = H + [0.068, -0.15, 0.128]


def sx(P, k):
    Q = P.copy(); Q[..., 0] = Q[..., 0] * k; return Q


BASE_HS = 1.22
BASE_HOFF = np.array([0, -0.06, 0.045])
HS, HOFF = BASE_HS, BASE_HOFF.copy()


def head(P0):
    P = H + (P0 - H - HOFF) / HS
    sn = cf('snout')                                  # grugno piu' lungo o piu' corto
    P = P.copy(); fw = np.clip(-(P[..., 1] - H[1]) - 0.10, 0, None); P[..., 1] += fw * (1 - 1 / sn)
    Q = mir(P)
    # cuneo: cranio basso che scende dritto nel grugno, piu' alto che largo
    d = capsule(sx(P, 1.3), SNOUT_A, SNOUT_B, 0.125, 0.05)
    d = smin(d, ellip(P, H + [0, -0.01, 0.10], [0.10, 0.12, 0.095]), 0.06)
    d = smin(d, disc(P, NOSE, NOSE_N, 0.043, 0.014), 0.025)
    d = smin(d, ellip(Q, H + [0.08, -0.10, 0.02 - 0.02 * (cf('jowl') - 1)], np.array([0.08, 0.12, 0.09]) * cf('jowl')), 0.05)        # giogaie gonfie
    d = smin(d, capsule(Q, H + [0.0, -0.17, 0.165], H + [0.08, -0.145, 0.15], 0.03 * cf('brow'), 0.028 * cf('brow')), 0.03)  # arcata pesante
    d = smin(d, capsule(sx(P, 1.2), JAW_A, JAW_B, 0.075, 0.04), 0.03)                   # mandibola
    d = smax(d, -ellip(Q, NOSE + NOSE_N * 0.022 + [0.024, 0, 0.004], [0.013, 0.02, 0.02]), 0.006)   # narici
    d = smax(d, -ellip(Q, EYE, [0.024, 0.022, 0.018]), 0.008)                           # orbite piccole
    m = capsule(Q, H + [0.0, -0.355, -0.022], H + [0.08, -0.10, -0.005], 0.007, 0.012)   # bocca
    d = smax(d, -m, 0.008)
    return d * HS


# ---------------------------------------------------------------- corpo
def body(P):
    Q = mir(P)
    d = ellip(P, [0, 0.0, 0.80], np.array([0.205, 0.16, 0.14]) * cf('hips'))                                 # bacino
    d = smin(d, ellip(P, [0, -0.05 - 0.04 * (cf('belly') - 1), 0.97 - cf('belly_drop', 0.0)], np.array([0.215, 0.18, 0.18]) * cf('belly')), 0.08)                 # pancia
    d = smin(d, ellip(P, [0, -0.06, 1.16], np.array([0.27, 0.19, 0.17]) * cf('chest')), 0.08)                  # torace a botte
    d = smin(d, ellip(P, [0, 0.05 + 0.03 * (cf('hump') - 1), 1.31 + 0.03 * (cf('hump') - 1)], np.array([0.27, 0.18, 0.18]) * cf('hump')), 0.09)                  # gobba
    d = smin(d, ellip(P, [0, 0.04, 1.42 + 0.05 * (cf('hump') - 1)], np.array([0.14, 0.13, 0.10]) * cf('hump')), 0.08)                  # garrese
    d = smin(d, capsule(P, [0, -0.01, 1.22], H + [0, -0.07, 0.10], 0.15 * cf('neck'), 0.13 * cf('neck')), 0.06)  # collo taurino
    d = smin(d, capsule(Q, [0.0, 0.02, 1.36], L('Arm') + [-0.04, 0.02, 0.05], 0.10 * cf('neck'), 0.09 * cf('arm')), 0.07)  # trapezi
    d = smin(d, ellip(Q, L('Arm') + [0.0, 0.0, 0.01], np.array([0.12, 0.12, 0.11]) * cf('arm')), 0.05)      # deltoidi
    # pettorali
    d = smin(d, ellip(Q, [0.12, -0.17 - 0.03 * (cf('chest') - 1), 1.22 - cf('chest_sag', 0.0)], np.array([0.13, 0.07, 0.09]) * cf('chest')), 0.06)
    # braccia
    d = smin(d, capsule(Q, L('Arm'), L('ForeArm'), 0.105 * cf('arm'), 0.088 * cf('arm')), 0.05)
    d = smin(d, ellip(Q, (L('Arm') * 0.55 + L('ForeArm') * 0.45) + [0, 0.0, -0.025], np.array([0.11, 0.07, 0.07]) * cf('arm')), 0.04)  # bicipite/tricipite
    d = smin(d, capsule(Q, L('ForeArm'), L('Hand'), 0.092 * cf('forearm'), 0.06 * cf('forearm')), 0.04)
    d = smin(d, ellip(Q, (L('ForeArm') * 0.7 + L('Hand') * 0.3) + [0, 0, 0.01], np.array([0.11, 0.075, 0.07]) * cf('forearm')), 0.04)
    # mani tozze
    hand = J['LeftHand']; mid = J['LeftHandMiddle1']
    d = smin(d, ellip(Q, hand * 0.45 + mid * 0.55, np.array([0.07, 0.055, 0.03]) * cf('hand')), 0.03)
    for f, r0 in (('Index', 0.02), ('Middle', 0.021), ('Ring', 0.02), ('Pinky', 0.017), ('Thumb', 0.022)):
        r0 *= cf('hand')
        a = J['LeftHand' + f + '1']; b = J['LeftHand' + f + '2']; c = J['LeftHand' + f + '3']
        tip = ext(b, c, 0.9)
        base = hand if f == 'Thumb' else a
        d = smin(d, capsule(Q, base, b, r0, r0 * 0.95), 0.012)
        d = smin(d, capsule(Q, b, c, r0 * 0.95, r0 * 0.85), 0.008)
        d = smin(d, capsule(Q, c, tip, r0 * 0.85, r0 * 0.7), 0.006)
    # gambe
    d = smin(d, capsule(Q, L('UpLeg'), L('Leg'), 0.135 * cf('leg'), 0.09 * cf('leg')), 0.07)
    d = smin(d, ellip(Q, L('UpLeg') * 0.6 + L('Leg') * 0.4 + [0.0, -0.02, 0], np.array([0.12, 0.12, 0.15]) * cf('leg')), 0.06)
    d = smin(d, capsule(Q, L('Leg'), L('Foot'), 0.09 * cf('leg'), 0.06 * cf('leg')), 0.04)
    d = smin(d, ellip(Q, L('Leg') * 0.7 + L('Foot') * 0.3 + [0, 0.03, 0], np.array([0.08, 0.08, 0.11]) * cf('leg')), 0.04)   # polpaccio
    # piede tozzo: caviglia e pianta, gli zoccoli sono a parte
    foot = L('Foot'); toe = L('ToeBase')
    d = smin(d, capsule(Q, foot + [0, 0.02, -0.04], toe + [0, 0.035, 0.035], 0.065, 0.055), 0.04)
    for sgn in (1, -1):
        t = J['LeftToeBase'] * [sgn, 1, 1]
        g = capsule(P, t + [0, -0.03, -0.02], t + [0, -0.09, 0.06], 0.006, 0.006)
        d = smax(d, -g, 0.006)
    d = np.maximum(d, -(P[..., 2] - 0.0))                                              # niente sotto il suolo
    return d


def tail(P):
    t = J['Tail']
    return capsule(P, t + [0, -0.03, 0.02], t + [0, 0.11, -0.03], 0.022, 0.012)


def field(P):
    return smin(body(P), head(P), 0.035)


def build(step=0.006):
    lo = np.array([-1.22, -0.86, -0.01]); hi = np.array([1.22, 0.48, 1.86])
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
