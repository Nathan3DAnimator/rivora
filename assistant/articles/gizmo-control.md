id: gizmo-control
kind: howto
title: Controle total do gizmo (eixos, pivô, valores digitados) | Full gizmo control (axes, pivot, typed values)
keys: gizmo, manipulador, manipulator, pivo, pivot, ponto de pivo, pivot point, mediana, median, origens individuais, individual origins, orientacao, orientation, mundo, local, vista, view, trackball, restringir eixo, constrain axis, digitar valor, type value, cancelar arrasto, cancel drag, precisao, precision, tamanho do gizmo, gizmo size, tlender, blender
--- pt
```diagram keys
X/Y/Z: Eixo | Shift+X/Y/Z: Plano | Números: Valor | Ctrl: Encaixe | Shift: Preciso | Esc: Cancelar
```
## Durante o arrasto (como no Tlender)
- **X / Y / Z** prendem o movimento naquele eixo; **Shift+X / Y / Z** no plano sem ele. Apertar de novo troca Mundo/Local; de novo, solta.
- **Digite o valor**: 250 move 250 cm, 90 gira 90°, 2 dobra a escala. **Tab** passa ao próximo eixo, **-** inverte o sinal, **Backspace** apaga.
- **Ctrl** inverte o encaixe da barra; **Shift** move um décimo (precisão); **+ / -** mudam o tamanho do gizmo.
- **Botão direito** ou **Esc** cancelam: tudo volta e nada vai para o Ctrl+Z. Um clique que não mexeu também não cria passo de desfazer.
- **Shift** ao pegar uma seta usa o plano dos outros dois eixos.
- O **cursor** mostra o que o cabo faz (setas para mover, mão para girar) e, arrastando, vira o cursor da transformação.

## Na barra da viewport
- **Orientação** (globo): **Mundo**, **Local** (os eixos do ator) ou **Vista** (a tela). **Ctrl+`** alterna Mundo/Local.
- **Pivô** (alvo): **Ator Ativo**, **Ponto Mediano**, **Centro da Caixa** ou **Origens Individuais** (cada ator gira no próprio centro). **Só Posições** move os atores em volta do pivô sem girá-los nem escalá-los.
- Girar tem um **trackball** dentro dos anéis: arraste para girar livre (o lado virado para você acompanha o mouse).
- **Tamanho do Gizmo**: Preferências do Editor > Viewport. [Mostrar onde](show:Edit>Editor Preferences...)
--- en
```diagram keys
X/Y/Z: Axis | Shift+X/Y/Z: Plane | Digits: Value | Ctrl: Snap | Shift: Precise | Esc: Cancel
```
## While dragging (as in the Tlender)
- **X / Y / Z** lock the motion to that axis; **Shift+X / Y / Z** to the plane without it. Press again for World/Local; again, off.
- **Type the value**: 250 moves 250 cm, 90 turns 90°, 2 doubles the scale. **Tab** goes to the next axis, **-** flips the sign, **Backspace** erases.
- **Ctrl** inverts the toolbar's snapping; **Shift** moves a tenth (precision); **+ / -** resize the gizmo.
- **Right click** or **Esc** cancel: everything goes back and nothing reaches Ctrl+Z. A click that didn't move makes no undo step either.
- **Shift** when grabbing an arrow uses the plane of the other two axes.
- The **cursor** shows what the handle does (arrows to move, a hand to rotate) and, while dragging, becomes the transform's cursor.

## On the viewport toolbar
- **Orientation** (globe): **World**, **Local** (the actor's axes) or **View** (the screen). **Ctrl+`** toggles World/Local.
- **Pivot** (target): **Active Actor**, **Median Point**, **Bounding Box Center** or **Individual Origins** (each actor turns about its own center). **Only Locations** moves the actors about the pivot without turning or scaling them.
- Rotate has a **trackball** inside its rings: drag to turn freely (the side facing you follows the mouse).
- **Gizmo Size**: Editor Preferences > Viewport. [Show me where](show:Edit>Editor Preferences...)
