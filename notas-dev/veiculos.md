# Veículos (2026-11-07, pedido do usuário)

Pedido: "sistemas para quem quer fazer jogos de carros: profissional mas intuitivo, com suporte a todo tipo de veículo,
terrestre, aquático, trens e voadores; completo: suspensão com molas e amortecedores, pistões, engrenagens, rodas,
partes móveis como portas".

## Arquivos
- `src/vehicle.h/.c`: o asset `.rvehicle` (JSON: chassi + até 48 peças), os 10 modelos, a montagem do ator (componentes
  de formas básicas com `CAttach`, todos no nível 1), Water Body e Rail.
- `src/vehicle_sim.inc`: a simulação (SI por dentro, 4 subpassos fixos por quadro, nada alocado no quadro).
- `src/vehicle_play.inc`: o quadro, a entrada do jogador, os trens, as peças visuais, a câmera, os trilhos desenhados,
  o autoteste.
- `src/vehicle_editor.c/h`: a janela do asset (chassi, marchas, peças; vistas de cima e de lado).
- Componentes: `CVehicle` (asset, Auto Possess, motor ligado, trilho, engate), `CWater`, `CRail` (64 pontos).
- `bp_components_place_actor`: coloca os componentes de um ator na hora (o veículo move depois do posicionamento do
  quadro). `bp_get_input`: os eixos do jogador. Os veículos saem das caixas estáticas do nível (`build_boxes`), e as
  caixas passam a ser montadas também quando há veículos.

## A simulação
- Chassi rígido (inércia de caixa, giroscópico), gravidade, arrasto `0.5 rho CdA v|v|`, downforce.
- Roda: raio do ponto de montagem para baixo (`bp_trace_boxes`: caixas e landscapes); mola + amortecedor + barra
  estabilizadora (metade da mola entre as rodas do eixo) + batente. Pneu: Pacejka (B 10, C 1.9 longitudinal; B 8, C 1.3
  lateral) com elipse de atrito; esterço reduzido com a velocidade.
- **Roda implícita** (bug achado): a rotação da roda contra a inclinação do pneu é resolvida de forma semi-implícita.
  Explícita, num subpasso ela passava do pico do pneu e disparava (3000 rad/s; o tanque não andava, a moto ia a 21 km/h).
  Mais o controle de tração e o ABS (torque até o pico de aderência; o freio de mão trava).
- Motor: curva de torque (pico em Peak RPM), embreagem que patina abaixo de 1,5x a marcha lenta, freio motor,
  limitador. Câmbio automático pelas RPM **das rodas** (as do motor solto faziam trocas falsas). Ré: acelerador negativo
  parado (o nó de freio só freia). Diferencial: as rodas de tração puxadas para a média (travado 1, autoblocante p).
- Tanque: cada lado pelo acelerador ± direção, atrito lateral das esteiras 0,3, giro amortecido (~40 graus/s).
- Moto: torque de inclinação para a inclinação que a curva pede `atan(v yawrate / g)`.
- Boias: empuxo `rho g V fração` + arrasto da água (correnteza); ondas de dois trens senoidais.
- Asa: alfa pela velocidade no ponto, Cl linear até o estol e caindo depois, arrasto induzido; controles (profundor,
  ailerons com o sinal pelo lado, leme e deriva: +1 vira o nariz à direita).
- Rotor: coletivo em volta do pairar (Estabilidade), cíclico inclina o empuxo, cauda gira, nivelamento e amortecimento.
- Trem: 1D ao longo do trilho (Catmull-Rom com tabela de comprimento), tração limitada pela aderência, rampa, arrasto,
  freio; vagões engatados seguem à folga; carroceria entre os dois truques.
- Colisão do chassi: 13 pontos da caixa contra as caixas do nível e o landscape (impulso com restituição 0,15 e atrito).
- **Convenção**: +Y é a direita (a da Unreal, `level_camera_basis`); os modelos põem L em -Y.

## Medidas (RTX 3060, 1440p)
| Cena | Resultado |
|---|---|
| `--scene vehicles` (11 veículos de 9 tipos, 36 rodas, validação) | 313 fps; veículos 0,08 ms de CPU por quadro |
| autoteste | carro 0-144 km/h em 10 s, para em 6 s; tanque gira 123 graus em 3 s e anda a 60 km/h; moto 111 km/h inclinada 23 graus; barco 29 km/h; avião a 103 m em 40 s; helicóptero paira (sobe 80 cm em 3 s solto); trem 64 km/h, vagão a 19 m |

