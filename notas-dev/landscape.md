# Landscape (terreno) e Foliage (vegetação)

Código: `src/landscape.h/.c` e `src/foliage.h/.c` (no `re_render`), componentes `CLandscape` e `CFoliage`
(`components.h`), painéis de modo e Detalhes (`editor.c`), pincéis no viewport (`editor_viewport.c`), vento em
`shaders/mesh.vert` (`INST_WIND`). Adicionar > Paisagem > Landscape / Foliage. Cenas de teste: `--scene landscape`,
`--scene landscape_hm`.

## Modos (como a Unreal, relatório #68)
- **Shift+1** Seleção, **Shift+2** Paisagem, **Shift+3** Folhagem (botão de modo na toolbar). Os modos Paisagem e
  Folhagem trocam o painel Place Actors (à esquerda) pelo painel do modo, com largura mínima de 22 em.
- **Paisagem**: *Gerenciar* (Criar Novo por ruído, ou **Importar de Arquivo**: um heightmap, com "Resolução da imagem"),
  *Esculpir* (Elevar, Rebaixar, Suavizar, Aplanar; raio, força) e as linhas de heightmap da paisagem selecionada.
- **Folhagem**: os tipos (até 4) e o pincel (Pintar / Apagar, raio, densidade da pintura), Preencher, Remover Todos.

## Landscape
- **Alturas**: grade (64 a 1024 quads por lado) gerada por ruído fBm (oitavas altas em cristas: morros a montanhas,
  Aspereza 0-1, Semente) com o meio mais plano, **ou de um heightmap** (Import from File da Unreal): PNG/TIFF de 16 bits
  em cinza lidos exatos pelo WIC (`mat_load_image_gray16`), 8 bits ou cor pela luminância, `.r16`/`.raw` da Unreal
  (quadrado, 16 bits little-endian). Preto = a altura do ator, branco = Altura (cm); colunas ao longo de +X, linhas ao
  longo de +Y; amostragem bilinear (a imagem não precisa ter o tamanho da grade). **Somar Deslocamento** soma a imagem às
  alturas que já existem (cinza médio = nada). A importação é aplicada quando a grade fica pronta (`landscape_import_heightmap`
  depois de mudar a resolução). Fica em `Content/Landscapes/<nome>.rland`.
- **Desenho**: blocos (1-16 por lado, padrão 4×4) gravados como `.rmesh` de verdade (`<nome>_<i>.rmesh`): passam
  pelo culling, sombras por raio, GI e lightmaps como qualquer malha. Entidades filhas (`ATTACH_LANDSCAPE`). O renderer
  tem 255 malhas no total: cada bloco ocupa uma.
- **Esculpir**: botão esquerdo no viewport (Shift inverte), anel do pincel no chão. Só os blocos tocados são regravados
  (a cada 0,35 s durante o traço e ao soltar); a grade é salva ao soltar.
- **Chão**: `landscape_height` (o mesmo triângulo que a malha desenha) entra na colisão dos Characters/projéteis, em
  `bp_trace_boxes` (Foot IK, visão da IA) e na grade de navegação.
- O jogo empacotado agora leva `.rland` (antes as esculturas se perdiam: o jogo gerava o ruído de novo) e `.rfol`.

## Foliage (separada da malha do chão)
- Um ator **Foliage** (`CFoliage`, o Instanced Foliage Actor da Unreal) guarda as instâncias pintadas em
  `Content/Foliage/<nome>.rfol` (posição, giro, tamanho, tipo; 28 bytes cada, até 60 000). Cada instância é uma entidade
  filha estática (`ATTACH_FOLIAGE`) com o mesh/material/cor do seu tipo; **vento** no vertex shader.
- **Tipos** (até 4): malha, material, cor, densidade (por 100 m²), escala mín/máx, alongamento (altura/largura),
  inclinação máxima, vento, e se o pincel pinta/apaga esse tipo. Mudar a aparência de um tipo atualiza as entidades dele.
- **Pincel**: completa os tipos marcados até a densidade dentro do círculo (em ~0,2 s segurado, sem passar dela, com
  distância mínima entre instâncias do mesmo tipo), respeitando a inclinação; Shift (ou a ferramenta Apagar) remove. O chão
  é a paisagem; sem paisagem, o plano onde o mouse bate. **Preencher** cobre as paisagens na densidade do tipo (no lugar do
  que ele tinha). Adicionar/remover cria/destrói só a entidade daquela instância; o arquivo é gravado ao soltar o botão.
- **Níveis antigos** (até 0.14 a paisagem espalhava as próprias plantas): ao abrir, a configuração vira o primeiro tipo
  de um ator Foliage (criado se não houver), que é preenchido uma vez (`foliage_moved` marca a paisagem).
- Desfazer (Ctrl+Z) depois de pintar recria as plantas a partir do arquivo (a pintura em si não é desfeita, como a escultura).

