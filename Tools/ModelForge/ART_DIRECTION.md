# Direzione artistica di TSDH (modelli, materiali, luce)

Riferimenti di tono: la guerra di trincea sporca e religiosa (Trench Crusade), il gotico militare industriale
(Warhammer 40k), la fanteria corazzata leggibile a distanza (Halo), la guerra sporca e affollata di Helldivers.
Non si copiano disegni, loghi o personaggi di quelle opere: se ne prendono solo le proprieta' qui sotto.
Metodo preso da `gamedev-skills/awesome-gamedev-agent-skills` (create-game-assets: art-direction,
three-d-pipeline; shader-programming): si descrive un sistema di proprieta' osservabili, si approva un modello
"bersaglio", si producono famiglie coerenti e si giudica tutto nel gioco, alla distanza vera.

## Forma
- Masse grosse e pesanti, spigoli vivi con smusso piccolo (2-6 mm): niente forme gonfie o "a caramella".
- Silhouette a cuneo e a scudo: spalle e petto larghi, testa incassata, prue che puntano avanti.
- Ogni oggetto ha una funzione leggibile: rivetti, cerniere, cinghie, tubi, saldature, assi inchiodate.
- Asimmetria e danno: niente oggetto intatto; ammaccature, pezzi mancanti, rattoppi, filo legato.

## Silhouette (si legge a 30 m)
- Soldato: casco, spallacci, zaino/generatore. Vanguard: blocco unico, piu' largo che alto alle spalle.
- Prop di mappa: sacchi (file morbide e sfalsate), filo spinato (spirali), cavalli di frisia (X d'acciaio),
  croci di legno storte, alberi morti spezzati (mai chiome piene).

## Valori (scala di grigi)
- Tre bande: scuro (ferro, fango bagnato, ombre: 0.02-0.08 lineare), medio (terra, legno, tela: 0.08-0.25),
  chiaro solo per pochi punti (pietra dell'Ordo, ossa, metallo consumato sugli spigoli: 0.3-0.5).
- Nessuna superficie grande sopra 0.5 lineare: il bianco vero e' riservato a luci, fiamme e lampi.

## Colore e ruoli
- Ambiente: terra bruna, fango quasi nero, legno marcio, ruggine, juta. Saturazione bassa.
- Accenti con un significato: viola = Vanguard/ossidiana, oro sporco = sacro/Ordo, rosso = sangue e pericolo,
  verde malato = radiazione e acido, azzurro = energia. Solo le fazioni portano colori puliti.
- Cielo di smog caldo all'orizzonte e freddo in alto; ombre fredde, luci calde.

## Materiali (PBR glTF: colore sRGB, ORM e normali lineari)
- Ferro: metallo 0.8-1, ruvidita' 0.4-0.6, spigoli lucidi, ruggine nelle cavita'.
- Pietra/ceramica: metallo 0, ruvidita' 0.6-0.7, venature, colature verticali di sporco, scheggiature.
- Legno: venature, fibre, assi con chiodi, marcio e scuro in basso.
- Juta/tela: trama fine, fango in basso, macchie scure.
- Sporco sempre dal basso verso l'alto e dentro le cavita'. Niente luce cotta nel colore.

## Luce e immagine
- Sole basso e velato, nebbia in altezza con raggi di luce volumetrici, occlusione forte ai piedi delle cose.
- Riflessi metallici dal cielo (split-sum), occlusione speculare: niente plastica.
- Gradazione: neri profondi, contrasto alto, saturazione bassa, vignetta, grana leggera.

## Regole tecniche (three-d-pipeline)
- Metri, Y in alto nel .glb, origine ai piedi (prop) o allo scheletro (personaggi).
- Texture in potenze di due; texel density uguale nella famiglia; 1024 per prop, 2048 per eroi.
- Le collisioni restano i blocchi della mappa: le mesh sono solo da vedere.
- Ogni famiglia ha uno script di Blender che la ricostruisce (`build.py`) e un'anteprima (`render.py`).
