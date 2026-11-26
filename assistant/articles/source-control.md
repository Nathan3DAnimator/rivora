id: source-control
kind: concept
title: Controle de versão com Git | Source control with Git | Control de versiones con Git
keys: git push, git pull, travar arquivo, lock file, git lfs lock, diff do asset, asset diff, git, controle de versao, source control, versionamento, version control, commit, submit content, enviar conteudo, git lfs, lfs, arquivos binarios no git, binary assets git, status do git, git status, bolinha no content browser, dot on the asset, historico, history, perforce, svn, repositorio, repository
--- pt
```diagram steps
O projeto numa pasta com Git (git init, ou um clone)
Content Browser: bolinha verde (novo), amarela (modificado), vermelha (apagado)
Arquivo > Controle de Versão > Enviar Conteúdo: escolha os arquivos, escreva a mensagem, Enviar
```
O editor usa o **Git** do seu PC, como o plugin de Git da Unreal. Nada acontece sem o Git instalado ou fora de um repositório.
- **Estado de cada asset:** o `git status` roda numa thread (o editor nunca espera) a cada 5 s. Cada miniatura do Content Browser ganha uma bolinha no canto: **verde** para novo, **amarela** para modificado e **vermelha** para apagado.
- **Enviar Conteúdo** (Arquivo > Controle de Versão): a lista de arquivos alterados, cada um com uma caixa para marcar, e um campo para a mensagem. **Enviar** faz o `git add` dos marcados e o `git commit`, também numa thread; o resultado aparece na barra de status.
- **Atualizar Status:** lê o estado de novo na hora.
- **Rastrear Binários com Git LFS:** grava no `.gitattributes` as extensões dos assets da engine (.rmesh, .rtex, .rlevel, .rasset...) para o **Git LFS**, que guarda arquivos grandes fora do histórico. Precisa do Git LFS instalado (`git lfs install`).
- **Pull** e **Push** (no mesmo menu e na janela Enviar Conteúdo): trazem os commits do servidor (só avançando, sem mesclar) e enviam os seus, numa thread. O primeiro push de um repositório novo precisa do `git push -u origin` de sempre.
- **Diff**, ao lado de cada arquivo na janela: mostra o que mudou desde o último commit (em arquivos de texto, como Grafos e materiais).
- **Travar arquivos:** botão direito num asset > **Lock (Git LFS)**. Os outros não conseguem enviar mudanças nele até você usar **Unlock**. Use isso em binários, que o Git não mescla. Precisa de um servidor com LFS (GitHub, GitLab).
--- en
```diagram steps
The project in a folder with Git (git init, or a clone)
Content Browser: a green dot (new), yellow (modified), red (deleted)
File > Source Control > Submit Content: pick the files, write the message, Submit
```
The editor uses your PC's **Git**, like Unreal's Git plugin. Nothing happens without Git installed or outside a repository.
- **Each asset's state:** `git status` runs on a thread (the editor never waits) every 5 s. Each Content Browser thumbnail gets a dot in its corner: **green** for new, **yellow** for modified and **red** for deleted.
- **Submit Content** (File > Source Control): the list of changed files, each with a check box, and a field for the message. **Submit** runs `git add` on the checked ones and `git commit`, also on a thread; the result shows in the status bar.
- **Refresh Status:** reads the state again right away.
- **Track Binaries with Git LFS:** writes the engine's asset extensions (.rmesh, .rtex, .rlevel, .rasset...) into `.gitattributes` for **Git LFS**, which keeps big files out of the history. It needs Git LFS installed (`git lfs install`).
- **Pull** and **Push** (in the same menu and in the Submit Content window): bring in the server's commits (fast-forward only, no merge) and send yours, on a thread. A new repository's first push needs the usual `git push -u origin`.
- **Diff**, next to each file in the window: shows what changed since the last commit (in text files, like Graphs and materials).
- **Locking files:** right-click an asset > **Lock (Git LFS)**. The others can't push changes to it until you use **Unlock**. Use it on binaries, which Git doesn't merge. It needs a server with LFS (GitHub, GitLab).
--- es
```diagram steps
El proyecto en una carpeta con Git (git init, o un clon)
Explorador de contenidos: un punto verde (nuevo), amarillo (modificado), rojo (eliminado)
Archivo > Source Control > Submit Content: elige los archivos, escribe el mensaje, Submit
```
El editor usa el **Git** de tu PC, como el plugin de Git de Unreal. No pasa nada sin Git instalado o fuera de un repositorio.
- **El estado de cada asset:** `git status` se ejecuta en un hilo (el editor nunca espera) cada 5 s. Cada miniatura del Explorador de contenidos recibe un punto en su esquina: **verde** para nuevo, **amarillo** para modificado y **rojo** para eliminado.
- **Submit Content** (Archivo > Source Control): la lista de archivos cambiados, cada uno con una casilla, y un campo para el mensaje. **Submit** ejecuta `git add` en los marcados y `git commit`, también en un hilo; el resultado aparece en la barra de estado.
- **Refresh Status:** vuelve a leer el estado de inmediato.
- **Track Binaries with Git LFS:** escribe las extensiones de assets del motor (.rmesh, .rtex, .rlevel, .rasset...) en `.gitattributes` para **Git LFS**, que mantiene los archivos grandes fuera del historial. Necesita Git LFS instalado (`git lfs install`).
- **Pull** y **Push** (en el mismo menú y en la ventana Submit Content): traen los commits del servidor (solo fast-forward, sin merge) y envían los tuyos, en un hilo. El primer push de un repositorio nuevo necesita el habitual `git push -u origin`.
- **Diff**, junto a cada archivo en la ventana: muestra lo que cambió desde el último commit (en archivos de texto, como Grafos y materiales).
- **Bloquear archivos:** clic derecho en un asset > **Lock (Git LFS)**. Los demás no pueden enviar cambios a él hasta que uses **Unlock**. Úsalo en binarios, que Git no fusiona. Necesita un servidor con LFS (GitHub, GitLab).
