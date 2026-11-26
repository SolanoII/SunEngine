# Texture fotografiche CC0 (Poly Haven) per le forge di Blender (armi, Vanguard, ...).
#
# Si scaricano una volta in Tools/ModelForge/_photo/ (fuori dal repo, vedi .gitignore) in 2k jpg: colore, ruvidita',
# altezza, piu' le misure reali del pezzo fotografato. Nelle cotture la foto non decide la tinta: se ne usa il colore
# RELATIVO (foto / colore medio), cosi' la tinta resta quella del materiale e dalla foto arrivano graffi, ruggine,
# venature, grana del cuoio, colature. Senza rete la forgia continua con i soli materiali procedurali.
import os, json, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, '_photo')
UA = {'User-Agent': 'SunEngine-ModelForge/1.0'}
_dims = None
_means = {}


def _get(u):
    return urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=120).read()


def path(pid, k):
    return os.path.join(DIR, f'{pid}_{k}.jpg')


def ensure(ids):
    """scarica le foto che mancano; restituisce {id: [larghezza_mm, altezza_mm]}"""
    global _dims
    os.makedirs(DIR, exist_ok=True)
    dp = os.path.join(DIR, 'dimensions.json')
    dims = json.load(open(dp)) if os.path.exists(dp) else {}
    for i in ids:
        if i in dims and all(os.path.exists(path(i, k)) for k in ('diff', 'rough')): continue
        try:
            f = json.loads(_get(f'https://api.polyhaven.com/files/{i}'))
            dims[i] = json.loads(_get(f'https://api.polyhaven.com/info/{i}')).get('dimensions') or [1000, 1000]
            for k, key in (('diff', 'Diffuse'), ('rough', 'Rough'), ('disp', 'Displacement')):
                if key in f: open(path(i, k), 'wb').write(_get(f[key]['2k']['jpg']['url']))
            print('foto scaricata', i, flush=True)
        except Exception as e:
            print('foto non scaricata', i, e, flush=True)
    json.dump(dims, open(dp, 'w'))
    _dims = dims
    return dims


def available(pid):
    return os.path.exists(path(pid, 'diff'))


def mean(pid, k='diff'):
    """colore medio lineare della foto (per il colore relativo)"""
    key = (pid, k)
    if key not in _means:
        import numpy as np
        from PIL import Image
        a = np.asarray(Image.open(path(pid, k)).convert('RGB').resize((256, 256)), np.float32) / 255.0
        if k == 'diff': a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
        _means[key] = a.reshape(-1, 3).mean(0).tolist()
    return _means[key]


def nodes(nt, vec, pid, mult=1.0, blend=0.3, gray=True):
    """nodi della foto proiettata a cubo su `vec` (coordinate in metri) con la sua misura reale * 1/mult.
    -> dict con 'rel' (colore relativo, 1 = invariato; con gray solo la luminanza), 'rough' (scarto dalla ruvidita' media, -0.5..0.5), 'disp'
    (altezza 0..1) oppure None se la foto non c'e'"""
    if not available(pid): return None
    L = nt.links.new
    size_m = max(0.05, ((_dims or ensure([pid])).get(pid) or [1000])[0] / 1000.0) / mult
    mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0 / size_m,) * 3
    L(vec, mp.inputs['Vector'])

    def tex(k, noncolor):
        if not os.path.exists(path(pid, k)): return None
        im = bpy_images().load(path(pid, k), check_existing=True)
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = im; t.projection = 'BOX'; t.projection_blend = blend
        L(mp.outputs['Vector'], t.inputs['Vector'])
        return t.outputs['Color']

    out = {}
    m = mean(pid)
    if gray:   # solo la luminanza: la foto non porta la sua tinta (ruggine arancio, vernice verde...) sul materiale
        bw = nt.nodes.new('ShaderNodeRGBToBW'); L(tex('diff', False), bw.inputs[0])
        rel = nt.nodes.new('ShaderNodeMath'); rel.operation = 'MULTIPLY'
        L(bw.outputs[0], rel.inputs[0]); rel.inputs[1].default_value = 1.0 / max(0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2], 1e-3)
        out['rel'] = rel.outputs[0]
    else:
        rel = nt.nodes.new('ShaderNodeVectorMath'); rel.operation = 'MULTIPLY'
        L(tex('diff', False), rel.inputs[0]); rel.inputs[1].default_value = tuple(1.0 / max(x, 1e-3) for x in m)
        out['rel'] = rel.outputs['Vector']
    r = tex('rough', True)
    if r is not None:
        mr = mean(pid, 'rough')[0]
        sub = nt.nodes.new('ShaderNodeMath'); sub.operation = 'SUBTRACT'
        sep = nt.nodes.new('ShaderNodeSeparateColor'); L(r, sep.inputs[0])
        L(sep.outputs[0], sub.inputs[0]); sub.inputs[1].default_value = mr
        out['rough'] = sub.outputs[0]
    h = tex('disp', True)
    if h is not None:
        sep = nt.nodes.new('ShaderNodeSeparateColor'); L(h, sep.inputs[0]); out['disp'] = sep.outputs[0]
    return out


def bpy_images():
    import bpy
    return bpy.data.images
