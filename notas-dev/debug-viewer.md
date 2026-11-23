# Debug Viewer (`debug_viewer.c`)

Pedido: uma janela, no editor e no jogo, que mostra o que pesa na cena — malhas, iluminação, animação, shaders e a lógica
de Blueprints —, uma lista por sistema com o custo na frente e, dentro, os objetos da cena com o custo de cada um (os
pesados em vermelho); o custo do próprio viewer à vista; nada consumido enquanto fechado; um caminho pronto para os
sistemas que vierem (folhagem, simulação...).

## Uso
- **F10** (editor e jogo empacotado) ou Janela > Debug Viewer. `--debug-viewer` abre no início; com `--bench` as listas
  saem no log (`dv_dump`). No editor, clicar num item seleciona o ator. No jogo o cursor fica livre enquanto ele está
  aberto (o olhar espera) e o ImGui só é criado na primeira vez que ele abre (`ui_init` em `game_main.c`).
- Cores: lista vermelha acima de 1 ms ou de 1/3 do quadro, amarela acima de 40% disso; item vermelho acima de 0,5 ms ou de
  40% da lista. `≈` = medida total repartida por um modelo de custo.

## Listas
| Lista | Total | Itens |
|---|---|---|
| Malhas | cull na GPU + mapa de sombra + malhas no passe principal (timestamps), menos a parte das luzes | cada ator visível: triângulos do LOD que o cull desenhou (+0,3× os da sombra) + 64 por instância + 200 000 × a parte da tela (esfera envolvente / 2) |
| Iluminação | GI (cache, sondas, contato/bake), atmosfera, clima, exposição (medidos) + a parte das luzes no passe principal | os subsistemas medidos; cada luz: parte da tela que o raio de atenuação cobre × (1 + 3 se faz raios de sombra), contra 4 por pixel do material |
| Shaders | o GPU do quadro | cada passe com o seu tempo (mesh.frag, céu/grade + resolve do MSAA, cull, sombra, cache, sondas, clima, atmosfera, contorno, exposição, composite); os materiais entram aqui quando existirem |
| Animação | — | registrada de verdade pelo sistema de Skeletal Mesh (`dv_register("Animation", ...)` substitui a padrão) |
| Blueprints | `bp_stats` (CPU) | cada instância: Tick + Delays medidos (QPC) e operações da VM; o movimento dos Characters |
| Sistemas de Jogo | soma dos estágios | cada estágio do job system (os sistemas que rodam juntos) |
| Frame Budget | GPU das fatias | as fatias do orçamento de 10 ms com a meta de cada uma (`frame_budget.c`, notas-dev/orcamento.md) |
Cabeçalho: fps, quadro, CPU, GPU e **o custo do próprio viewer** (coleta a cada 250 ms + a janela).

## Medir só aberto
`dv_frame` liga/desliga ao abrir/fechar: `render_set_detail` (timestamps 8–13: atmosfera, cena/cull, clima, sondas, cache,
fim das malhas; leitura de volta do `vis` por slot — LOD desenhado e sombra — só quando a lista pede, copiada para a CPU) e
`bp_profile` (cronômetro por instância). Fechado: nada disso grava nem lê.

## Novo sistema
```c
static void collect_foliage(DvList* l, const DvContext* c) {
    l->gpu = true; l->ms = foliage_gpu_ms();
    for (each instance group) dv_add(l, name, info, ms, entity);
}
dv_register("Foliage", collect_foliage); // na inicialização do sistema
```
Itens ilimitados: o viewer guarda os 256 mais pesados (heap) e soma o resto ("+ N outros").

## Medidas (RTX 3060, 2560×1417, `--scene house --bp-walkview`, 3 s)
| | fps | CPU ms | GPU ms |
|---|---|---|---|
| Fechado | 391 | 0,71–0,75 | 2,48 |
| Aberto | 382–384 | 0,79–0,81 | 2,52–2,54 |
Coleta 0,02 ms a cada 250 ms; janela 0,02 ms por quadro. Teste `debug_viewer` (validação, > 350 fps, listas preenchidas).

## Custo por Objeto no 3D (sugestão #65, 2026-11-04)
Modo de visualização **Custo por Objeto** (View Mode, `--view objcost`): cada objeto do viewport pintado pelo que custa
(verde leve, amarelo, vermelho pesado; escala logarítmica de 0,01 ms a 0,5 ms, o "pesado" dos itens do viewer) e, no
canto inferior direito, o painel do objeto sob o mouse (senão o do meio da tela): o total e cada parte com barra —
Malhas (LOD, triângulos, parte da tela, sombra), Animação, Blueprint, Física, IA, Luz, Construction Script — e o que
não é medido por objeto: as texturas do material (lado e MB) e como a GI dele é feita (Estático/Móvel).
- **Os custos são os do Debug Viewer** (`editor_cost.c`): enquanto o modo está ligado ele mede como se estivesse aberto
  (`dv_costs`: timestamps finos, `bp_profile`), coleta 4× por segundo e soma por ator (o componente para o seu ator).
