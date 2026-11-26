# Vanguard Armor (Blender)

Corpi d'autore delle Vanguard Armor: umano potenziato chiuso in un carapace spesso e aderente.

- `python3 build.py` -> `Sandbox/GameData/Models/Characters/vanguard/model.glb` (armatura generica: ferro scuro, nucleo di ossidiana viola)
- `python3 build.py --variant ordo` -> `.../vanguard/variants/vanguard_konrad.glb` (Ordo Domus Humilitatis: pietra bianca invecchiata, nucleo nero, croci, ossa)
- `--fast` (texture piccole, prove veloci), `--tex 2048`, `--out file.glb`
- `DISPLAY=:99 python3 render.py vanguard_<var>.blend anteprima.png [--cycles]`: cinque viste di controllo

Lo scheletro e' quello di `vanguard_master.fbx` (le clip del gioco ci girano sopra). `model.json` con
`"complete_armor": true` dice al gioco di non disegnare i pezzi d'armatura dell'unita' sopra il modello.
Serve Blender come modulo Python (`bpy`) e `scikit-image` per la tuta sotto le piastre.

Sotto i materiali cotti ci sono scansioni fotografiche CC0 di Poly Haven (`../photo_lib.py`): piastre in
`green_metal_rust` (generica) o `marble_rock_01` (Ordo, pietra bianca venata), finiture in `rusty_metal_sheet`, cuoio
`brown_leather`, stoffa `hessian_230`, tuta `rubberized_track`. La foto da' graffi, venature e grana; la tinta resta
quella della tavolozza.
