# Ossa del corpo Vanguard (vanguard_master.fbx, rig del gioco mixamorig1) -> rig.json: posizioni di riposo in metri
# (Z in alto, -Y avanti, come Blender), gerarchia, e la mesh del corpo di prima come riferimento delle misure.
import os, sys, json, bpy, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'Sandbox/GameData/Models/Characters/vanguard_master.fbx'))
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
M = arm.matrix_world
J, T, P = {}, {}, {}
for b in arm.data.bones:
    n = b.name.split(':')[-1]
    J[n] = list(M @ b.head_local); T[n] = list(M @ b.tail_local)
    P[n] = b.parent.name.split(':')[-1] if b.parent else None
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
V = np.concatenate([np.array([list(o.matrix_world @ v.co) for v in o.data.vertices]) for o in meshes])
json.dump({'joints': J, 'tails': T, 'parent': P, 'prefix': arm.data.bones[0].name.split(':')[0] + ':',
           'bounds': [V.min(0).tolist(), V.max(0).tolist()], 'meshes': [o.name for o in meshes]}, open(os.path.join(HERE, 'rig.json'), 'w'), indent=0)
np.save(os.path.join(HERE, 'old_body.npy'), V[::7])
print(len(J), 'ossa', V.min(0), V.max(0), [o.name for o in meshes])
