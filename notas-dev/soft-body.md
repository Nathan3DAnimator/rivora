# Soft body (Q9, 2026-11-18, pedido de 17/11)

Pedido: "adicionar soft simulation, por exemplo para fazer um sofá, um sistema de soft physics" (lote 3 do plano 0.43:
corpos moles por volume, almofadas que afundam quando alguém senta, colchão, bola de borracha, gelatina, barriga/gordura
presa aos ossos).

## A escolha
- **Volume por tetraedros numa rede de cubos** (o "embedded FEM" dos jogos): a malha do ator fica dentro de uma rede
  regular; cada cubo preenchido vira **6 tetraedros de Kuhn** (diagonal principal comum: os cubos vizinhos casam sem
  paridade). Gerar tetraedros de uma malha qualquer (TetGen) é caro e frágil com malhas abertas; a rede funciona com
  qualquer malha, as cores saem de graça ((x, y, z) mod 2: 8 cores) e a inversa do repouso é **fechada** (linhas
  (e_a - e_b, e_b - e_c, e_c) / célula): nada de repouso lido nem inversa por tetraedro.
- **Vertex Block Descent** (o núcleo do tecido, `cloth_sim.comp`) com **Neo-Hookean estável** (Smith, de Goes, Kim 2018:
  Ψ = μ/2 (I_C − 3) + λ/2 (J − α)², α = 1 + μ/λ; os Lamé de Smith: μ' = 4/3 μ, λ' = λ + 5/6 μ). No passo de Newton de
  um vértice a Hessiana é **exata e positiva sem projeção**: μ |d|² I + λ g gᵀ (J é afim na posição de um vértice).
  Não inverte, conserva o volume pelo coeficiente de Poisson.
- **Contato OGC** como o tecido (o passo nunca passa de 0,45 da distância ao colisor), com a **casca da malha**: cada
  vértice da borda da rede sabe quanto está fora da superfície da malha (`muv.x`) e entra nos colisores por isso: a
  malha, não a rede, encosta.

## Arquivos
- `src/softbody.h/.c`: a rede (`SoftTopo`): amostra os triângulos a 0,4 célula, inunda o fora a partir da borda, as
  células preenchidas, os bits das 8 células de cada vértice, a fixação (Baixo/Faixa/Osso por distância com transição;
  Pintado pelo vermelho das cores de vértice lidas do `.rmesh`), a distância de cada vértice da borda à malha, o lugar
  de cada vértice da malha (LOD 0) na rede; as camadas de preenchimento cortadas (almofada 13×13×6 → 11×11×4). Presets
  e `soft_selftest` (`--softbody-test`).
- `CCloth` cresceu no fim (`kind` = `CLOTH_KIND_SOFT` e os campos `soft_*`): **sem componente novo** porque o ECS está
  nos 64 bits de máscara (`COMP_COUNT` = 64; um 65º pede máscaras de duas palavras). Na UI aparece como **Soft Body**
  (Details > Soft Body, o Outliner).
- `cloth.c`: o corpo mole no `cloth_fill` (`soft_fill_one`), as listas de cor, os buffers `mrest`/`muv`/`mnormal`
  reaproveitados (repouso e peso, deslocamento e bits, amassado), o `sembed` (lugares da malha, duas metades: o layout
  novo escreve a que nenhum quadro em voo lê), a visão para o render (`cloth_soft_view`), o retorno da GPU por quadro em
  voo (`feedback`: forças por colisor, centro e velocidade por corpo), sono, impulso, `cloth_soft_set_prop`.
- `cloth_sim.comp`: `sb_tet`/`sb_cell`/`sb_finish`/`soft_post`, o colisor **caixa orientada** (tipo 4, quaternion: os
  corpos com física e as malhas giradas; 3 vec4 por colisor agora). Variante **`SOFT_BLOCK`** (`cloth_soft.comp.inc`).
- Render: o corpo mole desenha a **própria Static Mesh pelo caminho de skin** (`skin.comp`, ramo `pad == 1`): posição
  trilinear na célula, normal pelo cofator do jacobiano da deformação; material, sombras, BLAS refit, velocidade, tudo
  como um skin. `cull.comp`: instância `INST_SKINNED` só no LOD 0, sem clusters nem o cubo de longe. O skin de um quadro
  lê as posições do quadro anterior (o skin roda antes da simulação: um quadro de atraso, invisível a 200 fps).
- `physics.c` (Q8): só `physics_add_impulse_at` (impulso num ponto) e os corpos moles fora dos colisores estáticos.
- Editor: Details > Soft Body, Adicionar Rápido > Corpo Mole > preset, Ator > Criar Corpo Mole a partir da Seleção,
  Content Browser > Criar > Soft Body (Q6), `editor_softbody.inc` (malhas `SM_SoftCushion`/`SM_SoftCube` geradas,
  cenas, testes). Nós: Add Soft Body Impulse, Set Soft Body Stiffness, Set Soft Body Pinned, Set Soft Body Property.

