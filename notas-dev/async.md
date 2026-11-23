# Async: o editor nunca espera um trabalho longo

Pedido de 2026-11-06: o editor não pode travar enquanto executa uma função, qualquer que seja:
- abrir, importar ou alterar um personagem;
- fazer um bake;
- construir algo;
- arrastar algo para o viewport.

Junto veio a interface: barras de carregamento no canto inferior direito.
- São móveis e vêm em cascata.
- Cada uma diz o que está carregando.
- Um botão deixa todas finas, só com o progresso.
- Mostram o que acontece sem atrapalhar.

## O núcleo (`src/async.h`, `async.c`, no `re_core`)
Uma tarefa é de um destes três tipos:
- **Trabalho numa thread** (`async_run`):
  - roda num pool fixo de 2 workers de prioridade baixa (1 numa máquina de 2 núcleos);
  - o resultado é posto no lugar pelo `done`, na thread principal, no começo do quadro (`async_update`).
- **Passos na thread principal** (`async_steps`): o que precisa da GPU ou do mundo, uma fatia por quadro dentro de um
  orçamento (6 ms no editor, 2 ms no Play).
- **Progresso de outro sistema** (`async_track`): o sistema já anda sozinho (o bake de luz); o cartão só mostra.

Regras:
- A tarefa informa o que faz (`async_status`), quanto andou (`async_progress`, `async_progress_n`) e se foi cancelada
  (`async_cancelled`).
- `async_current()` dá a tarefa da thread, para código fundo informar sem receber parâmetro.
- Nada roda num worker que toque o mundo, a GPU ou um registro. O trabalho escreve arquivos ou memória própria; o
  `done` põe no lugar.
- Sem workers (o jogo empacotado, que não chama `async_init`), `async_run` roda na hora, no chamador: o mesmo código
  serve aos dois.
- Uma tabela de 64 tarefas e um SRW lock com seções curtas. Os textos são copiados para dentro e para fora sob o lock.

## Os cartões (`src/async_ui.c`)
- Canto inferior direito, acima da barra de status, por cima dos painéis. Nunca pegam o teclado (`NoFocusOnAppearing`,
  `NoNav`).
- Empilhados: o mais novo embaixo, no canto, e a pilha cresce para cima. São 5 cartões; o resto vira "+N".
- Cada cartão mostra:
  - o ícone (girando, ✓ verde ou ✗ vermelho);
  - o nome ("Importando Bonnie.tlend");
  - o que faz agora (o arquivo, a malha, o material);
  - a barra (deslizando quando o total é desconhecido);
  - "38%" ou "4 / 12", o tempo até agora e o que falta;
  - o X, que é Cancelar enquanto roda e Fechar depois.
- **Mover:** arrastar o cabeçalho. A distância ao canto fica no `editor.ini` (`async_x`, `async_y`).
- **Fino:** o botão do cabeçalho deixa uma barra de 4 px por tarefa; passar o mouse mostra o texto. Também fica no
  `editor.ini` (`async_compact`).
- **Sem piscar:** uma tarefa só ganha cartão depois de 0,15 s (as `ASYNC_QUIET`, depois de 0,4 s). Os passos rápidos
  nunca aparecem.
- Acabou: verde por 2,5 s e some (falhou: vermelho por 8 s, com o motivo).

