# PCG: geração procedural de conteúdo (Q15)

O PCG da Unreal, mais simples: um **PCG Graph** (asset `.rpcg`, JSON) de nós que fazem, filtram, mudam e criam
**pontos**; um **Volume PCG** roda o grafo dentro da sua caixa, no editor (ao mudar algo) e em jogo (ao carregar, sob
demanda, ou por célula do World Partition). Determinístico pela semente. Artigo do Assistente: `pcg`. Área de testes: `pcg`.

## Arquivos
- `src/pcg.h`: a API inteira (asset, pontos, execução, interface de splines, componente, Blueprints, editor).
- `src/pcg_graph.c` (re_render): a tabela dos 21 tipos de nó e das opções de cada um (nome, tipo, slot, padrão,
  faixa), edição do grafo (adicionar, ligar sem ciclo, remover), o arquivo (nós pelo id do tipo e opções pelo nome: um
  tipo ou opção nova lê os arquivos antigos), o registro dos assets por hash do caminho (lido uma vez; a cópia do editor
  substitui: `pcg_graph_edited`), `pcg_graph_cached` (sem disco, para os workers) e `pcg_graph_prepare` (os subgrafos e
  aninhados lidos na thread principal antes), os exemplos em `Content/PCG/Examples`.
- `src/pcg_exec.c` (re_render): a execução. Ordem de Kahn; os pontos em SoA (posição, normal, rotação, escala,
  densidade, extensão, semente e até 8 atributos com nome) na arena linear da geração; várias ligações numa entrada são
  juntadas na ordem das ligações.
- `src/pcg_world.c` (re_render): o componente em execução (unidades, streaming, lotes, spawn, limpeza), os provedores
  de spline (Trilho, Rio), os nós de Blueprint, as estatísticas, o Debug Viewer (lista "PCG").
- `src/pcg_editor.c` (editor): a janela do grafo, os Detalhes do componente, os pontos e a caixa na viewport, Gaveta >
  Adicionar > PCG. `src/pcg_test.c`: autoteste e cenas.

## O componente (sem COMP_ novo)
A máscara de componentes está cheia (`COMP_COUNT` = 64, `_Static_assert`). O Componente PCG é um **modo do Procedural
World**: `CProcWorld.mode = PROCGEN_PCG` (16, fora do combo de modos) e os campos `CProcWorld.pcg` no fim (grafo,
semente, extensão da caixa, flags, gatilho, célula, alcance, orçamento, até 8 parâmetros sobrescritos). Níveis antigos
leem zeros. `procgen.c` e `navmesh.c` ignoram atores nesse modo; o Outliner mostra "PCG Volume" e os Detalhes trocam a
seção do Procedural World pela do PCG. Quando as máscaras forem alargadas (o Q14 precisou de um componente novo?), mover
`pcg` para um `COMP_PCG` próprio é mecânico: todo acesso passa por `settings()`/`pcg_is_component()` em `pcg_world.c`.

## Nós
- Fontes: **Surface Sampler** (Landscape, malhas no volume ou os dois: um ponto por célula de uma grade do *mundo*,
  movido pela Folga; altura e normal do Landscape por `landscape_height` e diferenças de ±50 cm; malhas pela CPU do
  renderer, `render_mesh_cpu`, triângulos binados numa grade 2D e raio vertical, o mais alto), **Volume Sampler** (grade
  3D do mundo), **Spline Sampler** (na spline com Deslocamento / dos dois lados, ou enchendo a largura; alinhado; projetado
  no chão).
- Filtros (saídas Dentro / Fora): **Attribute** (Density, X, Y, Z/Height, Steepness, Scale, Yaw, Seed ou atributo
  próprio; < <= > >= == !=), **Density**, **Steepness**, **Landscape Layer** (`landscape_layer_at`, novo no
  landscape.c: a camada pintada em qualquer Landscape sob o ponto).
- Pontos: **Density Noise**, **Spatial Noise** (ruído de valor fBm no xy do mundo: o mesmo entre células),
  **Create Attribute**, **Transform Points** (offset/rotação/escala aleatórios, uniforme, absoluta, alinhar à normal por
  `rotator_axes`/`axes_rotator`), **Project to Surface**, **Self Pruning**, **Difference**, **Merge**.
- Criadores: **Static Mesh Spawner** (4 entradas: malha = nome de forma ou caminho importado, material, cor, peso,
  deslocamento e escala no referencial do ponto; "Sortear Uma pelo Peso" ou "Todas as Entradas"; colisão, sombra, vento),
  **Spawn Actor** (classe Blueprint pelo caminho), **Nested PCG** (outro grafo numa caixa em volta de cada ponto, semente
  do ponto), **Subgraph** (os nós de outro grafo sobre estes pontos). Parâmetros de mesmo nome descem para os dois.
