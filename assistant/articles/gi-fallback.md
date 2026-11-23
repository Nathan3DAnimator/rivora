id: gi-fallback
kind: concept
title: GI de fallback: luz indireta sem raios (Baixo, Android, Quest) | GI fallback: indirect light without rays (Low, Android, Quest)
keys: gi de fallback, gi fallback, gi sem raios, gi without rays, gi no celular, gi on mobile, android gi, quest gi, preset baixo gi, low preset gi, ao em volume, ao volume, oclusao do ceu, sky occlusion, luz rebatida do chao, ground bounce, bounce map, embaixo do telhado escuro, dark under the roof, iluminacao indireta barata, cheap indirect light
--- pt
Quando não há lightmap, sondas nem cache de superfície (o preset **Baixo**, Android, Quest, GPUs sem ray tracing), a engine usa uma **GI de fallback** sozinha:
- **AO em volume**: uma grade de 128 × 128 × 32 m em volta da câmera, em células de 2 m, feita das caixas das malhas. Cada célula guarda quanto do céu ela vê (5 raios). Embaixo de um telhado, ao pé de um prédio ou entre muros, a luz do céu cai.
- **Luz rebatida do chão**: o sol bate no chão e volta para cima. O que olha para baixo ou para o lado recebe essa luz, e a parte do céu escondida é preenchida por ela (embaixo de um telheiro não fica preto, fica com a luz quente do chão ensolarado).
- **Automático**: não há nada para ligar. Com sondas ou lightmap, eles têm prioridade.
- **Custo**: ~0,02 ms no passe principal; o volume é refeito numa thread quando a câmera anda 16 m (~6 ms fora do quadro).
- **Qualidade**: na cena de teste, a diferença para o RealGI (com raios) cai de 10,3% para 9,2%.

```diagram steps
sem lightmap, sem sondas | volume de AO em volta da câmera | céu escondido escurece | chão ensolarado rebate a luz | indireta perto da do RealGI
```
--- en
When there's no lightmap, no probes and no surface cache (the **Low** preset, Android, Quest, GPUs without ray tracing), the engine uses a **GI fallback** by itself:
- **AO volume**: a 128 × 128 × 32 m grid around the camera, in 2 m cells, made from the meshes' boxes. Each cell keeps how much of the sky it sees (5 rays). Under a roof, at a building's foot or between walls, the sky light drops.
- **Ground bounce**: the sun hits the ground and comes back up. What faces down or sideways gets that light, and the hidden part of the sky is filled with it (under a carport it isn't black, it gets the sunlit ground's warm light).
- **Automatic**: nothing to turn on. With probes or a lightmap, those come first.
- **Cost**: ~0.02 ms in the main pass; the volume is rebuilt on a thread when the camera moves 16 m (~6 ms off the frame).
- **Quality**: in the test scene, the difference to the RealGI (with rays) drops from 10.3% to 9.2%.

```diagram steps
no lightmap, no probes | AO volume around the camera | the hidden sky darkens | the sunlit ground bounces the light | indirect light close to the RealGI
```
