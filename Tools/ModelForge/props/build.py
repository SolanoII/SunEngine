# Prop delle mappe di TSDH (vedi ../ART_DIRECTION.md): pezzi di trincea fatti in Blender, cotti e esportati in .glb.
#
#   python3 Tools/ModelForge/props/build.py                 -> tutti in Sandbox/GameData/Models/Props/<id>.glb
#   python3 Tools/ModelForge/props/build.py --only sandbag_wall --fast
#
# Ogni prop: metri, origine al centro della base, lato lungo sull'asse X (nel gioco: X), profondita' sull'asse Y di
# Blender (nel gioco: Z), alto Z (nel gioco: Y). Il gioco li usa per "vestire" i blocchi delle mappe (sacchi,
# terrapieni, alberi) e per spargere filo spinato, cavalli di frisia e croci (GameLayerProps.cpp).
import sys, os, math, random
import numpy as np
import bpy, bmesh

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
ARGV = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
def arg(n, d=None): return ARGV[ARGV.index(n) + 1] if n in ARGV else d
OUTDIR = os.path.join(ROOT, 'Sandbox/GameData/Models/Props')
FAST = '--fast' in ARGV
ONLY = arg('--only')
SC = None


# ------------------------------------------------------------------ geometria
def reset():
    global SC
    bpy.ops.wm.read_factory_settings(use_empty=True)
    SC = bpy.context.scene


def link(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(map(float, v)) for v in verts], [], [tuple(int(i) for i in f) for f in faces]); me.update()
    ob = bpy.data.objects.new(name, me); SC.collection.objects.link(ob)
    return ob


def activate(ob):
    for o in bpy.context.view_layer.objects: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


def apply_mods(ob):
    activate(ob)
    for m in list(ob.modifiers): bpy.ops.object.modifier_apply(modifier=m.name)


def smooth(ob, angle=40):
    for p in ob.data.polygons: p.use_smooth = True
    try:
        activate(ob); bpy.ops.object.shade_smooth_by_angle(angle=math.radians(angle))
    except Exception:
        pass


def grid_surface(name, fn, nu, nv, closed_u=False):
    """superficie da una funzione (i, j) -> punto; quad fra righe"""
    V = [fn(i, j) for j in range(nv + 1) for i in range(nu + (0 if closed_u else 1))]
    W = nu if closed_u else nu + 1
    F = []
    for j in range(nv):
        for i in range(nu if closed_u else nu):
            a = j * W + i; b = j * W + (i + 1) % W if closed_u else j * W + i + 1
            F.append((a, b, b + W, a + W))
    return link(name, V, F)


def box(name, c, s, rot=(0, 0, 0), bevel=0.0, seg=2):
    bpy.ops.mesh.primitive_cube_add(size=1, location=c, rotation=rot)
    ob = bpy.context.active_object; ob.name = name; ob.scale = s
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel > 0:
        m = ob.modifiers.new('B', 'BEVEL'); m.width = bevel; m.segments = seg; m.limit_method = 'ANGLE'
        apply_mods(ob)
    return ob


def tube(name, pts, radii, sides=10, cap=True):
    """tubo lungo una polilinea con raggio variabile (rami, filo, pali)"""
    pts = [np.asarray(p, float) for p in pts]
    V, F = [], []
    n = len(pts)
    prev = None
    for k, p in enumerate(pts):
        t = pts[min(k + 1, n - 1)] - pts[max(k - 1, 0)]; t /= max(np.linalg.norm(t), 1e-9)
        if prev is None:
            a = np.cross(t, (0, 0, 1)) if abs(t[2]) < 0.9 else np.cross(t, (1, 0, 0))
        else:
            a = prev - t * np.dot(prev, t)
        a /= max(np.linalg.norm(a), 1e-9); b = np.cross(t, a); prev = a
        r = radii[k] if hasattr(radii, '__len__') else radii
        for s in range(sides):
            ang = 2 * math.pi * s / sides
            V.append(p + r * (math.cos(ang) * a + math.sin(ang) * b))
    for k in range(n - 1):
        for s in range(sides):
            i0 = k * sides + s; i1 = k * sides + (s + 1) % sides
            F.append((i0, i1, i1 + sides, i0 + sides))
    if cap:
        F.append(tuple(reversed(range(sides)))); F.append(tuple(range((n - 1) * sides, n * sides)))
    return link(name, V, F)


def noise3(p, seed=0):
    """rumore liscio economico (somma di seni) per deformare le mesh"""
    x, y, z = p
    return (math.sin(x * 3.1 + seed) * math.sin(y * 2.7 + seed * 1.7) * math.sin(z * 3.3 + seed * 0.3)
            + 0.5 * math.sin(x * 7.3 - seed) * math.sin(y * 6.1 + seed) * math.sin(z * 5.9 + 2 * seed))


def displace(ob, amp, freq, seed=0, mask=None):
    for v in ob.data.vertices:
        p = np.array(v.co[:])
        if mask and not mask(p): continue
        n = np.array(v.normal[:])
        v.co = p + n * amp * noise3(p * freq, seed)


