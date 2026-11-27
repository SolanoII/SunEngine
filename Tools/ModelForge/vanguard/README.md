# Vanguard Armor (Blender)

Corpi d'autore delle Vanguard Armor: umano potenziato chiuso in un'armatura pesante a piastre.

Forma: lamiere spesse piegate a sfaccettature (loft di sezioni ottagonali con la chiglia davanti, `loft`/`limb`/`tring`
in `build.py`), pieghe arrotondate e bordi smussati netti, piastre sovrapposte a embrice. Spallacci a cupola con bordo
d'ottone, chiodi e due lame; collare alto in cui affonda l'elmo; zaino con reattore in gabbia, alette e scarichi;
pistoni idraulici su braccia e polpacci; cintura con tasche e fiancali. L'elmo e' un disegno originale: calotta
bassa sfaccettata, fronte a becco che scende sul viso, due feritoie inclinate incassate in ombra, mascella a prua
con sfiati sui fianchi. Sulla generica c'e' anche lo sporco di battaglia (fango sulle gambe, colature, fuliggine).

Konrad (Ordo) e' un cavaliere teutonico: elmo a secchio massiccio che si allarga sul collo, croce nera sul viso, feritoie
e fori di respirazione tagliati nella lamiera (booleana, la luce sta dentro), cimiero a corna nere; croce nera su una placca al centro del petto, nucleo bianco all'altezza del diaframma; cappa bianca fino ai polpacci con la croce nera sulla schiena (pesi per vertice, segue il busto);
scanalature gotiche su spallacci e pettorali; cotta di maglia sotto le piastre; sopravveste bianca con la croce.

- `python3 build.py` -> `Sandbox/GameData/Models/Characters/vanguard/model.glb` (armatura generica: vernice grigio-oliva scheggiata, ottone brunito, nucleo di ossidiana viola)
- `python3 build.py --variant ordo` -> `.../vanguard/variants/vanguard_konrad.glb` (Ordo Domus Humilitatis: cavaliere teutonico in pietra bianca, cappa e croci nere, feritoia di luce bianco-dorata)
- `--fast` (texture piccole, prove veloci), `--tex 2048`, `--out file.glb`
- `DISPLAY=:99 python3 render.py vanguard_<var>.blend anteprima.png [--cycles]`: cinque viste di controllo

Lo scheletro e' quello di `vanguard_master.fbx` (le clip del gioco ci girano sopra). `model.json` con
`"complete_armor": true` dice al gioco di non disegnare i pezzi d'armatura dell'unita' sopra il modello.
Serve Blender come modulo Python (`bpy`) e `scikit-image` per la tuta sotto le piastre.

Sotto i materiali cotti ci sono scansioni fotografiche CC0 di Poly Haven (`../photo_lib.py`): piastre in
`green_metal_rust` (generica) o `marble_rock_01` (Ordo, pietra bianca venata), finiture in `rusty_metal_sheet`, cuoio
`brown_leather`, stoffa `hessian_230`, tuta `rubberized_track`. La foto da' graffi, venature e grana; la tinta resta
quella della tavolozza. Sulla generica le zone chiare di `rusty_metal_04` decidono dove la vernice e' saltata fino
all'acciaio, soprattutto sugli spigoli.
