# Escala: milhões de atores (render.c, cull.comp, tlas_fill.comp, rt.glsl)

Meta do pedido: milhões de objetos e mais de 15 milhões de vértices na cena sem travar nem derrubar o FPS.

## Arquitetura
- **Cena persistente na GPU**: um slot por entidade (índice do handle), `GpuInstance` de 112 B no buffer `R.scene`,
  flags `INST_ALIVE`, `INST_DYNAMIC`, forma em `ids[1] >> 16`. Nada é reenviado por frame: o ECS mantém um **log de
  mudanças** (anel de 4096 + `revision`); o render manda só os slots tocados (cópias por região de um staging por
  frame em voo). Revisão nova (abrir nível, desfazer, snapshot do PIE) ou anel estourado: reconstrução inteira.
- **Conjunto dinâmico** (Movable e Rotating): cópia na CPU de cada instância; só o que mudou (memcmp) vai para a GPU.
- **Culling na GPU** (`cull.comp`, 6 passes, 64 threads, contagem por workgroup em memória compartilhada: um atômico
  global por workgroup e draw): frustum, LOD pelo raio na tela (LOD 0 acima de 60 px, ×0,4 por nível, abaixo de
  0,5 px some), caixa da sombra; somas de prefixo; listas visíveis. **Draws indiretos**: 5 formas × 4 LODs = 20 por
  lista (principal, sombra). Esfera/cilindro/cone com 4 LODs (48/24/12/8 segmentos).
- **Dois TLAS**: o estático (instâncias escritas na GPU por `tlas_fill.comp`, reconstruído só quando a cena estática
  muda, refit quando só transformações mudam) e o dinâmico (CPU, reconstruído quando o conjunto muda). Um raio
  consulta o estático e só consulta o dinâmico se cruzar a caixa dele (`hits_dyn`, teste de slabs: a maioria não
  cruza e economiza uma travessia a partir da raiz).
- **Outliner**: lista de linhas em cache, refeita só quando o log de mudanças avança (antes O(n) por frame).
- **Oclusão em duas fases** (prepass + Hi-Z em buffer + lista tardia) **automática** (`occlusion_auto`, como o Nanite:
  só paga onde ajuda): a cada 3 s mede o tempo de GPU do próprio frame 24 frames sem e 24 com (4 de espera: os frames
  em voo) e fica com o mais rápido (com só se 3% melhor). A imagem é a mesma (as duas fases são conservadoras).
  `REALENGINE_OCCLUSION=0/1` força. Medidas (RTX 3060, 1440p): casa 364 → **448 fps** (GPU 2,52 → 2,07 ms: cada pixel
  sombreado uma vez, o que está atrás das paredes não é desenhado); nível padrão fica sem (1,70 contra 1,95 ms): 541
  fps; 1 M atores fica sem: 177 fps (com seria 169). A linha "Scene:" do --bench mostra a escolha e a medida.

## O crash do driver (NVIDIA 596.36) — bug do driver, corrigido no 617.42
**No driver 617.42 não crasha mais**: a bateria inteira (a reprodução que dava 6/6, 1 M atores `--gi full`, com 40 k luzes
projetando sombra, 1 M luzes, nível padrão) deu 0 crashes em 26 execuções. Sem a espera antes de gravar o frame também: 0/26.
Com a GPU livre (medidas anteriores tinham o Tlender aberto e uma tela compartilhada usando a GPU), sem a espera é
mais rápido em tudo: nível padrão 471 contra 411 fps, `--gi off` 1070 contra 910, 1 M atores `--gi full` 178 contra
164, 1 M luzes 177 contra 159. Então a espera só fica em drivers NVIDIA anteriores ao 617 (`serialize_frames` no
`vkctx.c`; `REALENGINE_SERIALIZE_FRAMES=1` força). Abaixo, a investigação no 596.36.

