id: editor-plugins-kit
kind: howto
title: Plugins embutidos do editor (32 ferramentas por categoria) | The editor's built-in plugins (32 tools by category) | Plugins integrados del editor (32 herramientas por categoría)
keys: plugins embutidos, built-in plugins, ferramentas do editor, editor tools, batch rename, renomear em lote, color palette, paleta de cores, material swap, troca de material, mesh audit, auditoria de malhas, scatter, espalhar, snap to ground, encaixar no chao, align, alinhar, distribuir, distribute, array, grade, measure, medir, regua, ruler, animation retime, velocidade das animacoes, animation audit, motion trails, rastros, debug cheats, trapacas, god mode, modo deus, slow motion, camera lenta, spawn palette, teleporter, teleporte, tag finder, camera bookmarks, favoritos de camera, actor notes, notas, todo, selection sets, conjuntos de selecao, command palette, paleta de comandos, ctrl+shift+p, asset references, referencias, reference viewer, unused assets, assets sem uso, limpar projeto, screenshot, captura de tela, f9, lighting audit, auditoria de luz, scene stats, estatisticas, light radius, raio das luzes, sound audit, sound map, mapa de sons, ai overview, nav path tester, teste de caminho, safe frames, areas seguras, title safe, net stats, estatisticas de rede
--- pt
A engine traz **32 plugins embutidos**, ferramentas pequenas para o artista, o level designer, o animador, o gameplay, a produtividade, o render, o áudio, a IA, a UI e o online. Eles aparecem em **Editar > Plugins** (barra lateral por categoria, cada um com o seu cartão) e ligam e desligam **na hora**, sem reiniciar. Ligado, cada um ganha a sua janela em **Ferramentas > Plugins > <categoria>**. Vêm desligados, menos os básicos (Encaixar no Chão, Alinhar e Distribuir, Favoritos de Câmera, Conjuntos de Seleção e Paleta de Comandos). Tudo o que mudam nos atores dá para desfazer com Ctrl+Z.

