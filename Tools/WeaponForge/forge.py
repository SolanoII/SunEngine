# Forgia delle armi: trasforma ogni modello a blocchi (Sandbox/GameData/BlockModels/<id>.json) in una mesh vera,
# Sandbox/GameData/Models/Weapons/<id>.glb, con lo stesso ingombro e gli stessi punti d'appoggio delle mani.
#
# Ogni blocco diventa un pezzo con la sua forma (canne e anelli rotondi, lame affilate, denti e punte a cuneo,
# rivetti, impugnature arrotondate, il resto smussato) e il suo materiale (ferro brunito, acciaio delle lame, legno
# scuro, cuoio, ottone ossidato, vernice scrostata, celle d'energia). I colori dei blocchi servono solo a capire il
# materiale: niente colori da giocattolo, tutto e' consumato, sporco e scrostato.
# La texture (colore, ruvidita', metallo) e' cotta con Cycles: spigoli lucidati dall'uso, sporco negli incavi,
# graffi, ruggine, polvere sulle superfici in alto. Le parti luminose (celle, lenti) hanno un materiale a parte.
#
# Uso:  python Tools/WeaponForge/forge.py [--only id1,id2] [--preview <cartella>] [--size 1024]
import sys, os, json, math, glob, colorsys, random, argparse
import numpy as np
import bpy, bmesh
from mathutils import Vector, Matrix, Euler

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
BLOCKS = os.path.join(ROOT, 'Sandbox', 'GameData', 'BlockModels')
OUT = os.path.join(ROOT, 'Sandbox', 'GameData', 'Models', 'Weapons')

ROUND = ('canna', 'barrel', 'tubo', 'camicia', 'smorzatore', 'ugello', 'bocca', 'freno', 'muzzle', 'exhaust', 'shroud',
         'campana', 'anello', 'ring', 'fascia', 'fascetta', 'vent', 'bobina', 'coil', 'cartucce', 'serbatoio', 'fiasca',
         'spoletta', 'lente', 'lens', 'emitter', 'tamburo', 'pomolo', 'pommel', 'rivet', 'rivetto', 'vite', 'perno', 'tappo',
         'corpo', 'body', 'razzo', 'testa', 'cella', 'cell', 'cilindro', 'palo', 'colonna', 'miccia', 'filo', 'cavo',
         'mirino', 'cane', 'spia', 'microfono', 'cornetta', 'teste', 'rotolo', 'pompa', 'dissipatore', 'ottica')
BOXY = ('castello', 'receiver', 'piastra', 'plate', 'calcio', 'stock', 'guardamano', 'handguard', 'impugnatura', 'grip',
        'caricatore', 'magazine', 'mag', 'cassa', 'cassetta', 'lamiera', 'coperchio', 'carrello', 'blade', 'lama', 'scudo',
        'zaino', 'pannello', 'lastra', 'cheek', 'poggiaguancia', 'butt', 'calciolo', 'top', 'rail', 'engine', 'power')
BLADE = ('blade', 'lama', 'edge', 'fuller', 'bar_tip', 'ganascia')
POINTY = ('tooth', 'dente', 'spike', 'punta', 'aletta', 'ala')
WOOD = ('calcio', 'stock', 'guardamano', 'handguard', 'impugnatura', 'grip', 'handle', 'manico', 'maniglia', 'cheek',
        'poggiaguancia', 'cassa', 'palo', 'traversa')
LEATHER = ('wrap', 'cinghia', 'nastro', 'fibbia', 'astuccio', 'strap')
SKIP = ('rust',)                 # le "toppe" di ruggine diventano macchie nella texture


def has(name, keys):
    n = name.lower()
    return any(k in n for k in keys)


def g2b(v):
    """spazio dell'arma (+Y alto, canna verso -Z) -> Blender (Z alto, canna verso +Y)"""
    return Vector((v[0], -v[2], v[1]))