- **Na GPU**: um float por slot da cena (`render_object_cost_map`, memória visível ao host, criada no primeiro uso; o
  endereço no lugar do antigo `mat_pad` do `GpuFrame`), lido pelo `mesh.frag` no modo novo. Só os slots escritos da
  última vez são zerados; nada por quadro além do painel. O objeto sob o mouse é escolhido 10× por segundo.
- Desligado: nada coletado nem medido (o custo de sempre). Teste `object_cost` (`--validation`, > 250 fps, o mapa
  preenchido: "Object cost: N actors").

## Trace, como o Unreal Insights (B14b, 2026-11-09)
Gravação de **cada quadro** num arquivo, para achar a travada que aparece de vez em quando (o Debug Viewer só mostra o agora).
- `src/trace.c/.h` (re_core): um buffer de `TRACE_MAX_EVENTS` (2 M eventos, 24 B cada) criado no `trace_start`; cada evento
  pega o slot com um `re_atomic_add`. Sem malloc no quadro. Desligado, `trace_t0()` é uma leitura de bool.
- Escopos: `TRACE("Nome", chamada)` ou `trace_t0()` + `trace_scope(nome, t0)`; nome é string estática. Na thread do jogo:
  Online, Systems, AI, Blueprints, Physics, Vehicles, Animation, Crowds, World Partition, Plugins, Particles, Audio, Editor
  Frame, Wait for GPU, Render (record scene), Submit and Present, e o `Frame`. Os workers: `g_re_pool_span` (platform.h) em
  `pool_take_items` grava a parte de cada thread num `re_parallel_for` ("Parallel Work").
- GPU: `trace_frame` põe os passes do `render_stats()` (timestamps do quadro anterior) em sequência na trilha GPU; cada
  quadro da GPU começa onde o anterior terminou (o quadro da GPU é mais longo que o da CPU quando a GPU limita; atrasou
  mais de 50 ms: volta ao início do quadro). É a duração medida, não o instante em que começou.
- `trace_stop` passa o buffer para uma thread que escreve o JSON (trace do Chrome, um evento por linha) e libera.
  `trace_load` lê de volta (só o formato que escrevemos), ordena por trilha/início e calcula a profundidade de cada escopo.
- Janela (`trace_view.c`, Ferramentas > Trace (Insights)): gráfico de quadros (escala no percentil 99 × 1,6, para o
  primeiro quadro longo não achatar), linha do tempo (GPU em cima, thread do jogo, workers; roda = zoom no mouse, arrastar =
  mover, Shift+roda = rolar trilhas), Timers do trecho visível (contagem, total, média, máx.). Arquivos em
  `<projeto>/Saved/Profiling/trace_AAAAMMDD_HHMMSS.json`; também abrem no ui.perfetto.dev.
- `--trace ARQUIVO` (editor e jogo): grava do início ao fim; na saída lê de volta e imprime
  "Trace: N events, M frames, K tracks...". `--trace-open ARQUIVO`: abre na janela (teste).
- Testes: `trace_record` (crowd2000 em Play 4 s: trilhas do jogo, workers e GPU) e `trace_window` (a janela desenhando o
  arquivo durante a medida).
- Medida (RTX 3060, 2560×1417, crowd2000 em Play, 3 s): sem trace 276/269 fps, com trace 276/276 fps (dentro do ruído).
  4 s geram ~45 mil eventos e ~3,6 MB de JSON; escrita numa thread de baixa prioridade.
- Falta: marcadores com nome por objeto (Blueprint por classe), contadores de memória, comparar dois traces, trace por
  rede (gravar o jogo de outro PC).

## GPU por passe, como o `stat gpu` da Unreal (2026-11-13, estudo de desempenho)
Os marcadores de crash da GPU (`gpu_mark`, ~45 no quadro: céu, TLAS, nuvens, neblina, GI, sombras, pré-passe, malhas,
céu e grade, denoiser, TSR, pós...) viraram também o perfilador por passe.
- `vkctx.c`: com a medida ligada, cada `gpu_mark` escreve um timestamp `ALL_COMMANDS` (o fim de todo o trabalho antes
  dele) num pool próprio (128 por slot); o passe vai da sua marca até a próxima, o último até o fim do quadro. Lido no
  `vkctx_begin_frame` do mesmo slot (o quadro já terminou), somado por nome (`vkctx_pass_stats`). Desligado: nenhum
  timestamp, nada lido (o pool existe, vazio).
- Liga: `--gpu-passes` / `REALENGINE_GPU_PASSES=1` (a medida imprime "GPU passes (N frames, X ms a frame): ..." do mais
  caro ao mais barato) e o **Debug Viewer > GPU Passes** (só enquanto aberto; a média desde a última coleta).
- Um passe novo aparece sozinho com um `gpu_mark(cmd, "nome")` antes dele. Partes finas de um passe (as do clima, as
  do passe principal: malhas, céu e grade, denoiser) usam `gpu_mark_detail`: só existem com a medida ligada (cada
  marcador de crash espera o trabalho anterior: 9 a mais custavam ~0,05-0,1 ms de bolhas na GPU).
- Como o `stat gpu`, a sobreposição entre passes vizinhos cai no passe da marca anterior; a soma é o quadro.
