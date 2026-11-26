id: live-parameters
kind: recipe
title: Parâmetros vivos e Emission Intensity | Live parameters and Emission Intensity | Parámetros vivos y Emission Intensity
keys: cor ao carregar, objetos brancos, branco ao carregar, white while loading, loading colors, mips progressivos, progressive textures, parametro vivo, parâmetro vivo, parametros vivos, live parameter, live parameters, sem bake, without a bake, no bake, emission intensity, intensidade da emissao, intensidade da emissão, intensidade, multiplicador de emissao, emissive multiplier, brilho, glow, piscar, flicker, lampada piscando, flickering bulb, set scalar parameter value, set vector parameter value, material instance, instancia de material, tint, cor por parametro, dobra afim, affine fold, material leve, shading leve, lighter shading, mesh_x, re_ext
--- pt
Um **parâmetro** do material (um nó **Value** ou **RGB** com **Parameter Name**) que só **multiplica, soma, subtrai, divide** ou entra num **Mix com fator fixo** antes de chegar à Cor Base, Metálico, Rugosidade ou Emissão é **vivo**: a Material Instance, o **Set Scalar / Vector Parameter Value on Materials** do Grafo e o Material Editor mudam **só um número no buffer**, sem bake (antes: 4 a 23 ms por mudança). A engine assa a parte que não depende do parâmetro (a textura, o ruído) e aplica `valor = assado × a + b` por pixel.
- Exemplos vivos: Textura × **Tint**, Ruído × **Rough** na Rugosidade, Cor × **Glow** na Força da Emissão, Mix (Fator fixo) entre a textura e uma cor-parâmetro.
- Continuam assando (como antes): parâmetro na **Escala** de um ruído, na UV, no fator de uma **Color Ramp**, num Mix cujo fator é uma textura, na Normal ou no Alpha.
- **Emission Intensity** (Detalhes do material > Material): multiplica a emissão na hora, sem bake. Todo material que brilha tem esse parâmetro: a Material Instance muda, e o Grafo usa **Set Scalar Parameter Value on Materials** com o nome `Emission Intensity`. A luz emissiva (Emissive Light) e a GI acompanham.
- Material Instance só com valores vivos **usa as texturas do pai** (nada de bake nem de memória nova).
- **Ao carregar**, cada malha já aparece com a cor do material (Base Color, metálico, rugosidade e emissão constantes do grafo), antes de qualquer bake; as texturas chegam pequenas (64²) e sobem até a resolução cheia em poucos quadros.
- Malha com material de lóbulo extra (Coat, Anisotropia, Subsuperfície, Toon, Wet...) desenha com as pipelines **RE_EXT**; as outras malhas continuam nas leves (antes um material desses trocava todas). Chuva, Light Function, Force Toon e decals ainda trocam todas.

```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Emission Intensity
       Target = self, Parameter Value = Random Float in Range( 0.2, 1.5 )   (lâmpada piscando)
```
```diagram bars ms
Set Parameter antes (bake 4096): 23.1
Set Parameter agora (só o buffer): 0.01
```
- Veja também: [Materiais emissivos que iluminam](article:emissive-light), [Materiais](article:materials), [Mudar a cor](article:change-color).
--- en
A material **parameter** (a **Value** or **RGB** node with a **Parameter Name**) that only **multiplies, adds, subtracts, divides** or goes into a **Mix with a fixed factor** before the Base Color, Metallic, Roughness or Emission is **live**: the Material Instance, the Graph's **Set Scalar / Vector Parameter Value on Materials** and the Material Editor change **only a number in a buffer**, no bake (before: 4 to 23 ms a change). The engine bakes what doesn't depend on the parameter (the texture, the noise) and applies `value = baked × a + b` per pixel.
- Live examples: Texture × **Tint**, Noise × **Rough** into the Roughness, Color × **Glow** into the Emission Strength, a Mix (fixed Factor) between the texture and a color parameter.
- Still baking (as before): a parameter into a noise's **Scale**, the UV, a **Color Ramp**'s factor, a Mix whose factor is a texture, the Normal or the Alpha.
- **Emission Intensity** (the material's Details > Material): multiplies the emission at run time, no bake. Every glowing material has this parameter: a Material Instance changes it, and a Graph uses **Set Scalar Parameter Value on Materials** with the name `Emission Intensity`. The emissive light (Emissive Light) and the GI follow.
- A Material Instance with only live values **uses its parent's textures** (no bake, no new memory).
- **While loading**, each mesh shows its material's color at once (the graph's constant Base Color, metallic, roughness and emission), before any bake; the textures come small (64 square) and climb to the full size in a few frames.
- A mesh with an extra-lobe material (Coat, Anisotropy, Subsurface, Toon, Wet...) draws with the **RE_EXT** pipelines; the other meshes keep the light ones (before, one such material switched all of them). Rain, Light Functions, Force Toon and decals still switch all of them.

