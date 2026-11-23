# Auditoria contra o GTA 6 (Q27, 2026-11-23)

Pesquisa da tecnologia do GTA 6 (lançado em 19/11/2026: PS5, PS5 Pro, Xbox Series X|S) em seis frentes: iluminação e
render, personagens e multidões, cidade/mundo/água, veículos e trânsito, desempenho e plataforma, animação e ragdoll.
O relatório completo, com fontes e grau de confiança: [reports/Tecnologia do GTA 6.md](../reports/Tecnologia%20do%20GTA%206.md);
as notas de cada frente em `research_notes/Tecnologia do GTA 6/`.

## O que importa para a Engine
- O GTA 6 roda a 30 fps travados em todos os consoles (1440p interno; PSSR no Pro, 720p no Series S), limitado pela CPU
  (simulação de NPCs, trânsito, física): um tiro numa multidão promove dezenas de NPCs para a IA cara de uma vez e o
  quadro cai para 20 e poucos fps.
- A imagem é uma pilha de raios conhecida: RTGI e reflexos por raio em todo console, sombras por raio só no Pro, reflexo
  escolhido por superfície (planar, SSR, raio), patente de bounce map + AO em volume como base barata (US 12,263,404),
  LOD contínuo por quantização com mistura entre níveis (US 11,978,162).
- Personagens: cabelo em fios com física em todos (NPCs também), reação ao tiro pela região e gravidade; Euphoria
  confirmado até o RDR2, não confirmado no GTA 6. Vagas pedem motion matching e ML/RL para controle de personagem.
- A Real Engine já tem quase toda a pilha visual (RealGI, sombras virtuais, muitas luzes, DAG de clusters, HLOD,
  impostores, oceano FFT, chuva, Groom, pele, Motion Matching, ragdoll ativo). Falta a **camada de cidade viva**.

## Plano Q27 (ordem do relatório: maior ganho primeiro; cada item com ms, fps e teste)
1. **Feito (2026-11-23, multidao.md):** Crowd Alarm com onda e fila de promoção (64/quadro), anéis de simulação, LOD 3 (`crowd_panic`). Fica para depois: pele por compute no anel do meio e impostor animado no longe.
2. **Feito (2026-11-23, transito.md):** vias por Spline, IDM com perfil por motorista, semáforos, espera na entrada, anéis (`traffic_queue`). Fica para depois: curva pelo cruzamento, troca de faixa, desvio de via bloqueada, veículo físico perto.
3. **Feito (2026-11-23, render.md):** orçamento de segmentos por quadro, simulação só nos 16 mais perto (GPU 8,0 → 3,9 ms com 100 cabeças; `hair_crowd`). Fica para depois: cards e malha no couro para os distantes.
4. **Feito (2026-11-23, veiculos.md):** colisão entre veículos, grade de amassados, motor e alinhamento danificados (`vehicle_crash`). Fica para depois: os amassados nos vértices, peças que se soltam.
5. **Feito (2026-11-23, ikrig.md):** Apply Wound Reaction (segurar o ferimento, mancar com o joelho dobrado, se contorcer no chão), braços agitados na queda alta, orçamento dos 32 ativos mais perto (200 ativos: 1,5 ms; `ragdoll_wounds`). Fica para depois: máscara de dor no Anim Blueprint, reação de medo/cobertura.
6. **Feito (2026-11-23, agua.md):** campo de tempestades (chuva, vento, nuvens, mar), mapa molhado persistente de 4 km (0,02 ms; a poça continua lá 10 min depois; `weather_regional`). Fica para depois: a chuva vista de longe dentro da tempestade, vento regional no Foliage e no cabelo.
7. **Feito (2026-11-23, mundos-grandes.md):** células pelo caminho dos próximos 2 s, fila pelo tempo até chegar (250 km/h: engasgos 0,25 → 0,14 %, nenhuma célula à vista faltando; `wp_fast`). Fica para depois: menos trânsito/multidão e LOD em alta velocidade, leitura das células nos workers.
8. **Feito em parte (2026-11-23, materiais.md):** retrorrefletor em qualquer material (placa 13× mais clara no farol; corrigido o arredondamento que apagava o Eye Glow) e flocos metálicos sob o verniz (`city_night`). Fica para depois: reflexo planar, política de RT por superfície, faróis distantes como sprites.
9. **Feito em parte (2026-11-23, multidao.md):** Looks no Crowd Spawner: pele, roupa de cima, de baixo e corpo por semente, nas paletas, na GPU (200 de 200 distintos, sem custo medido; `npc_variety`). Fica para depois: peças modulares trocáveis, morphs na pele, retarget em tempo real.
10. **Feito (2026-11-23, cidade.md):** relógio do jogo, Smart Objects com horário e vagas, City Routine na multidão, crime e testemunha que chama a polícia (0,0007 ms por agente; `city_routine`). Fica para depois: agenda por agente, polícia indo ao local, notícias na UI.
11. **Feito em parte (2026-11-23, animacao.md):** estilos Injured/Tired/Drunk/Carrying por personagem: o Motion Matching filtra os clipes pelo nome do estilo, ou uma camada procedural; troca sem salto, 0,0006 ms por personagem, Injured pelo mancar do ragdoll (`loco_styles`). Fica para depois: motion warping até alvos, tabelas de transição.
12. **Feito (2026-11-23, gi.md):** AO em volume em volta da câmera (das caixas das malhas, numa thread) e a luz rebatida do chão pelo sol onde nada mais ilumina (+0,016 ms; diferença para o RealGI 10,3 → 9,2 %; `gi_fallback`). Fica para depois: medir num celular, volume pela geometria.
