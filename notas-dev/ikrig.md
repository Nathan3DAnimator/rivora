# IK Rig e física dos ossos (2026-11-07, pedido do usuário)

Pedido: "um sistema de IK Rig onde o usuário configura o IK de personagens e monstros, com IK de várias partes, não
apenas 2 ossos; que cubra IK, target e ossos com física em tempo real, bem avançada, sem bugs; e uma seção separada
para a colisão dos ossos igual à da Unreal, possibilitando ragdoll".

## Arquivos
- `src/ikrig.c/h`: os assets (`.rikrig`, `.rphys`, JSON), os solvers, a simulação, o estado por instância, o autoteste.
- `src/ikrig_editor.c/h`: os dois editores (uma janela: lista à esquerda, prévia 2D do esqueleto no meio, propriedades à
  direita), o "Adicionar > IK Rig e Física" do Content Browser, a parte dos Detalhes do Skeletal Mesh.
- `CSkeletalMesh` ganhou no fim: `ik_rig`, `physics_asset`, `simulate` (0 não, 1 ragdoll, 2 abaixo de um osso),
  `simulate_bone`, `physics_blend` (níveis antigos abrem: o componente cresce no fim).
- `anim.c pose_one`: depois da pose (e do Foot IK), antes de colocar as partes e as skins: `ikrig_apply`, depois
  `phys_apply` (só jogando). Os assets são carregados e o estado criado na thread principal (`ikphys_prepare`), os jobs
  de pose só leem.

## IK Rig (`.rikrig`)
- Metas: um osso e um alvo; o alvo segue um ator pelo nome (+ deslocamento, + rotação opcional) ou vem do nó Set IK Goal.
- Solvers em ordem: Chain (FABRIK, N ossos, pole), Limb (2 ossos), Full Body (FABRIK em árvore: várias pontas de uma
  raiz; a raiz anda por `1 - root_stiffness`; ossos compartilhados ficam na média das cadeias), Look At.
- Por osso: rigidez (mistura com a direção animada) e limite (cone a partir da direção animada).
- Depois do FABRIK cada osso gira para seguir o seu segmento; os descendentes vêm junto pela transformação local.
- `ikrig_create`: acha mão/pé (lados por `_l/_r/left/right/.l`), cabeça e bacia pelos nomes: Full Body + Look At.

## Physics Asset (`.rphys`)
- Corpos: cápsula (do osso ao filho mais longe), esfera (cabeça, pés na ponta, "hubs" com 3+ filhos: bacia e peito, no
  meio dos filhos), caixa. Ossos curtos, de torção, dedos, olhos e mandíbula ficam no corpo de cima.
- Juntas: ao corpo de cima mais próximo; swing por tipo (joelho/cotovelo 70, coluna 20, pescoço/cabeça 40, mão/pé 35,
  resto 60), twist (10 ou 30).
- Massas igualadas ao longo das juntas (vizinho >= 1/4 do outro).

## Simulação (XPBD, 8 subpassos, 1 passada)
- Junta: a cabeça do osso filho presa à âncora no pai (posição), swing (cone do eixo do filho a partir da referência no
  pai), twist (perpendicular do filho em torno do eixo). Inércia certa da cápsula (em torno do próprio eixo, não do
  eixo Y do osso) e no mundo uma vez por subpasso.
- Colisão: esferas nas pontas e no meio de cada cápsula contra as caixas do nível (`bp_world_boxes`, a mesma grade da
  VM), o chão (landscape ou os pés) e entre corpos (pares próximos escolhidos uma vez por quadro). Fora: vizinhos, avôs
  e pares que já se tocam na pose inicial.
- Atrito estático pelo ponto de contato (girado com o corpo), desinterpenetração limitada (0,5 cm por subpasso além do
  que o corpo entrou: como o Max Depenetration Velocity), amortecimento no ar, no chão e nas juntas, guarda de
  velocidade (40 m/s, 50 rad/s), sono depois de 1 s parado.
- Animação física: cada corpo puxado para a pose animada pela `strength`. Mistura com a animação pelo `blend`.

## Bugs achados no caminho
- Inércia da cápsula no eixo Y do osso: giravam fácil demais e explodiam (39 m/s). Tensor pelo eixo da cápsula.
- Atrito pelo centro do corpo (ignorava o giro): tremor eterno no chão. Atrito pelo ponto de contato.
- Coluna do Mannequin em cápsulas de 0,5 kg entre a bacia (6 kg) e o peito (14 kg): a bacia girava no limite. Massas
  igualadas; o tronco "grosso" (raio pelo corpo) piorou (pares a 2 juntas brigando com os limites): não usado.

## Blueprints (7 nós, Animation|Physics e Animation|IK Rig)
Set Simulate Physics (Ragdoll), Set All Bodies Below Simulate Physics, Set Physics Blend Weight, Add Impulse to Bone,
Is Simulating Physics, Set IK Goal, Clear IK Goal. Target: o ator ou o seu Skeletal Mesh (`anim_mesh_entity`).

