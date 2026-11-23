# Job system (jobs.c)

- Cada sistema declara `require` (componentes do arquétipo), `reads` e `writes`. `schedule_add` põe o sistema no
  estágio seguinte ao último sistema anterior com quem ele conflita (um escreve o que o outro lê ou escreve).
- `schedule_run`: estágio por estágio; cada estágio vira tarefas (sistema × arquétipo × bloco de 16384 linhas) e
  roda em `re_parallel_for` (workers fixos, um por núcleo, itens por contador atômico, sem lock; quem chama também
  trabalha). Sistemas do mesmo estágio não tocam os mesmos dados. Nada é alocado na execução.
- Teste `jobs`: estágios corretos, ordem entre dependentes em 300 mil entidades, paralelo == serial em 1M.
- Medida (16 threads): Rotating Movement em 1M entidades 4,8 ms serial → 3,8 ms paralelo. É **limite de banda**
  (~92 MB lidos/escritos por frame com a coluna Transform intercalada). Próximo passo para estes laços: separar o
  Transform em campos quentes/frios (SoA por campo) para ler só a rotação.

## O pool não espera mais quem dorme (2026-11-19, sobra do Q8)
`re_parallel_for` esperava **todos** os workers informarem o fim, inclusive os que dormiam e acordavam sem item nenhum:
um worker dormindo leva ~0,1–0,3 ms para acordar, e todo trabalho curto pagava isso (o passo da física faz ~15
chamadas com partes seriais no meio, em que os workers dormem). Agora quem chama espera só os **itens** (contador `done`)
e, antes de escrever o próximo trabalho, os workers que de fato **entraram** no anterior (`active`, estão rodando: um
instante). A geração fica ímpar enquanto o trabalho é escrito; o worker se registra e confere a geração de novo, então
nunca lê um trabalho pela metade. Windows e POSIX (Android) iguais. Medida: a física com 100 corpos parados,
broadphase 0,50 → 0,11 ms e solver 0,43 → 0,10 ms por passo. Testado e descartado: manter os workers girando durante o
passo (com 16 threads lógicas ocupadas o Windows tira a thread principal do núcleo: o passo foi de 25 para 59 ms).
