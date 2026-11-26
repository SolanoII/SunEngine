# Campo di battaglia "Trincee di Solemnace" (Vs Bot 5 contro 5): genera Sandbox/GameData/Maps/vsbot_trincee.json.
#
#   python3 Tools/MapForge/battlefield.py [--out file.json] [--seed 7]
#
# Simmetrico (rotazione di 180 gradi): si disegna la meta' della squadra 0 (base a +z) e si ruota per la squadra 1.
#   - trincea di prima linea a denti (tratti di 11 m sfalsati di 4 m, traverse), muri di terra con assi (vestiti da
#     trench_berm), gradino di tiro, sacchi sul parapetto, varchi per uscire, passerelle sul fondo
#   - due camminamenti dalla base alla prima linea
#   - casamatta di cemento su un rialzo dietro la linea, con feritoia sopra il parapetto, porta e gradini dietro
#   - base: rovine di cemento e muretti di sacchi attorno ai punti di partenza
#   - terra di nessuno: due file di reticolati con i varchi, cavalli di frisia, crateri, rovina della fattoria al
#     centro, alberi morti, sacchi e croci
#   - bordi: muri invisibili coperti da terrapieni
# I blocchi sono le collisioni e la navmesh (i bot ci camminano); i prop ("type": "prop") sono solo da vedere.
import json, math, os, random, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
ARGV = sys.argv[1:]
OUT = ARGV[ARGV.index('--out') + 1] if '--out' in ARGV else os.path.join(ROOT, 'Sandbox/GameData/Maps/vsbot_trincee.json')
rng = random.Random(int(ARGV[ARGV.index('--seed') + 1]) if '--seed' in ARGV else 7)

EARTH = [0.34, 0.27, 0.19, 1]
SAND = [0.55, 0.5, 0.38, 1]
CONC = [0.36, 0.36, 0.38, 1]
RUBBLE = [0.42, 0.4, 0.37, 1]
TRUNK = [0.3, 0.2, 0.12, 1]
HALF_X, HALF_Z = 48.0, 68.0

S = []        # strutture della meta' 0 (si ruotano per la squadra 1)
SHARED = []   # strutture gia' simmetriche (al centro) o uniche (bordi)


def block(x, y, z, sx, sy, sz, color, material=None, out=None, **kw):
    b = {'type': 'block', 'position': [round(x, 3), round(y, 3), round(z, 3)], 'scale': [round(sx, 3), round(sy, 3), round(sz, 3)], 'color': color}
    if material: b['material'] = material
    b.update(kw)
    (S if out is None else out).append(b)


def prop(model, x, z, yaw=0.0, scale=1.0, y=0.0, out=None):
    p = {'type': 'prop', 'model': model, 'position': [round(x, 3), round(y, 3), round(z, 3)], 'yaw': round(yaw, 1)}
    p['scale'] = [round(v, 3) for v in scale] if isinstance(scale, (list, tuple)) else round(scale, 3)
    (S if out is None else out).append(p)


# ------------------------------------------------------------------ trincee
WALL_H, WALL_T, HALF_W = 1.5, 0.8, 1.2          # muro alto 1.5, spesso 0.8; fondo largo 2.4
OFF = HALF_W + WALL_T / 2


def offset_polyline(pts, d):
    """polilinea a gomiti retti spostata a sinistra (d > 0) della direzione di marcia (x, z)"""
    out = []
    for i, p in enumerate(pts):
        ns = []
        for a, b in ((pts[i - 1], p) if i > 0 else (None, None), (p, pts[i + 1]) if i < len(pts) - 1 else (None, None)):
            if a is None: continue
            dx, dz = b[0] - a[0], b[1] - a[1]; L = math.hypot(dx, dz)
            ns.append((-dz / L, dx / L))
        nx = sum(n[0] for n in ns); nz = sum(n[1] for n in ns)
        if len(ns) == 2 and abs(ns[0][0] * ns[1][0] + ns[0][1] * ns[1][1]) < 0.01:   # gomito retto: punta della squadra
            out.append((p[0] + nx * d, p[1] + nz * d))
        else:
            L = math.hypot(nx, nz) or 1.0
            out.append((p[0] + nx / L * d, p[1] + nz / L * d))
    return out


