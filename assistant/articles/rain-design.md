id: rain-design
kind: recipe
title: Chuva: predefinições, visual, som, trovões e Blueprints | Rain: presets, look, sound, thunder and Blueprints
keys: chuva, rain, predefinicao da chuva, rain preset, garoa, drizzle, tempestade, storm, moncao, monsoon, chuva estilizada, stylized rain, cor da chuva, cor das gotas, drop color, comprimento do risco, streak length, espessura da gota, drop width, rajadas, gusts, vento variando, chuva ao longe, far rain, chuva some de cima, chuva vista do alto, camera alta, high camera, tamanho dos respingos, splash size, som da chuva, rain sound, estilo do som, sound style, telhado de zinco, tin roof, folhas, leaves, lona, tent, som abafado, muffle, abafar sob telhado, som proprio, custom sound, trovao, trovoes, thunder, relampago, relampagos, lightning, clarao, personagem molhado, actor wetness, molhado do ator, secar devagar, dry time, molhar devagar, wet time, set rain property, get rain property, set rain preset, set rain drop color, get actor wetness, set actor wetness, sound graph, grafo de som
--- pt
O ator **Rain** tem, além da água no chão, tudo o que muda o **visual** e o **som** da chuva — e tudo pode ser mudado por Blueprint.
- **Predefinição** (no topo dos Detalhes): Garoa, Chuva Fraca, Moderada, Forte, Tempestade, Monção e Estilizada mudam o visual e o som juntos. Mexer num campo depois a torna Personalizada.
- **Visual**: **Cor das Gotas** (branco é água clara; acima de 1 brilha), **Comprimento do Risco**, **Espessura da Gota**, **Tamanho dos Respingos** e **Rajadas** (o vento vai e vem e as gotas balançam).
- **Chuva ao Longe**: com a câmera bem alta, metade das gotas e dos respingos vai para o chão para onde ela olha, então a chuva nunca some do chão, a qualquer altura.
- **Atores molhados**: Characters, Skeletal Meshes, veículos e corpos com física têm o próprio molhado. Encharcam aos poucos na chuva (**Tempo para Molhar Atores**, padrão 60 s) e secam devagar sob um telhado ou depois da chuva (**Tempo para Secar Atores**, padrão 8 min). Quando a chuva para, o chão também seca no **Tempo para Secar** dele, em vez de secar na hora.
- **Som**: **Volume do Som**, **Estilo do Som** (Chão Aberto, Telhado, Telhado de Zinco, Folhas, Lona, Distante), **Abafar Sob Telhado** (a chuva soa abafada com um telhado sobre a câmera) e **Som Próprio** (um Sound Wave, Sound Cue ou Sound Graph do projeto no lugar do da engine).
- **Trovões e Relâmpagos** (no Play): **Trovões** é a frequência (1: cerca de a cada 12 s), **Volume dos Trovões**, e **Relâmpagos** é o clarão no céu. O estrondo chega depois do clarão, pela distância.
- **Blueprint**: **Set Rain Property** e **Get Rain Property** mudam ou leem qualquer campo pelo nome dos Detalhes (`Wind Speed`, `Gusts`, `Sound Style`, `Thunder`...); **Set Rain Preset**, **Set Rain Drop Color**, **Get Actor Wetness** e **Set Actor Wetness** (um personagem que sai de um lago encharcado).

```diagram steps
Adicionar Rápido > Chuva | Detalhes > Predefinição: Tempestade | Estilo do Som, Trovões, Relâmpagos | Play | Set Rain Property muda qualquer campo
```
```blueprint
Event Begin Play -> Set Rain Preset ( Preset = "Storm" )
Event Key T -> Set Rain Property ( Property = "Thunder", Value = 1 )
Event Key Y -> Set Rain Property ( Property = "Sound Style", Value = 2 )
Event Key U -> Set Actor Wetness ( Wetness = 1 )
```
--- en
The **Rain** actor has, besides the water on the ground, everything that changes how the rain **looks** and **sounds** — and all of it can be changed from a Blueprint.
- **Preset** (top of the Details): Drizzle, Light Rain, Moderate, Heavy, Storm, Monsoon and Stylized set the look and the sound together. Changing a field afterwards makes it Custom.
- **Look**: **Drop Color** (white is clear water; above 1 it glows), **Streak Length**, **Drop Width**, **Splash Size** and **Gusts** (the wind comes and goes and the drops sway).
- **Far Rain**: with the camera high up, half the drops and splashes go to the ground where it looks, so the rain never vanishes from the ground at any height.
- **Wet actors**: Characters, Skeletal Meshes, vehicles and physics bodies keep their own wetness. They soak gradually in the rain (**Actor Wet Time**, 60 s by default) and dry slowly under a roof or after the rain (**Actor Dry Time**, 8 min by default). When the rain stops the ground dries over its **Dry Time** too, not at once.
- **Sound**: **Sound Volume**, **Sound Style** (Open Ground, Roof, Tin Roof, Leaves, Tent, Distant), **Muffle Under Roof** (muffled with a roof over the camera) and **Custom Sound** (a project's Sound Wave, Sound Cue or Sound Graph instead of the engine's).
- **Thunder and Lightning** (in Play): **Thunder** is how often (1: about every 12 s), **Thunder Volume**, and **Lightning** is the flash in the sky. The rumble arrives after the flash, by its distance.
- **Blueprint**: **Set Rain Property** and **Get Rain Property** change or read any field by its Details name (`Wind Speed`, `Gusts`, `Sound Style`, `Thunder`...); **Set Rain Preset**, **Set Rain Drop Color**, **Get Actor Wetness** and **Set Actor Wetness** (a character soaked coming out of a lake).

```diagram steps
Quick Add > Rain | Details > Preset: Storm | Sound Style, Thunder, Lightning | Play | Set Rain Property changes any field
```
```blueprint
Event Begin Play -> Set Rain Preset ( Preset = "Storm" )
Event Key T -> Set Rain Property ( Property = "Thunder", Value = 1 )
Event Key Y -> Set Rain Property ( Property = "Sound Style", Value = 2 )
Event Key U -> Set Actor Wetness ( Wetness = 1 )
```
