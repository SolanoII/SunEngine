# Modello del Pighcib

Il Pighcib è un porcellino d'India guastato dalla pietra, grottesco come un goblin. Il modello d'autore finisce in
`Sandbox/GameData/Models/Characters/pighcib/model.glb` e il gioco lo usa al posto del corpo procedurale. La prima
versione, quella carina, è tenuta da parte come `indianpig` (`../indianpig/`).

## Com'è fatto

- **Corpo:** gobbo, con le costole in vista sopra una pancia gonfia e cadente, le vertebre a nodi lungo la schiena e
  bubboni sparsi. Gli arti sono magri e nodosi, con mani ossute dalle dita lunghe e piedi lunghi.
- **Testa:** ancora da porcellino d'India, ma stirata e scavata. Ha arcate pesanti sopra orbite infossate, zigomi aguzzi e
  tempie incavate. Dai Vrepigh prende il disco del naso, a carne viva.
- **Bocca:** larga da goblin, taglia le guance. Ha incisivi da roditore gialli e lunghi, file di denti a spillo storti
  sopra e sotto e due zannette, una spezzata. I denti di sotto sono sull'osso `Jaw`.
- **Orecchie:** lunghe e appuntite, ricadono all'indietro e hanno tacche e buchi (ossa `LeftEar` / `RightEar`).
- **Occhi:** piccoli, in fondo alle orbite, con un riflesso rosso.
- **La pietra:** schegge scure, appena luminose di verde malato, bucano la pelle su spalla, schiena, cranio, braccio e
  fianco. Attorno hanno carne viva e un reticolo di vene nere.
- **Pelle:** grigio-rosata e rognosa, con croste, chiazze di pelo sporco (fulvo, fuliggine, crema ingiallito) e ciuffi
  infeltriti. Mani e piedi sono neri di sporco.

Lo scheletro è quello del gioco (`rig.json`, dal `pighcib_master.fbx`), più `Jaw` e le due orecchie.

I materiali sono questi:

- `Skin` e `Skin_Ears`: pelle cotta in una texture;
- `Fur_Rust`, `Fur_Soot`, `Fur_Dirty`: ciuffi di pelo;
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
python Tools/ModelForge/vrepigh/render.py Tools/ModelForge/pighcib/pighcib.blend anteprima.png 0.95 -0.19 0.76
```

Dopo ogni modifica bisogna rifare anche gli stracci:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only pighcib`.
