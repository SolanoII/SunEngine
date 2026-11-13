# Genera le texture di dettaglio di GameData/Textures/Surfaces (albedo relativo, normale, dati: altezza, rugosita', occlusione).
# Uso: python genera_texture.py <cartella di uscita> [nomi...]   (servono numpy, scipy, pillow)
import numpy as np, sys, os
from scipy.spatial import cKDTree
from scipy.ndimage import map_coordinates, gaussian_filter
from PIL import Image

N = int(os.environ.get('RES', 1024))
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) / N   # coordinate 0..1 (ripetibili)

def rng(s): return np.random.default_rng(s)

def band(seed, f0, f1, aniso=(1.0, 1.0)):
    """rumore periodico a banda limitata (frequenze in cicli per tile), normalizzato 0..1"""
    r = rng(seed)
    w = r.standard_normal((N, N)).astype(np.float32)
    F = np.fft.fft2(w)
    fy = np.fft.fftfreq(N) * N * aniso[1]; fx = np.fft.fftfreq(N) * N * aniso[0]
    R = np.sqrt(fx[None, :] ** 2 + fy[:, None] ** 2)
    m = np.clip((R - f0 * 0.7) / (f0 * 0.3 + 1e-6), 0, 1) * np.clip((f1 * 1.3 - R) / (f1 * 0.3 + 1e-6), 0, 1)
    v = np.real(np.fft.ifft2(F * m)).astype(np.float32)
    v = (v - v.mean()) / (v.std() + 1e-8)
    return np.clip(v * 0.18 + 0.5, 0, 1)

def fbm(seed, f0, octaves=5, gain=0.5, aniso=(1.0, 1.0)):
    v = np.zeros((N, N), np.float32); a = 1.0; tot = 0
    for i in range(octaves):
        v += a * (band(seed + i * 31, f0 * 2 ** i, f0 * 2 ** (i + 1), aniso) - 0.5); tot += a; a *= gain
    return v / tot + 0.5

def ss(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)

def voronoi(seed, count, jitter_pts=None):
    r = rng(seed)
    pts = r.random((count, 2)) if jitter_pts is None else jitter_pts
    tree = cKDTree(pts, boxsize=1.0)
    q = np.stack([xx.ravel(), yy.ravel()], 1)
    d, i = tree.query(q, k=2, workers=2)
    F1 = d[:, 0].reshape(N, N).astype(np.float32); F2 = d[:, 1].reshape(N, N).astype(np.float32)
    idx = i[:, 0].reshape(N, N)
    return F1, F2, idx, pts

def warp(img, dx, dy):
    return map_coordinates(img, [(yy + dy) * N, (xx + dx) * N], order=1, mode='grid-wrap').astype(np.float32)

def blur(img, s): return gaussian_filter(img, s, mode='wrap')

def normal_from_h(h, strength):
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    n = np.stack([-gx, gy, np.ones_like(h)], -1)   # +Y in alto nell'immagine (convenzione OpenGL dopo il flip)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n

def cavity_ao(h, s1=2.0, s2=10.0, k=6.0):
    c = (blur(h, s2) - h)
    return np.clip(1.0 - np.maximum(c, 0) * k, 0.35, 1.0)

def lines(seed, count, lenr, width, angle=None, angle_jit=np.pi):
    """graffi: segmenti sottili (ripetibili). Ritorna maschera 0..1"""
    r = rng(seed); m = np.zeros((N, N), np.float32)
    for _ in range(count):
        cx, cy = r.random(2)
        a = (angle if angle is not None else r.random() * np.pi) + (r.random() - 0.5) * angle_jit
        L = r.uniform(*lenr); w = width * r.uniform(0.6, 1.4)
        steps = int(L * N * 1.5) + 2
        t = np.linspace(-0.5, 0.5, steps)
        # leggera curva
        bend = r.uniform(-0.004, 0.004)
        px = cx + np.cos(a) * L * t - np.sin(a) * bend * (t * 2) ** 2
        py = cy + np.sin(a) * L * t + np.cos(a) * bend * (t * 2) ** 2
        ix = (px * N).astype(int) % N; iy = (py * N).astype(int) % N
        inten = r.uniform(0.4, 1.0) * np.sin(np.pi * (t + 0.5)) ** 0.5
        np.maximum.at(m, (iy, ix), inten)
    return np.clip(blur(m, width * N * 0.5 + 0.4) * (width * N + 1.0) * 1.6, 0, 1)

