id: delight
kind: howto
title: Tirar a luz de uma textura escaneada | De-lighting a scanned texture | Quitar la luz de una textura escaneada (De-light)
keys: delight, de-light, de-lighting, tirar a luz, tirar sombra, remover luz da textura, remove baked light, fotogrametria, photogrammetry, scan, textura escaneada, scanned texture, sombra assada na textura, sombra assada, baked shadow, albedo limpo, clean albedo, polycam, reality capture, metashape
--- pt
```diagram steps
Importe a malha escaneada com a textura de cor
Selecione o ator na cena
Ferramentas > Tirar a Luz da Textura do Ator Selecionado
A engine grava <textura>_delit.png e troca no material
```
Uma malha de **fotogrametria** traz na textura a luz do dia em que foi fotografada (um lado mais claro, sombras, cantos escuros). Isso briga com a luz da cena. O **de-lighting** tira essa luz para a cor ficar só o material.
- **Como usar:** selecione o ator da malha e use **Ferramentas > Tirar a Luz da Textura do Ator Selecionado**. A textura ligada à **Base Color** (uma Image Texture) é processada e o material passa a usar a cópia `_delit.png` (a original fica).
- **Como funciona:** as normais da malha são desenhadas no espaço UV da textura; uma iluminação suave (harmônicos esféricos) é ajustada ao brilho e dividida fora; depois o que sobra de sombra larga sai pela metade. A cor (matiz) é mantida.
- **Sem malha** (a malha sem UV que cubra a textura): só a parte da sombra larga.
- A barra de status mostra quanto a luz variava antes e depois.
--- en
```diagram steps
Import the scanned mesh with its color texture
Select the actor in the scene
Tools > De-light Selected Actor's Texture
The engine writes <texture>_delit.png and swaps it in the material
```
A **photogrammetry** mesh carries in its texture the light of the day it was shot (one side brighter, shadows, dark corners). That fights the scene's light. **De-lighting** takes that light out so the color is only the material.
- **How to use:** select the mesh's actor and use **Tools > De-light Selected Actor's Texture**. The texture in the **Base Color** (an Image Texture) is processed and the material switches to the `_delit.png` copy (the original stays).
- **How it works:** the mesh's normals are drawn into the texture's UV space; a soft lighting (spherical harmonics) is fitted to the brightness and divided out; then what is left of the broad shading goes out by half. The color (hue) is kept.
- **Without a mesh** (a mesh whose UVs don't cover the texture): only the broad shading part.
- The status bar shows how much the light varied before and after.
--- es
```diagram steps
Importa la malla escaneada con su textura de color
Selecciona el actor en la escena
Herramientas > De-light Selected Actor's Texture
El motor escribe <textura>_delit.png y la cambia en el material
```
Una malla de **fotogrametría** lleva en su textura la luz del día en que se fotografió (un lado más brillante, sombras, rincones oscuros). Eso choca con la luz de la escena. El **De-lighting** quita esa luz para que el color sea solo el material.
- **Cómo usarlo:** selecciona el actor de la malla y usa **Herramientas > De-light Selected Actor's Texture**. Se procesa la textura del **Base Color** (una Image Texture) y el material pasa a la copia `_delit.png` (el original se conserva).
- **Cómo funciona:** las normales de la malla se dibujan en el espacio UV de la textura; se ajusta una iluminación suave (armónicos esféricos) al brillo y se divide; después, lo que queda del sombreado amplio se reduce a la mitad. El color (el tono) se conserva.
- **Sin malla** (una malla cuyas UV no cubren la textura): solo la parte del sombreado amplio.
- La barra de estado muestra cuánto variaba la luz antes y después.
