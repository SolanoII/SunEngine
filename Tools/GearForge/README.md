# Forgia di vesti e armature (GearForge)

Script di Blender che costruisce ogni strato `garments` delle armature (e i vestiti base delle unità) sul corpo vero
del gioco e li salva in `Sandbox/GameData/Gear/<corpo>/`. Il gioco li carica al posto dei gusci generati dal corpo
(vedi `Engine/Renderer/GearMesh.h`): se un pezzo manca, resta il guscio di prima.

Cosa fa per ogni tipo di pezzo:

| tipo | come nasce |
|---|---|
| camicie, pantaloni, calzoni | zona del corpo, orli tagliati dritti, giro largo che scavalca i muscoli, poi simulazione della stoffa (pieghe sotto le braccia, sbuffo sopra lo stivale) |
| stracci | come sopra, con buchi e orli strappati |
| stivali | gambale dal corpo + involucro del piede (niente dita), suola piatta, due cinghie |
| elmi | griglia sferica misurata sul cranio, calotta liscia, orlo arrotolato, rivetti; tesa e sottogola (Regno), fasce a croce (vrepigh, rottami) |
| spallacci | coppa sulla spalla + lame sul braccio, rivetti, cinghia |
| bracciali, schinieri, ginocchiere | piastre su griglie cilindriche attorno all'osso, bordi arrotondati, cinghie |
| giubbetti antischegge | giubbetto imbottito con scalfi e scollo, piastre in rilievo davanti e dietro, spallacci imbottiti |
| corazze vrepigh | fasce di lamiera sovrapposte e rivettate (solo davanti, o tutto attorno per quella pesante) |
| cinturoni | fascia attorno ai fianchi con fibbia |

Ogni pezzo porta nei vertici la distanza dal bordo (orli, cuciture, spigoli consumati nello shader) e l'occlusione
ambientale cotta con Cycles. I colori, le texture e le varianti restano quelli dei JSON delle armature.

## Quando rilanciarla

- è cambiato un corpo (nuovo modello d'autore in `GameData/Models/Characters/<corpo>/`, muscoli, BodyFit...);
- è cambiato un JSON di armatura (zona, spessore) o si è aggiunto un pezzo nuovo.

## Come

1. Python 3.13 e il modulo di Blender: `pip install bpy` (più `numpy scipy pillow` se mancano).
2. Esporta i corpi dal gioco: avvia il gioco con la variabile `SUN_GEAR_DUMP` e chiudilo appena arriva al menu.
   In PowerShell:
   ```
   $env:SUN_GEAR_DUMP = "D:\Engine\gear_bodies"; .\ProjectSunCollector.exe
   ```
   Nella cartella compaiono `footman.body`, `candidate.body`, `vrepigh.body`, `pighcib.body`...
3. Lancia la forgia dalla radice del repo:
   ```
   python Tools/GearForge/forge.py --bodies D:\Engine\gear_bodies --gamedata Sandbox/GameData
   ```
   Opzioni: `--only footman,vrepigh` (solo alcuni corpi), `--ids footman_helmet` (solo alcuni strati),
   `--kinds helmet,boot` (solo alcuni tipi), `--preview <cartella>` (anteprime PNG), `--no-ao` (più veloce).
   Un corpo intero richiede pochi minuti (la stoffa è la parte lenta).
4. Ricompila (la cartella GameData viene copiata accanto all'eseguibile) e controlla in Vetrina.

Pezzi con la stessa forma (stessa zona e spessore, colore diverso, per esempio recluta e veterano) vengono costruiti
una volta sola: `index.json` collega ogni id al suo file.

## File

- `forge.py`: raccolta degli strati dai JSON, corpo di ogni gruppo, scrittura di `.gear` e `index.json`
- `gf_body.py`: corpo esportato dentro Blender (pelle rifusa, ossa, pesi, raggi)
- `gf_cloth.py`: vesti di stoffa simulate
- `gf_armor.py`: elmi, piastre, corazze, stivali, cinturoni
- `gf_geom.py`: tagli, lisciature, spessore, occlusione, pesi, esportazione
- `gf_io.py`: formato dei file (uguale a `Engine/Renderer/GearMesh.h`)
- `gf_preview.py`: anteprime veloci
