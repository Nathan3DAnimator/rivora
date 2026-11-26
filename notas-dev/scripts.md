# Scripts: programação por código (Real, C#, Python, C)

> (25/11/2026, item 13) A linguagem "Real" se chama **Rivora Script** na tela; a extensão nova é `.rvs` e `.real` continua abrindo (`script_lang_of`); `SL_REAL`, os nomes internos e o resto desta nota mantêm "Real".

Pedido de 2026-11-06: programar o jogo em código, não só em C. São quatro linguagens:
- C#, Python e C;
- a linguagem própria **Real**.

Junto vieram:
- um compilador interno que roda o código da forma mais otimizada;
- uma IDE inteligente dentro da engine (sugestões que conhecem a linguagem, detecção de erros, cores);
- funcionar no editor e no jogo;
- uma referência da API em janela flutuante;
- Blueprints e scripts que modificam o editor.

## Arquitetura
Cada script é **uma classe**.

O texto (qualquer das 4 linguagens) vira uma árvore comum (`script_internal.h`). Ela é checada e **rebaixada para um
grafo de Blueprint** oculto (`script_lower.c`), que o compilador dos Blueprints transforma no **bytecode por
registradores** da VM. Daí:
- O script roda onde o Blueprint roda: no Play do editor, no jogo empacotado e online, com eventos, timers, Delay,
  replicação e Custom Events (os dos widgets, dos notifies, das sequências, `OnAudioFinished_...`).
- **A API inteira da engine** sai pronta. Cada nó de Blueprint (cerca de 450) vira uma função: o nome do nó sem espaços
  (`Play Sound 2D` → `PlaySound2D`).
  - Em Python também vale snake_case (`play_sound_2d`); a busca não diferencia caixa nem `_`.
  - O pino `Target` é o próprio ator, ou o objeto de `obj.Funcao()`.
  - Nós que leem um asset ou nome no Details recebem como primeiro argumento esse nome entre aspas
    (`PlaySound2D("Explosao")`).
- A classe gerada vai para o registro dos Blueprints (`bp_adopt`, `generated`: nunca vira `.rbp`). Ela é colocada,
  criada (Spawn Actor), herdada (um Blueprint pode ter um script como pai) e usada no Details como um Blueprint.
- Os erros do compilador dos Blueprints voltam para a linha do script: cada nó guarda `L<linha>:C<coluna>` no comentário.

