# Vanguard Armor (Blender)

Corpi d'autore delle Vanguard Armor: umano potenziato chiuso in un carapace spesso e aderente.

- `python3 build.py` -> `Sandbox/GameData/Models/Characters/vanguard/model.glb` (armatura generica: ferro scuro, nucleo di ossidiana viola)
- `python3 build.py --variant ordo` -> `.../vanguard/variants/vanguard_konrad.glb` (Ordo Domus Humilitatis: pietra bianca invecchiata, nucleo nero, croci, ossa)
- `--fast` (texture piccole, prove veloci), `--tex 2048`, `--out file.glb`
- `DISPLAY=:99 python3 render.py vanguard_<var>.blend anteprima.png [--cycles]`: cinque viste di controllo

Lo scheletro e' quello di `vanguard_master.fbx` (le clip del gioco ci girano sopra). `model.json` con
`"complete_armor": true` dice al gioco di non disegnare i pezzi d'armatura dell'unita' sopra il modello.
Serve Blender come modulo Python (`bpy`) e `scikit-image` per la tuta sotto le piastre.
