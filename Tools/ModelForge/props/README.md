# Prop delle mappe (Blender)

Pezzi di trincea secondo `../ART_DIRECTION.md`, costruiti e cotti da script:

| id | cosa | nel gioco |
|---|---|---|
| `sandbag_wall` | file sfalsate di sacchi di juta, 2 x 1 x 0.8 m | veste i blocchi `sandbags` |
| `trench_berm` | terrapieno con assi, pali e filo, 3 x 1.3 x 1 m | veste i blocchi `earthworks` |
| `dead_tree` | albero morto spezzato, 7.4 m | sostituisce tronco + chioma a cubi |
| `barbed_wire` | filo spinato a spirale su cavalletti, 3 m | sparso sul terreno libero |
| `czech_hedgehog` | cavallo di frisia d'acciaio | sparso |
| `grave_cross` | croce di legno con elmetto, teschio e straccio | sparso |

```
python3 build.py                       # tutti, cotti (Cycles), in Sandbox/GameData/Models/Props/<id>.glb
python3 build.py --only dead_tree      # uno solo
python3 build.py --fast                # senza cottura (solo forme)
DISPLAY=:99 python3 render.py anteprima.png --cycles
```

Regole: metri, origine al centro della base, lato lungo su X; trasformazioni applicate prima di esportare (il gioco
legge i vertici, non la posizione dei nodi). Un oggetto per materiale, texture colore/ORM/normali cotte.
Il gioco li carica e li dispone in `Sandbox/GameLayerProps.cpp`: i blocchi restano per collisioni e navmesh.

## Materiali fotografici

Sacchi (juta `hessian_230`), assi (`rough_wood`), corteccia (`bark_brown_02`), ferro (`rusty_metal_04`) e terra
(`brown_mud_02`) usano foto scansionate di Poly Haven (CC0): `build.py` le scarica in `_photo/` (non nel repo) con le
loro misure reali e le proietta a cubo sui pezzi. Sopra restano fango in basso, sporco nelle pieghe e usura sugli
spigoli. Senza rete restano i materiali procedurali di prima.