def join(obs, name):
    obs = [o for o in obs if o is not None]
    for o in bpy.context.view_layer.objects: o.select_set(False)
    for o in obs: o.select_set(True)
    bpy.context.view_layer.objects.active = obs[0]
    if len(obs) > 1: bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active; ob.name = name
    return ob


# ------------------------------------------------------------------ materiali procedurali -> texture
def node(nt, t, **kw):
    n = nt.nodes.new(t)
    for k, v in kw.items():
        if k in n.inputs: n.inputs[k].default_value = v
        else: setattr(n, k, v)
    return n


def _sock(n, ident):
    for x in n.inputs:
        if x.identifier == ident: return x
    raise KeyError(ident)


def _set(nt, sock, val):
    if hasattr(val, 'node') and hasattr(val, 'is_output'): nt.links.new(val, sock)
    elif isinstance(val, (tuple, list)): sock.default_value = (*val[:3], 1) if len(val) == 3 else tuple(val)
    else: sock.default_value = val


def mixc(nt, fac, a, b, blend='MIX'):
    n = nt.nodes.new('ShaderNodeMix'); n.data_type = 'RGBA'; n.blend_type = blend
    _set(nt, _sock(n, 'Factor_Float'), fac); _set(nt, _sock(n, 'A_Color'), a); _set(nt, _sock(n, 'B_Color'), b)
    return next(o for o in n.outputs if o.identifier == 'Result_Color')


def mixf(nt, fac, a, b):
    n = nt.nodes.new('ShaderNodeMix'); n.data_type = 'FLOAT'
    _set(nt, _sock(n, 'Factor_Float'), fac); _set(nt, _sock(n, 'A_Float'), a); _set(nt, _sock(n, 'B_Float'), b)
    return next(o for o in n.outputs if o.identifier == 'Result_Float')


def mrange(nt, val, fmin, fmax, tmin=0.0, tmax=1.0):
    n = nt.nodes.new('ShaderNodeMapRange'); n.clamp = True
    n.inputs['From Min'].default_value = fmin; n.inputs['From Max'].default_value = fmax
    n.inputs['To Min'].default_value = tmin; n.inputs['To Max'].default_value = tmax
    _set(nt, n.inputs['Value'], val)
    return n.outputs['Result']


def mul(nt, a, b):
    n = nt.nodes.new('ShaderNodeMath'); n.operation = 'MULTIPLY'; _set(nt, n.inputs[0], a); _set(nt, n.inputs[1], b); return n.outputs[0]


def add(nt, a, b):
    n = nt.nodes.new('ShaderNodeMath'); n.operation = 'ADD'; _set(nt, n.inputs[0], a); _set(nt, n.inputs[1], b); return n.outputs[0]


# Materiali fotografici (Poly Haven, CC0) scaricati in _photo/ (vedi README): colore, ruvidita' e altezza vere,
# proiettate a cubo sulle coordinate oggetto con la loro misura reale; sopra restano sporco, fango e usura procedurali.
PHOTO_DIR = os.path.join(HERE, '_photo')
PHOTO = {'Burlap': ('hessian_230', 0.55), 'Wood': ('rough_wood', 0.55), 'Bark': ('bark_brown_02', 0.45),
         'Iron': ('rusty_metal_04', 0.6), 'Earth': ('brown_mud_02', 0.5)}
PHOTO_IDS = ['hessian_230', 'rough_wood', 'bark_brown_02', 'rusty_metal_04', 'brown_mud_02']


def ensure_photos():
    """scarica le texture fotografiche che mancano (2k jpg) e le loro misure reali"""
    import json, urllib.request
    os.makedirs(PHOTO_DIR, exist_ok=True)
    dims_p = os.path.join(PHOTO_DIR, 'dimensions.json')
    dims = json.load(open(dims_p)) if os.path.exists(dims_p) else {}
    UA = {'User-Agent': 'SunEngine-ModelForge/1.0'}
    get = lambda u: urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=120).read()
    for i in PHOTO_IDS:
        if all(os.path.exists(os.path.join(PHOTO_DIR, f'{i}_{k}.jpg')) for k in ('diff', 'rough', 'disp')) and i in dims: continue
        try:
            f = json.loads(get(f'https://api.polyhaven.com/files/{i}'))
            dims[i] = json.loads(get(f'https://api.polyhaven.com/info/{i}')).get('dimensions')
            for k, key in (('diff', 'Diffuse'), ('rough', 'Rough'), ('disp', 'Displacement')):
                if key in f: open(os.path.join(PHOTO_DIR, f'{i}_{k}.jpg'), 'wb').write(get(f[key]['2k']['jpg']['url']))
        except Exception as e:
            print('foto non scaricata', i, e)
    json.dump(dims, open(dims_p, 'w'))
    return dims


PHOTO_DIMS = {}