def classify(p):
    """materiale di un blocco: dal nome e dal colore"""
    name = p['name'].lower()
    r, g, b = p.get('color', [0.5, 0.5, 0.5, 1])[:3]
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    met = p.get('metallic', 0.0)
    if p.get('emissive', 0.0) > 0.05: return 'glow'
    if has(name, SKIP): return 'skip'
    if has(name, BLADE) or has(name, POINTY) and l > 0.45: return 'blade'
    if has(name, LEATHER): return 'leather'
    brownish = 0.03 < h < 0.13 and s > 0.25 and met < 0.45
    if has(name, WOOD) and (brownish or met < 0.15 and l > 0.1): return 'wood' if brownish or l > 0.12 else 'leather'
    if brownish and met < 0.3: return 'wood'
    if 0.08 < h < 0.17 and s > 0.4 and l > 0.3 and met > 0.5: return 'brass'
    if met < 0.2 and l < 0.12: return 'leather' if has(name, ('handle', 'grip', 'manico', 'impugnatura')) else 'iron'
    if s > 0.22 and l > 0.15: return 'paint'
    return 'iron'


# colori di base (lineari) e ruvidita'/metallo dei materiali
MAT = {
    'iron':    dict(col=(0.03, 0.029, 0.028), rough=0.55, metal=0.85),
    'blade':   dict(col=(0.13, 0.125, 0.12), rough=0.4, metal=1.0),
    'wood':    dict(col=(0.034, 0.018, 0.009), rough=0.6, metal=0.0),
    'leather': dict(col=(0.02, 0.014, 0.01), rough=0.7, metal=0.0),
    'brass':   dict(col=(0.16, 0.1, 0.035), rough=0.5, metal=0.95),
    'paint':   dict(col=None, rough=0.7, metal=0.15),
    'glow':    dict(col=None, rough=0.3, metal=0.2),
}


def paint_color(c):
    """vernice di fazione: spenta, scura, sporca"""
    h, l, s = colorsys.rgb_to_hls(*c[:3])
    return colorsys.hls_to_rgb(h, min(l, 0.5) * 0.13, s * 0.35)


# ---------------------------------------------------------------------------------------------- geometria dei pezzi
def bevel(bm, w, seg=2, only_edges=None):
    edges = only_edges if only_edges is not None else bm.edges[:]
    if w > 1e-5:
        bmesh.ops.bevel(bm, geom=edges, offset=w, segments=seg, profile=0.5, affect='EDGES', clamp_overlap=True)


def make_box(size, bev):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    bevel(bm, bev)
    return bm


def make_cyl(size, axis, bev, seg=16):
    """cilindro (sezione ellittica) lungo l'asse 'axis' (0,1,2 in Blender)"""
    dims = list(size)
    L = dims[axis]; a, b = [dims[i] for i in range(3) if i != axis]
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=0.5, radius2=0.5, depth=1.0)
    for v in bm.verts:
        x, y, z = v.co.x * a, v.co.y * b, v.co.z * L
        v.co = Vector((z, x, y)) if axis == 0 else (Vector((x, z, y)) if axis == 1 else Vector((x, y, z)))
    caps = [e for e in bm.edges if all(abs(abs(vv.co[axis]) - L / 2) < 1e-6 for vv in e.verts)]
    bevel(bm, bev, 2, caps)
    return bm


def make_pointy(size, out_axis, sign):
    """cuneo/punta: base larga, punta verso fuori lungo out_axis"""
    bm = make_box(size, 0.0)
    for v in bm.verts:
        if v.co[out_axis] * sign > 0:
            for i in range(3):
                if i != out_axis: v.co[i] *= 0.08
    return bm


def make_blade(size, edge_axis, edge_sign, thin_axis):
    """lama: lo spessore si assottiglia verso il filo"""
    bm = make_box(size, min(size) * 0.08)
    half = size[edge_axis] / 2
    for v in bm.verts:
        t = (v.co[edge_axis] * edge_sign + half) / (2 * half)          # 0 dorso, 1 filo
        v.co[thin_axis] *= max(0.12, 1.0 - 0.85 * t ** 1.6)
    return bm


