# Mundos grandes: World Partition, Data Layers e HLOD (B1)

Código: `src/world_partition.c/.h`. Componentes `CWorldPartition` (o ator do nível, um só, como o World Settings da
Unreal) e `CPartition` (de cada ator: Is Spatially Loaded e as Data Layers). Nós: Set / Get Data Layer Runtime State,
Is Location Loaded. Artigo do Assistente: `world-partition`. Teste: `world_partition` (cena `wp`).

## Como funciona
- **Começo do Play** (`wp_begin`, antes dos BeginPlay; editor e jogo empacotado): cada ator gerenciado (da grade, ou
  numa Data Layer) vira um registro: a máscara de componentes e os bytes de cada um, numa arena só (898 KB para 6048
  atores). As células (128 m) e os registros ficam fixos durante o Play. O que não é querido no começo sai antes do
  BeginPlay (nunca roda para ele). O snapshot do PIE devolve o nível inteiro no Stop.
- **Cada quadro** (`wp_update`): cada célula mede a distância do quadrado dela à câmera; entra até o Loading Range e sai
  além dele mais 1/4 de célula (sem piscar na borda). Os registros cujo estado mudou entram numa fila, feita dentro do
  **Streaming Budget** (1 ms por quadro): descargas primeiro, depois as cargas, as mais perto primeiro. Carga = o ator
  refeito dos bytes (Blueprint: componentes refeitos + BeginPlay; a colisão entra pelo `bp_box_spawned`); descarga =
  `bp_destroy_actor` com End Play.
- **Auto** (Is Spatially Loaded): fica sempre o que o nível inteiro precisa (sol, céu, neblina, nuvem, Landscape,
  Foliage, mundo procedural, água, rios, trilhos, chuva, NavMesh, câmera, Player Start, Character, Game Rules, Quest
  Log, HUD, Salvável, Inventário, Interactor, Splat, Audio Volume, veículos, constraints, checkpoints); o resto com
  Transform vai para a grade. Yes/No forçam.
- **Data Layers**: até 16 por nível, estado Unloaded / Loaded (dentro, escondido) / Activated. Um ator em várias camadas
  só entra se nenhuma estiver Unloaded e só aparece se todas estiverem Activated. Uma camada mudada reavalia os
  registros dela na mesma fila.
- **HLOD em dois níveis** (como as camadas de HLOD da Unreal):
  - Perto (o tipo Instancing da Unreal): cada ator de malha estática da grade (sem Blueprint, sem física, maior que
    HLOD Min Size) tem um **proxy** feito no começo: a mesma malha e o mesmo material, `TRANSFORM_NO_COLLISION`, sem
    tick. Aparece quando o ator saiu e a região dele não mostra a malha combinada.
  - Longe (Merged/Simplified Mesh): **Compilar > Construir HLODs** junta cada região de N x N células (4: 512 m) numa
    malha (posições relativas ao centro da região, normais pela inversa da escala, cor de cada malha = `avg_base` do
    material ou a Base Color, em sRGB nas cores dos vértices), simplifica com `meshopt_simplify` (5 % dos triângulos,
    `meshopt_simplifySloppy` quando o cuidadoso não chega lá) e grava `Content/HLOD/<nível>/r_<x>_<y>.rmesh` com LODs
    (material `M_HLOD`: Color Attribute na Base Color, rugosidade 0,75). Aparece quando nenhuma célula da região está no
    mundo nem esperando para sair.
  - Por que não uma malha por célula: o renderer tem 1023 malhas (`RE_MAX_MESHES`); 4 x 4 km em 128 m são 1024
    células. As regiões de 512 m dão 64, e os proxies cobrem a faixa entre o Loading Range e a região inteira fora.
- **Física**: `gather_statics` agora pula `TRANSFORM_NO_COLLISION` (os proxies e o NoCollision da Unreal).
- **64 componentes**: `CWorldPartition` e `CPartition` fecharam a máscara de 64 bits; dois deslocamentos por
  `COMP_COUNT` viraram indefinidos (Blueprints sem componentes, níveis recusados) e foram corrigidos (`COMP_ALL`,
  notas-dev/editor.md). O próximo componente precisa de máscaras maiores.
