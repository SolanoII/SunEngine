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
Tutto finisce in `Skin_BaseColor` (2048 px), dentro il GLB.

Materiali, che il gioco riconosce dal nome:

- `Skin`, `Skin_Ears`, `Skin_Tail`: pelle, con la luce sotto la superficie;
- `Fur_Bristle`: setole;
- `Tusk_Ivory`: zanne;
- `Eye`: occhi;
- `Claw_Horn`: artigli.

Per la forgia delle vesti conta solo `Skin`: orecchie, coda, setole, zanne e artigli sono parti aggiunte e non reggono
le vesti (`GameLayer::RegisterGearBody`).

## Rifarlo

```
pip install bpy numpy scipy scikit-image pillow
python Tools/ModelForge/vrepigh/build.py
```

Lo script scrive `model.glb` nella cartella del gioco (oppure dove indica `--out <file>`) e lascia qui
`vrepigh.blend`, utile per ritoccarlo a mano. Per cambiare la forma, modifica `sdf.py` e cancella `shape.npz`, che
contiene la forma già calcolata. Per un'anteprima rapida:

```
python Tools/ModelForge/vrepigh/check.py --pose --blend Tools/ModelForge/vrepigh/vrepigh.blend anteprima.png
```

Dopo ogni modifica al corpo bisogna rifare le vesti e le armature:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only vrepigh` (vedi il README della forgia).
