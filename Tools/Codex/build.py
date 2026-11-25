# Codex: rigenera le foto (GameData/Codex/img) e la pagina web del Codex dai dati del gioco.
#
#   python Tools/Codex/build.py                        # foto delle armi cambiate + pagina in Tools/Codex/out
#   python Tools/Codex/build.py --shots <cartella>     # aggiunge le foto delle unita' fatte dal gioco (SUN_CODEX_SHOTS)
#   python Tools/Codex/build.py --out <cartella>
#
# La scheda CODEX nel gioco legge gli stessi file (GameData/Codex/labels.json per le etichette, img/ per le foto),
# quindi gioco e pagina restano allineati. Le foto delle armi si rifanno solo quando il .glb e' piu' nuovo della foto.
import os, sys, json, glob, shutil, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
GD = os.path.join(ROOT, 'Sandbox', 'GameData')
IMG = os.path.join(GD, 'Codex', 'img')
W, H = 720, 416


def load(p):
    with open(p, encoding='utf-8') as f: return json.load(f)


def photograph_weapons(wids):
    if not wids: return
    sys.path.insert(0, os.path.join(ROOT, 'Tools', 'WeaponForge'))
    import bpy
    import forge2
    from PIL import Image
    tmp = os.path.join(IMG, '_tmp'); os.makedirs(tmp, exist_ok=True)
    for wid in wids:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=os.path.join(GD, 'Models', 'Weapons', wid + '.glb'))
        objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
        if not objs: continue
        forge2.preview(objs, tmp, wid, max(max(o.dimensions) for o in objs))
        Image.open(os.path.join(tmp, wid + '.png')).convert('RGB').resize((W, H)).save(os.path.join(IMG, wid + '.jpg'), quality=82)
        print('[codex] foto arma', wid, flush=True)
    shutil.rmtree(tmp, ignore_errors=True)


