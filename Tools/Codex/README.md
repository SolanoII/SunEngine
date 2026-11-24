# Codex

Il Codex raccoglie tutto quello che c'è nel gioco: unità, nemici e personaggi, armi, equipaggiamento e armature.
Per ognuno ci sono la foto, le statistiche dei file e chi può usarlo. Esiste in due forme che leggono gli stessi dati:

- **Nel gioco:** la scheda **CODEX** del menu principale (`Sandbox/GameLayerCodex.cpp`).
  - Panoramica con "a che punto siamo" e cosa manca.
  - Una categoria per tipo, con ricerca per nome.
  - Per le unità, il pulsante "Vedi in 3D" apre la vetrina.
- **Pagina web:** l'artifact "Codex di SunCollector", con le stesse schede, per vedere lo stato del gioco senza aprirlo.

## File condivisi (in GameData, vanno con il gioco)

- `Sandbox/GameData/Codex/labels.json`: nomi italiani dei campi, gruppi di statistiche e traduzioni dei valori.
  Un campo nuovo senza etichetta compare comunque in "Altro" col nome del file.
- `Sandbox/GameData/Codex/img/<id>.jpg`: le foto (720×416).

## Aggiornare

```
# 1. foto delle unità dal gioco (vestite con la dotazione base, una a testa; poi il gioco si chiude da solo)
SUN_CODEX_SHOTS=codexshots ./ProjectSunCollector

# 2. foto delle armi cambiate (Blender), ritaglio delle foto delle unità, pagina web in Tools/Codex/out
pip install bpy numpy pillow
python Tools/Codex/build.py --shots codexshots
```

Senza `--shots` si rifanno solo le foto delle armi il cui `.glb` è più nuovo della foto. `Tools/Codex/out/` non va
nel repository.
