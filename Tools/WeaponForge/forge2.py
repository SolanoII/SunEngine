# Forgia delle armi, seconda versione: armi modellate per archetipo (arch.py) sulle misure dei modelli a blocchi,
# con texture cotte (colore, ruvidita'/metallo, normal map con venature, butterature, martellature, cuoio).
#
#   python Tools/WeaponForge/forge2.py                                   # tutte
#   python Tools/WeaponForge/forge2.py --only footman_auto_rifle --preview anteprime/
#
# Le armi che non hanno un archetipo (barricate, muri, stendardi, basi) restano quelle della forgia v1 (forge.py).
import os, sys, json, glob, math, argparse, zlib
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bpy, bmesh
from mathutils import Vector, Matrix
import kit
from kit import MATS
import arch
import arch2
from arch import Lay

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
BLOCKS = os.path.join(ROOT, 'Sandbox', 'GameData', 'BlockModels')
OUT = os.path.join(ROOT, 'Sandbox', 'GameData', 'Models', 'Weapons')

FACTION = {'k': ('footman', 'cadet', 'levy', 'recruit', 'auto_pistol_standard', 'auto_rifle_mg', 'combat_knife', 'impulse_pistol_standard', 'standard_toothsword'),
           'v': ('candidate', 'defender', 'explorer', 'konrad'),
           'x': ('gunner', 'pighcib', 'raider', 'tusker', 'kragh', 'vrepigh', 'zhelvar')}
SKIP = ('banner', '_base', 'wall', 'barricade', 'pillar')


def style_of(wid):
    for s, keys in FACTION.items():
        if any(wid.startswith(k) for k in keys): return s
    return 'k'


def archetype(wid, style):
    """id -> (funzione, argomenti extra)"""
    A = arch
    has = lambda *k: any(x in wid for x in k)
    if has(*SKIP): return None
    if has('knife'): return A.knife, {}
    if has('toothsword'): return A.toothsword, {}
    if has('heavy_cleaver'): return A.cleaver, {'heavy': True}
    if has('cleaver'): return A.cleaver, {}
    if has('wrench'): return A.wrench, {}
    if has('nail_grenade'): return A.grenade, {'kind': 'stick'}
    if has('scrap_grenade'): return A.grenade, {'kind': 'scrap'}
    if has('frag', 'piercing_grenade'): return A.grenade, {'kind': 'frag'}
    if has('mines'): return A.mine, {}
    if has('demolition_bomb'): return A.kit_box, {'kind': 'bomb'}
    if has('medkit'): return A.kit_box, {'kind': 'medkit'}
    if has('radio'): return A.kit_box, {'kind': 'radio'}
    if has('scanner'): return A.kit_box, {'kind': 'scanner'}
    if has('power_pack'): return A.kit_box, {'kind': 'power'}
    if has('cloak'): return A.kit_box, {'kind': 'cloak'}
    if has('backpack'): return A.crate, {'kind': 'backpack'}
    if has('ammo'): return A.crate, {'kind': 'ammo'}
    if has('flamer'): return A.flamer, {}
    if has('grenade_launcher'): return A.launcher, {'kind': 'grenade'}
    if has('rocket'): return A.launcher, {'kind': 'rocket'}
    if has('blunderbuss'): return A.blunderbuss, {}
    W = arch2
    if has('impulse_cannon'): return W.war_energy, {'kind': 'impulse'}
    if has('beam'): return W.war_energy, {'kind': 'beam'}
    if has('discharge'): return W.war_energy, {'kind': 'discharge'}
    if has('fusion'): return W.war_energy, {'kind': 'fusion'}
    if has('pistol'):
        if has('impulse'): return W.hand_cannon, {'kind': 'energy'}
        if has('heavy_round', 'high_power'): return W.hand_cannon, {'kind': 'heavy'}
        if style == 'x': return W.hand_cannon, {'kind': 'crude'}
        return W.hand_cannon, {'kind': 'auto'}
    if has('impulse'): return W.war_energy, {'kind': 'impulse'}
    if has('_mg', 'heavy_crude_gun'): return W.war_rifle, {'kind': 'mg'}
    if has('shotgun'): return W.war_rifle, {'kind': 'shotgun'}
    if has('sniper', 'precision', 'high_power'): return W.war_rifle, {'kind': 'bolt'}
    if has('heavy_round'): return W.war_rifle, {'kind': 'heavy'}
    return W.war_rifle, {'kind': 'auto'}


