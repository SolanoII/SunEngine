# Modello del Vrepigh (uomo-cinghiale)

Script di Blender che costruiscono il modello d'autore del Vrepigh,
`Sandbox/GameData/Models/Characters/vrepigh/model.glb`. Il gioco lo usa al posto del corpo procedurale
(vedi `Engine/Renderer/AuthoredModel.h`).

## Com'è fatto

| parte | come nasce |
|---|---|
| corpo | campo di distanza (`sdf.py`): ellissoidi e capsule fusi con unioni morbide, poi marching cubes, decimazione e lisciatura. Torace a botte, gobba e garrese alti, collo taurino, braccia e avambracci massicci, mani tozze, piedi con lo zoccolo fesso |
| testa | cuneo basso che scende nel grugno, giogaie gonfie, arcata pesante sopra occhi piccoli, narici, solco della bocca, mandibola separata (osso `Jaw`) |
| zanne | due grandi dalla mandibola, curve in alto e in fuori (seguono `Jaw`), e due più corte dal labbro superiore |
| orecchie | foglie appuntite a coppa, ossa `LeftEar` / `RightEar` |
| setole | criniera dalla fronte lungo la cresta, la gobba e la schiena; ciuffi sulle guance; ciuffo in fondo alla coda |
| coda | esce sopra il cinturone, osso `Tail` |
| artigli | coni scuri in punta alle dita |

Le ossa sono quelle del gioco (`mixamorig1:*`, posizioni in `rig.json`), più `Jaw`, `LeftEar`, `RightEar` e `Tail`:
le clip del gioco passano dal retarget dei modelli d'autore. I pesi del corpo sono calcolati da Blender (calore) e poi
corretti a mano: testa e mandibola sono definite dalla forma; gobba e collo non prendono peso dalla testa.

Il colore è dipinto per vertice (cuoio bruno scuro, schiena e arti più scuri, ventre e grugno più chiari, zoccoli
quasi neri, interno delle orecchie rosato), con i pori e le striature del pelo aggiunti al momento della cottura.
Sopra c'e' il cuoio fotografato `fabric_leather_02` (Poly Haven, CC0, `../photo_lib.py`): rughe e pieghe nel colore,
ruvidita' in `Skin_ORM` e rilievo (pieghe + pori) in `Skin_Normal`. Tutto finisce nel GLB (`Skin_BaseColor` 2048 px).

Materiali, che il gioco riconosce dal nome:

- `Skin`, `Skin_Ears`, `Skin_Tail`: pelle, con la luce sotto la superficie;
- `Fur_Bristle`: setole;
- `Tusk_Ivory`: zanne;
- `Eye`: occhi;
- `Claw_Horn`: artigli.

Per la forgia delle vesti conta solo `Skin`: orecchie, coda, setole, zanne e artigli sono parti aggiunte e non reggono
le vesti (`GameLayer::RegisterGearBody`).

## Le varianti (una per unità)

Ogni unità Vrepigh ha il suo modello, costruito con lo stesso scheletro: cambia solo la carne. La taglia in partita resta
quella del `model_scale` nel JSON dell'unità.

| unità | variante | com'è |
|---|---|---|
| `vrepighs_tusker` (Vrepigh) | base, `model.glb` | il soldato tipo |
| `vrepighs_raider` (Razziatore) | `variants/vrepighs_raider.glb` | asciutto e nervoso, cresta di setole alta, cuoio rossiccio |
| `vrepighs_gunner` (Sparammazza) | `variants/vrepighs_gunner.glb` | tarchiato, avambracci grossi, una zanna scheggiata, un orecchio mozzato |
| `vrepighs_feral_pig` (Feral Pig) | `variants/vrepighs_feral_pig.glb` | giovane e magro, testa grande, criniera arruffata, zanne appena spuntate, striature da cucciolo |
| `vrepighs_kragh` (Kragh) | `variants/vrepighs_kragh.glb` | un muro di muscoli, gobba enorme, zanne grosse, cicatrici |
| `vrepighs_gorvakh` (Gorvakh) | `variants/vrepighs_gorvakh.glb` | enorme e quasi nero, zanne da ariete arrotolate, giogaie pesanti, cicatrici ovunque |
| `vrepighs_zhelvar` (Zhelvar, il Vecchio) | `variants/vrepighs_zhelvar.glb` | pancia che cade, zanne lunghissime (una spezzata), criniera e grugno grigi, orecchie che ricadono |

I numeri delle varianti sono in `variants.py`. Il gioco carica `variants/<id unità>.glb` accanto al modello del corpo:
anima lo scheletro del corpo e disegna la variante (`GameLayer::LoadBodyVariants`). Ogni variante ha anche le sue vesti
in `GameData/Gear/<id unità>/`, che la forgia costruisce come per il corpo base.

## Rifarlo

```
pip install bpy numpy scipy scikit-image pillow
python Tools/ModelForge/vrepigh/build.py                      # modello base (Vrepigh)
python Tools/ModelForge/vrepigh/build.py --variant gorvakh    # una variante
```
Il modello base va fatto per primo: scrive `rig_extra.json` (posizione delle ossa della mascella, delle orecchie e della
coda), che le varianti riusano.

Lo script scrive `model.glb` nella cartella del gioco (oppure dove indica `--out <file>`) e lascia qui
`vrepigh_<variante>.blend`, utile per ritoccarlo a mano. La forma già calcolata resta in `shape_<variante>.npz` e si
rifà da sola quando cambiano i numeri della variante; se modifichi `sdf.py`, cancella quei file. Per un'anteprima rapida:

```
python Tools/ModelForge/vrepigh/check.py --pose --blend Tools/ModelForge/vrepigh/vrepigh_tusker.blend anteprima.png
```

Dopo ogni modifica al corpo bisogna rifare le vesti e le armature:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only vrepigh,vrepighs_raider,...` (le varianti si chiamano come l'unità; vedi il
   README della forgia).
