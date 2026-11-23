# Forma del Pighcib come campo di distanza: un porcellino d'India guastato dalla pietra. Gobbo, costole in vista
# sopra una pancia gonfia e cadente, vertebre a nodi lungo la schiena, bubboni, arti magri e nodosi, dita lunghe.
# Testa da porcellino d'India stirata e scavata: arcate pesanti su orbite infossate, zigomi aguzzi, una bocca larga
# da goblin che taglia le guance, disco del naso dei Vrepigh. Stesse primitive del Vrepigh (../vrepigh/sdf.py).
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
SKULL = H + [0, -0.05, 0.08]
MUZZLE = H + [0, -0.165, 0.04]
NOSE = H + [0, -0.228, 0.052]
NOSE_N = np.array([0, -0.95, 0.3]) / np.linalg.norm([0, -0.95, 0.3])
EYE = H + [0.06, -0.12, 0.105]
MOUTH_FRONT = H + [0, -0.212, 0.002]          # angolo davanti della bocca (in mezzo)
MOUTH_BACK = H + [0.082, -0.08, 0.012]         # angolo dietro, sulle guance
JAW_A = H + [0, -0.06, -0.02]
JAW_B = H + [0, -0.2, -0.018]


def head(P0):
    P = H + (P0 - H) / HS
    Q = mir(P)
    d = ellip(P, SKULL, [0.098, 0.15, 0.09])                                            # cranio stretto e lungo
    d = smin(d, ellip(P, MUZZLE, [0.064, 0.08, 0.058]), 0.04)                          # muso lungo
    d = smin(d, capsule(P, SKULL + [0, -0.03, 0.068], NOSE + [0, 0.03, 0.02], 0.045, 0.022), 0.03)   # naso a cuneo
    d = smin(d, capsule(Q, H + [0.0, -0.11, 0.135], H + [0.07, -0.1, 0.125], 0.024, 0.02), 0.02)     # arcate pesanti
    d = smin(d, ellip(Q, H + [0.072, -0.1, 0.05], [0.034, 0.055, 0.03]), 0.02)          # zigomi aguzzi
    d = smin(d, disc(P, NOSE, NOSE_N, 0.017, 0.006), 0.01)                             # disco del naso
    d = smin(d, capsule(P, JAW_A, JAW_B, 0.048, 0.03), 0.025)                          # mandibola
    d = smax(d, -ellip(Q, H + [0.088, -0.035, 0.1], [0.03, 0.05, 0.04]), 0.02)          # tempie scavate
    d = smax(d, -ellip(Q, EYE, [0.024, 0.022, 0.02]), 0.006)                            # orbite infossate
    d = smax(d, -ellip(Q, NOSE + NOSE_N * 0.012 + [0.009, 0, 0], [0.006, 0.01, 0.008]), 0.003)    # narici
    m = capsule(Q, MOUTH_FRONT, MOUTH_BACK, 0.009, 0.013)                              # bocca larga fino alle guance
    d = smax(d, -m, 0.006)
    return d * HS


