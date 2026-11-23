# UDIM (Q18: `udim.c/.h`, `material_udim.inc`, `udim_test.c`, `shaders/mat_vm.glsl`, `mesh.frag`)

Pedido (plano 0.43, Q18): texturas UDIM como na Unreal. Uma pasta ou um nome `<base>.1001.png`…`10xx` vira um asset de
textura UDIM (tiles BC7/BC5 com mips); o Texture Sample comum (o nó **Image Texture**) aceita UDIM, o tile vem da UV e não
há emenda nos mips; streaming por tile no orçamento de texturas (só os tiles vistos ficam residentes); os importadores
reconhecem UDIM pelo nome.

## Numeração e UV
- Tile 1001 = u 0..1, v 0..1; 1002 à direita (u 1..2); dez por linha; 1011 acima (v 1..2 no V para cima do
  Blender/Mari). Lidos: 1001–1100 (10 × 10, a faixa da Unreal e das ferramentas).
- O V da engine desce (`v = 1 - v_blender`, os importadores já viram): o tile acima de 1001 é **v -1..0**. Tudo passa por
  `udim_of_uv`/`udim_origin` (C) e `udim_tile` (GLSL): coluna `floor(u)`, linha `-floor(v)`.

## O asset `.rudim` (versão 1, `udim.h`)
Binário little-endian, lido por mmap como está (`re_map_file`; no jogo empacotado vem do `.rpak` do mesmo jeito):
- `RUdimHeader` (64 B): `RUDM`, `version`, `header_size`, `flags` (`UDIM_F_NORMAL`, `UDIM_F_SRGB`), `format` (0 RGBA8,
  1 BC7, 2 BC5), `tile_count`, `tile_entry_size`, `table_offset`, `file_size`, o nome do padrão de origem.
- `RUdimTile` × n (48 B cada): `udim`, `width`, `height`, `mips`, `data_offset` (alinhado a 64), `data_size`, a média RGBA8.
- Os mips de cada tile, do maior ao 1×1: blocos BC linha a linha (`(w+3)/4 × (h+3)/4 × 16 B`) ou linhas RGBA8.
- **Versões**: o leitor aceita qualquer versão ≤ `UDIM_VERSION` e lê só os campos que conhece (`header_size` e
  `tile_entry_size` crescem nas próximas: o teste abre um arquivo com cabeçalho de 96 B e entradas de 64 B). Uma versão
  maior é recusada com o motivo ("version 2 (this engine reads up to 1)"); arquivo cortado ou offsets fora: recusado.
  Mudou o formato? Suba `UDIM_VERSION`, só acrescente campos no fim e mantenha a leitura das antigas.
- **Sem asset**: um caminho com o token do Blender (`skin.<UDIM>.png`, `<UVTILE>` do Mudbox) também abre, lendo os PNGs
  de cada tile como qualquer imagem (BC7 na GPU ao carregar). É o caminho do `.tlend` antigo; os importadores preferem
  criar o `.rudim`.

### Import (`udim_import`)
- Origem: uma pasta (o conjunto com mais tiles), um padrão com token, ou o arquivo de um tile (os irmãos vêm juntos).
  Nomes: `<base>.1001.ext`, `<base>_1001.ext`, `<base>-1001.ext`, `<base>_u1_v1.ext`. Um "rock.1024.png" sozinho continua
  imagem comum (precisa de pelo menos 2 tiles, ou do token).
- Cada tile: decodificado pelo WIC (Targa à parte), levado à potência de 2 mais próxima (≤ 8192, filtro de caixa),
  mips na CPU (cor em luz linear, normal como vetor renormalizado, máscaras — rough/metal/ao/height/mask/orm — como
  estão), comprimido em **BC7 modo 6** (o mesmo codificador do `bc7.comp`, portado para C) ou **BC5** para normal maps
  (pelo nome: normal, nrm, _nor, _n.), em paralelo (`re_parallel_for` por linha de blocos). O PSNR do mip 0 sai no log.
- Content Browser: soltar um tile (ou a pasta) do Explorer, ou clique direito > **Importar Pasta UDIM...**: um cartão
  assíncrono (`async_run`), o `.rudim` na pasta atual. O `.rudim` aparece como textura (miniatura e visualizador: o tile
  1001 decodificado na CPU, `udim_preview_rgba`).

