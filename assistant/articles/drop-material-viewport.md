id: drop-material-viewport
kind: howto
title: Arrastar material ou textura para um objeto na viewport | Drag a material or texture onto an object in the viewport | Soltar un material en la ventana gráfica
keys: arrastar material, drag material, soltar material, drop material, aplicar material, apply material, material no objeto, material on object, arrastar textura para objeto, drag texture onto object, destacar objeto, highlight object, onde vai aplicar, where it applies, trocar material na viewport, change material in viewport
--- pt
```diagram steps
Arraste o material (ou a textura) do Content Browser | Passe sobre a viewport | O objeto que vai receber fica contornado, com o nome | Solte: aplicado (Ctrl+Z desfaz)
```
## Como funciona
- Arraste um **Material** ou **Material Instance** do Content Browser sobre a viewport: a malha sob o mouse fica **contornada** e a dica diz "Aplicar M_X em Objeto". Solte para aplicar; **Ctrl+Z** desfaz.
- Uma **textura** funciona igual: ao soltar, a engine usa o material `M_<textura>` ao lado dela (ou cria um na hora, já com as texturas irmãs, como normal e roughness, ligadas pelo nome) e aplica.
- Fora de uma malha, a dica some e nada é aplicado.
--- en
```diagram steps
Drag the material (or the texture) from the Content Browser | Move over the viewport | The object that will take it is outlined, named | Drop: applied (Ctrl+Z undoes it)
```
## How it works
- Drag a **Material** or **Material Instance** from the Content Browser over the viewport: the mesh under the mouse is **outlined** and the tip says "Apply M_X to Object". Drop it to apply; **Ctrl+Z** undoes it.
- A **texture** works the same: dropped, the engine uses the `M_<texture>` material beside it (or makes one at once, its sibling textures such as normal and roughness already linked by name) and applies it.
- Off a mesh, the tip goes away and nothing is applied.
--- es
```diagram steps
Arrastra el material (o la textura) desde el Explorador de contenidos | Muévelo sobre la ventana gráfica | El objeto que lo recibirá se resalta, con su nombre | Suelta: aplicado (Ctrl+Z lo deshace)
```
## Cómo funciona
- Arrastra un **Material** o **Material Instance** del Explorador de contenidos sobre la ventana gráfica: la malla bajo el ratón queda **resaltada** y la sugerencia dice "Apply M_X to Object". Suéltalo para aplicarlo; **Ctrl+Z** lo deshace.
- Una **textura** funciona igual: al soltarla, el motor usa el material `M_<textura>` que está a su lado (o crea uno al instante, con sus texturas hermanas como normal y roughness ya enlazadas por el nombre) y lo aplica.
- Fuera de una malla, la sugerencia desaparece y no se aplica nada.
