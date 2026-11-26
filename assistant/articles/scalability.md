id: scalability
kind: howto
title: Escalabilidade e qualidade gráfica | Scalability and graphics quality | Escalabilidad y calidad gráfica
keys: vrs, variable rate shading, sombreamento de taxa variavel, gtao, ssao, oclusao de ambiente, ambient occlusion, cantos escuros, escalabilidade, qualidade grafica, baixo medio alto epico, low medium high epic, presets, configuracoes graficas, pc fraco, notebook, msaa, anti aliasing, modo desempenho, modo qualidade, mapa de sombra, shadow map, sem ray tracing, gpu sem rt, luz atravessa parede
--- pt
- **Configurações** (canto superior direito do editor): Baixo, Médio, Alto, Épico. [Mostrar onde](show:Settings)
- Cada recurso caro tem preset: sombras, GI, nuvens, neblina volumétrica, MSAA/TSR.
- **Sombras das luzes locais** (pontuais, spot, retangulares): traçadas por raio do **Médio** para cima. No **Baixo**, e numa GPU sem ray tracing em qualquer preset, vêm de **mapas de sombra**: as luzes mais importantes na tela (tamanho, distância, intensidade) ganham um mapa, as outras ficam sem sombra.
  - Quantas luzes com mapa: Baixo 8, Médio 16, Alto 24, Épico 32. A resolução de cada uma segue o tamanho dela na tela.
  - Os mapas ficam guardados: só se redesenha o que mudou (a luz se moveu, um objeto se moveu dentro do alcance dela).
  - Uma luz só projeta sombra com **Cast Shadows** ligado (Details da luz). Desligar em luzes pequenas e de enfeite poupa o orçamento.
- **Oclusão de ambiente em tela (GTAO)**, do **Alto** para cima: os cantos (parede com chão, com teto, móveis encostados) ganham a sombra suave da luz indireta. Custa ~0,3 ms a 1440p.
- **Variable Rate Shading** (Configurações do Projeto > Rendering, desligado por padrão): sombreia as áreas lisas a cada 2×2 pixels (~0,6 ms a menos no passe principal a 1440p). As bordas de profundidade ficam sempre com taxa cheia (sem luz vazando pela aresta), a taxa segue o movimento e não fica trocando de um quadro para o outro: a cintilação fica quase igual à de sem VRS.
- O jogo empacotado tem a sua em Configurações do Projeto > Rendering, e o projeto pode ser **Desempenho** ou **Qualidade** (o path tracer como vista do jogo).
--- en
- **Settings** (the editor's top right): Low, Medium, High, Epic. [Show me where](show:Settings)
- Each costly feature has presets: shadows, GI, clouds, volumetric fog, MSAA/TSR.
- **Local lights' shadows** (point, spot, rect): ray traced from **Medium** up. On **Low**, and on a GPU without ray tracing at any preset, they come from **shadow maps**: the lights that matter most on screen (size, distance, intensity) get a map, the others cast none.
  - How many lights get a map: Low 8, Medium 16, High 24, Epic 32. Each one's resolution follows its size on screen.
  - The maps are kept: only what changed is drawn again (the light moved, an object moved inside its reach).
  - A light casts shadows only with **Cast Shadows** on (the light's Details). Turning it off on small decorative lights saves the budget.
- **Screen space ambient occlusion (GTAO)**, from **High** up: corners (wall and floor, wall and ceiling, furniture against a wall) get the soft shadow of the indirect light. It costs ~0.3 ms at 1440p.
- **Variable Rate Shading** (Project Settings > Rendering, off by default): shades flat areas once per 2×2 pixels (~0.6 ms less in the main pass at 1440p). Depth edges always keep the full rate (no light leaking past the edge), the rate follows motion and doesn't flip from frame to frame: the shimmer stays close to no VRS.
- The packaged game has its own in Project Settings > Rendering, and the project can be **Performance** or **Quality** (the path tracer as the game's view).
--- es
- **Settings** (arriba a la derecha del editor): Bajo, Medio, Alto, Épico. [Muéstrame dónde](show:Settings)
- Cada función costosa tiene presets: sombras, GI, nubes, niebla volumétrica, MSAA/TSR.
- **Sombras de las luces locales** (puntual, foco, rectangular): con ray tracing desde **Medio**. En **Bajo**, y en una GPU sin ray tracing con cualquier preset, vienen de **mapas de sombra**: las luces que más importan en pantalla (tamaño, distancia, intensidad) reciben un mapa, las demás no proyectan ninguno.
  - Cuántas luces reciben un mapa: Bajo 8, Medio 16, Alto 24, Épico 32. La resolución de cada una sigue su tamaño en pantalla.
  - Los mapas se conservan: solo se vuelve a dibujar lo que cambió (la luz se movió, un objeto se movió dentro de su alcance).
  - Una luz proyecta sombras solo con **Cast Shadows** activado (los Detalles de la luz). Desactivarlo en luces decorativas pequeñas ahorra presupuesto.
- **Oclusión ambiental en espacio de pantalla (GTAO)**, desde **Alto**: las esquinas (pared y suelo, pared y techo, muebles contra una pared) reciben la sombra suave de la luz indirecta. Cuesta ~0,3 ms a 1440p.
- **Variable Rate Shading** (Project Settings > Rendering, desactivado por defecto): sombrea las zonas planas una vez por cada 2×2 píxeles (~0,6 ms menos en el pase principal a 1440p). Los bordes de profundidad siempre mantienen la tasa completa (sin luz que se cuele por el borde), la tasa sigue el movimiento y no cambia de un fotograma a otro: el centelleo queda cerca del de sin VRS.
- El juego empaquetado tiene el suyo en Project Settings > Rendering, y el proyecto puede ser **Performance** o **Quality** (el path tracer como la vista del juego).