def save(name, alb, h, rough, ao, nstrength):
    # albedo relativo: ogni canale diviso per la sua media (0.5 = colore del motore invariato)
    alb = alb * ao[..., None] ** 0.5
    alb = alb / alb.reshape(-1, 3).mean(0) * 0.5
    a8 = (np.clip(alb, 0, 1) * 255 + 0.5).astype(np.uint8)
    h = (h - h.min()) / (h.max() - h.min() + 1e-8)
    n = normal_from_h(h, nstrength)
    n8 = ((n * 0.5 + 0.5) * 255 + 0.5).astype(np.uint8)
    rough = np.clip(rough - rough.mean() + 0.5, 0, 1)
    d8 = (np.stack([h, rough, ao], -1) * 255 + 0.5).astype(np.uint8)
    q = int(os.environ.get('Q', 90))
    Image.fromarray(a8).save(f'{OUT}/{name}_albedo.jpg', quality=q, subsampling=0)
    Image.fromarray(n8).save(f'{OUT}/{name}_normal.jpg', quality=q, subsampling=0)
    Image.fromarray(d8).save(f'{OUT}/{name}_data.jpg', quality=q, subsampling=0)
    print(name, 'ok', flush=True)

def lerp(a, b, t): return a + (b - a) * t
def C(*c): return np.array(c, np.float32)

# ------------------------------------------------------------------ terra
def soil():
    base = fbm(1, 3, 5)
    wx = (fbm(17, 6, 3) - 0.5) * 0.03; wy = (fbm(18, 6, 3) - 0.5) * 0.03
    F1, F2, idx, _ = voronoi(2, 45)
    F1 = warp(F1, wx, wy); F2 = warp(F2, wx, wy)
    crackmask = ss(0.52, 0.66, fbm(3, 2, 3))
    crack = (1 - ss(0.0, 0.010, F2 - F1)) * crackmask
    clods = fbm(19, 10, 4)                               # zolle e grumi
    grit = band(4, 180, 450)
    r = rng(5); M = 2600
    pts = r.random((M, 2)); rad = r.uniform(0.0015, 0.008, M) ** 1.0 * (r.random(M) < 0.45)
    tree = cKDTree(pts, boxsize=1.0)
    qx = (xx + (band(20, 40, 90) - 0.5) * 0.012) % 1.0; qy = (yy + (band(21, 40, 90) - 0.5) * 0.012) % 1.0
    d, i = tree.query(np.stack([qx.ravel(), qy.ravel()], 1), k=1, workers=2)
    d = d.reshape(N, N); i = i.reshape(N, N); rr = rad[i]
    peb = np.clip(1 - (d / np.maximum(rr, 1e-5)) ** 2, 0, 1) * (rr > 0)
    embed = r.random(M)[i]                                # quanto il sasso affiora dalla terra
    peb = np.clip(peb - embed * 0.5, 0, 1) / np.maximum(1 - embed * 0.5, 0.3)
    pebdome = np.sqrt(peb)
    tone = r.random(M)[i]
    h = base * 0.5 + clods * 0.3 + grit * 0.06 + pebdome * 0.22 - crack * 0.18
    soilc = lerp(C(0.30, 0.24, 0.18), C(0.22, 0.18, 0.14), fbm(7, 4, 4)[..., None])
    soilc = soilc * (0.8 + 0.4 * clods)[..., None] * (0.85 + 0.3 * grit)[..., None]
    pebc = lerp(C(0.40, 0.37, 0.33), C(0.24, 0.21, 0.18), tone[..., None]) * (0.85 + 0.3 * band(9, 200, 500))[..., None]
    alb = lerp(soilc, pebc, ss(0.0, 0.35, peb)[..., None])
    specks = ss(0.78, 0.86, band(10, 150, 350))
    alb *= (1 - 0.35 * specks)[..., None]
    alb *= (1 - 0.45 * crack)[..., None]
    rough = 0.6 - 0.12 * ss(0.0, 0.3, peb) + 0.1 * grit
    ao = cavity_ao(h, k=5.0)
    save('soil', alb, h, rough, ao, 7.0)

