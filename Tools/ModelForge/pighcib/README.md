# Modello del Pighcib

Un piccolo Vrepigh che somiglia a un porcellino d'India. Il modello d'autore finisce in
`Sandbox/GameData/Models/Characters/pighcib/model.glb` e il gioco lo usa al posto del corpo procedurale.

## Com'è fatto

- **Corpo:** a pera, con la pancia bassa e le cosce piene che arrivano al ginocchio; zampette corte con tre dita
  davanti.
- **Testa:** grande e lunga, senza collo (affonda nelle spalle), naso romano e muso largo e smussato. Dai Vrepigh
  prende il disco del naso e le zannette agli angoli della bocca (osso `Jaw`); ha anche guance gonfie e il labbro
  spaccato.
- **Orecchie:** a petalo, tonde, che ricadono in fuori (ossa `LeftEar` / `RightEar`).
- **Occhi:** occhietti neri, lucidi e sporgenti.
- **Pelo:** a rosette (vortici) sulla schiena, sui fianchi, sulle spalle e in testa, con ciuffi sulle guance.
- **Colori:** chiazze in tre colori, crema, fulvo e scuro. Sul muso c'è una lista bianca, con una mascherina scura da un
  lato e fulva dall'altro. Naso, interno delle orecchie e zampe sono rosati.

Lo scheletro è quello del gioco (`rig.json`, dal `pighcib_master.fbx`), più `Jaw` e le due orecchie.

I materiali sono questi:

- `Skin` e `Skin_Ears`: pelle e pelo cotti in una texture;
- `Fur_Cream`, `Fur_Ginger`, `Fur_Dark`: ciuffi delle rosette;
- `Eye`: occhi;
- `Tusk_Ivory`: zannette;
- `Claw_Horn`: unghiette.

Per la forgia delle vesti conta solo `Skin`.

## Rifarlo

```
python Tools/ModelForge/pighcib/build.py
```

Usa le primitive di `../vrepigh/sdf.py`. Se modifichi `sdf.py`, cancella `shape.npz`. Dopo ogni modifica bisogna rifare
anche le vesti:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only pighcib`.
