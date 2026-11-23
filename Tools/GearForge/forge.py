# Forgia delle vesti e delle armature di Project SunCollector.
#
# Costruisce in Blender ogni strato ("garments") delle armature e delle unita' sul corpo vero del gioco e lo scrive in
# GameData/Gear/<corpo>/ (file .gear + index.json). Il gioco li usa al posto dei gusci generati (GearMesh.h).
#
# Uso (serve il modulo bpy: pip install bpy, stessa versione di Python che chiede il pacchetto):
#   1) esporta i corpi dal gioco:   SUN_GEAR_DUMP=<cartella>  ProjectSunCollector   (basta arrivare al menu)
#   2) python forge.py --bodies <cartella> --gamedata <repo>/Sandbox/GameData [--only footman] [--ids a,b] [--preview <dir>]
# Vedi README.md per i dettagli.
import argparse, glob, hashlib, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy
import numpy as np

# Corpo di ogni gruppo di armature (cartelle di GameData/Armors) e delle unita' (animated_body nel JSON)
BODY_OF_DIR = {
    'Kingship': ['footman'],
    'Vanguard/explorer': ['candidate'], 'Vanguard/candidate': ['candidate'],
    'Vrepighs/pighcib': ['pighcib'],
    'Vrepighs': ['vrepigh'],
}
BODY_OF_UNIT = {'human': 'footman', 'candidate': 'candidate', 'vrepigh': 'vrepigh', 'pighcib': 'pighcib'}
# Varianti di un corpo per unita' (Models/Characters/<corpo>/variants/<id unita'>.glb): il gioco le esporta come
# <id unita'>.body e ricevono gli stessi strati del corpo da cui nascono
VARIANT_PREFIX = {'vrepigh': 'vrepighs_'}
# triangoli massimi per tipo di pezzo (un soldato completo resta sui 40-50 mila)
BUDGET = {'shirt': 9000, 'pants': 6000, 'rags': 6000, 'boot': 3500, 'helmet': 5000, 'band': 1500, 'bracer': 2200,
          'arm_plate': 4500, 'leg_plate': 4000, 'flak': 10000, 'chest_plate': 6000, 'heavy_torso': 9000, 'belt': 2500}
SHAPE_KEYS = ('bones', 'min_weight', 'height_range', 'side', 'max_abs_x', 'min_abs_x', 'facing', 'facing_min', 'inflate',
              'noise', 'noise_scale', 'plate_cells', 'plate_step', 'alpha_cut')


def kind_of(g):
    i = g['id']
    tex = os.path.basename(g.get('texture', ''))
    if 'rags' in i: return 'rags'
    if 'shirt' in i: return 'shirt'
    if 'pants' in i or 'trousers' in i: return 'pants'
    if 'boot' in i: return 'boot'
    if 'helmet_band' in i: return 'band'
    if 'helmet' in i or i.endswith('helm'): return 'helmet'
    if 'bracer' in i: return 'bracer'
    if 'arm_guard' in i or 'plate_arm' in i or 'scrap_arm' in i: return 'arm_plate'
    if 'leg_guard' in i or 'plate_leg' in i or 'scrap_leg' in i: return 'leg_plate'
    if 'flak' in i: return 'flak'
    if 'heavy_torso' in i: return 'heavy_torso'
    if 'chest' in i: return 'chest_plate'
    if 'belt' in i: return 'belt'
    return None


def style_of(g):
    t = os.path.basename(g.get('texture', ''))
    if 'scrap' in t: return 'scrap'
    if 'vrepigh_plate' in t: return 'vrepigh'
    if 'flak' in t: return 'flak'
    return 'cloth'


def collect(gamedata):
    """(corpo, strato, file di origine) per tutti gli strati da costruire"""
    out = []
    for f in sorted(glob.glob(os.path.join(gamedata, 'Armors', '*', '*', '*.json'))):
        rel = os.path.relpath(f, os.path.join(gamedata, 'Armors')).replace('\\', '/')
        bodies = None
        for k in sorted(BODY_OF_DIR, key=len, reverse=True):
            if rel.startswith(k + '/'): bodies = BODY_OF_DIR[k]; break
        if not bodies: continue
        j = json.load(open(f, encoding='utf-8'))
        for g in j.get('garments', []):
            if g.get('procedural') or g.get('model') or not kind_of(g): continue
            for b in bodies: out.append((b, g, rel))
    for f in sorted(glob.glob(os.path.join(gamedata, 'Units', '*.json'))):
        j = json.load(open(f, encoding='utf-8'))
        b = BODY_OF_UNIT.get(j.get('animated_body', ''))
        if not b: continue
        for g in j.get('base_skin', {}).get('garments', []):
            if g.get('procedural') or g.get('model') or not kind_of(g): continue
            out.append((b, g, os.path.basename(f)))
    return out


