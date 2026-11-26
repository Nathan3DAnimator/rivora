# Localização do jogo (2026-11-24, item 15)

Pedido do usuário: "um sistema de tradução automática que traduz o jogo inteiro, igual ao sistema de localização da
Unreal, onde dá para traduzir Widgets, textos e conversas". É separado do i18n do editor (`TR()`, `i18n.h`): aquele traduz
a interface da engine; este, o texto do jogo de cada projeto, em quantas culturas o projeto quiser.

## Arquivos
| Arquivo | O quê |
|---|---|
| `src/loc.h`, `src/loc.c` | A tabela (`LocProject`: nativa, alvos, entradas com chave e tradução por cultura e status), `Game.rloc` (JSON), `.po` e `.csv`, a compilação (`<cultura>.rlocres`), o tempo de execução (`loc_set_culture`, `loc_text`, `loc_revision`), a pseudo-localização, as culturas e os nomes delas |
| `src/loc_gather.c` | A coleta (Widgets, Blueprints/scripts, Diálogos, Missões, itens), as chaves, a troca dos Text das classes (`loc_bp_apply`), `loc_switch`, `loc_widget_text`, `loc_item` |
| `src/loc_auto.c` | A tradução automática: Pseudo-localização (offline), LibreTranslate, DeepL, Google por HTTP num worker (`async.h`), a mescla por chave; as preferências (`editor.ini`) |
| `src/loc_ui.c` | Ferramentas > Painel de Localização (o Localization Dashboard da Unreal) |
| `src/loc_test.inc` | `--loc-test <pasta>` (teste `loc`, sem janela) |

## Chaves (namespace + key, como o FText)
- Widget: `W:<classe>` + `<elemento>.Text` (só o texto como a classe escreveu: um Set Text em jogo não é trocado).
- Blueprint/script: `BP:<classe>` + `T_<hash do texto>` (os pinos Text com literal e os padrões de variável Text). Em
  jogo, a tabela de strings da classe (`BpAsset.strings`) é trocada pela da cultura (os originais guardados por classe;
  compilada de novo, a cópia é refeita).
- Diálogo: `DLG:<asset>` + `n<índice do nó>` (falas e escolhas). Missão: `Q:<asset>` + `title`, `desc`, `stage<i>`,
  `obj<i>`. Item: `ITEM:<asset>` + `name`, `desc`.
- Uma fonte que mudou: as traduções voltam a Automático (a revisar); uma entrada não achada na coleta fica `stale`
  (fora da contagem, do export e da compilação).

## Tempo de execução
- `loc_text(ns, key, fonte)`: busca binária pelo hash de 64 bits na tabela compilada da cultura; sem tabela (a nativa),
  a fonte, sem custo. Os ganchos: `ui_widget_draw.c` (o texto de cada widget, por quadro), `dlg_node_text`, os getters
  de missão, `BPK_INV_ITEM_INFO` e a dica do inventário.
- Troca ao vivo: `loc_switch(cultura)` (tabela + `loc_bp_apply`); os widgets são desenhados com o texto novo no quadro
  seguinte; o diálogo reescreve as falas quando `loc_revision()` muda. As tabelas lidas ficam em memória (voltar a uma
  cultura é trocar ponteiros); `loc_compile` as descarta.
- Nós: **Set Current Culture** (Culture, Save to Config: grava `Culture` no Game User Settings), **Get Current
  Culture**, **Get Localized Cultures**. Game User Settings: `Culture=<código>` no `GameUserSettings.ini`; com
  culturas no projeto, o Idioma do WBP_Options lista elas (e pt/en também mudam o idioma da interface da engine).
- Jogo empacotado: `.rloc` e `.rlocres` entram no pacote (`pack_entry`); o `gus_apply` da partida troca para a cultura
  salva. No editor, parar o Play volta à nativa.

## Tradução automática
- Não há tradutor offline embutido: o Bergamot/Firefox Translations é MPL-2.0 (código e modelos, ~17 MB por par) e o
  CTranslate2 (MIT) usa modelos OPUS-MT CC-BY 4.0 de ~75-300 MB por par; o contrato pedia MIT/Apache e leve. Fica como
  opção futura com a decisão do usuário.
- Provedores: Desligado (padrão), Pseudo-localização (offline: vogais acentuadas e colchetes, os `{argumentos}` intactos),
  LibreTranslate (URL do servidor; chave se ele pedir), DeepL (chave; `:fx` usa a API Free), Google (chave). Nada sai
  sem o usuário escolher o provedor e clicar em Traduzir Automaticamente; a chave fica no `editor.ini` do usuário.
- Um worker manda um texto por vez (cartão com progresso e Cancelar); a primeira falha para tudo (configuração errada);
  o resultado entra como Automático, nunca por cima de um Revisado.

## Medidas (`loc`, sem janela)
- Tabela de 4 textos: `.po` exportado e relido igual; troca para en em ~1 ms (a primeira leitura do arquivo).
- 20 004 textos: compilar **6,9 ms**; a troca **7,0 ms** na primeira vez (lê o arquivo) e **0,0001 ms** de volta (em
  memória); a tabela **751 KB**; uma busca **0,107 µs**.
- No Play (`blueprints`, bp_test.c): Set Current Culture en troca o texto do WBP_LocTest e a fala do DLG_LocTest na hora e
  volta ao pt-BR.

## Falta
- Sequências não têm texto próprio (as legendas viriam por Widgets: já cobertos). String Tables da Unreal: não há.
- Plural/gênero (ICU) e textos com argumento por idioma: `Format Text` usa o texto traduzido com os mesmos `{0}`.
