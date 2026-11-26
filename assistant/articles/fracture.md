id: fracture
kind: howto
title: Objetos que quebram (fratura) | Objects that break (fracture) | Objetos que se rompen (fractura)
keys: quebrar, fratura, destruir, destrutivel, estilhacar, pedacos, explodir caixa, fracture, break, destructible, shatter, chaos
--- pt
```diagram steps
Ator com malha | Detalhes > Adicionar > Física | Simular Física + Fratura | Bate rápido: vira pedaços
```
1. Selecione um ator com malha (uma caixa, um vaso).
2. **Detalhes > Adicionar > Física** e ligue **Simular Física**. [Abrir os Detalhes](open:details)
3. Ligue **Fratura**. Ajuste:
   - **Velocidade de Fratura**: a pancada (cm/s) que quebra. 300 quebra numa queda de ~0,5 m; 1000 só com força.
   - **Pedaços por Eixo** (2-4): 2 = 8 pedaços, 4 = 64.
4. Aperte Play (**Alt+P**) e derrube, empurre ou atire algo nele.
- Os pedaços herdam a velocidade e o giro da parte que bateu e voam para fora. Até 512 pedaços por quadro.
- Para quebrar por Grafo: **Add Impulse** ou **Add Radial Impulse** (explosão) acima da velocidade de fratura.
- No editor, ao parar o Play o objeto volta inteiro.
- Veja também: [Física](article:physics), [Empurrar com física](article:physics-push).
--- en
```diagram steps
An actor with a mesh | Details > Add > Physics | Simulate Physics + Fracture | Hit hard: it breaks into pieces
```
1. Select an actor with a mesh (a crate, a vase).
2. **Details > Add > Physics** and turn on **Simulate Physics**. [Open the Details](open:details)
3. Turn on **Fracture**. Tune:
   - **Fracture Speed**: the impact (cm/s) that breaks it. 300 breaks on a ~0.5 m fall; 1000 needs force.
   - **Pieces per Axis** (2-4): 2 = 8 pieces, 4 = 64.
4. Press Play (**Alt+P**) and drop, push or throw something at it.
- The pieces keep the speed and spin of the part that hit and fly outward. Up to 512 pieces a frame.
- To break it from a Graph: **Add Impulse** or **Add Radial Impulse** (an explosion) above the fracture speed.
- In the editor, stopping Play brings the object back whole.
- See also: [Physics](article:physics), [Push with physics](article:physics-push).
--- es
```diagram steps
Un actor con una malla | Detalles > Añadir > Physics | Simulate Physics + Fracture | Golpéalo fuerte: se rompe en pedazos
```
1. Selecciona un actor con una malla (una caja, un jarrón).
2. **Detalles > Añadir > Physics** y activa **Simulate Physics**. [Abrir los Detalles](open:details)
3. Activa **Fracture**. Ajusta:
   - **Fracture Speed**: el impacto (cm/s) que lo rompe. 300 rompe en una caída de ~0,5 m; 1000 necesita fuerza.
   - **Pieces per Axis** (2-4): 2 = 8 piezas, 4 = 64.
4. Pulsa Jugar (**Alt+P**) y suelta, empuja o lanza algo contra él.
- Los pedazos conservan la velocidad y el giro de la parte que golpeó y salen despedidos hacia fuera. Hasta 512 pedazos por fotograma.
- Para romperlo desde un Grafo: **Add Impulse** o **Add Radial Impulse** (una explosión) por encima de la fracture speed.
- En el editor, al detener Jugar el objeto vuelve entero.
- Mira también: [Física](article:physics), [Empujar con física](article:physics-push).