```diagram steps
Editar > Plugins: escolha a categoria | Marque Ativado no cartão | Ferramentas > Plugins > a categoria > o plugin | A janela dele abre
```
## Artista
- **Renomear em Lote**: um prefixo, um sufixo, buscar e trocar, ou um nome base numerado (Prop_01, Prop_02...) nos atores selecionados, com a prévia.
- **Paleta de Cores**: as cores do projeto (Config/ColorPalette.txt); um clique dá a cor às malhas e luzes selecionadas; o botão direito copia em hex.
- **Troca de Material**: todo ator com um material ganha outro, no nível inteiro ou só na seleção.
- **Auditoria de Malhas**: as malhas do nível, quantos atores usam cada uma, quantos fazem sombra, os sem material; um clique seleciona.
## Level Design
- **Espalhar**: cópias dos selecionados ao acaso num raio, com espaçamento mínimo, giro e escala aleatórios, no chão.
- **Encaixar no Chão**: larga os selecionados na superfície abaixo (com ajuste de altura e alinhado à inclinação, se quiser).
- **Alinhar e Distribuir**: mínimo, centro, máximo ou espaçados por igual em X, Y ou Z.
- **Grade**: o selecionado copiado em linhas, colunas e camadas.
- **Medir**: a distância entre os dois últimos selecionados (a linha na viewport) e a extensão da seleção.
## Animação
- **Velocidade das Animações**: a velocidade de reprodução de toda Skeletal Mesh selecionada, com variação para a multidão não andar no mesmo passo.
- **Auditoria de Animação**: cada Skeletal Mesh, o que toca, a velocidade, Physics Asset e IK Rig; os sem animação se destacam.
- **Rastros de Movimento**: o caminho dos selecionados (e de todo Character no Play) com a velocidade: root motion, pé deslizando, caminhos da IA.
## Gameplay
- **Trapaças de Depuração** (no Play): Modo Deus, Andar Rápido, Gravidade Baixa, Curar Tudo e Câmera Lenta; tudo volta ao parar.
- **Paleta de Atores**: as classes que você mais usa em botões grandes; um clique coloca na frente da câmera.
- **Teleporte**: os Player Starts e Checkpoints; a câmera voa até um, ou no Play o jogador vai para lá.
- **Busca de Tags**: as Gameplay Tags do nível com a contagem; um clique seleciona os atores de uma tag.
## Produtividade
- **Favoritos de Câmera**: dez vistas por nível; **Ctrl+Shift+0..9** guarda e um clique volta.
- **Notas nos Atores**: pendências sobre os atores, na viewport e numa lista (Config/ActorNotes.txt, vai pelo Git).
- **Conjuntos de Seleção**: guarda a seleção com um nome e a seleciona de novo num clique.
- **Paleta de Comandos**: **Ctrl+Shift+P** em qualquer lugar; digite e Enter: salvar, Play, focar, as vistas, a janela de um plugin, uma classe para colocar.
- **Referências de Assets**: quem usa um asset e o que ele usa (como o Reference Viewer), lido em segundo plano.
- **Assets sem Uso**: os assets para os quais nada aponta, com o tamanho; vão para a **Lixeira** só depois de você confirmar.
## Render, Áudio, IA, UI, Online
- **Captura de Tela** (**F9**): PNG da viewport, sem ícones nem grade, ou da janela inteira, em Saved/Screenshots.
- **Auditoria de Luz**: dois sóis, sem Sky Light, luzes em 0, raios enormes, luzes com sombra demais sobrepostas.
- **Estatísticas da Cena**: fps, CPU, GPU e as contagens do nível num painel sobre a viewport.
- **Raio das Luzes**: até onde cada luz local alcança (esfera ou cone).
- **Auditoria de Som** e **Mapa de Sons**: os sons do nível numa tabela (mudos, altos demais, sem som) e marcados na viewport.
- **Visão Geral da IA** e **Teste de Caminho**: os agentes, os sentidos e o que fazem no Play; o caminho entre dois atores com o comprimento.
- **Áreas Seguras**: o formato da tela (16:9, 21:9, 4:3, celular, VR), as áreas de título e de ação e a regra dos terços.
- **Estatísticas de Rede**: ping, perda e tráfego sobre a viewport quando online.

```diagram bars ms
Auto-teste dos 32 plugins (ligar, testar, desligar): 37
```
--- en
The engine comes with **32 built-in plugins**: small tools for the artist, the level designer, the animator, gameplay, productivity, rendering, audio, AI, UI and online. They show in **Edit > Plugins** (a side bar by category, each on its own card) and turn on and off **at once**, with no restart. Once on, each gets its window under **Tools > Plugins > <category>**. They come off, except the basic ones (Snap to Ground, Align and Distribute, Camera Bookmarks, Selection Sets and Command Palette). Whatever they change on the actors can be undone with Ctrl+Z.

