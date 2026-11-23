# Trânsito (Q27.2, 2026-11-23)

Código: `src/traffic.c/.h`; a via é uma Spline (`CRail`, campos `road_*`: o fim do componente, níveis antigos leem 0).
Details da Spline > **Road**: Traffic Road, Lanes Each Way (1-4), One Way, Speed Limit, Traffic Density, Lane Width,
Signal Green Time. Cena de teste: `--scene traffic`. Do GTA 6 (auditoria-gta6.md): a patente de navegação da Take-Two
dá a cada motorista características próprias (aceleração, frenagem, velocidade máxima e em curva) sobre nós de via.

## Como funciona
- **Rede**: cada via com Traffic Road vira faixas pela curva da spline no mundo, deslocadas para a direita do sentido
  (mão dupla: as de volta do outro lado). A ponta de uma faixa e o começo de outra (de outra via) a menos de 12 m se
  ligam: um cruzamento (nó). Nós com três vias ou mais e Signal Green Time > 2,5 s têm semáforo: duas fases (o eixo de
  onde cada faixa chega), o verde de cada uma pelo tempo da via, 2 s de tudo vermelho entre elas, defasado por nó.
- **Carros** em SoA (faixa, distância, velocidade e o perfil): velocidade desejada 85-115% do limite, aceleração
  1,2-2,2 m/s², frenagem confortável 2-3 m/s², tempo de distância 1,0-1,8 s, folga parado 1,5-2,5 m, por semente.
- **IDM** (Intelligent Driver Model, Treiber 2000): `a[1 - (v/v0)^4 - (s*/gap)^2]`, `s* = s0 + vT + v·Δv/(2√(ab))`. O
  líder: o carro da frente na faixa, ou o primeiro da próxima faixa; no vermelho, a linha de parada (3 m antes do fim)
  é um carro parado, se ainda dá para parar (b × 1,5). Os carros de cada faixa ordenados por distância a cada quadro
  (contagem por faixa e inserção: quase em ordem, ninguém ultrapassa na faixa), passo em paralelo por faixa.
- **Cruzamento**: ao passar do fim, a próxima faixa sorteada entre as ligadas (por carro e quadro). **Entrada**: se o
  começo da faixa de destino está ocupado (por quem já estava ou por quem entrou neste quadro de outra aproximação), o
  carro espera na linha. Sem isso, dois fluxos entrando na mesma faixa viam o mesmo líder e se sobrepunham (-430 cm).
- **Anéis**: além de 150 m da câmera, o carro anda em quadros alternados com o dobro do dt.
- **Desenho**: um ator por carro (Static Mesh, caixa 4,4 × 1,8 × 1,4 m, cor de uma paleta), posto pela curva a cada
  quadro em paralelo; o yaw é `rotation[1]` (pitch, yaw, roll como o rotator da Unreal). Cinemático: os veículos do
  jogador são os de `vehicle.c`.

## Medidas (RTX 3060, 1440p)
`--scene traffic`: grade de 8 × 8 cruzamentos a 100 m, 112 vias de mão dupla (224 faixas, 64 nós, 60 com semáforo de
12 s), 1568 carros (7,5 por 100 m de faixa): **320 fps**, trânsito **0,47 ms** de CPU (ordenação, IDM em paralelo,
passagens, 1568 atores postos), menor distância 163 cm, nenhum vermelho avançado com espaço para parar, 163
cruzamentos atravessados em 4 s, ~120 parados nas filas, média 14 km/h (sinais de 12 s, quadras de 100 m). Com 9 carros
por 100 m a grade quase travava (média 9 km/h).

## Teste
`traffic_queue` (área transito, no kit rápido): ≥ 1500 carros, semáforos, nenhuma sobreposição, nenhum vermelho
avançado, ≥ 150 cruzamentos e ≥ 20 parados (fluir e fazer fila), ≤ 1 ms, ≥ 200 fps.

## Próximo
Curva suave pelo cruzamento (hoje o carro passa da ponta de uma faixa ao começo da outra: um salto lateral de uma
faixa), troca de faixa, desvio de via bloqueada (A* com custo pela ocupação), carros estacionados por PCG, o veículo
físico perto do jogador trocando com o cinemático sem salto, faróis distantes como sprites, pedestres atravessando.
