id: prototyping
kind: howto
title: Montar fases rápido (peças de protótipo) | Build levels fast (prototyping pieces) | Construir niveles rápido (piezas de prototipado)
keys: alcas, handles, esticar, portas, mais portas, door, prototipo, blockout, greybox, montar fase rapido, caixa, escada, rampa, parede com porta, sala, corredor, plataforma, level design, array de portas, quantas portas, door array, number of doors, spline, curva, muro curvo, curved wall, corredor curvo, caminho, path, estrada, road, pontos, points
--- pt
- **Ator > Adicionar** tem as peças: **Caixa, Escada, Rampa, Parede (com porta), Sala, Corredor, Plataforma**, ajustadas por números no Details (degraus, largura, altura).
- Já colidem e a navegação da IA anda nelas: dá para testar a fase antes da arte. [Adicionar uma sala](add:Room) [Adicionar uma rampa](add:Ramp)
- **Editar no viewport:** com a peça selecionada, quadradinhos laranja nas faces: arraste para esticar (a face oposta fica parada); o azul na porta a desliza pela parede. **Portas:** Parede e Corredor podem ter de 1 a 8 portas; a Sala pode ter porta também atrás, à esquerda e à direita.
- **Array de portas:** cada parede com porta ganha, logo abaixo da caixa (Porta da Frente, de Trás, Esquerda, Direita), um **array "Portas"** como o da Unreal: **+** adiciona uma porta, a lixeira volta a uma, o **x** de cada elemento remove aquela. Até 8 por parede, distribuídas por igual.
- **Por spline (como o rio):** no Detalhes de um Muro, Corredor, Caixa, Rampa ou Plataforma, **Fazer Spline**: a peça segue uma curva pelos pontos. Arraste os pontos no viewport (Shift: altura; Alt: copia o ponto); botão direito num ponto adiciona ou apaga; **Reta de Novo** volta ao normal.
--- en
- **Actor > Add** has the pieces: **Box, Stairs, Ramp, Wall (with a door), Room, Corridor, Platform**, set by numbers in Details (steps, width, height).
- They collide already and the AI's navigation walks on them: you can test the level before the art. [Add a room](add:Room) [Add a ramp](add:Ramp)
- **Edit in the viewport:** with the piece selected, orange squares on its faces: drag to stretch (the opposite face stays); the blue one on the door slides it along the wall. **Doors:** a Wall and a Corridor take 1 to 8 doors; a Room can also have doors at the back, left and right.
- **Door array:** each wall with a door gets, right under its checkbox (Front, Back, Left, Right Door), a **"Doors" array** like Unreal's: **+** adds a door, the bin goes back to one, each element's **x** removes it. Up to 8 a wall, spread evenly.
- **Along a spline (as a river):** in the Details of a Wall, Corridor, Box, Ramp or Platform, **Make Spline**: the shape follows a curve through points. Drag the points in the viewport (Shift: height; Alt: copies the point); right click a point to add or delete; **Straight Again** goes back.
--- es
- **Actor > Añadir** tiene las piezas: **Box, Stairs, Ramp, Wall (con puerta), Room, Corridor, Platform**, ajustadas con números en Detalles (escalones, ancho, alto).
- Ya colisionan y la navegación de la IA camina sobre ellas: puedes probar el nivel antes del arte. [Añadir una sala](add:Room) [Añadir una rampa](add:Ramp)
- **Editar en la ventana gráfica:** con la pieza seleccionada, cuadrados naranjas en sus caras: arrastra para estirar (la cara opuesta se queda); el azul de la puerta la desliza por la pared. **Puertas:** una Wall y un Corridor admiten de 1 a 8 puertas; una Room también puede tener puertas atrás, a la izquierda y a la derecha.
- **Array de puertas:** cada pared con puerta recibe, justo debajo de su casilla (Front, Back, Left, Right Door), un **array "Doors"** como el de Unreal: **+** añade una puerta, la papelera vuelve a una, la **x** de cada elemento la quita. Hasta 8 por pared, repartidas por igual.
- **A lo largo de una spline (como un río):** en los Detalles de una Wall, Corridor, Box, Ramp o Platform, **Make Spline**: la forma sigue una curva por puntos. Arrastra los puntos en la ventana gráfica (Shift: altura; Alt: copia el punto); clic derecho en un punto para añadir o borrar; **Straight Again** vuelve atrás.
