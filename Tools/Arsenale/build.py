# Rigenera la pagina "Arsenale" (catalogo di armi, attrezzi e unita') da Sandbox/GameData.
#
#   python Tools/Arsenale/build.py [cartella_di_uscita]          (predefinita: Tools/Arsenale/out)
#
# Per ogni arma legge le statistiche (Weapons/*/*.json), le unita' che possono portarla (allowed_units,
# unit_restriction e dotazione base delle Units) e fotografa il modello Models/Weapons/<id>.glb con Blender.
# Le foto si rifanno solo quando il .glb e' piu' nuovo della foto. In uscita: arsenale.html, data.json, img/.
import os, sys, json, glob, ast, shutil, math

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
GD = os.path.join(ROOT, 'Sandbox', 'GameData')
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, 'out')
CAT = {'Ranged2h': 'Armi pesanti e fucili', 'Ranged1h': 'Pistole', 'Melee1h': 'Armi bianche a una mano',
       'Melee2h': 'Armi bianche a due mani', 'Equipment': 'Equipaggiamento'}


def photograph(wids):
    """foto dei modelli (stessa inquadratura delle anteprime della forgia delle armi)"""
    if not wids: return
    sys.path.insert(0, os.path.join(ROOT, 'Tools', 'WeaponForge'))
    import bpy
    import forge2
    from PIL import Image
    tmp = os.path.join(OUT, '_png'); os.makedirs(tmp, exist_ok=True)
    for wid in wids:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=os.path.join(GD, 'Models', 'Weapons', wid + '.glb'))
        objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
        if not objs: continue
        forge2.preview(objs, tmp, wid, max(max(o.dimensions) for o in objs))
        Image.open(os.path.join(tmp, wid + '.png')).convert('RGB').resize((720, 416)).save(os.path.join(OUT, 'img', wid + '.jpg'), quality=82)
        print('[arsenale] foto', wid, flush=True)
    shutil.rmtree(tmp, ignore_errors=True)


def main():
    os.makedirs(os.path.join(OUT, 'img'), exist_ok=True)
    units, loads = {}, {}
    for f in glob.glob(os.path.join(GD, 'Units', '*.json')):
        j = json.load(open(f, encoding='utf-8')); units[j['id']] = j.get('displayName', j['id'])
        dl = j.get('default_loadout')
        if isinstance(dl, str):
            try: dl = ast.literal_eval(dl)
            except Exception: dl = None
        ids = set()
        if isinstance(dl, dict):
            for v in dl.values():
                if isinstance(v, str): ids.add(v)
                elif isinstance(v, list): ids.update(x for x in v if isinstance(x, str))
        loads[j['id']] = ids
    units.setdefault('story_konrad', 'Konrad (storia)')
    weapons, todo = [], []
    for f in sorted(glob.glob(os.path.join(GD, 'Weapons', '*', '*.json'))):
        j = json.load(open(f, encoding='utf-8')); wid = j['id']; cat = os.path.basename(os.path.dirname(f))
        us = list(j.get('allowed_units') or [])
        if j.get('unit_restriction'): us.append(j['unit_restriction'])
        us = [u for u in us if u != '__nessuno__']
        for u, ids in loads.items():
            if wid in ids and u not in us: us.append(u)
        glb = os.path.join(GD, 'Models', 'Weapons', wid + '.glb')
        jpg = os.path.join(OUT, 'img', wid + '.jpg')
        if os.path.exists(glb) and (not os.path.exists(jpg) or os.path.getmtime(jpg) < os.path.getmtime(glb)): todo.append(wid)
        d = {k: v for k, v in j.items() if k not in ('moveset', 'primary_color', 'core_color', 'weapon_scale', 'hold_offset', 'barrel_offset', 'skin_parts', 'default_loadout')}
        n = (j.get('moveset') or {}).get('nodes') or {}
        if n:
            d['_combo'] = len(n); d['_maxmul'] = max(x.get('damage_multiplier', 1) for x in n.values())
            d['_stam'] = [min(x.get('stamina_cost', 0) for x in n.values()), max(x.get('stamina_cost', 0) for x in n.values())]
        d.update(_cat=cat, _catname=CAT.get(cat, cat), _who=[{'id': u, 'name': units.get(u, u), 'base': wid in loads.get(u, set())} for u in us],
                 _img=('img/' + wid + '.jpg') if os.path.exists(glb) else None)
        weapons.append(d)
    photograph(todo)
    for d in weapons:
        if d['_img'] and not os.path.exists(os.path.join(OUT, d['_img'])): d['_img'] = None
    json.dump(weapons, open(os.path.join(OUT, 'data.json'), 'w', encoding='utf-8'), ensure_ascii=False)
    shutil.copy(os.path.join(HERE, 'arsenale.html'), os.path.join(OUT, 'arsenale.html'))
    print(f'[arsenale] {len(weapons)} voci, {sum(1 for d in weapons if d["_img"])} con foto, {len(todo)} foto rifatte -> {OUT}')


if __name__ == '__main__':
    main()
