id: transmission
kind: howto
title: Transmissão: vidro, água, folhas e cortinas (Transmission) | Transmission: glass, water, leaves and curtains
keys: transmissao, transmission, transmision, transmisión, vidro, glass, cristal, vidrio, agua, water, folha, leaf, hoja, cortina, curtain, papel, paper, abajur, lampshade, pantalla, orelha, ear, oreja, luz por tras, backlight, retroiluminacion, contraluz, fino, thin, delgado, refrativo, refractive, refractivo, absorcao, absorption, absorcion, sombra colorida, colored shadow, sombra de color, vitral, stained glass, garrafa, bottle, botella, ambar, amber, M_AmberGlass, M_Curtain, REALENGINE_NO_GLASS_SHADOW
--- pt
```diagram steps
Principled BSDF: Transmission Weight (ou um Value com Parameter Name)
Details > Transmissão: Modo Refrativo ou Fino
Refrativo: Cor e Distância da Absorção, Sombra Colorida
Fino: luz de trás atravessa na cor do material
```
O **Transmission** é quanta luz atravessa a superfície. No editor de material, em **Details > Transmissão**:
1. **Transmissão** (0..1): o Transmission Weight do Principled BSDF. Para mudar em **Material Instances** e por **Blueprint**, ligue um nó **Value** com **Parameter Name** (ex.: "Transmission") no pino; a GPU lê o valor direto, sem recompilar.
2. **Modo de Transmissão**:
   - **Refrativo** (vidro, água): transparente, entortado pelo IOR e pela normal (Refraction, Refraction Thickness), fosco pela **rugosidade**, colorido pela **Cor Base** e pela **Absorção** (a cor que a luz guarda depois de atravessar a **Distância da Absorção** dentro dele: partes grossas ficam mais fundas). **Sombra Colorida**: o sol que passa pelo vidro pinta o chão com a cor dele (vitral, garrafa); com sombras por raio é exata, sem raio (Baixo, GPU sem RT) cada vidro vira uma caixa testada pelo sol (+0,01 ms).
   - **Fino** (folha, cortina, papel, abajur, orelha): opaco, mas uma luz **atrás** dele aparece pela frente na cor do material. Use Two Sided para folhas e cortinas.
```blueprint
Event BeginPlay
  -> Set Scalar Parameter Value on Materials
       Transmission
       Target = self, Parameter Value = 0.8
```
- Cena de teste `glass`: o painel âmbar (Refrativo, absorção, sombra colorida) e a cortina vermelha (Fino, parâmetro Transmission).
- Custo (RTX 3060, 1440p Épico): a sombra colorida ~0 ms (um raio para o sol só nos quadros com vidro); `REALENGINE_NO_GLASS_SHADOW=1` para comparar.
- Veja também: [Vidro e translúcidos](article:glass-translucent), [Materiais](article:materials).
--- en
```diagram steps
Principled BSDF: Transmission Weight (or a Value with a Parameter Name)
Details > Transmission: Refractive or Thin mode
Refractive: Absorption Color and Distance, Colored Shadow
Thin: a light behind shows through in the material's color
```
**Transmission** is how much light goes through the surface. In the material editor, under **Details > Transmission**:
1. **Transmission** (0..1): the Principled BSDF's Transmission Weight. To change it in **Material Instances** and by **Blueprint**, link a **Value** node with a **Parameter Name** (e.g. "Transmission") to the pin; the GPU reads it directly, no recompile.
2. **Transmission Mode**:
   - **Refractive** (glass, water): see-through, bent by the IOR and the normal (Refraction, Refraction Thickness), frosted by the **roughness**, tinted by the **Base Color** and the **Absorption** (the color the light keeps after crossing the **Absorption Distance** inside it: thick parts get deeper). **Colored Shadow**: the sun through the glass paints the floor in its color (stained glass, a bottle); exact with ray traced shadows, and without rays (Low, a GPU without RT) each glass becomes a box tested toward the sun (+0.01 ms).
   - **Thin** (leaf, curtain, paper, lampshade, ear): opaque, but a light **behind** it shows through on the front in the material's color. Use Two Sided for leaves and curtains.
```blueprint
Event BeginPlay
  -> Set Scalar Parameter Value on Materials
       Transmission
       Target = self, Parameter Value = 0.8
```
- Test scene `glass`: the amber pane (Refractive, absorption, colored shadow) and the red curtain (Thin, a Transmission parameter).
- Cost (RTX 3060, 1440p Epic): the colored shadow ~0 ms (one ray toward the sun only on frames with glass); `REALENGINE_NO_GLASS_SHADOW=1` to compare.
- See also: [Glass and translucency](article:glass-translucent), [Materials](article:materials).
--- es
**Transmisión**: cuánta luz atraviesa la superficie (Details > Transmisión en el editor de materiales). **Refractivo** (vidrio, agua): transparente, refractado por el IOR, esmerilado por la rugosidad, teñido por el Color Base y la **Absorción**; **Sombra de color** del sol a través del vidrio (sombras por rayo). **Delgado** (hoja, cortina, papel, pantalla de lámpara, oreja): opaco, una luz detrás se ve por delante en el color del material. Un nodo **Value** con **Parameter Name** en el pin lo cambia en Material Instances y por Blueprint (Set Scalar Parameter Value on Materials).
