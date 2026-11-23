# Cidade viva: relógio, Smart Objects e eventos do mundo (Q27.10)

As rotinas da cidade do GTA 6 (lojas que abrem e fecham, gente que vai até elas, testemunhas que chamam a polícia) num
módulo pequeno, sem componente novo (os 64 estão em uso).

## Arquivos
- `src/city.h/.c`: o relógio, o registro dos Smart Objects do quadro, o barramento de eventos (anel de 64), as chamadas
  das testemunhas.
- `src/crowd.c`: a City Routine dos agentes (no ramo de vagar do `step_agents`), a reserva das vagas, `crowd_nearest`.
- `CInteractable` (no fim): `so_slots`, `open_q`, `close_q` em bytes (horários em quartos de hora): o padrão de uma classe
  Blueprint guarda 128 bytes e o componente tinha 120.

## Como funciona
- **Relógio**: a hora do dia (0..24) andando `scale` segundos de jogo por segundo real enquanto joga (padrão 9:00, 60).
  `city_update` roda dentro do `crowd_update` (os dois laços, editor e jogo, já o chamam). Nós: Get / Set Time of Day.
- **Smart Object**: qualquer ator com Interactable e **Smart Object Slots** > 0; **Opens At** / **Closes At** (as duas 0:
  sempre aberto; 22 a 6 passa da meia-noite). Juntados a cada quadro (até 256), as visitas mantidas por entidade.
- **City Routine** (Crowd Spawner): a cada quarto de hora metade dos agentes (pelo índice e o quarto) pode ir a um Smart
  Object aberto com vaga; a vaga é reservada com `re_atomic_add` (os passos rodam em paralelo: sem isso o primeiro quadro
  aberto mandou 199 para 12 vagas), fica no seu lugar em volta dele 10-40 minutos de jogo e sai; fechou, todos saem.
- **Eventos**: Report Crime (Location, Radius) posta CRIME, faz o pânico da multidão (`crowd_alarm`) e escolhe o agente
  mais perto como testemunha; 2-4 s depois a chamada chega (POLICE_CALLED, Get Police Calls).

## Medidas (RTX 3060, 1440p, cena `city_routine`: 200 agentes, loja de 12 vagas das 9 às 11h, relógio a 1800×)
- Antes das 9h: 0 visitantes; aberta: até 12 (as vagas); depois das 11h: 0; o crime às 11:45: 1 chamada, 2 eventos.
- **0,0007 ms por agente** no pior quadro da multidão (meta 0,002); 433 fps.

## Fica para depois
- Agenda por agente (casa, trabalho, lazer por hora), State Tree na multidão, polícia de verdade indo ao local, notícias
  na UI, Smart Objects com animação na vaga (sentar, comprar).
