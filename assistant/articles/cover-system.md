id: cover-system
kind: recipe
title: Cobertura em terceira pessoa: entrar, andar na parede, espiar | Third-person cover: in, along the wall, peek | Cobertura en tercera persona: entrar, avanzar por la pared, asomarse
keys: cobertura, cover, sistema de cobertura, cover system, encostar na parede, take cover, se esconder atras da parede, hide behind wall, agachar atras, crouch behind, espiar, peek, mirar pela borda, aim around corner, tiro em terceira pessoa, third person shooter, gears, enter cover, exit cover, is in cover, cover peek
--- pt
```diagram steps
Enter Cover (acha a parede mais perto) | De costas na parede; baixa: agacha | Anda só para os lados, para nas pontas | Na ponta: Cover Peek | Exit Cover
```
## Como usar
- **Enter Cover** (Target, Search Distance) leva o Character até a parede mais perto (dentro de Search Distance, 200 cm se 0), **de costas para ela**. Se a parede é **baixa** (menos de ~120 cm), ele **agacha** atrás dela.
- Em cobertura ele **só anda para os lados** ao longo da parede, mais devagar, e **para nas pontas**.
- **Is In Cover** diz se está em cobertura, se é **Low** e se está **At Left Edge** / **At Right Edge**.
- **Cover Peek** (Peek = true) numa ponta **inclina para fora** para mirar pela borda; atrás de uma parede baixa, **levanta** para mirar por cima. Peek = false volta.
- **Exit Cover** solta o personagem.

```blueprint
Event Key Q -> Branch ( Is In Cover ) True -> Exit Cover ( Target = self )
                                      False -> Enter Cover ( Target = self, Search Distance = 200 )
Event Key Mouse Right -> Cover Peek ( Target = self, Peek = true )
```
--- en
```diagram steps
Enter Cover (finds the nearest wall) | Back to the wall; low: crouches | Moves only sideways, stops at the ends | At an end: Cover Peek | Exit Cover
```
## How to use it
- **Enter Cover** (Target, Search Distance) takes the Character to the nearest wall (within Search Distance, 200 cm when 0), **with its back to it**. If the wall is **low** (under ~120 cm), it **crouches** behind it.
- In cover it **only moves sideways** along the wall, slower, and **stops at the ends**.
- **Is In Cover** says whether it is in cover, whether it is **Low** and whether it is **At Left Edge** / **At Right Edge**.
- **Cover Peek** (Peek = true) at an end **leans out** to aim around the edge; behind a low wall it **stands up** to aim over it. Peek = false goes back.
- **Exit Cover** lets the character go.

```blueprint
Event Key Q -> Branch ( Is In Cover ) True -> Exit Cover ( Target = self )
                                      False -> Enter Cover ( Target = self, Search Distance = 200 )
Event Key Mouse Right -> Cover Peek ( Target = self, Peek = true )
```
--- es
```diagram steps
Enter Cover (busca la pared más cercana) | De espaldas a la pared; baja: se agacha | Se mueve solo hacia los lados, se detiene en los extremos | En un extremo: Cover Peek | Exit Cover
```
## Cómo usarlo
- **Enter Cover** (Target, Search Distance) lleva al Character a la pared más cercana (dentro de Search Distance, 200 cm cuando es 0), **de espaldas a ella**. Si la pared es **baja** (menos de ~120 cm), se **agacha** detrás.
- En cobertura **solo se mueve hacia los lados** a lo largo de la pared, más lento, y **se detiene en los extremos**.
- **Is In Cover** dice si está en cobertura, si es **Low** y si está **At Left Edge** / **At Right Edge**.
- **Cover Peek** (Peek = true) en un extremo **se inclina hacia fuera** para apuntar por la esquina; detrás de una pared baja **se pone de pie** para apuntar por encima. Peek = false vuelve.
- **Exit Cover** suelta al personaje.

```blueprint
Event Key Q -> Branch ( Is In Cover ) True -> Exit Cover ( Target = self )
                                      False -> Enter Cover ( Target = self, Search Distance = 200 )
Event Key Mouse Right -> Cover Peek ( Target = self, Peek = true )
```