def walls_along(pts, height, gaps=(), extend=True):
    """un blocco per tratto (asse x o z), con varchi (x o z del centro del varco, larghezza 2.4)"""
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        horiz = abs(bz - az) < 1e-6
        lo, hi = (min(ax, bx), max(ax, bx)) if horiz else (min(az, bz), max(az, bz))
        if extend: lo -= WALL_T / 2; hi += WALL_T / 2
        cuts = sorted(g for g in gaps if lo < g < hi)
        spans, cur = [], lo
        for g in cuts: spans.append((cur, g - 1.2)); cur = g + 1.2
        spans.append((cur, hi))
        for s0, s1 in spans:
            if s1 - s0 < 0.3: continue
            c = (s0 + s1) / 2
            if horiz: block(c, 0, az, s1 - s0, height, WALL_T, EARTH, 'earthworks')
            else: block(ax, 0, c, WALL_T, height, s1 - s0, EARTH, 'earthworks')


def duckboards(pts):
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, bz - az); n = max(1, int(L // 2.4))
        for k in range(n):
            t = (k + 0.5) / n
            prop('duckboard', ax + (bx - ax) * t, az + (bz - az) * t, 0.0 if abs(bz - az) < 1e-6 else 90.0, [L / n / 2.4 * 0.98, 1.0, 1.0])


# prima linea della squadra 0: denti sfalsati fra z = 31 e z = 35
XS = [-45, -33, -22, -11, 0, 11, 22, 33, 45]
ZS = [31, 35, 31, 35, 31, 35, 31, 35]
line = []
for i in range(len(XS) - 1):
    line.append((XS[i], ZS[i])); line.append((XS[i + 1], ZS[i]))
line = [p for i, p in enumerate(line) if i == 0 or p != line[i - 1]]
front = offset_polyline(line, -OFF)     # destra della marcia verso +x = verso -z = verso il nemico
rear = offset_polyline(line, +OFF)
SORTIES = [-16.5, 16.5, -38.0, 38.0]    # varchi nel parapetto per uscire in terra di nessuno
COMMS = [-27.5, 27.5]                   # dove si innestano i camminamenti (varchi nel paradorso)
walls_along(front, WALL_H, SORTIES)
walls_along(rear, WALL_H, COMMS)
duckboards(line)


def glacis(pts, toward, height, depth, gaps):
    """terra ammucchiata fuori dal muro (solo da vedere): da fuori la trincea e' un argine di terra, non uno steccato.
    toward = -1 se il fuori e' verso -z. Solo i tratti lungo x; spezzata ai varchi."""
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        if abs(bz - az) > 1e-6: continue
        lo, hi = min(ax, bx) - WALL_T / 2, max(ax, bx) + WALL_T / 2
        spans, cur = [], lo
        for g in sorted(g for g in gaps if lo < g < hi): spans.append((cur, g - 1.5)); cur = g + 1.5
        spans.append((cur, hi))
        for s0, s1 in spans:
            if s1 - s0 < 1.0: continue
            sz = depth / 7.0
            zc = az + toward * (WALL_T / 2 - 0.1 + 3.5 * sz)
            prop('earth_bank', (s0 + s1) / 2, zc, 180.0 if toward < 0 else 0.0, [(s1 - s0) / 10.0, height / 3.8, sz], y=-0.05)


glacis(front, -1, 1.75, 3.0, SORTIES)   # parapetto: argine verso il nemico
glacis(rear, +1, 1.3, 2.0, COMMS)       # paradorso
# gradino di tiro lungo il parapetto e sacchi sopra (meta' dei tratti), lontano dai varchi
for (ax, az), (bx, bz) in zip(line, line[1:]):
    if abs(bz - az) > 1e-6: continue
    lo, hi = min(ax, bx) + 0.6, max(ax, bx) - 0.6
    ok_spans, cur = [], lo
    for g in sorted(g for g in SORTIES if lo < g < hi): ok_spans.append((cur, g - 1.6)); cur = g + 1.6
    ok_spans.append((cur, hi))
    for s0, s1 in ok_spans:
        if s1 - s0 < 1.0: continue
        c = (s0 + s1) / 2
        block(c, 0, az - HALF_W + 0.3, s1 - s0, 0.45, 0.6, EARTH, 'earthworks')                   # gradino di tiro
        if rng.random() < 0.6: block(c, WALL_H, az - OFF, s1 - s0, 0.55, 0.9, SAND, 'sandbags')   # sacchi sul parapetto

# camminamenti dalla base: dal paradorso fino a z = 52, con un gomito
for xc in COMMS:
    seg_z = next(ZS[i] for i in range(len(XS) - 1) if XS[i] < xc < XS[i + 1])
    z0 = seg_z + OFF
    side = 1 if xc > 0 else -1
    comm = [(xc, z0), (xc, 42.0), (xc + 4.5 * side, 42.0), (xc + 4.5 * side, 53.0)]
    walls_along(offset_polyline(comm, OFF), WALL_H)
    walls_along(offset_polyline(comm, -OFF), WALL_H)
    duckboards(comm)

# ------------------------------------------------------------------ casamatta su un rialzo, dietro la linea
bx, bz = -5.5, 41.0
block(bx, 0, bz, 8.0, 1.0, 7.0, EARTH, 'earthworks')                              # rialzo di terra
block(bx, 0, bz + 4.1, 2.4, 0.5, 1.2, EARTH, 'earthworks')                        # gradino
block(bx, 0, bz + 3.7, 2.4, 0.75, 0.6, EARTH, 'earthworks')
F = bz - 3.2                                                                      # muro davanti (verso il nemico)
block(bx, 1.0, F, 6.0, 1.1, 0.6, CONC, 'concrete')                                # sotto la feritoia
block(bx, 2.5, F, 6.0, 0.7, 0.6, CONC, 'concrete')                                # sopra la feritoia (feritoia 2.1-2.5)
block(bx - 2.7, 1.0, bz - 0.7, 0.6, 2.2, 5.0, CONC, 'concrete')
block(bx + 2.7, 1.0, bz - 0.7, 0.6, 2.2, 5.0, CONC, 'concrete')
block(bx - 1.85, 1.0, bz + 1.8, 2.3, 2.2, 0.6, CONC, 'concrete')                  # dietro, con la porta in mezzo
block(bx + 1.85, 1.0, bz + 1.8, 2.3, 2.2, 0.6, CONC, 'concrete')
block(bx, 3.2, bz - 0.7, 6.6, 0.5, 5.6, CONC, 'concrete')                         # tetto
block(bx, 3.7, F + 0.3, 6.6, 0.45, 0.9, SAND, 'sandbags')                         # sacchi sul tetto
block(bx - 3.6, 1.0, bz - 1.0, 0.9, 0.9, 4.0, SAND, 'sandbags')
block(bx + 3.6, 1.0, bz - 1.0, 0.9, 0.9, 4.0, SAND, 'sandbags')
# nido di mitragliatrice di sacchi dall'altra parte della linea
block(20.0, 0, 40.0, 4.0, 1.1, 0.9, SAND, 'sandbags'); block(18.4, 0, 41.8, 0.9, 1.1, 3.0, SAND, 'sandbags'); block(21.6, 0, 41.8, 0.9, 1.1, 3.0, SAND, 'sandbags')

# ------------------------------------------------------------------ base
for x in (-14.0, 14.0):                                    # rovine di cemento ai lati dei punti di partenza
    block(x, 0, 56.0, 7.0, 2.6, 0.7, CONC, 'concrete'); block(x + (3.2 if x > 0 else -3.2), 0, 58.5, 0.7, 2.0, 5.0, CONC, 'concrete')
    block(x - (2.0 if x > 0 else -2.0), 0, 53.8, 2.5, 1.0, 0.8, RUBBLE, 'rubble')
for x in (-6.5, 0.0, 6.5):                                 # muretti di sacchi davanti alla partenza
    block(x, 0, 54.5, 4.0, 1.0, 0.8, SAND, 'sandbags')

# ------------------------------------------------------------------ terra di nessuno (meta' della squadra 0: z da 0 a 27)
for z, shift in ((24.5, 0.0), (22.5, 1.5)):               # due file di reticolati, varchi davanti ai varchi del parapetto
    x = -44.0 + shift
    while x < 44.0:
        if all(abs(x - g) > 3.2 for g in SORTIES[:2]): prop('barbed_wire', x, z, rng.uniform(-6, 6), [1.0, rng.uniform(0.85, 1.1), 1.0])
        x += 3.0
for _ in range(7):                                         # cavalli di frisia
    prop('czech_hedgehog', rng.uniform(-40, 40), rng.uniform(13, 20), rng.uniform(0, 360), rng.uniform(0.9, 1.2))
for (x, z) in ((-31, 9), (13, 17), (36, 5), (-9, 21), (27, 12)):   # alberi morti (tronchi: il gioco ci mette l'albero)
    block(x, 0, z, 0.6, rng.uniform(3.5, 5.0), 0.6, TRUNK, 'wood')
for (x, z) in ((-22, 14), (6, 11), (31, 19), (-37, 18)):           # sacchi abbandonati: riparo nei crateri
    block(x, 0, z, 2.6, 0.9, 0.8, SAND, 'sandbags')
prop('grave_cross', -3.5, 9.0, 12); prop('grave_cross', 7.5, 6.5, -20); prop('grave_cross', -18.0, 18.0, 4)

# rovina della fattoria al centro (gia' simmetrica: si scrive una volta)
for (x, z, sx, sy, sz) in ((0, -4.5, 9.0, 2.4, 0.6), (4.2, -1.5, 0.6, 1.7, 6.0), (-4.2, 1.0, 0.6, 1.2, 4.0), (-2.0, 4.5, 4.5, 0.9, 0.6)):
    for sgn in (1, -1):
        block(x * sgn, 0, z * sgn, sx, sy, sz, RUBBLE, 'rubble', out=SHARED)
block(0, 0, 0, 2.2, 0.5, 2.2, RUBBLE, 'rubble', out=SHARED)

# crateri: lontano da trincee, rovine e alberi (meta' 0, poi ruotati)
taken = [(0, 0, 7.5)] + [(x, z, 2.5) for x, z in ((-31, 9), (13, 17), (36, 5), (-9, 21), (27, 12))] + [(x, z, 3.0) for x, z in ((-22, 14), (6, 11), (31, 19), (-37, 18))]
craters = 0
for _ in range(400):
    if craters >= 26: break
    x, z = rng.uniform(-43, 43), rng.uniform(-1.0, 27.0)
    s = rng.uniform(0.55, 1.4)
    if z + 3.6 * s > 28.5: continue
    if any(math.hypot(x - tx, z - tz) < tr + 3.2 * s for tx, tz, tr in taken): continue
    taken.append((x, z, 3.2 * s))
    prop('crater', x, z, rng.uniform(0, 360), [s, rng.uniform(0.7, 1.2) * s, s])
    craters += 1
# crateri anche dietro le linee (artiglieria sulle retrovie)
for (x, z, s) in ((-36, 46, 0.9), (34, 49, 0.8), (-22, 62, 0.7), (40, 60, 0.6), (9, 47, 0.6)):
    prop('crater', x, z, rng.uniform(0, 360), s)

# ------------------------------------------------------------------ bordi: muri invisibili coperti da terrapieni
for z in (-67.0, 67.0):
    block(0, 0, z, 96, 6, 2, CONC, 'concrete', out=SHARED, hidden=True)
for x in (-47.0, 47.0):
    block(x, 0, 0, 2, 6, 136, CONC, 'concrete', out=SHARED, hidden=True)
for x in range(-45, 46, 10):
    prop('earth_bank', x, 63.9, 180.0, [1.05, 0.8, 0.45], out=SHARED)
    prop('earth_bank', -x, -63.9, 0.0, [1.05, 0.8, 0.45], out=SHARED)
for z in range(-60, 61, 10):
    prop('earth_bank', 44.2, z, -90.0, [1.05, 0.75, 0.42], out=SHARED)
    prop('earth_bank', -44.2, -z, 90.0, [1.05, 0.75, 0.42], out=SHARED)


# ------------------------------------------------------------------ simmetria e scrittura
def rotated(e):
    e = json.loads(json.dumps(e))
    p = e['position']; p[0] = -p[0]; p[2] = -p[2]
    if e['type'] == 'prop': e['yaw'] = round((e.get('yaw', 0.0) + 180.0) % 360.0, 1)
    return e


structures = S + [rotated(e) for e in S] + SHARED
data = {
    'id': 'vsbot_trincee',
    'name': 'Trincee di Solemnace (5 contro 5)',
    'comment': "Generata da Tools/MapForge/battlefield.py (non modificare a mano: rilancia lo script). Simmetrica (rotazione di 180 gradi). "
               "player_spawn_points = squadra 0 (base a +z), enemy_spawn_points = squadra 1 (base a -z).",
    'terrain': {'size': [96.0, 1.0, 136.0], 'color': [0.16, 0.13, 0.1, 1.0]},
    'scatter_props': False,
    'player_spawn_points': [[-8, 0, 60], [-4, 0, 62], [0, 0, 60], [4, 0, 62], [8, 0, 60]],
    'enemy_spawn_points': [[8, 0, -60], [4, 0, -62], [0, 0, -60], [-4, 0, -62], [-8, 0, -60]],
    'structures': structures,
}
json.dump(data, open(OUT, 'w'), indent=1)
nb = sum(1 for e in structures if e['type'] == 'block'); npr = len(structures) - nb
print(f'scritta {OUT}: {nb} blocchi, {npr} prop')
