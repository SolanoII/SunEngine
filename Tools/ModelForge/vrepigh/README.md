# Modello del Vrepigh (uomo-cinghiale)

Script di Blender che costruiscono il modello d'autore del Vrepigh,
`Sandbox/GameData/Models/Characters/vrepigh/model.glb`. Il gioco lo usa al posto del corpo procedurale
(vedi `Engine/Renderer/AuthoredModel.h`).

## Com'è fatto

Linea della razza: violenta, affamata, bellicosa. Predatore asciutto e nodoso, non un maiale grasso: gobba e scudo
delle spalle da cinghiale maschio, trapezi enormi, costole sui fianchi, muso ringhiante con i denti scoperti.
Riferimenti guardati (solo proprietà generali, nessun modello preso): cinghiali e facoceri veri (profilo a cuneo,
disco del grugno, labbro gonfio sulle zanne, verruche, criniera dorsale, scudo delle spalle), sculture di uomini
cinghiale e orchi-cinghiale su Sketchfab e ArtStation (arcate aggrottate, bocca aperta, muscoli separati).

| parte | come nasce |
|---|---|
| corpo | campo di distanza (`sdf.py`): ellissoidi e capsule fusi con unioni morbide e **solchi** sottratti tra i muscoli (pettorali, deltoidi, bicipite e tricipite, linea alba, spina), addominali a placche, costole modulate sui fianchi (`lean`), gobba, garrese, dorsali e scapole, mani grandi nocchiute, polpacci e quadricipiti, piedi con lo zoccolo fesso |
| testa | cuneo basso e lungo con cresta sagittale; arcate aggrottate a V con il nodo di rabbia in mezzo e i solchi verticali; occhi piccoli e infossati; zigomi; giogaie; labbro gonfio sopra la zanna; disco del grugno con narici profonde; **bocca aperta nel ringhio** (`snarl`): il labbro sale davanti, la mandibola cala, si vedono gengive, lingua e denti; rughe del ringhio alla radice del grugno |
| zanne | due grandi a lama dalla mandibola (osso `Jaw`), curve in alto, in fuori e indietro; due affilatoi dal labbro |
| denti | due file di zanne corte e storte (sopra seguono `Head`, sotto `Jaw`) |
| orecchie | foglie larghe all'indietro, con i morsi sull'orlo (o mozzate), ossa `LeftEar` / `RightEar` |
| setole | cresta irta dalla fronte alla schiena, mantello sulla gobba e sulle spalle, ciuffi su guance e mento, ciuffo della coda |
| artigli | uncini lunghi, neri, sporchi di sangue |

Le ossa sono quelle del gioco (`mixamorig1:*`, posizioni in `rig.json`), più `Jaw`, `LeftEar`, `RightEar` e `Tail`:
le clip del gioco passano dal retarget dei modelli d'autore. I pesi del corpo sono calcolati da Blender (calore) e poi
corretti a mano: testa e mandibola sono definite dalla forma (sotto la linea della bocca = `Jaw`); gobba e collo non
prendono peso dalla testa.

### Due risoluzioni e texture cotte

Il campo si estrae a 5 mm (modello alto, ~200-400 mila facce) e da lì si decima il modello del gioco (~42 mila).
Sull'alto si dipinge per vertice:

- colore: cuoio bruno quasi nero, schiena e arti più scuri, ventre più caldo, grugno nudo grigio, disco del naso
  umido, ombra attorno agli occhi, gengive e bocca rosso scuro, lingua violacea, zoccoli neri;
- cicatrici (pelle chiara rialzata con l'orlo incavato), verruche sotto gli occhi e sul grugno, vene gonfie su
  bicipiti e avambracci;
- pitture di guerra: graffi d'ocra rossa sopra l'occhio sinistro e strisce su spalle e petto (`paint`, `paint_col`:
  cenere chiara per Kragh e Gorvakh);
- sangue rappreso: muso, mento, colature sul petto, mani e avambracci (`blood`); fango secco dai piedi alle ginocchia.

Poi si cuoce dall'alto al basso (Cycles, "selected to active"): `Skin_BaseColor` (2048), `Skin_Normal` (2048: forma
alta + cicatrici, vene, verruche, pori, pelo ispido e pieghe del cuoio fotografato `fabric_leather_02`, Poly Haven
CC0) e `Skin_ORM` (1024: ruvidità; sangue e bocca più lucidi, fango più opaco). Zanne, denti, artigli e setole hanno
texture loro, fatte nello script: `Ivory_*` (radice sporca, anelli di crescita, crepe, sangue verso la punta),
`Claw_BaseColor`. Le setole restano a tinta unita (`mane_col`): il materiale "capelli" con una texture costa al gioco
centinaia di MB per modello.