## O que roda em segundo plano
| Operação | Como |
|---|---|
| Ler as malhas de novo (importar, Landscape esculpido, pacote de animação, Auto Rig) | `render_meshes_reload_async`. Um worker lê os `.rmesh` e monta os clusters num `MeshStage`; a thread principal só envia à GPU (`stage_upload`) e refaz as BLAS. Enquanto isso, as malhas antigas continuam desenhadas. Pedida de novo durante a leitura: mais uma depois. |
| Importar (`.tlend`, OBJ, glTF, FBX) | `editor_import_async`. O importador roda num worker com um mundo próprio. Os materiais viram só arquivos (`import_material` não usa mais o registro). No fim, os atores são copiados para o nível (um passo de desfazer, selecionados) e os materiais são lidos de novo (`mat_reload_file`). Uma importação por vez; as outras esperam na fila. |
| Materiais (mudar um nó, soltar um material numa malha, abrir um projeto com materiais novos) | As imagens de cada material são decodificadas pelos workers (WIC). O material espera desenhado com as texturas antigas e é assado na thread principal, alguns por quadro (6 ms). O cartão "Compilando materiais" mostra quantos faltam. A tela de carregamento e as chamadas explícitas (`mat_update` sem command buffer) ainda assam tudo de uma vez. |
| Build Lighting | Já andava uma fatia por quadro. Agora o progresso e o Cancelar ficam num cartão, não mais na barra de status. |
| Empacotar o Jogo | `editor_package_game_async`. A thread principal salva o nível, lista os arquivos e escreve o `game.ini`. Um worker copia tudo, grava o ícone e a versão do `.exe` e percorre os níveis e os assets. Tem Cancelar. A linha de comando e os testes usam a versão síncrona (o mesmo código). |

## Medidas e o teste (`async`, `--async-test <arquivo>`)
Num projeto com as malhas do teste pesado (2 M triângulos), tudo ao mesmo tempo:
- as malhas lidas de novo;
- um glTF importado e colocado;
- todos os materiais assados de novo;
- uma tarefa de 30 passos;
- uma tarefa longa cancelada pelo botão.

O teste mede o pior quadro enquanto tudo roda. Antes, o editor parava pelo tempo inteiro da leitura das malhas.
Passa se o pior quadro ficar abaixo de 60% do travamento antigo, medido na mesma execução, e abaixo de 150 ms.

### Medidas (RTX 3060, Release, 2560x1417; outro programa usava a mesma GPU, os números oscilam)
- **Ler as malhas de novo** (projeto com 2 M triângulos):
  - antes: o editor parava 200-360 ms;
  - agora: os arquivos, os clusters, os buffers da GPU, a cópia para o staging e as estruturas das BLAS são feitos
    pelo worker;
  - na thread principal sobram 40-70 ms num único quadro: esperar a GPU 2-7, as cópias 13-20 e o build das BLAS 22-58.
    É trabalho da GPU.
  - Antes da divisão, a parte principal era 100-110 ms (as BLAS alocavam memória e scratch uma a uma, por malha; agora
    é um buffer e um scratch para todas).
- **Material animado** (flipbook de 16 quadros): antes 65-130 ms de uma vez; agora o quadro 0 na hora e os outros
  dentro da fatia de 6 ms.
- **Importação de um glTF:** o importador inteiro roda no worker. Na thread principal fica só copiar os atores e ler
  os materiais (< 1 ms).
- **Teste completo:** importação, malhas, 9 materiais, 30 passos e o cancelamento, tudo em 0,6-1,2 s. O pior quadro
  foi de 78-108 ms (a aplicação das malhas); os outros quadros andam normalmente, e os cartões aparecem em quase todos.
- **Custo dos cartões:** desenhados só quando há tarefa visível. Sem tarefa, `async_update` e `async_ui_draw` custam
  um lock e uma varredura de 64 entradas (< 0,01 ms).

## Limites e próximos passos
- O que muda o mundo direto continua na thread principal, mas é curto: o Auto Rig, os pacotes de animação (as malhas
  deles já são lidas em segundo plano) e abrir um nível.
- Os passos que só existem na GPU (o envio das malhas, as BLAS) ainda custam um quadro mais longo.
  - O próximo passo é reaproveitar as BLAS e os vértices das malhas que não mudaram: hoje uma importação pequena refaz
    os 2 M triângulos do projeto.
  - Depois, o envio pode deixar de esperar a GPU.
- Uma tarefa só ganha cartão se for chamada pelo `async_*`. Os sistemas novos que demoram devem usar `async_run` /
  `async_steps` (e `dv_register` para o Debug Viewer).