def g2b(v):
    return (v[0], -v[2], v[1])


# ---------------------------------------------------------------------------------------------- materiale da cuocere
def bake_material(size):
    m = bpy.data.materials.new('bake'); m.use_nodes = True
    nt = m.node_tree; N = nt.nodes; Lk = nt.links
    for n in list(N): N.remove(n)
    out = N.new('ShaderNodeOutputMaterial'); out.name = 'out'
    emit = N.new('ShaderNodeEmission'); emit.name = 'emit'
    base = N.new('ShaderNodeAttribute'); base.attribute_name = 'base'
    rm = N.new('ShaderNodeAttribute'); rm.attribute_name = 'rm'      # ruvidita', metallo, nudo sotto
    fx = N.new('ShaderNodeAttribute'); fx.attribute_name = 'fx'      # tipo di rilievo, ruggine, -
    geo = N.new('ShaderNodeNewGeometry')
    ao = N.new('ShaderNodeAmbientOcclusion'); ao.inputs['Distance'].default_value = size * 0.05; ao.samples = 8
    tc = N.new('ShaderNodeTexCoord')
    S = 1.0 / max(size, 0.05)

    def M(op, a, b=None, clamp=True):
        n = N.new('ShaderNodeMath'); n.operation = op; n.use_clamp = clamp
        for i, x in enumerate((a, b)):
            if x is None: continue
            if isinstance(x, (int, float)): n.inputs[i].default_value = x
            else: Lk.new(x, n.inputs[i])
        return n.outputs[0]

    def noise(scale, detail=4, vec=None, rough=0.55):
        n = N.new('ShaderNodeTexNoise'); n.inputs['Scale'].default_value = scale; n.inputs['Detail'].default_value = detail
        n.inputs['Roughness'].default_value = rough
        Lk.new(vec or tc.outputs['Object'], n.inputs['Vector']); return n.outputs['Fac']

    def voronoi(scale, vec=None):
        n = N.new('ShaderNodeTexVoronoi'); n.inputs['Scale'].default_value = scale
        Lk.new(vec or tc.outputs['Object'], n.inputs['Vector']); return n.outputs['Distance']

    def stretched(sc):
        mp = N.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = sc
        Lk.new(tc.outputs['Object'], mp.inputs['Vector']); return mp.outputs['Vector']

    def mix(fac, a, b):
        n = N.new('ShaderNodeMix'); n.data_type = 'RGBA'; n.clamp_factor = True
        Lk.new(fac, n.inputs['Factor'])
        for slot, x in (('A', a), ('B', b)):
            if isinstance(x, tuple): n.inputs[slot].default_value = (*x, 1)
            else: Lk.new(x, n.inputs[slot])
        return n.outputs['Result']

    rsep = N.new('ShaderNodeSeparateXYZ'); Lk.new(rm.outputs['Vector'], rsep.inputs[0])
    fsep = N.new('ShaderNodeSeparateXYZ'); Lk.new(fx.outputs['Vector'], fsep.inputs[0])

    def _cmp(t):
        n = N.new('ShaderNodeMath'); n.operation = 'COMPARE'
        Lk.new(fsep.outputs[0], n.inputs[0]); n.inputs[1].default_value = float(t); n.inputs[2].default_value = 0.4
        return n.outputs[0]

    bare = rsep.outputs[2]
    # --- maschere d'usura
    bev = N.new('ShaderNodeBevel'); bev.inputs['Radius'].default_value = min(0.004, size * 0.006); bev.samples = 8
    dp = N.new('ShaderNodeVectorMath'); dp.operation = 'DOT_PRODUCT'
    Lk.new(bev.outputs['Normal'], dp.inputs[0]); Lk.new(geo.outputs['Normal'], dp.inputs[1])
    pt = M('MULTIPLY', M('SUBTRACT', 0.985, dp.outputs['Value']), 14.0)
    edge = M('MULTIPLY', M('MULTIPLY', pt, M('ADD', noise(70 * S, 6), 0.1)), bare)
    dirt = M('MULTIPLY', M('SUBTRACT', 1.0, ao.outputs['AO']), M('ADD', noise(45 * S, 5), 0.5))
    nz = N.new('ShaderNodeSeparateXYZ'); Lk.new(geo.outputs['Normal'], nz.inputs[0])
    dust = M('MULTIPLY', M('SUBTRACT', nz.outputs[2], 0.5), M('MULTIPLY', noise(28 * S, 3), 1.5))
    scratch = M('MULTIPLY', M('MULTIPLY', M('SUBTRACT', noise(120 * S ** 0.5 * 4, 2, stretched((1.0, 16.0, 1.0))), 0.68), 6.0), bare)
    # ruggine: chiazze grandi + puntini, quantita' dal materiale, piu' forte negli incavi
    rn = M('ADD', M('ADD', M('MULTIPLY', noise(26 * S, 8, None, 0.65), 0.75), M('MULTIPLY', noise(110 * S, 4), 0.25)), M('MULTIPLY', M('SUBTRACT', 1.0, ao.outputs['AO']), 0.35))
    thr = M('SUBTRACT', 0.78, M('MULTIPLY', fsep.outputs[1], 0.36))
    rust = M('MULTIPLY', M('SUBTRACT', rn, thr), 4.0)
    rust = M('MULTIPLY', rust, M('GREATER_THAN', fsep.outputs[1], 0.001))
    rust_tone = mix(noise(60 * S, 4), (0.13, 0.04, 0.012), (0.05, 0.016, 0.006))
    # --- legno: venature lungo l'arma (asse Y di Blender)
    grain = noise(260 * S ** 0.5, 4, stretched((1.0, 0.05, 1.0)))
    isw = _cmp(1)
    col = base.outputs['Color']
    col = mix(M('MULTIPLY', M('MULTIPLY', M('SUBTRACT', grain, 0.42), 2.2), isw), col, (0.008, 0.004, 0.002))
    # --- lamiera martellata: chiazze di tono
    ham = _cmp(3)
    col = mix(M('MULTIPLY', M('MULTIPLY', noise(30 * S, 3), 0.6), ham), col, (0.03, 0.022, 0.016))
    col = mix(rust, col, rust_tone)
    col = mix(M('MULTIPLY', dirt, 0.9), col, (0.008, 0.007, 0.006))
    col = mix(M('MULTIPLY', dust, 0.45), col, (0.075, 0.064, 0.05))
    col = mix(M('MULTIPLY', scratch, 0.4), col, (0.15, 0.148, 0.142))
    col = mix(M('MULTIPLY', edge, 0.85), col, (0.25, 0.245, 0.235))
    # --- ruvidita' e metallo
    rough = M('ADD', rsep.outputs[0], M('MULTIPLY', dirt, 0.2))
    rough = M('SUBTRACT', rough, M('MULTIPLY', edge, 0.3))
    rough = M('ADD', rough, M('MULTIPLY', rust, 0.35))
    rough = M('ADD', rough, M('MULTIPLY', M('SUBTRACT', noise(90 * S, 3), 0.5), 0.15))
    metal = M('SUBTRACT', rsep.outputs[1], M('MULTIPLY', rust, 0.85))
    metal = M('MAXIMUM', metal, M('MULTIPLY', edge, 0.95))
    metal = M('SUBTRACT', metal, M('MULTIPLY', dust, 0.6))
    comb = N.new('ShaderNodeCombineColor'); comb.inputs[0].default_value = 1.0
    Lk.new(rough, comb.inputs[1]); Lk.new(metal, comb.inputs[2])
    sw = N.new('ShaderNodeMix'); sw.data_type = 'RGBA'; sw.name = 'switch'; sw.inputs['Factor'].default_value = 0.0
    Lk.new(col, sw.inputs['A']); Lk.new(comb.outputs[0], sw.inputs['B'])
    Lk.new(sw.outputs['Result'], emit.inputs['Color'])
    Lk.new(emit.outputs[0], out.inputs['Surface'])
    # --- rilievo per la normal map
    h_wood = M('MULTIPLY', grain, isw)
    pits = M('MULTIPLY', M('SUBTRACT', 1.0, M('MULTIPLY', voronoi(220 * S ** 0.5 * 3), 4.0)), _cmp(2))
    h_ham = M('MULTIPLY', voronoi(40 * S), ham)
    h_lea = M('MULTIPLY', noise(500 * S ** 0.5, 8, None, 0.7), _cmp(4))
    h = M('ADD', M('ADD', M('MULTIPLY', h_wood, 0.6), M('MULTIPLY', pits, 0.25)), M('ADD', M('MULTIPLY', h_ham, 1.0), M('MULTIPLY', h_lea, 0.5)))
    h = M('ADD', h, M('MULTIPLY', rust, 0.4))
    h = M('SUBTRACT', h, M('MULTIPLY', scratch, 0.3))
    bump = N.new('ShaderNodeBump'); bump.inputs['Strength'].default_value = 0.6; bump.inputs['Distance'].default_value = 0.0015
    Lk.new(h, bump.inputs['Height'])
    bsdf = N.new('ShaderNodeBsdfPrincipled'); bsdf.name = 'bsdf'
    Lk.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
    return m