def shape_hash(body, g):
    k = {key: g.get(key) for key in SHAPE_KEYS if key in g}
    k['kind'] = kind_of(g); k['style'] = style_of(g); k['body'] = body
    return hashlib.sha1(json.dumps(k, sort_keys=True).encode()).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bodies', required=True)
    ap.add_argument('--gamedata', required=True)
    ap.add_argument('--out', default=None, help='cartella di uscita (predefinita: <gamedata>/Gear)')
    ap.add_argument('--only', default='', help='corpi da fare, separati da virgola (anche le varianti: vrepighs_kragh...)')
    ap.add_argument('--ids', default='', help='solo questi strati (id separati da virgola)')
    ap.add_argument('--kinds', default='', help='solo questi tipi (shirt,pants,boot,helmet,...)')
    ap.add_argument('--preview', default='', help='cartella per le anteprime')
    ap.add_argument('--no-ao', action='store_true')
    a = ap.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:])
    out_root = a.out or os.path.join(a.gamedata, 'Gear')
    only = set(filter(None, a.only.split(','))); ids = set(filter(None, a.ids.split(','))); kinds = set(filter(None, a.kinds.split(',')))
    todo = collect(a.gamedata)
    by_body = {}
    variants = {}
    for base, pre in VARIANT_PREFIX.items():
        for f in sorted(glob.glob(os.path.join(a.bodies, pre + '*.body'))):
            variants[os.path.splitext(os.path.basename(f))[0]] = base
    todo = todo + [(v, g, src) for v, base in variants.items() for b, g, src in todo if b == base]
    for b, g, src in todo:
        if only and b not in only: continue
        if ids and g['id'] not in ids: continue
        if kinds and kind_of(g) not in kinds: continue
        by_body.setdefault(b, []).append((g, src))
    import gf_body, gf_build, gf_geom as G
    for bname, items in by_body.items():
        path = os.path.join(a.bodies, bname + '.body')
        if not os.path.exists(path):
            print(f'[forgia] manca il corpo {path}: salto {bname}'); continue
        bpy.ops.wm.read_factory_settings(use_empty=True)
        body = gf_body.Body(path, bname)
        body.obj.color = (0.72, 0.58, 0.5, 1)
        odir = os.path.join(out_root, bname); os.makedirs(odir, exist_ok=True)
        ipath = os.path.join(odir, 'index.json')
        index = json.load(open(ipath)) if os.path.exists(ipath) else {}
        pieces = index.get('pieces', {})
        built = {}
        t0 = time.time()
        for g, src in items:
            h = shape_hash(bname, g)
            fname = f'{kind_of(g)}_{h}.gear'
            if h in built:
                pieces[g['id']] = built[h]; continue
            t = time.time()
            try:
                obj = gf_build.build(body, g, kind_of(g), style_of(g))
            except Exception as ex:
                import traceback; traceback.print_exc()
                print(f'[forgia] {bname}/{g["id"]}: ERRORE {ex}'); continue
            if obj is None:
                print(f'[forgia] {bname}/{g["id"]}: tipo {kind_of(g)} non ancora fatto'); continue
            G.triangulate(obj)
            nt = len(obj.data.polygons); bud = BUDGET.get(kind_of(g), 6000)
            if nt > bud: G.decimate(obj, bud / nt)
            for p_ in obj.data.polygons: p_.use_smooth = True
            ao = None
            if not a.no_ao:
                ao = G.bake_ao([obj], body, samples=16).get(obj.name)
            ntri = G.export(obj, body, os.path.join(odir, fname), ao=ao, rigid=obj.get('rigid'),
                            smooth_iters=int(obj.get('wsmooth', 4)))
            built[h] = fname; pieces[g['id']] = fname
            print(f'[forgia] {bname}/{g["id"]} ({kind_of(g)}, {style_of(g)}): {ntri} triangoli, {time.time() - t:.0f} s', flush=True)
            if a.preview:
                obj.color = gf_build.preview_color(g)
                obj.hide_render = False
            else:
                G.delete(obj)
        index['pieces'] = dict(sorted(pieces.items()))
        index['body'] = {'name': bname, 'height': round(body.H, 4), 'vertices': int(len(body.P))}
        index['note'] = 'Generato da Tools/GearForge/forge.py: non modificare a mano, rilanciare la forgia'
        json.dump(index, open(ipath, 'w'), indent=1)
        if not (ids or kinds):   # giro completo: via i file che nessuno strato usa piu'
            used = set(pieces.values())
            for f in os.listdir(odir):
                if f.endswith('.gear') and f not in used: os.remove(os.path.join(odir, f))
        print(f'[forgia] {bname}: {len(built)} pezzi in {time.time() - t0:.0f} s -> {odir}')
        if a.preview:
            import gf_preview
            os.makedirs(a.preview, exist_ok=True)
            gf_preview.render_views(os.path.join(a.preview, bname + '.png'), body)
            gf_preview.render_views(os.path.join(a.preview, bname + '_dettagli.png'), body, views=('torso', 'torso_back', 'legs'))


if __name__ == '__main__':
    main()
