id: light-function
kind: howto
title: Função de Luz: projetor, persiana, nuvens, lâmpada piscando (Light Function) | Light Function: projector, blinds, clouds, flickering bulb
keys: funcao de luz, light function, funcion de luz, función de luz, projetor, projector, proyector, slide, gobo, cookie, persiana, blinds, persianas, nuvem passando, clouds passing, nubes, sombra de nuvem, cloud shadow, piscar, flicker, parpadeo, lampada piscando, flickering bulb, bombilla, padrao de luz, light pattern, patron de luz, dominio do material, material domain, dominio del material, LF_Blinds, LF_Clouds, LF_Flicker, LF_Projector, REALENGINE_NO_LIGHTFN
--- pt
```diagram steps
Material: Details > Domínio do Material = Função de Luz
Emissão (ou Cor Base) = o padrão; um nó Time anima
Luz Spot, Point ou Directional: Details > Função de Luz
Directional: Escala da Função de Luz (cm)
```
Uma **Função de Luz** (a Light Function da Unreal) é um material que multiplica a luz: o slide de um projetor, a sombra de uma persiana, nuvens passando no sol, uma lâmpada que pisca.
1. Crie um material e em **Details > Domínio do Material** escolha **Função de Luz**. O padrão é a **Emissão** (ou a **Cor Base**, se não houver emissão); branco deixa a luz passar, preto apaga. Colorido tinge a luz (slide colorido).
2. Para **animar**, use o nó **Time** (o loop é assado em quadros): nuvens andando (fase de uma Wave Texture), piscar (um ruído branco por degrau de tempo).
3. Selecione a luz **Spot**, **Point** ou **Directional** e escolha o material em **Details > Função de Luz** (ou arraste do Content Browser). Spot: o padrão cobre o cone. Point: todas as direções. Directional: repete no mundo a cada **Escala da Função de Luz** cm.
- Exemplos prontos em Materials/Examples/LightFunctions: **LF_Projector**, **LF_Blinds**, **LF_Clouds**, **LF_Flicker**. Cena de teste: `lightfn`.
- Custo (RTX 3060, 1440p Épico, cena `lightfn` com 4 luzes com função): +0,30 ms (2,84 → 3,14 ms). `REALENGINE_NO_LIGHTFN=1` para comparar.
- Entra também na **neblina volumétrica** (feixes de nuvens, o slide do projetor no ar) e na **GI** (a luz rebatida segue o padrão). Só o bake de luz estática não grava a função (ela anima).
- Veja também: [Luzes](article:lights), [Materiais](article:materials).
--- en
```diagram steps
Material: Details > Material Domain = Light Function
Emission (or Base Color) = the pattern; a Time node animates it
Spot, Point or Directional light: Details > Light Function
Directional: Light Function Scale (cm)
```
A **Light Function** (Unreal's) is a material multiplying a light: a projector's slide, a window blind's shadow, clouds passing over the sun, a flickering bulb.
1. Make a material and under **Details > Material Domain** pick **Light Function**. The pattern is its **Emission** (or its **Base Color** when it emits nothing); white lets the light through, black blocks it. Colored tints the light (a colored slide).
2. To **animate** it, use the **Time** node (the loop is baked in frames): clouds drifting (a Wave Texture's phase), a flicker (white noise per time step).
3. Select the **Spot**, **Point** or **Directional** light and pick the material in **Details > Light Function** (or drag it from the Content Browser). Spot: the pattern fills the cone. Point: every direction. Directional: it repeats over the world every **Light Function Scale** cm.
- Ready examples in Materials/Examples/LightFunctions: **LF_Projector**, **LF_Blinds**, **LF_Clouds**, **LF_Flicker**. Test scene: `lightfn`.
- Cost (RTX 3060, 1440p Epic, the `lightfn` scene with 4 lights with functions): +0.30 ms (2.84 → 3.14 ms). `REALENGINE_NO_LIGHTFN=1` to compare.
- It also lights the **volumetric fog** (shafts through clouds, a projector's slide in the air) and the **GI** (the bounced light follows the pattern). Only the static light bake doesn't record it (it animates).
- See also: [Lights](article:lights), [Materials](article:materials).
--- es
Una **Función de Luz** (la Light Function de Unreal) es un material que multiplica una luz: la diapositiva de un proyector, la sombra de una persiana, nubes pasando sobre el sol, una bombilla que parpadea. En el material: **Details > Dominio del Material = Función de Luz** (el patrón es la Emisión, o el Color Base; el nodo **Time** lo anima). En la luz **Spot**, **Point** o **Directional**: **Details > Función de Luz** (Directional: **Escala** en cm). Ejemplos: LF_Projector, LF_Blinds, LF_Clouds, LF_Flicker.