# ------------------------------------------------------------------ cemento
def concrete():
    paste = fbm(11, 3, 5)
    sand = band(12, 250, 500)
    r = rng(13); M = 5000
    pts = r.random((M, 2)); rad = r.uniform(0.0015, 0.009, M)
    tree = cKDTree(pts, boxsize=1.0)
    d, i = tree.query(np.stack([xx.ravel(), yy.ravel()], 1), k=1, workers=2)
    d = d.reshape(N, N); i = i.reshape(N, N)
    expose = ss(0.35, 0.65, fbm(14, 2, 3))              # zone dove l'inerte affiora
    stone = (d < rad[i] * (0.4 + 0.8 * expose)).astype(np.float32)
    stone = blur(stone, 0.7)
    tone = r.random(M)[i]
    pores_p = r.random((2500, 2)); pr = r.uniform(0.0008, 0.0035, 2500)
    t2 = cKDTree(pores_p, boxsize=1.0)
    d2, i2 = t2.query(np.stack([xx.ravel(), yy.ravel()], 1), k=1, workers=2)
    d2 = d2.reshape(N, N); i2 = i2.reshape(N, N)
    pore = np.clip(1 - (d2 / pr[i2]) ** 2, 0, 1)
    h = paste * 0.4 + sand * 0.12 + stone * 0.15 - pore * 0.6 + fbm(15, 16, 4) * 0.2
    pc = C(0.50, 0.49, 0.46) * (0.88 + 0.24 * paste)[..., None] * (0.92 + 0.16 * sand)[..., None]
    sc = lerp(C(0.36, 0.35, 0.34), C(0.60, 0.56, 0.50), tone[..., None])
    alb = lerp(pc, sc, stone[..., None])
    alb *= (1 - 0.55 * ss(0.1, 0.6, pore))[..., None]
    stain = ss(0.55, 0.75, fbm(16, 3, 4))
    alb *= (1 - 0.18 * stain)[..., None]
    rough = 0.6 + 0.15 * sand - 0.1 * stone + 0.1 * pore
    ao = cavity_ao(h, k=4.0)
    save('concrete', alb, h, rough, ao, 5.0)

# ------------------------------------------------------------------ lamiera verniciata
def metal():
    peel = band(21, 60, 160)
    mott = fbm(22, 3, 4)
    pitmask = ss(0.5, 0.7, fbm(23, 3, 4))
    pits = ss(0.80, 0.9, band(24, 200, 450)) * pitmask
    scr = lines(25, 260, (0.02, 0.12), 0.0012)
    scr2 = lines(26, 900, (0.005, 0.03), 0.0008)
    chips = ss(0.60, 0.64, fbm(27, 6, 5)) * ss(0.4, 0.7, fbm(28, 2, 3))
    h = peel * 0.05 + mott * 0.05 - pits * 0.25 - scr * 0.12 - scr2 * 0.06 - chips * 0.2
    paint = C(0.42, 0.44, 0.40) * (0.9 + 0.2 * mott)[..., None]
    alb = paint * (1 - 0.06 * peel)[..., None]
    rustc = lerp(C(0.42, 0.22, 0.10), C(0.22, 0.12, 0.07), band(29, 30, 120)[..., None])
    alb = lerp(alb, rustc, np.clip(pits * 0.9 + chips * 0.6, 0, 1)[..., None])
    alb = lerp(alb, C(0.62, 0.61, 0.60), np.clip(scr * 0.6 + scr2 * 0.35, 0, 1)[..., None])
    rough = 0.55 + 0.08 * peel + 0.25 * pits + 0.15 * chips - 0.25 * scr
    ao = cavity_ao(h, k=3.0)
    save('metal', alb, h, rough, ao, 4.0)

