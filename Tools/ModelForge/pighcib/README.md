# Modello del Pighcib

Il Pighcib è una bestiola viva e arrabbiata, a metà tra un Vrepigh, un porcellino d'India, un ratto e un goblin. Il
modello d'autore finisce in `Sandbox/GameData/Models/Characters/pighcib/model.glb` e il gioco lo usa al posto del corpo
procedurale. Le versioni precedenti sono tenute da parte: quella carina come `indianpig` (`../indianpig/`), quella
non morta come `zombie_indianpig` (`../zombie_indianpig/`).

## Com'è fatto

- **Corpo:** nervoso, con la gobba dei Vrepigh, petto e braccia asciutti, mani grandi da goblin con dita lunghe e
  artigli, cosce piene e piedi lunghi da ratto.
- **Testa:** da porcellino d'India, col muso lungo e appuntito del ratto. Dai Vrepigh prende il disco del naso e le
  zanne, che escono dalla mandibola e si piegano verso l'alto (osso `Jaw`).
- **Espressione:** fronte aggrottata a V, naso arricciato, labbro alzato su incisivi gialli e denti a spillo, occhi
  stretti color ambra.
- **Orecchie:** appuntite, schiacciate all'indietro, con tacche e buchi (ossa `LeftEar` / `RightEar`).
- **Pelo:** mantello a chiazze (fulvo, nero, crema sporco), con la criniera irta dei Vrepigh dalla fronte alla schiena
  e baffi lunghi sul muso.
- **Coda:** lunga e nuda da ratto, legata all'anca (`Hips`).
- **La pietra:** poche schegge scure, appena luminose, su spalla, schiena e braccio, con un po' di vene attorno.

Lo scheletro è quello del gioco (`rig.json`, dal `pighcib_master.fbx`), più `Jaw` e le due orecchie.

I materiali sono questi:

- `Skin` e `Skin_Ears`: pelle cotta in una texture;
- `Fur_Rust`, `Fur_Soot`, `Fur_Dirty`, `Fur_Whiskers`: pelo, criniera e baffi;
- `Skin_Tail`: coda;
- `Stone_Shard`: le schegge, con una leggera emissione;
- `Eye`: occhi, con una leggera emissione;
- `Teeth_Bone`: denti;
- `Tusk_Ivory`: zannette;
- `Claw_Horn`: artigli.

Per la forgia delle vesti conta solo `Skin`.

## Rifarlo

```
python Tools/ModelForge/pighcib/build.py
```

Usa le primitive di `../vrepigh/sdf.py`. Se modifichi `sdf.py`, cancella `shape.npz`. Per un'anteprima a colori:

```
python Tools/ModelForge/vrepigh/render.py Tools/ModelForge/pighcib/pighcib.blend anteprima.png 0.95 -0.21 0.76
```

Dopo ogni modifica bisogna rifare anche gli stracci:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only pighcib`.