- Parâmetros do grafo: até 16; um número de nó ligado a um (`PcgNode.bind`) usa o valor resolvido (padrão do grafo →
  sobrescrito no ator → Set PCG Parameter).
- Depuração: nó marcado (D) ou o componente com Mostrar Pontos (os pontos que chegaram aos criadores) → pontos na
  viewport pela densidade (`pcg_draw_debug`, até 80 000); cada cartão mostra pontos e ms da última geração.

## Determinismo
Todo número aleatório = hash(semente do componente, id do nó, semente do ponto, k). A semente do ponto vem da célula da
grade do mundo (ix, iy[, iz]) e do id do nó (o id é fixo no arquivo, nunca reaproveitado), nunca da ordem. As linhas
dos amostradores rodam no `re_parallel_for` e cada linha escreve o seu trecho, compactado na ordem depois: o mesmo
resultado com 1 ou N threads (autoteste). Um ponto pertence à célula onde cai: gerar a caixa inteira ou em 4 células dá
o **mesmo conjunto** (autoteste, sem auto-exclusão). A Auto-Exclusão é por geração: na borda de duas células pode
manter dois pontos que a caixa inteira separaria (como a da Unreal).

## Execução no editor e em jogo
- Unidade = a caixa inteira, ou uma célula (Particionado). Estados: na fila → gerada (worker) → criando → pronta; longe →
  sai (no próximo lote).
- Geração: até N unidades por quadro em paralelo (`re_parallel_for`, uma por worker), cada uma com a **própria arena**
  (1 GB reservado, comprometido ao usar; resetada a cada geração: sem malloc no quadro) e os próprios buffers de spawns
  e de pontos de depuração (`ReVmBuf`, reservados uma vez). As fontes (malhas e splines que tocam a caixa) são juntadas
  na thread principal; a célula pega só as malhas perto dela.
- Spawn: malhas = entidades `NAME|TRANSFORM|STATIC_MESH|ATTACH` (kind `ATTACH_PCG`, dono = o ator), atores = `bp_spawn`;
  todos com `TRANSFORM_PCG` (bit 7 do CTransform). Nunca salvos: `pcg_purge` antes de salvar (ao lado do `procgen_purge`),
  refeitos pela semente no próximo quadro. Sem colisão → `TRANSFORM_NO_COLLISION`; jogando com colisão → `bp_box_spawned`.
- Editor: com Gerar ao Mudar, o hash do que ele lê (opções, transform, versão do grafo e dos subgrafos, malhas e splines
  na caixa, alturas do chão numa grade 9x9) é refeito quando o mundo muda (`w->changes`); diferente → refaz. Uma revisão
  nova do mundo (desfazer, nível carregado, snapshot do PIE de volta) apaga tudo o que tem `TRANSFORM_PCG` e refaz.
- PIE: o que o editor fez fica (a mesma semente faz o mesmo: teste); Gerar Sob Demanda é limpo no começo e espera o
  Generate PCG. O jogo empacotado gera ao carregar (nada foi salvo).
- **Lotes no jogo** (achados no `pcg_stream`): (1) uma entidade custa ~0,2 µs; o orçamento de 1 ms criava ~4000 num
  quadro e estourava o anel de mudanças do ECS (4096): o renderer reenviava a cena inteira (50-60 ms). Agora no máximo
  2048 por lote. (2) Um estático com sombra que aparece ou some faz a sombra virtual redesenhar inteiros os níveis sobre
  ele (um por quadro): spawns a cada quadro mantinham um nível redesenhando sempre. Agora descargas e spawns saem juntos
  a cada 0,5 s (as células nascem 256 m à frente). (3) No `render.c`, um estático **destruído** chamava `shadows_invalidate()` (todos os níveis no mesmo
  quadro: picos de 100-800 ms ao descarregar células); agora o renderer guarda a caixa de cada estático com sombra
  (`R.vsm_box`, 24 bytes por slot) e suja só onde ele estava. Isso vale também para o World Partition.

## Nós de Blueprint (categoria PCG, bloco (Q15) no fim do enum)
Generate PCG, Cleanup PCG, Set PCG Seed, Set PCG Parameter, Is PCG Generated (puro: e Count). Bloqueados no Construction
Script do editor.

## Exemplos (Content/PCG/Examples, escritos quando faltam)
PCG_Forest (amostrador → ruído espacial → densidade → inclinação ≤ 28° → auto-exclusão → transformação → tronco + copa;
arbustos nas bordas longe das árvores por Difference; parâmetro Tree Density), PCG_Rocks (pedras viradas e achatadas, com
musgo por atributo; parâmetro Rock Density), PCG_RockClusters (um Nested PCG do PCG_Rocks a cada lugar, Rock Density 0,12
passada para baixo), PCG_Fence (poste a cada 2,5 m e duas travessas ao longo das splines do volume).