## Medidas (RTX 3060, 1440p)
| Cena | Resultado |
|---|---|
| `--scene ragdoll` (6 ragdolls, 3 abaixo de spine_03, 3 IK, validação) | 467 fps; IK/física 0,55 ms de CPU (180 corpos) |
| antes das otimizações (2 passadas, todos os pares, inércia recalculada) | 1,46 ms |
- Ragdolls em repouso no chão e no degrau (1 a 10 cm/s depois de 6 s, vários dormindo), mãos a 0,00 cm das metas.
- Testes: `ikphys_selftest` (cadeia de 3, limite de cone, ragdoll de 4 ossos cai e para, swing dentro do limite,
  projeção do editor), `ikphys_ragdoll` (a cena, validação, 200 fps).

## Limites / próximos passos
- Juntas sem molas (drives) próprias: a animação física é um puxão por corpo. Sem colisão com malhas (só caixas,
  landscape e o chão). (A prévia do editor já é o viewport 3D do asset desde o Q7.)
- O ragdoll de um Character não desliga o movimento dele: no Blueprint, desative a entrada/movimento ao morrer.

## Ragdoll Ativo (2026-11-16, "um sistema de Ragdoll tipo Euphoria ... que ainda tenta seguir a animação de acordo com um peso")
- **Modo `PHYS_ACTIVE` (simulate 3)**: todos os corpos simulados. A cada subpasso, cada junta gira o filho para a rotação
  animada relativa ao pai (`trel`, feita uma vez por quadro): uma fração `1 - exp(-k·45·h)` do erro, limitada a `k·14`
  rad/s (o torque). Assim um golpe vence por um momento e o músculo traz de volta. `k` = força geral × do osso
  (Set Active Ragdoll Strength, com tempo) × golpe (Apply Hit Reaction, volta com o tempo) × Músculo do corpo no Physics
  Asset × perfil (pernas, coluna, braços, cabeça, resto).
- **Equilíbrio**: com o equilíbrio cheio (`support > 0,5`), a raiz (o corpo sem pai: a bacia) segue a animação como
  cinemática e os membros pendem dela com os músculos. Com o equilíbrio fraco, ela vira dinâmica, puxada para cima e de
  pé com limite. Os pés, achados sozinhos (as pontas mais baixas: 2 pés, 4 patas), ficam plantados enquanto está em pé.
  Os músculos que saem da raiz segura não a giram (a reação ia girar a bacia).
- **Estados**: Balanced → Stumbling (raiz a mais de 28 cm ou 35° da animação: um passo, o equilíbrio cai em 0,7 s) →
  Balanced (se segurou: o ator vai para onde parou) ou Fallen (raiz abaixo de 55% da altura, ou 1 s) → parado no chão
  (velocidade da raiz suavizada < 45 cm/s, músculos soltando: 0,6 → 0,15 → 0,03) → Getting Up (o ator vai para onde
  caiu, mistura de física para animação em 0,8 s) → Balanced. Nunca fica preso no chão: levanta no máximo `delay + 4 s`.
- Um golpe derruba conforme a força: o equilíbrio × `(1 - |impulso| / 900)`, e o corpo todo leva 35% do impulso.
- **Thread principal** (`ikphys_main`, em `anim.c` antes dos jobs de pose): move o ator (um passo, o levantar) e segura
  o movimento do Character enquanto ele não está de pé.
- **Correção no ragdoll comum**: uma esfera "hub" (bacia) de poucos cm com as juntas a 15 cm girava muito a cada correção
  e nunca parava no chão (2000 cm/s). Agora a inércia é no mínimo a de uma esfera que alcança as juntas, e o bípede
  parado no chão fica a 2 cm/s.
- **Nós** (Animation|Active Ragdoll): Set Active Ragdoll, Set Active Ragdoll Strength, Set Muscle Profile, Apply Hit
  Reaction, Get Balance State, Is Balanced, Get Up, Set Auto Get Up. Detalhes: Simular Física > Ragdoll Ativo. Physics
  Asset: Músculo por corpo (autogen: mãos 0,6, braços 0,75, cabeça 0,8, cauda 0,5, o resto 1).

| Medida (RTX 3060, 50 Mannequins, `--scene active_ragdoll`) | CPU | fps |
|---|---|---|
| sem física (o "antes") | 1,65 ms | 374 |
| ragdoll ativo, 8 subpassos sempre | 4,73 ms (física 3,06 ms somados) | 210 |
| + metade dos subpassos em pé com equilíbrio cheio, tipos de osso em cache | 3,24 ms (física 1,81 ms) | 307 |
- Autoteste (`--ik-test`): um bípede (16 corpos, 2 pés) e um quadrúpede (13 corpos, 4 patas) feitos no código ficam de
  pé 3 s (raiz a 1,6 e 0,8 cm da animação), caem com um golpe forte, levantam sozinhos, balançam com um leve sem cair;
  0,034–0,040 ms por quadro. Cena `active_ragdoll` (50, um golpe a cada 0,5 s), teste `active_ragdoll` (200 fps).

