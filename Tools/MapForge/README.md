# MapForge: mappe generate da script

`battlefield.py` scrive `Sandbox/GameData/Maps/vsbot_trincee.json` ("Trincee di Solemnace", Vs Bot 5 contro 5):

```
python3 Tools/MapForge/battlefield.py            # stessa mappa ogni volta (seme 7)
python3 Tools/MapForge/battlefield.py --seed 12  # crateri e detriti in altri punti
```

La mappa e' simmetrica (rotazione di 180 gradi): si disegna la meta' della squadra 0 e si ruota per la squadra 1.

- prima linea a denti con traverse, muri di terra (vestiti da `trench_berm`), gradino di tiro, sacchi sul
  parapetto, varchi per uscire e passerelle (`duckboard`) sul fondo; fuori dal muro un argine di terra (`earth_bank`)
- due camminamenti con gomito dalla base alla prima linea
- casamatta di cemento su un rialzo con feritoia, porta e tetto; nido di mitragliatrice di sacchi
- terra di nessuno: due file di reticolati con varchi, cavalli di frisia, crateri (`crater`), alberi morti, croci,
  rovina della fattoria al centro
- bordi: muri invisibili (`"hidden": true`) coperti da terrapieni

## Formato

| voce | cosa fa |
|---|---|
| `{"type": "block", ...}` | collisione + navmesh; `material` sceglie la superficie, `"hidden": true` la nasconde |
| `{"type": "prop", "model": "crater", "position": [x, y, z], "yaw": 90, "scale": 1.2}` | solo da vedere: `Models/Props/<model>.glb`, base nel punto dato; `scale` numero o `[x, y, z]` |
| `"scatter_props": false` | niente reticolati/croci sparsi a caso (la mappa li mette gia' a mano) |

I blocchi di sacchi, terrapieni bassi e tronchi vengono comunque vestiti dai prop (`Sandbox/GameLayerProps.cpp`).
I prop non hanno collisione: chi ci passa sopra cammina sul terreno.
