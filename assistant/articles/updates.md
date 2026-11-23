id: updates
kind: howto
title: Atualizar a Real Engine | Update the Real Engine
keys: compilando os shaders, compiling the shaders, compilando shaders, travou ao abrir, freeze on start, nao responde, not responding, congelou, primeira vez, first time, erro 504, error 504, gateway timeout, falha ao atualizar, update failed, atualizar, atualizacao, versao nova, update, upgrade, nova versao, baixar versao, instalar atualizacao, beta, release
--- pt
```diagram steps
Ao abrir: procura versão nova | Aparece o aviso | Baixar e Instalar | Ao fechar: copia e reabre
```
- A engine procura uma versão nova sozinha ao abrir (sem travar). Achou, mostra o aviso com **Baixar e Instalar**.
- À mão: **Ajuda > Procurar Atualizações...** [Mostrar onde](show:Help>Check for Updates...)
- O pacote é conferido pela assinatura antes de instalar; ao fechar o editor, o atualizador copia os arquivos (guardando os antigos até dar certo) e abre a versão nova.
- Seus projetos não são tocados: ficam onde estão e abrem na versão nova.
- **Primeira abertura depois de atualizar**: os shaders mudaram, então a placa de vídeo compila tudo de novo uma vez. A tela de carregamento mostra **Compilando os shaders: n de N**; a compilação usa todos os núcleos e começa já na intro e enquanto o Project Browser está aberto, e o resultado fica guardado na hora (a próxima abertura é imediata). O editor não fica mais parado "sem responder".
- **Erro 504** (o GitHub demorou): desde a 0.24 o download tenta outro caminho do GitHub e repete sozinho. Numa versão mais antiga, baixe o pacote na página da release e descompacte por cima.
--- en
```diagram steps
On start: looks for a new version | A notice shows | Download and Install | On close: copies and reopens
```
- The engine looks for a new version by itself on start (without freezing). When there is one, a notice offers **Download and Install**.
- By hand: **Help > Check for Updates...** [Show me where](show:Help>Check for Updates...)
- The package is checked by its signature before installing; when you close the editor the updater copies the files (keeping the old ones until it works) and opens the new version.
- Your projects are not touched: they stay where they are and open in the new version.
- **The first start after an update**: the shaders changed, so the graphics card compiles them all again once. The loading screen shows **Compiling the shaders: n of N**; the compile uses every core and starts during the intro and while the Project Browser is open, and its result is kept at once (the next start is immediate). The editor no longer stands "not responding".
- **Error 504** (GitHub took too long): since 0.24 the download tries GitHub's other path and retries by itself. On an older version, download the package from the release page and unzip it over the old one.
