id: montage-combo-hit
kind: recipe
title: Combos e acertos de golpe no montage | Combos and attack hits in a montage | Combos y golpes en un Montage
keys: combo, combos, encadear golpes, chain attacks, janela de combo, combo window, golpe, hit, acerto, hitbox, hit window, janela de acerto, notify state, anim notify state, luta, fighting, soco, punch, montage combo input, is in montage window, on montage hit, parry, invencibilidade
--- pt
```diagram steps
Montage com seções (Golpe1, Golpe2) | Janela Combo (Próxima Seção) | Janela Hit (osso e raio) | Montage Combo Input no botão | On Montage Hit: Apply Damage
```
## Janelas no Montage Editor
Em **Janelas**, **Adicionar Janela** cria uma com nome, tipo e tempo:
- **State**: só avisa quando abre e fecha (**Event On Montage Window**): um bloqueio, uma invencibilidade.
- **Combo**: se o jogador apertar dentro dela (ou um pouco antes), o montage segue para a **Próxima Seção** quando ela fecha; senão o golpe **termina com a seção** em que está.
- **Hit**: uma esfera (**Raio do Golpe**) varre o **Osso do Golpe** (punho, lâmina, pé) a cada quadro; **Event On Montage Hit** roda **uma vez por ator** que ela toca: quem, onde e qual janela.
- **Is In Montage Window** diz se uma janela de certo nome está aberta agora.

```blueprint
Event Key Mouse Left -> Branch ( Is Montage Playing ) True -> Montage Combo Input ( Target = self )
                                                      False -> Play Montage ( Target = self, Play Rate = 1 )
Event On Montage Hit -> Apply Damage ( Damaged Actor = Other Actor, Base Damage = 20 )
```
O montage do golpe (com as seções e as janelas) fica nos Detalhes do Play Montage.
--- en
```diagram steps
Montage with sections (Attack1, Attack2) | Combo window (Next Section) | Hit window (bone and radius) | Montage Combo Input on the button | On Montage Hit: Apply Damage
```
## Windows in the Montage Editor
Under **Windows**, **Add Window** makes one with a name, a kind and its time:
- **State**: only tells when it opens and closes (**Event On Montage Window**): a parry, an invulnerability.
- **Combo**: if the player presses inside it (or a little before), the montage goes on to its **Next Section** when it closes; otherwise the attack **ends with the section** it is in.
- **Hit**: a sphere (**Hit Radius**) sweeps the **Hit Bone** (fist, blade, foot) every frame; **Event On Montage Hit** runs **once per actor** it touches: who, where and which window.
- **Is In Montage Window** says whether a window of a name is open now.

```blueprint
Event Key Mouse Left -> Branch ( Is Montage Playing ) True -> Montage Combo Input ( Target = self )
                                                      False -> Play Montage ( Target = self, Play Rate = 1 )
Event On Montage Hit -> Apply Damage ( Damaged Actor = Other Actor, Base Damage = 20 )
```
The attack montage (with its sections and windows) goes in the Play Montage's Details.
--- es
```diagram steps
Montage con secciones (Attack1, Attack2) | Ventana de combo (Next Section) | Ventana de golpe (hueso y radio) | Montage Combo Input en el botón | On Montage Hit: Apply Damage
```
## Ventanas en el Montage Editor
En **Windows**, **Add Window** crea una con un nombre, un tipo y su tiempo:
- **State**: solo avisa cuándo se abre y se cierra (**Event On Montage Window**): una parada, una invulnerabilidad.
- **Combo**: si el jugador pulsa dentro de ella (o un poco antes), el montage pasa a su **Next Section** cuando se cierra; si no, el ataque **termina con la sección** en que está.
- **Hit**: una esfera (**Hit Radius**) barre el **Hit Bone** (puño, hoja, pie) en cada fotograma; **Event On Montage Hit** se ejecuta **una vez por actor** que toca: quién, dónde y qué ventana.
- **Is In Montage Window** dice si una ventana de un nombre está abierta ahora.

```blueprint
Event Key Mouse Left -> Branch ( Is Montage Playing ) True -> Montage Combo Input ( Target = self )
                                                      False -> Play Montage ( Target = self, Play Rate = 1 )
Event On Montage Hit -> Apply Damage ( Damaged Actor = Other Actor, Base Damage = 20 )
```
El montage de ataque (con sus secciones y ventanas) va en los Detalles del Play Montage.
