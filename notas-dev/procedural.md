# Mundos procedurais (2026-11-07, pedido do usuário)

Pedido: "um sistema para quem faz jogos procedurais (Repo, Lethal Company, Minecraft, Peak e outros): geração de mapa
procedural com spawn de criaturas e estruturas, com suporte a longas distâncias como mais de 100 km".

## Arquivos
- `src/procgen.c/h`: o ruído, o terreno em chunks, as árvores, as estruturas, as criaturas, a instalação, o rebase,
  o autoteste. Componente `CProcWorld` (modo, semente, blocos, alturas, densidades, classes de Blueprint, salas).
- O que o gerador faz são filhos do ator com `CAttach` kind `ATTACH_PROCGEN` (fora do Outliner, das caixas do
  Blueprint e dos colisores da física; `procgen_purge` antes de salvar o nível: o arquivo guarda só o ator).
- O chão: `landscape_height`/`landscape_trace` chamam `procgen_height` (o topo da coluna mais fina sob o ponto):
  personagens, IA, veículos (o raio das rodas via `bp_trace_boxes`), ragdolls.

## Terreno
- Ruído de gradiente numa malha inteira (int64, `hash3` com splitmix), gradientes por tabela de 256 (sem cos/sin),
  double por dentro: fBm, ridged (cordilheiras), domain warp (costas sem retas), continente, colinas, máscara de
  montanha. Biomas por temperatura/umidade/altura: oceano, praia, planície, floresta, deserto, neve, montanha.
- Chunk = 16 blocos (32 m com bloco de 2 m). Detalhe por anel de distância: 16, 8, 4, 2, 1 colunas por lado
  (2,5 / 6 / 14 / 30 chunks). Cada coluna vai do topo arredondado (`step`) até o vizinho mais baixo (sem buracos
  entre detalhes diferentes: as colunas são sólidas).
- Streaming: os chunks que faltam (anéis de dentro para fora) até 32 por quadro; as alturas no job system
  (`re_parallel_for`), as entidades com orçamento de 2,5 ms (8 ms no editor). Novo antes de apagar o velho.
- Árvores por coluna (hash, densidade por bioma; coníferas na neve), uma estrutura por chunk com chance por km²
  (torre ou ruínas, ou a `structure_class`), criaturas nos chunks mais perto jogando (`creature_class` ou o
  Mannequin com BT_Chase), apagadas com o chunk.
- Água: um Water Body que segue o jogador ao nível do mar (barcos flutuam).

## Longas distâncias
- Coordenadas sem fim = posição no nível + `origin` (double) - base do ator. Passou de `rebase_distance` do 0: todas
  as transformações movidas por -d (em múltiplos de chunk), `origin -= d`, revisão do mundo (cena e colisores de
  novo), caixas do Blueprint, sondas da GI, ganchos (`procgen_rebase_hook`: veículos, corpos do IK/física) e as câmeras
  do editor (`procgen_set_camera_hook`).
- **Bug achado:** o hash da configuração incluía a posição do ator; o rebase move o ator, então cada rebase recriava
  o mundo inteiro e zerava a origem. O hash agora é só dos números; mover o ator à mão no editor regenera.

## Instalação
- Grade de salas crescida da entrada (sala existente com lado livre ganha vizinha por uma porta: árvore), laços com
  `loop_chance`, a entrada para -y. Cada sala: piso e teto (Box de prototipagem), paredes (Wall com abertura nas
  portas; **as paredes sólidas zeram a abertura** - o padrão do componente tem porta), luz pontual (mais quente mais
  fundo), itens, criaturas pela profundidade.
- (2026-11-25, item 19) As paredes entram T/2 no piso e no teto (antes encostavam face a face: costura para a GI); a porta
  ganha T/2 e segue com 2,40 m livres. A lâmpada de cada sala pelo piso: I = 100 lux x área / (0,5 x 4 pi) (sala de 12 m
  ~2300 cd; eram 260 cd fixas, ~7 lux: escura sob um Min EV100 alto, e o "bake bugado" era o contraste com a GI que
  vazava antes dele); `procgen_selftest` confere. Cena `--scene facility_leak` (a sala mais funda; `REALENGINE_FACILITY_LIGHTS`,
  `_BAKE`, `_ENTRANCE`), teste `facility_leak`.

## Medidas (RTX 3060, 1440p)
| Cena | Resultado |
|---|---|
| `--scene procgen` parado (1961 chunks, 24 mil colunas, 800 m) | 384 fps |
| `--scene procgen` a 250 m/s (4 rebases em 2,2 km) | ~100-120 fps; streaming 1,5 ms (pico 7 ms); sombra virtual refeita a cada chunk novo |
| antes (alturas na thread principal, cos/sin) | 20 fps (e o bug do rebase) |
| `--scene facility` (30 salas, validação) | 244-359 fps; 6 ms para montar |
Testes: `procgen_selftest` (determinismo, alcance e biomas a 100 km e 10 000 km; continuidade a 50 000 km; todas as
salas alcançáveis), `procgen_scene`, `procgen_facility`.

## Limites / próximos passos
- Terreno em colunas (o estilo Minecraft/terraços); terreno liso exigiria malhas de terreno na GPU (o caminho do
  skin cache com uma grade por instância é o candidato).
- Corpos rígidos não colidem com o terreno procedural (a física não usa landscapes).
- O rebase custa um quadro (~60 ms) ao reenviar a cena; distâncias maiores (2 km padrão) espaçam.
- Na prévia do editor o mundo é feito até 600 m; desfazer/carregar regenera.
