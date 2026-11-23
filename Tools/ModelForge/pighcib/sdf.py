# Forma del Pighcib come campo di distanza: una bestiola viva e arrabbiata, a meta' tra un Vrepigh, un porcellino
# d'India, un ratto e un goblin. Corpo tozzo ma nervoso, gobba da Vrepigh, petto e braccia asciutti, mani grandi da
# goblin; testa da porcellino d'India con il muso piu' lungo e appuntito del ratto, il disco del naso e le zanne dei
# Vrepigh, la fronte aggrottata a V, il naso arricciato e il labbro alzato sui denti.
# Stesse primitive del Vrepigh (../vrepigh/sdf.py).
import os, sys, json, importlib.util, numpy as np
from skimage import measure
_spec = importlib.util.spec_from_file_location('vrepigh_sdf', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vrepigh', 'sdf.py'))
_v = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_v)
smin, smax, capsule, ellip, disc, mir = _v.smin, _v.smax, _v.capsule, _v.ellip, _v.disc, _v.mir

R = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rig.json')))
J = {k: np.array(v) for k, v in R['joints'].items()}


def L(n):
    return J['Left' + n]


def ext(a, b, f):
    return b + (b - a) * f


H = J['Head']
HS = 1.12
SKULL = H + [0, -0.035, 0.08]
MUZZLE = H + [0, -0.2, 0.03]
NOSE = H + [0, -0.282, 0.034]
NOSE_N = np.array([0, -0.96, 0.27]) / np.linalg.norm([0, -0.96, 0.27])
EYE = H + [0.058, -0.112, 0.098]
MOUTH_FRONT = H + [0, -0.262, -0.008]         # bocca: dalla punta del muso...
MOUTH_BACK = H + [0.066, -0.11, 0.0]           # ...all'angolo, dietro le guance
JAW_A = H + [0, -0.06, -0.022]
JAW_B = H + [0, -0.24, -0.024]


def head(P0):
    P = H + (P0 - H) / HS
    Q = mir(P)
    d = ellip(P, SKULL, [0.1, 0.12, 0.085])                                             # cranio da porcellino d'India
    d = smin(d, ellip(Q, H + [0.06, -0.1, 0.02], [0.055, 0.07, 0.055]), 0.04)          # guance piene
    d = smin(d, capsule(sx(P, 1.15), H + [0, -0.09, 0.05], NOSE + [0, 0.02, -0.004], 0.064, 0.026), 0.035)   # muso lungo da ratto
    d = smin(d, capsule(P, SKULL + [0, -0.03, 0.06], NOSE + [0, 0.035, 0.014], 0.038, 0.018), 0.03)            # dorso del naso
    d = smin(d, disc(P, NOSE, NOSE_N, 0.02, 0.006), 0.01)                             # disco del naso dei Vrepigh
    # fronte aggrottata: arcate a V, basse in mezzo e alte ai lati
    d = smin(d, capsule(Q, H + [0.006, -0.14, 0.104], H + [0.06, -0.112, 0.118], 0.017, 0.015), 0.016)
    # naso arricciato: tre pieghe sul dorso
    for k, y in enumerate((-0.17, -0.19, -0.21)):
        d = smin(d, capsule(Q, H + [0.0, y, 0.088 - 0.012 * k], H + [0.026, y + 0.004, 0.076 - 0.012 * k], 0.0045, 0.0035), 0.006)
    # labbro alzato: un rigonfiamento sopra i denti, ai lati del muso
    d = smin(d, capsule(Q, H + [0.02, -0.245, 0.01], H + [0.05, -0.14, 0.022], 0.011, 0.016), 0.01)
    d = smin(d, capsule(P, JAW_A, JAW_B, 0.044, 0.026), 0.025)                         # mandibola
    d = smax(d, -ellip(Q, EYE, [0.02, 0.018, 0.013]), 0.006)                            # occhi stretti sotto le arcate
    d = smax(d, -ellip(Q, NOSE + NOSE_N * 0.012 + [0.009, 0, 0], [0.006, 0.01, 0.008]), 0.003)    # narici
    m = capsule(Q, MOUTH_FRONT, MOUTH_BACK, 0.012, 0.009)                              # ringhio: bocca aperta davanti
    d = smax(d, -m, 0.006)
    return d * HS


def sx(P, k):
    Q = P.copy(); Q[..., 0] = Q[..., 0] * k; return Q


