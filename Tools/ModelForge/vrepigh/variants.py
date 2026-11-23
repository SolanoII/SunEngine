# Varianti del Vrepigh, una per unita' (GameData/Units/vrepighs_*.json). Tutte usano lo scheletro del modello base:
# cambia solo la carne (moltiplicatori della forma in sdf.py), zanne, criniera, orecchie e colori.
# La taglia in partita la da' "model_scale" nel JSON dell'unita'.
BASE = 'tusker'

# chiavi che cambiano la forma del corpo (se cambiano si rifa' il campo di distanza)
SHAPE_KEYS = ('head', 'head_fwd', 'head_up', 'snout', 'jowl', 'brow', 'hips', 'belly', 'belly_drop', 'chest', 'chest_sag',
              'hump', 'neck', 'arm', 'forearm', 'hand', 'leg')

V = {
    # Vrepigh: il soldato tipo, il modello base
    'tusker': {},

    # Razziatore: asciutto e nervoso, criniera alta a cresta, zanne medie, cuoio rossiccio
    'raider': dict(seed=11, belly=0.86, chest=0.95, hump=0.92, arm=0.93, forearm=0.95, leg=0.95, jowl=0.92, snout=1.06,
                   tusk=0.9, mane=1.35, mane_n=1.2, mane_spread=0.7, cheek=0.6, ear=1.05, scars=3,
                   base=(0.085, 0.05, 0.036), dark=(0.04, 0.022, 0.016), mane_col=(0.05, 0.022, 0.012)),

    # Sparammazza: tarchiato, spalle larghe e avambracci da rinculo, una zanna scheggiata, cuoio grigio scuro
    'gunner': dict(seed=13, chest=1.07, belly=1.04, hump=1.02, neck=1.06, arm=1.08, forearm=1.16, hand=1.1, leg=1.04,
                   tusk=0.9, tusk_broken='R', mane=0.85, ear=0.95, ear_torn='L', scars=4,
                   base=(0.062, 0.054, 0.048), dark=(0.028, 0.025, 0.023)),

    # Feral Pig: giovane, magro e storto, testa grande sul collo, criniera arruffata, zanne appena spuntate,
    # ancora le striature da cucciolo
    'feral_pig': dict(seed=17, head=1.08, belly=0.78, chest=0.85, hips=0.9, hump=1.05, neck=0.85, arm=0.82, forearm=0.85,
                      hand=0.92, leg=0.86, jowl=0.85, brow=0.85, snout=0.95, tusk=0.55, tusk_up=0.4, tusk_r=0.75,
                      mane=1.5, mane_n=1.35, mane_spread=1.6, cheek=1.5, cheek_len=1.3, ear=1.15, stripes=1.0,
                      base=(0.1, 0.068, 0.048), belly_col=(0.16, 0.11, 0.08), mane_col=(0.06, 0.04, 0.028)),

    # Kragh: capo di un vargh, un muro di muscoli, gobba enorme, zanne grosse e scure, cicatrici
    'kragh': dict(seed=19, head=1.04, chest=1.14, belly=1.02, hump=1.25, neck=1.18, arm=1.17, forearm=1.18, hand=1.1,
                  leg=1.12, hips=1.06, jowl=1.12, brow=1.25, tusk=1.3, tusk_r=1.25, tusk_curl=1.2, tusk_up=1.2, mane=1.15,
                  mane_spread=1.25, scars=7, ear_torn='R',
                  base=(0.058, 0.044, 0.036), dark=(0.025, 0.019, 0.016), tusk_col=(0.5, 0.44, 0.32)),

    # Gorvakh: il superiore di Kragh, enorme, quasi nero, zanne da ariete che si arrotolano, giogaie pesanti,
    # cicatrici ovunque
    'gorvakh': dict(seed=23, head=1.1, head_fwd=0.02, head_up=0.03, chest=1.22, belly=1.12, belly_drop=0.03, hump=1.32, neck=1.28,
                    arm=1.26, forearm=1.25, hand=1.18, leg=1.18, hips=1.1, jowl=1.3, brow=1.4, snout=1.05,
                    tusk=1.6, tusk_r=1.45, tusk_curl=1.7, tusk_up=1.5, mane=1.3, mane_n=1.3, mane_spread=1.4,
                    cheek=1.3, cheek_len=1.25, scars=11, ear=0.9, ear_torn='L',
                    base=(0.04, 0.032, 0.028), belly_col=(0.07, 0.052, 0.044), dark=(0.016, 0.013, 0.012),
                    snout_col=(0.11, 0.07, 0.06), mane_col=(0.018, 0.014, 0.012), tusk_col=(0.46, 0.4, 0.29),
                    eye=(0.25, 0.03, 0.0)),

    # Zhelvar, il Vecchio: pancia che cade, petto svuotato, zanne lunghissime (una spezzata), criniera e grugno
    # grigi, orecchie che ricadono
    'zhelvar': dict(seed=29, head=1.02, chest=1.0, chest_sag=0.04, belly=1.2, belly_drop=0.07, hump=1.22, neck=1.05,
                    arm=0.96, forearm=1.0, hand=1.02, leg=1.0, jowl=1.22, brow=1.15, snout=1.08,
                    tusk=1.5, tusk_r=1.15, tusk_curl=1.5, tusk_up=1.3, tusk_broken='L', mane=1.1, mane_spread=1.2,
                    cheek=1.6, cheek_len=1.5, ear_droop=0.45, ear=1.05, grizzle=0.85, scars=6,
                    base=(0.07, 0.062, 0.056), dark=(0.04, 0.036, 0.033), mane_col=(0.2, 0.19, 0.18),
                    tusk_col=(0.55, 0.47, 0.33)),
}

# unita' del gioco -> variante
UNITS = {'vrepighs_' + k: k for k in V}


def get(name):
    if name not in V:
        raise SystemExit(f'variante sconosciuta: {name} (ci sono: {", ".join(V)})')
    return dict(V[name])
