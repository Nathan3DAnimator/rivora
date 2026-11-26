id: icons-tooltips
kind: concept
title: Ícones e dicas dos controles | Icons and control tooltips | Iconos y descripciones de los controles
keys: ícone, ícones, icone, icones, icon, icons, dica, dicas, tooltip, tooltips, descrição, descricao, description, o que faz este botão, what does this button do, para que serve, atalho, atalhos, shortcut, shortcuts, f1, cor do ícone, icon color, família, familia, family, duotom, duotone, tipo de asset, asset type, selo, badge, folha de ícones, icon sheet, iconos, ayuda emergente, información sobre herramientas, atajo, sound graph, grafo de som
--- pt
```diagram steps
Pare o mouse num controle | A dica aparece ao lado: ícone, NOME, atalho | Embaixo: o que ele faz | F1: o Assistente busca aquele controle
```
## A dica de cada controle
Todo botão, item de menu, caixa, campo e aba do editor tem uma dica. Pare o mouse sobre ele por meio segundo e ela aparece **ao lado**, nunca por cima:
- em cima, o **ícone** e o **nome** do controle em destaque, com o **atalho** numa tecla à direita (por exemplo `Ctrl+S`);
- embaixo, **o que ele faz**, em português, inglês ou espanhol (o idioma do editor);
- por fim, **F1: mais no Assistente**. Apertar **F1** com o mouse sobre o controle abre o Assistente já buscando por ele.

Um campo numa linha do painel Details usa a dica da própria linha (o nome da propriedade à esquerda).

## Os ícones
Os ícones são desenhados pela engine (vetoriais, nítidos em qualquer escala). Cada um segue a mesma gramática:
- grade de 16, traço de 1,5 px que acompanha a escala da interface, pontas redondas;
- **duotom**: o traço na cor do texto e o corpo preenchido de leve (cerca de 20%) na **cor da família**.

As famílias de asset têm cores fixas (todas do tema, então mudam com ele):

| Família | Cor | Exemplos |
|---|---|---|
| Grafo | ciano | Grafo, Behavior Tree, script, macro |
| Material | verde | Material, Material Instance |
| Malha | laranja | Static Mesh, Skeletal Mesh, Landscape, veículo |
| Animação | rosa | Animation, Montage, Blend Space, Level Sequence |
| Áudio | violeta | Sound Wave, Sound Cue, Sound Graph |
| Efeitos | âmbar | partículas, luzes, neblina |
| UI | azul | Widget Graph, tema, controles |
| Dados | cinza-azulado | Structure, Enum, Data Asset, Quest, Dialogue |
| Nível | branco | Level, atores, câmera |

As ferramentas usam o acento do tema e os comandos da própria engine (Play, Simular, Compilar, Empacotar, Projeto) usam o laranja **Ignição**, com a órbita como motivo. No Content Browser o tipo do asset aparece num **selo** colorido no canto da miniatura.
--- en
```diagram steps
Rest the mouse on a control | The tooltip shows beside it: icon, NAME, shortcut | Under it: what it does | F1: the Assistant searches that control
```
## Every control's tooltip
Every button, menu item, check box, field and tab of the editor has a tooltip. Rest the mouse on it for half a second and it shows **beside** it, never over it:
- on top, the control's **icon** and **name** in bold, with its **shortcut** as a key cap on the right (for example `Ctrl+S`);
- under them, **what it does**, in English, Portuguese or Spanish (the editor's language);
- last, **F1: more in the Assistant**. Pressing **F1** with the mouse on the control opens the Assistant already searching for it.

A field in a Details row uses the row's own tooltip (the property's name on the left).

## The icons
The icons are drawn by the engine (vector, sharp at any scale). Each follows the same grammar:
- a 16 grid, a 1.5 px stroke that follows the interface's scale, round ends;
- **duotone**: the stroke in the text's color and the body lightly filled (about 20%) in the **family's color**.

The asset families have fixed colors (all theme colors, so they change with it):

| Family | Color | Examples |
|---|---|---|
| Graph | cyan | Graph, Behavior Tree, script, macro |
| Material | green | Material, Material Instance |
| Mesh | orange | Static Mesh, Skeletal Mesh, Landscape, vehicle |
| Animation | pink | Animation, Montage, Blend Space, Level Sequence |
| Audio | violet | Sound Wave, Sound Cue, Sound Graph |
| Effects | amber | particles, lights, fog |
| UI | blue | Widget Graph, theme, controls |
| Data | blue-gray | Structure, Enum, Data Asset, Quest, Dialogue |
| Level | white | Level, actors, camera |

The tools use the theme's accent and the engine's own commands (Play, Simulate, Compile, Package, Project) the **Ignition** orange, with the orbit as their motif. In the Content Browser the asset's kind shows as a colored **badge** in the thumbnail's corner.
--- es
```diagram steps
Deja el ratón sobre un control | La descripción aparece a su lado: icono, NOMBRE, atajo | Debajo: qué hace | F1: el Asistente busca ese control
```
## La descripción de cada control
Cada botón, elemento de menú, casilla, campo y pestaña del editor tiene una descripción emergente. Deja el ratón sobre él medio segundo y aparece **al lado**, nunca encima:
- arriba, el **icono** y el **nombre** del control en negrita, con su **atajo** como una tecla a la derecha (por ejemplo `Ctrl+S`);
- debajo, **qué hace**, en inglés, portugués o español (el idioma del editor);
- al final, **F1: más en el Asistente**. Al pulsar **F1** con el ratón sobre el control se abre el Asistente ya buscándolo.

Un campo de una fila de Detalles usa la descripción de la propia fila (el nombre de la propiedad a la izquierda).

## Los iconos
Los iconos los dibuja el motor (vectoriales, nítidos a cualquier escala). Todos siguen la misma gramática:
- una cuadrícula de 16, un trazo de 1,5 px que sigue la escala de la interfaz, extremos redondeados;
- **duotono**: el trazo en el color del texto y el cuerpo con un relleno ligero (cerca del 20 %) en el **color de la familia**.

Las familias de assets tienen colores fijos (todos son colores del tema, así que cambian con él):

| Familia | Color | Ejemplos |
|---|---|---|
| Grafo | cian | Grafo, Behavior Tree, script, macro |
| Material | verde | Material, Material Instance |
| Malla | naranja | Static Mesh, Skeletal Mesh, Landscape, vehículo |
| Animación | rosa | Animación, Montage, Blend Space, Level Sequence |
| Audio | violeta | Sound Wave, Sound Cue, Sound Graph |
| Efectos | ámbar | partículas, luces, niebla |
| UI | azul | Widget Graph, tema, controles |
| Datos | gris azulado | Structure, Enum, Data Asset, Quest, Dialogue |
| Nivel | blanco | Nivel, actores, cámara |

Las herramientas usan el acento del tema y los comandos propios del motor (Jugar, Simular, Compilar, Empaquetar, Proyecto) el naranja **Ignition**, con la órbita como motivo. En el Explorador de contenidos, el tipo de asset se muestra como una **insignia** de color en la esquina de la miniatura.
