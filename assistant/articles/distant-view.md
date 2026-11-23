id: distant-view
kind: howto
title: Visão distante: sombra do terreno, impostores e a neblina do céu | Distant view: the terrain's shadow, impostors and the sky's fog
keys: visão distante, visao distante, vista distante, distant view, vista lejana, rdr2, red dead, mirante, lookout, mirador, horizonte longe, far horizon, montanhas ao longe, far mountains, montañas a lo lejos, sombra distante, far shadow, sombra lejana, sombra do terreno, terrain shadow, sombra del terreno, sombra das montanhas, mountain shadow, vale na sombra, valley shadow, far shadow strength, impostor, impostores, impostors, octaedral, octahedral, octaédrico, cartão de árvore, tree card, billboard, árvores ao longe, far trees, árboles lejanos, floresta ao longe, far forest, bosque lejano, hlod impostor, estouro de lod, lod pop, lod popping, salto de lod, sky atmosphere ambient, atmosfera na neblina, sky atmosphere fog, perspectiva aérea distante, far aerial perspective, r.farshadow, r.impostors
--- pt
```diagram steps
Landscape: as alturas numa grade (8 m por texel) | Sol: o horizonte de cada texel na GPU (só quando o sol gira) | Além das sombras virtuais: a sombra do terreno | Longe: o impostor (cartão octaedral) no lugar da malha | A neblina distante na cor do céu
```
A **visão distante** (como a de Red Dead Redemption 2) junta três coisas que deixam quilômetros de paisagem bonitos e baratos:

1. **Sombra distante** (Detalhes da luz direcional > **Sombra Distante**, **Força da Sombra Distante**): as sombras virtuais do sol cobrem 2 km em volta da câmera (Épico). Além delas, a **sombra do terreno**: as alturas das Landscapes numa grade e, para cada texel, o quanto o terreno sobe na direção do sol (calculado na GPU só quando o sol gira). As montanhas sombreiam os vales atrás delas, até onde a vista alcança; dentro das sombras virtuais ela soma o que sobe além do alcance delas (o vale ao pé da serra no pôr do sol). Na Landscape não há nada a ligar.
2. **Impostores** (Detalhes do ator Foliage > **Impostores**): uma malha que muitos atores usam (árvores, pedras, os proxies do HLOD) é fotografada de 64 direções do hemisfério de cima em dois atlas (cor e alfa; normal, profundidade e rugosidade). Longe, cada instância vira **um cartão** de frente para a câmera que mistura as três vistas mais próximas com um passo de paralaxe pela profundidade. O cartão é iluminado como a malha (sombra do sol no ponto que ele mostra, GI, perspectiva aérea, neblina), então a troca não "estoura". A troca segue o preset: abaixo de **20 px** de raio na tela no Épico (Cinemático 16, Alto 24, Médio 28, Baixo 32). Um material **Translucent**, um esqueleto e um ator que se move ficam com a malha.
3. **Neblina na cor do céu** (Detalhes da Exponential Height Fog > **Ambiente da Atmosfera do Céu**): a luz ambiente da neblina distante vem do céu do lado para onde a vista olha (mais clara e quente para o lado do sol), não do cinza do céu inteiro, então as montanhas somem no céu atrás delas, na cor da **perspectiva aérea** (Configurações > Aerial Perspective Scale).

Pelo jogo: `r.FarShadow` (0/1), `r.FarShadow.Resolution`, `r.Impostors` (0/1), `r.Impostors.ScreenSize` (px), `r.Impostors.Resolution`, `r.Fog.SkyAtmosphere` (0..1), e as propriedades pelo nome.

```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.Impostors.ScreenSize", Value = 32 )
  -> Set Property by Name ( Property = "Far Shadow Strength", Value = 0.6 )
```
--- en
```diagram steps
Landscape: its heights in a grid (8 m a texel) | Sun: each texel's horizon on the GPU (only when the sun turns) | Past the virtual shadow maps: the terrain's shadow | Far: the impostor (an octahedral card) for the mesh | The far fog in the sky's color
```
The **distant view** (as Red Dead Redemption 2's) puts together three things that make kilometers of landscape look right and cost little:

1. **Far Shadow** (the directional light's Details > **Far Shadow**, **Far Shadow Strength**): the sun's virtual shadow maps cover 2 km around the camera (Epic). Past them, the **terrain's shadow**: the Landscapes' heights in a grid and, for each texel, how steeply the terrain rises toward the sun (made on the GPU only when the sun turns). Mountains shade the valleys behind them as far as the eye sees; inside the virtual maps it adds what rises beyond their reach (the valley under the ridge at sunset). Nothing to turn on in the Landscape.
2. **Impostors** (the Foliage actor's Details > **Impostors**): a mesh many actors use (trees, rocks, the HLOD's proxies) is photographed from 64 directions of the upper hemisphere into two atlases (color and alpha; normal, depth and roughness). Far away each instance becomes **one card** facing the camera that blends the three nearest views with a step of parallax by the depth. The card is lit like the mesh (the sun's shadow at the point it shows, the GI, the aerial perspective, the fog), so the switch doesn't pop. The switch follows the preset: under **20 px** of screen radius on Epic (Cinematic 16, High 24, Medium 28, Low 32). A **Translucent** material, a skeleton and a moving actor keep their mesh.
3. **The fog in the sky's color** (the Exponential Height Fog's Details > **Sky Atmosphere Ambient**): the far fog's ambient light comes from the sky on the side the view looks toward (brighter and warmer toward the sun), not the whole sky's gray, so mountains fade into the sky behind them in the **aerial perspective**'s color (Settings > Aerial Perspective Scale).

In the game: `r.FarShadow` (0/1), `r.FarShadow.Resolution`, `r.Impostors` (0/1), `r.Impostors.ScreenSize` (px), `r.Impostors.Resolution`, `r.Fog.SkyAtmosphere` (0..1), and the properties by name.

```blueprint
Event BeginPlay
  -> Set Console Variable ( Name = "r.Impostors.ScreenSize", Value = 32 )
  -> Set Property by Name ( Property = "Far Shadow Strength", Value = 0.6 )
```
--- es
La **vista lejana** (como la de Red Dead Redemption 2) junta tres cosas que dejan kilómetros de paisaje bonitos y baratos:

1. **Sombra lejana** (Details de la luz direccional > **Far Shadow**, **Far Shadow Strength**): las sombras virtuales del sol cubren 2 km alrededor de la cámara (Épico). Más allá, la **sombra del terreno**: las alturas de las Landscapes en una grilla y, por cada texel, cuánto sube el terreno hacia el sol (calculado en la GPU solo cuando el sol gira). Las montañas sombrean los valles detrás de ellas hasta donde alcanza la vista.
2. **Impostores** (Details del actor Foliage > **Impostors**): una malla que usan muchos actores (árboles, rocas, los proxies del HLOD) se fotografía desde 64 direcciones del hemisferio superior en dos atlas. A lo lejos cada instancia se vuelve **una tarjeta** frente a la cámara que mezcla las tres vistas más cercanas con paralaje. La tarjeta se ilumina como la malla, así el cambio no "salta". El cambio sigue el preset: menos de **20 px** de radio en pantalla en Épico.
3. **La niebla en el color del cielo** (Details de la Exponential Height Fog > **Sky Atmosphere Ambient**): la luz ambiente de la niebla lejana viene del cielo del lado hacia donde mira la vista, no del gris del cielo entero, así las montañas se funden con el cielo detrás de ellas.

En el juego: `r.FarShadow`, `r.Impostors`, `r.Impostors.ScreenSize`, `r.Fog.SkyAtmosphere` y las propiedades por nombre.