- **Details**: o buffer de desfazer guardava 128 bytes por componente; o `CWorldPartition` tem ~700. Agora 4096
  (`DETAILS_COMP_MAX`), com um FAIL se algum componente passar disso.

## Medidas (RTX 3060, 1440p, Épico; cena `wp`: 4 x 4 km, 6000 props de 1 a 12 m, o jogador cruzando a 400 m/s)
| | sem World Partition (`REALENGINE_WP_OFF=1`) | com |
|---|---|---|
| fps | 240 | 252 |
| CPU / GPU | 1,84 / 4,02 ms | 2,73 / 3,79 ms |
| quadros acima de 2x a mediana | 3,16 % (pior 18,2 ms) | 0,16 % (pior 11,2 ms) |
| atores no mundo | 6048 | ~100 (18 células) |
- Streaming: 0,046 ms por quadro, pior 0,26 ms; 1201 cargas e 1187 descargas na travessia. `wp_begin`: 5,96 ms.
- Construir HLODs: 64 regiões, 3,78 M -> 295 mil triângulos, 2,2 s.
- HLOD no fim da travessia: 79 proxies e 62 das 64 regiões à vista.
- O CPU sobe ~0,9 ms com a troca de atores (as caixas de colisão varridas, a cena da GPU atualizada); a GPU desce.

## Falta
- O pacote do jogo ainda grava o nível inteiro: as células em arquivos próprios lidos por uma thread (memória do jogo
  menor que o nível).
- Rebase de origem sem engasgo (B1d) espalhado em quadros; um arquivo por ator (B1e, com o B14).
- Material das regiões assado em textura (hoje só a cor média de cada malha); HLOD de Blueprints e de malhas com física.
- Uma janela própria do World Partition (hoje o mapa fica nos Details do ator) e fontes de streaming além da câmera.

## Impostores (Q21, 2026-11-18)
Os proxies do HLOD (instâncias da mesma malha dos atores) ganham o impostor octaedral longe, como as árvores do Foliage:
[visao-distante.md](visao-distante.md).

## Streaming preditivo pela velocidade (Q27.7, 2026-11-23: o streaming do GTA 6 para um carro rápido)
- `wp_update` mede a velocidade do foco (média de ~0,3 s; um salto de mais de 2 km/s, como um teleporte, não conta).
  Acima de 5 m/s, as células perto do caminho dos próximos `WP_PREDICT_S` (2 s, no máximo 2 × Loading Range à frente)
  entram também: o ponto do segmento mais perto do meio da célula, a distância do quadrado dela até ele no mesmo alcance
  (com a mesma folga de 1/4 de célula para sair). A fila das cargas vai pelo **tempo até chegar** (`Cell.eta`: 0 dentro
  do alcance, a fração do caminho × o tempo dele à frente), depois pela distância; descargas continuam primeiro.
- Medida nova: registros ainda esperando carga numa célula a menos de 0,75 × o alcance (`WpStats.missing`, os quadros
  com algum, o pior) e as células pedidas à frente (`ahead`). `REALENGINE_WP_NO_PREDICT=1` compara.
- **Medidas** (RTX 3060, 1440p, cena `wp`, 10 s, quadros acima do dobro da mediana):
  | | sem previsão | com |
  |---|---|---|
  | `wp_fast` (250 km/h) | 0,25 % | 0,14 / 0,11 / 0,17 % |
  | `wp` (400 m/s) | 0,32 / 0,48 % | 0,44 / 0,28 % (ruído) |
  Nenhum registro faltando dentro do alcance nas duas, com ou sem (o orçamento de 1 ms dá conta da grade de teste); o
  ganho é a carga espalhada antes (2 células pedidas à frente a 250 km/h). Streaming 0,047 ms por quadro (pior 0,21).
- Teste `wp_fast` (área mundos-grandes): 250 km/h, nenhum quadro com célula à vista carregando, ≤ 0,2 % de engasgos.
- Fica para depois: menos trânsito e multidão e LOD mais baixo em alta velocidade, leitura e descompressão das células
  nos workers (hoje o ator é refeito dos bytes na thread principal).