Con `--textures <cartella>` lo script copia tutte le texture anche in `<cartella>/vrepighs_<variante>/` (PNG).

Materiali, che il gioco riconosce dal nome:

- `Skin`, `Skin_Ears`, `Skin_Tail`: pelle, con la luce sotto la superficie;
- `Fur_Bristle`: setole;
- `Tusk_Ivory`: zanne e denti;
- `Eye`: occhi;
- `Claw_Horn`: artigli.

Per la forgia delle vesti conta solo `Skin`: orecchie, coda, setole, zanne, denti e artigli sono parti aggiunte e non
reggono le vesti (`GameLayer::RegisterGearBody`).

## Le varianti (una per unità)

Ogni unità Vrepigh ha il suo modello, costruito con lo stesso scheletro: cambia solo la carne. La taglia in partita resta
quella del `model_scale` nel JSON dell'unità.

| unità | variante | com'è |
|---|---|---|
| `vrepighs_tusker` (Vrepigh) | base, `model.glb` | il soldato tipo |
| `vrepighs_raider` (Razziatore) | `variants/vrepighs_raider.glb` | asciutto e nervoso, cresta di setole alta, cuoio rossiccio |
| `vrepighs_gunner` (Sparammazza) | `variants/vrepighs_gunner.glb` | tarchiato, avambracci grossi, una zanna scheggiata, un orecchio mozzato |
| `vrepighs_feral_pig` (Feral Pig) | `variants/vrepighs_feral_pig.glb` | giovane e magro, testa grande, criniera arruffata, zanne appena spuntate, striature da cucciolo |
| `vrepighs_kragh` (Kragh) | `variants/vrepighs_kragh.glb` | un muro di muscoli, gobba enorme, zanne grosse, cicatrici |
| `vrepighs_gorvakh` (Gorvakh) | `variants/vrepighs_gorvakh.glb` | enorme e quasi nero, zanne da ariete arrotolate, giogaie pesanti, cicatrici ovunque |
| `vrepighs_zhelvar` (Zhelvar, il Vecchio) | `variants/vrepighs_zhelvar.glb` | pancia che cade, zanne lunghissime (una spezzata), criniera e grugno grigi, orecchie che ricadono |

I numeri delle varianti sono in `variants.py`. Il gioco carica `variants/<id unità>.glb` accanto al modello del corpo:
anima lo scheletro del corpo e disegna la variante (`GameLayer::LoadBodyVariants`). Ogni variante ha anche le sue vesti
in `GameData/Gear/<id unità>/`, che la forgia costruisce come per il corpo base.

## Rifarlo

```
pip install bpy numpy scipy scikit-image pillow
python Tools/ModelForge/vrepigh/build.py                      # modello base (Vrepigh)
python Tools/ModelForge/vrepigh/build.py --variant gorvakh    # una variante
python Tools/ModelForge/vrepigh/build.py --fast                # prova veloce (campo a 7 mm, texture 1024)
```
Il modello base va fatto per primo: scrive `rig_extra.json` (posizione delle ossa della mascella, delle orecchie e della
coda), che le varianti riusano.

Lo script scrive `model.glb` nella cartella del gioco (oppure dove indica `--out <file>`) e lascia qui
`vrepigh_<variante>.blend`, utile per ritoccarlo a mano. La forma già calcolata resta in `shape_<variante>.npz` e si rifà da sola quando cambiano i numeri della variante o `sdf.py`. Per un'anteprima rapida:

```
python Tools/ModelForge/vrepigh/check.py --pose --blend Tools/ModelForge/vrepigh/vrepigh_tusker.blend anteprima.png
```

Dopo ogni modifica al corpo bisogna rifare le vesti e le armature:

1. esporta i corpi dal gioco con `SUN_GEAR_DUMP`;
2. lancia `Tools/GearForge/forge.py --only vrepigh,vrepighs_raider,...` (le varianti si chiamano come l'unità; vedi il
   README della forgia).