def unit_shots(folder):
    """foto del gioco (ppm a schermo intero) -> ritaglio centrale 720x416"""
    from PIL import Image
    n = 0
    for p in glob.glob(os.path.join(folder, '*.ppm')):
        im = Image.open(p).convert('RGB')
        w, h = im.size
        ch = int(h * 0.8); cw = int(ch * W / H)
        if cw > w: cw = w; ch = int(cw * H / W)
        x0 = (w - cw) // 2; y0 = int((h - ch) * 0.55)
        im.crop((x0, y0, x0 + cw, y0 + ch)).resize((W, H), Image.LANCZOS).save(os.path.join(IMG, os.path.basename(p)[:-4] + '.jpg'), quality=84)
        n += 1
    print(f'[codex] {n} foto di unita\' da {folder}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(HERE, 'out'))
    ap.add_argument('--shots', default='')
    ap.add_argument('--no-photos', action='store_true')
    a = ap.parse_args()
    os.makedirs(IMG, exist_ok=True)
    if a.shots: unit_shots(a.shots)

    labels = load(os.path.join(GD, 'Codex', 'labels.json'))
    # rarita' e tratti: copiati da Sandbox/ItemRarity.h, cosi' gioco e pagina dicono la stessa cosa
    import re
    h = open(os.path.join(ROOT, 'Sandbox', 'ItemRarity.h'), encoding='utf-8').read()
    lv = [{'name': n, 'color': [float(r), float(g), float(b)], 'stat': float(m), 'traits': int(t)}
          for n, r, g, b, m, t in re.findall(r'\{ "([^"]+)",\s*ImVec4\(([\d.]+)f, ([\d.]+)f, ([\d.]+)f, 1\.0f\), ([\d.]+)f, (\d) \}', h)]
    tr = [{'code': c, 'name': n, 'effect': e, 'kind': k} for c, n, e, k in re.findall(r'\{ "(\w+)",\s*"([^"]+)",\s*"([^"]+)",\s*ItemKind::(\w+) \}', h)]
    if len(lv) == 5 and tr:
        labels.setdefault('rarity', {})
        labels['rarity'].update({'levels': lv, 'traits': tr})
        with open(os.path.join(GD, 'Codex', 'labels.json'), 'w', encoding='utf-8') as f: json.dump(labels, f, ensure_ascii=False, indent=1)
    units = {}
    for f in glob.glob(os.path.join(GD, 'Units', '*.json')):
        j = load(f); units[j['id']] = j
    tree = load(os.path.join(GD, 'Research', 'research_tree.json'))
    nodes = {n['id']: dict(n, _faction=fa['id']) for fa in tree['factions'] for n in fa['nodes']}
    by_unit = {n['unit']: n for n in nodes.values() if n.get('unit')}
    by_item = {}
    for n in nodes.values():
        for it in ([n['item']] if isinstance(n.get('item'), str) else (n.get('item') or [])) + list(n.get('items') or []):
            by_item.setdefault(it, n)

    def research(n):
        if not n: return None
        p = nodes.get(n.get('parent') or '')
        return {'tier': n.get('tier', 1), 'research': n.get('research_cost', 0), 'purchase': n.get('purchase_cost', 0),
                'premium': bool(n.get('premium')), 'premium_cost': n.get('premium_cost', 0), 'from': p.get('name') if p else None}

    def default_loadout(u):
        dl = u.get('default_loadout') or {}
        if isinstance(dl, str):
            import ast
            try: dl = ast.literal_eval(dl)
            except Exception: dl = {}
        return dl if isinstance(dl, dict) else {}

    def can_weapon(w, uid):
        al = w.get('allowed_units') or []
        if units[uid].get('strict_loadout'): return uid in al
        return not al or uid in al

    def can_armor(a, uid):
        al = a.get('allowed_units') or []
        if units[uid].get('strict_loadout'): return uid in al
        if al: return uid in al
        r = a.get('unit_restriction') or ''
        return not r or r == uid

    loads = {}
    for uid, u in units.items():
        dl = default_loadout(u); ids = set()
        for v in dl.values():
            if isinstance(v, str): ids.add(v)
            elif isinstance(v, list): ids.update(x for x in v if isinstance(x, str))
            elif isinstance(v, dict): ids.update(x for x in v.values() if isinstance(x, str))
        loads[uid] = ids

    entries, todo = [], []
    CAT = {'Ranged1h': 'ranged', 'Ranged2h': 'ranged', 'Melee1h': 'melee', 'Melee2h': 'melee', 'Equipment': 'equipment'}
    for f in sorted(glob.glob(os.path.join(GD, 'Weapons', '*', '*.json'))):
        j = load(f); wid = j['id']; folder = os.path.basename(os.path.dirname(f))
        glb = os.path.join(GD, 'Models', 'Weapons', wid + '.glb'); jpg = os.path.join(IMG, wid + '.jpg')
        if os.path.exists(glb) and (not os.path.exists(jpg) or os.path.getmtime(jpg) < os.path.getmtime(glb)): todo.append(wid)
        who = [{'id': u, 'name': units[u].get('displayName', u), 'base': wid in loads[u]} for u in sorted(units) if can_weapon(j, u) or wid in loads[u]]
        e = {k: v for k, v in j.items() if k not in ('primary_color', 'core_color', 'weapon_scale', 'hold_offset', 'barrel_offset', 'skin_parts', 'garments')}
        n = (j.get('moveset') or {}).get('nodes') or {}
        if n:
            e['_combo'] = len(n); e['_maxmul'] = max(x.get('damage_multiplier', 1) for x in n.values())
        e.pop('moveset', None)
        e.update(_kind='weapon', _cat=CAT.get(folder, 'ranged'), _folder=folder, _who=who, _model=os.path.exists(glb), _research=research(by_item.get(wid)))
        entries.append(e)
    for f in sorted(glob.glob(os.path.join(GD, 'Armors', '**', '*.json'), recursive=True)):
        j = load(f); aid = j['id']
        who = [{'id': u, 'name': units[u].get('displayName', u), 'base': aid in loads[u]} for u in sorted(units) if can_armor(j, u) or aid in loads[u]]
        e = {k: v for k, v in j.items() if k not in ('skin_parts', 'skin_blocks', 'garments')}
        e.update(_kind='armor', _cat='armor', _who=who, _research=research(by_item.get(aid)))
        entries.append(e)
    wname = {e['id']: e.get('displayName', e['id']) for e in entries}
    items = list(entries)
    for uid, u in sorted(units.items()):
        dl = default_loadout(u)
        rn = by_unit.get(uid)
        base = []
        for slot, lab in (('primary', 'Primaria'), ('secondary', 'Secondaria'), ('melee', 'Mischia')):
            if dl.get(slot): base.append([lab, wname.get(dl[slot], dl[slot])])
        for x in dl.get('equipment') or []:
            if x: base.append(['Equipaggiamento', wname.get(x, x)])
        arm = dl.get('armor') or {}
        PART = {'head': 'Testa', 'torso': 'Torso', 'arm_l': 'Braccio sinistro', 'arm_r': 'Braccio destro', 'leg_l': 'Gamba sinistra', 'leg_r': 'Gamba destra'}
        if isinstance(arm, dict):
            for k, lab in PART.items():
                if arm.get(k): base.append([lab, wname.get(arm[k], arm[k])])
        uses = {'ranged': [], 'melee': [], 'equipment': [], 'armor': []}
        for e in items:
            if any(w['id'] == uid for w in e['_who']): uses[e['_cat']].append(e.get('displayName', e['id']))
        for v in uses.values(): v.sort()
        e = {k: v for k, v in u.items() if k not in ('default_loadout', 'base_skin', 'skeleton')}
        fac = 'Kingship' if uid.startswith('kingship_') else 'Vanguard' if uid.startswith('vanguard_') else 'Vrepighs' if uid.startswith('vrepighs_') else ''
        e.update(_kind='unit', _cat='unit' if (rn and not rn.get('premium')) else 'other', faction=fac, _base=base, _uses=uses, _research=research(rn))
        entries.append(e)

    # idee: unita' dell'albero che non esistono ancora (anche oltre il grado mostrato) e gerarchie delle fazioni
    hier = []
    maxt = tree.get('max_tier', 0)
    FAC = {'kingship': 'Kingship', 'vanguard': 'Vanguard', 'vrepighs': 'Vrepighs'}
    for fa in tree['factions']:
        names = {n['id']: n.get('name', '') for n in fa['nodes']}
        for n in fa['nodes']:
            uid = n.get('unit')
            if not uid or uid in units: continue
            entries.append({'id': uid, 'displayName': n.get('name', uid), 'faction': FAC.get(fa['id'], fa['id']), 'comment': n.get('description', ''),
                            'tier': n.get('tier', 1), 'caste': n.get('caste', ''), 'default_name': n.get('default_name', ''),
                            '_kind': 'idea', '_cat': 'idea', '_parent': names.get(n.get('parent') or '', ''), '_hidden': bool(maxt and n.get('tier', 1) > maxt),
                            '_research': {'tier': n.get('tier', 1), 'research': n.get('research_cost', 0), 'purchase': 0, 'premium': bool(n.get('premium')), 'premium_cost': 0, 'from': names.get(n.get('parent') or '')}})
        for sct in fa.get('sections', []):
            hier.append(dict(sct, faction=FAC.get(fa['id'], fa['id'])))

    if not a.no_photos: photograph_weapons(todo)
    for e in entries:
        e['_img'] = ('img/' + e['id'] + '.jpg') if os.path.exists(os.path.join(IMG, e['id'] + '.jpg')) else None

    shutil.rmtree(os.path.join(a.out, 'img'), ignore_errors=True)   # niente foto vecchie di cose cancellate
    os.makedirs(os.path.join(a.out, 'img'), exist_ok=True)
    for p in glob.glob(os.path.join(IMG, '*.jpg')): shutil.copy2(p, os.path.join(a.out, 'img', os.path.basename(p)))
    with open(os.path.join(a.out, 'data.json'), 'w', encoding='utf-8') as f:
        import datetime
        json.dump({'updated': datetime.date.today().strftime('%d/%m/%Y'), 'labels': labels, 'entries': entries, 'hierarchies': hier}, f, ensure_ascii=False)
    shutil.copy(os.path.join(HERE, 'codex.html'), os.path.join(a.out, 'codex.html'))
    print(f'[codex] {len(entries)} schede, {sum(1 for e in entries if e["_img"])} con foto, {len(todo)} foto di armi rifatte -> {a.out}')


if __name__ == '__main__':
    main()
