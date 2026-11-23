# Traces e desenho de debug (2026-11-19, pedido do usuário)

"Sphere trace, line trace, todos os tipos de trace... ter como visualizar na tela como debug, e executar mais de 200 mil
traces e lógicas complexas sem travar." A engine não tinha os nós de trace da Unreal (só traces especiais: AR, VR, lag)
nem desenho de debug 3D (só overlays do ImGui).

## Consultas (`src/scene_query.c/.h`)
- **Formas**: linha, esfera, caixa (alinhada ao mundo) e cápsula em pé; simples (`sq_trace`), múltiplo
  (`sq_trace_multi`: um acerto por ator, do mais perto, até 256) e em lote paralelo (`sq_trace_batch`).
- **O mundo**: copiado uma vez por quadro, na primeira consulta do quadro (thread principal):
  - as caixas de colisão do jogo (`bp_query_boxes`, as mesmas dos Characters; montadas na hora se ainda não existem);
  - as cápsulas dos Characters;
  - os Landscapes.
- **Primitiva**: caixa-núcleo + raio (caixa: raio 0; cápsula: o segmento como caixa achatada + raio).
  - A forma da consulta somada (Minkowski) dá de novo caixa + raio, e o segmento contra essa caixa arredondada é exato:
    faces por slabs; arestas e quinas por cápsulas (Ericson 5.5.7).
  - Por isso esfera, caixa e cápsula não são aproximadas.
- **BVH de 8 filhos**: os 8 testados de uma vez com AVX2 (`phys_simd.h`; laço simples no Android).
  - Montada por SAH em 12 baldes e depois colapsada de binária para 8 filhos.
  - Remontada quando muda a contagem (ou a cada 240 quadros com algo se mexendo); refit quando algo se mexe.
- **Landscape** (`landscape_raycast`): DDA por blocos de 16×16 células com o mínimo e o máximo de cada bloco
  (`landscape_query_prepare`), e as duas triângulos de cada célula exatos.
  - O antigo `landscape_trace` (64 passos + 10 metades, ~74 alturas por raio) fica para quem já o usava.
  - Mundo Procedural: marchado como antes.
  - Varreduras descem o segmento pela base da forma (aproximação).
- **Canais**: Visibility e Camera veem tudo; World Static, World Dynamic, Pawn e Landscape filtram (`SQ_OBJ_*`).
  - Ignore Self é sempre ligado; Actors to Ignore até 63.
- **Debug Viewer**: lista "Scene Queries" (consultas e lotes do último quadro, ms, build/refit da BVH).

## Desenho de debug (`src/debug_draw.c/.h`, `shaders/debug_line.vert/frag`, `render_debug_draw`)
- **Primitivas de 32 bytes** (linha com largura em px, ou ponto quadrado).
  - Listas do quadro e as guardadas (por duração ou persistentes, em grupos com o fim de cada um).
  - Reserva por contador atômico: qualquer thread, sem lock, sem malloc no quadro; a lista cresce no começo do quadro
    seguinte.
- **Desenho**: depois do composite, no mesmo passe, como quads em espaço de tela com borda suavizada.
  - O teste de profundidade é feito à mão: a profundidade da cena, binding 21.
  - O trecho escondido fica com 30% (dá para ver o trace atravessando a parede).
  - A matriz é a `tsr_vp`, sem o jitter.
- **Registros de trace dos lotes** (`DdTrace`, 32 bytes: início, fim, t, duas cores RGB565).
  - Os próprios workers do lote escrevem (`sq_trace_batch_drawn`), sem passo de montagem.
  - A GPU os expande em linhas de hardware: 8 vértices (até o acerto, depois dele, uma cruz no impacto).
  - Com quads antisserrilhados (18 vértices) 600 mil segmentos custavam ~16 ms de GPU; em linhas, ~5 ms.
- **Nós** (`bp_vm.c` `trace_native`, categorias Collision|Traces e Collision|Debug):
  - Line/Sphere/Box/Capsule Trace By Channel e os Multi;
  - Line Trace Batch e Sphere Trace Batch;
  - Draw Debug Line/Point/Box/Sphere/Capsule, Flush Persistent Debug Lines, Set Debug Trace Colors.
- **Pinos**: Trace Channel e Draw Debug Type são int com lista na interface (`int(ETraceChannel)`,
  `int(EDrawDebugTrace)`: `bp_builtin_enum`).
- **Limite de 16 pinos por nó**: sem Trace Complex nem Trace Hit Color nos pinos (a cor sai de Set Debug Trace Colors).
- **Saídas**: achatadas (Location, Impact Point, Normal, Hit Actor, Distance), sem a struct Hit Result.

## Medidas (RTX 3060, i7-11700F 8 núcleos/16 threads, 1440p)
- **`--sq-test`** (20 500 primitivas aleatórias, 2 861 nós, BVH montada em 6–8 ms): lotes de 200 000 traces de cada
  forma em 8–14 ms.
  - Medido com outros dois agentes compilando em todos os núcleos (CPU a ~55%): número a refazer com a máquina livre.
  - Exatidão contra força bruta: 600/600 por forma (os 2 "erros" de linha eram raspões em quinas que a força bruta
    amostrada pula; o teste confere o ponto na superfície).