## Medidas (RTX 3060, 2560x1417)
| | |
|---|---|
| Volume Sampler, 100 000 pontos | 0,85-1,0 ms nos workers; 2,5 ms em 1 thread |
| Grafo inteiro em 100 000 pontos (ruído, transformação, auto-exclusão, criador) | 23-26 ms; a auto-exclusão 17 ms (gulosa, sequencial: ordem por classe de raio e curva de Morton, grade densa 3D) |
| Auto-exclusão antes das otimizações | 45 ms (hash 2D, ordem aleatória) |
| Arena da geração de 100 000 pontos | 25-33 MB comprometidos, iguais na 2ª, 3ª, 4ª vez (nada cresce) |
| `pcg_forest` (400 m, 4370 entidades: floresta 3468, aglomerados 362, cerca 540) | gerar no editor < 1 ms por nó; 252-318 fps (sem PCG: 359); pior quadro 182-218 ms (a geração inteira no editor de uma vez) |
| Célula de 128 m da floresta | 0,25-0,45 ms num worker |
| `pcg_stream`, pior atualização do PCG jogando (geração nos workers + spawn) | 0,64-1,5 ms |
| `pcg_stream` (plano de 3,2 km, 80 m/s, 70 células geradas, 48 descartadas, Tree Density 0,015), GPU livre, com validação | 356 fps; quadros acima de 2x a mediana 0,84 % (pior 18 ms) |
| o mesmo sem PCG (`REALENGINE_PCG_OFF=1`) | 425 fps; 0,03 % (pior 5,8 ms) |
| o mesmo com a floresta densa do exemplo (0,04: 14 mil entidades por perto) | 303 fps; 1,6-1,7 % acima de 2x a mediana: a sombra virtual re-centra os níveis a cada ~0,2 s com a câmera a 80 m/s e redesenha as árvores (6-10 ms), sem relação com os lotes do PCG (0,5 s) |
| antes dos lotes e do `vsm_box` (spawn a cada quadro, 1 ms) | 12-16 % acima de 2x a mediana, picos de 60-800 ms (anel de mudanças estourado, `shadows_invalidate` ao descarregar) |
| com outra sessão na GPU (para comparar) | o próprio `world_partition` cai para 4 % e o `pcg_stream` sem PCG para 7-11 % |

## Testes (área `pcg`)
- `pcg_selftest` (`--pcg-test`, sem janela): 100 000 pontos e o custo, determinismo (duas vezes, serial x workers, outra
  semente), caixa inteira x 4 células, spline de 1 km (401 / 802 / 5005 pontos), subgrafo e aninhado (9 e 81), atributo
  e parâmetro, arquivo escrito e lido (mesmo hash), exemplos, arena sem crescer.
- `pcg_forest` (`--scene pcg_forest`): floresta no Landscape no editor (todo tronco no chão a < 1 cm, nenhum em encosta
  > 28°), aglomerados e cerca; PIE mantém as mesmas entidades e o mesmo hash; Set PCG Seed 8 muda, 7 volta à mesma.
- `pcg_stream` (`--scene pcg_stream --play`): chão plano de 3,2 km (como o do `world_partition`: um Landscape dessa
  escala já engasga sozinho com a câmera rápida na sombra virtual), World Partition, a floresta particionada (cópia do
  exemplo com o alvo Landscape e Malhas; Tree Density 0,015 pelo parâmetro do ator); o jogador a 80 m/s por 1,3 km;
  células geradas e descartadas, critério do `world_partition` (quadros acima de 2x a mediana < 1 %, pior atualização do
  PCG < 6 ms). `PCG_SPIKES=1` imprime os quadros acima de 9 ms com o que o PCG fez neles.

## Falta
- A sombra virtual do sol com muitos estáticos densos e a câmera rápida: cada re-centralização de nível redesenha o nível
  inteiro (6-10 ms com 14 mil árvores); um cache por páginas/blocos dos níveis estáticos tiraria isso (área render/luz).
- Grafo com execução assíncrona entre quadros (hoje a geração de uma célula roda dentro do quadro, nos workers).
- Self Pruning entre células vizinhas (hoje por geração), Spline Component do Q14 (ligar o provedor no merge),
  instâncias de malha agrupadas (hoje uma entidade por instância, como o Foliage), HLOD do que o PCG gera.
- `REALENGINE_PCG_OFF=1`: nada é gerado (o "antes" das medidas).