def photo(nt, obj, kind):
    """(colore, ruvidita', altezza) dalla foto, o None se manca"""
    if kind not in PHOTO: return None
    pid, bright = PHOTO[kind]
    paths = {k: os.path.join(PHOTO_DIR, f'{pid}_{k}.jpg') for k in ('diff', 'rough', 'disp')}
    if not os.path.exists(paths['diff']): return None
    L = nt.links.new
    size_m = max(0.1, (PHOTO_DIMS.get(pid) or [1000])[0] / 1000.0)
    mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0 / size_m,) * 3; L(obj, mp.inputs['Vector'])
    def tex(k, noncolor):
        if not os.path.exists(paths[k]): return None
        im = bpy.data.images.load(paths[k], check_existing=True)
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        t = node(nt, 'ShaderNodeTexImage'); t.image = im; t.projection = 'BOX'; t.projection_blend = 0.3
        L(mp.outputs['Vector'], t.inputs['Vector'])
        return t.outputs['Color']
    c = mixc(nt, 1.0, tex('diff', False), (bright, bright, bright), 'MULTIPLY')
    r = tex('rough', True); h = tex('disp', True)
    return c, r, h


def procedural(nt, kind):
    """(colore, ruvidita', metallo, altezza, forza del rilievo). Coordinate oggetto in metri; Z = alto."""
    L = nt.links.new
    tc = node(nt, 'ShaderNodeTexCoord'); obj = tc.outputs['Object']
    geo = node(nt, 'ShaderNodeNewGeometry'); pt = geo.outputs['Pointiness']
    sep = node(nt, 'ShaderNodeSeparateXYZ'); L(obj, sep.inputs['Vector']); Z = sep.outputs['Z']
    nbig = node(nt, 'ShaderNodeTexNoise', Scale=1.5, Detail=4.0); L(obj, nbig.inputs['Vector'])
    nmid = node(nt, 'ShaderNodeTexNoise', Scale=9.0, Detail=5.0); L(obj, nmid.inputs['Vector'])
    nfine = node(nt, 'ShaderNodeTexNoise', Scale=70.0, Detail=6.0, Roughness=0.7); L(obj, nfine.inputs['Vector'])
    edge = mrange(nt, pt, 0.55, 0.65)
    cav = mrange(nt, pt, 0.5, 0.44)
    low = mrange(nt, Z, 0.45, 0.0)                 # vicino a terra: fango
    mud = (0.035, 0.028, 0.02)
    if kind == 'Burlap':
        weave = node(nt, 'ShaderNodeTexWave', wave_type='BANDS', bands_direction='X'); weave.inputs['Scale'].default_value = 260.0; L(obj, weave.inputs['Vector'])
        weave2 = node(nt, 'ShaderNodeTexWave', wave_type='BANDS', bands_direction='Y'); weave2.inputs['Scale'].default_value = 260.0; L(obj, weave2.inputs['Vector'])
        w = mul(nt, add(nt, weave.outputs['Fac'], weave2.outputs['Fac']), 0.5)
        ph = photo(nt, obj, kind)
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7), (0.085, 0.068, 0.045), (0.14, 0.11, 0.072))
        c = mixc(nt, mul(nt, mrange(nt, nfine.outputs['Fac'], 0.4, 0.7), 0.3), c, (0.06, 0.045, 0.03))
        if ph: c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7, 0.85, 1.0), ph[0], c, 'MULTIPLY'); w = ph[2]
        c = mixc(nt, mul(nt, mrange(nt, nmid.outputs['Fac'], 0.45, 0.7), 0.7), c, (0.07, 0.055, 0.04))      # macchie bagnate
        c = mixc(nt, mul(nt, add(nt, low, mul(nt, cav, 0.6)), 0.85), c, mud)
        r = mixf(nt, mul(nt, low, 0.8), 0.92, 0.55)                                                       # fango bagnato lucido
        if ph: return c, mixf(nt, mul(nt, low, 0.8), ph[1], 0.55), 0.0, add(nt, mul(nt, ph[2], 0.7), mul(nt, nmid.outputs['Fac'], 0.3)), 0.35
        return c, r, 0.0, add(nt, mul(nt, w, 0.15), mul(nt, nmid.outputs['Fac'], 0.6)), 0.25
    if kind == 'Wood':
        mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 1.0, 1.0); L(obj, mp.inputs['Vector'])
        mp.inputs['Scale'].default_value = (0.6, 14.0, 14.0)
        gn = node(nt, 'ShaderNodeTexNoise', Scale=2.0, Detail=8.0, Roughness=0.65); L(mp.outputs['Vector'], gn.inputs['Vector'])
        g = gn.outputs['Fac']
        c = mixc(nt, mrange(nt, g, 0.3, 0.7), (0.045, 0.032, 0.022), (0.11, 0.08, 0.052))
        ph = photo(nt, obj, kind)
        if ph: c = ph[0]; g = ph[2]
        c = mixc(nt, mul(nt, mrange(nt, nbig.outputs['Fac'], 0.55, 0.75), 0.8), c, (0.03, 0.028, 0.026))   # marcio e grigio
        c = mixc(nt, mul(nt, edge, 0.5), c, (0.24, 0.2, 0.15))
        c = mixc(nt, mul(nt, add(nt, low, mul(nt, cav, 0.7)), 0.8), c, mud)
        return c, mrange(nt, nfine.outputs['Fac'], 0, 1, 0.7, 0.95), 0.0, add(nt, g, mul(nt, nfine.outputs['Fac'], 0.5)), 0.35
    if kind == 'Bark':
        mp = node(nt, 'ShaderNodeMapping'); mp.inputs['Scale'].default_value = (9.0, 9.0, 1.2); L(obj, mp.inputs['Vector'])
        bark = node(nt, 'ShaderNodeTexVoronoi', feature='DISTANCE_TO_EDGE'); bark.inputs['Scale'].default_value = 3.0; L(mp.outputs['Vector'], bark.inputs['Vector'])
        c = mixc(nt, mrange(nt, bark.outputs['Distance'], 0.0, 0.12), (0.012, 0.011, 0.01), (0.075, 0.062, 0.05))
        ph = photo(nt, obj, kind)
        bh = bark.outputs['Distance']
        if ph: c = ph[0]; bh = ph[2]
        c = mixc(nt, mul(nt, mrange(nt, nbig.outputs['Fac'], 0.5, 0.7), 0.9), c, (0.006, 0.006, 0.006))   # carbonizzato
        c = mixc(nt, mul(nt, low, 0.7), c, mud)
        return c, 0.9, 0.0, add(nt, bh, mul(nt, nfine.outputs['Fac'], 0.2)), 0.6
    if kind == 'Iron':
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7), (0.035, 0.032, 0.03), (0.06, 0.05, 0.042))
        ph = photo(nt, obj, kind)
        if ph:   # ferro fotografico: la ruggine e' gia' nella foto
            c = mixc(nt, mul(nt, edge, 0.5), ph[0], (0.25, 0.24, 0.23))
            c = mixc(nt, mul(nt, low, 0.7), c, mud)
            return c, ph[1], 0.35, ph[2], 0.3
        rust = mul(nt, add(nt, mrange(nt, nmid.outputs['Fac'], 0.42, 0.62), mul(nt, cav, 0.8)), mrange(nt, nfine.outputs['Fac'], 0.3, 0.6))
        c = mixc(nt, mul(nt, rust, 0.9), c, (0.17, 0.065, 0.025))
        c = mixc(nt, mul(nt, edge, 0.6), c, (0.25, 0.24, 0.23))
        c = mixc(nt, mul(nt, low, 0.7), c, mud)
        r = mixf(nt, rust, 0.55, 0.92); m = mixf(nt, rust, 0.85, 0.1)
        return c, r, m, add(nt, mul(nt, rust, 0.6), mul(nt, nfine.outputs['Fac'], 0.3)), 0.25
    if kind == 'Earth':
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7), (0.05, 0.04, 0.03), (0.1, 0.08, 0.058))
        ph = photo(nt, obj, kind)
        if ph:
            c = mixc(nt, mul(nt, mrange(nt, nmid.outputs['Fac'], 0.5, 0.65), 0.6), ph[0], (0.025, 0.02, 0.016))
            return c, ph[1], 0.0, ph[2], 0.6
        stones = node(nt, 'ShaderNodeTexVoronoi'); stones.inputs['Scale'].default_value = 18.0; L(obj, stones.inputs['Vector'])
        st = mrange(nt, stones.outputs['Distance'], 0.12, 0.05)
        c = mixc(nt, mul(nt, st, 0.6), c, (0.15, 0.14, 0.12))
        c = mixc(nt, mul(nt, mrange(nt, nmid.outputs['Fac'], 0.5, 0.65), 0.7), c, (0.025, 0.02, 0.016))     # pozzanghere scure
        r = mixf(nt, mrange(nt, nmid.outputs['Fac'], 0.5, 0.65), 0.95, 0.35)
        return c, r, 0.0, add(nt, mul(nt, nmid.outputs['Fac'], 0.6), mul(nt, st, 0.4)), 0.5
    if kind == 'Helmet':
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7), (0.05, 0.055, 0.04), (0.08, 0.085, 0.06))   # verde oliva spento
        c = mixc(nt, mul(nt, edge, 0.8), c, (0.2, 0.19, 0.18))
        rust = mul(nt, mrange(nt, nmid.outputs['Fac'], 0.5, 0.7), 0.9)
        c = mixc(nt, rust, c, (0.15, 0.06, 0.025))
        return c, mixf(nt, edge, 0.75, 0.4), mixf(nt, edge, 0.2, 0.9), nfine.outputs['Fac'], 0.15
    if kind == 'Bone':
        crack = node(nt, 'ShaderNodeTexVoronoi', feature='DISTANCE_TO_EDGE'); crack.inputs['Scale'].default_value = 30.0; L(obj, crack.inputs['Vector'])
        c = mixc(nt, mrange(nt, crack.outputs['Distance'], 0.0, 0.04), (0.1, 0.08, 0.055), (0.42, 0.37, 0.28))
        c = mixc(nt, mul(nt, add(nt, cav, low), 0.6), c, mud)
        return c, 0.65, 0.0, crack.outputs['Distance'], 0.15
    if kind == 'Cloth':
        c = mixc(nt, mrange(nt, nbig.outputs['Fac'], 0.3, 0.7), (0.06, 0.012, 0.01), (0.12, 0.03, 0.02))    # stoffa rossa sporca
        c = mixc(nt, mul(nt, mrange(nt, nmid.outputs['Fac'], 0.45, 0.7), 0.8), c, (0.03, 0.02, 0.015))
        return c, 0.95, 0.0, nfine.outputs['Fac'], 0.1
    raise ValueError(kind)


