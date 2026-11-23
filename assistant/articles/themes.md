id: themes
kind: howto
title: Temas e Editor de Tema | Themes and the Theme Editor
keys: azul real, novo visual, new look, visual padrão, default look, voltar ao visual anterior, previous look, tema, temas, theme, themes, tema claro, light theme, tema escuro, dark theme, modo escuro, dark mode, cores do editor, editor colors, mudar cor, change colors, editor de tema, theme editor, rtheme, orbita, órbita, grafite, meia-noite, oled, alto contraste, high contrast, daltonico, daltônico, color blind, colorblind, okabe, sepia, sépia, neon, brasa, floresta, fonte, fontes, font, fonts, inter, sora, jetbrains mono, contraste, contrast, aparência, aparencia, appearance, tema oscuro, tema claro, apariencia, colores del editor, editor de temas, daltonismo, fuente, fuentes
--- pt
```diagram steps
Editar > Preferências do Editor > Aparência | Clique num tema: muda no quadro seguinte | Editar uma cópia: cada cor ao vivo | Salvar: vira um tema seu (.rtheme)
```
## Trocar de tema
Abra [Editar > Preferências do Editor > Aparência](open:prefs) e clique num dos cartões. O editor inteiro muda no quadro seguinte (painéis, grafos, ícones, prévias) e a escolha fica salva.

Temas que vêm com a engine:
- **Azul Real** (o padrão desde a 0.44): azul-marinho profundo, um só azul forte para o que está ativo (abas, seleção, botões principais), Play verde, cabeçalhos de Blueprint vermelhos (eventos), azuis (funções) e verdes (puras), pastas laranja.
- **Órbita Escuro** (o padrão até a 0.43): grafite azulado, acento ciano e laranja para o Play.
- **Grafite**: cinzas neutros. **Meia-noite**: preto, para telas OLED e sala escura.
- **Claro** e **Sépia**: temas claros (texto escuro em superfícies claras).
- **Alto Contraste**: fundo preto, texto branco, bordas de 2 px, acento amarelo.
- **Brasa**, **Floresta** e **Neon**: paletas quentes, verdes e sintéticas.
- **Daltônico seguro**: acentos, estados e cores dos pinos na paleta Okabe-Ito (seguros para os três tipos de daltonismo).

## O visual novo (0.44)
Quem usava o padrão antigo (Órbita Escuro) recebe o **Azul Real** e o novo layout de painéis uma vez, com um cartão no topo: **Manter** ou **Voltar ao Meu Visual Anterior** (o tema e o layout de antes; o layout também fica em Janela > Layout como "Antes do Azul Real"). Quem tinha escolhido outro tema só recebe a oferta (**Experimentar** / **Não, Obrigado**). Os 11 temas continuam todos aqui.

## Criar o seu
1. Escolha o tema de partida e clique em **Editar uma cópia**.
2. Mude as cores por grupo (Superfícies, Texto, Acentos, Estados, Pinos de Blueprint, Famílias de nó e de asset...). Cada mudança aparece na hora. **Derivar** devolve uma cor ao valor calculado a partir das outras.
3. Ajuste **Tamanhos e animações**: cantos, espaço entre painéis, margens, espessura da borda, duração das animações e tamanho do texto.
4. Dê um nome e clique em **Salvar**. O tema vai para `%APPDATA%\RealEngine\Themes` como um arquivo `.rtheme`.

Enquanto você edita, a linha **Contraste** avisa se algum texto ou ícone ficou difícil de ler (texto 4,5:1, ícones 3:1, a regra WCAG).

## Compartilhar
**Exportar...** grava o tema num `.rtheme` (texto simples) e **Importar...** traz um de qualquer lugar. **Duplicar** faz uma cópia com outro nome.

## Fontes
A interface usa **Inter**, os títulos **Sora** e o código e os números **JetBrains Mono**, embutidas na engine (licença OFL). Elas não dependem das fontes instaladas no Windows.
--- en
```diagram steps
Edit > Editor Preferences > Appearance | Click a theme: it changes on the next frame | Edit a copy: every color live | Save: it becomes one of yours (.rtheme)
```
## Switching themes
Open [Edit > Editor Preferences > Appearance](open:prefs) and click one of the cards. The whole editor changes on the next frame (panels, graphs, icons, previews) and the choice is saved.

