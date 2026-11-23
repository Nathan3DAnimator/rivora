# Idiomas (i18n.c, i18n_table.c)

- O mesmo esquema do Tlender (`TR()`, `TR_ID()` para IDs estáveis do ImGui), com o **msgid em inglês** (os nomes da
  Unreal) e a tabela em português do Brasil. Edit > Editor Preferences > Language troca na hora.
- Padrão: o idioma do Windows (português → pt-BR, senão inglês); a escolha fica em `%APPDATA%\RealEngine\editor.ini`
  (junto com VSync, escalabilidade e Stats).
- Toda string nova da UI vai em `TR("...")` e ganha uma linha na tabela. Teste `i18n`: nenhum msgid repetido, toda
  entrada traduzida, sufixo `##id` mantido. Conferência das mensagens do código contra a tabela:
  `perl -ne 'while (/TR(?:_ID)?\("([^"]*)"\)/g) { print "$1\n" }' src/*.c | sort -u` contra os msgids da tabela.