def bake_material(ob, kind, size, prefix):
    name = f'{prefix}_{kind}'
    activate(ob)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(55), island_margin=0.004)
    bpy.ops.object.mode_set(mode='OBJECT')
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; L = nt.links.new
    out = nt.nodes['Material Output']; bs = nt.nodes['Principled BSDF']
    ob.data.materials.clear(); ob.data.materials.append(m)
    col, rough, metal, height, strength = procedural(nt, kind)
    if FAST:
        _set(nt, bs.inputs['Base Color'], col); _set(nt, bs.inputs['Roughness'], rough); _set(nt, bs.inputs['Metallic'], metal)
        return
    emis = node(nt, 'ShaderNodeEmission')
    imgs = {}

    def target(tag, noncolor):
        im = bpy.data.images.new(f'{name}_{tag}', size, size, alpha=False)
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
        for n in nt.nodes: n.select = False
        tn.select = True; nt.nodes.active = tn
        imgs[tag] = im

    SC.render.engine = 'CYCLES'; SC.cycles.samples = 1; SC.cycles.device = 'CPU'; SC.render.bake.margin = 4
    target('BaseColor', False); _set(nt, emis.inputs['Color'], col); L(emis.outputs[0], out.inputs['Surface'])
    bpy.ops.object.bake(type='EMIT')
    comb = node(nt, 'ShaderNodeCombineColor'); comb.inputs['Red'].default_value = 1.0
    for ch, src in (('Green', rough), ('Blue', metal)): _set(nt, comb.inputs[ch], float(src) if isinstance(src, (int, float)) else src)
    L(comb.outputs[0], emis.inputs['Color'])
    target('ORM', True); bpy.ops.object.bake(type='EMIT')
    bump = node(nt, 'ShaderNodeBump', Strength=strength, Distance=0.004); _set(nt, bump.inputs['Height'], height)
    L(bump.outputs['Normal'], bs.inputs['Normal']); L(bs.outputs[0], out.inputs['Surface'])
    target('Normal', True); SC.render.bake.normal_space = 'TANGENT'; bpy.ops.object.bake(type='NORMAL')
    for n in list(nt.nodes):
        if n not in (out, bs): nt.nodes.remove(n)

    def tex(tag, noncolor):
        im = imgs[tag]; im.pack()
        tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
        if noncolor: im.colorspace_settings.name = 'Non-Color'
        return tn
    L(tex('BaseColor', False).outputs['Color'], bs.inputs['Base Color'])
    sp = nt.nodes.new('ShaderNodeSeparateColor'); L(tex('ORM', True).outputs['Color'], sp.inputs['Color'])
    L(sp.outputs['Green'], bs.inputs['Roughness']); L(sp.outputs['Blue'], bs.inputs['Metallic'])
    nm = nt.nodes.new('ShaderNodeNormalMap'); L(tex('Normal', True).outputs['Color'], nm.inputs['Color']); L(nm.outputs['Normal'], bs.inputs['Normal'])
    L(bs.outputs[0], out.inputs['Surface'])