# ---------------------------------------------------------------------------------------------- un'arma
def build_mesh(B, wid):
    """fonde i pezzi in un oggetto con gli attributi del materiale, gia' nello spazio di Blender"""
    me_all = bpy.data.meshes.new(wid)
    out = bmesh.new()
    base_l = rm_l = fx_l = None
    for bm, mat in B.parts:
        col, rough, metal, bare, bump, rust = MATS[mat]
        tmp = bpy.data.meshes.new('t'); bm.to_mesh(tmp)
        n = len(tmp.vertices)
        tmp.attributes.new('base', 'FLOAT_COLOR', 'POINT').data.foreach_set('color', np.tile([*col, 1.0], n))
        tmp.attributes.new('rm', 'FLOAT_VECTOR', 'POINT').data.foreach_set('vector', np.tile([rough, metal, bare], n))
        tmp.attributes.new('fx', 'FLOAT_VECTOR', 'POINT').data.foreach_set('vector', np.tile([bump, rust, 0.0], n))
        out.from_mesh(tmp); bpy.data.meshes.remove(tmp)
    # spazio del gioco -> Blender
    kit.transform(out, Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1))))
    bmesh.ops.triangulate(out, faces=out.faces[:], quad_method='BEAUTY', ngon_method='BEAUTY')
    out.to_mesh(me_all); out.free()
    ob = bpy.data.objects.new(wid, me_all); bpy.context.scene.collection.objects.link(ob)
    for p in me_all.polygons: p.use_smooth = True
    if hasattr(me_all, 'set_sharp_from_angle'): me_all.set_sharp_from_angle(angle=math.radians(38))
    return ob