def body(P):
    Q = mir(P)
    d = ellip(P, [0, 0.01, 0.46], [0.12, 0.1, 0.08])                                    # bacino stretto
    d = smin(d, ellip(P, [0, -0.05, 0.45], [0.15, 0.14, 0.13]), 0.05)                  # pancia gonfia
    d = smin(d, ellip(P, [0, -0.045, 0.385], [0.12, 0.11, 0.075]), 0.05)               # che cade
    d = smin(d, ellip(P, [0, -0.015, 0.615], [0.125, 0.105, 0.1]), 0.05)               # gabbia toracica
    d = smin(d, ellip(P, [0, 0.07, 0.665], [0.13, 0.095, 0.095]), 0.06)                # gobba
    # costole: archi sui fianchi e sul davanti del petto
    for z in (0.57, 0.595, 0.62, 0.645):
        k_ = np.sqrt(max(1 - ((z - 0.615) / 0.1) ** 2, 0.05))
        pts = [np.array([0.125 * k_ * np.cos(t), -0.015 + 0.105 * k_ * np.sin(t), z - 0.012 * (t + 1.45)]) for t in np.linspace(-0.95, 0.35, 6)]
        for k in range(len(pts) - 1):
            for sgn in (1, -1):
                d = smin(d, capsule(P, pts[k] * [sgn, 1, 1], pts[k + 1] * [sgn, 1, 1], 0.0055, 0.0055), 0.009)
    # vertebre a nodi
    for z in np.arange(0.44, 0.73, 0.034):
        y = 0.085 + 0.06 * np.clip((z - 0.5) / 0.2, 0, 1)
        d = smin(d, ellip(P, [0, y, z], [0.018, 0.018, 0.014]), 0.01)
    # bubboni
    d = smin(d, ellip(P, [0.088, 0.035, 0.705], [0.045, 0.04, 0.042]), 0.02)
    d = smin(d, ellip(P, [-0.112, 0.07, 0.55], [0.036, 0.034, 0.04]), 0.015)
    d = smin(d, ellip(P, [0.128, -0.065, 0.47], [0.028, 0.026, 0.03]), 0.012)
    d = smin(d, ellip(P, [-0.1, -0.12, 0.41], [0.024, 0.022, 0.026]), 0.01)
    # braccia magre, gomiti a nodo, mani ossute con dita lunghe
    d = smin(d, ellip(Q, L('Arm'), [0.045, 0.045, 0.042]), 0.03)
    d = smin(d, capsule(Q, L('Arm'), L('ForeArm'), 0.034, 0.024), 0.02)
    d = smin(d, ellip(Q, L('ForeArm'), [0.028, 0.026, 0.026]), 0.012)
    d = smin(d, capsule(Q, L('ForeArm'), L('Hand'), 0.026, 0.017), 0.012)
    hand = J['LeftHand']; mid = J['LeftHandMiddle1']
    d = smin(d, ellip(Q, hand * 0.45 + mid * 0.55, [0.03, 0.024, 0.011]), 0.01)
    for f, r0 in (('Index', 0.0065), ('Middle', 0.007), ('Ring', 0.0065), ('Pinky', 0.0058), ('Thumb', 0.007)):
        a = J['LeftHand' + f + '1']; b = J['LeftHand' + f + '2']; c = J['LeftHand' + f + '3']
        base = hand if f == 'Thumb' else a
        tip = ext(b, c, 1.3)
        d = smin(d, capsule(Q, base, b, r0, r0 * 0.9), 0.004)
        d = smin(d, ellip(Q, b, [r0 * 1.3] * 3), 0.003)                                # nocche
        d = smin(d, capsule(Q, b, c, r0 * 0.9, r0 * 0.8), 0.003)
        d = smin(d, capsule(Q, c, tip, r0 * 0.8, r0 * 0.6), 0.003)
    # gambe magre, ginocchia a nodo, piedi lunghi
    d = smin(d, capsule(Q, L('UpLeg'), L('Leg'), 0.062, 0.036), 0.04)
    d = smin(d, ellip(Q, L('Leg') + [0, -0.008, 0], [0.034, 0.034, 0.032]), 0.012)
    d = smin(d, capsule(Q, L('Leg'), L('Foot'), 0.03, 0.022), 0.015)
    foot = L('Foot'); toe = L('ToeBase')
    d = smin(d, capsule(Q, foot + [0, 0.02, -0.04], toe + [0, -0.01, -0.0], 0.026, 0.018), 0.02)
    for k in (-1, 0, 1):
        for sgn in (1, -1):
            t = toe * [sgn, 1, 1]
            d = smin(d, capsule(P, t + [0.012 * k * sgn, 0.0, 0.0], t + [0.02 * k * sgn, -0.05, -0.0], 0.008, 0.005), 0.005)
    d = np.maximum(d, -(P[..., 2] - 0.0))
    return d


def field(P):
    return smin(body(P), head(P), 0.05)


def build(step=0.0035):
    lo = np.array([-0.56, -0.42, -0.01]); hi = np.array([0.56, 0.24, 1.0])
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