def finish(pid, parts, sizes=None):
    """parts: {materiale: [oggetti]} -> un oggetto per materiale, cottura, .glb"""
    sizes = sizes or {}
    objs = []
    for kind, obs in parts.items():
        # trasformazioni applicate: il gioco legge i vertici senza la posizione dei nodi del .glb
        for o in bpy.context.view_layer.objects: o.select_set(False)
        for o in obs: o.select_set(True)
        bpy.context.view_layer.objects.active = obs[0]
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        ob = join(obs, f'{pid}_{kind}')
        smooth(ob)
        bake_material(ob, kind, sizes.get(kind, 1024), pid)
        objs.append(ob)
    for o in bpy.context.view_layer.objects: o.select_set(False)
    for o in objs: o.select_set(True)
    os.makedirs(OUTDIR, exist_ok=True)
    out = os.path.join(OUTDIR, pid + '.glb')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', use_selection=True, export_yup=True, export_apply=True,
                              export_image_format='JPEG', export_jpeg_quality=90)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, pid + '.blend'))
    print('esportato', out, sum(len(o.data.vertices) for o in objs), 'vertici')


# ------------------------------------------------------------------ i prop
def sandbag(name, c, size, yaw, rng, flat_top=0.86):
    """sacco di juta pieno: cuscino con le cuciture ai bordi, schiacciato dal peso sopra"""
    a, b, h = size[0] / 2, size[1] / 2, size[2] / 2
    nu, nv = 14, 8
    V = []
    def f(u, v):
        return max(0.0, (1 - abs(u) ** 4)) ** 0.5 * max(0.0, (1 - abs(v) ** 4)) ** 0.5
    for side in (1, -1):
        for j in range(nv + 1):
            for i in range(nu + 1):
                u = -1 + 2 * i / nu; v = -1 + 2 * j / nv
                y = side * h * f(u, v)
                if side > 0: y = min(y, h * flat_top) + 0.004 * math.sin(u * 9 + rng.random() * 6) * f(u, v)
                else: y = max(y, -h * 0.75)
                V.append((a * u * (1 - 0.05 * abs(v)), b * v * (1 + 0.03 * math.cos(u * 3)), y))
    W = nu + 1; F = []
    for side in (0, 1):
        o = side * W * (nv + 1)
        for j in range(nv):
            for i in range(nu):
                q = (o + j * W + i, o + j * W + i + 1, o + (j + 1) * W + i + 1, o + (j + 1) * W + i)
                F.append(q if side == 0 else tuple(reversed(q)))
    ob = link(name, V, F)
    activate(ob); bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=0.002); bpy.ops.object.mode_set(mode='OBJECT')
    ob.rotation_euler = (rng.uniform(-0.04, 0.04), rng.uniform(-0.05, 0.05), yaw + rng.uniform(-0.06, 0.06))
    ob.location = c
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=False)
    return ob


