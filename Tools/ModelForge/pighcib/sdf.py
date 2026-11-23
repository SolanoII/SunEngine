# Forma del Pighcib come campo di distanza: piccolo, a pera, testa grande e tonda senza collo, muso corto e smussato
# col disco del naso dei Vrepigh, guance gonfie, zampette. Stesse primitive del Vrepigh (../vrepigh/sdf.py).
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
SKULL = H + [0, -0.05, 0.08]
MUZZLE = H + [0, -0.15, 0.04]
NOSE = H + [0, -0.212, 0.058]
NOSE_N = np.array([0, -0.93, 0.36]) / np.linalg.norm([0, -0.93, 0.36])
EYE = H + [0.085, -0.095, 0.105]


HS = 1.15


def head(P0):
    P = H + (P0 - H) / HS
    Q = mir(P)
    d = ellip(P, SKULL, [0.12, 0.15, 0.104])                                           # cranio lungo e basso
    d = smin(d, ellip(P, MUZZLE, [0.08, 0.07, 0.068]), 0.045)                          # muso largo e smussato
    d = smin(d, capsule(P, SKULL + [0, -0.02, 0.07], NOSE + [0, 0.035, 0.018], 0.05, 0.024), 0.035)   # naso romano
    d = smin(d, ellip(Q, H + [0.062, -0.12, 0.015], [0.062, 0.068, 0.058]), 0.035)     # guance gonfie
    d = smin(d, disc(P, NOSE, NOSE_N, 0.016, 0.005), 0.012)                            # disco del naso
    d = smin(d, ellip(P, H + [0, -0.135, -0.01], [0.045, 0.05, 0.03]), 0.02)           # mento
    d = smax(d, -ellip(Q, NOSE - NOSE_N * 0.0 + NOSE_N * 0.012 + [0.009, 0, 0], [0.005, 0.008, 0.007]), 0.003)   # narici
    d = smax(d, -capsule(P, NOSE + [0, 0.002, -0.012], H + [0, -0.19, 0.008], 0.0035, 0.0035), 0.004)     # labbro spaccato
    m = capsule(Q, H + [0.0, -0.187, 0.006], H + [0.045, -0.15, 0.006], 0.004, 0.006)                     # bocca
    d = smax(d, -m, 0.004)
    return d * HS


def body(P):
    Q = mir(P)
    d = ellip(P, [0, 0.0, 0.50], [0.175, 0.16, 0.2])                                    # pera
    d = smin(d, ellip(P, [0, -0.035, 0.46], [0.16, 0.14, 0.15]), 0.06)                 # pancia
    d = smin(d, ellip(P, [0, -0.02, 0.63], [0.15, 0.12, 0.09]), 0.06)                  # petto
    d = smin(d, ellip(Q, [0.075, 0.01, 0.40], [0.11, 0.125, 0.12]), 0.05)               # cosce piene
    d = smin(d, ellip(Q, [0.07, 0.0, 0.29], [0.085, 0.09, 0.09]), 0.05)                # ginocchia nel pelo
    # braccia corte e tonde
    d = smin(d, ellip(Q, L('Arm') + [0.0, 0.0, 0.0], [0.065, 0.06, 0.058]), 0.04)
    d = smin(d, capsule(Q, L('Arm'), L('ForeArm'), 0.055, 0.045), 0.03)
    d = smin(d, capsule(Q, L('ForeArm'), L('Hand'), 0.045, 0.032), 0.02)
    hand = J['LeftHand']; mid = J['LeftHandMiddle1']
    d = smin(d, ellip(Q, hand * 0.4 + mid * 0.6, [0.035, 0.03, 0.016]), 0.015)
    for f, r0 in (('Index', 0.009), ('Middle', 0.0095), ('Ring', 0.009), ('Pinky', 0.008), ('Thumb', 0.01)):
        a = J['LeftHand' + f + '1']; b = J['LeftHand' + f + '2']; c = J['LeftHand' + f + '3']
        base = hand if f == 'Thumb' else a
        d = smin(d, capsule(Q, base, b, r0, r0), 0.006)
        d = smin(d, capsule(Q, b, ext(b, c, 0.4), r0, r0 * 0.8), 0.004)
    # gambe: cosce dentro il corpo, stinchi corti e pieni, zampe ovali
    d = smin(d, capsule(Q, L('UpLeg'), L('Leg'), 0.085, 0.06), 0.05)
    d = smin(d, capsule(Q, L('Leg'), L('Foot'), 0.055, 0.04), 0.03)
    foot = L('Foot'); toe = L('ToeBase')
    d = smin(d, capsule(Q, foot + [0, 0.015, -0.035], toe + [0, -0.005, 0.02], 0.038, 0.03), 0.03)
    for k in (-1, 0, 1):                                   # tre dita davanti
        for sgn in (1, -1):
            t = toe * [sgn, 1, 1]
            d = smin(d, ellip(P, t + [0.018 * k * sgn, -0.03, 0.013], [0.012, 0.016, 0.011]), 0.008)
    d = np.maximum(d, -(P[..., 2] - 0.0))
    return d


def field(P):
    return smin(body(P), head(P), 0.07)                   # niente collo: la testa affonda nelle spalle


def build(step=0.004):
    lo = np.array([-0.55, -0.40, -0.01]); hi = np.array([0.55, 0.24, 1.02])
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