# ------------------------------------------------------------------ juta (sacchi)
def weave_plain(nthreads, seed, twill=False, fibre_aniso=12.0, gap=0.18, jit=0.12):
    u = xx * nthreads; v = yy * nthreads
    # filo irregolare: spessore e posizione variano lungo il filo
    ox = (band(seed, 2, 6, (1.0, 4.0)) - 0.5) * jit * 2
    oy = (band(seed + 1, 2, 6, (4.0, 1.0)) - 0.5) * jit * 2
    fu = (u + ox) % 1.0; fv = (v + oy) % 1.0
    iu = np.floor(u + ox).astype(int); iv = np.floor(v + oy).astype(int)
    warp_p = np.clip(1 - ((fu - 0.5) / (0.5 - gap * 0.5)) ** 2, 0, 1) ** 0.5   # profilo del filo verticale
    weft_p = np.clip(1 - ((fv - 0.5) / (0.5 - gap * 0.5)) ** 2, 0, 1) ** 0.5
    if twill: over = ((iu + iv) % 4) < 2
    else: over = ((iu + iv) % 2) == 0
    # sopra/sotto: il filo che passa sopra sale al centro dell'incrocio
    archw = np.sin(np.pi * fv); archf = np.sin(np.pi * fu)
    hw = warp_p * (0.55 + 0.45 * np.where(over, archw, 1 - archw))
    hf = weft_p * (0.55 + 0.45 * np.where(over, 1 - archf, archf))
    top_warp = hw >= hf
    h = np.maximum(hw, hf)
    fib_w = band(seed + 2, 60, 200, (1.0, fibre_aniso))    # fibre lungo il filo
    fib_f = band(seed + 3, 60, 200, (fibre_aniso, 1.0))
    fib = np.where(top_warp, fib_w, fib_f)
    tid = np.where(top_warp, iu, iv + 7919)
    return h, fib, top_warp, tid

def burlap():
    h0, fib, tw, tid = weave_plain(40, 31, gap=0.28, jit=0.18)
    fuzz = band(32, 120, 400)
    tone = (np.sin(tid * 12.9898) * 43758.5453) % 1.0
    h = h0 * 0.75 + fib * 0.2 + fuzz * 0.05
    c = lerp(C(0.46, 0.37, 0.24), C(0.36, 0.28, 0.18), tone[..., None]) * (0.75 + 0.5 * fib)[..., None]
    c *= (0.35 + 0.65 * ss(0.0, 0.35, h0))[..., None]   # buchi fra i fili scuri
    c *= (0.9 + 0.2 * fbm(33, 3, 3))[..., None]
    rough = 0.85 + 0.1 * fuzz
    ao = cavity_ao(h, 1.0, 4.0, 3.0)
    save('burlap', c, h, rough, ao, 6.0)

# ------------------------------------------------------------------ legno (venatura lungo Y)
def wood():
    wv = fbm(41, 2, 4, aniso=(1.0, 10.0))
    rings = np.sin((xx * 22 + wv * 3.0 + band(46, 1, 3, (1.0, 3.0)) * 2.0) * 2 * np.pi) * 0.5 + 0.5
    rings = rings ** 1.6
    fibres = band(42, 150, 450, (1.0, 25.0))
    fibres2 = band(43, 40, 120, (1.0, 15.0))
    # assi: 4 tavole per tile con fuga
    plank = (xx * 4) % 1.0
    joint = 1 - ss(0.0, 0.012, np.minimum(plank, 1 - plank))
    pid = np.floor(xx * 4).astype(int)
    ptone = np.array([0.0, 0.6, 0.25, 0.85])[pid % 4]
    cr = lines(44, 60, (0.05, 0.25), 0.0015, angle=np.pi / 2, angle_jit=0.08)
    h = rings * 0.25 + fibres * 0.25 + fibres2 * 0.2 - joint * 0.7 - cr * 0.3
    c = lerp(C(0.42, 0.30, 0.20), C(0.27, 0.18, 0.11), rings[..., None])
    c = c * lerp(1.0, 0.82, ptone)[..., None] * (0.85 + 0.3 * fibres)[..., None]
    c *= (1 - 0.7 * joint - 0.5 * cr)[..., None]
    weather = ss(0.5, 0.7, fbm(45, 3, 4))
    c = lerp(c, C(0.40, 0.38, 0.35) * (0.8 + 0.4 * fibres)[..., None], weather[..., None] * 0.4)
    rough = 0.7 + 0.15 * fibres + 0.1 * weather
    ao = cavity_ao(h, 1.0, 6.0, 3.0)
    save('wood', c, h, rough, ao, 6.0)

