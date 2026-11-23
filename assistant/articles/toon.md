id: toon
kind: howto
title: Toon e contorno: visual de desenho animado (cel shading, Auto Adaptive Outline) | Toon and outline: the cartoon look (cel shading, Auto Adaptive Outline)
keys: toon, cel, cel shading, cell shading, anime, desenho animado, cartoon, dibujos animados, estilizado, stylized, estilizado, faixas, bands, bandas, rampa, ramp, rampa de tons, tone ramp, sombra colorida, shadow color, color de sombra, luz de borda, rim light, rim, luz de contorno, brilho estilizado, stylized highlight, specular toon, hachura, hatching, sombreado con lineas, contorno, outline, contorno adaptativo, auto adaptive outline, linha de contorno, line art, inked, tinta, silhueta, silhouette, largura do contorno, outline width, distancia, distance, forcar toon, force toon, nunca toon, never toon, luz toon, toon light, M_Toon, M_ToonRamp, M_ToonHatch, M_ToonSkin, MI_ToonBlue, Set Toon Parameter, Set Toon Color, Set Actor Outline, Set Light Toon Mode, REALENGINE_NO_OUTLINE, REALENGINE_NO_TOON
--- pt
```diagram steps
Material: Details > Modelo de Sombreamento = Toon
Faixas, Cor da Sombra, Rampa (pontos ou imagem)
Brilho, Luz de Borda, Hachura, Ambiente do Toon
Contorno: Desenhar Contorno, Cor e Largura da Linha, Auto Adaptativo
Ator: Details > Contorno do Ator; Luz: Details > Luz Toon
```
```blueprint
Event BeginPlay
  -> Set Toon Parameter ( Target = self, Parameter = "Bands", Value = 3 )
  -> Set Toon Color ( Target = self, Parameter = "Shadow Color", Color = Make Vector( 0.3, 0.2, 0.5 ) )
  -> Set Actor Outline ( Target = self, Enabled = true, Color = Make Vector( 1, 0.1, 0.05 ), Width = 4 )
```
O **Toon** dá o visual de desenho animado: a luz em **faixas chapadas** em vez de um degradê, o lado escuro numa **cor de sombra** própria e um **contorno** desenhado em volta dos objetos.
1. No material, **Details > Modelo de Sombreamento = Toon**. **Faixas do Toon**: 2 é o cel clássico (luz e sombra); 3 ou 4 dão meios-tons. **Suavidade do Toon** amacia a borda de cada faixa; **Limite da Sombra** move onde a sombra começa.
2. **Cor da Sombra** é para onde o lado escuro vai (vezes a cor base). **Cor da Sombra Recebida** pinta onde outro objeto tapa a luz (sombra projetada), separada do lado escuro.
3. A **rampa**: até 4 **pontos** (posição e cor) do escuro ao iluminado, ou uma **Imagem da Rampa** lida da esquerda para a direita (uma rampa pintada).
4. **Brilho** (cor, força, tamanho, dureza): um ponto de luz duro. **Luz de Borda**: a borda rasante acesa no lado que a luz alcança. **Hachura**: linhas na tela onde a luz é baixa (cruzadas onde é mais escuro), firmes mesmo com o TSR. **Ambiente do Toon**: quanto da luz indireta entra (menos = faixas mais chapadas).
5. **Contorno** (em qualquer modelo de sombreamento): **Desenhar Contorno**, **Cor da Linha** e **Largura da Linha** (px a 1440p). **Auto Adaptativo**: a largura segue a distância ao longo da própria silhueta (perto fino, longe grosso), pela **Escala Perto/Longe** e a **Curva da Largura** entre a **Distância Perto** e a **Distância Longe**. **Sensibilidade da Profundidade** desenha linhas dentro do ator (um braço na frente do corpo); **Ângulo da Dobra** desenha as arestas.
6. Uma **Material Instance** pode ter os seus: **Substituir Toon e Contorno**. Um **ator** pode ter o próprio contorno: **Details > Contorno do Ator** (Ligado, cor, largura). Uma **luz** escolhe em **Details > Luz Toon**: Como o material, **Forçar Toon** (todo material num degrau cel) ou **Nunca Toon** (luz de preenchimento suave).
- No jogo: **Set Toon Parameter** e **Set Toon Color** (por nome, em inglês ou português; o ator ganha a própria instância, sem bake), **Get Toon Parameter**, **Set Actor Outline** (o item que dá para pegar, o inimigo na mira), **Set Light Toon Mode**.
- Exemplos em Materials/Examples/Toon: **M_Toon**, **M_ToonRamp**, **M_ToonHatch**, **M_ToonSkin**, **M_ToonGround** e **MI_ToonBlue**. Cena de teste: `toon`.
- Custo (RTX 3060, cena `toon`, Épico com TSR, render 1599x992): o contorno 0,12 ms de GPU (~0,28 ms a 1440p nativo), o toon no passe principal o mesmo das variantes RE_EXT. `REALENGINE_NO_OUTLINE=1` e `REALENGINE_NO_TOON=1` para comparar. Veja o custo no Debug Viewer (F10) > Toon Outline.
- Veja também: [Materiais](article:materials), [Estilos de render](article:render-styles), [Luzes](article:lights).
--- en
```diagram steps
Material: Details > Shading Model = Toon
Bands, Shadow Color, Ramp (points or an image)
Highlight, Rim Light, Hatching, Toon Ambient
Outline: Draw Outline, Line Color and Width, Auto Adaptive
Actor: Details > Actor Outline; Light: Details > Toon Light
```
```blueprint
Event BeginPlay
  -> Set Toon Parameter ( Target = self, Parameter = "Bands", Value = 3 )
  -> Set Toon Color ( Target = self, Parameter = "Shadow Color", Color = Make Vector( 0.3, 0.2, 0.5 ) )
  -> Set Actor Outline ( Target = self, Enabled = true, Color = Make Vector( 1, 0.1, 0.05 ), Width = 4 )
```
**Toon** gives the cartoon look: the light in **flat bands** instead of a gradient, the dark side in a **shadow color** of its own and an **outline** drawn around the objects.
1. In the material, **Details > Shading Model = Toon**. **Toon Bands**: 2 is the classic cel (light and shadow); 3 or 4 add mid tones. **Toon Softness** softens each band's edge; **Shadow Threshold** moves where the shadow starts.
2. **Shadow Color** is where the dark side goes (times the base color). **Received Shadow Color** tints where another object hides the light (cast shadows), apart from the dark side.
3. The **ramp**: up to 4 **points** (position and color) from dark to lit, or a **Ramp Image** read from left to right (a painted ramp).
4. **Highlight** (color, strength, size, hardness): a hard spot of light. **Rim Light**: the grazing edge lit on the side a light reaches. **Hatching**: lines on the screen where the light is low (crossed where darker), steady under TSR too. **Toon Ambient**: how much indirect light comes in (less = flatter bands).
5. **Outline** (any shading model): **Draw Outline**, **Line Color** and **Line Width** (px at 1440p). **Auto Adaptive**: the width follows the distance along the silhouette itself (thin near, thick far), by the **Near/Far Width Scale** and the **Width Curve** between the **Near Distance** and the **Far Distance**. **Depth Sensitivity** draws lines inside the actor (an arm in front of the body); **Crease Angle** draws the edges.
6. A **Material Instance** may have its own: **Override Toon and Outline**. An **actor** may have its own outline: **Details > Actor Outline** (On, color, width). A **light** picks in **Details > Toon Light**: As the material, **Force Toon** (every material in a cel step) or **Never Toon** (a soft fill light).
- In the game: **Set Toon Parameter** and **Set Toon Color** (by name, in English or Portuguese; the actor gets its own instance, no bake), **Get Toon Parameter**, **Set Actor Outline** (the item you can pick up, the enemy in your sights), **Set Light Toon Mode**.
- Examples in Materials/Examples/Toon: **M_Toon**, **M_ToonRamp**, **M_ToonHatch**, **M_ToonSkin**, **M_ToonGround** and **MI_ToonBlue**. Test scene: `toon`.
- Cost (RTX 3060, the `toon` scene, Epic with TSR, a 1599x992 render): the outline 0.12 ms of GPU (~0.28 ms at native 1440p); the toon in the main pass the same as the RE_EXT variants. `REALENGINE_NO_OUTLINE=1` and `REALENGINE_NO_TOON=1` to compare. See its cost in the Debug Viewer (F10) > Toon Outline.
- See also: [Materials](article:materials), [Render styles](article:render-styles), [Lights](article:lights).
--- es
**Toon** da el aspecto de dibujos animados: la luz en **bandas planas**, el lado oscuro en un **color de sombra** propio y un **contorno** alrededor de los objetos. En el material: **Details > Modelo de Sombreado = Toon** (bandas, color de sombra, rampa de puntos o imagen, brillo, luz de borde, sombreado con líneas). **Contorno** en cualquier material: color, ancho en px a 1440p y **Auto Adaptive** (el ancho sigue la distancia: fino cerca, grueso lejos). Cada actor puede tener su propio contorno (**Details > Contorno del Actor**) y cada luz su modo (**Forzar Toon**, **Nunca Toon**). En el juego: Set Toon Parameter, Set Toon Color, Set Actor Outline, Set Light Toon Mode. Ejemplos en Materials/Examples/Toon; escena de prueba `toon`.
