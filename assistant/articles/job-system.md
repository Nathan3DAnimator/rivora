id: job-system
kind: concept
title: Job system: sistemas em paralelo (para profissionais) | Job system: systems in parallel (for professionals) | Job system: sistemas en paralelo (para profesionales)
keys: job system, jobs, threads, paralelo, multithread, nucleos, workers, estagios, parallel for, dependencias, schedule, cpu cores
--- pt
```diagram steps
Cada sistema declara o que lê e escreve | Conflito = estágio seguinte | Estágio: blocos de 16384 linhas | Um worker por núcleo, sem trava
```
- Os sistemas do ECS (movimento, animação, IA...) dizem os componentes que leem e escrevem. Dois que não tocam os mesmos dados rodam juntos; quem conflita vai para o estágio seguinte.
- Cada estágio vira tarefas (sistema × arquétipo × bloco) que os workers pegam por um contador atômico. Nada é alocado na execução.
- Medida: Rotating Movement em 1 milhão de entidades, 4,8 ms em série → 3,8 ms em 16 threads (o limite é a memória, não a CPU).
- O Debug Viewer (F10) mostra cada estágio na lista **Sistemas de Jogo**.
- Tarefas longas do editor (importar, assar luz) usam outro caminho: [Tarefas em segundo plano](article:async-api).
- Veja também: [Arquitetura ECS](article:ecs-architecture).
--- en
```diagram steps
Each system says what it reads and writes | A conflict = the next stage | A stage: blocks of 16384 rows | One worker per core, lock-free
```
- The ECS systems (movement, animation, AI...) state the components they read and write. Two that touch different data run together; a conflicting one goes to the next stage.
- Each stage becomes tasks (system × archetype × block) the workers take through an atomic counter. Nothing is allocated while it runs.
- Measured: Rotating Movement over 1 million entities, 4.8 ms serial → 3.8 ms on 16 threads (memory is the limit, not the CPU).
- The Debug Viewer (F10) shows each stage in the **Game Systems** list.
- The editor's long tasks (import, light baking) take another path: [Background tasks](article:async-api).
- See also: [ECS architecture](article:ecs-architecture).
--- es
```diagram steps
Cada sistema dice qué lee y qué escribe | Un conflicto = la siguiente etapa | Una etapa: bloques de 16384 filas | Un worker por núcleo, sin locks
```
- Los sistemas del ECS (movimiento, animación, IA...) declaran los componentes que leen y escriben. Dos que tocan datos distintos se ejecutan juntos; uno que entra en conflicto pasa a la siguiente etapa.
- Cada etapa se vuelve tareas (sistema × arquetipo × bloque) que los workers toman mediante un contador atómico. No se asigna nada mientras se ejecuta.
- Medido: Rotating Movement sobre 1 millón de entidades, 4,8 ms en serie → 3,8 ms en 16 hilos (la memoria es el límite, no la CPU).
- El Debug Viewer (F10) muestra cada etapa en la lista **Game Systems**.
- Las tareas largas del editor (importar, hornear luz) toman otro camino: [Tareas en segundo plano](article:async-api).
- Mira también: [Arquitectura ECS](article:ecs-architecture).