## Medidas (RTX 3060, 2560x1417)
| Situação | Resultado |
|---|---|
| Heightmap 257x257 (.r16) numa grade 129x129 | 0,2-0,4 ms; erro máximo da altura 0,013 cm |
| Pincel de Foliage, 3 pinceladas de 5 m (10 instâncias) | 0,01-0,02 ms |
| Preencher 200 m (3118 plantas) / montar as entidades | 3,3-3,6 ms |
| Editor no modo Folhagem (cena `landscape_hm`) | 362 fps, CPU 1,2 ms, GPU 2,7 ms |
| Jogando a cena `landscape` (personagem no chão, 3118 plantas do ator Foliage) | 189-207 fps, CPU 1,6-1,7 ms, GPU 4,7-5,1 ms |
| O mesmo no commit anterior (4800 plantas da própria paisagem), mesma máquina e hora | 140-171 fps, CPU 1,8-2,0 ms, GPU 5,6-6,8 ms |

Medidas antigas (0.13): jogando 221 fps, CPU 1,4 ms, GPU 4,35 ms; editor de cima 192 fps; montagem inicial ~50-96 ms.

## Teste
- `landscape`: 1 terreno, 16 blocos, > 1000 plantas (de um ator Foliage preenchido), o Character largado de 30 m fica de
  pé, a navegação cobre os morros, ≥ 180 fps.
- `landscape_heightmap`: um `.r16` de rampa importado (alturas dentro de 1 cm), o pincel pinta 5-12 instâncias dentro do
  círculo e no chão, apagar deixa 0, ≥ 300 fps.

## Limites / próximos passos
- LOD por bloco, buracos. (Pintura de camadas, desfazer da escultura e exportar heightmap: feitos no B11, abaixo; Q23.)
- Rotação/escala do ator não se aplicam às alturas (só a posição). Foliage só sobre paisagens (ou o plano do mouse).

## GI nos blocos (2026-11-03)
Relatório #42 (andando, o terreno "sumia", ficava preto e a luz pixelada): os blocos (malhas de milhares de triângulos)
caíam na regra das malhas densas do cache de superfície e iam para as sondas de 2 m, enterradas nos morros (preto), e a
distância de cada bloco era medida pelo raio de um cubo unitário. Agora os blocos ficam no cache com quadro pelo tamanho
x/y (o UV2 é o bloco visto de cima) e a distância pela caixa da malha ([gi.md](gi.md), 2026-11-03). `--scene landscape`:
806/806 malhas ao alcance com quadro (antes os blocos todos nas sondas); teste `landscape` com `--cache-all`.

## Camadas pintadas, desfazer e exportar (B11, 09/11)
- **Material**: `Material.layer_path[3]`/`layer_tile[3]` (JSON `layers`), Details > Landscape Layers no editor de material.
  Compilado como o detail normal: `MATX_LAYERS`, os slots das camadas em e3.xyz e os tiles em e5.xyz (lidos só sem
  `MATX_LOBES`: um material com camadas desliga os próprios lóbulos extras), seguidos por `detail_follow` (o
  `any_detail` passou a ligar também com camadas: sem isso os slots ficavam 0 e as camadas saíam pretas).
- **mesh.frag** (RE_EXT): os pesos são a cor de vértice rgb com alfa 0 (alfa 1: malha sem camadas, peso 0; sem essa marca
  os blocos sem cores liam peso 1 em tudo); cada camada com peso lê base e normal no xy do mundo / tile; base, normal,
  rugosidade e metal misturados (o resto é o material do Landscape).
- **Landscape**: os pesos (3 bytes por ponto) em `<nome>.rlw` ao lado do `.rland`; os blocos ganham `RMESH_COLORS` (bytes
  sRGB: a decodificação do mesh.vert devolve o peso). A primeira pintura cria a grade e marca todos os blocos. Ferramentas
  Paint e Erase (Shift troca), `landscape_set_paint_layer`; uma camada subindo empurra as outras (soma <= 1).
- **Desfazer**: um anel de 8 "antes" (alturas e pesos) tirado no começo de cada traço; Ctrl+Z no modo Landscape
  (`landscape_undo`), depois o desfazer do editor.
- **Exportar .r16**: `landscape_export_heightmap` (16 bits little-endian, o mais baixo 0, o mais alto 65535), botão nos
  Details.
- **Medidas**: `landscape_paint` (200 m, 3 manchas de 18 m) GPU 4,08-4,26 ms contra 3,68 ms sem camadas (+0,4-0,5 ms).
  Testes: `landscape` (pinta, confere 1,00 no centro e 0 longe, desfaz, exporta 132 098 bytes) e `landscape_paint` (as três
  camadas na tela, validado).
- **Falta**: LOD por bloco com costura, buracos, foliage sobre qualquer malha, PCG, terreno procedural liso com colisão, a
  neve e o molhado como camada, mais de 3 camadas.

## Sombra distante (Q21, 2026-11-18)
As alturas das Landscapes viram o heightfield da sombra distante do sol (`landscape_generation` refaz quando mudam):
[visao-distante.md](visao-distante.md).