```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Emission Intensity
       Target = self, Parameter Value = Random Float in Range( 0.2, 1.5 )   (a flickering bulb)
```
```diagram bars ms
Set Parameter before (a 4096 bake): 23.1
Set Parameter now (the buffer only): 0.01
```
- See also: [Emissive materials that light](article:emissive-light), [Materials](article:materials), [Change a color](article:change-color).
--- es
Un **parámetro** de material (un nodo **Value** o **RGB** con un **Parameter Name**) que solo **multiplica, suma, resta, divide** o entra en un **Mix con un factor fijo** antes del Base Color, Metallic, Roughness o Emission es **vivo**: la Material Instance, el **Set Scalar / Vector Parameter Value on Materials** del Grafo y el Material Editor cambian **solo un número en un búfer**, sin horneado (antes: de 4 a 23 ms por cambio). El motor hornea lo que no depende del parámetro (la textura, el ruido) y aplica `value = baked × a + b` por píxel.
- Ejemplos vivos: Texture × **Tint**, Noise × **Rough** hacia el Roughness, Color × **Glow** hacia la Emission Strength, un Mix (Factor fijo) entre la textura y un parámetro de color.
- Siguen horneando (como antes): un parámetro en la **Scale** de un ruido, el UV, el factor de un **Color Ramp**, un Mix cuyo factor es una textura, la Normal o el Alpha.
- **Emission Intensity** (en Detalles del material > Material): multiplica la emisión en tiempo de ejecución, sin horneado. Todo material que brilla tiene este parámetro: una Material Instance lo cambia, y un Grafo usa **Set Scalar Parameter Value on Materials** con el nombre `Emission Intensity`. La luz emisiva (Emissive Light) y la GI lo siguen.
- Una Material Instance con solo valores vivos **usa las texturas de su padre** (sin horneado, sin memoria nueva).
- **Mientras carga**, cada malla muestra al instante el color de su material (el Base Color, metálico, rugosidad y emisión constantes del grafo), antes de cualquier horneado; las texturas llegan pequeñas (64 de lado) y suben al tamaño completo en pocos fotogramas.
- Una malla con un material de lóbulo extra (Coat, Anisotropy, Subsurface, Toon, Wet...) se dibuja con los pipelines **RE_EXT**; las demás mallas conservan los ligeros (antes, un material así cambiaba todos). La lluvia, las Light Functions, Force Toon y los decals siguen cambiándolos todos.

```blueprint
Event Tick
  -> Set Scalar Parameter Value on Materials
       Emission Intensity
       Target = self, Parameter Value = Random Float in Range( 0.2, 1.5 )   (una bombilla parpadeante)
```
```diagram bars ms
Set Parameter antes (un horneado de 4096): 23.1
Set Parameter ahora (solo el búfer): 0.01
```
- Mira también: [Materiales emisivos que iluminan](article:emissive-light), [Materiales](article:materials), [Cambiar un color](article:change-color).
