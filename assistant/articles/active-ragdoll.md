id: active-ragdoll
kind: recipe
title: Ragdoll Ativo: física que segue a animação | Active Ragdoll: physics that follows the animation
keys: ragdoll ativo, active ragdoll, euphoria, ragdoll que segue a animacao, physical animation, animacao fisica, musculos, muscles, forca do ragdoll, ragdoll strength, equilibrio, balance, tropecar, stumble, cair, fall, levantar, get up, levantar sozinho, auto get up, reacao ao golpe, hit reaction, tiro no braco, soco, empurrao, push, perfil muscular, muscle profile, bebado, drunk, mole, limp, quadrupede, quadruped, animal, criatura, creature, set active ragdoll, apply hit reaction, get balance state, is balanced
--- pt
O **Ragdoll Ativo** é um ragdoll que continua tentando seguir a animação, como o Euphoria: todos os corpos são simulados, e cada junta tem um **músculo** que gira o corpo de volta para a pose animada. O personagem anda, fica parado e anima normalmente, mas um golpe, um empurrão ou uma queda o movem de verdade. Empurrado demais, ele **tropeça**, depois **cai**, e quando fica parado no chão **levanta sozinho**, misturando de volta para a animação onde caiu.
- **Ligar**: no Skeletal Mesh (com um Physics Asset), Detalhes > Simular Física > **Ragdoll Ativo**, ou o nó **Set Active Ragdoll**.
- **Qualquer esqueleto**: humanos, quadrúpedes, criaturas. Os pés (as pontas mais baixas: dois pés, quatro patas) são achados sozinhos.
- **Força**: **Set Active Ragdoll Strength** (0 mole, 1 normal, acima de 1 rígido), no corpo todo ou a partir de um osso para baixo (um braço que fica mole), com um tempo de mistura. No Physics Asset, cada corpo tem um **Músculo** (pernas e coluna seguram mais que braços e cabeça).
- **Perfis**: **Set Muscle Profile**: Default, Relaxed, Tense, Protective (braços firmes), Drunk (solto e oscilando), Limp.
- **Golpes**: **Apply Hit Reaction** (osso, impulso, Weaken, Recover Time): empurra aquele corpo e enfraquece os músculos dele e os de baixo, que voltam com o tempo. Um golpe forte derruba.
- **Equilíbrio**: **Get Balance State** (0 Equilibrado, 1 Tropeçando, 2 Caído, 3 Levantando) e **Is Balanced**. **Get Up** levanta na hora; **Set Auto Get Up** liga ou desliga o levantar sozinho (desligado: fica caído, para um nocaute).
- **Custo**: ~0,036 ms de CPU por personagem em pé (50 personagens: 1,8 ms nas threads da animação). Parados no chão, dormem.

```diagram steps
Skeletal Mesh + Physics Asset | Simular Física: Ragdoll Ativo | Play | Apply Hit Reaction | cai e levanta sozinho
```
```blueprint
Event Begin Play -> Set Active Ragdoll ( Target = self, Enabled = true )
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "spine_03", Impulse = Make Vector(600, 0, 100), Weaken = 0.2, Recover Time = 0.8 )
Event Key B -> Set Muscle Profile ( Target = self, Profile = "Drunk" )
```
--- en
The **Active Ragdoll** is a ragdoll that keeps trying to follow the animation, like Euphoria: every body is simulated, and each joint has a **muscle** turning its body back toward the animated pose. The character walks, idles and plays as animated, but a hit, a push or a fall moves it for real. Pushed too hard it **stumbles**, then **falls**, and once it lies still it **gets up by itself**, blending back into the animation where it fell.
- **Turn it on**: on the Skeletal Mesh (with a Physics Asset), Details > Simulate Physics > **Active Ragdoll**, or the **Set Active Ragdoll** node.
- **Any skeleton**: people, quadrupeds, creatures. The feet (the lowest ends: two feet, four paws) are found by themselves.
- **Strength**: **Set Active Ragdoll Strength** (0 limp, 1 normal, above 1 stiff), on the whole body or from a bone down (an arm going slack), over a blend time. In the Physics Asset each body has a **Muscle** (legs and spine hold more than arms and head).
- **Profiles**: **Set Muscle Profile**: Default, Relaxed, Tense, Protective (arms firm), Drunk (loose, swaying), Limp.
- **Hits**: **Apply Hit Reaction** (bone, impulse, Weaken, Recover Time): pushes that body and weakens its muscles and those below it, coming back over time. A strong hit knocks it down.
- **Balance**: **Get Balance State** (0 Balanced, 1 Stumbling, 2 Fallen, 3 Getting Up) and **Is Balanced**. **Get Up** gets up now; **Set Auto Get Up** turns getting up by itself on or off (off: it stays down, a knockout).
- **Cost**: ~0.036 ms of CPU per standing character (50 characters: 1.8 ms on the animation's threads). Lying still, they sleep.

```diagram steps
Skeletal Mesh + Physics Asset | Simulate Physics: Active Ragdoll | Play | Apply Hit Reaction | falls and gets up by itself
```
```blueprint
Event Begin Play -> Set Active Ragdoll ( Target = self, Enabled = true )
Event AnyDamage -> Apply Hit Reaction ( Target = self, Bone Name = "spine_03", Impulse = Make Vector(600, 0, 100), Weaken = 0.2, Recover Time = 0.8 )
Event Key B -> Set Muscle Profile ( Target = self, Profile = "Drunk" )
```
