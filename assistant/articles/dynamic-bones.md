id: dynamic-bones
kind: recipe
title: Ossos dinâmicos: brincos, cabelo, cauda, alças e bolsas | Dynamic bones: earrings, hair, tails, straps and bags
keys: ossos dinamicos, dynamic bones, jiggle bones, spring bones, animdynamics, osso com mola, ossos com fisica, brinco, earring, rabo de cavalo, ponytail, cabelo de ossos, bone hair, cauda, rabo, tail, alca, strap, cinto, belt, bolsinha, pouch, coldre, holster, mochila, backpack, bolsa, bag, balancar, sway, swing, vento nos ossos, wind, inercia, inertia, set dynamic bones enabled, set dynamic bones wind, reset dynamic bones
--- pt
Os **ossos dinâmicos** (como o AnimDynamics da Unreal) são cadeias de ossos do esqueleto movidas por molas: balançam com o movimento do próprio personagem, a gravidade e o vento, voltam à forma animada e **colidem com os corpos do Physics Asset** (um brinco nunca entra no pescoço). Servem para brincos, rabo de cavalo e cabelo feito de ossos, cauda, alças, cinto e bolsinhas, coldre e mochila.
- **Criar**: no editor do Physics Asset, selecione o primeiro osso e **+ Cadeia no osso selecionado** com um preset (Brinco, Cabelo, Cauda, Alça / Bolsinha, Bolsa Pesada). A cadeia vai desse osso até as pontas (ou até o **Último Osso**).
- **Ajustar**: **Rigidez** (0 pende livre, 1 mantém a forma), **Amortecimento**, **Gravidade**, **Parte do Vento**, **Inércia do Personagem** (o quanto sente os passos e giros), **Raio** contra o corpo, **Limite de Ângulo**.
- **Ver**: Simular > **Ossos Dinâmicos (Balanço)** balança o esqueleto na prévia.
- No jogo rodam sozinhos (com ou sem ragdoll, depois dele). Nós: **Set Dynamic Bones Enabled** (desligue nos personagens longe), **Set Dynamic Bones Wind**, **Reset Dynamic Bones** (depois de um teleporte).
- Um acessório preso num **socket** (Attach Actor To Socket) acompanha a pose com os ossos dinâmicos.
- **Custo**: ~0,012 ms de CPU por personagem com 8 partículas (um brinco e uma cauda).

```diagram steps
Physics Asset | selecione o osso | + Cadeia (Brinco) | Simular: Balanço | Play
```
```blueprint
Event Begin Play -> Set Dynamic Bones Wind ( Target = self, Wind = Make Vector(300, 0, 0) )
Event Key R -> Reset Dynamic Bones ( Target = self )
```
--- en
**Dynamic bones** (like Unreal's AnimDynamics) are chains of the skeleton's bones moved by springs: they swing with the character's own motion, gravity and the wind, spring back to the animated shape and **collide with the Physics Asset's bodies** (an earring never goes into the neck). For earrings, a ponytail and hair made of bones, a tail, straps, a belt and its pouches, a holster and a backpack.
- **Make one**: in the Physics Asset editor, select the first bone and **+ Chain on the selected bone** with a preset (Earring, Hair, Tail, Strap / Pouch, Heavy Bag). The chain goes from that bone to the ends (or down to the **Last Bone**).
- **Tune**: **Stiffness** (0 hangs free, 1 keeps the shape), **Damping**, **Gravity**, **Wind Share**, **Character Inertia** (how much it feels the steps and turns), **Radius** against the body, **Angle Limit**.
- **See it**: Simulate > **Dynamic Bones (Sway)** sways the skeleton in the preview.
- In the game they run by themselves (with or without a ragdoll, after it). Nodes: **Set Dynamic Bones Enabled** (off on far characters), **Set Dynamic Bones Wind**, **Reset Dynamic Bones** (after a teleport).
- An accessory hung on a **socket** (Attach Actor To Socket) follows the pose with the dynamic bones.
- **Cost**: ~0.012 ms of CPU per character with 8 particles (an earring and a tail).

```diagram steps
Physics Asset | select the bone | + Chain (Earring) | Simulate: Sway | Play
```
```blueprint
Event Begin Play -> Set Dynamic Bones Wind ( Target = self, Wind = Make Vector(300, 0, 0) )
Event Key R -> Reset Dynamic Bones ( Target = self )
```