## Ragdoll realista, ragdoll ativo que se protege, ossos dinâmicos, roupas e sockets (Q10, 2026-11-18)
Pedido (17/11): física com ênfase "a roupa de personagens e a acessórios que ficariam no personagem, às vezes simulados
por bones na armature", e "melhorar o sistema de ragdoll para ser realista". Aqui é o ragdoll ativo leve (o Euphoria
completo para humanos e quadrúpedes segue guardado para quando o usuário pedir).

### Limites anatômicos (`PhysConstraint`, `phys_joint_defaults`, `joint_swing`, `joint_twist`)
- **Tipo de junta** (`PJ_*`): Dobradiça (joelho, cotovelo), Coluna, Pescoço, Ombro, Quadril, Pulso / Tornozelo, Cauda,
  Esfera. Gerar Corpos acha o tipo pelo nome do osso e põe os limites; o editor mostra Dobra, Lados, Torção (mín/máx).
- Os limites são medidos **da pose de bind** (o quadro da junta: o eixo do filho, a direção da dobra `bend` no corpo pai,
  a dobradiça `= eixo × dobra`), não da pose em que o ragdoll começa (antes: um ragdoll que começava agachado tinha o
  "zero" agachado). O balanço do filho vira um vetor pelo log map (dobra × lados); a torção é a parte em torno do eixo
  da rotação relativa desde a bind (decomposição swing-twist).
- Direção da dobra: o joelho para trás e o cotovelo para a frente pelo **forward do esqueleto** (dos pés para os dedos;
  sem pés, da raiz para a cabeça); uma junta já dobrada 8° ou mais na bind dobra para onde já está. Ombro: flexão para a
  frente até 150°, extensão 45°, abdução pela elevação do braço na bind (caído, pose A ou T: o mesmo alcance).
- Arquivos antigos abrem com o cone e a torção simétricos de antes (um círculo, como era); `tone 0`, `damping 6`.
- **Massa pelo volume** (cápsula, esfera, caixa × Densidade, 1,0 g/cm³), depois igualada ao longo das juntas
  (`phys_masses_from_volume`). O bípede do teste: 104,8 kg.
- **Autocolisão**: os pares fora são vizinhos, avôs e os que se tocam **na bind** (antes: na pose inicial; duas pernas
  cruzadas na queda deixavam de colidir).
- **Amortecimento por junta** (1/s, por tipo) aplicado **a cada subpasso** e **tônus** do ragdoll comum (puxa de leve
  para a pose em que caiu, some com `exp(-t / 1,2 s)`): cai como corpo, não como trapo, e assenta no chão.

### Bugs achados no caminho
- **Projeção radial numa elipse fina**: o joelho (0..140° × ±4°) projetado para o centro da elipse saltava dezenas de
  graus na dobra a cada correção: tremor eterno no chão. Agora cada eixo preso no seu intervalo (o "pyramid" do PhysX).
- **Torção por perpendicular projetada**: vira 180° quando o membro passa de 90° (a coxa dobrada): a torção "passava"
  147° do limite. Agora swing-twist em relação à rotação relativa da bind.
- **Amortecimento das juntas uma vez por quadro**: as velocidades dos subpassos seguintes saem das posições, então valia
  ~1/8; uma cauda balançava por segundos. Agora em cada subpasso.
- **Cauda com ±35° por segmento** brigava com o chão em que estava deitada (presa entre o limite e o contato): ±60°.
- **Os limites depois dos contatos**: a cauda ou a coluna empurradas pelo chão passavam até 12° dos limites; agora os
  limites rodam de novo depois das colisões, até 4 passadas de Gauss-Seidel, parando quando uma passada corrige < 0,33°.
- Quadrúpede de pé com o golpe forte (900 cm/s): o equilíbrio voltava em 0,15 s e ele não caía (a raiz andou 25 cm, o
  limiar de tropeço é 28): um **atordoamento** de 0,3 s depois de um golpe forte antes do equilíbrio voltar.

