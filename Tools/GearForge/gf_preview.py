# Anteprime veloci (Workbench) dei pezzi sul corpo: davanti, dietro e testa, affiancate in un PNG.
import bpy, math, os
import numpy as np


def _cam(sc, loc, look, lens):
    cam = sc.camera
    if cam is None:
        cam = bpy.data.objects.new('gf_cam', bpy.data.cameras.new('gf_cam'))
        sc.collection.objects.link(cam); sc.camera = cam
    cam.data.lens = lens
    cam.location = loc
    d = np.array(look) - np.array(loc)
    yaw = math.atan2(d[0], -d[1]) if False else math.atan2(-d[0], d[1])
    pitch = math.atan2(d[2], math.hypot(d[0], d[1]))
    cam.rotation_euler = (math.pi / 2 + pitch, 0, yaw)


def render_views(path, body, views=('front', 'back', 'head'), size=(520, 820)):
    from PIL import Image
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sh = sc.display.shading
    sh.light = 'STUDIO'; sh.color_type = 'OBJECT'
    sh.show_cavity = True; sh.cavity_type = 'BOTH'; sh.show_shadows = True
    sc.render.film_transparent = False
    sh.background_type = 'VIEWPORT' if hasattr(sh, 'background_type') else None
    H = body.H; z0 = body.z0
    mid = z0 + H * 0.5
    tiles = []
    for v in views:
        if v == 'head':
            hp = body.head('Head') + np.array([0, 0, 0.06 * H / 1.75])
            loc = hp + np.array([0.55, -0.95, 0.08]) * H / 1.75 * 1.1
            _cam(sc, loc, hp, 85); res = (size[0], size[0])
        elif v in ('torso', 'legs', 'torso_back'):
            zc = z0 + H * (0.66 if v.startswith('torso') else 0.2)
            s = 1 if v == 'torso_back' else -1
            look = np.array([0, 0, zc])
            loc = look + np.array([0.35 * H * (-s), s * 1.1 * H, 0.05 * H])
            _cam(sc, loc, look, 60); res = (size[0], size[0])
        elif v in ('foot', 'foot_side'):
            fp = body.head('LeftFoot') if body.has('LeftFoot') else np.array([0.1, 0, z0 + 0.05 * H])
            look = np.array([fp[0], fp[1] - 0.04, z0 + 0.06 * H])
            off = np.array([0.55, -0.35, 0.18]) if v == 'foot' else np.array([0.65, 0.05, 0.08])
            _cam(sc, look + off * H / 1.75, look, 70); res = (size[0], size[0])
        else:
            s = -1 if v == 'front' else 1
            loc = np.array([0.9 * H * (0.55 if v == 'front' else -0.55), s * 2.3 * H, mid + 0.08 * H])
            _cam(sc, loc, [0, 0, mid], 50); res = size
        sc.render.resolution_x, sc.render.resolution_y = res
        tmp = path + f'.{v}.png'
        sc.render.filepath = tmp
        bpy.ops.render.render(write_still=True)
        tiles.append(Image.open(tmp).convert('RGB'))
        os.remove(tmp)
    W = sum(t.width for t in tiles); Hh = max(t.height for t in tiles)
    out = Image.new('RGB', (W, Hh), (40, 40, 40)); x = 0
    for t in tiles:
        out.paste(t, (x, 0)); x += t.width
    out.save(path)
    return path
