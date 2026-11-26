id: sockets-in-game
kind: recipe
title: Sockets no jogo: prender armas, chapéus e acessórios | Sockets in the game: attaching weapons, hats and accessories | Sockets en el juego: sujetar armas, sombreros y accesorios
keys: socket, sockets, prender no socket, attach to socket, prender ator, attach actor, arma na mao, weapon in hand, espada, sword, chapeu, hat, lanterna no cinto, lantern on the belt, acessorio, accessory, deslocamento do socket, socket offset, posicao do socket, socket location, rotacao do socket, socket rotation, cano da arma, muzzle, attach actor to socket, detach from socket, get socket location, get socket rotation
--- pt
Um **socket** é um lugar com nome num osso (Editor de Skeletal Mesh > aba Esqueleto > **Add Socket**, movido com o gizmo). No jogo, o que é preso num socket acompanha a **pose a cada quadro** — a animação, o ragdoll e os ossos dinâmicos — no **deslocamento e na rotação do socket**, mais o deslocamento próprio.
- **Prender um ator**: **Attach Actor To Socket** (Target: o ator que vai preso; Parent: o ator com o Skeletal Mesh; Socket Name: o socket ou o nome de um osso; Location / Rotation: um deslocamento a mais no espaço do socket). **Detach From Socket** solta onde está.
- **Itens equipados** (Inventário > Equip Item) vão para o socket do Item Definition sozinhos, inclusive em personagens com skin.
- **Ler o lugar**: **Get Socket Location** / **Get Socket Rotation** (de onde sai o tiro, onde nasce um efeito).

```diagram steps
Skeletal Mesh Editor | Esqueleto: Add Socket | Play | Attach Actor To Socket | segue a mão
```
```blueprint
Event Begin Play -> Attach Actor To Socket ( Target = self, Parent = Get Player Character, Socket Name = "hand_r" )
Event Key F -> Detach From Socket ( Target = self )
```
--- en
A **socket** is a named place on a bone (Skeletal Mesh Editor > Skeleton tab > **Add Socket**, moved with the gizmo). In the game, what hangs on a socket follows **the pose every frame** — the animation, the ragdoll and the dynamic bones — at the **socket's offset and rotation**, plus its own offset.
- **Attach an actor**: **Attach Actor To Socket** (Target: the actor to hang; Parent: the actor with the Skeletal Mesh; Socket Name: the socket, or a bone's name; Location / Rotation: an offset more in the socket's space). **Detach From Socket** lets it go where it is.
- **Equipped items** (Inventory > Equip Item) go to their Item Definition's socket by themselves, skinned characters too.
- **Read the place**: **Get Socket Location** / **Get Socket Rotation** (where a shot comes from, where an effect spawns).

```diagram steps
Skeletal Mesh Editor | Skeleton: Add Socket | Play | Attach Actor To Socket | follows the hand
```
```blueprint
Event Begin Play -> Attach Actor To Socket ( Target = self, Parent = Get Player Character, Socket Name = "hand_r" )
Event Key F -> Detach From Socket ( Target = self )
```
--- es
Un **socket** es un lugar con nombre en un hueso (Editor de Skeletal Mesh > pestaña Skeleton > **Add Socket**, movido con el gizmo). En el juego, lo que cuelga de un socket sigue **la pose en cada fotograma** — la animación, el ragdoll y los huesos dinámicos — en el **desplazamiento y rotación del socket**, más su propio desplazamiento.
- **Unir un actor**: **Attach Actor To Socket** (Target: el actor que cuelga; Parent: el actor con el Skeletal Mesh; Socket Name: el socket, o el nombre de un hueso; Location / Rotation: un desplazamiento más en el espacio del socket). **Detach From Socket** lo suelta donde está.
- Los **ítems equipados** (Inventory > Equip Item) van solos al socket de su Item Definition, también en personajes con skin.
- **Leer el lugar**: **Get Socket Location** / **Get Socket Rotation** (de dónde sale un disparo, dónde se genera un efecto).

```diagram steps
Editor de Skeletal Mesh | Skeleton: Add Socket | Jugar | Attach Actor To Socket | sigue la mano
```
```blueprint
Event Begin Play -> Attach Actor To Socket ( Target = self, Parent = Get Player Character, Socket Name = "hand_r" )
Event Key F -> Detach From Socket ( Target = self )
```