# ------------------------------------------------------------------ foglie secche / erba morta
def foliage():
    r = rng(51)
    h = np.full((N, N), -1.0, np.float32); col = np.zeros((N, N, 3), np.float32)
    h += fbm(52, 4, 4) * 0.2
    col[:] = C(0.10, 0.08, 0.06)
    palette = [C(0.38, 0.33, 0.17), C(0.30, 0.22, 0.12), C(0.42, 0.36, 0.22), C(0.24, 0.24, 0.13), C(0.47, 0.40, 0.25)]
    for k in range(4200):
        L = r.uniform(0.012, 0.05); W = L * r.uniform(0.12, 0.45)
        if r.random() < 0.35: W = L * 0.07; L *= 1.6                  # fili d'erba
        a = r.random() * np.pi
        cx, cy = r.random(2); z = r.random()
        R = int((L * 0.6) * N) + 2
        ix0 = int(cx * N); iy0 = int(cy * N)
        ys = (np.arange(iy0 - R, iy0 + R)) ; xs = (np.arange(ix0 - R, ix0 + R))
        X, Y = np.meshgrid(xs, ys)
        dx = X / N - cx; dy = Y / N - cy
        lu = dx * np.cos(a) + dy * np.sin(a); lv = -dx * np.sin(a) + dy * np.cos(a)
        e = (lu / (L * 0.5)) ** 2 + (lv / (W * 0.5)) ** 2
        inside = e < 1
        if not inside.any(): continue
        dome = np.sqrt(np.clip(1 - e, 0, 1))
        curl = r.uniform(-0.3, 0.3) * (lu / (L * 0.5)) ** 2
        vein = 1 - ss(0.0, 0.08, np.abs(lv) / (W * 0.5 + 1e-6))
        hz = z + dome * 0.08 + curl * 0.05
        yi = Y % N; xi = X % N
        cur = h[yi, xi]
        upd = inside & (hz > cur)
        if not upd.any(): continue
        h[yi[upd], xi[upd]] = hz[upd]
        pc = palette[r.integers(len(palette))] * r.uniform(0.75, 1.15)
        shade = (0.75 + 0.35 * dome - 0.12 * vein)[upd]
        col[yi[upd], xi[upd]] = pc[None, :] * shade[:, None] * (0.6 + 0.4 * z)
    h = np.clip(h, -0.2, 1.1)
    fine = band(53, 200, 450)
    h = h + fine * 0.03
    col *= (0.9 + 0.2 * fine)[..., None]
    rough = 0.8 + 0.1 * fine
    ao = cavity_ao(h, 2.0, 8.0, 2.0)
    save('foliage', col, h, rough, ao, 7.0)

# ------------------------------------------------------------------ ghiaia / macerie
def gravel():
    r = rng(61)
    h = np.zeros((N, N), np.float32); c = np.zeros((N, N, 3), np.float32); cov = np.zeros((N, N), np.float32)
    for layer, (cnt, w) in enumerate([(260, 0.05), (900, 0.025)]):
        wx = (band(63 + layer, 6, 20) - 0.5) * 0.02; wy = (band(65 + layer, 6, 20) - 0.5) * 0.02
        F1, F2, idx, pts = voronoi(61 + layer * 10, cnt)
        edge = warp(F2 - F1, wx, wy); idx = idx
        stone = ss(0.0, w * 0.6, edge)
        keep = (r.random(cnt) < (0.75 if layer == 0 else 0.9))[idx]
        dome = np.sqrt(np.clip(stone, 0, 1)) * keep * (0.7 + 0.3 * r.random(cnt)[idx]) + layer * -0.25
        tone = r.random(cnt)[idx]
        cc = lerp(C(0.50, 0.48, 0.45), C(0.28, 0.26, 0.24), tone[..., None])
        cc = lerp(cc, C(0.44, 0.33, 0.25), (r.random(cnt)[idx] < 0.2)[..., None] * 0.7)
        cc *= (0.75 + 0.35 * stone)[..., None]
        upd = dome > h
        h = np.where(upd, dome, h); c = np.where(upd[..., None], cc, c); cov = np.maximum(cov, stone * keep)
    surf = band(62, 80, 300)
    h = h + surf * 0.06
    c *= (0.85 + 0.3 * surf)[..., None]
    c = lerp(C(0.10, 0.085, 0.07), c, ss(0.0, 0.35, cov)[..., None])
    rough = 0.7 + 0.15 * surf + 0.1 * (1 - cov)
    ao = cavity_ao(h, 2.0, 7.0, 3.0)
    save('gravel', c, h, rough, ao, 6.0)

