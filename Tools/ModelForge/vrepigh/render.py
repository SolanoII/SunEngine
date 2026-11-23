# Render a colori (EEVEE) di un .blend del modello: python render.py file.blend out.png <altezza> <testa y> <testa z>
import sys, bpy, math, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import prev
a = sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else sys.argv[1:]
bpy.ops.wm.open_mainfile(filepath=a[0])
sc = bpy.context.scene
try:
    sc.render.engine = 'BLENDER_EEVEE_NEXT'
except Exception:
    sc.render.engine = 'BLENDER_EEVEE'
w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.35, 0.35, 0.38, 1); w.node_tree.nodes['Background'].inputs['Strength'].default_value = 1.0
for nm, rot, en in (('k', (50, 0, 30), 4.0), ('f', (60, 0, -120), 1.5)):
    l = bpy.data.objects.new(nm, bpy.data.lights.new(nm, 'SUN')); l.data.energy = en; l.rotation_euler = [math.radians(x) for x in rot]; sc.collection.objects.link(l)
cam = bpy.data.objects.new('c', bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera = cam
H = float(a[2]); hy, hz = float(a[3]), float(a[4])
from PIL import Image
tiles = []
sc.render.resolution_x, sc.render.resolution_y = 500, 760
for i, (ang, zoom) in enumerate(((0, 1), (40, 1), (160, 1), (30, 0.42), (90, 0.42))):
    ar = math.radians(ang); look = np.array([0, hy if zoom < 1 else 0, hz if zoom < 1 else H * 0.5])
    dist = 3.3 * H * zoom / 1.55; loc = look + np.array([math.sin(ar) * dist, -math.cos(ar) * dist, 0.05 * H])
    cam.location = loc; dd = look - loc; cam.data.lens = 50
    cam.rotation_euler = (math.pi / 2 + math.atan2(dd[2], math.hypot(dd[0], dd[1])), 0, math.atan2(-dd[0], dd[1]))
    f = f'/tmp/_r{i}.png'; sc.render.filepath = f; bpy.ops.render.render(write_still=True); tiles.append(Image.open(f).convert('RGB'))
im = Image.new('RGB', (sum(t.width for t in tiles), 760)); x = 0
for t in tiles: im.paste(t, (x, 0)); x += t.width
im.save(a[1])