## No material
- O nó **Image Texture** com um `.rudim` (ou um padrão) vira, no compilador, a palavra de textura `bit 31 | linha da
  tabela` (`ud_source`): uma tabela por fonte no buffer de parâmetros (cabeçalho com o BC5, depois os índices bindless
  dos tiles, 4 por linha). `mat_vm.glsl mat_sample_udim` acha o tile pela UV, lê no UV dentro dele (as texturas dos tiles
  usam o sampler **clamp**: nada vaza da borda oposta) e reconstrói o B de um BC5. Tile ausente: preto.
- Nos Details do nó: "UDIM: N tiles (1001 - 10xx) BC7 512"; uma imagem `x.1001.png` com irmãos mostra **Usar como UDIM**.
- **Bake por tile**: um material cujo grafo lê UDIM tem um tile para cada tile que as fontes têm (a união; 1001 primeiro).
  Cada variante (a do material, a de uma Material Instance, a de um ator) é **dona** dos seus tiles: o primeiro é a
  própria variante (o slot dela: o que todo outro caminho lê — sombra por raio, prévia, GI pela média), os outros são
  variantes de tile assadas no quadrado `BakePush.uv_off` (o mesmo programa, os mesmos parâmetros). A média da GI é a dos
  tiles todos. `MATF_UDIM` + `anim.w` = a tabela do dono (uma linha por tile: base, normal, emissão).
- `mesh.frag` (e `masked.frag`, a sombra por raio do `rt.glsl`): `udim_tile` pega o tile sob a UV e lê com
  **`textureGrad` nas derivadas da UV inteira** — elas não pulam na borda do tile, então o mip não despenca no menor
  ali (a emenda clássica do `fract()`); o viés de mip do TSR entra multiplicando as derivadas.
- Fora: material animado (flipbook) com UDIM fica num quadrado só; o caminho de pixel dos reflexos/GI por raio e o path
  tracer leem o primeiro tile (a cor média da GI já é de todos).

## Streaming por tile
- O streaming dos materiais (`material_stream.inc`) não toca nos materiais UDIM; cada tile tem o próprio tamanho no
  `material_udim.inc`, com o mesmo job (subida assada em fatias de ~0,4 ms de GPU, compressão BC7 no fim, troca quando o
  quadro terminou; descida copiando os mips menores). Tiles e materiais se revezam no job (`S.ud_turn`).
- **Necessidade por tile**: a varredura do `render.c` (`stream_sweep`), quando o material da malha é UDIM, calcula uma vez
  por malha a caixa dos triângulos de cada tile (`udim_mesh_tiles`, pelo centro UV do triângulo; LOD 0) e, por tile, se
  está no cone da câmera e o tamanho dele na tela → `mat_stream_note_tile`.
- **Só os vistos**: um tile fora de vista por 2 s desce ao mínimo (64², nunca fica sem textura), mesmo sem pressão de
  memória. Os vistos pedem a potência de 2 acima do tamanho na tela; sem caber em 90% do orçamento (ao lado do resto),
  todos os vistos caem pela metade juntos. Subidas em degraus de 4× (64 → 256 → 1024 → 2048). Com o orçamento folgado
  (o projeto comum) os tiles são assados cheios ao carregar.
- **Fontes também**: o tile do `.rudim` sobe à GPU só no tamanho que o bake daquele tile pede (o mip certo do arquivo
  mapeado; `PrefetchVirtualMemory` pede as páginas 2 quadros antes) e volta ao mínimo depois do bake: só os tiles
  assados ficam na memória. O pico da subida conta a fonte no orçamento.
- Instâncias dinâmicas de atores: tiles sempre cheios (como no streaming dos materiais).
- **Um tile só** (2026-11-23): o material não é `MATF_UDIM` e é assado como comum, mas a fonte `.rudim` dele também
  sobe ao tamanho do bake e volta ao mínimo depois (`ud_lone`; `bake_variant` e a subida do streaming dos materiais).
  Antes o bake lia a fonte de 64² (uma textura 4K importada saía borrada). Conferido no `udim_seams` (`M_UDIM_Lone`).
- Debug Viewer (F10) > Texture Streaming: "UDIM: N tiles, M vistos, X MB" e cada tile (`material [1002]`, atual → pedido /
  cheio).

## O que o exportador do Tlender precisa gravar (outro repositório)
O `.tlend` grava a imagem do Image Texture como caminho. Para UDIM:
1. No campo `image` do nó, o caminho com o token do Blender: `//textures/Corpo.<UDIM>.png` (ou `<UVTILE>`), como o
   Blender guarda `Image.source = 'TILED'`. Os arquivos dos tiles ao lado (`Corpo.1001.png`, `Corpo.1002.png`...).
