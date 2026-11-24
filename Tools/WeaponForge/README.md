# Forgia delle armi (WeaponForge)

Script di Blender che trasforma ogni modello a blocchi delle armi e degli attrezzi
(`Sandbox/GameData/BlockModels/<id>.json`) in una mesh vera, `Sandbox/GameData/Models/Weapons/<id>.glb`. Il gioco
disegna la mesh al posto dei cubi, mentre il modello a blocchi resta per l'ingombro, gli appoggi delle mani e la bocca
della canna. Se una mesh manca, l'arma torna a cubi.

## Cosa fa

- **Forma.** Ogni blocco diventa un pezzo con la forma che gli spetta:
  - canne, anelli, celle, spolette, pomoli e rivetti diventano rotondi;
  - le lame si assottigliano verso il filo;
  - denti e punte diventano cunei;
  - impugnature e calci si arrotondano;
  - tutto il resto viene smussato.
- **Materiale.** Si ricava dal nome e dal colore del blocco: ferro brunito, acciaio delle lame, legno scuro, cuoio,
  ottone ossidato, vernice di fazione (spenta e scrostata) e celle d'energia luminose. I colori accesi dei blocchi
  servono solo a capire il materiale: nella mesh non restano.
- **Usura cotta in texture** (colore, ruvidità, metallo) con Cycles:
  - spigoli lucidati dall'uso;
  - sporco negli incavi;
  - graffi;
  - ruggine, anche al posto delle "toppe" di ruggine dei blocchi;
  - polvere sulle superfici rivolte in alto;
  - venatura del legno.

Il materiale `Weapon` è metallo per il motore, `Weapon_Glow` è la parte luminosa.

## Seconda forgia: armi modellate (forge2.py)

`forge2.py` non parte più dai cubi. Ogni arma viene costruita da un archetipo (`arch2.py` o `arch.py`) con gli
attrezzi di `kit.py`:
profili estrusi, pezzi torniti, fori e feritoie (booleane), rivetti, filo avvolto e punte saldate. Le misure e i punti
d'appoggio (mani, bocca della canna) vengono dal modello a blocchi, quindi in gioco tutto resta dov'era.

- **Armi da fuoco e a energia (`arch2.py`): armi da guerra, non tubi.** Le sagome sono massicce:
  - castelli alti e squadrati con smussi, pannelli incassati, feritoie e piastre imbullonate o saldate;
  - camicie di raffreddamento e freni di bocca enormi;
  - caricatori spessi o a tamburo, calci pieni;
  - baionette seghettate, lame da sega e punte.
- **Fucili:** automatico, pesante, di precisione (ottica e bipiede), a pompa, mitragliatrice con cassetta e nastro.
  La Vanguardia ha mirini elettronici e un lanciagranate sotto la canna.
- **Pistole:** cannoni da mano con compensatore, revolver enorme con sottocanna, a energia con cavi, doppia canna di
  rottami.
- **Energia:**
  - impulso: dissipatori a lamelle;
  - scarica: bobine di rame, isolatori di ceramica e punte a forca;
  - fusione: camere gemelle;
  - raggio dei Vrepigh.

  Tutte hanno il capacitore col vetro acceso sopra il castello.
- **Altri archetipi (`arch.py`):** lanciafiamme, lanciagranate, lanciarazzi, archibugio.
  - Armi bianche: coltello da trincea, mannaia (anche pesante), spada a denti, chiave.
  - Equipaggiamento: granate, mine, bomba da demolizione, medikit, radio, rilevatore, zaino energetico, mantello,
    casse e zaini di munizioni.
- **Stile per fazione** (dal prefisso dell'id):
  - Regno (stile trincea): acciaio rivettato, noce massiccio, ottone, tamburi, baionette seghettate.
  - Vanguardia: gusci d'acciaio lavorati a macchina, piastre rosso sangue, strisce di pericolo gialle e nere.
  - Vrepigh: lamiere sovrapposte e saldate, punte, lame da sega, ruggine.
- **Texture cotte con Cycles.**
  - BaseColor, ORM e normal map.
  - Usura: spigoli consumati (rilevati col nodo Bevel), sporco negli incavi, polvere, graffi.
  - Ruggine in quantità diversa per ogni materiale.
  - Rilievo: venatura del legno, butterature, lamiera martellata, grana del cuoio.

Barricate, muri, pilastri, basi e stendardi non hanno archetipo e restano quelli della v1.

```
python Tools/WeaponForge/forge2.py                                        # tutte (texture 1024, 512 per le piccole)
python Tools/WeaponForge/forge2.py --only footman_auto_rifle --preview anteprime/
```

## Rifarla (v1)

```
pip install bpy numpy
python Tools/WeaponForge/forge.py                              # tutte le armi (circa un'ora)
python Tools/WeaponForge/forge.py --only footman_auto_rifle    # una sola
python Tools/WeaponForge/forge.py --only tusker_cleaver --preview anteprime/
```

Va rilanciata quando cambia un modello a blocchi o se ne aggiunge uno. Le texture sono a 512 px per le armi lunghe e
256 px per quelle piccole; `--size 1024` le raddoppia.

## Nel gioco

- **Armi in mano.** Seguono le leggi dell'impugnatura (`Engine/ECS/WeaponHold.h`), poi vengono spinte fuori dal corpo,
  così niente canne nel petto, nella gobba o nella testa (`Engine/Renderer/BodyGear.h`).
- **Tutto il resto si vede addosso** (`Sandbox/GameLayerGear.cpp`):
  - fucile di traverso sulla schiena;
  - pistola nella fondina sulla coscia destra;
  - arma bianca sul fianco sinistro (quelle lunghe di traverso sulla schiena);
  - granate e piccoli attrezzi alla cintura;
  - casse e zaini sulla schiena.

  Muri, barricate e stendardi non si portano addosso.
- **Sfoderare richiede tempo.** Il tempo dipende da dove sta l'arma (fondina 0,32 s, fianco 0,38 s, schiena 0,62 s,
  zaino 0,8 s), dal peso (`weight_kg`, 0,075 s per kg) e dalla forza dell'unità. Finché l'arma non è arrivata in mano
  non si spara. L'arma parte dal suo posto sul corpo e arriva in mano con un arco del braccio; le mani la seguono.
