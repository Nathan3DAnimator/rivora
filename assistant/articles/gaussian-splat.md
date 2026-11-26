id: gaussian-splat
kind: concept
title: Cenário escaneado (Gaussian Splatting) | Scanned scenery (Gaussian Splatting) | Gaussian Splat
keys: gaussian splatting, gaussian splat, 3dgs, 3dgs ply, load a 3dgs ply file, ply file, carregar ply, arquivo ply, splat, splats, .ply, .splat, escanear, scan, escaneado, scanned scene, fotogrametria, photogrammetry, captura de celular, phone capture, luma, polycam, postshot, nuvem de gaussianas, cenario realista escaneado, REALENGINE_NO_SPLATS
--- pt
```diagram steps
Escaneie um lugar com o celular e treine (Luma, Polycam, Postshot...)
Exporte o .ply (3DGS) ou .splat e ponha em Content/Splats
Adicionar > Efeitos Visuais > Gaussian Splat, aponte o File
A GPU ordena as gaussianas a cada quadro e desenha sobre a cena
```
O **Gaussian Splat** mostra um lugar escaneado como milhões de "manchas" 3D (o método 3DGS): o atalho para um cenário fotorrealista sem modelar.
- **Como usar:** treine a captura num app (Luma, Polycam, Postshot, o 3DGS original) e exporte o **.ply** (ou um **.splat**). Ponha em `Content/Splats/` e, em **Adicionar > Efeitos Visuais > Gaussian Splat**, escreva o caminho em **File**.
- **Eixos e escala:** **Up Axis** (os treinos do 3DGS usam -Y para cima, o padrão) e **Unit** (100: o arquivo em metros). Gire, mova e escale pelo ator.
- **Luz:** **Lit by the Scene** usa a cor como albedo sob o céu e o sol da cena; **Own Light** mostra a cor como luz própria (**Brightness** em nits), bom para capturas com a luz já gravada.
- **Junto com as malhas:** o que está na frente tapa as gaussianas e elas aparecem por trás das malhas certas (a profundidade da cena).
- **Custo** (RTX 3060, Épico): ~1,4 ms com 140 mil gaussianas perto da câmera; cresce com quantas se sobrepõem na tela.
- **Exemplos:** `Splats/Example_Bush2.splat` e `Splats/Example_Flowers.ply` (cena de teste `splat`).
- **Ainda não:** sombras das gaussianas, cor que muda com o ângulo (as bandas SH acima da 0) e rasterização em tiles para cenas de milhões.
--- en
```diagram steps
Scan a place with a phone and train it (Luma, Polycam, Postshot...)
Export the .ply (3DGS) or .splat and put it in Content/Splats
Add > Visual Effects > Gaussian Splat, point File at it
The GPU sorts the Gaussians every frame and draws them over the scene
```
A **Gaussian Splat** shows a scanned place as millions of 3D "blobs" (the 3DGS method): the shortcut to photoreal scenery without modeling.
- **How to use:** train the capture in an app (Luma, Polycam, Postshot, the original 3DGS) and export the **.ply** (or a **.splat**). Put it in `Content/Splats/` and, in **Add > Visual Effects > Gaussian Splat**, type the path in **File**.
- **Axes and scale:** **Up Axis** (3DGS trainings use -Y up, the default) and **Unit** (100: the file in meters). Turn, move and scale it with the actor.
- **Light:** **Lit by the Scene** uses the color as an albedo under the scene's sky and sun; **Own Light** shows the color as its own light (**Brightness** in nits), good for captures with their light baked in.
- **With the meshes:** what is in front hides the Gaussians and they show behind the right meshes (the scene's depth).
- **Cost** (RTX 3060, Epic): ~1.4 ms with 140 thousand Gaussians near the camera; it grows with how many overlap on screen.
- **Examples:** `Splats/Example_Bush2.splat` and `Splats/Example_Flowers.ply` (test scene `splat`).
- **Not yet:** shadows from the Gaussians, color changing with the angle (the SH bands above 0) and tiled rasterization for scenes of millions.
--- es
```diagram steps
Escanea un lugar con el teléfono y entrénalo (Luma, Polycam, Postshot...)
Exporta el .ply (3DGS) o .splat y ponlo en Content/Splats
Añadir > Visual Effects > Gaussian Splat, apunta File hacia él
La GPU ordena las gaussianas en cada cuadro y las dibuja sobre la escena
```
Un **Gaussian Splat** muestra un lugar escaneado como millones de "manchas" 3D (el método 3DGS): el atajo hacia un escenario fotorrealista sin modelar.
- **Cómo usarlo:** entrena la captura en una aplicación (Luma, Polycam, Postshot, el 3DGS original) y exporta el **.ply** (o un **.splat**). Ponlo en `Content/Splats/` y, en **Añadir > Visual Effects > Gaussian Splat**, escribe la ruta en **File**.
- **Ejes y escala:** **Up Axis** (los entrenamientos 3DGS usan -Y arriba, el valor por defecto) y **Unit** (100: el archivo en metros). Gíralo, muévelo y escálalo con el actor.
- **Luz:** **Lit by the Scene** usa el color como albedo bajo el cielo y el sol de la escena; **Own Light** muestra el color como luz propia (**Brightness** en nits), bueno para capturas con su luz ya incorporada.
- **Con las mallas:** lo que está delante oculta las gaussianas y ellas se ven detrás de las mallas correctas (la profundidad de la escena).
- **Costo** (RTX 3060, Épico): ~1,4 ms con 140 mil gaussianas cerca de la cámara; crece con cuántas se superponen en pantalla.
- **Ejemplos:** `Splats/Example_Bush2.splat` y `Splats/Example_Flowers.ply` (escena de prueba `splat`).
- **Aún no:** sombras de las gaussianas, color que cambia con el ángulo (las bandas SH por encima de 0) y rasterización por mosaicos para escenas de millones.