Testes: `vehicle_selftest` (sem nível: chão plano em 0), `vehicles_scene` (a cena, validação, 200 fps).

## Limites / próximos passos
- Rodas como raios (sem largura no contato); chassi sem colisão contra corpos rígidos (contra outros veículos: Q27.4, abaixo).
- Sem som de motor ainda (o RPM está em `vehicle_info` para ligar a um Sound Cue).
- Trem só em trilhos do mesmo ator (sem desvios/agulhas).
- No editor a locomotiva fica onde foi posta até o Play (só então entra no trilho).

## Veículo a partir do modelo importado (2026-11-08, pedido do usuário: "confuso com a mesh que ele importou")
- **Ator > Criar Veículo a partir da Seleção > tipo** (e o botão **Criar Veículo...** nos Detalhes de uma malha):
  `vehicle_from_actors` (`vehicle_build.inc`). A maior peça é a carroceria; rodas pelo nome (wheel, tire, roda, pneu,
  rueda...) ou pelo formato (redondas, na metade de baixo); raio, largura e lugar medidos pelas caixas no mundo; a
  suspensão/pneu/direção/tração da roda dianteira/traseira do modelo do tipo. Portas (door, porta, puerta): dobradiça na
  borda da frente, F abre. O resto: peça fixa (Slider de 0 a 0). Sem rodas soltas num tipo com rodas: rodas invisíveis
  nos cantos (`veh_fit_to_body`). Motor, câmbio, asas, rotores, boias do modelo do tipo, posições escaladas.
- A frente do modelo: automática (lado mais comprido; +X) ou escolhida (+X, -X, +Y, -Y). O ator novo fica com Possuir
  Automaticamente (Play já dirige). Asset em `Content/Vehicles/V_<nome>.rvehicle`.
- Asset: `body_mesh`, `body_material`, `body_loc/rot/scale`; peça: `mesh`, `material`, `mloc/mrot/mscale` (JSON, ids em
  hex). Com `body_mesh`, o ator é a carroceria + só as peças com malha (as formas básicas do tipo saem).
- Editor do veículo: seção **Seu Modelo** (Malha da Carroceria, Deslocamento, Giro, Ajustar à Malha da Carroceria) e
  **Malha / Giro / Escala / Posição da Malha** de cada peça.
- Teste `vehicle_from_mesh` (`--vehicle-mesh-test`): carroceria + 4 rodas "Wheel_*" deitadas → 4 rodas de 33 cm com a
  malha, apoiada nas 4, 96 km/h e virando 45 graus em 7 s.

## Batidas e dano (Q27.4, 2026-11-23)
Do GTA 6 (auditoria-gta6.md): a deformação dos painéis muda a dirigibilidade.
- **Veículo contra veículo**: depois dos passos do quadro, cada par perto (as esferas das caixas se tocam) testa os 13
  pontos da caixa de um (cantos, centro de baixo e das faces) contra a caixa orientada do outro; o mais fundo dá a
  normal (a face de menor penetração), os dois são afastados pelas massas e recebem o impulso (restituição 0,1, atrito
  0,4 de raspar, massa e inércia dos dois). Trens ficam de fora (o trilho manda).
- **Dano**: uma grade 8 × 4 × 4 de amassados (cm, para dentro) sobre a caixa do veículo. Um impulso acima de 2500 N·s
  (um carro de 1300 kg a ~7 km/h) empurra as células perto do ponto local (0,004 cm por N·s, queda de 60 cm, 25 cm no
  máximo). Frente batida tira potência do motor (até 80%); uma quina dianteira entorta o toe daquela roda (até 0,06
  rad: o carro puxa). O dano total: a média dos amassados sobre 10 cm. `vehicle_info`: `damage`, `engine_damage`,
  `toe_deg`, `hits`.
- **Medidas** (`--scene vehicle_crash`: 50 carros em duas filas a 30 m, de frente, 60 cm fora do centro, acelerando;
  RTX 3060, 1440p): 337 fps, veículos 0,41 ms de CPU; os 50 amassados (média 0,27), 50 com potência perdida, 25
  puxando, os centros a no mínimo 331 cm (ninguém dentro do outro).
- **Teste `vehicle_crash`**: ≥ 40 amassados, ≥ 30 sem potência, ≥ 10 puxando, centros ≥ 150 cm, ≤ 1 ms, ≥ 200 fps.
- **Próximo**: os amassados nos vértices da carroceria (pelo caminho do `skin.comp`, com refit da BLAS), peças que se
  soltam, colisão contra corpos rígidos e Characters, rastro das rodas na lama.