def build_glow(B, wid):
    if not B.glow: return None
    g = kit.merge(B.glow)
    kit.transform(g, Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1))))
    me = bpy.data.meshes.new(wid + '_glow'); g.to_mesh(me); g.free()
    ob = bpy.data.objects.new(wid + '_glow', me); bpy.context.scene.collection.objects.link(ob)
    gm = bpy.data.materials.new('Weapon_Glow'); gm.use_nodes = True
    b = gm.node_tree.nodes['Principled BSDF']
    c = B.glow_color
    b.inputs['Base Color'].default_value = (*[x * 0.3 for x in c], 1)
    b.inputs['Emission Color'].default_value = (*c, 1); b.inputs['Emission Strength'].default_value = 1.5
    b.inputs['Roughness'].default_value = 0.2
    me.materials.append(gm)
    for p in me.polygons: p.use_smooth = True
    return ob


def forge_one(path, size_px, preview_dir):
    j = json.load(open(path))
    wid = j['id']
    style = style_of(wid)
    a = archetype(wid, style)
    if a is None: return None, 0
    fn, kw = a
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    lay = Lay(j)
    B = kit.Builder(wid)
    rng = np.random.default_rng(zlib.crc32(wid.encode()))
    fn(B, lay, style, rng, **kw)
    ob = build_mesh(B, wid)
    me = ob.data
    dims = np.array(ob.dimensions[:])
    size = float(dims.max())
    # UV
    bpy.context.view_layer.objects.active = ob; ob.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(45), island_margin=0.004)
    bpy.ops.object.mode_set(mode='OBJECT')
    # cottura
    bm_mat = bake_material(size)
    me.materials.append(bm_mat)
    res = size_px if size > 0.3 else size_px // 2
    nt = bm_mat.node_tree
    tn = nt.nodes.new('ShaderNodeTexImage')
    img_c = bpy.data.images.new(wid + '_BaseColor', res, res)
    img_o = bpy.data.images.new(wid + '_ORM', res, res); img_o.colorspace_settings.name = 'Non-Color'
    img_n = bpy.data.images.new(wid + '_Normal', res, res); img_n.colorspace_settings.name = 'Non-Color'
    sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = 4; sc.render.bake.margin = 8
    for img, sw in ((img_c, 0.0), (img_o, 1.0)):
        tn.image = img; nt.nodes.active = tn
        nt.nodes['switch'].inputs['Factor'].default_value = sw
        bpy.ops.object.bake(type='EMIT'); img.pack()
    nt.links.new(nt.nodes['bsdf'].outputs[0], nt.nodes['out'].inputs['Surface'])
    tn.image = img_n; nt.nodes.active = tn
    sc.render.bake.normal_space = 'TANGENT'
    bpy.ops.object.bake(type='NORMAL'); img_n.pack()
    # materiale finale
    me.materials.clear()
    wm = bpy.data.materials.new('Weapon'); wm.use_nodes = True
    t = wm.node_tree; bs = t.nodes['Principled BSDF']
    tc_ = t.nodes.new('ShaderNodeTexImage'); tc_.image = img_c
    to_ = t.nodes.new('ShaderNodeTexImage'); to_.image = img_o
    tn_ = t.nodes.new('ShaderNodeTexImage'); tn_.image = img_n
    nm = t.nodes.new('ShaderNodeNormalMap')
    sp = t.nodes.new('ShaderNodeSeparateColor')
    t.links.new(tc_.outputs['Color'], bs.inputs['Base Color'])
    t.links.new(to_.outputs['Color'], sp.inputs[0])
    t.links.new(sp.outputs['Green'], bs.inputs['Roughness'])
    t.links.new(sp.outputs['Blue'], bs.inputs['Metallic'])
    t.links.new(tn_.outputs['Color'], nm.inputs['Color'])
    t.links.new(nm.outputs['Normal'], bs.inputs['Normal'])
    me.materials.append(wm)
    for nmn in ('base', 'rm', 'fx'):
        if nmn in me.attributes: me.attributes.remove(me.attributes[nmn])
    objs = [ob]
    g = build_glow(B, wid)
    if g: objs.append(g)
    os.makedirs(OUT, exist_ok=True)
    for o in bpy.context.view_layer.objects: o.select_set(o in objs)
    outp = os.path.join(OUT, wid + '.glb')
    bpy.ops.export_scene.gltf(filepath=outp, export_format='GLB', use_selection=True, export_yup=True, export_apply=True,
                              export_image_format='JPEG', export_jpeg_quality=90, export_tangents=True)
    if preview_dir: preview(objs, preview_dir, wid, size)
    return outp, len(me.polygons)


