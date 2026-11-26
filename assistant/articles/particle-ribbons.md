id: particle-ribbons
kind: howto
title: Rastros e fitas de partículas (Ribbon Renderer) | Particle trails and ribbons (Ribbon Renderer) | Estelas y cintas de partículas (Ribbon Renderer)
keys: ribbon, ribbons, ribbon renderer, fita, fitas, rastro, rastros, trail, trails, rastro de espada, rastro atras da espada, rastro do ataque, sword trail, attack trail, swing trail, rastro de projetil, projectile trail, rastro de bala, bullet trail, rastro magico, magic trail, feixe, beam, faixa de luz, light streak, cauda, tail, particulas em fita, ribbon trail
--- pt
```diagram steps
Sistema de Partículas: Render > Ribbon Renderer (no lugar do Sprite)
Cada nascimento ligado ao seguinte: uma fita
A largura é o tamanho da partícula; a cor é a dela
Mova o ator (ou dê velocidade): o rastro fica para trás
```
O **Ribbon Renderer** (o da Niagara) desenha uma fita passando pelas partículas na ordem em que nasceram, em vez de um sprite em cada: rastros de espada e de projéteis, feixes, fitas de magia.
- **Como usar:** no editor do Sistema de Partículas, troque o **Sprite Renderer** por **Render > Ribbon Renderer**, ou crie um com **Adicionar > Efeitos Visuais > Sistema de Partículas > Ribbon Trail**.
- **Largura e cor:** a largura é o tamanho de cada partícula (**Initial Size**, **Size over Life**) vezes **Width Scale**; a cor e a opacidade são as delas (**Color over Life**). **Tail Fade** apaga aos poucos a ponta mais velha.
- **Brilho e textura:** **Additive** e **Glow** como nos sprites (magia, energia); **Lit** para fumaça em fita. Um material dá a textura, que corre ao longo da fita a cada **UV Repeat** partículas.
- **Rastro de algo que se move:** com **Spawn Rate** alto (60 ou mais), mova o ator (anexado a uma espada, a um projétil): cada partícula fica onde nasceu e a fita desenha o caminho.
- **Custo:** um quadrado por par de partículas, sem ordenação (os nascimentos já vêm em ordem); a fita se parte sozinha onde duas partículas seguidas se afastam mais do que o movimento delas explica.
--- en
```diagram steps
Particle System: Render > Ribbon Renderer (instead of the Sprite)
Each birth joined to the next: a ribbon
Its width is the particle's size; its color is the particle's
Move the actor (or give it speed): the trail stays behind
```
The **Ribbon Renderer** (Niagara's) draws a strip through the particles in the order they were born, instead of a sprite on each: sword and projectile trails, beams, ribbons of magic.
- **How to use:** in the Particle System editor, replace the **Sprite Renderer** with **Render > Ribbon Renderer**, or make one with **Add > Visual Effects > Particle System > Ribbon Trail**.
- **Width and color:** the width is each particle's size (**Initial Size**, **Size over Life**) times **Width Scale**; the color and opacity are theirs (**Color over Life**). **Tail Fade** fades the oldest end out.
- **Glow and texture:** **Additive** and **Glow** as on sprites (magic, energy); **Lit** for a ribbon of smoke. A material gives the texture, which runs along the ribbon every **UV Repeat** particles.
- **Trail of something moving:** with a high **Spawn Rate** (60 or more), move the actor (attached to a sword, a projectile): each particle stays where it was born and the ribbon draws the path.
- **Cost:** one quad per pair of particles, no sorting (the births already come in order); the ribbon breaks by itself where two particles in a row are farther apart than their motion explains.
--- es
```diagram steps
Particle System: Render > Ribbon Renderer (en lugar del Sprite)
Cada nacimiento unido al siguiente: una cinta
Su ancho es el tamaño de la partícula; su color es el de la partícula
Mueve el actor (o dale velocidad): la estela queda atrás
```
El **Ribbon Renderer** (el de Niagara) dibuja una tira a través de las partículas en el orden en que nacieron, en lugar de un sprite en cada una: estelas de espadas y proyectiles, haces, cintas de magia.
- **Cómo usarlo:** en el editor de Particle System, reemplaza el **Sprite Renderer** por **Render > Ribbon Renderer**, o crea uno con **Añadir > Visual Effects > Particle System > Ribbon Trail**.
- **Ancho y color:** el ancho es el tamaño de cada partícula (**Initial Size**, **Size over Life**) por **Width Scale**; el color y la opacidad son los de ellas (**Color over Life**). **Tail Fade** desvanece el extremo más antiguo.
- **Brillo y textura:** **Additive** y **Glow** como en los sprites (magia, energía); **Lit** para una cinta de humo. Un material da la textura, que se repite a lo largo de la cinta cada **UV Repeat** partículas.
- **Estela de algo en movimiento:** con un **Spawn Rate** alto (60 o más), mueve el actor (unido a una espada, un proyectil): cada partícula se queda donde nació y la cinta dibuja el recorrido.
- **Costo:** un quad por cada par de partículas, sin ordenar (los nacimientos ya llegan en orden); la cinta se corta por sí sola donde dos partículas seguidas están más lejos de lo que su movimiento explica.