# ------------------------------------------------------------------ stoffa (saia)
def fabric():
    h0, fib, tw, tid = weave_plain(64, 71, twill=True, fibre_aniso=10.0, gap=0.1, jit=0.06)
    fuzz = band(72, 150, 450)
    pill = ss(0.82, 0.9, band(73, 60, 140)) * ss(0.4, 0.7, fbm(74, 2, 3))   # pallini di lana consumata
    h = h0 * 0.7 + fib * 0.2 + fuzz * 0.06 + pill * 0.2
    tone = ((np.sin(tid * 78.233) * 43758.5453) % 1.0)
    c = C(0.5, 0.5, 0.5) * (0.88 + 0.12 * tone)[..., None] * (0.8 + 0.4 * fib)[..., None]
    c *= (0.55 + 0.45 * ss(0.0, 0.3, h0))[..., None]
    wornout = ss(0.55, 0.75, fbm(75, 3, 4))
    c = lerp(c, c * 1.12, wornout[..., None])   # tessuto lisciato e sbiadito
    rough = 0.85 + 0.1 * fuzz - 0.15 * wornout
    ao = cavity_ao(h, 1.0, 3.0, 3.0)
    save('fabric', c, h, rough, ao, 5.0)

# ------------------------------------------------------------------ armatura (piastra fusa/martellata)
def armour():
    F1, F2, idx, _ = voronoi(81, 160)
    dimple = -(1 - ss(0.0, 0.06, F1)) * 0.0 + F1 ** 2 * 60    # martellature basse
    dimple = (dimple - dimple.mean()) / dimple.std()
    cast = fbm(82, 6, 5)
    pits = ss(0.84, 0.92, band(83, 250, 500)) * ss(0.45, 0.7, fbm(84, 3, 3))
    scr = lines(85, 500, (0.01, 0.08), 0.0007)
    brush = band(86, 120, 400, (1.0, 30.0))
    h = dimple * 0.04 + cast * 0.25 - pits * 0.35 - scr * 0.12 + brush * 0.04
    c = C(0.5, 0.5, 0.5) * (0.9 + 0.15 * cast)[..., None] * (0.96 + 0.08 * brush)[..., None]
    c *= (1 - 0.45 * pits)[..., None]
    c = lerp(c, C(0.65, 0.65, 0.65), (scr * 0.5)[..., None])
    rough = 0.5 + 0.2 * cast + 0.2 * pits - 0.3 * scr + 0.06 * brush
    ao = cavity_ao(h, 1.0, 5.0, 3.0)
    save('armour', c, h, rough, ao, 3.0)

# ------------------------------------------------------------------ pelle (pori e rughe sottili)
def skin():
    r = rng(91)
    pp = r.random((3500, 2)); prr = r.uniform(0.0015, 0.004, 3500)
    tree = cKDTree(pp, boxsize=1.0)
    d, i = tree.query(np.stack([xx.ravel(), yy.ravel()], 1), k=1, workers=2)
    d = d.reshape(N, N); i = i.reshape(N, N)
    pore = np.clip(1 - (d / prr[i]) ** 2, 0, 1)
    # rughe incrociate: creste di rumore anisotropo in due direzioni
    w1 = 1 - ss(0.0, 0.03, np.abs(band(92, 25, 70, (1.0, 4.0)) - 0.5))
    w2 = 1 - ss(0.0, 0.03, np.abs(band(93, 25, 70, (4.0, 1.0)) - 0.5))
    wr = np.maximum(w1, w2 * 0.8)
    F1, F2, idx, _ = voronoi(94, 900)
    cells = ss(0.0, 0.01, F2 - F1)                     # micro-rilievo a celle della pelle
    mott = fbm(95, 3, 4)
    h = cells * 0.2 - wr * 0.12 - pore * 0.5 + band(96, 150, 400) * 0.08
    c = C(0.5, 0.5, 0.5) * (0.94 + 0.12 * mott)[..., None]
    c = lerp(c, c * C(1.05, 0.9, 0.88), ss(0.55, 0.75, fbm(97, 4, 3))[..., None])   # capillari arrossati
    c *= (1 - 0.18 * pore - 0.04 * wr)[..., None]
    rough = 0.5 + 0.2 * pore + 0.15 * wr - 0.1 * cells
    ao = cavity_ao(h, 1.0, 4.0, 2.0)
    save('skin', c, h, rough, ao, 3.0)

todo = sys.argv[2:] or ['soil', 'concrete', 'metal', 'burlap', 'wood', 'foliage', 'gravel', 'fabric', 'armour']
for t in todo: globals()[t]()