def preview(objs, d, wid, big):
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'; sc.cycles.samples = 24; sc.cycles.use_denoising = False
    w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
    w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.05, 0.05, 0.055, 1)
    for nm, rot, en in (('k', (55, 0, 35), 4.0), ('f', (70, 0, -140), 1.0), ('r', (100, 0, 180), 2.0)):
        l = bpy.data.objects.new(nm, bpy.data.lights.new(nm, 'SUN')); l.data.energy = en
        l.rotation_euler = [math.radians(x) for x in rot]; sc.collection.objects.link(l)
    cam = bpy.data.objects.new('c', bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera = cam
    bb = np.array([o.matrix_world @ Vector(c) for o in objs for c in o.bound_box])
    ext = np.ptp(bb, 0); elong = ext.max() / max(np.sort(ext)[1], 1e-3)
    cam.data.type = 'ORTHO'; cam.data.ortho_scale = big * (1.15 if elong > 2.5 else 1.9)
    ctr = Vector(bb.mean(0))
    cam.location = ctr + Vector((big * 2, big * 0.35, big * 0.45))
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Y').to_euler()
    sc.render.resolution_x, sc.render.resolution_y = 900, 520
    sc.view_settings.view_transform = 'AgX' if 'AgX' in [x.identifier for x in sc.view_settings.bl_rna.properties['view_transform'].enum_items] else 'Filmic'
    os.makedirs(d, exist_ok=True)
    sc.render.filepath = os.path.join(d, wid + '.png'); bpy.ops.render.render(write_still=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--preview', default='')
    ap.add_argument('--size', type=int, default=1024)
    a = ap.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:])
    only = set(filter(None, a.only.split(',')))
    for f in sorted(glob.glob(os.path.join(BLOCKS, '*.json'))):
        wid = os.path.splitext(os.path.basename(f))[0]
        if only and wid not in only: continue
        try:
            outp, nf = forge_one(f, a.size, a.preview)
            if outp is None: print(f'[armi2] {wid}: nessun archetipo, resta la v1', flush=True); continue
            print(f'[armi2] {wid}: {nf} facce -> {os.path.relpath(outp, ROOT)}', flush=True)
        except Exception as ex:
            import traceback; traceback.print_exc()
            print(f'[armi2] {wid}: ERRORE {ex}', flush=True)