def part_mesh(p, centre, all_parts):
    name = p['name'].lower()
    size_g = np.array(p['size'], float)
    size = np.abs(np.array([size_g[0], size_g[2], size_g[1]]))              # Blender: x, y(=-z del gioco), z(=y del gioco)
    mn = float(size.min())
    order = np.argsort(size)
    kind = classify(p)
    if has(name, POINTY):
        off = np.array(g2b(p['offset'])) - centre
        ax = int(np.argmax(np.abs(off) / np.maximum(size, 1e-4)))
        bm = make_pointy(size, ax, 1 if off[ax] >= 0 else -1)
    elif kind == 'blade' and has(name, BLADE):
        off = np.array(g2b(p['offset'])) - centre
        thin = int(order[0])
        rest = [i for i in range(3) if i != thin]
        long_ = rest[int(np.argmax(size[rest]))]; edge = [i for i in rest if i != long_][0]
        sign = -1 if off[edge] > 0 else 1                                 # il filo e' dalla parte opposta al centro
        if has(name, ('edge', 'filo')): sign = 1 if off[edge] >= 0 else -1
        bm = make_blade(size, edge, sign, thin)
    elif has(name, ROUND) and not has(name, BOXY):
        a, b, c = size[order]
        if c / max(b, 1e-5) > 1.15 and b / max(a, 1e-5) < 1.35:
            bm = make_cyl(size, int(order[2]), mn * 0.12)                   # lungo: cilindro lungo l'asse maggiore
        elif b / max(a, 1e-5) > 1.15 and c / max(b, 1e-5) < 1.35:
            bm = make_cyl(size, int(order[0]), mn * 0.2)                    # piatto: disco lungo l'asse minore
        else:
            bm = make_cyl(size, 2 if has(name, ('corpo', 'body', 'spoletta', 'testa')) else int(order[2]), mn * 0.15)
    elif kind in ('wood', 'leather') or has(name, ('grip', 'handle', 'impugnatura', 'manico', 'maniglia')):
        bm = make_box(size, mn * 0.3)
    else:
        bm = make_box(size, mn * 0.14)
    # posizione e rotazione del blocco (gradi, ordine Y, X, Z nello spazio del gioco)
    rot = p.get('rotation', [0, 0, 0]) or [0, 0, 0]
    R = Matrix.Rotation(math.radians(rot[1]), 4, 'Z') @ Matrix.Rotation(math.radians(rot[0]), 4, 'X') @ Matrix.Rotation(math.radians(-rot[2]), 4, 'Y')
    M = Matrix.Translation(g2b(p['offset'])) @ R
    bmesh.ops.transform(bm, matrix=M, verts=bm.verts)
    return bm, kind