def build_sandbag_wall():
    """2 m x 0.8 m x 1.0 m: file sfalsate di sacchi (testa e fascia), la cima un po' irregolare"""
    rng = random.Random(4)
    bags = []
    L_, D, rows = 2.0, 0.8, 7
    bh = 1.0 / rows
    for r in range(rows):
        z = bh * (r + 0.5) - 0.01
        header = r % 2 == 1
        if header:   # sacchi di testa: lati corti verso fuori
            n = 6
            for k in range(n):
                x = -L_ / 2 + (k + 0.5) * L_ / n + rng.uniform(-0.02, 0.02)
                bags.append(sandbag(f'B{r}_{k}', (x, rng.uniform(-0.02, 0.02), z), (0.74, 0.36, bh * 1.45), math.pi / 2, rng))
        else:
            n = 3
            off = 0.0 if r % 4 == 0 else L_ / n / 2
            for side in (-1, 1):
                for k in range(n + (1 if off else 0)):
                    x = -L_ / 2 + (k + 0.5) * L_ / n - off + rng.uniform(-0.02, 0.02)
                    if x < -L_ / 2 - 0.05 or x > L_ / 2 + 0.05: continue
                    bags.append(sandbag(f'B{r}_{side}_{k}', (x, side * D / 4, z), (0.7, 0.4, bh * 1.45), 0.0, rng))
    finish('sandbag_wall', {'Burlap': bags}, {'Burlap': 1024})


def build_trench_berm():
    """terrapieno di 3 m x 1 m x 1.3 m: anima di terra battuta, assi orizzontali e pali sui due lati, filo legato"""
    rng = random.Random(7)
    L_, D, H = 3.0, 1.0, 1.3
    # anima di terra: griglia chiusa sopra, piu' alta al centro, con zolle
    nu, nv = 40, 12
    def earth(i, j):
        u = -1 + 2 * i / nu; v = -1 + 2 * j / nv
        x = u * L_ / 2; y = v * (D / 2 - 0.05)
        z = H + 0.06 * (1 - v * v) + 0.05 * noise3((x * 2.0, y * 3, 0.5), 3) + 0.02 * noise3((x * 9, y * 9, 1), 5)
        return (x, y, z)
    top = grid_surface('EarthTop', earth, nu, nv)
    # fianchi di terra dietro le assi (si vedono fra un'asse e l'altra)
    sides = []
    for s in (-1, 1):
        sides.append(box(f'EarthSide{s}', (0, s * (D / 2 - 0.16), H / 2), (L_, 0.16, H)))
    planks, posts = [], []
    for s in (-1, 1):
        y = s * (D / 2 - 0.01)
        z = 0.0
        k = 0
        while z < H - 0.05:
            ph = rng.uniform(0.16, 0.22)
            for seg in range(2):   # assi da 1.5 m, giunte sfalsate
                x0 = -L_ / 2 + seg * L_ / 2 + (0.3 if k % 2 else 0.0) - (L_ / 2 if seg == 1 and k % 2 else 0)
                ln = L_ / 2
                xc = -L_ / 2 + L_ / 4 + seg * L_ / 2
                b = box(f'P{s}_{k}_{seg}', (xc, y, z + ph / 2), (ln - rng.uniform(0.0, 0.05), 0.035, ph - 0.012),
                        rot=(rng.uniform(-0.03, 0.03), 0, rng.uniform(-0.01, 0.01)), bevel=0.006)
                displace(b, 0.006, 6.0, seed=k * 3 + seg)
                planks.append(b)
            z += ph; k += 1
        for xp in (-L_ / 2 + 0.08, 0.0, L_ / 2 - 0.08):
            posts.append(box(f'Post{s}{xp}', (xp, y + s * 0.06, (H + 0.25) / 2 - 0.05), (0.11, 0.1, H + 0.25), rot=(s * rng.uniform(0.02, 0.06), rng.uniform(-0.03, 0.03), 0), bevel=0.01))
    wire = []
    for xp in (-L_ / 2 + 0.08, 0.0, L_ / 2 - 0.08):   # filo che lega i pali sopra il terrapieno
        wire.append(tube(f'Tie{xp}', [(xp, -D / 2 - 0.07, H + 0.12), (xp, -0.2, H + 0.1), (xp, 0.2, H + 0.1), (xp, D / 2 + 0.07, H + 0.12)], 0.006, sides=5))
    finish('trench_berm', {'Earth': [top] + sides, 'Wood': planks + posts, 'Iron': wire}, {'Earth': 1024, 'Wood': 1024, 'Iron': 256})