def body(P):
    Q = mir(P)
    d = ellip(P, [0, 0.0, 0.46], [0.13, 0.11, 0.09])                                    # bacino
    d = smin(d, ellip(P, [0, -0.03, 0.48], [0.125, 0.105, 0.1]), 0.05)                  # pancia soda
    d = smin(d, ellip(P, [0, -0.02, 0.6], [0.14, 0.11, 0.11]), 0.05)                   # petto
    d = smin(d, ellip(Q, [0.06, -0.085, 0.61], [0.06, 0.04, 0.05]), 0.03)              # pettorali asciutti
    d = smin(d, ellip(P, [0, 0.065, 0.67], [0.14, 0.1, 0.1]), 0.06)                    # gobba da Vrepigh
    d = smin(d, capsule(Q, [0.0, 0.03, 0.7], L('Arm') + [-0.01, 0.01, 0.02], 0.05, 0.04), 0.04)   # trapezi
    # braccia nervose, avambracci forti, mani grandi da goblin con dita lunghe
    d = smin(d, ellip(Q, L('Arm'), [0.05, 0.05, 0.047]), 0.03)
    d = smin(d, capsule(Q, L('Arm'), L('ForeArm'), 0.038, 0.027), 0.02)
    d = smin(d, ellip(Q, L('Arm') * 0.55 + L('ForeArm') * 0.45 + [0, 0, -0.01], [0.05, 0.03, 0.03]), 0.02)
    d = smin(d, capsule(Q, L('ForeArm'), L('Hand'), 0.032, 0.02), 0.015)
    d = smin(d, ellip(Q, L('ForeArm') * 0.7 + L('Hand') * 0.3, [0.05, 0.034, 0.03]), 0.015)
    hand = J['LeftHand']; mid = J['LeftHandMiddle1']
    d = smin(d, ellip(Q, hand * 0.45 + mid * 0.55, [0.036, 0.03, 0.014]), 0.012)
    for f, r0 in (('Index', 0.0075), ('Middle', 0.008), ('Ring', 0.0075), ('Pinky', 0.0065), ('Thumb', 0.008)):
        a = J['LeftHand' + f + '1']; b = J['LeftHand' + f + '2']; c = J['LeftHand' + f + '3']
        base = hand if f == 'Thumb' else a
        tip = ext(b, c, 1.2)
        d = smin(d, capsule(Q, base, b, r0, r0 * 0.9), 0.005)
        d = smin(d, ellip(Q, b, [r0 * 1.2] * 3), 0.003)
        d = smin(d, capsule(Q, b, c, r0 * 0.9, r0 * 0.8), 0.003)
        d = smin(d, capsule(Q, c, tip, r0 * 0.8, r0 * 0.6), 0.003)
    # gambe: cosce piene, polpacci asciutti, piedi lunghi da ratto
    d = smin(d, capsule(Q, L('UpLeg'), L('Leg'), 0.068, 0.04), 0.04)
    d = smin(d, ellip(Q, L('UpLeg') * 0.55 + L('Leg') * 0.45 + [0, 0.0, 0], [0.065, 0.07, 0.08]), 0.03)
    d = smin(d, capsule(Q, L('Leg'), L('Foot'), 0.035, 0.024), 0.015)
    d = smin(d, ellip(Q, L('Leg') * 0.7 + L('Foot') * 0.3 + [0, 0.012, 0], [0.034, 0.034, 0.05]), 0.012)
    foot = L('Foot'); toe = L('ToeBase')
    d = smin(d, capsule(Q, foot + [0, 0.02, -0.04], toe + [0, -0.01, -0.0], 0.028, 0.02), 0.02)
    for k in (-1, 0, 1):
        for sgn in (1, -1):
            t = toe * [sgn, 1, 1]
            d = smin(d, capsule(P, t + [0.012 * k * sgn, 0.0, 0.0], t + [0.02 * k * sgn, -0.05, -0.0], 0.0085, 0.0055), 0.005)
    d = np.maximum(d, -(P[..., 2] - 0.0))
    return d


def field(P):
    return smin(body(P), head(P), 0.05)


def build(step=0.0035):
    lo = np.array([-0.56, -0.46, -0.01]); hi = np.array([0.56, 0.24, 1.0])
    n = np.ceil((hi - lo) / step).astype(int) + 1
    xs = [lo[i] + np.arange(n[i]) * step for i in range(3)]
    F = np.empty(n, np.float32)
    Y, Z = np.meshgrid(xs[1], xs[2], indexing='ij')
    for i, x in enumerate(xs[0]):
        F[i] = field(np.stack([np.full_like(Y, x), Y, Z], -1))
    v, f, _, _ = measure.marching_cubes(F, 0.0, spacing=(step,) * 3)
    return v + lo, f[:, ::-1]


if __name__ == '__main__':
    v, f = build()
    np.savez('shape.npz', v=v, f=f, key='')
    print(len(v), len(f))