# ---------------------------------------------------------------------------------------------- materiale cotto
def build_bake_material(rust_spots, size_hint):
    m = bpy.data.materials.new('bake'); m.use_nodes = True
    nt = m.node_tree; N = nt.nodes; Lk = nt.links
    for n in list(N): N.remove(n)
    out = N.new('ShaderNodeOutputMaterial'); emit = N.new('ShaderNodeEmission')
    Lk.new(emit.outputs[0], out.inputs['Surface'])
    attr = N.new('ShaderNodeAttribute'); attr.attribute_name = 'base'         # colore del materiale
    rm = N.new('ShaderNodeAttribute'); rm.attribute_name = 'rm'               # x ruvidita', y metallo, z tipo (1 metallo nudo possibile)
    geo = N.new('ShaderNodeNewGeometry')
    ao = N.new('ShaderNodeAmbientOcclusion'); ao.inputs['Distance'].default_value = size_hint * 0.08; ao.samples = 8
    tc = N.new('ShaderNodeTexCoord')

    def noise(scale, detail=4, vec=None):
        n = N.new('ShaderNodeTexNoise'); n.inputs['Scale'].default_value = scale; n.inputs['Detail'].default_value = detail
        Lk.new((vec or tc.outputs['Object']), n.inputs['Vector']); return n

    def math_(op, a, b=None, clamp=True):
        n = N.new('ShaderNodeMath'); n.operation = op; n.use_clamp = clamp
        for i, x in enumerate((a, b)):
            if x is None: continue
            if isinstance(x, (int, float)): n.inputs[i].default_value = x
            else: Lk.new(x, n.inputs[i])
        return n.outputs[0]

    def mix(fac, a, b):
        n = N.new('ShaderNodeMix'); n.data_type = 'RGBA'
        Lk.new(fac, n.inputs['Factor'])
        for slot, x in (('A', a), ('B', b)):
            if isinstance(x, tuple): n.inputs[slot].default_value = (*x, 1)
            else: Lk.new(x, n.inputs[slot])
        return n.outputs['Result']

    # spigoli consumati: pointiness alta = spigolo vivo; rotto dal rumore
    pt = math_('SUBTRACT', geo.outputs['Pointiness'], 0.53)
    edge = math_('MULTIPLY', pt, 9.0)
    wn = noise(60.0 / max(size_hint, 0.05), 6)
    edge = math_('MULTIPLY', edge, math_('ADD', wn.outputs['Fac'], 0.15))
    sep = N.new('ShaderNodeSeparateXYZ'); Lk.new(rm.outputs['Vector'], sep.inputs[0])
    edge = math_('MULTIPLY', edge, sep.outputs[2])                          # solo dove sotto c'e' metallo
    # sporco negli incavi
    grime_n = noise(40.0 / max(size_hint, 0.05), 5)
    dirt = math_('MULTIPLY', math_('SUBTRACT', 1.0, ao.outputs['AO']), math_('ADD', grime_n.outputs['Fac'], 0.4))
    # polvere sulle superfici rivolte in alto (Z di Blender)
    nz = N.new('ShaderNodeSeparateXYZ'); Lk.new(geo.outputs['Normal'], nz.inputs[0])
    dust_n = noise(25.0 / max(size_hint, 0.05), 3)
    dust = math_('MULTIPLY', math_('SUBTRACT', nz.outputs[2], 0.55), math_('MULTIPLY', dust_n.outputs['Fac'], 1.6))
    # graffi: rumore stirato
    mp = N.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (1.0, 14.0, 1.0)
    Lk.new(tc.outputs['Object'], mp.inputs['Vector'])
    scr = noise(160.0 / max(size_hint, 0.05) ** 0.5, 2, mp.outputs['Vector'])
    scratch = math_('MULTIPLY', math_('SUBTRACT', scr.outputs['Fac'], 0.7), 5.0)
    scratch = math_('MULTIPLY', scratch, sep.outputs[2])
    # ruggine: macchie a rumore + vicino alle "toppe" dei blocchi
    rn = noise(18.0 / max(size_hint, 0.05), 6)
    rust = math_('MULTIPLY', math_('SUBTRACT', rn.outputs['Fac'], 0.62), 3.0)
    rust = math_('MULTIPLY', rust, sep.outputs[2])
    if rust_spots:
        attr_r = N.new('ShaderNodeAttribute'); attr_r.attribute_name = 'rustmask'
        rust = math_('MAXIMUM', rust, attr_r.outputs['Fac'])

    col = attr.outputs['Color']
    gm = N.new('ShaderNodeMapping'); gm.inputs['Scale'].default_value = (1.0, 0.06, 1.0)
    Lk.new(tc.outputs['Object'], gm.inputs['Vector'])
    grain = noise(220.0 / max(size_hint, 0.05) ** 0.5, 3, gm.outputs['Vector'])
    isw = math_('SUBTRACT', 1.0, math_('MULTIPLY', math_('ADD', sep.outputs[1], sep.outputs[2]), 10.0))   # legno e cuoio
    gr = math_('MULTIPLY', math_('SUBTRACT', grain.outputs['Fac'], 0.45), math_('MULTIPLY', isw, 1.4))
    col = mix(gr, col, (0.012, 0.006, 0.003))
    col = mix(rust, col, (0.11, 0.035, 0.012))
    col = mix(dirt, col, (0.012, 0.01, 0.008))
    col = mix(math_('MULTIPLY', dust, 0.5), col, (0.085, 0.072, 0.056))
    col = mix(math_('MULTIPLY', scratch, 0.35), col, (0.16, 0.155, 0.15))
    col = mix(math_('MULTIPLY', edge, 0.8), col, (0.26, 0.255, 0.245))
    # canale ORM in un secondo attributo di uscita: lo sceglie il nodo "Mix" finale col valore 'mode'
    rough = math_('ADD', sep.outputs[0], math_('MULTIPLY', dirt, 0.25))
    rough = math_('SUBTRACT', rough, math_('MULTIPLY', edge, 0.3))
    rough = math_('ADD', rough, math_('MULTIPLY', rust, 0.3))
    metal = math_('SUBTRACT', sep.outputs[1], math_('MULTIPLY', rust, 0.8))
    metal = math_('MAXIMUM', metal, math_('MULTIPLY', edge, 0.95))
    metal = math_('SUBTRACT', metal, math_('MULTIPLY', dust, 0.6))
    comb = N.new('ShaderNodeCombineColor')
    comb.inputs[0].default_value = 1.0
    Lk.new(rough, comb.inputs[1]); Lk.new(metal, comb.inputs[2])
    sw = N.new('ShaderNodeMix'); sw.data_type = 'RGBA'; sw.name = 'switch'
    sw.inputs['Factor'].default_value = 0.0
    Lk.new(col, sw.inputs['A']); Lk.new(comb.outputs[0], sw.inputs['B'])
    Lk.new(sw.outputs['Result'], emit.inputs['Color'])
    return m


