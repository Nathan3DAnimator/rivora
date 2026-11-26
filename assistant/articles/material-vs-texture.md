id: material-vs-texture
kind: concept
title: Material e textura: a diferença | Material and texture: the difference | Material y textura: la diferencia
keys: material ou textura, diferenca, textura, imagem, png, jpg, pbr, rugosidade, metalico, normal map, o que e material
--- pt
```diagram compare
 | Textura | Material
É | uma imagem | a aparência da superfície
Tem | pixels | cor, rugosidade, metal, relevo, brilho
Usa texturas | não | sim
Feito em | outro programa | editor de nós
```
- Uma **textura** é uma imagem (PNG, JPG, TGA...).
- Um **material** diz como a superfície parece: cor, rugosidade, metal, relevo, brilho, transparência. Ele pode usar texturas.
- Os materiais são grafos de nós (os do Blender). A engine assa o resultado em texturas comprimidas: rápido no jogo.
- Veja [Materiais](article:materials).
--- en
```diagram compare
 | Texture | Material
Is | an image | the surface's look
Has | pixels | color, roughness, metal, bumps, glow
Uses textures | no | yes
Made in | another program | the node editor
```
- A **texture** is an image (PNG, JPG, TGA...).
- A **material** says how a surface looks: color, roughness, metal, bumps, glow, transparency. It can use textures.
- Materials are node graphs (Blender's nodes). The engine bakes the result into compressed textures: fast in the game.
- See [Materials](article:materials).
--- es
```diagram compare
 | Textura | Material
Es | una imagen | el aspecto de la superficie
Tiene | píxeles | color, rugosidad, metal, relieves, brillo
Usa texturas | no | sí
Se hace en | otro programa | el editor de nodos
```
- Una **textura** es una imagen (PNG, JPG, TGA...).
- Un **material** dice cómo se ve una superficie: color, rugosidad, metal, relieves, brillo, transparencia. Puede usar texturas.
- Los materiales son grafos de nodos (los nodos de Blender). El motor hornea el resultado en texturas comprimidas: rápido en el juego.
- Mira [Materiales](article:materials).
