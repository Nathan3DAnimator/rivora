# Câmera em primeira pessoa realista e Camera Shakes (Q12, plano 0.43, 2026-11-18)

O **First Person Camera** não é um componente novo (o ECS está nos 64 componentes): são campos no fim do `CCamera`
(`components.h`, `fp` e os valores; níveis e Blueprints antigos leem 0 = desligado). Código: `src/fpcam.c/.h` (núcleo,
re_render), `src/camshake.c/.h` (asset `.rshake`), `src/fpcam_ui.c` (Details), `src/camshake_editor.c` (editor do asset),
`src/fpcam_test.c` (testes e a cena), `src/ui_tips_table_q12.c` (descrições).

## Como funciona
- **Onde**: `bp_components_update` (só jogando), na linha da câmera depois do Spring Arm a colocar: `fpcam_component`
  soma deslocamentos e giros no próprio transform da câmera — o que pende dela (a arma, os braços) vai junto. Só a câmera
  do jogador (`bp_player`); outras câmeras não custam nada. `fpcam_tick` (no `bp_tick`, antes dos componentes) anda o
  tempo dos shakes. A vista lê o roll e o FOV por `bp_player_camera_ex` (editor_viewport.c, game_main.c:
  `level_roll_basis`); o FOV da câmera só vale com o First Person ligado (as outras câmeras seguem com 90°, como antes).
- **Passo puro** `fpcam_step(FpState*, CCamera*, FpInput*)`: sem mundo, os testes o dirigem direto.
  - **Balanço no ritmo dos pés**: fase do passo 0..1 (0 = contato); vertical `-A cos(2πφ)` (mais baixo no contato, mais
    alto no meio do apoio), lateral e roll `sin` sobre dois passos (lado do pé de apoio), um aceno de pitch. A fase
    **trava nos notifies** cujo nome começa com `Foot` (`Footstep` do Mannequin; `anim.c fire_notify` →
    `fpcam_footstep`): depois de 2, o ritmo é o intervalo entre eles e cada notify corrige a fase (suave, 14/s; o
    primeiro de uma caminhada ajusta de uma vez, a amplitude ainda pequena). Sem notifies: velocidade / **Stride**.
    Amplitude = velocidade / Bob Speed (até 1,5×), some no ar.
  - **Inércia do olhar**: mola (Hz, amortecimento) entre a rotação de controle e a vista, limitada a ±8°.
  - **Inclinação**: roll pela taxa de giro (por 180°/s) e pelo strafe (na velocidade de andar).
  - **Respiração**: pitch e altura por um seno; **Exertion** (mais funda e rápida) e **Fear** (mais rápida, curta, um
    tremor de 0,05°) 0-1 pelos nós, suavizados.
  - **Aterrissagem**: mergulho = cm por metro × altura da queda (v²/2g da última velocidade no ar), até o máximo; mola
    crítica **analítica** (o pico é exatamente o pedido, 1/ω depois). **Recuo**: o mesmo (pitch, yaw, coice).
  - **Autofoco**: raio no centro (caixas e landscapes do jogo por `bp_trace_boxes` + cápsulas dos outros Characters) ou o
    ator do Set Focus Target; suavizado em **dioptrias** (como um olho acomoda). `fpcam_post` (chamado no fim de
    `post_fill_view`) põe a distância no `PP_DOF_FOCAL_DISTANCE`; sem Lente Física da Câmera de Cinema, a lente vem do
    FOV (sensor 36 mm) e o f-stop do campo **Focus f-stop** (bokeh: as lâminas da câmera).
  - **Vinheta de esforço**: soma no `PP_VIGNETTE` do próprio pós-processo (sem pilha nova).
- **Corpo visível**: `FP_BODY` chama `anim_hide_bone(owner, "head")` (o Hide Bone by Name da Unreal: o osso e os de baixo
  encolhidos até a raiz dele, no pose job, antes das partes/skins). O Character ganhou **Use Controller Rotation Yaw**
  (`CCharacter.use_controller_yaw`, no fim): o corpo olha para onde a câmera olha. Limite: a sombra também perde a
  cabeça (não há "só sombra" no renderer).
