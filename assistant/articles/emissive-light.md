id: emissive-light
kind: howto
title: Materiais emissivos que iluminam a cena | Emissive materials that light the scene | Material emisivo que ilumina
keys: emissivo, emissive, emissao, emission, brilho, glow, luz emissiva, emissive light, lampada, lamp, neon, tela brilhante, glowing screen, led, rebate na gi, bounces in the gi
--- pt
```diagram steps
Material: Emission Color e Strength | Emissive Light (1) | Cada ator ilumina | A GI rebate
```
## Como funciona
- Um material com **Emission** (Cor da Emissão × Intensidade da Emissão) **ilumina a cena**: cada ator com ele ilumina como uma lâmpada e o brilho **rebate na GI**.
- **Emissive Shadows** liga as sombras por raio dessa luz (o que estiver no caminho a bloqueia; custa cerca de 1 ms).
- **Emissive Light** (Detalhes do material > Material) diz quanto o brilho ilumina: 1 normal, 2 o dobro, **0 brilha mas não ilumina**.
- O painel mostra a **emissão média em nits** depois do bake.
- Brilhos fracos demais (um LED pequeno) entram só pela GI. Os reflexos sempre mostram o brilho.

[Abrir o Content Browser](open:content)
--- en
```diagram steps
Material: Emission Color and Strength | Emissive Light (1) | Each actor lights | The GI bounces it
```
## How it works
- A material with **Emission** (Emission Color × Emission Strength) **lights the scene**: each actor with it lights like a lamp and the glow **bounces in the GI**.
- **Emissive Shadows** turns on that light's ray traced shadows (what stands in the way blocks it; about 1 ms).
- **Emissive Light** (the material's Details > Material) is how much the glow lights: 1 normal, 2 twice, **0 glows but lights nothing**.
- The panel shows the **mean emission in nits** after the bake.
- A glow too faint (a small LED) goes in through the GI alone. Reflections always show the glow.

[Open the Content Browser](open:content)
--- es
```diagram steps
Material: Emission Color y Strength | Emissive Light (1) | Cada actor ilumina | La GI lo hace rebotar
```
## Cómo funciona
- Un material con **Emission** (Emission Color × Emission Strength) **ilumina la escena**: cada actor que lo tiene ilumina como una lámpara y el brillo **rebota en la GI**.
- **Emissive Shadows** activa las sombras por trazado de rayos de esa luz (lo que se interpone la bloquea; cerca de 1 ms).
- **Emissive Light** (Detalles del material > Material) es cuánto ilumina el brillo: 1 normal, 2 el doble, **0 brilla pero no ilumina nada**.
- El panel muestra la **emisión media en nits** después del bake.
- Un brillo demasiado tenue (un LED pequeño) entra solo por la GI. Los reflejos siempre muestran el brillo.

[Abrir el Explorador de contenidos](open:content)