Sintoma: leitura em nulo+0x10 numa thread do próprio driver (`nvoglv64.dll+0x8535c`), ~2,7 s (≈490 frames de GPU
cheia) depois de abrir; validação, sincronização e GPU-AV limpos; sem Xid/TDR.

**A thread é a de compilação de shaders em segundo plano do driver Vulkan da NVIDIA**: a função que a cria tem a
string `[vkbgc] Compilation Thread` (dumpbin no `nvoglv64.dll`), e o crash é num laço sobre registros de 256 bytes do
compilador (`mov rcx,[rax+10h]` com `rax = [rdi+18h] = 0`). Depois de uns segundos de uso pesado, o driver recompila
um shader quente e o compilador dele quebra. Por isso: o tempo quase fixo, `sleep(20 ms)` por frame que evita, e a
dependência de *quais* recursos estão ligados (o código especializado muda) — não de um acesso inválido nosso.

Reprodução determinística (6/6): `realengine --stress-objects 1000000 --gi probes --stress-lights 40000 --bench 6`.
Bissecção nela: precisa do laço da grade de luzes **e** das sondas da GI no `mesh.frag` (tirando um ou outro: 0/3);
o laço só lendo os itens, sem BRDF, ainda crasha. Não resolveram: laço único, laço plano, sondas sem laço
(desenroladas à mão), `texelFetch` nas sondas, `[[dont_unroll]]`, `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`, 4
cópias do pipeline alternadas, endereço do frame alternado, camadas implícitas desligadas. Ajudaram e ficaram:
- sem vetores indexados por variável na grade (`lights.glsl`): 12/12 → 5/12 num caso;
- esperar a GPU antes de gravar o frame (`vkctx_wait_submitted` no `render_scene`), ~0,35 ms de CPU;
- raio válido sempre (`ray_ok` no `rt.glsl`: tmin ≤ tmax, origem/direção finitas — o spec diz indefinido).
Taxas agora (6 s cada): nível padrão 0/6, 1 M luzes 0/4, 400 k atores `--gi full` 0/4, 1 M atores `--gi full` 4/6,
1 M atores + 40 k luzes 6/6.
- Tentativa guardada no branch local `rt-compute-experiment`: sombras/reflexos em compute depois de um pré-passe
  com G-buffer (sem ray queries no fragment; um shader para cada efeito). Tirou o `--gi full` de 1 M atores para 0/8,
  mas é ~25% mais lento (padrão 330 contra 430 fps) e o caso com luzes continua crashando: não entrou.
- Opções do driver testadas num perfil só do `realengine.exe` (NVAPI DRS, perfil apagado depois; o perfil vale:
  o limitador a 60 fps pegou), cada uma 3–4× na reprodução: Offline Shader Compile 0, Threaded optimization Off,
  Shader Cache Off, Power management "máximo desempenho", Vulkan/OpenGL present method 1 — todas ainda crasham. O
  driver só expõe 125 opções com nome; a da compilação em segundo plano não é uma delas.
- Relato para a NVIDIA escrito e não enviado: o 617.42 já corrige.
- Teste `scale_rt` (ctest, 1 M atores, `--gi full`): passa no 617.42; no 596.36 falhava às vezes.
## Medidas (RTX 3060, 2560×1417, MSAA 4x, Alto, sem vsync)
| Cena | fps | GPU (ms) |
|---|---|---|
| Nível padrão (sombras por raio) | 451 | cena 0,05 · GI 0,16 · principal 1,35 |
| Nível padrão, `--gi off` / `--gi probes` | 885 / 497 | principal 0,54 / 1,17 |
| 0.3 publicada, nível padrão (sem a espera) | 517 | — |
| 400 k atores, `--gi full` | 215 | cena 0,18 · principal 3,35 |
| 1 M atores, `--gi full` (194 k desenhados, 6,8 M triângulos) | 178 | cena 0,38 · GI 0,20 · principal 4,4 |
| 1 M atores, só reflexos / só sombras por raio | 195 / 188 | principal 3,4 / 4,1 |
| 1 M atores, `--gi off` / `--no-rt` | 316 / 322 | principal 1,9 / 2,4 |
| 1 M atores, Path Tracing | 186 | — |

