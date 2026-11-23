# Indianpig

Il porcellino d'India dei Vrepigh: è la prima versione del Pighcib, tenuta da parte per riusarla.

Il modello è in `Sandbox/GameData/Models/Extra/indianpig/model.glb`. Per ora il gioco non lo carica. Ha lo scheletro
del Pighcib (`rig.json`, cioè le ossa `mixamorig1:*` più `Jaw`, `LeftEar` e `RightEar`), quindi può usare le stesse
animazioni.

Per usarlo come corpo di un'unità: copialo in `Models/Characters/pighcib/variants/<id unità>.glb` e aggiungi
`pighcib` a `VARIANT_PREFIX` in `Tools/GearForge/forge.py`.

## Com'è fatto

- **Corpo:** a pera, con la testa lunga e tonda senza collo.
- **Testa:** muso smussato col disco del naso, labbro spaccato e zannette.
- **Orecchie e occhi:** orecchie a petalo che ricadono, occhietti neri lucidi.
- **Pelo:** a rosette in tre colori, crema, fulvo e scuro, con una lista bianca sul muso.

## Rifarlo

```
python Tools/ModelForge/indianpig/build.py
```
