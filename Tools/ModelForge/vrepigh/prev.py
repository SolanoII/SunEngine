# Rendering veloce (Workbench) di una mesh o della scena aperta: usato da check.py
import sys, bpy, math, numpy as np
from PIL import Image
def setup():
    sc=bpy.context.scene; sc.render.engine='BLENDER_WORKBENCH'
    sh=sc.display.shading; sh.light='STUDIO'; sh.show_cavity=True; sh.show_shadows=True
    sh.color_type='OBJECT'
    if sc.camera is None:
        cam=bpy.data.objects.new('c',bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera=cam
    return sc
def shots(out, H=1.6, views=((0,1),(50,1),(90,1),(180,1),(35,0.42),(90,0.42),(150,0.42)), res=(500,760), head=(-0.28, 1.42)):
    sc=setup(); cam=sc.camera; tiles=[]
    sc.render.resolution_x, sc.render.resolution_y = res
    for i,(ang,zoom) in enumerate(views):
        a=math.radians(ang); look=np.array([0,head[0] if zoom<1 else 0,(head[1] if zoom<1 else H*0.5)])
        dist=3.3*H*zoom/1.55; loc=look+np.array([math.sin(a)*dist,-math.cos(a)*dist,0.05 if zoom==1 else 0.1])
        cam.location=loc; dd=look-loc; cam.data.lens=50
        cam.rotation_euler=(math.pi/2+math.atan2(dd[2],math.hypot(dd[0],dd[1])),0,math.atan2(-dd[0],dd[1]))
        import tempfile, os; f=os.path.join(tempfile.gettempdir(), f'_p{i}.png'); sc.render.filepath=f; bpy.ops.render.render(write_still=True); tiles.append(Image.open(f).convert('RGB'))
    im=Image.new('RGB',(sum(t.width for t in tiles),res[1])); x=0
    for t in tiles: im.paste(t,(x,0)); x+=t.width
    im.save(out)
if __name__=='__main__':
    d=np.load(sys.argv[1]); bpy.ops.wm.read_factory_settings(use_empty=True)
    me=bpy.data.meshes.new('b'); me.from_pydata(d['v'].tolist(),[],d['f'].tolist()); me.update()
    for p in me.polygons: p.use_smooth=True
    ob=bpy.data.objects.new('b',me); ob.color=(0.55,0.45,0.4,1); bpy.context.scene.collection.objects.link(ob)
    a = sys.argv[3:]
    shots(sys.argv[2], H=float(a[0]) if a else 1.6, head=(float(a[1]), float(a[2])) if len(a) > 2 else (-0.28, 1.42))
