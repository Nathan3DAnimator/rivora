id: ragdoll-wounds
kind: recipe
title: Ferimentos do Ragdoll Ativo: segurar, mancar, se contorcer | Active Ragdoll wounds: holding, limping, writhing | Heridas del Ragdoll Activo: sujetarse, cojear, retorcerse
keys: ferimento, wound, ferido, injured, mancar, limp, mancando, limping, segurar o ferimento, hold the wound, mao no ferimento, hand on wound, se contorcer, writhe, writhing, dor no chao, pain on the ground, queda alta, high fall, agitar os bracos, flail, windmill, euphoria, reacao por regiao, reaction by region, orcamento de ragdoll, ragdoll budget, muitos ragdolls, many ragdolls, apply wound reaction, get limp, is writhing
--- pt
Reações como as do GTA 6 em cima do **Ragdoll Ativo**: cada ferimento muda o corpo conforme a região atingida.
- **Apply Wound Reaction** (Target, Bone Name, Location, Severity 0..1): o ferimento num ponto do osso. Use junto com o **Apply Hit Reaction**, que dá o empurrão.
- **Tronco, braço ou cabeça**: a mão do outro lado vai até o ponto e segura (4 a 12 s, pela gravidade). No braço esquerdo, a mão direita segura.
- **Perna**: ele **manca**. O joelho ferido dobra, a pelve desce e inclina para aquele lado, e os músculos da perna ficam mais fracos (8 a 20 s). **Get Limp** devolve o quanto manca agora, para misturar uma caminhada de ferido ou reduzir a velocidade.
- **Grave (acima de 0,7)**: os joelhos cedem e ele cai. No chão ele **se contorce** alguns segundos (a coluna e as pernas se mexem) antes de levantar. **Is Writhing** fica verdadeiro enquanto isso dura: som de dor, aviso de reviver.
- **Queda alta**: caindo rápido (mais de 5 m/s por um quarto de segundo), os braços se agitam até ele bater no chão.
- **Orçamento**: só os **32 ragdolls ativos mais perto da câmera** simulam. Os outros em pé seguem a animação, e os caídos dormem onde estão. Quem está tropeçando ou levantando termina antes. 200 ragdolls ativos custam cerca de 1,5 ms.

```diagram steps
Ragdoll Ativo | Apply Hit Reaction (empurrão) | Apply Wound Reaction (região) | segura, manca ou cai e se contorce | levanta
```
```blueprint
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "calf_l", Impulse = Make Vector(300, 0, 0), Weaken = 0.5, Recover Time = 1.0 ) -> Apply Wound Reaction ( Target = self, Bone Name = "calf_l", Location = Get Actor Location, Severity = 0.6 )
Event Tick -> Print String ( Get Limp ( Target = self ) )
```
--- en
GTA 6-style reactions on top of the **Active Ragdoll**: each wound changes the body by the region it hits.
- **Apply Wound Reaction** (Target, Bone Name, Location, Severity 0..1): the wound at a point on the bone. Use it together with **Apply Hit Reaction**, which gives the push.
- **Torso, arm or head**: the other side's hand goes to the point and holds it (4 to 12 s, by the severity). On the left arm, the right hand holds it.
- **Leg**: it **limps**. The hurt knee bends, the pelvis drops and leans to that side, and the leg's muscles get weaker (8 to 20 s). **Get Limp** returns how badly it limps now, to blend an injured walk or slow it down.
- **Grave (above 0.7)**: the knees give and it falls. On the ground it **writhes** a few seconds (the spine and the legs move) before it gets up. **Is Writhing** is true while that lasts: a pain sound, a revive prompt.
- **High fall**: falling fast (over 5 m/s for a quarter second), the arms flail until it hits the ground.
- **Budget**: only the **32 active ragdolls nearest the camera** simulate. The others standing follow the animation, and the fallen ones sleep where they lie. One stumbling or getting up finishes first. 200 active ragdolls cost about 1.5 ms.

```diagram steps
Active Ragdoll | Apply Hit Reaction (push) | Apply Wound Reaction (region) | holds, limps or falls and writhes | gets up
```
```blueprint
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "calf_l", Impulse = Make Vector(300, 0, 0), Weaken = 0.5, Recover Time = 1.0 ) -> Apply Wound Reaction ( Target = self, Bone Name = "calf_l", Location = Get Actor Location, Severity = 0.6 )
Event Tick -> Print String ( Get Limp ( Target = self ) )
```
--- es
Reacciones al estilo GTA 6 sobre el **Active Ragdoll**: cada herida cambia el cuerpo según la región que golpea.
- **Apply Wound Reaction** (Target, Bone Name, Location, Severity 0..1): la herida en un punto del hueso. Úsalo junto con **Apply Hit Reaction**, que da el empujón.
- **Torso, brazo o cabeza**: la mano del otro lado va al punto y lo sujeta (4 a 12 s, según la gravedad). En el brazo izquierdo, lo sujeta la mano derecha.
- **Pierna**: **cojea**. La rodilla herida se dobla, la pelvis baja y se inclina hacia ese lado, y los músculos de la pierna se debilitan (8 a 20 s). **Get Limp** devuelve cuánto cojea ahora, para mezclar un andar herido o frenarlo.
- **Grave (más de 0.7)**: las rodillas ceden y cae. En el suelo **se retuerce** unos segundos (la columna y las piernas se mueven) antes de levantarse. **Is Writhing** es verdadero mientras dura: un sonido de dolor, un aviso para reanimar.
- **Caída alta**: cayendo rápido (más de 5 m/s durante un cuarto de segundo), los brazos se agitan hasta tocar el suelo.
- **Presupuesto**: solo simulan los **32 ragdolls activos más cercanos a la cámara**. Los demás, de pie, siguen la animación, y los caídos duermen donde yacen. El que tropieza o se levanta termina primero. 200 ragdolls activos cuestan unos 1,5 ms.

```diagram steps
Active Ragdoll | Apply Hit Reaction (empujón) | Apply Wound Reaction (región) | se sujeta, cojea o cae y se retuerce | se levanta
```
```blueprint
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "calf_l", Impulse = Make Vector(300, 0, 0), Weaken = 0.5, Recover Time = 1.0 ) -> Apply Wound Reaction ( Target = self, Bone Name = "calf_l", Location = Get Actor Location, Severity = 0.6 )
Event Tick -> Print String ( Get Limp ( Target = self ) )
```
