# Privacidade / Privacy

## Português

A Rivora Engine não coleta dados de uso, não tem telemetria e não tem anúncios. Ela só usa a internet em três casos:

1. **Atualizações** — ao abrir, o editor consulta a página pública de releases no GitHub
   (`api.github.com/repos/<repositório de releases>/releases/latest`) para saber se há versão nova. Nada seu é enviado;
   o GitHub vê o seu endereço IP, como em qualquer acesso a um site.
2. **Assistente (F1)** — na primeira pergunta, o editor baixa a base de artigos de um repositório público do GitHub.
   As suas perguntas **não** saem do computador: a busca é local.
3. **Relatórios de bug, crash e sugestões** — só quando **você** envia. A janela mostra exatamente o que vai antes de
   enviar e pede a sua confirmação. Antes de sair, o texto perde o seu nome de usuário do Windows, a sua pasta pessoal,
   o nome do computador e endereços de e-mail. O relatório vai para um serviço do desenvolvedor (um Cloudflare Worker)
   e fica num repositório privado, lido só pelo desenvolvedor para corrigir problemas. Sem internet, ele espera na
   pasta `%APPDATA%\Rivora\outbox` e você pode apagá-lo ali.

Os jogos feitos com a Rivora Engine não enviam nada à Rivora Engine. Se um jogo usa a Steam ou o online, isso é entre o
jogo, o seu desenvolvedor e a Valve.

## English

The Rivora Engine collects no usage data, has no telemetry and no ads. It goes online only in three cases:

1. **Updates** — when it opens, the editor reads the public releases page on GitHub to know whether there is a new
   version. Nothing of yours is sent; GitHub sees your IP address, as with any website.
2. **Assistant (F1)** — on the first question, the editor downloads the articles from a public GitHub repository. Your
   questions **never** leave the computer: the search is local.
3. **Bug reports, crash reports and suggestions** — only when **you** send them. The window shows exactly what goes and
   asks before sending. The text loses your Windows user name, your home folder, the computer's name and e-mail
   addresses first. It goes to the developer's relay (a Cloudflare Worker) and is kept in a private repository, read
   only by the developer to fix problems. Offline, it waits in `%APPDATA%\Rivora\outbox`, where you can delete it.

Games made with the Rivora Engine send nothing to the Rivora Engine. If a game uses Steam or online play, that is between
the game, its developer and Valve.