def build_dead_tree():
    """albero morto spezzato dalle granate: tronco curvo, rami rotti, cima a schegge; 7 m"""
    rng = random.Random(11)
    def bent(p0, d, length, n, r0, r1, wobble):
        pts, rad = [], []
        p = np.array(p0, float); d = np.array(d, float); d /= np.linalg.norm(d)
        for k in range(n + 1):
            t = k / n
            pts.append(p.copy()); rad.append(r0 * (1 - t) + r1 * t)
            d = d + np.array([rng.uniform(-wobble, wobble), rng.uniform(-wobble, wobble), rng.uniform(-wobble * 0.3, wobble * 0.3)])
            d /= np.linalg.norm(d); p = p + d * length / n
        return pts, rad
    obs = []
    pts, rad = bent((0, 0, -0.3), (0.05, 0.02, 1), 7.5, 20, 0.42, 0.14, 0.07)
    rad[0] *= 1.5; rad[1] *= 1.25
    trunk = tube('Trunk', pts, rad, sides=14)
    # cima spezzata: i vertici dell'ultimo anello sfrangiati in schegge
    tv = len(trunk.data.vertices)
    for i in range(tv - 14, tv):
        v = trunk.data.vertices[i]; v.co.z += rng.uniform(-0.1, 0.45); v.co.x *= 0.8; v.co.y *= 0.8
    displace(trunk, 0.035, 2.2, seed=2)
    obs.append(trunk)
    for k in range(8):   # rami rotti
        t = rng.uniform(0.35, 0.9)
        i = int(t * (len(pts) - 1)); base = pts[i]
        ang = rng.uniform(0, 2 * math.pi)
        d = (math.cos(ang), math.sin(ang), rng.uniform(0.2, 0.9))
        bp, br = bent(base, d, rng.uniform(1.4, 3.2) * (1.25 - t), 7, rad[i] * 0.5, 0.035, 0.22)
        obs.append(tube(f'Br{k}', bp, br, sides=7))
        if rng.random() < 0.6:   # rametto
            sp, sr = bent(bp[3], (d[0] + rng.uniform(-1, 1), d[1] + rng.uniform(-1, 1), 0.6), 0.6, 4, br[3] * 0.6, 0.01, 0.3)
            obs.append(tube(f'Tw{k}', sp, sr, sides=5))
    # radici che escono dal fango
    for k in range(5):
        ang = k * 2 * math.pi / 5 + rng.uniform(-0.3, 0.3)
        rp, rr = bent((0, 0, 0.25), (math.cos(ang), math.sin(ang), -0.45), 1.1, 5, 0.16, 0.03, 0.1)
        obs.append(tube(f'Root{k}', rp, rr, sides=7))
    finish('dead_tree', {'Bark': obs}, {'Bark': 1024})


def build_barbed_wire():
    """filo spinato a spirale lungo 3 m su cavalletti a X"""
    obs, wood = [], []
    rng = random.Random(3)
    L_, R, turns = 3.0, 0.42, 16
    pts = []
    n = turns * 14
    for k in range(n + 1):
        t = k / n; a = t * turns * 2 * math.pi
        pts.append((-L_ / 2 + L_ * t + 0.02 * math.sin(a * 3), R * math.cos(a) * (1 + 0.06 * math.sin(a * 0.7)), R + R * math.sin(a) * 0.95 + 0.04))
    obs.append(tube('Coil', pts, 0.005, sides=5, cap=False))
    for s in (-1, 1):   # due fili tesi lungo
        obs.append(tube(f'Line{s}', [(-L_ / 2, s * R * 0.7, R * 1.1), (0, s * R * 0.72, R * 1.05), (L_ / 2, s * R * 0.7, R * 1.1)], 0.004, sides=5))
    for k in range(0, n, 3):   # spine
        p = np.array(pts[k])
        for q in range(2):
            d = np.array([rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)]); d /= np.linalg.norm(d)
            obs.append(tube(f'Barb{k}{q}', [p - d * 0.018, p + d * 0.018], 0.0025, sides=4))
    for xp in (-L_ / 2 + 0.1, 0.0, L_ / 2 - 0.1):   # cavalletti
        for s in (-1, 1):
            wood.append(box(f'X{xp}{s}', (xp, 0, 0.5), (0.05, 0.05, 1.25), rot=(s * 0.55, 0, 0), bevel=0.006))
        wood.append(box(f'Xb{xp}', (xp, 0, 0.62), (0.04, 0.9, 0.04), bevel=0.004))
    finish('barbed_wire', {'Iron': obs, 'Wood': wood}, {'Iron': 512, 'Wood': 512})


