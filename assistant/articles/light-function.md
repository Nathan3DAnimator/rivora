id: light-function
kind: howto
title: Função de Luz: projetor, persiana, nuvens, lâmpada piscando (Light Function) | Light Function: projector, blinds, clouds, flickering bulb | Función de luz: proyector, persiana, nubes, bombilla parpadeante (Light Function)
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
```diagram steps
Material: Detalles > Material Domain = Light Function
Emission (o Base Color) = el patrón; un nodo Time lo anima
Luz Spot, Point o Directional: Detalles > Light Function
Directional: Light Function Scale (cm)
```
Una **Light Function** (la de Unreal) es un material que multiplica una luz: la diapositiva de un proyector, la sombra de una persiana, nubes que pasan sobre el sol, una bombilla que parpadea.
1. Crea un material y en **Detalles > Material Domain** elige **Light Function**. El patrón es su **Emission** (o su **Base Color** cuando no emite nada); el blanco deja pasar la luz, el negro la bloquea. Con color, tiñe la luz (una diapositiva de color).
2. Para **animarlo**, usa el nodo **Time** (el bucle se hornea en fotogramas): nubes que se desplazan (la fase de una Wave Texture), un parpadeo (ruido blanco por paso de tiempo).
3. Selecciona la luz **Spot**, **Point** o **Directional** y elige el material en **Detalles > Light Function** (o arrástralo desde el Explorador de contenidos). Spot: el patrón llena el cono. Point: todas las direcciones. Directional: se repite sobre el mundo cada **Light Function Scale** cm.
- Ejemplos listos en Materials/Examples/LightFunctions: **LF_Projector**, **LF_Blinds**, **LF_Clouds**, **LF_Flicker**. Escena de prueba: `lightfn`.
- Coste (RTX 3060, 1440p Épico, la escena `lightfn` con 4 luces con funciones): +0.30 ms (2.84 → 3.14 ms). `REALENGINE_NO_LIGHTFN=1` para comparar.
- También ilumina la **niebla volumétrica** (haces a través de nubes, la diapositiva de un proyector en el aire) y la **GI** (la luz rebotada sigue el patrón). Solo el horneado de luces estáticas no lo registra (se anima).
- Mira también: [Luces](article:lights), [Materiales](article:materials).