```diagram steps
Edit > Plugins: pick the category | Tick Enabled on the card | Tools > Plugins > the category > the plugin | Its window opens
```
## Artist
- **Batch Rename**: a prefix, a suffix, find and replace, or a numbered base name (Prop_01, Prop_02...) on the selected actors, with a preview.
- **Color Palette**: the project's colors (Config/ColorPalette.txt); a click gives a color to the selected meshes and lights; right click copies it as hex.
- **Material Swap**: every actor with one material gets another, in the whole level or only the selection.
- **Mesh Audit**: the level's meshes, how many actors use each, how many cast shadows, the ones without a material; a click selects them.
## Level Design
- **Scatter**: copies of the selection at random in a radius, with a minimum gap, random yaw and scale, on the ground.
- **Snap to Ground**: drops the selection onto the surface below (with a height offset, and turned to the slope if you want).
- **Align and Distribute**: min, center, max or evenly spaced on X, Y or Z.
- **Array**: the selected actor copied in rows, columns and layers.
- **Measure**: the distance between the two last selected actors (the line in the viewport) and the selection's span.
## Animation
- **Animation Retime**: the play rate of every selected Skeletal Mesh, with a spread so a crowd doesn't walk in step.
- **Animation Audit**: each Skeletal Mesh, what it plays, its rate, Physics Asset and IK Rig; the ones without animation stand out.
- **Motion Trails**: the path of the selection (and of every Character in Play) with the speed: root motion, foot sliding, AI paths.
## Gameplay
- **Debug Cheats** (in Play): God Mode, Fast Walk, Low Gravity, Heal to Full and Slow Motion; everything goes back at Stop.
- **Spawn Palette**: your most used classes as big buttons; a click places one in front of the camera.
- **Teleporter**: the Player Starts and Checkpoints; the camera flies to one, or in Play the player goes there.
- **Tag Finder**: the level's Gameplay Tags with their counts; a click selects the actors with a tag.
## Productivity
- **Camera Bookmarks**: ten views per level; **Ctrl+Shift+0..9** keeps one and a click goes back.
- **Actor Notes**: TODOs on actors, in the viewport and in a list (Config/ActorNotes.txt, shared through Git).
- **Selection Sets**: keeps the selection under a name and selects it again in a click.
- **Command Palette**: **Ctrl+Shift+P** anywhere; type and Enter: save, play, focus, the views, a plugin's window, a class to place.
- **Asset References**: who uses an asset and what it uses (like the Reference Viewer), read in the background.
- **Unused Assets**: the assets nothing points at, with their size; they go to the **Recycle Bin** only after you confirm.
## Rendering, Audio, AI, UI, Online
- **Screenshot** (**F9**): a PNG of the viewport, without icons or grid, or of the whole window, in Saved/Screenshots.
- **Lighting Audit**: two suns, no Sky Light, lights at 0, huge radii, too many shadowed lights overlapping.
- **Scene Stats**: fps, CPU, GPU and the level's counts in a panel over the viewport.
- **Light Radius**: how far each local light reaches (a sphere or a cone).
- **Sound Audit** and **Sound Map**: the level's sounds in a table (silent, too loud, without a sound) and labeled in the viewport.
- **AI Overview** and **Nav Path Tester**: the agents, their senses and what they do in Play; the path between two actors with its length.
- **Safe Frames**: a screen's shape (16:9, 21:9, 4:3, phone, VR), the title and action safe areas and the rule of thirds.
- **Net Stats**: ping, loss and traffic over the viewport while online.

```diagram bars ms
Self-test of the 32 plugins (on, test, off): 37
```
--- es
El motor incluye **32 plugins integrados**: pequeñas herramientas para el artista, el diseñador de niveles, el animador, el gameplay, la productividad, el renderizado, el audio, la IA, la UI y el online. Aparecen en **Editar > Plugins** (una barra lateral por categoría, cada uno en su tarjeta) y se activan y desactivan **al instante**, sin reiniciar. Una vez activado, cada uno tiene su ventana en **Herramientas > Plugins > <categoría>**. Vienen desactivados, salvo los básicos (Snap to Ground, Align and Distribute, Camera Bookmarks, Selection Sets y Command Palette). Todo lo que cambian en los actores se puede deshacer con Ctrl+Z.

