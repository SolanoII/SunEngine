# Zombie indianpig

Una variante non morta del porcellino d'India dei Vrepigh. È la seconda versione del Pighcib, tenuta da parte.

Il modello è in `Sandbox/GameData/Models/Extra/zombie_indianpig/model.glb`. Per ora il gioco non lo carica. Ha lo
scheletro del Pighcib (`rig.json`, cioè le ossa `mixamorig1:*` più `Jaw`, `LeftEar` e `RightEar`), quindi può usare le
stesse animazioni. Per usarlo come corpo di un'unità, vedi `../indianpig/README.md`.

## Com'è fatto

- **Corpo:** gobbo, con costole e vertebre a nodi in vista, pancia gonfia e cadente, bubboni, arti magri e nodosi.
- **Testa:** scavata, con orbite infossate e occhi rossi.
- **Bocca:** larga da goblin, con incisivi gialli e denti a spillo.
- **Orecchie:** sbrindellate.
- **Pelle:** rognosa, con pelo infeltrito, schegge di pietra e vene nere.

## Rifarlo

```
python Tools/ModelForge/zombie_indianpig/build.py
```
