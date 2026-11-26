id: gizmo-control
kind: howto
title: Controle total do gizmo (eixos, pivô, valores digitados) | Full gizmo control (axes, pivot, typed values) | Control total del gizmo (ejes, pivote, valores escritos)
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
--- es
```diagram keys
X/Y/Z: Eje | Shift+X/Y/Z: Plano | Dígitos: Valor | Ctrl: Snap | Shift: Preciso | Esc: Cancelar
```
## Mientras arrastras (como en el Tlender)
- **X / Y / Z** bloquean el movimiento a ese eje; **Shift+X / Y / Z** al plano sin él. Púlsalo de nuevo para Mundo/Local; otra vez, desactivado.
- **Escribe el valor**: 250 mueve 250 cm, 90 gira 90°, 2 duplica la escala. **Tab** pasa al siguiente eje, **-** invierte el signo, **Retroceso** borra.
- **Ctrl** invierte el ajuste (snapping) de la barra de herramientas; **Shift** mueve una décima (precisión); **+ / -** cambian el tamaño del gizmo.
- **Clic derecho** o **Esc** cancelan: todo vuelve atrás y nada llega a Ctrl+Z. Un clic que no movió nada tampoco crea un paso de deshacer.
- **Shift** al agarrar una flecha usa el plano de los otros dos ejes.
- El **cursor** muestra lo que hace el control (flechas para mover, una mano para girar) y, mientras arrastras, pasa a ser el cursor de la transformación.

## En la barra de herramientas de la ventana gráfica
- **Orientación** (globo): **World**, **Local** (los ejes del actor) o **View** (la pantalla). **Ctrl+`** alterna Mundo/Local.
- **Pivote** (diana): **Active Actor**, **Median Point**, **Bounding Box Center** u **Individual Origins** (cada actor gira sobre su propio centro). **Only Locations** mueve los actores respecto al pivote sin girarlos ni escalarlos.
- Rotar tiene un **trackball** dentro de sus anillos: arrastra para girar libremente (el lado que te mira sigue al mouse).
- **Gizmo Size**: Preferencias del editor > Ventana gráfica. [Mostrar dónde](show:Edit>Editor Preferences...)