## O passe em bloco (o que fez a meta)
Por cor, o custo era a corrente: 8 cores × 8 iterações = 64 despachos com barreira, e cada despacho só com os ~60
vértices de uma cor por corpo (latência pura). A variante `SOFT_BLOCK` roda **um workgroup por corpo** (512 threads):
a rede inteira (até 2048 vértices) na **memória compartilhada** (posições e amassados em meia precisão, os 32 colisores
do corpo), todas as iterações e cores de um subpasso **num despacho**, `barrier()` entre as cores. Cada vértice usa
**8 lanes**: os seus 24 pares (célula, tetraedro) divididos 3 por lane, somados com `subgroupClusteredAdd` (sem
suporte: o caminho por cor). Depois a velocidade, o amassado e o retorno no mesmo despacho. Uma rede maior que 2048
vértices vai pelo caminho por cor (`REALENGINE_SOFT_BY_COLOR=1` força).

## Acoplamento com os corpos rígidos
- A GPU soma, na última iteração de cada subpasso, a força **quase-estática** que cada vértice em contato faz no
  colisor (elasticidade + peso do vértice, só na normal) e o momento; a CPU lê 2-3 quadros depois (o quadro em voo) e
  aplica um **impulso no ponto** da linha de ação (`physics_add_impulse_at`). A inércia dos vértices ficava fora: com o
  atraso, o pico do impacto lançava a caixa para cima (1000 N, subiu 40 cm) e a fazia escorregar.
- Por isso o **amortecimento do contato vai na CPU com a velocidade atual** do corpo (sem atraso): 30/s ao longo do
  empurrão, 8/s de lado. Um corpo dormindo só acorda se o empurrão passa do próprio peso.
- Personagens: as cápsulas e os ossos amassam o corpo mole, mas não sentem a reação (o CMC não é um corpo de física).

## Sono e LOD
Parado (o vértice mais rápido abaixo de 1 cm/s por 45 quadros, lido do retorno), sem colisores mexendo por perto
(hash dos colisores) e sem mudar os campos: dorme, nenhum vértice anda; todos dormindo, nenhum despacho. Longe da
câmera (Distância para Dormir, 60 m) também dorme. No editor fora do Jogar os corpos moles ficam no repouso (como a
física); entrar e sair do Jogar recomeça.

## Medidas (RTX 3060, 1440p, `--play`, `--gpu-passes`)
| Cena | Antes | Custo do passe "tecido (simulacao)" |
|---|---|---|
| `sofa` (3 almofadas de 484 vértices, 5400 tetraedros; alguém sentado, o Mannequin pulando) | por cor, repouso lido: 1,87 ms | bloco 1×64: 1,18 ms; 8 lanes, rede cortada, colisores na compartilhada: 0,58 ms; **6 iterações: 0,465 ms** |
| `softbody_rigid` (1 almofada, caixa de 25 kg) | 1,30 ms | 0,41 ms (8 iterações); afunda 5,8 cm e para |
| `softbody` (gelatina 1331 vértices, bola 991) | 1,23 ms (71 despachos) | 1,01 ms (bloco); volume 97,2% e 100% |
Por iteração ~0,06 ms por corpo (as 8 cores de um corpo num SM); os corpos rodam em SMs diferentes ao mesmo tempo.
A cena `sofa` inteira: 383 fps (2,6 ms de GPU). Com outra sessão usando a GPU o passe mediu 0,58 ms (fatias de tempo).
Caixa na almofada: afunda 6 cm, um tremor de ~3 mm sobra (o empurrão lido 2-3 quadros depois): o teste mede a altura
no último segundo (< 1,5 cm).
O tecido ficou ~0,1 ms mais caro no `cloth_mesh` (colisor de 3 vec4): ver tecido.md. Rede da almofada: 1,9 ms de CPU uma vez (por malha e ajuste).

## Bugs achados
- Metade dos tetraedros de Kuhn tem orientação negativa: o volume com sinal somava zero (`soft_volume` usa o sinal do
  repouso; o solver usa F = Ds Dm⁻¹, indiferente à orientação).
- A primeira medida do sofá deu 103 ms: outra sessão usava a GPU (a fatia de tempo por despacho); repetida, 1,9 ms.

## Falta
- Personagens sentirem a reação (sentar de verdade: o CMC parar no corpo mole amassado); pose de sentado do Mannequin.
- Casca com pressão interna (balões) e tetraedros conformes à malha (TetGen) para malhas finas; malhas muito finas
  pedem mais células.
- Normais suaves entre células (hoje o jacobiano trilinear: C0 entre células).
- Amassado em corpos livres (sem um referencial: hoje só nos presos).
