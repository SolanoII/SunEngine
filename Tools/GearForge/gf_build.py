# Smistamento: ogni tipo di strato al suo costruttore.
import gf_cloth, gf_armor


def build(body, g, kind, style):
    name = g['id']
    if kind == 'shirt':
        return gf_cloth.shirt(body, g, name)
    if kind == 'pants':
        if g.get('side'):
            return gf_cloth.pants(body, g, name, bottom_frac=gf_cloth.pants_bottom(body, g))
        # calzoni interi (nessun lato nel JSON): due gambe cucite al centro
        import gf_geom as G
        legs = [gf_cloth.pants(body, dict(g, side=s), f'{name}_{s}', bottom_frac=gf_cloth.pants_bottom(body, g)) for s in (1, -1)]
        return G.join(legs, name)
    if kind == 'rags':
        return gf_cloth.rags(body, g, name)
    if kind == 'boot':
        return gf_armor.boot(body, g, name, style)
    if kind == 'helmet':
        return gf_armor.helmet(body, g, name, style)
    if kind == 'band':
        return gf_armor.helmet_band(body, g, name)
    if kind in ('arm_plate', 'leg_plate', 'bracer'):
        return gf_armor.limb_plate(body, g, name, kind, style)
    if kind in ('flak', 'chest_plate', 'heavy_torso'):
        return gf_armor.torso_armor(body, g, name, kind, style)
    if kind == 'belt':
        return gf_armor.belt(body, g, name, style)
    return None


def preview_color(g):
    c = g.get('color', [0.6, 0.6, 0.6, 1])
    t = g.get('texture', '')
    base = (0.45, 0.43, 0.33) if 'fatigues' in t else (0.5, 0.5, 0.48) if 'flak' in t else (0.42, 0.33, 0.25) if 'cloth' in t or 'rags' in t else (0.45, 0.4, 0.36)
    return (base[0] * c[0], base[1] * c[1], base[2] * c[2], 1)
