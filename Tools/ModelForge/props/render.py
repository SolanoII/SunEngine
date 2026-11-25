# Anteprima dei prop: DISPLAY=:99 python3 render.py <out.png> [--cycles] [id,id,...]
# Li mette in fila su un terreno di fango, luce di sole basso e cielo di smog, camera di tre quarti.
import sys, os, math
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
PROPS = os.path.join(ROOT, 'Sandbox/GameData/Models/Props')
ARGV = sys.argv[1:]
OUT = os.path.abspath(ARGV[0])
CYCLES = '--cycles' in ARGV
ids = next((a.split(',') for a in ARGV[1:] if not a.startswith('--')), None) or \
    ['sandbag_wall', 'trench_berm', 'barbed_wire', 'czech_hedgehog', 'grave_cross', 'dead_tree']

import tempfile
from PIL import Image
tiles = []
for pid in ids:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    bpy.ops.import_scene.gltf(filepath=os.path.join(PROPS, pid + '.glb'))
    ms = [o for o in bpy.data.objects if o.type == 'MESH']
    import mathutils
    lo = mathutils.Vector((1e9, 1e9, 1e9)); hi = -lo
    for o in ms:
        for c in o.bound_box:
            w_ = o.matrix_world @ mathutils.Vector(c); lo = mathutils.Vector(map(min, lo, w_)); hi = mathutils.Vector(map(max, hi, w_))
    ctr = (lo + hi) / 2; size = max((hi - lo).length, 0.5)
    bpy.ops.mesh.primitive_plane_add(size=60, location=(ctr.x, ctr.y, lo.z))
    g = bpy.context.active_object
    m = bpy.data.materials.new('Mud'); m.use_nodes = True
    bs = m.node_tree.nodes['Principled BSDF']; bs.inputs['Base Color'].default_value = (0.05, 0.04, 0.03, 1); bs.inputs['Roughness'].default_value = 0.85
    g.data.materials.append(m)
    sun = bpy.data.objects.new('Sun', bpy.data.lights.new('Sun', 'SUN')); sc.collection.objects.link(sun)
    sun.data.energy = 3.5; sun.data.color = (1.0, 0.82, 0.62); sun.rotation_euler = (math.radians(58), 0, math.radians(-35))
    w = bpy.data.worlds.new('W'); sc.world = w; w.use_nodes = True
    w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.32, 0.27, 0.22, 1); w.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.7
    cam = bpy.data.objects.new('Cam', bpy.data.cameras.new('Cam')); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.lens = 40
    dirv = mathutils.Vector((0.55, -1.0, 0.42)).normalized()
    cam.location = ctr + dirv * size * 1.55
    d = ctr - cam.location
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    sc.render.engine = 'CYCLES' if CYCLES else 'BLENDER_EEVEE'
    if CYCLES: sc.cycles.samples = 32; sc.cycles.device = 'CPU'; sc.cycles.use_denoising = True
    sc.render.resolution_x, sc.render.resolution_y = 600, 600
    sc.view_settings.view_transform = 'AgX'; sc.view_settings.look = 'AgX - Medium High Contrast'
    f = os.path.join(tempfile.gettempdir(), pid + '_prev.png'); sc.render.filepath = f
    bpy.ops.render.render(write_still=True); tiles.append(f)
cols = min(3, len(tiles)); rows = (len(tiles) + cols - 1) // cols
sheet = Image.new('RGB', (600 * cols, 600 * rows))
for i, f in enumerate(tiles): sheet.paste(Image.open(f).convert('RGB'), ((i % cols) * 600, (i // cols) * 600))
sheet.save(OUT)
print('anteprima', OUT)