Themes that come with the engine:
- **Azul Real** (the default since 0.44): deep navy, one strong blue for what is active (tabs, selection, main buttons), green Play, red (events), blue (functions) and green (pure) Blueprint headers, orange folders.
- **Órbita Escuro** (the default up to 0.43): bluish graphite, a cyan accent and orange for Play.
- **Grafite**: neutral grays. **Meia-noite**: black, for OLED screens and dark rooms.
- **Claro** and **Sépia**: light themes (dark text on light surfaces).
- **Alto Contraste**: black background, white text, 2 px borders, a yellow accent.
- **Brasa**, **Floresta** and **Neon**: warm, green and synthwave palettes.
- **Daltônico seguro**: accents, states and pin colors from the Okabe-Ito palette (safe for the three kinds of color blindness).

## The new look (0.44)
If you used the old default (Órbita Escuro), you get **Azul Real** and the new panel layout once, with a card at the top: **Keep It** or **Go Back to My Previous Look** (the theme and layout from before; the layout also stays in Window > Layout as "Before Azul Real"). If you had chosen another theme, it is only offered (**Try It** / **No, Thanks**). All 11 themes are still here.

## Making your own
1. Pick the theme to start from and click **Edit a copy**.
2. Change the colors by group (Surfaces, Text, Accents, States, Blueprint pins, Node and asset families...). Every change shows at once. **Derive** puts a color back to the value computed from the others.
3. Adjust **Sizes and animations**: corners, the gap between panels, padding, border width, animation durations and text size.
4. Name it and click **Save**. The theme goes to `%APPDATA%\RealEngine\Themes` as a `.rtheme` file.

While you edit, the **Contrast** line warns when a text or an icon became hard to read (text 4.5:1, icons 3:1, the WCAG rule).

## Sharing
**Export...** writes the theme into a `.rtheme` (plain text) and **Import...** brings one from anywhere. **Duplicate** makes a copy under another name.

## Fonts
The interface uses **Inter**, the titles **Sora** and the code and numbers **JetBrains Mono**, embedded in the engine (OFL license). They don't depend on the fonts installed in Windows.
--- es
```diagram steps
Editar > Preferencias del Editor > Apariencia | Clic en un tema: cambia en el siguiente fotograma | Editar una copia: cada color en vivo | Guardar: se vuelve uno tuyo (.rtheme)
```
## Cambiar de tema
Abre [Editar > Preferencias del Editor > Apariencia](open:prefs) y haz clic en una de las tarjetas. Todo el editor cambia en el siguiente fotograma (paneles, grafos, iconos, vistas previas) y la elección queda guardada.

Temas incluidos: **Azul Real** (el predeterminado desde la 0.44: azul marino, un solo azul de acento, Play verde, cabeceras de Blueprint rojas, azules y verdes), **Órbita Escuro** (el anterior: grafito azulado, acento cian y naranja para Play), **Grafite**, **Meia-noite** (OLED), **Claro**, **Sépia**, **Alto Contraste**, **Brasa**, **Floresta**, **Neon** y **Daltônico seguro** (paleta Okabe-Ito, segura para el daltonismo).

## Crear el tuyo
1. Elige el tema de partida y haz clic en **Editar una copia**.
2. Cambia los colores por grupo; cada cambio aparece al instante. **Derivar** devuelve un color al valor calculado a partir de los demás.
3. Ajusta **Tamaños y animaciones** (esquinas, espacio entre paneles, márgenes, borde, duraciones, tamaño del texto).
4. Ponle un nombre y haz clic en **Guardar** (`%APPDATA%\RealEngine\Themes`, archivo `.rtheme`).

La línea **Contraste** avisa cuando un texto o icono queda difícil de leer (texto 4,5:1, iconos 3:1, WCAG). **Exportar...** e **Importar...** comparten temas como `.rtheme`.

## Fuentes
**Inter** (interfaz), **Sora** (títulos) y **JetBrains Mono** (código y números), incluidas en el motor (licencia OFL).
