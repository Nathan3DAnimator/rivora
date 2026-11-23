# Prototipagem de nível (blockout)

Código: `src/proto.h/.c` (no `re_render`), componente `CProto`, menu Adicionar > Prototipagem, seção "Protótipo" nos
Detalhes (`editor.c`). Cena de teste: `--scene proto`.

## Como funciona
- Formas por números (cm): **Caixa**, **Escada** (degraus sólidos; mostra a altura de cada degrau e avisa acima de 45
  cm, o que um Character não sobe), **Rampa** (degraus de até 6 cm: parece rampa, anda como escada), **Parede** (com
  porta: largura, altura, deslocamento), **Sala** (piso, 4 paredes, porta na da frente, teto opcional), **Corredor**
  (piso, duas paredes, teto opcional), **Plataforma** (laje na altura, pernas nos cantos).
- Cada forma vira **caixas filhas** (`ATTACH_PROTO`): o cubo com o material de grade (um quadrado por metro). Por serem
  caixas, entram direto na **colisão dos Characters** (`build_boxes`) e na **navegação** (`ai_nav_build`): um blockout
  já é jogável.
- Mudou um número, moveu ou girou o ator: refeita (hash dos números + transform no campo `built`, só quando o mundo
  mudou). Filhas de uma forma apagada somem.

## Medidas
| Cena | Resultado |
|---|---|
| `--scene proto` (7 formas, 56 caixas) | 299-302 fps com validação; construção < 0,1 ms por forma |

Teste `proto`: > 20 caixas, o Character largado sobre a plataforma fica de pé a 240 cm (o topo), há caminho de
navegação até o último degrau.

## Limites
- Navegação 2.5D: sob uma plataforma ela vê o chão de baixo (um piso por célula).
- Rotação que não seja múltipla de 90° vira caixa alinhada aos eixos na colisão (o desenho segue a rotação).

## Edição no viewport e mais portas (2026-11-10, "não apenas no painel de detalhes")
- `editor_viewport.c` `handles_frame`: com um ator selecionado, alças antes do gizmo (sobre uma, o gizmo e a seleção
  esperam). Rio e Trilho: os pontos da spline (arrastar no plano da altura do ponto, Shift na vertical, Alt no começo
  insere uma cópia depois e arrasta ela, botão direito adiciona / apaga, mínimo 2). Prototipagem: ±X, ±Y e topo (a face
  oposta fica: o meio anda metade do crescimento), a porta desliza pela parede (`opening[2]`). Undo no começo do arraste.
- Portas: nos bits de `CProto.flags` (sem mudar o tamanho do componente salvo): `PROTO_DOOR_BACK/LEFT/RIGHT` (Sala),
  `PROTO_DOOR_FRONT` (Corredor: antes nunca tinha), `PROTO_DOORS` 1-8 espaçadas na Parede e na parede da frente.
  `wall_y` faz as paredes laterais com porta. Teste `proto` passa.

## Array de portas (2026-11-10)
- Cada parede com porta tem o próprio número de portas (bits 8-10 frente, 11-13 trás, 14-16 esquerda, 17-19 direita),
  mostrado no Details como um array da Unreal (+, lixeira, x por elemento).

## Peças por spline (2026-11-10, "os manipuladores do spline do rio devem estar nas partes de prototipagem")
- `CProto` cresceu no fim: `npoints` e `points[24][3]` (espaço do ator; níveis antigos abrem retos). Com 2 ou mais
  pontos, o **Muro**, o **Corredor** (piso, duas paredes, teto se pedido), a **Caixa** (faixa maciça), a **Rampa** (um
  caminho com o topo na curva) e a **Plataforma** (caminho na altura, um pé em cada ponto) seguem uma Catmull-Rom pelos
  pontos: um segmento a cada ~120 cm (pelo menos 1 por trecho), cada caixa virada pelo segmento (`PBox.rot`: pitch e yaw
  somados à rotação do ator) e um pouco mais longa que ele (sem buraco nas curvas). O hash do `proto_update` inclui os pontos.
- Os manipuladores são os do rio (`spline_points` devolve os do `CProto`): arrastar, Shift sobe e desce, Alt arrasta uma
  cópia, botão direito adiciona ou apaga, setas X/Y/Z no ponto selecionado. Detalhes: **Fazer Spline** (3 pontos numa
  curva suave pelo Comprimento) e **Reta de Novo**.
- Cena `proto`: um muro em S por 4 pontos; o teste `proto` confere 17 caixas com yaw de -64° a 48°.