```diagram steps
Editar > Plugins: elige la categoría | Marca Enabled en la tarjeta | Herramientas > Plugins > la categoría > el plugin | Se abre su ventana
```
## Artist
- **Batch Rename**: un prefijo, un sufijo, buscar y reemplazar, o un nombre base numerado (Prop_01, Prop_02...) en los actores seleccionados, con vista previa.
- **Color Palette**: los colores del proyecto (Config/ColorPalette.txt); un clic da un color a las mallas y luces seleccionadas; clic derecho lo copia en hexadecimal.
- **Material Swap**: todo actor con un material recibe otro, en todo el nivel o solo en la selección.
- **Mesh Audit**: las mallas del nivel, cuántos actores usan cada una, cuántos proyectan sombras, las que no tienen material; un clic las selecciona.
## Level Design
- **Scatter**: copias de la selección al azar dentro de un radio, con una separación mínima, yaw y escala aleatorios, sobre el suelo.
- **Snap to Ground**: deja caer la selección sobre la superficie de abajo (con un desfase de altura, y girada según la pendiente si quieres).
- **Align and Distribute**: mínimo, centro, máximo o espaciado uniforme en X, Y o Z.
- **Array**: el actor seleccionado copiado en filas, columnas y capas.
- **Measure**: la distancia entre los dos últimos actores seleccionados (la línea en la ventana gráfica) y la extensión de la selección.
## Animation
- **Animation Retime**: la velocidad de reproducción de cada Skeletal Mesh seleccionado, con una dispersión para que una multitud no camine al mismo paso.
- **Animation Audit**: cada Skeletal Mesh, lo que reproduce, su velocidad, Physics Asset e IK Rig; los que no tienen animación se destacan.
- **Motion Trails**: el recorrido de la selección (y de cada Character en Play) con la velocidad: root motion, deslizamiento de pies, rutas de la IA.
## Gameplay
- **Debug Cheats** (en Play): God Mode, Fast Walk, Low Gravity, Heal to Full y Slow Motion; todo vuelve a la normalidad al detener.
- **Spawn Palette**: tus clases más usadas como botones grandes; un clic coloca una delante de la cámara.
- **Teleporter**: los Player Starts y Checkpoints; la cámara vuela hasta uno o, en Play, el jugador va allí.
- **Tag Finder**: los Gameplay Tags del nivel con sus cantidades; un clic selecciona los actores con una etiqueta.
## Productivity
- **Camera Bookmarks**: diez vistas por nivel; **Ctrl+Shift+0..9** guarda una y un clic vuelve a ella.
- **Actor Notes**: TODOs en los actores, en la ventana gráfica y en una lista (Config/ActorNotes.txt, compartido por Git).
- **Selection Sets**: guarda la selección con un nombre y la vuelve a seleccionar con un clic.
- **Command Palette**: **Ctrl+Shift+P** en cualquier lugar; escribe y Enter: guardar, jugar, enfocar, las vistas, la ventana de un plugin, una clase para colocar.
- **Asset References**: quién usa un asset y qué usa él (como el Reference Viewer), leído en segundo plano.
- **Unused Assets**: los assets a los que nada apunta, con su tamaño; solo van a la **Papelera de reciclaje** después de que confirmes.
## Rendering, Audio, AI, UI, Online
- **Screenshot** (**F9**): un PNG de la ventana gráfica, sin iconos ni cuadrícula, o de toda la ventana, en Saved/Screenshots.
- **Lighting Audit**: dos soles, ningún Sky Light, luces a 0, radios enormes, demasiadas luces con sombra superpuestas.
- **Scene Stats**: fps, CPU, GPU y los conteos del nivel en un panel sobre la ventana gráfica.
- **Light Radius**: hasta dónde llega cada luz local (una esfera o un cono).
- **Sound Audit** y **Sound Map**: los sonidos del nivel en una tabla (silenciosos, demasiado fuertes, sin sonido) y etiquetados en la ventana gráfica.
- **AI Overview** y **Nav Path Tester**: los agentes, sus sentidos y lo que hacen en Play; la ruta entre dos actores con su longitud.
- **Safe Frames**: la forma de una pantalla (16:9, 21:9, 4:3, móvil, VR), las áreas seguras de título y de acción y la regla de los tercios.
- **Net Stats**: ping, pérdida y tráfico sobre la ventana gráfica mientras estás online.

```diagram bars ms
Autoprueba de los 32 plugins (activar, probar, desactivar): 37
```
