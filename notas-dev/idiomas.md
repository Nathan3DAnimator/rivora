# Idiomas (i18n.c, i18n_table.c)

- O mesmo esquema do Tlender (`TR()`, `TR_ID()` para IDs estáveis do ImGui), com o **msgid em inglês** (os nomes da
  Unreal) e a tabela em português do Brasil. Edit > Editor Preferences > Language troca na hora.
- Padrão: o idioma do Windows (português → pt-BR, senão inglês); a escolha fica em `%APPDATA%\RealEngine\editor.ini`
  (junto com VSync, escalabilidade e Stats).
- Toda string nova da UI vai em `TR("...")` e ganha uma linha na tabela. Teste `i18n`: nenhum msgid repetido, toda
  entrada traduzida, sufixo `##id` mantido. Conferência das mensagens do código contra a tabela:
  `perl -ne 'while (/TR(?:_ID)?\("([^"]*)"\)/g) { print "$1\n" }' src/*.c | sort -u` contra os msgids da tabela.

## Espanhol e a varredura (2026-11-25, item 12)
- `LANG_ES` (es_ES): a tabela própria `i18n_table_es.c` (`{ msgid, "es" }`, os mesmos msgids da `i18n_table.c`; o pt
  ficou em `I18nEntry.text[0]`); as descrições em `ui_tips_table_es.c` (`ui_tips_set_es` no main.c). O idioma do Windows
  em espanhol escolhe es; diálogos e missões do jogo também (`dlg_language` 2).
- Bug achado: o índice das descrições (`TIP_CAP 4096`, metade usada) só achava as primeiras 2048 de 2452 descrições: agora
  8192 (e `TIP_MAX` 4096); o do i18n foi de 16384 para 32768 (os nomes dos nós entraram).
- Os nós dos Blueprints agora traduzidos na tela: título (`node_title`, os formatos "Set %s", "Call %s"...), pinos,
  dicas, categorias e nomes no menu de ações (a busca acha pelo inglês e pelo idioma da interface; os ids ficam ingleses).
- `tools/i18n_audit.py` (teste `i18n_audit`): (1) literal visível sem TR nas chamadas ig*/ui_*/ImDrawList_AddText/
  editor_status (exceções em `tools/i18n_exceptions.txt`: nomes, unidades, termos da Unreal; `// i18n: none (<motivo>)` na
  linha para dados como nomes de asset de exemplo); (2) toda chave TR e todo texto de tabela (`rivora --i18n-dump`: nós,
  pinos, categorias, componentes, tipos de asset, classes) em pt e es. `tools/i18n_baseline.txt` guarda literais de
  arquivos de outros donos (vazio hoje). `REALENGINE_I18N_MISSING=<arquivo>`: o editor grava ao sair os msgids pedidos sem
  tradução (a parte em tempo de execução da varredura).
- Pendente: o Assistente em espanhol (os artigos têm a seção es, o índice só carrega pt/en); `mat_editor.c` usa
  `L(en, pt)` próprio (espanhol cai no inglês).
- (25/11, bloqueio do tester nos itens 12/13) `--lang` aceita `en`/`pt`/`es` (e `pt-BR`, `PT_br`...) e avisa um código
  desconhecido; aba do editor de Grafo pelo `TR(g_bp_asset_names)`; Opaque/Masked/Translucent traduzidos; "Graph" no es
  é Grafo (Grafo de Eventos em pt e es); o Assistente passa o texto visível (corpo, links, diagramas, menos `layout`)
  pelo `i18n_terms`, as ações (`show:`) ficam em inglês.
