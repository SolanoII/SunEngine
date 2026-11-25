# Anteprima del modello: python render.py <file.blend> <out.png> [--cycles]
# Cinque viste: davanti, tre quarti, dietro, primo piano dell'elmo, primo piano del petto.
import sys, math, bpy, numpy as np
a = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
bpy.ops.wm.open_mainfile(filepath=a[0])
sc = bpy.context.scene
if '--cycles' in a:
    sc.render.engine = 'CYCLES'; sc.cycles.samples = 24; sc.cycles.device = 'CPU'; sc.cycles.use_denoising = True
else:
    sc.render.engine = 'BLENDER_EEVEE'
w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
bg = w.node_tree.nodes['Background']; bg.inputs['Color'].default_value = (0.30, 0.31, 0.34, 1); bg.inputs['Strength'].default_value = 0.8
for nm, rot, en in (('k', (55, 0, 35), 3.5), ('f', (65, 0, -130), 1.2), ('r', (110, 0, 180), 2.0)):
    l = bpy.data.objects.new(nm, bpy.data.lights.new(nm, 'SUN')); l.data.energy = en; l.rotation_euler = [math.radians(x) for x in rot]; sc.collection.objects.link(l)
cam = bpy.data.objects.new('c', bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera = cam
sc.view_settings.view_transform = 'AgX' if 'AgX' in [v.identifier for v in type(sc.view_settings).bl_rna.properties['view_transform'].enum_items] else 'Filmic'
from PIL import Image
tiles = []
sc.render.resolution_x, sc.render.resolution_y = 480, 760
views = ((0, 2.15, 1.1, 50), (35, 2.15, 1.1, 50), (165, 2.15, 1.1, 50), (25, 0.55, 2.0, 50), (15, 0.75, 1.62, 50))
for i, (ang, h, lz, lens) in enumerate(views):
    ar = math.radians(ang); look = np.array([0, 0, lz])
    dist = 3.6 * h / 2.15; loc = look + np.array([math.sin(ar) * dist, -math.cos(ar) * dist, 0.08 * h])
    cam.location = loc; dd = look - loc; cam.data.lens = lens
    cam.rotation_euler = (math.pi / 2 + math.atan2(dd[2], math.hypot(dd[0], dd[1])), 0, math.atan2(-dd[0], dd[1]))
    f = f'/tmp/_vg{i}.png'; sc.render.filepath = f; bpy.ops.render.render(write_still=True); tiles.append(Image.open(f).convert('RGB'))
im = Image.new('RGB', (sum(t.width for t in tiles), 760)); x = 0
for t in tiles: im.paste(t, (x, 0)); x += t.width
im.save(a[1])
print('anteprima', a[1])