### Ragdoll ativo: se proteger, levantar de bruços ou de costas, mistura por osso
- Tropeçando ou caindo (até 0,9 s caído), os **braços** (do ombro para baixo; a clavícula não) miram o chão adiante, na
  direção da queda (a velocidade e a inclinação da raiz), com os músculos mais fortes; só em bípedes (as "patas da
  frente" de um quadrúpede são pés). Teste: as mãos tocam o chão antes da cabeça e do peito.
- Caído: **de bruços** ou **de costas** pela frente do peito (o forward da bind girado com o corpo); nó Is Lying Face Down.
- **Montagens de levantar** no Physics Asset (Levantar de Bruços / de Costas, o seletor de assets): pedida à thread
  principal (`ikphys_main` → `anim_play_montage`); 0,3 s depois (a mistura de entrada) a primeira pose dela é casada com o
  corpo — o ator gira em torno do lugar (o sinal do yaw tirado das próprias linhas do transform) e anda para a raiz da
  montagem cair na do corpo — e o corpo se mistura nela em 0,35 s. Sem montagem: como antes (mistura para a animação).
- A altura da raiz sobre os pés e os pés vêm da **bind** (depois de levantar ele reinicia agachado, no meio da montagem).
- **Set All Bodies Below Physics Blend Weight**: peso da física por corpo (com tempo), multiplicado pelo geral.

### Ossos dinâmicos (`DynChain` no Physics Asset, `dyn_apply`)
- Uma cadeia: o primeiro osso (gira; a cabeça dele fica onde o esqueleto tem) e os de baixo até as pontas ou até o
  Último Osso; uma partícula na cabeça de cada osso e uma além de cada ponta. Verlet no mundo, 2 subpassos: amortecimento,
  gravidade, vento (Set Dynamic Bones Wind), a mola para a forma animada a partir do pai simulado, o comprimento, o limite
  de ângulo, fora das cápsulas do Physics Asset (não as da própria cadeia, não a do corpo de onde ela pende, nem as que
  ela já começa dentro na bind: o brinco dentro da esfera grande do peito). A inércia do personagem: a parte do movimento
  da raiz que a cadeia leva junto rígida. Depois cada osso gira para a sua partícula (os comprimentos pelo esqueleto).
- Presets: Brinco, Cabelo, Cauda, Alça / Bolsinha, Bolsa Pesada. Roda jogando, com ou sem ragdoll (depois dele).
- Editor: lista "Ossos Dinâmicos", "+ Cadeia no osso selecionado", as propriedades; Simular > "Ossos Dinâmicos (Balanço)"
  balança o esqueleto na prévia. Nós: Set Dynamic Bones Enabled, Set Dynamic Bones Wind, Reset Dynamic Bones.

### Sockets no jogo (o que o Q7 deixou) e roupa contra o corpo
- `anim_attach_to_socket` (CAttach vira `ATTACH_BONE`, o `bp_components` não o move) e `anim_attach_actor_to_socket`
  (um ator, sem CAttach): a pose que move aquele osso os coloca (osso × socket × o deslocamento próprio) no job de pose de
  cada instância; um ator que se moveu é `ecs_touch` na thread principal depois dos jobs. O inventário equipa no socket
  assim (personagens com skin também: antes iam para a malha inteira). `anim_socket_world`: o lugar do socket na última
  pose (a primeira pergunta guarda a pose dali em diante). Nós: Attach Actor To Socket, Detach From Socket, Get Socket
  Location, Get Socket Rotation.
- Tecido: `phys_publish_bodies` guarda as cápsulas do Physics Asset de cada personagem na pose final (e as do quadro
  anterior); `phys_capsules_near` as dá ao `cloth.c`, que as manda como **cápsulas que se movem** (tipo 4 + o lugar
  anterior no tipo 5, ver `tecido.md`). Um personagem com skin e Physics Asset agora tem colisores (antes só as partes
  rígidas tinham), e Seguir Osso acha o osso de uma malha com skin (`anim_socket_world`). Presets Camisa e Saia (finos,
  2 subpassos, presos na faixa de cima, girando com o osso); a Capa ganhou espessura e 2 subpassos.

### Testes (área ikrig; socket_offset na animacao)
- `ragdoll_limits` (`--ik-test ragdoll_limits`): os tipos e a dobra do joelho para trás e do cotovelo para a frente, a
  massa, 5 quedas (4 empurrões e um chute no joelho e no cotovelo nos sentidos errados): a pior junta 1,97° além do
  limite, dois corpos 0,10 cm um no outro, 16 cm/s no fim.
- `ragdoll_active`: o de antes (bípede e quadrúpede) + empurrado para a frente cai de bruços com as mãos no chão antes da
  cabeça e do peito, para trás cai de costas, levanta e volta à animação; um braço sozinho balança e volta à animação.
- `dyn_bones`: brinco e cauda balançam (15 e 4,6 cm), param (a 0,18 e 0,5 cm do repouso no último segundo), o brinco
  encosta no braço e no peito sem entrar; desligados ficam na animação.
- `socket_offset` (`--scene socket_offset`, projeto de teste): uma espada num socket da mão (12,5 cm do osso, girado 90°)
  e uma lanterna (ator) presa na bacia correndo em círculo: 0,000 cm do esperado em todos os quadros.
- `cloth_on_body` (`--scene cloth_on_body`, `REALENGINE_CLOTH_READBACK=1`): saia (malha) e capa (grade) no Mannequin
  correndo (MM_Run, 3,8 m/s): vértices além de meio raio de um corpo = 0, além de 1 cm ≤ 0,05%. Medido: saia 0,002%
  (o pior vértice 1,7 cm), capa 0,000–0,005% (0,3–1,9 cm) com 2 × 8; com 2 × 4, 0 vértices dentro; só com as cápsulas das partes (o "antes",
  `REALENGINE_CLOTH_NO_BODIES=1`): saia 2,4 cm, capa 2,7 cm e 0,077%.

### Medidas (RTX 3060, 1440p, 2026-11-18; a máquina livre, alternando com a build da main df71400)
| Cena | main | Q10 |
|---|---|---|
| `--scene active_ragdoll` (50 Mannequins, um golpe a cada 0,5 s) | 250-256 fps, CPU 3,8-3,9 ms, física 1,7 ms somada | **385 fps, CPU 2,5 ms**, física 3,1-3,5 ms somada |
| `--scene ragdoll_q10`, 1 Ragdoll Ativo (`REALENGINE_Q10_N=1`) | — | 390 fps, física 0,061 ms, GPU 2,49 ms |
| `--scene ragdoll_q10`, 50 | — | 385 fps, CPU 2,50 ms, física 3,19 ms somada, GPU 2,46 ms |
| ragdoll de 19 corpos caindo (`ragdoll_limits`, 1 thread) | ~0,08 ms | 0,11 ms (sem as passadas de limite depois dos contatos 0,080 ms, mas a cauda passava 23°) |
| Ragdoll Ativo de pé (autoteste, bípede) | 0,050 ms | 0,049 ms |
| ossos dinâmicos, 1 personagem (brinco + cauda, 8 partículas) | — | 0,011 ms |
| ossos dinâmicos, 50 personagens, 1 thread | — | 0,51 ms (nas tarefas de pose: dividido pelos núcleos) |
| `cloth_on_body` (saia + capa, 1496 vértices), passe do tecido na GPU | 1,90 ms (só as partes) | 2,28 ms (cápsulas do Physics Asset que se movem) |
- A física somada subiu (as passadas de limite depois dos contatos, a torção por quatérnio, mais quedas pelo atordoamento),
  mas o quadro ficou mais rápido: o `anim.c` punha 64 instâncias por tarefa (50 personagens = uma tarefa, tudo numa thread);
  agora 8 por tarefa até 512 instâncias. As medidas de limite e sobreposição (`ikphys_info`) só rodam depois que alguém
  pede (os testes); o teste "dentro do cone interno" evita o log map na maioria das juntas.
- Os presets Camisa/Saia/Capa começaram em 2 subpassos × 8 iterações: o tecido custava 4,5 ms de GPU nessa cena; agora
  2 × 4 (os despachos de 1 × 8), 2,3 ms, sem penetração medida.
- (Resolvido em 2026-11-19, seção abaixo: clipes de levantar de verdade e o teste `getup_q10`.)

## Levantar do ragdoll com clipe de verdade (sobra do Q10, 2026-11-19)
Pedido: testar a montagem de levantar com um clipe real (licença livre ou feito à mão no formato da engine), ligar no
personagem de demonstração, conferir a troca ragdoll → pose → montagem (sem salto, pé fora do chão, de bruços e de
costas) e um teste que mede.

### Os clipes (`src/anim_getup.inc`)
- Feitos à mão no formato da engine, como os da luta (nenhum asset de fora: sem licença a conferir). **MM_GetUp_Front**
  (3,2 s): de bruços, empurra nas mãos, joelhos para dentro (quatro apoios), o pé direito à frente (meia ajoelhada), de
  pé. **MM_GetUp_Back** (3,0 s): de costas, queixo para dentro e o peito sobe nas mãos, sentado apoiado nos braços com os
  pés puxados, o balanço para a frente com os braços à frente (cócoras), de pé.
- Poses-chave da bacia, coluna e cabeça; cada mão e cada pé é um lugar no espaço do ator resolvido por **IK de dois ossos
  com polo** (`limb_ik2`): mão ou pé na mesma chave fica plantado. Os dois terminam na primeira pose do idle na origem,
  de frente para +X; de bruços deitado com a cabeça para +X, de costas com a cabeça para -X (`align_getup` casa a bacia e
  a direção bacia → peito com o corpo).
- As chaves foram feitas num protótipo em Python com o mesmo FK/IK e as partes do Mannequin (caixas, cilindros) contra o
  chão: nenhuma parte mais de ~1 cm no chão; no motor, sozinhos (sem física), a sola fica em -0,1 cm (frente) e -0,8 cm
  (costas).
- **AM_GetUp_Front / AM_GetUp_Back** (DefaultSlot, entrada 0,2 s, saída 0,3 s) em `Characters/Mannequin/GetUp`, feitos
  quando faltam (`getup_pack` no `engine_clips_update`: projetos antigos também ganham). `phys_autogen` escolhe essas
  montagens quando os ossos do clipe estão todos no esqueleto (Gerar Corpos, `phys_temp`): o Mannequin das cenas
  `active_ragdoll`, `ragdoll_q10` e de qualquer Physics Asset gerado para ele já levanta com elas.

### A troca (ikrig.c)
- A montagem começa **parada no primeiro quadro** (`anim_set_montage_rate(…, 0)`, novo, como o Montage Set Play Rate);
  o ator é casado com ela aos 0,3 s e só então ela anda (taxa 1 no `ikphys_main`). Antes ela andava 0,3 s antes do
  casamento (o "primeiro quadro" era o de 0,3 s).
- Durante a troca o corpo fica **parado como caiu** (asleep) e os músculos soltos; a mistura vai em **0,5 s com
  suavização** (era linear em 0,35 s). Antes os músculos voltavam inteiros na mistura e puxavam a mão a 12 m/s.
- Depois da mistura a montagem toca **como foi feita** (`getup_phase` 3: nada simulado, custo de um personagem sem
  ragdoll); quando termina (ou chega um golpe: `ikphys_main`), o Ragdoll Ativo volta com os corpos refeitos da animação.
  Antes ele ficava "de pé" seguindo a montagem pelos músculos: o pé afundava 9 cm e o peito ficava torto no fim.
- **Bug do fim da mistura** (valia também sem montagem): ao terminar, `blend` voltava a 1 no mesmo quadro em que os
  corpos ainda eram os do ragdoll deitado (refeitos só no seguinte): um quadro do corpo deitado, a mão saltando 60 cm.
  Agora o peso volta a 1 só quando os corpos são refeitos (`blend_back`).

### Bugs achados no caminho
- **A frente do Mannequin era +Y** (`skel_forward`): sem osso do dedo do pé e com a cabeça em cima da raiz, caía no
  padrão (0,1,0) — o lado. "De bruços" era o lado direito para baixo e o joelho dobrava de lado nos limites. Agora, sem
  dedos, a frente vem dos ossos `_l`/`_r` (direita × cima): +X no Mannequin, +Y numa malha virada para +Y.
- **Pé sem dedo era uma bola no tornozelo**: deitado, a ponta do pé entrava 11 cm no chão. Agora uma cápsula ao longo do
  pé (calcanhar ~ altura do tornozelo atrás, ponta ~2×, as pontas um pouco mais baixas para cobrir as quinas): 2-4 cm em
  repouso (o chão do ragdoll é 1 cm abaixo dos pés e os limites rodam depois dos contatos).
- **Os braços se protegendo depois de cair**: continuavam empurrando o chão até 0,9 s caído e viravam de costas quem
  caiu de bruços (1 em cada 3 quedas para a frente). Agora param quando o peito chega ao chão (`landed`).

### Teste `getup_q10` (área ikrig, `--scene getup_q10`, `editor_q10.c`)
- Um Mannequin como Ragdoll Ativo leva 4 golpes fortes (frente, trás, frente, trás); outro, sem física, toca as duas
  montagens sozinho (a sola do clipe). Por queda: deitado como (de bruços/costas) → a montagem certa; a mais rápida que
  um osso andou na troca (salto: > 10 m/s, 17 cm num quadro de 60 Hz) e no levantar todo (> 15 m/s); a sola (linha do meio
  do pé: calcanhar, meio, ponta) deitado (≥ -5 cm) e na montagem (≥ -1,5 cm); de pé no fim (bacia ≥ 85 cm, peito ≥ 35 cm
  acima dela), de frente para onde a cabeça apontava (de bruços) ou os pés (de costas) a menos de 35°, a menos de 1,5 m de
  onde caiu; as duas montagens vistas e a maioria das quedas caindo para o lado do golpe.
- `REALENGINE_Q10_DEBUG=1`: cada osso rápido, cada sola funda, o peito durante a queda.

| Medida (RTX 3060, 1440p, 1 Mannequin, 4 quedas, várias rodadas) | antes (o caminho do Q10) | agora |
|---|---|---|
| osso mais rápido na troca ragdoll → montagem | 59 000 cm/s (o salto da raiz) / 60 cm num quadro no fim | **120-320 cm/s** |
| osso mais rápido no levantar inteiro | 14 000-59 000 cm/s | **390-470 cm/s** |
| sola mais funda deitado / na montagem | -11 cm / -9 cm | **-1,3 a -4,2 cm / -0,1 a -0,9 cm** |
| de pé no fim: bacia, peito acima dela, direção | 93 cm, 19-34 cm (torto), — | **94 cm, 39-40 cm, 0-11°** |
| física por quadro durante o levantar | 0,06-0,15 ms | **0,03 ms** (a montagem sem simulação) |
| poses por quadro (o personagem todo) | 0,07-0,13 ms | **0,05 ms** |
| custo dos clipes | — | 2 arquivos `.ranim` (97 e 91 quadros) + 2 `.rmontage`, gerados uma vez |
| `--scene active_ragdoll` (50, um golpe a cada 0,5 s; sem validação, alternado com a build da main de 19/11) | 242 / 231 fps | 241 / 226 fps (limitado pela GPU, ~4 ms: igual) |

## Sub-passos fixos (2026-11-19: o `ikphys_ragdoll` que falhava com o PC ocupado)
- **O bug**: o passo era `h = dt / 8`, então cada fps simulava um ragdoll diferente. Com o PC ocupado, os quadros ficavam
  desiguais (5 a 40 ms, travadas de 1,3 s) e injetavam energia. Os 12 ragdolls deitados se debatiam a 400–2200 cm/s e
  50 rad/s (o limite). Isso acontecia de 54 a 108 fps sob carga, na suíte ao lado de outro trabalho, e falhava igual na
  main 867ac95 (3 de 5). Um jogo a 60 fps com quadros irregulares sofreria o mesmo.
- **A correção** (`phys_apply`): sub-passos de tamanho fixo, `SUB_H` = 1/960 s, que são os 8 de antes a 120 Hz.
  - Cada quadro roda quantos sub-passos couberem, até `MAX_SUBS` (32, o que dá 1/30 s); a sobra fica em `PState.sub_acc`.
  - O ragdoll ativo de pé e equilibrado usa `2 × SUB_H`, como antes (metade dos sub-passos).
  - Os ossos dinâmicos (`dyn_apply`) não mudaram.
- **Medida**: `REALENGINE_RAGDOLL_DT=<s>` fixa o dt de cada quadro; `rand` sorteia de 2,5 a 40 ms. O teste novo
  `ikphys_ragdoll_jitter` (área ikrig) roda a cena `ragdoll` com `rand`.
  - Com `rand`: o mais rápido estava a 85 cm/s e caiu para 5,7 cm/s.
  - Com a CPU toda ocupada (109 e 137 fps): 2200 cm/s caiu para 25 cm/s no máximo, sem nenhum FAIL.
  - Com dt fixo (1/240 a 1/30): parado, antes e depois.
- **Custo** (máquina livre):
  - Cena `active_ragdoll` (cerca de 285 fps, 50 ragdolls): os jobs de pose somados caíram de 3,2–4,3 para 1,9 ms,
    porque são menos sub-passos por quadro; fps igual (284/287 contra 288/274).
  - `ragdoll_limits` (quadros de 1/60 s): de 0,12–0,20 para 0,20–0,34 ms por quadro, com 16 sub-passos em vez de 8.
  - Resultado do `ragdoll_limits` igual: 0,71° além do limite, 0,21 cm entre corpos, 26 cm/s no fim.

## Ferimentos e orçamento do Ragdoll Ativo (Q27.5, 2026-11-23: as reações do GTA 6)
- **Apply Wound Reaction** (`phys_wound`: osso, ponto no mundo, gravidade 0..1), resolvido no próximo passo: o corpo do
  osso (ou o ancestral mais perto com corpo), o ponto guardado no espaço dele.
  - **Tronco, braço, cabeça**: a mão do outro lado (lado pelo nome do osso: `_l`/`left`/`.l`) mira o ponto (antebraço
    nele, braço com o cotovelo para baixo), braços mais fortes; 4 + 8 × gravidade segundos.
  - **Perna**: manca 8 + 12 × gravidade s. A perna do corpo atingido para baixo fica até 55% mais fraca; o joelho dobra
    (coxa para a frente, canela para trás, a frente pelos pés); o alvo da raiz (cinemático em pé) desce 6 cm × gravidade
    e inclina até 6° para a perna ferida. **Get Limp** para a caminhada de ferido (Q27.11).
  - **Grave** (> 0,7): `support` × 0,15 e 1 s atordoado (cai); no chão **se contorce** 3 + 5 × gravidade s (coluna e
    pernas giram em volta do próprio eixo de onde estão, músculos a 30%) e só depois levanta. **Is Writhing**.
- **Queda alta**: a raiz descendo a mais de 5 m/s por 0,25 s; os braços giram em moinho (fora de fase) até a descida
  ficar abaixo de 1,5 m/s.
- **Orçamento** (`ACTIVE_BUDGET` 32): `ikphys_frame_end` escolhe por quickselect os 32 ativos mais perto da vista
  (`ikphys_set_view`, chamado pelo `anim_set_view_point`); os outros ficam dormentes no quadro seguinte: em pé, a pose da
  animação sem simular (voltam a simular do zero pela animação); caídos, dormem. Tropeçando ou levantando nunca dormem.
  `IkPhysStats.dormant` conta.
- **Teste** `ragdoll_wounds` (área ikrig): canela ferida: pelve 96,2 → 93,0 cm em pé; ferimento no peito: a mão direita
  de 49 para 12 cm dele; grave: cai em 0,37 s, se contorce 7,5 s, levanta em 7,9 s (sem: ~1,5 s); queda de 8 m: braços
  agitados 0,55 s, param ao bater; **200 ragdolls ativos: 168 dormentes, 32 simulados, 1,50 ms por quadro** (uma thread;
  sem orçamento seriam ~200 × 0,036 = 7,2 ms).
- Fica para depois: nós de dor por região no Anim Blueprint (máscara de ferido), cambalear pelo tiro no meio da corrida
  (o tropeço já existe), reação de cobertura por medo.

## Detecção de rig e física pela malha (2026-11-23)
Pedido: "detecção de rig: todas as rigs compatíveis com o Tlender também com a Real, já sabendo o que é IK, FK e a
hierarquia, o IK do pé funcionando só de ativar no AnimGraph; para a física, o tamanho dos ossos e a grossura da malha
que cada osso pega, sem overlap, automático ao gerar; predefinições: cauda, orelha de coelho, corrente, acessórios".
- **Copiado do Tlender** (adaptar, nunca incluir): `charmap.h/.c` e `charmap_analyze.c` (a definição de personagem:
  papéis, cadeias, classe), `rig_systems.h/.c` (Rigify, Auto-Rig Pro, AdvancedSkeleton, Mixamo/HumanIK, UE/MetaHuman,
  VRoid, CC/AccuRig, Daz, Biped, CAT, SMPL, Valve, MPFB, rig de controle desconhecido). `rig_armature.h` é a parte da
  `Armature` do Tlender que eles leem (ossos com cabeça/cauda, deform, constraints, as marcas do auto-rig);
  `rig_armature_math.h` tem os `v3_*` só para esses arquivos (o `v3()` colidia com o do `ikrig.c`).
- **`src/rig_detect.c/h`**: monta a armature a partir da bind do `SkelMesh` (cabeças nas juntas, cauda no filho com mais
  ossos abaixo, metros), os ossos que deformam pelos pesos das peles na CPU (`render_mesh_cpu`, `uv2`), e roda o
  analisador nas 4 rotações em Z: vence a que tem mais papéis e os dedos do pé para -Y (os lados são por X). Guarda no
  `SkelMesh.rig` (`RigInfo`: sistema, classe, papel → osso, controle FK, alças de IK e polos por membro, tipo de cada
  osso — deforma, controle, alça de IK, polo, MCH, torção, dedo, cadeia, auxiliar —, cadeias com o tipo: cauda, orelha,
  cabelo, corrente, acessório, capa/saia). Feito ao carregar e ao recarregar o Skeletal Mesh (`anim.c`).
- **Nomes da Unreal em qualquer rig**: `rig_bone_ue(m, "foot_l")` = o nome exato, senão o osso do papel (uma coluna que o
  rig não tem cai na mais perto). `bone_named` (AnimGraph: Leg IK, Foot IK, Aim Offset), `style_bone` (estilos,
  cobertura, golpes, warping) e `mm_feet_bones` (Motion Matching) passam por ele: o ABP com `foot_l`/`foot_r` funciona
  num Mixamo, Rigify, CC... só ligando o Foot IK.
- **Gerar Corpos** (`phys_autogen`): os ossos com papel recebem o nome da Unreal na lógica antiga (os tipos de junta,
  limites e músculos seguem o papel); controles, alças, polos, MCH, torções, dedos, cadeias e auxiliares sem peso não
  ganham corpo. **`phys_fit_to_mesh`**: os vértices que cada osso mais move (peso >= 0,5) medidos do segmento do corpo,
  o raio na mediana (histograma de 1 cm, uma passada); depois os corpos não ligados por junta que se sobrepõem na bind
  encolhem pela sua parte (nunca abaixo de 35%). As cadeias do rig viram **Ossos Dinâmicos** com a predefinição do tipo
  (uma cadeia que o asset já tem fica como está). Predefinições novas: **Orelha** (rigidez 0,6: fica em pé e balança),
  **Corrente** (sem forma própria, solta) e **Capa / Saia** (o vento leva).
- **Editor**: no Skeletal Mesh, embaixo da Árvore do Esqueleto, **Rig Detectado** (sistema, classe, papéis principais,
  controles, alças de IK, cadeias, a lista Papéis que seleciona o osso, Analisar Rig).
- **Testes** (área ikrig): `rig_detect` (`--rig-test`: Mixamo, Rigify com controles + cauda + orelhas, UE virado para +X,
  sem nomes, CC) e `rig_fit` (importa `tests/data/import/humanoid.glb`, feito por `make_humanoid.py`: nomes do Mixamo,
  cauda, orelhas, cada parte da pele um tubo de raio conhecido).
- **Medidas** (i7-11700F): análise de um rig de 27-45 ossos em 1,1-1,6 ms (4 rotações), ao carregar. `humanoid.glb`:
  Mixamo, 15/15 papéis, 1 cauda e 2 orelhas; Gerar Corpos com o ajuste à pele em 0,24 ms (3840 vértices): 20 corpos, coxa
  8,6 cm (tubo 8), canela 6,3 (6), braço 5,7 (5), 0 sobrepostos; 3 cadeias (Cauda, 2 Orelhas).
