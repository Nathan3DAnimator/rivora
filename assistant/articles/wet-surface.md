id: wet-surface
kind: howto
title: Superfície molhada e suor (Wet Surface) | Wet surfaces and sweat (Wet Surface) | Superficie mojada y sudor
keys: molhado, superficie molhada, shading molhado, wet, wet surface, wet shading, wetness, suor, sweat, suado, sweaty, pele suada, sweaty skin, gotas, droplets, gotas escorrendo, running drops, pelicula de agua, water film, porosidade, porosity, encharcado, soaked, umido, damp, chuva no material, rain on material, molhar personagem, wet character, M_WetConcrete, M_SweatySkin, REALENGINE_NO_WET
--- pt
```diagram steps
Principled BSDF (o material seco)
Wet Surface (Shader: molhado, película, gotas, suor)
Material Output (Surface)
Blueprint muda o Molhado/Suor durante o jogo
```
O nó **Wet Surface** (Superfície Molhada, categoria Shader no editor de material) deixa qualquer material molhado, com ou sem chuva: concreto depois da chuva, uma roupa encharcada, a pele suada de quem corre.
1. Abra o material, **Adicionar > Shader > Wet Surface** e ligue-o entre o **Principled BSDF** e o **Material Output** (BSDF → Shader, Shader → Surface).
2. Ajuste: **Wetness** (quanto molhado), **Porosity** (−1: automático pela rugosidade; poroso fica mais escuro, liso fica com a água por cima), **Water Film** e **Film Roughness** (a película d'água que espelha), **Droplets** e **Droplet Size** (gotas paradas, mm), **Running** (gotas escorrendo e deixando rastro), **Sweat** (suor: um brilho oleoso, escurece pouco, gotinhas mais rasas).
3. **Máscara**: Tudo, Cor do Vértice (R) (pinte onde fica molhado), Virado para Cima (onde a água cai) ou Manchas.
4. **Durante o jogo:** ligue um nó **Value** com **Parameter Name** (ex.: "Sweat") no pino. O Grafo **Set Scalar Parameter Value on Materials** muda o valor na hora, **sem recompilar** o material (a GPU lê o parâmetro direto).
```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Sweat
       Target = self, Parameter Value = Vector Length ( Get Velocity ) / 600
```
- As gotas andam com o objeto (ficam no espaço dele) e escorrem para baixo do mundo. Com chuva, o molhado da chuva soma por cima.
- Exemplos prontos em Materials/Examples: **M_WetConcrete** e **M_SweatySkin** (com o parâmetro Sweat). Cena de teste: `materials` (fileira da frente).
- Custo (RTX 3060, 1440p Épico): ~0,4 ms com a superfície molhada cobrindo boa parte da tela; zero para quem não usa. No preset Baixo (sem as variantes de lóbulos) o material aparece seco. Para comparar: `REALENGINE_NO_WET=1`.
- Veja também: [Materiais](article:materials), [Shading da chuva](article:rain-shading).
--- en
```diagram steps
Principled BSDF (the dry material)
Wet Surface (Shader: wetness, film, drops, sweat)
Material Output (Surface)
A Blueprint changes Wetness/Sweat during play
```
The **Wet Surface** node (Shader category in the material editor) makes any material wet, rain or not: concrete after the rain, soaked clothes, the sweaty skin of someone running.
1. Open the material, **Add > Shader > Wet Surface** and put it between the **Principled BSDF** and the **Material Output** (BSDF → Shader, Shader → Surface).
2. Adjust: **Wetness** (how wet), **Porosity** (−1: automatic by the roughness; porous goes darker, smooth keeps the water on top), **Water Film** and **Film Roughness** (the mirror-like water film), **Droplets** and **Droplet Size** (drops that stay, mm), **Running** (drops running down and leaving a trail), **Sweat** (an oily sheen: little darkening, flatter beads).
3. **Mask**: Everywhere, Vertex Color (R) (paint where it is wet), Facing Up (where water lands) or Patches.
4. **During play:** link a **Value** node with a **Parameter Name** (e.g. "Sweat") to the pin. The Graph **Set Scalar Parameter Value on Materials** changes it at once, **without recompiling** the material (the GPU reads the parameter directly).
```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Sweat
       Target = self, Parameter Value = Vector Length ( Get Velocity ) / 600
```
- The drops move with the object (they live in its space) and run down the world's down. In the rain, the rain's wetness adds on top.
- Ready examples in Materials/Examples: **M_WetConcrete** and **M_SweatySkin** (with the Sweat parameter). Test scene: `materials` (the front row).
- Cost (RTX 3060, 1440p Epic): ~0.4 ms with the wet surface over much of the screen; nothing for whoever doesn't use it. On the Low preset (without the lobes' variants) the material shows dry. To compare: `REALENGINE_NO_WET=1`.
- See also: [Materials](article:materials), [Rain shading](article:rain-shading).
--- es
```diagram steps
Principled BSDF (el material seco)
Wet Surface (Shader: humedad, película, gotas, sudor)
Material Output (Surface)
Un Blueprint cambia Wetness/Sweat durante el juego
```
El nodo **Wet Surface** (categoría Shader en el editor de materiales) moja cualquier material, llueva o no: concreto después de la lluvia, ropa empapada, la piel sudada de quien corre.
1. Abre el material, **Añadir > Shader > Wet Surface** y colócalo entre el **Principled BSDF** y el **Material Output** (BSDF → Shader, Shader → Surface).
2. Ajusta: **Wetness** (cuán mojado), **Porosity** (−1: automática según la roughness; lo poroso se oscurece, lo liso mantiene el agua encima), **Water Film** y **Film Roughness** (la película de agua como espejo), **Droplets** y **Droplet Size** (gotas que se quedan, mm), **Running** (gotas que corren hacia abajo y dejan un rastro), **Sweat** (un brillo aceitoso: poco oscurecimiento, perlas más planas).
3. **Mask**: Everywhere, Vertex Color (R) (pinta dónde está mojado), Facing Up (donde cae el agua) o Patches.
4. **Durante el juego:** conecta un nodo **Value** con un **Parameter Name** (p. ej. "Sweat") al pin. El nodo de Grafo **Set Scalar Parameter Value on Materials** lo cambia al instante, **sin recompilar** el material (la GPU lee el parámetro directamente).
```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Sweat
       Target = self, Parameter Value = Vector Length ( Get Velocity ) / 600
```
- Las gotas se mueven con el objeto (viven en su espacio) y corren hacia el abajo del mundo. Bajo la lluvia, la humedad de la lluvia se suma encima.
- Ejemplos listos en Materials/Examples: **M_WetConcrete** y **M_SweatySkin** (con el parámetro Sweat). Escena de prueba: `materials` (la fila del frente).
- Costo (RTX 3060, 1440p Épico): ~0,4 ms con la superficie mojada sobre gran parte de la pantalla; nada para quien no la usa. En el preset Bajo (sin las variantes de los lóbulos) el material se ve seco. Para comparar: `REALENGINE_NO_WET=1`.
- Mira también: [Materiales](article:materials), [Shading de la lluvia](article:rain-shading).
