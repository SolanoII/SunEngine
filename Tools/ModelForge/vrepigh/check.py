# Anteprima del modello (Workbench): python check.py [--pose] [--tex] [--blend file.blend] out.png
import sys, bpy, math
sys.path.insert(0,'.'); import prev
bpy.ops.wm.open_mainfile(filepath=sys.argv[sys.argv.index('--blend') + 1] if '--blend' in sys.argv else 'vrepigh.blend')
arm=bpy.data.objects['Armature']; pose = '--pose' in sys.argv
if pose:
    pb=arm.pose.bones
    for s,n in ((1,'Left'),(-1,'Right')):
        b=pb['mixamorig1:'+n+'Arm']; b.rotation_mode='XYZ'
        # ruota attorno all'asse avanti del mondo
        import mathutils
        R=mathutils.Matrix.Rotation(math.radians(65*s),4,'Y')
        M=arm.matrix_world.inverted()@R@arm.matrix_world
        b.matrix = mathutils.Matrix.Translation(b.matrix.translation) @ (M.to_3x3() @ b.matrix.to_3x3()).to_4x4()
        bpy.context.view_layer.update()
    j=pb['mixamorig1:Jaw']; j.rotation_mode='XYZ'; j.rotation_euler[0]=math.radians(20)
sc=prev.setup(); sc.display.shading.color_type='TEXTURE' if '--tex' in sys.argv else 'MATERIAL'
for o in bpy.data.objects:
    o.select_set(False)
prev.shots(sys.argv[-1])