2. Nenhum campo novo é obrigatório: o importador (`import_material`) reconhece o token ou o nome de um tile com irmãos,
   cria `<pasta>/Textures/Corpo.rudim` (BC7/BC5 com mips) e aponta o nó para ele.
3. Opcional, quando o Tlender quiser exportar já comprimido: gravar o próprio `.rudim` no formato acima (versão 1) e pôr
   o caminho dele no `image`; a Engine abre direto.
4. UV como hoje (V para cima do Blender: a Engine vira).

## Testes (área `udim` do `tools/suite_areas.cmake`)
- `udim_format` (`--udim-test`, sem GPU): nomes (`.1001.`, `_1001`, `_u2_v1`, um "rock_2048" não é tile), import de um
  tile e da pasta, tamanho que não é potência de 2, BC7 (PSNR > 34 dB) e BC5 (> 36 dB), mips até 1×1, cabeçalho maior
  abre, versão maior recusada, arquivo cortado recusado.
- `udim_import` (`--udim-import-test`, sem GPU): um OBJ cujo MTL aponta `T_Body.1001.png` (com o 1002 ao lado) → o
  material lê `/Content/Body/Textures/T_Body.rudim` com os 2 tiles (glTF, FBX e .tlend passam pelo mesmo
  `import_material`).
- `udim_seams` (`--scene udim_seams --shot`): o piso de 4 × 2 tiles visto de cima, sem luz, com o gradiente contínuo
  através dos tiles; a captura é lida: nenhum degrau entre pixels vizinhos acima de 6 níveis. Medido: **2** níveis (média
  0,27). Com a leitura ingênua (`texture()` no UV fracionado) o mesmo teste dá **13** no cruzamento dos tiles — o teste
  pega a emenda.
- `udim_stream` (`--scene udim_stream`, orçamento 256 MB): estrada de 20 tiles de 2048 (400 cm cada), câmera a 8 m/s;
  cobra subidas, descidas, nem todos os tiles residentes ao mesmo tempo e o orçamento respeitado.
- `--scene udim`: o exemplo (piso iluminado com cor BC7 e normal BC5, e um letreiro emissivo).

## Medidas (RTX 3060, 2560×1417, 2º monitor, 2026-11-18)
| | antes | depois |
|---|---|---|
| Cena `materials` (nenhum UDIM): GPU mínima / passe principal | 2,444–2,450 / 1,555–1,562 ms | 2,438–2,450 / 1,545–1,555 ms (igual) |
| Cena `udim` (piso de 8 tiles em boa parte da tela): passe principal, só o tile 1001 (`REALENGINE_NO_UDIM=1`) × UDIM | 1,423–1,433 ms | 1,434–1,439 ms (**+0,01 ms**: a linha da tabela e o `textureGrad`) |
| `texture_streaming` (400 materiais 4096, 512 MB) | 343 de 512 MB, 40 subidas / 326 descidas | 343 de 512 MB, 43 / 327 (igual) |

| UDIM | |
|---|---|
| Import (CPU, 16 threads): 8 tiles 512² BC7 / 8 tiles 256² BC5 / 20 tiles 1024² BC7 | 114 / 32 / ~1050 ms; PSNR do BC7 ≥ 54 dB nos tiles de teste |
| Bake ao carregar: material de 8 tiles 1024 / 8 tiles 2048 (emissivo) | 37–48 / 70–78 ms |
| Memória dos tiles na cena de streaming: todos cheios (36 tiles, orçamento grande) | 358 MB |
| ... com o streaming por tile (orçamento 256 MB, câmera a 8 m/s) | 22,8 MB no fim, no máximo 20 dos 36 tiles acima do mínimo; 61 subidas, 57 descidas; pico 256 de 256 MB |
| Fatia de bake de uma subida (GPU, pior) / CPU do streaming (pior) | 1,18 ms / 1,74 ms (o pior: a 1ª subida de uma fonte, cópia do arquivo mapeado) |
| Costura (`udim_seams`): maior degrau entre pixels vizinhos | 2 níveis (média 0,27); leitura ingênua: 13 |

## Falta
- Reflexos/GI por raio e o path tracer leem só o primeiro tile no ponto atingido (a cor média da GI é a de todos).
- Material animado (flipbook) com UDIM: um quadrado só.
- A leitura das fontes usa o arquivo mapeado no quadro (com prefetch 2 quadros antes); uma thread de leitura tiraria o
  pior caso de CPU quando o disco está frio.
- Instâncias dinâmicas de atores com UDIM: todos os tiles cheios (sem streaming, como as dos materiais).
