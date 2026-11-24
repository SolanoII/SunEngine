# Arsenale

Pagina-catalogo di tutte le armi, degli attrezzi e delle unità che li portano, con la foto del modello 3D e le
statistiche dei file di gioco. È pubblicata come artifact "Arsenale di SunCollector" e va rigenerata a ogni modifica
o aggiunta di armi, unità o equipaggiamento.

```
pip install bpy numpy pillow
python Tools/Arsenale/build.py            # scrive Tools/Arsenale/out (arsenale.html, data.json, img/)
```

- Legge `Sandbox/GameData/Weapons/*/*.json`, `Units/*.json` (dotazione base) e `Models/Weapons/<id>.glb`.
- Le foto si rifanno solo per i modelli cambiati.
- `out/` non va nel repository.
- La pagina (`arsenale.html`) carica `data.json` e `img/` accanto a sé.
