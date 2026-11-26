id: sky-fog
kind: howto
title: Céu, nuvens e neblina | Sky, clouds and fog | Cielo, nubes y niebla
keys: raios de sol, god rays, feixes de luz, light shafts, local fog volume, neblina local, neblina numa caixa, neblina so dentro, fog volume, mist volume, ceu, nuvem, neblina, por do sol, noite, dia, atmosfera, sky preset, fog, cloud, horario
--- pt
- **Ator > Adicionar > Sky Preset** troca o céu inteiro (dia, entardecer, noite...). [Mostrar onde](show:Actor>Add>Sky Preset)
- **Exponential Height Fog** dá a neblina; **Volumetric Cloud** as nuvens. [Adicionar neblina](add:Fog) [Adicionar nuvens](add:Cloud)
- O horário vem do ângulo da Directional Light (o sol): gire-a com **E**.
- **Raios de sol (feixes) num lugar fechado:** a neblina de altura com **Volumetric** ligado (Médio para cima) e o sol entrando por frestas: os feixes saem das sombras do sol (os mapas de sombra virtuais, várias amostras por froxel: bordas suaves). Densidade 0,01-0,02 e anisotropia 0,7-0,8 para brilharem contra o sol. Exemplo: [a cena do metrô](article:metro-scene).
- **Neblina só num lugar (Local Fog Volume):** **Adicionar > Efeitos Visuais > Local Fog Volume**: neblina numa esfera ou caixa (o tamanho do ator), com Density, Edge Falloff, Height Falloff, cor e brilho próprios. Iluminada com a neblina volumétrica (Médio para cima): os raios de sol e as lâmpadas atravessam. Bom para pântanos, cavernas, uma sala com fumaça.
--- en
- **Actor > Add > Sky Preset** swaps the whole sky (day, sunset, night...). [Show me where](show:Actor>Add>Sky Preset)
- **Exponential Height Fog** gives the fog; **Volumetric Cloud** the clouds. [Add fog](add:Fog) [Add clouds](add:Cloud)
- The time of day comes from the Directional Light's angle (the sun): rotate it with **E**.
- **Sun shafts (god rays) indoors:** the height fog with **Volumetric** on (Medium and up) and the sun coming through gaps: the shafts come from the sun's shadows (its virtual shadow maps, several taps a froxel: soft edges). Density 0.01-0.02 and anisotropy 0.7-0.8 so they glow against the sun. Example: [the metro scene](article:metro-scene).
- **Fog in one place (Local Fog Volume):** **Add > Visual Effects > Local Fog Volume**: fog in a sphere or a box (the actor's size), with Density, Edge Falloff, Height Falloff, its own color and glow. Lit with the volumetric fog (Medium and up): the sun's shafts and the lamps go through it. Good for swamps, caves, a smoky room.
--- es
- **Actor > Añadir > Sky Preset** cambia todo el cielo (día, atardecer, noche...). [Muéstrame dónde](show:Actor>Add>Sky Preset)
- **Exponential Height Fog** da la niebla; **Volumetric Cloud** las nubes. [Añadir niebla](add:Fog) [Añadir nubes](add:Cloud)
- La hora del día sale del ángulo de la Directional Light (el sol): gírala con **E**.
- **Haces de sol (god rays) en interiores:** la niebla de altura con **Volumetric** activado (Medio y superior) y el sol entrando por las rendijas: los haces salen de las sombras del sol (sus mapas de sombra virtuales, varias muestras por froxel: bordes suaves). Densidad 0,01-0,02 y anisotropía 0,7-0,8 para que brillen frente al sol. Ejemplo: [la escena del metro](article:metro-scene).
- **Niebla en un lugar (Local Fog Volume):** **Añadir > Visual Effects > Local Fog Volume**: niebla en una esfera o una caja (el tamaño del actor), con Density, Edge Falloff, Height Falloff, su propio color y brillo. Se ilumina con la niebla volumétrica (Medio y superior): los haces del sol y las lámparas la atraviesan. Bueno para pantanos, cuevas, una habitación con humo.
