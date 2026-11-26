# Corpo umano d'autore (Regno e aspiranti)

Anatomia vera al posto del manichino senza volto: base di **MakeHuman** (asset CC0, nessun obbligo di licenza),
adattata allo scheletro del gioco (`footman_master.fbx`), con occhi e pelle cotti in texture.

```
python3 prepare_mh.py <makehuman/data>    # una volta: mh_body.npz (maschio giovane muscoloso, articolazioni)
python3 build.py --copy-candidate         # Models/Characters/footman/ e candidate/ (model.glb + model.json)
python3 build.py --fast                   # senza cottura, per provare le forme
```

- `build.py`: scheletro provvisorio sulle articolazioni di MakeHuman, pesi dal calore di Blender, poi ogni osso di
  braccia, mani e gambe porta la sua pelle dalla posa A alla posa T del gioco (busto e testa restano come sono).
  Stessi pesi sullo scheletro vero; occhi sull'osso della testa.
- `skin.py`: si cuociono posizione, normale e occlusione di ogni texel, poi numpy disegna capelli rasati,
  sopracciglia, barba corta, labbra, rossori, occhiaie, rughe della fronte, pori e sporco di trincea. Il colore e' una
  mappa di dettaglio: il gioco la moltiplica per la carnagione del garage (`"tint": true` in `model.json`).
- Occhi: sclera con capillari, iride a raggi, pupilla, cornea lucida.

Dopo ogni modifica del corpo vanno rifatte le vesti (vedi `Tools/GearForge/README.md`): `SUN_GEAR_DUMP`, poi
`forge.py --only footman,candidate`.
