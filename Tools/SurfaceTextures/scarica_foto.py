# Dettaglio fotografico delle superfici da materiali scansionati CC0 (Poly Haven, ambientCG) al posto di quelli generati.
#
#   python3 Tools/SurfaceTextures/scarica_foto.py Sandbox/GameData/Textures/Surfaces [--res 1024] [nomi...]
#
# Per ogni strato si scaricano colore, normali (OpenGL), ruvidita', occlusione e altezza in 2k, si riducono e si
# scrivono nel formato che legge il motore (DeferredRenderer::LoadSurfaceDetail):
#   <nome>_albedo.jpg  colore RELATIVO (0.5 = invariato): foto / colore medio, cosi' la tinta resta quella del
#                      materiale della mappa e dalla foto arrivano sassi, crepe, ruggine, fibre, venature
#   <nome>_normal.jpg  normali tangenti (convenzione OpenGL, come la foto)
#   <nome>_data.jpg    R altezza, G ruvidita' relativa (0.5 = invariata), B occlusione
# Tutti i materiali sono CC0 (pubblico dominio): l'elenco con le pagine e' in CREDITI.md accanto alle texture.
import sys, os, io, json, zipfile, urllib.request
import numpy as np
from PIL import Image

OUT = sys.argv[1]
RES = int(sys.argv[sys.argv.index('--res') + 1]) if '--res' in sys.argv else 1024
ONLY = [a for a in sys.argv[2:] if not a.startswith('--') and not a.isdigit()]

# strato del motore -> (fonte, id, nota)
LAYERS = {
    'soil':     ('ph', 'brown_mud_02', 'fango bruno bagnato'),
    'concrete': ('ph', 'cracked_concrete_wall', 'muro di cemento crepato e sporco'),
    'metal':    ('ph', 'green_metal_rust', 'lamiera verniciata di verde e arrugginita'),
    'burlap':   ('ph', 'hessian_230', 'juta grossa dei sacchi'),
    'wood':     ('ph', 'rough_wood', 'legno grezzo'),
    'foliage':  ('ph', 'forest_leaves_02', 'foglie morte'),
    'gravel':   ('ph', 'rubble', 'macerie di cemento'),
    'fabric':   ('ph', 'denim_fabric_06', 'tela di cotone scura e consumata (divise)'),
    'armour':   ('ph', 'metal_plate_02', 'lamiera d\'acciaio consumata e arrugginita'),
}
UA = {'User-Agent': 'SunEngine-SurfaceTextures/1.0'}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r: return r.read()


def img(data, mode):
    return Image.open(io.BytesIO(data)).convert(mode)


def fetch_ph(aid):
    files = json.loads(get(f'https://api.polyhaven.com/files/{aid}'))
    pick = lambda key: files[key]['2k']['jpg']['url'] if key in files else None
    out = {}
    for k, key in (('color', 'Diffuse'), ('normal', 'nor_gl'), ('rough', 'Rough'), ('ao', 'AO'), ('height', 'Displacement')):
        u = pick(key)
        if u: out[k] = get(u)
    return out


def fetch_acg(aid):
    z = zipfile.ZipFile(io.BytesIO(get(f'https://ambientcg.com/get?file={aid}_2K-JPG.zip')))
    out = {}
    for n in z.namelist():
        low = n.lower()
        for k, tag in (('color', '_color.'), ('normal', '_normalgl.'), ('rough', '_roughness.'), ('ao', '_ambientocclusion.'), ('height', '_displacement.')):
            if tag in low: out[k] = z.read(n)
    return out


def tileable_resize(im, size):
    return im.resize((size, size), Image.LANCZOS)


def convert(name, src):
    c = np.asarray(tileable_resize(img(src['color'], 'RGB'), RES), np.float32) / 255.0
    mean = c.reshape(-1, 3).mean(0)
    rel = np.clip(0.5 * c / np.maximum(mean, 1e-3), 0, 1)
    nrm = np.asarray(tileable_resize(img(src['normal'], 'RGB'), RES), np.float32) / 255.0 if 'normal' in src else np.dstack([np.full((RES, RES), .5)] * 2 + [np.ones((RES, RES))])
    r = np.asarray(tileable_resize(img(src['rough'], 'L'), RES), np.float32) / 255.0 if 'rough' in src else np.full((RES, RES), 0.7, np.float32)
    rrel = np.clip(0.5 + (r - r.mean()) * 1.2, 0, 1)
    ao = np.asarray(tileable_resize(img(src['ao'], 'L'), RES), np.float32) / 255.0 if 'ao' in src else np.ones((RES, RES), np.float32)
    if 'height' in src:
        h = np.asarray(tileable_resize(img(src['height'], 'L'), RES), np.float32) / 255.0
        h = np.clip(0.5 + (h - h.mean()) / (h.std() * 4 + 1e-4), 0, 1)
    else:
        h = np.clip(0.5 + (c.mean(2) - c.mean()) * 2, 0, 1)
    save = lambda a, p: Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, p), quality=92)
    save(rel, f'{name}_albedo.jpg')
    save(nrm, f'{name}_normal.jpg')
    save(np.dstack([h, rrel, ao]), f'{name}_data.jpg')
    print(f'{name}: colore medio {np.round(mean, 3).tolist()}, ruvidita\' media {r.mean():.2f}')


os.makedirs(OUT, exist_ok=True)
credits = ['# Crediti delle texture di dettaglio', '', 'Tutte CC0 (pubblico dominio), scaricate e convertite da `Tools/SurfaceTextures/scarica_foto.py`.', '']
for name, (where, aid, note) in LAYERS.items():
    page = f'https://polyhaven.com/a/{aid}' if where == 'ph' else f'https://ambientcg.com/view?id={aid}'
    credits.append(f'- `{name}`: {aid} ({note}) - {"Poly Haven" if where == "ph" else "ambientCG"}, {page}')
    if ONLY and name not in ONLY: continue
    src = fetch_ph(aid) if where == 'ph' else fetch_acg(aid)
    if 'color' not in src: print(name, ': colore mancante, salto'); continue
    convert(name, src)
open(os.path.join(OUT, 'CREDITI.md'), 'w', encoding='utf-8').write('\n'.join(credits) + '\n')