| Arquivo | O quê |
|---|---|
| `src/script.h`, `script.c` | Registro (varre o Content, compila tudo na abertura do projeto e do jogo, recompila ao mudar o arquivo), API como funções, modelos, palavras-chave |
| `src/script_parse.c`, `script_grammar.inc` | Léxico único das 4 linguagens; parsers por indentação (Real, Python) e por chaves (C#, C) |
| `src/script_lower.c` | Checagem de tipos, dobra de constantes e o rebaixamento para o grafo |
| `src/script_ide.h/.c` | A IDE (Editor de Código) |
| `src/script_tools.c` | A referência da API e os Utilitários do Editor |
| `src/script_test.c` | `--script-test` (teste `scripts`) |

## As linguagens
- **Real (`.real`)**:
  - indentação como Python;
  - `class X extends Actor`;
  - `var x: float = 1` (tipo opcional, inferido) e `@editable` para mostrar no Details;
  - `event Nome()` (Custom Event e eventos da engine) e `func f(x: int) -> int`;
  - `if` / `elif` / `else`, `while`, `for i in 0..10` (`..=` inclusivo, `step`) e `for x in array`;
  - `and` / `or` / `not` (também `&&` `||` `!`);
  - interpolação `"HP: {hp}"`, comentários `#` e `//`.
- **C# (`.cs`)**:
  - `public class X : Actor { ... }`, campos (`[Editable]` / `[SerializeField]`) e métodos;
  - `var`, `new Vector(...)`, `$"..."`;
  - `for` (vira laço de intervalo quando é `i < n; i++`), `foreach`, `while`, `break`, `continue`, `++` / `--`;
  - o construtor roda no começo do BeginPlay.
- **Python (`.py`)**:
  - `class X(Actor):`;
  - atributos da classe ou `self.x = v` no `__init__` (viram campos);
  - `def begin_play(self):`, `for i in range(...)`, f-strings, `True` / `False` / `None`;
  - `a if c else b`, `**`, `//`;
  - variáveis novas por atribuição.
- **C (`.c`)**:
  - o arquivo é a classe; o pai vem de `// parent: Character`;
  - globais são os campos; funções `void BeginPlay(void)`;
  - casts `(float)x`; `#include` ignorado.

- **C++ (`.cpp`, `.rcpp`; 2026-11-25, item 13)**: o caminho do C# (`parse_brace_file`) com `SL_CPP`: o léxico tira `std::`
  e troca `auto`->var, `vector`->List, `FVector`/`FRotator`/`FString`/`FLinearColor`/`int32`/`AActor`..., `to_string`->str;
  o parser aceita `class X : public Actor { public: ... };`, rótulos de acesso, `const`/`override`/`final`/`noexcept` depois
  dos parâmetros, `= default;`, `for (const auto& x : xs)` (vira for each), `static_cast<T>(e)`, `++i;`, `T&` (valor) e
  os campos editáveis (como o C). Fora: templates, exceções, ponteiros, sobrecarga. O teste `scripts` tem a mesma classe
  em C++ (TestCpp). Código nativo: o `plugin_api.hpp` e o Novo Plugin > C++ (teste `plugin_template`).

## O que vira o quê
- **Eventos da engine pelo nome**, em qualquer grafia:
  - `BeginPlay` / `Start` / `Awake` / `_ready`;
  - `Tick` / `Update` (`dt` = Delta Seconds);
  - `EndPlay` / `OnDestroy`;
  - `OnBeginOverlap` / `OnTriggerEnter` (`other`) e `OnEndOverlap`.
- **Outros métodos:** sem parâmetros e sem retorno viram Custom Event (chamáveis pelo nome: widgets, notifies,
  sequências). Com parâmetros ou retorno viram função de Blueprint (o tipo de retorno pode ser inferido).
- **Campos:** viram variáveis da classe. Valor literal é o padrão; o resto é calculado no começo do BeginPlay.
- **Locais:** variáveis por método (`<metodo>_<nome>`). Parâmetros e variáveis de laço são pinos, somente leitura.
- **Operadores:**
  - por tipo: int (nós Integer), float, vetor (`+ - *` por escalar, `/`), string (`+` = Append, `==`), bool, ator
    (`== null` = Is Valid);
  - constantes dobradas na compilação.
- **Controle de fluxo:**
  - `if` vira Branch (as duas pontas se juntam);
  - `while` vira While Loop, com uma flag de `break` na condição;
  - `for` de intervalo vira For Loop with Break, `for each` vira For Each with Break, e `step` vira while com contador;
  - `return` numa função vira Return Node.
  - `switch` (C#, C) e `match` (Python 3.10, Real) viram uma cadeia de `if` / `else if`, cada caso
    `assunto == valor` (vários valores: `case 1: case 2:` em C#/C, `case 1 | 2:` em Python, `case 1, 2` em Real).
    `default`, `case _` e `else` são o último `else`. O `break` do fim de cada caso some; um caso sem `break` avisa
    (cada caso roda só as próprias linhas). Um assunto que não é uma variável é recalculado a cada caso, e a IDE avisa.
  - A condição de um `while` não pode chamar funções com efeito: ela é relida a cada volta. Leia numa variável dentro
    do laço.
- **Embutidos:** `print`, `Vector`, `Rotator`, `Color`, `len`, `str`, `int`, `float`, `abs`, `min`, `max`, `clamp`,
  `sqrt`, `pow`, `floor`, `ceil`, `round`, `sin`, `cos`, `tan`, `atan2`, `lerp`, `random`, `randint`, `bool`.
- **Arrays:** `Add` / `append`, `Remove`, `RemoveAt`, `Contains`, `IndexOf`, `Insert`, `Clear`, `Count` / `len`,
  `Shuffle`, `a[i]` e `a[i] = v`.
  - **Literais:** `[3, 4, 5]` (C# e C também `{ 3, 4, 5 }`). Numa declaração (`var xs = [..]`, `int[] xs = {..}`) ou
    numa atribuição a um array, os itens vão para o próprio array (`Clear` e um `Add` por item). Em outro lugar, vão
    para um array oculto do método. O tipo dos itens é o mais largo (inteiros com um float: float). `[]` precisa do
    tipo declarado (`var a: int[] = []`).
- **Vetores:** `.x .y .z`, `v.x = 3` (refaz o vetor), `Length`, `Normalize`, `Dot`, `Cross`, `Distance`.
- **Structs** (2026-11-07):
  - Declaração:
    - Real: `struct Hit` com linhas `var damage: int = 0`;
    - C#: `struct Hit { public int damage; }`, dentro ou fora da classe;
    - Python: `@dataclass` + `class Hit:` com `damage: int = 0`;
    - C: `struct Hit { ... };` ou `typedef struct { ... } Hit;`.
  - Cada uma vira uma **estrutura de Blueprint** gerada, com o script como arquivo (`bp_adopt`, como a classe). Ela é
    registrada antes da classe, porque os nós Make / Break leem seus campos. Uma estrutura do projeto (`.rstruct`)
    também serve de tipo pelo nome.
  - Valor:
    - `Hit(5, 2.0)` / `new Hit(5, 2.0f)` / C `Hit h = { 5, 2.0f };` viram Make Hit, com os campos na ordem (os que
      faltam ficam no padrão);
    - `h.damage` vira Break Hit;
    - `h.damage = 3` (e `+=`) faz o struct de novo (Break e Make) e o atribui;
    - campos da classe, locais, parâmetros e retorno podem ser de um struct.
  - Os tipos de struct são `ST_STRUCT + i` na tabela do compile (`SCtx.snames`): primeiro os do script (nomes lidos
    antes de tudo, `prescan_structs`), depois os do projeto usados. Mudou o layout? `bp_refresh_all`.
  - Limites: 16 campos (o dos Blueprints), padrões só literais, campo array ainda não, um struct não guarda a si mesmo,
    sem `==` entre structs.
- **Strings:** `ToUpper`, `ToLower`, `Contains`, `Replace`, `Substring`, `Length`.

## A IDE (Editor de Código)
Abre com duplo clique num script (o Content Browser tem o tipo **Script**). Adicionar > Script cria um script em
qualquer das 4 linguagens, ou um Utilitário do Editor.
- **Edição:**
  - abas, fonte monoespaçada (Consolas);
  - cores por linguagem: palavras-chave, tipos, strings (com as chaves da interpolação), números, comentários
    (inclusive `/* */` de várias linhas), funções da API e membros da classe;
  - indentação automática (depois de `:` e `{`, e das linhas que abrem bloco em Real) e pares `() [] {} ""`;
  - Tab / Shift+Tab em blocos, Ctrl+/ comenta, Ctrl+D duplica, desfazer e refazer;
  - buscar e substituir (Ctrl+F), ir para a linha (Ctrl+G).
- **Inteligência:**
  - **Completar** enquanto digita ou com Ctrl+Space. Mostra locais e parâmetros em escopo, campos, métodos, palavras-
    chave, tipos, embutidos e a API (em snake_case no Python), com a assinatura e a descrição de cada uma.
  - **Assinatura** da chamada sendo digitada, com o argumento atual destacado.
  - **Erros enquanto digita:** 0,5 s depois de parar, o texto é compilado sem salvar e sem trocar a classe. Os erros
    aparecem sublinhados (vermelho erro, amarelo aviso), com marcador na margem, dica ao passar o mouse e um painel de
    problemas clicável.
  - Passar o mouse numa função da API mostra a assinatura e a descrição.
  - **F12** vai à definição (campo, método, local) ou à página da API; **F1** abre a referência.
- **Salvar:** Ctrl+S salva e recompila a classe. Fora do Play, o editor também recompila os arquivos mudados por outro
  editor (recarga a quente a cada 0,5 s). Durante o Play a classe não é trocada.
- **Barra de status:** linguagem, linha e coluna, compila ou não, erros, avisos, ms da compilação, nós e ops.

## Referência da API (Ajuda > Referência da API, F1)
Janela flutuante (pode ser arrastada para fora do editor: multi-viewport).
- Busca por nome ou categoria; as funções agrupadas por categoria.
- Assinatura, pura ou ação, descrição.
- Exemplo na linguagem escolhida e nas quatro.
- Botão copiar.

## Utilitários do Editor (Ferramentas > Utilitários do Editor)
Classes cujo pai é `EditorUtility`, scripts ou Blueprints.
- Cada Custom Event delas vira um botão que roda no nível do editor, fora do Play. O Ctrl+Z desfaz.
- A VM roda o evento com um ator oculto como "self" (`bp_run_editor_event`).
- Nós novos de editor: **Get Selected Actor Count**, **Get Selected Actor** e **Select Actor**. Funcionam também como
  funções nos scripts.

Exemplo em Real:
```
class EUW_Raise extends EditorUtility
    event RaiseSelected()
        for i in 0..GetSelectedActorCount()
            var a = GetSelectedActor(i)
            a.AddActorWorldOffset(Vector(0, 0, 50))
```

## Medidas (RTX 3060, Release)
- **Compilação** (texto → grafo → bytecode): 0,12-0,30 ms por script de ~30 linhas (35-40 nós, 46-51 ops). Os 5
  scripts do teste levam 4-7 ms na abertura do projeto.
- **Execução:** é a da VM dos Blueprints, o mesmo custo de um Blueprint equivalente, com o código nativo (abaixo).
- **Código nativo (JIT):** 40.000 voltas de um laço aritmético (Real): interpretado 3,16 ms, nativo 0,40 ms (8×), o
  mesmo resultado. 29 das 40 ops da classe viram nativas; o código tem 1 KB e foi gerado em 0,05 ms.
- **IDE aberta**, com a referência da API e os utilitários também abertos: ~0,4 ms de CPU por quadro no editor.
  O ponteiro do script fica guardado no documento: antes cada identificador colorido consultava o disco, e o quadro
  custava 1,5 ms a mais.

## Compilação nativa (`src/bp_jit.c`, `bp_jit.h`)
Um **JIT x64** do bytecode de registradores, em modo misto com o interpretador. Vale para os Blueprints e os scripts,
no editor e no jogo empacotado.
- **Quando:** a primeira vez que uma classe roda, `bp_jit_build` gera uma função de código de máquina com um lugar para
  cada op. Um compile da classe (`bp_compile`) descarta o código; quem libera ou troca o asset também
  (`bp_jit_free`).
- **O que vira nativo** (SSE escalar e vetorial; a VM guarda inteiros como floats):
  - movimentos de até 4 valores;
  - saltos (`JMP`, `JZ`, `JNZ`, `JGT`, `JGE`, `JEQ`, com o NaN como no C);
  - `INC`, `DELTA`, componentes;
  - os nativos de aritmética: + - * / (com a divisão por zero dando 0), comparações, `and` / `or` / `not`, Select,
    min / max / abs / clamp / lerp / sqrt, floor / ceil / truncar, e soma, subtração, escala e montagem de vetores.
- **O resto** (chamadas, eventos, nativos que mexem no mundo, strings, arrays, latentes, `RET`): é um trecho que
  devolve o pc. O interpretador roda aquela op e chama o código de novo dali. Por isso o JIT nunca muda o que uma op
  faz: só tira o despacho das que são números.
- **Segurança:**
  - código folha, sem chamadas nem pilha, só registradores voláteis (`rcx`: os registradores da instância, `rdx`: as
    constantes, `r8`: o contexto), então não precisa de dados de unwind;
  - memória `PAGE_EXECUTE_READ` depois de escrita;
  - o salto para trás desconta as ops do laço do orçamento (1 M ops por execução), e um laço infinito ainda para com a
    mesma mensagem.
- `REALENGINE_JIT=0` desliga (o interpretador sozinho, para medir ou suspeitar do JIT).

## Teste (`scripts`, `--script-test`)
- A mesma classe nas 4 linguagens (laço, while com break, função com parâmetro e retorno, Custom Event, interpolação,
  API da engine, Tick, literal de array, switch / match), no Play. Resultados iguais em todas: soma 45, dobro 14,
  Tick, flag, `"n=5 sum=45"`, ator movido 100 cm, a soma do literal `[3, 4, 5]` = 12, o segundo caso escolhido (2) e
  um struct feito, mudado, guardado num campo e lido (16).
- A medida do JIT: o mesmo laço interpretado e nativo (o melhor de 5 cada). Os resultados têm de ser iguais, e o
  nativo precisa ser pelo menos 20% mais rápido.
- Um script com 3 erros reportados nas linhas certas.
- Um Utilitário do Editor que sobe o ator selecionado 50 cm.
- A IDE, a referência e os utilitários desenhados durante a medida.

## Limites e próximos passos
- Um script = uma classe, com os seus structs. `switch`/`match`, literais de array, structs e o JIT entraram em
  2026-11-07; `do ... while` (C#, C) em 2026-11-18 (abaixo).
- O acesso a variáveis de outro ator é só por funções ou interfaces.
- Locais contam no limite de 64 variáveis da classe (campos + locais).
- Custom Events não têm parâmetros (é a regra dos Blueprints); métodos com parâmetros viram funções.
- O JIT ainda não compila chamadas de função entre métodos, arrays nem strings: eles saem para o interpretador a cada
  op (o laço continua nativo).
- Pendente: depurador com breakpoints no Play; Editor Utility Widgets (painéis de UI próprios). Os dois esperam o
  comentário sobre a IDE.

## do ... while (Q24, 2026-11-18)
C# e C: `do { ... } while (cond);` vira no parser (`script_grammar.inc`) `while (true) { corpo; if (!(cond)) break; }`
(o corpo roda ao menos uma vez; a condição, uma vez por volta, no fim). Um `continue` solto no corpo pularia a condição:
dá erro ("continue inside do ... while isn't supported"), até o Q24.21. O C# do teste `scripts` conta n com um
do ... while (o rótulo "n=5 sum=45" sai dele). Auditoria da programação: [auditoria-ia-programacao.md](auditoria-ia-programacao.md).

## Breakpoints no Editor de Código (2026-11-09)
O depurador de Blueprint (blueprints.md "Debugger") vale para os scripts: os nós gerados guardam a linha
(`L<linha>:C<coluna>` no comentário → `BpDbgNode.line`). Clique na margem (ou F9 na linha) põe o breakpoint no primeiro
nó a rodar daquela linha (o menor site); um ponto vermelho na margem; parado, a linha fica amarela com a seta, e o
Step Over/Into anda por linha (os outros nós da mesma linha seguem). Uma linha sem código avisa na barra. Teste
`bp_debugger` (linha 6 → 7 de um script Real).
