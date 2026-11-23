id: ragdoll-get-up
kind: recipe
title: Levantar do ragdoll com montagens (de bruços e de costas) | Getting up from the ragdoll with montages (face down and on the back)
keys: levantar do chao, get up, get up animation, animacao de levantar, levantar de brucos, get up face down, levantar de costas, get up on its back, montagem de levantar, get up montage, am_getup_front, am_getup_back, mm_getup_front, mm_getup_back, ragdoll para animacao, ragdoll to animation, sair do ragdoll, recover from ragdoll, transicao do ragdoll, ragdoll transition, salto ao levantar, pop when getting up, pe atravessando o chao, foot through the floor, levantar sozinho, auto get up
--- pt
Caído, o **Ragdoll Ativo** levanta com uma **montagem de levantar**: **Levantar de Bruços** quando está de bruços, **Levantar de Costas** quando está de costas (Physics Asset > Configurações do Ragdoll). A troca não salta:
- A montagem começa **parada no primeiro quadro** (deitada como o clipe deita) enquanto o corpo ainda está solto.
- 0,3 s depois o ator **gira e anda** para que a bacia e a direção do clipe (bacia → peito) caiam em cima das do corpo; só então a montagem anda.
- O corpo se mistura nela em **0,35 s** e o resto é a montagem, com o ragdoll seguindo pelos músculos.
- O projeto já vem com as duas: **AM_GetUp_Front** e **AM_GetUp_Back** (Characters/Mannequin/GetUp, feitas à mão no formato da engine: mãos e pés plantados por IK). Um Physics Asset gerado para o esqueleto do Mannequin já as escolhe sozinho.
- **A sua montagem**: comece deitado e termine em pé na origem do ator, de frente para +X (de bruços com a cabeça para +X; de costas com a cabeça para -X). Ela é casada com o corpo pela primeira pose, então pode começar se mexendo.
- **Custo**: a pose da montagem e o casamento custam o mesmo que uma montagem comum; o teste `getup_q10` mede o salto na troca, o pé contra o chão e a posição final.

```diagram steps
Golpe forte | cai (de bruços ou de costas) | parado no chão | montagem no 1º quadro, ator casado com o corpo | mistura em 0,35 s | montagem até ficar em pé
```
```blueprint
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "spine_03", Impulse = Make Vector(1000, 0, 100), Weaken = 0.1, Recover Time = 0.8 )
```
--- en
Down, the **Active Ragdoll** gets up with a **get-up montage**: **Get Up Face Down** when it lies face down, **Get Up On Its Back** when it lies on its back (Physics Asset > Ragdoll Settings). The hand-over doesn't jump:
- The montage starts **held on its first frame** (lying as the clip lies) while the body is still loose.
- 0.3 s later the actor **turns and moves** so the clip's pelvis and heading (pelvis to chest) land on the body's; only then does the montage play.
- The body blends into it over **0.35 s**, and the rest is the montage, the ragdoll following by its muscles.
- Projects come with both: **AM_GetUp_Front** and **AM_GetUp_Back** (Characters/Mannequin/GetUp, keyed in the engine's own format: hands and feet planted by IK). A Physics Asset generated for the Mannequin's skeleton picks them by itself.
- **Your own montage**: start lying and end standing at the actor's origin facing +X (face down with the head toward +X; on the back with the head toward -X). It is matched to the body by its first pose, so it may start moving.
- **Cost**: the montage's pose and the matching cost what any montage costs; the `getup_q10` test measures the jump at the hand-over, the feet against the floor and where it ends.

```diagram steps
A strong hit | falls (face down or on its back) | still on the ground | montage held on frame 1, the actor matched to the body | blended in 0.35 s | the montage until it stands
```
```blueprint
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "spine_03", Impulse = Make Vector(1000, 0, 100), Weaken = 0.1, Recover Time = 0.8 )
```