def ibeam(name, length, rot, c=(0, 0, 0)):
    """trave a doppia T con anima e ali"""
    parts = [box(name + 'w', (0, 0, 0), (0.016, 0.15, length), bevel=0.002),
             box(name + 'f1', (0.0, 0.07, 0), (0.13, 0.016, length), bevel=0.002),
             box(name + 'f2', (0.0, -0.07, 0), (0.13, 0.016, length), bevel=0.002)]
    for p in parts: p.location = c
    ob = join(parts, name)
    ob.rotation_euler = rot; ob.location = c
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=False)
    displace(ob, 0.004, 3.0, seed=len(name))
    return ob


def build_czech_hedgehog():
    """cavallo di frisia: tre travi d'acciaio incrociate e imbullonate, alto 1.2 m"""
    c = (0, 0, 0.62)
    beams = [ibeam('A', 1.9, (0.0, math.radians(54.7), math.radians(45)), c),
             ibeam('B', 1.9, (0.0, math.radians(54.7), math.radians(165)), c),
             ibeam('C', 1.9, (0.0, math.radians(54.7), math.radians(285)), c)]
    bolts = [box('Plate', c, (0.16, 0.16, 0.16), bevel=0.02)]
    finish('czech_hedgehog', {'Iron': beams + bolts}, {'Iron': 512})


def build_grave_cross():
    """croce di legno storta su un mucchio di terra, elmetto appeso, teschio e straccio"""
    rng = random.Random(5)
    wood = [box('Up', (0, 0, 0.75), (0.08, 0.05, 1.55), bevel=0.008), box('Arm', (0, -0.035, 1.18), (0.6, 0.04, 0.07), bevel=0.008, rot=(0, 0.06, 0))]
    for w in wood: displace(w, 0.004, 8.0, seed=1)
    tilt = [w for w in wood]
    # mucchio di terra
    nu, nv = 24, 8
    def mound(i, j):
        a = 2 * math.pi * i / nu; r = 0.55 * (1 - j / nv) ** 0.7 + 0.01
        z = 0.16 * (1 - (r / 0.56) ** 2) + 0.03 * noise3((math.cos(a) * 3, math.sin(a) * 3, j), 2)
        return (r * math.cos(a) * 1.4, r * math.sin(a), max(z, 0.0) - 0.02)
    earth = grid_surface('Mound', mound, nu, nv, closed_u=True)
    # elmetto appeso alla croce
    hv, hf = [], []
    nu2, nv2 = 20, 7
    for j in range(nv2 + 1):
        for i in range(nu2):
            a = 2 * math.pi * i / nu2; t = j / nv2
            if j < nv2 - 1: r = 0.15 * math.sin(t * math.pi / 2 / (1 - 1 / nv2)) + 0.005; z = 0.11 * math.cos(t * math.pi / 2 / (1 - 1 / nv2))
            else: r = 0.19; z = -0.005
            hv.append((r * math.cos(a), r * math.sin(a), z))
    for j in range(nv2):
        for i in range(nu2):
            a = j * nu2 + i; b = j * nu2 + (i + 1) % nu2
            hf.append((a, b, b + nu2, a + nu2))
    helmet = link('Helmet', hv, hf)
    m = helmet.modifiers.new('S', 'SOLIDIFY'); m.thickness = 0.006; apply_mods(helmet)
    helmet.location = (0.02, -0.03, 1.47); helmet.rotation_euler = (0.25, -0.2, 0.3)
    activate(helmet); bpy.ops.object.transform_apply(location=True, rotation=True, scale=False)
    # teschio ai piedi
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=10, radius=0.09, location=(0.22, -0.28, 0.14))
    skull = bpy.context.active_object; skull.scale = (0.85, 1.05, 0.95)
    bpy.ops.object.transform_apply(scale=True)
    jaw = box('Jaw', (0.22, -0.345, 0.09), (0.09, 0.05, 0.04), bevel=0.01)
    # straccio legato al braccio
    rag = tube('Rag', [(-0.22, -0.06, 1.16), (-0.25, -0.07, 0.95), (-0.21, -0.08, 0.78)], [0.035, 0.03, 0.012], sides=6)
    finish('grave_cross', {'Wood': tilt, 'Earth': [earth], 'Helmet': [helmet], 'Bone': [skull, jaw], 'Cloth': [rag]},
           {'Wood': 512, 'Earth': 512, 'Helmet': 256, 'Bone': 256, 'Cloth': 256})


PROPS = {'sandbag_wall': build_sandbag_wall, 'trench_berm': build_trench_berm, 'dead_tree': build_dead_tree,
         'barbed_wire': build_barbed_wire, 'czech_hedgehog': build_czech_hedgehog, 'grave_cross': build_grave_cross}

PHOTO_DIMS.update(ensure_photos())
for pid, fn in PROPS.items():
    if ONLY and pid not in ONLY.split(','): continue
    reset(); fn()