- **`--trace-bench 200000`** na cena `house` jogando (validation layers ligadas, CPU ocupada pelos outros agentes):
  - sem traces: 263 fps;
  - 200 mil traces por quadro (¼ de cada forma) desenhados: 86 fps, traces 2,7 ms nos workers, GPU 10,4 ms;
  - sem desenhar: 130 fps.
  - O custo do desenho é da GPU (600 mil segmentos longos em 1440p).
- **`phys_crates`** (20 001 primitivas, 20 mil caixas se mexendo: refit a cada quadro): os traces 3,9 ms.

## Testes (área traces)
- `scene_queries` (`--sq-test`): as formas contra força bruta, múltiplo contra simples, os lotes.
- `trace_nodes` (`--trace-test`, com validation): um Blueprint com cada nó sobre um chão e um cubo; distâncias exatas
  (linha 450, esfera 400, caixa 420, cápsula 370), múltiplo, lote de 2, primitivas desenhadas.
- `trace_bench` (`--trace-bench 200000` na `house`): o relatório e um piso de fps.

## Trace Complex (2026-11-19, pedido do usuário)
- **Pino Trace Complex** em todos os nós de trace (`SqQuery.cplx`; `complex` é macro do MSVC).
- **O que ele testa**: a malha do próprio ator (a caixa que veio da malha dele: `bp_query_boxes` diz qual, os
  componentes não) por triângulos, e não pela caixa.
  - Os triângulos vêm da BVH por malha da física (`physics_mesh_tris`, a mesma do Use Complex Collision), montada no
    primeiro uso, na thread principal.
  - A partir do primeiro trace complexo, o mundo copiado leva a malha e o transform de cada caixa (`CxMesh`).
- **Linha**: exata, Möller-Trumbore no espaço da malha.
  - O segmento é levado pelo inverso do transform; o t é o mesmo.
  - A normal volta pela inversa transposta e fica voltada para o início.
- **Esfera, caixa e cápsula**: avanço conservador contra cada triângulo que a varredura alcança.
  - Candidatos: as caixas da BVH e de cada triângulo crescidas pela forma no espaço da malha, e o t de entrada.
  - Distância: o `phys_distance` (GJK) entre a forma e o triângulo.
  - Passo: a distância dividida pela velocidade de aproximação ao longo da normal separadora (nunca passa do contato);
    pára a 0,02 cm.
  - O Impact Point é o ponto da forma ao longo da normal.
- **Teste `trace_nodes`**, numa esfera de 50 cm:
  - a 30 cm do eixo, a caixa dá 450 cm e o trace complexo 460 cm (a superfície);
  - uma esfera de 20 cm complexa dá 430 cm.

## Traces assíncronos (2026-11-19, pedido do usuário)
- **API** (`sq_async_begin` / `sq_async_kick` / `sq_async_result` / `sq_async_join`):
  - o lote roda em threads próprias (núcleos − 2, dormindo numa porta: `re_gate_*` no `platform_win32.c`) enquanto o
    quadro segue (o resto do jogo, a gravação do render, a GPU);
  - o começo do quadro seguinte (`sq_frame_end` → `sq_async_join`, depois do `dd_frame_begin`) espera o que faltar, com
    a thread principal ajudando, e entrega o desenho;
  - os resultados valem nesse quadro, como o Async Line Trace da Unreal.
- **Memória**: 64 lotes por quadro, em dois jogos de vagas (par e ímpar).
  - Cada vaga guarda as consultas, os acertos e os registros de desenho, e é reusada dois quadros depois; sem malloc
    depois do primeiro.
  - As consultas são escritas direto na vaga (`sq_async_begin` devolve o espaço).
- **Segurança**: o `sq_sync` (refazer o mundo) e o `sq_reset` terminam antes qualquer lote em andamento.
- **Nós**:
  - Async Line Trace Batch e Async Sphere Trace Batch devolvem um Handle;
  - Get Async Trace Results (Handle) devolve Ready, Hit Count, Hits, Locations, Normals e Hit Actors;
  - o Handle é um número pequeno (exato num float) mapeado para o do lote.
- **Desenho**: pelo próprio worker, no quadro em que o resultado chega (só For One Frame).
- **Teste `trace_nodes`**: o lote assíncrono de 2 linhas, lido no Tick, devolve 2 acertos.
- **Medida** (`--trace-bench 200000` com `REALENGINE_TRACE_BENCH_ASYNC=1`, `house`, a GPU dividida com outra engine):
  - CPU da thread principal 5,1 ms por quadro contra 6,2 ms síncrono;
  - o lote fica fora da thread principal (o custo restante é gerar as consultas no teste).

## Falta
- **Box Trace com orientação**.
- **By Profile**.
- **Trace Complex nos componentes de malha presos a um ator** (o transform deles é local).
- **Async com For Duration ou Persistent**: desenhado só no quadro em que chega.