- **Presets** (`fpcam_apply_preset`): Realista (cabeça de pessoa: 1,6 cm vertical, 1,2 lateral, olhar 14 Hz, foco 6/s,
  f/2.8), Cinemático (mais balanço lateral, olhar 5 Hz, lente 2,5/s, f/1.4), Arcade (leve, sem atraso nem foco), Terror
  (passos pesados 300 cm/s, olhar 8 Hz, respiração 0,45°, vinheta 0,8). O template **Primeira Pessoa** usa o Realista,
  o Mannequin visível 14 cm atrás dos olhos e o nó Add Camera Recoil no tiro.
- **Camera Shakes** (`.rshake` JSON, Content Browser > Adicionar > Câmera): duração (0: até parar), blend in/out, escala,
  Single Instance; 7 canais (posição X/Y/Z, pitch, yaw, roll, FOV) com seno (fase aleatória opcional), **ruído Perlin**
  1D ou **curva** de até 8 chaves (repete na última). Determinístico: amostra = f(tempo, semente); a semente vem do
  handle (o mesmo Play dá o mesmo tremor). 8 prontos: Explosion, Earthquake, Handheld, Hit, Gunshot, Footstep, Heartbeat,
  Dread. Até 16 tocando; World Shake: escala `(1 - (d - inner)/(outer - inner))^falloff`.
- **Nós (13)**: Set/Get First Person Exertion e Fear, Add Camera Recoil, Set Focus Target, Get Focus Distance, Set First
  Person Camera Preset, Start / Stop / Stop All Camera Shake(s), Play World Camera Shake, Hide Bone by Name.
- Debug Viewer > **First Person Camera** (CPU por tick, cada pawn: balanço, foco, passos).

## Medidas (RTX 3060, 2560x1417, Médio do template)
| O quê | Medida |
|---|---|
| `fpcam_step` com tudo ligado | 0,09 µs |
| Amostra de um shake (7 canais) | 0,05 µs |
| CPU por tick no jogo (câmera + shakes, `fps_camera`) | 0,0022–0,0035 ms em média (alvo < 0,05); pico 0,07–0,2 ms (o 1º Start Camera Shake lê o Content uma vez) |
| Balanço × notifies do Mannequin (`fps_camera`, correndo) | 0,8–1,5 ms em média, 2,4–2,9 ms no pior (resolução: 1 quadro) |
| Autofoco 3 m → 12 m (5%) | olho 0,68 s, lente 1,62 s; 12 m → 40 cm: 0,51 / 1,21 s; sem passar do ponto |
| Aterrissagem 2 m / 10 m | 4,99 cm / 11,98 (máx. 12); recuo de 2° volta a 0,000 em 1 s |
| Template Primeira Pessoa (antes → depois) | 540 → 470–481 fps; GPU 1,77 → 2,00–2,07 ms: corpo animado ~0,15 ms (TLAS de 28 partes 0,03 → 0,10, cache de superfície +0,035), DOF do autofoco na composição 0,20 → 0,28 ms; CPU 0,94 → 1,02 ms |

## Testes
- `fp_bob_sync`, `fp_land_recoil` (área `animacao`), `camshake_determinism`, `fp_autofocus` (área `render`): sem janela
  (`--fpcam-test <nome>`, RE_CPU).
- `fps_camera` (área `animacao`): `--new-project ... --template 2 --scene fps_camera --play --bench 10`: autofoco numa parede
  a 3,7 m (371 cm × 370), longe em < 1 s (0,02 dioptria), balanço contra os Footstep do Mannequin, inclinação no strafe,
  Start + World Shake + Stop All, recuo, cabeça escondida, custo < 0,05 ms. `REALENGINE_FPCAM_DEBUG=1` imprime cada mínimo.

## Falta
- Sombra da cabeça escondida (precisa de "só sombra" no renderer); pés pelos notifies L/R (hoje alterna).
- Lente "caçando" foco (autofoco de câmera de vídeo) e foco por profundidade da tela (hoje: caixas e cápsulas).