def forge_one(path, size_px, preview_dir):
    j = json.load(open(path))
    wid = j['id']
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    parts = j['parts']
    pos = np.array([list(g2b(p['offset'])) for p in parts])
    centre = pos.mean(0)
    ext = np.max(np.abs(pos - centre).max(0)) if len(pos) else 0.1
    big = max(np.ptp(pos, 0).max() if len(pos) > 1 else 0.1, max(max(p['size']) for p in parts))

    bm_all = bmesh.new()
    base_layer = None
    glow = bmesh.new()
    glow_col = None
    rust_boxes = []
    for p in parts:
        bm, kind = part_mesh(p, centre, parts)
        if kind == 'skip':
            rust_boxes.append((np.array(g2b(p['offset'])), np.abs(np.array([p['size'][0], p['size'][2], p['size'][1]])) * 0.5 + 0.01))
            bm.free(); continue
        if kind == 'glow':
            c = np.array(p['color'][:3]); h, l, s = colorsys.rgb_to_hls(*c)
            glow_col = colorsys.hls_to_rgb(h, min(l, 0.5) * 0.8, s * 0.7)
            me = bpy.data.meshes.new('tmp'); bm.to_mesh(me); bm.free()
            glow.from_mesh(me); bpy.data.meshes.remove(me); continue
        mt = MAT[kind]
        col = mt['col'] if mt['col'] is not None else paint_color(p['color'])
        if kind == 'iron':
            # ferro: chi era chiaro nel blocco resta un filo piu' chiaro (parti lucidate, mirini)
            l = colorsys.rgb_to_hls(*p['color'][:3])[1]
            col = tuple(np.array(col) * (0.85 + 0.6 * l))
        bare = 1.0 if kind in ('iron', 'blade', 'paint', 'brass') else 0.0
        me = bpy.data.meshes.new('tmp'); bm.to_mesh(me); bm.free()
        ca = me.color_attributes.new('base', 'FLOAT_COLOR', 'POINT')
        rmv = me.attributes.new('rm', 'FLOAT_VECTOR', 'POINT')
        for i in range(len(me.vertices)):
            ca.data[i].color = (*col, 1.0)
            rmv.data[i].vector = (mt['rough'], mt['metal'], bare)
        bm_all.from_mesh(me)
        bpy.data.meshes.remove(me)

    me = bpy.data.meshes.new(wid); bm_all.to_mesh(me); bm_all.free()
    ob = bpy.data.objects.new(wid, me); sc.collection.objects.link(ob)
    for poly in me.polygons: poly.use_smooth = True
    me.set_sharp_from_angle(angle=math.radians(40)) if hasattr(me, 'set_sharp_from_angle') else None
    # macchie di ruggine dove c'erano le toppe
    rm = me.attributes.new('rustmask', 'FLOAT', 'POINT')
    V = np.array([v.co[:] for v in me.vertices])
    mask = np.zeros(len(V))
    for c, h in rust_boxes:
        d = np.max(np.abs(V - c) / h, axis=1)
        mask = np.maximum(mask, np.clip(1.4 - d, 0, 1))
    for i, v in enumerate(mask): rm.data[i].value = float(v)

    # UV e cottura: colore, poi ruvidita'/metallo
    bpy.context.view_layer.objects.active = ob; ob.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(50), island_margin=0.006)
    bpy.ops.object.mode_set(mode='OBJECT')
    bm_mat = build_bake_material(bool(rust_boxes), big)
    me.materials.append(bm_mat)
    res = size_px if big > 0.35 else size_px // 2
    img_c = bpy.data.images.new(wid + '_BaseColor', res, res)
    img_o = bpy.data.images.new(wid + '_ORM', res, res); img_o.colorspace_settings.name = 'Non-Color'
    tn = bm_mat.node_tree.nodes.new('ShaderNodeTexImage')
    sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = 4; sc.render.bake.margin = 6
    for img, sw in ((img_c, 0.0), (img_o, 1.0)):
        tn.image = img; bm_mat.node_tree.nodes.active = tn
        bm_mat.node_tree.nodes['switch'].inputs['Factor'].default_value = sw
        bpy.ops.object.bake(type='EMIT')
        img.pack()
    # materiale finale (PBR con le due texture): "Weapon" e' metallo per il motore
    me.materials.clear()
    wm = bpy.data.materials.new('Weapon'); wm.use_nodes = True
    nt = wm.node_tree; bs = nt.nodes['Principled BSDF']
    tc_ = nt.nodes.new('ShaderNodeTexImage'); tc_.image = img_c
    to_ = nt.nodes.new('ShaderNodeTexImage'); to_.image = img_o
    sepc = nt.nodes.new('ShaderNodeSeparateColor')
    nt.links.new(tc_.outputs['Color'], bs.inputs['Base Color'])
    nt.links.new(to_.outputs['Color'], sepc.inputs[0])
    nt.links.new(sepc.outputs['Green'], bs.inputs['Roughness'])
    nt.links.new(sepc.outputs['Blue'], bs.inputs['Metallic'])
    me.materials.append(wm)
    objs = [ob]
    if len(glow.verts):
        gme = bpy.data.meshes.new(wid + '_glow'); glow.to_mesh(gme)
        gob = bpy.data.objects.new(wid + '_glow', gme); sc.collection.objects.link(gob)
        gm = bpy.data.materials.new('Weapon_Glow'); gm.use_nodes = True
        b2 = gm.node_tree.nodes['Principled BSDF']
        b2.inputs['Base Color'].default_value = (*[c * 0.3 for c in glow_col], 1)
        b2.inputs['Emission Color'].default_value = (*glow_col, 1); b2.inputs['Emission Strength'].default_value = 1.2
        b2.inputs['Roughness'].default_value = 0.25
        gme.materials.append(gm)
        for poly in gme.polygons: poly.use_smooth = True
        objs.append(gob)
    glow.free()
    os.makedirs(OUT, exist_ok=True)
    for o in bpy.context.view_layer.objects: o.select_set(o in objs)
    outp = os.path.join(OUT, wid + '.glb')
    bpy.ops.export_scene.gltf(filepath=outp, export_format='GLB', use_selection=True, export_yup=True,
                              export_apply=True, export_image_format='JPEG', export_jpeg_quality=90)
    if preview_dir:
        preview(objs, preview_dir, wid, big)
    return outp, len(me.polygons)


