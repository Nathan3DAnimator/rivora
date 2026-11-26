id: hit-reaction
kind: recipe
title: Reação ao golpe pela direção | Hit reaction by direction | Reacción al golpe según la dirección
keys: reacao ao golpe, hit reaction, levou um tiro, got shot, apanhou, took a hit, flinch, tranco, recuo do corpo, body jolt, golpe pela frente, hit from the front, golpe pelas costas, hit from behind, play hit reaction, direcao do golpe, hit direction
--- pt
```diagram steps
O golpe chega (Event On Montage Hit, Any Damage) | Play Hit Reaction (lugar do golpe, força) | Coluna e cabeça jogadas para longe e voltam | Montage do lado, se existir
```
## Como usar
- **Play Hit Reaction** (Target, Hit Location, Strength, Montage Prefix) faz o personagem reagir pelo **lado** de onde veio o golpe: **Front**, **Back**, **Left** ou **Right** (devolvido no Return Value).
- Sempre: a **coluna e a cabeça** são jogadas para longe do golpe e voltam como uma mola (cerca de meio segundo), sem precisar de animação. **Strength**: 0,3 um tapa, 1 um soco, 2 uma marretada.
- Com animações: dê o começo do nome em **Montage Prefix** (por exemplo `AM_Hit`) e tenha `AM_Hit_Front`, `AM_Hit_Back`, `AM_Hit_Left`, `AM_Hit_Right`; o do lado toca (sem ele, o `_Front`).
- Medido: um soco pela frente leva a cabeça 10,8 cm para trás; pela direita, 9,8 cm para a esquerda; 1,3 s depois ela está de volta.

```blueprint
Event On Montage Hit -> Play Hit Reaction ( Target = Other Actor, Hit Location = Location, Strength = 1, Montage Prefix = "AM_Hit" )
```
--- en
```diagram steps
The hit arrives (Event On Montage Hit, Any Damage) | Play Hit Reaction (where, how hard) | Spine and head thrown away and back | The side's montage, when there is one
```
## How to use it
- **Play Hit Reaction** (Target, Hit Location, Strength, Montage Prefix) makes the character react by the **side** the hit came from: **Front**, **Back**, **Left** or **Right** (returned in Return Value).
- Always: the **spine and the head** are thrown away from the hit and spring back (about half a second), with no animation needed. **Strength**: 0.3 a slap, 1 a punch, 2 a hammer.
- With animations: give the name's start in **Montage Prefix** (for example `AM_Hit`) and have `AM_Hit_Front`, `AM_Hit_Back`, `AM_Hit_Left`, `AM_Hit_Right`; the side's one plays (without it, `_Front`).
- Measured: a punch from the front takes the head 10.8 cm back; from the right, 9.8 cm left; 1.3 s later it is back.

```blueprint
Event On Montage Hit -> Play Hit Reaction ( Target = Other Actor, Hit Location = Location, Strength = 1, Montage Prefix = "AM_Hit" )
```
--- es
```diagram steps
Llega el golpe (Event On Montage Hit, Any Damage) | Play Hit Reaction (dónde, con qué fuerza) | Columna y cabeza lanzadas hacia atrás y de vuelta | El montage del lado, cuando existe
```
## Cómo usarlo
- **Play Hit Reaction** (Target, Hit Location, Strength, Montage Prefix) hace que el personaje reaccione según el **lado** de donde vino el golpe: **Front**, **Back**, **Left** o **Right** (devuelto en Return Value).
- Siempre: la **columna y la cabeza** son lanzadas lejos del golpe y vuelven con un rebote (cerca de medio segundo), sin necesidad de animación. **Strength**: 0,3 una bofetada, 1 un puñetazo, 2 un martillazo.
- Con animaciones: da el comienzo del nombre en **Montage Prefix** (por ejemplo `AM_Hit`) y ten `AM_Hit_Front`, `AM_Hit_Back`, `AM_Hit_Left`, `AM_Hit_Right`; se reproduce el del lado (sin él, `_Front`).
- Medido: un puñetazo de frente lleva la cabeza 10,8 cm hacia atrás; desde la derecha, 9,8 cm a la izquierda; 1,3 s después está de vuelta.

```blueprint
Event On Montage Hit -> Play Hit Reaction ( Target = Other Actor, Hit Location = Location, Strength = 1, Montage Prefix = "AM_Hit" )
```
