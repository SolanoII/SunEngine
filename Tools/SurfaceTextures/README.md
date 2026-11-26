# Texture di dettaglio delle superfici

`Sandbox/GameData/Textures/Surfaces/<nome>_{albedo,normal,data}.jpg`: dettaglio che il motore stende sopra i
materiali delle mappe, delle vesti, delle armature e delle armi (vedi `DeferredRenderer::LoadSurfaceDetail`).

- `scarica_foto.py` (predefinito): materiali scansionati CC0 di Poly Haven, convertiti nel formato del motore.
  `python3 Tools/SurfaceTextures/scarica_foto.py Sandbox/GameData/Textures/Surfaces` (crediti in `CREDITI.md`)
- `genera_texture.py`: le vecchie texture generate, se si vuole tornare indietro o lavorare senza rete.