def preview(objs, d, wid, big):
    sc = bpy.context.scene
    try: sc.render.engine = 'BLENDER_EEVEE_NEXT'
    except Exception: sc.render.engine = 'BLENDER_EEVEE'
    w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
    w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.3, 0.3, 0.32, 1)
    for nm, rot, en in (('k', (50, 0, 40), 3.5), ('f', (65, 0, -130), 1.2)):
        l = bpy.data.objects.new(nm, bpy.data.lights.new(nm, 'SUN')); l.data.energy = en
        l.rotation_euler = [math.radians(x) for x in rot]; sc.collection.objects.link(l)
    cam = bpy.data.objects.new('c', bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.type = 'ORTHO'; cam.data.ortho_scale = big * 1.25
    bb = np.array([o.matrix_world @ Vector(c) for o in objs for c in o.bound_box])
    ctr = Vector(bb.mean(0))
    cam.location = ctr + Vector((big * 2, -big * 0.6, big * 0.5))
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Y').to_euler()
    sc.render.resolution_x, sc.render.resolution_y = 640, 400
    os.makedirs(d, exist_ok=True)
    sc.render.filepath = os.path.join(d, wid + '.png'); bpy.ops.render.render(write_still=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--preview', default='')
    ap.add_argument('--size', type=int, default=512)
    a = ap.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:])
    only = set(filter(None, a.only.split(',')))
    files = sorted(glob.glob(os.path.join(BLOCKS, '*.json')))
    for f in files:
        wid = os.path.splitext(os.path.basename(f))[0]
        if only and wid not in only: continue
        try:
            outp, nt = forge_one(f, a.size, a.preview)
            print(f'[armi] {wid}: {nt} facce -> {os.path.relpath(outp, ROOT)}', flush=True)
        except Exception as ex:
            import traceback; traceback.print_exc()
            print(f'[armi] {wid}: ERRORE {ex}', flush=True)