Antes da cena persistente, 1 M atores custavam 724 ms de CPU por frame; o culling na GPU custa 0,38 ms.

## Relevância (2026-10-30): nada que não aparece na vista custa
Pedido: se nem o objeto, nem a sombra, nem o reflexo, nem a luz indireta dele aparecem, não renderizar (malha, luz,
e no futuro armatures e animações).
- `cull.comp` passo 0 grava **um bit por slot** (`relevance`): visto, com sombra que cai na vista (a esfera varrida
  na direção do sol cruza o frustum) ou a menos de `RE_GI_RANGE` (60 m: ilumina a vista ou aparece em reflexo). A CPU
  recebe a cópia do frame terminado: `World.relevant` + `ecs_relevant()` para os sistemas (animação/armature vão
  pular ou desacelerar o que não é relevante); o **TLAS dinâmico** deixa de fora os Movable irrelevantes.
- Sombra por mapa: só entram os que projetam sombra **para dentro da vista** (varredura na direção da luz).
- **Luzes locais**: fora da grade se o alcance não toca a vista nem os 60 m da GI.
- Medidas: 1 M luzes 177 → 194 fps; 1 M atores `--gi full` 178 → 181; nível padrão 574 fps.

## 20 mil Movable (2026-11-19, sobra do Q8: notas-dev/fisica.md "20 mil corpos")
- A comparação de cada instância do conjunto dinâmico com a última enviada foi para a parte paralela (`dyn_one`): uma
  instância igual à enviada (parada) só soma a caixa no laço serial. Render CPU com 20 mil corpos (a maioria
  parada): 3,0 → 1,9 ms.
- O **TLAS dinâmico é refeito por refit** quando os membros são os mesmos (só se moveram; hash de slot + BLAS na ordem)
  e reconstruído quando mudam ou a cada 30 refits (a árvore se afasta de onde as coisas estão): "cena (TLAS, culling)"
  1,15 → 0,84 ms no armazém de 20 mil.

## Próximos passos
- Milhões de luzes: feito (notas-dev/luz.md); falta a amostragem estocástica.
- Driver mínimo recomendado: NVIDIA 617.42 (o crash acima); avisar no editor quando o driver for mais antigo.
- Recuperar a sobreposição CPU/GPU (gravar a UI num command buffer próprio, ou reavaliar num driver novo).
- Reavaliar a oclusão em duas fases em cenas com muitos oclusores (agora sem o crash).

## Vista de cima de um milhão de objetos (2026-10-31)
Relato: a cena de 1 M objetos vista de cima ficava lenta. Medido (`--stress-objects 1000000 --gi full`, RTX 3060, 1440p):
| Vista | Antes | Depois |
|---|---|---|
| Normal (194 k desenhados) | 140 fps | 155 fps |
| De cima a 200 m (11 k + o chão) | 249 fps | 331 fps |
| De cima a 1 km (218 k de 1–3 px) | 86 fps (7,6 M triângulos) | 128 fps (2,6 M) |
- LODs mais grossos para o que é pequeno na tela (esfera/cilindro/cone: segmentos 48/20/10/6, anéis 24/10/5/3) e LOD 0
  só acima de 48 px de raio.
- Abaixo de 2 px de raio, as formas redondas são desenhadas com o cubo (12 triângulos): a forma não aparece nesse tamanho.
- Abaixo de ~3 px na tela, nem raios de sombra nem de reflexo (cada pixel paga um quad 2x2 inteiro).
- Raio de oclusão dos reflexos ásperos só a até 40 m da câmera.
- O que resta a 1 km é rasterizar 218 k instâncias minúsculas (não o sombreamento): o próximo passo é um rasterizador por
  software para micro-triângulos (como a Nanite) ou impostores agrupados por célula.
