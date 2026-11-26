id: glass-translucent
kind: howto
title: Vidro e materiais transparentes | Glass and transparent materials | Vidrio y materiales translúcidos
keys: vidro, transparente, translucido, translucent, agua, janela, opacidade, alfa, alpha, blend mode, see through, glass, oit, refracao, refraction, distorcao, distortion, lente, lens, bola de vidro, glass ball, garrafa, bottle, ior, espessura, thickness, dispersao, dispersion, prisma, prism, cristal, crystal, vidro fosco, frosted glass
--- pt
```diagram compare
 | Opaque | Masked | Translucent
Vê através | não | só onde corta | sim, parcial
Bom para | quase tudo | folhas, grades, cercas | vidro, água, fumaça
Projeta sombra | sim | sim, recortada | não
Custo | menor | pequeno | maior (passe próprio)
```
1. Abra o material (duplo clique no Content Browser).
2. Em **Modo de Mistura** escolha **Translucent**.
3. O **Alpha** do Principled BSDF vira a opacidade: 0,1-0,3 para vidro, 0,6 para água turva.
4. Salve: os atores com esse material ficam transparentes na hora.
- Vários vidros um atrás do outro misturam na ordem certa sem ordenar (OIT), com reflexos e sombras por raio nos modos Alto e Épico.
- Translúcidos não projetam sombra e não entram no ray tracing; para cercas e folhas use **Masked**, que é mais barato.
- Muitas camadas grandes na tela custam (cada camada é preenchida). 400 vidros sobrepostos ~2 ms na GPU de desenvolvimento.
- **Refração**: o vidro entorta o que está atrás pela normal e pelo **IOR** do Principled BSDF (1,5 vidro, 1,33 água). No material: **Refraction** (0 desligado, 1 real), **Refraction Thickness** (quanto a vista atravessa: vidraça ~1 cm, copo ~5, lente ou bloco mais) e **Dispersion** (as cores se separam como num prisma). Vidro áspero (Roughness alta) fica **fosco**. Custo ~0,1 ms.
- Veja também: [Materiais](article:materials), [Material e textura](article:material-vs-texture).
--- en
```diagram compare
 | Opaque | Masked | Translucent
See through | no | only where cut | yes, partly
Good for | almost everything | leaves, grates, fences | glass, water, smoke
Casts shadows | yes | yes, cut out | no
Cost | lowest | small | highest (its own pass)
```
1. Open the material (double-click it in the Content Browser).
2. In **Blend Mode** pick **Translucent**.
3. The Principled BSDF's **Alpha** becomes the opacity: 0.1-0.3 for glass, 0.6 for murky water.
4. Save: the actors with that material turn transparent at once.
- Several panes behind each other blend in the right order without sorting (OIT), with ray-traced reflections and shadows on High and Epic.
- Translucent surfaces cast no shadow and stay out of ray tracing; for fences and leaves use **Masked**, which is cheaper.
- Many large layers on screen cost (each layer is filled). 400 overlapping panes take ~2 ms on the development GPU.
- **Refraction**: glass bends what is behind it by its normal and the Principled BSDF's **IOR** (1.5 glass, 1.33 water). In the material: **Refraction** (0 off, 1 real), **Refraction Thickness** (how far the view goes through it: a pane ~1 cm, a glass ~5, a lens or a block more) and **Dispersion** (the colors split as in a prism). Rough glass (high Roughness) looks **frosted**. Cost ~0.1 ms.
- See also: [Materials](article:materials), [Material and texture](article:material-vs-texture).
--- es
```diagram compare
 | Opaque | Masked | Translucent
Se ve a través | no | solo donde está recortado | sí, en parte
Bueno para | casi todo | hojas, rejillas, vallas | vidrio, agua, humo
Proyecta sombras | sí | sí, recortadas | no
Costo | el más bajo | pequeño | el más alto (su propio pase)
```
1. Abre el material (doble clic en el Explorador de contenidos).
2. En **Blend Mode** elige **Translucent**.
3. El **Alpha** del Principled BSDF pasa a ser la opacidad: 0,1-0,3 para vidrio, 0,6 para agua turbia.
4. Guarda: los actores con ese material se vuelven transparentes al instante.
- Varios cristales uno detrás de otro se mezclan en el orden correcto sin ordenar (OIT), con reflejos y sombras por trazado de rayos en Alto y Épico.
- Las superficies translúcidas no proyectan sombra y quedan fuera del trazado de rayos; para vallas y hojas usa **Masked**, que es más barato.
- Muchas capas grandes en pantalla cuestan (cada capa se rellena). 400 cristales superpuestos toman ~2 ms en la GPU de desarrollo.
- **Refracción**: el vidrio dobla lo que hay detrás según su normal y el **IOR** del Principled BSDF (1,5 vidrio, 1,33 agua). En el material: **Refraction** (0 apagada, 1 real), **Refraction Thickness** (cuánto atraviesa la vista: un cristal ~1 cm, un vaso ~5, una lente o un bloque más) y **Dispersion** (los colores se separan como en un prisma). El vidrio rugoso (Roughness alto) se ve **esmerilado**. Costo ~0,1 ms.
- Consulta también: [Materiales](article:materials), [Material y textura](article:material-vs-texture).
