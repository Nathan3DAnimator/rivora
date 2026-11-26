id: game-localization
kind: howto
title: Traduzir o jogo (Localization Dashboard) | Translate the game (Localization Dashboard) | Traducir el juego (Panel de localización)
keys: localizacao, localização, localization, traduzir o jogo, translate the game, traducao automatica, tradução automática, automatic translation, idiomas do jogo, game languages, cultura, culture, set current culture, get current culture, localization dashboard, painel de localizacao, painel de localização, gather text, coletar textos, po, csv, gettext, deepl, google translate, libretranslate, pseudo localizacao, pseudo-localização, pseudo localization, ftext, string table, idioma das opcoes, idioma das opções
--- pt
O **Painel de Localização** (Ferramentas > Painel de Localização), como o Localization Dashboard da Unreal, traduz o jogo inteiro: Widgets, textos dos Grafos e scripts, Diálogos, Missões e itens.

```diagram steps
Culturas: nativa e alvos | Coletar Textos | Traduzir Automaticamente ou à mão | Compilar | Set Current Culture / Opções > Idioma
```
## Passo a passo
1. **Culturas**: a **Cultura Nativa** (o idioma em que o jogo foi escrito) e as culturas alvo (**Adicionar Cultura**: en, pt-BR, es, fr...).
2. **Coletar Textos**: acha todo texto do jogo; cada um ganha uma **chave estável** (o asset e o lugar nele), como o FText da Unreal.
3. **Traduzir**: na tabela (busca, filtro por status: Faltando, Automático, Traduzido, Revisado; a contagem por cultura) ou **Traduzir Automaticamente** com um **Provedor**:
   - **Pseudo-localização** (offline): o texto acentuado e entre colchetes, para ver o que ainda não está localizado e o que transborda.
   - **LibreTranslate** (seu servidor), **DeepL** ou **Google** (sua chave). Vem **desligado**; nada é enviado até você escolher o provedor e clicar. A chave fica nas suas configurações do editor, nunca no projeto.
   - Um texto **Revisado** nunca é trocado pela tradução automática.
4. **Exportar / Importar .po** (por cultura, para um tradutor) e **.csv** (todas as culturas numa planilha).
5. **Compilar**: um arquivo por cultura em Content/Localization/Game (vai junto no jogo empacotado).

## No jogo
- **Set Current Culture** ("en", "pt-BR"...) troca os textos **na hora**, sem reiniciar: os Widgets, a fala do diálogo, as missões, os itens e os Text dos Grafos. **Save to Config** guarda no Game User Settings.
- **Get Current Culture** e **Get Localized Cultures** (a lista para um menu).
- O **Idioma** do WBP_Options mostra as culturas do projeto quando ele tem tradução.
- No editor: **Cultura da Prévia** no painel mostra o jogo naquela cultura (no Play também); ao parar o Play volta à nativa.

```diagram bars ms
Trocar de cultura (20 000 textos, a 1ª vez): 7.0
Trocar de volta (a tabela já em memória): 0.0001
```
--- en
The **Localization Dashboard** (Tools > Localization Dashboard), like Unreal's, translates the whole game: Widgets, Graph and script texts, Dialogues, Quests and items.

```diagram steps
Cultures: native and targets | Gather Text | Translate Automatically or by hand | Compile | Set Current Culture / Options > Language
```
## Step by step
1. **Cultures**: the **Native Culture** (the language the game is written in) and the target cultures (**Add Culture**: en, pt-BR, es, fr...).
2. **Gather Text**: finds every text of the game; each gets a **stable key** (its asset and its place in it), like Unreal's FText.
3. **Translate**: in the table (a search, a filter by status: Missing, Automatic, Translated, Reviewed; the count per culture) or **Translate Automatically** with a **Provider**:
   - **Pseudo-localization** (offline): the text accented and bracketed, to see what isn't localized yet and what overflows.
   - **LibreTranslate** (your server), **DeepL** or **Google** (your key). It comes **off**; nothing is sent until you pick the provider and click. The key stays in your editor settings, never in the project.
   - A **Reviewed** text is never replaced by the automatic translation.
4. **Export / Import .po** (per culture, for a translator) and **.csv** (every culture in a spreadsheet).
5. **Compile**: one file per culture in Content/Localization/Game (packaged with the game).

## In the game
- **Set Current Culture** ("en", "pt-BR"...) switches the texts **at once**, without a restart: the Widgets, the dialogue's line, the quests, the items and the Graphs' Text. **Save to Config** keeps it in the Game User Settings.
- **Get Current Culture** and **Get Localized Cultures** (the list for a menu).
- WBP_Options' **Language** lists the project's cultures when it is translated.
- In the editor: the dashboard's **Preview Culture** shows the game in that culture (in Play too); stopping Play goes back to the native.

```diagram bars ms
Switching culture (20,000 texts, the 1st time): 7.0
Switching back (the table kept in memory): 0.0001
```
--- es
El **Localization Dashboard** (Herramientas > Localization Dashboard), como el de Unreal, traduce todo el juego: Widgets, textos de Grafos y scripts, Diálogos, Misiones e ítems.

```diagram steps
Culturas: nativa y de destino | Gather Text | Traducir automáticamente o a mano | Compilar | Set Current Culture / Opciones > Idioma
```
## Paso a paso
1. **Cultures**: la **Native Culture** (el idioma en que está escrito el juego) y las culturas de destino (**Add Culture**: en, pt-BR, es, fr...).
2. **Gather Text**: encuentra todos los textos del juego; cada uno recibe una **clave estable** (su asset y su lugar en él), como el FText de Unreal.
3. **Translate**: en la tabla (una búsqueda, un filtro por estado: Missing, Automatic, Translated, Reviewed; el conteo por cultura) o **Translate Automatically** con un **Provider**:
   - **Pseudo-localización** (sin conexión): el texto con acentos y entre corchetes, para ver lo que aún no está localizado y lo que se desborda.
   - **LibreTranslate** (tu servidor), **DeepL** o **Google** (tu clave). Viene **desactivado**; no se envía nada hasta que elijas el proveedor y hagas clic. La clave queda en los ajustes de tu editor, nunca en el proyecto.
   - Un texto **Reviewed** nunca se reemplaza por la traducción automática.
4. **Export / Import .po** (por cultura, para un traductor) y **.csv** (todas las culturas en una hoja de cálculo).
5. **Compile**: un archivo por cultura en Content/Localization/Game (empaquetado con el juego).

## En el juego
- **Set Current Culture** ("en", "pt-BR"...) cambia los textos **al instante**, sin reiniciar: los Widgets, la línea del diálogo, las misiones, los ítems y el Text de los Grafos. **Save to Config** lo guarda en los Game User Settings.
- **Get Current Culture** y **Get Localized Cultures** (la lista para un menú).
- El **Language** de WBP_Options lista las culturas del proyecto cuando está traducido.
- En el editor: la **Preview Culture** del panel muestra el juego en esa cultura (también en Play); al detener Play vuelve a la nativa.

```diagram bars ms
Cambiar de cultura (20,000 textos, la 1.ª vez): 7.0
Volver (la tabla guardada en memoria): 0.0001
```
