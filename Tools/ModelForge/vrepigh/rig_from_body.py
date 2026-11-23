# Ossa del Vrepigh (posizioni di riposo del corpo procedurale di prima, gerarchia dal vrepigh_master.fbx) -> rig.json.
# Serve solo per rifare rig.json: uso  python rig_from_body.py <vrepigh.body del corpo procedurale>
import os, sys, bpy, json, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '../../..'))
sys.path.insert(0, os.path.join(ROOT, 'Tools/GearForge')); import gf_io
d=gf_io.read_body(sys.argv[1])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'Sandbox/GameData/Models/Characters/vrepigh_master.fbx'))
arm=[o for o in bpy.data.objects if o.type=='ARMATURE'][0]
par={b.name:(b.parent.name if b.parent else None) for b in arm.data.bones}
J={}
for n,o in zip(d['names'],d['offsets']):
    M=np.array(o)
    G=np.linalg.inv(M)
    p=G[:3,3]*0.01; J[n.split(':')[-1]]=[float(p[0]),float(-p[2]),float(p[1])]
    J[n.split(':')[-1]+'_R']=G[:3,:3].tolist()
json.dump({'joints':{k:v for k,v in J.items() if not k.endswith('_R')},'rot':{k[:-2]:v for k,v in J.items() if k.endswith('_R')},'parent':{k.split(':')[-1]:(v.split(':')[-1] if v else None) for k,v in par.items()},'meshes':[o.name for o in bpy.data.objects if o.type=='MESH']},open(os.path.join(HERE,'rig.json'),'w'),indent=0)
print(arm.name, len(par), [o.name for o in bpy.data.objects])
