id: city-night-materials
kind: recipe
title: Materiais da cidade à noite: retrorrefletor e flocos metálicos | City night materials: retroreflector and metallic flakes
keys: retrorrefletor, retroreflector, retrorreflexao, retroreflection, placa refletiva, reflective sign, placa de transito, road sign, tachao, cat's eye, olho de gato, colete refletivo, hi-vis vest, refletor de bicicleta, bike reflector, brilha no farol, shines in the headlights, pintura metalica, metallic paint, flocos metalicos, metallic flakes, glitter, purpurina, brilho do carro, car paint sparkle, cidade a noite, city at night, flake size, flake spread
--- pt
Dois ajustes no material (Details do editor de material, qualquer Shading Model menos Eye e Toon) para a cidade à noite, como no GTA 6:
- **Retroreflection** (0 a 16) e **Retroreflection Color**: a superfície devolve a luz para onde ela veio. Com um farol ao lado da câmera, uma placa, um tachão na pista, as faixas de um colete ou um refletor de bicicleta brilham muito mais que a tinta comum. Visto de lado da luz, voltam ao normal.
- **Metallic Flakes** (0 a 4), **Flake Size** (cm) e **Flake Spread**: flocos de metal sob o verniz, cada um inclinado de um jeito, que brilham um a um em volta do reflexo: a pintura metálica de carro. De longe os flocos se juntam num brilho suave, sem cintilar.
- **Exemplos prontos**: `Examples/M_RoadSign` (placa branca), `Examples/M_Reflector` (refletor vermelho) e `Examples/M_CarPaintFlakes` (azul metálico com flocos).
- **Custo**: o retrorrefletor é um termo a mais por luz, só nos materiais que o usam. Os flocos custam dois hashes por luz nos pixels do material. A cena `city_night` roda a mais de 500 fps.

```diagram steps
Editor de Material | Retroreflection 8 (placa) ou Metallic Flakes 1,5 (carro) | farol ao lado da câmera | a placa devolve a luz, a tinta brilha em pontos
```
--- en
Two material settings (the material editor's Details, any Shading Model but Eye and Toon) for the city at night, as in GTA 6:
- **Retroreflection** (0 to 16) and **Retroreflection Color**: the surface sends the light back toward where it came from. With a headlight beside the camera, a road sign, a cat's eye on the road, a vest's stripes or a bike reflector shine far brighter than plain paint. Seen from away from the light, they look normal.
- **Metallic Flakes** (0 to 4), **Flake Size** (cm) and **Flake Spread**: metal flakes under the coat, each tilted its own way, glinting one by one around the highlight: a car's metallic paint. Far away the flakes merge into a soft shine, without sparkling noise.
- **Ready examples**: `Examples/M_RoadSign` (white sign), `Examples/M_Reflector` (red reflector) and `Examples/M_CarPaintFlakes` (metallic blue with flakes).
- **Cost**: the retroreflector is one more term per light, only on the materials that use it. The flakes cost two hashes per light on the material's pixels. The `city_night` scene runs at over 500 fps.

```diagram steps
Material Editor | Retroreflection 8 (sign) or Metallic Flakes 1.5 (car) | a headlight beside the camera | the sign sends the light back, the paint sparkles
```
