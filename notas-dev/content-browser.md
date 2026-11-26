# Content Browser / Content Drawer

`src/content.c` (painel), `src/ui_icons.c` (ícones desenhados com a draw list, sem fonte de ícones nem textura).

## O que tem
- **Gaveta** (Ctrl+Space ou o botão na barra de status): sobe sobre o viewport, ganha o foco ao abrir e some quando
  o foco vai para outro lugar (como na Unreal); Esc ou X fecham. **Dock in Layout** prende o painel embaixo do
  viewport (altura ajustável pela divisória); **Undock** volta à gaveta. Window > Content Browser (docked).
- **Raiz**: `/Content` = pasta `Content` ao lado do executável (até existir o sistema de projeto) e `/Engine/BasicShapes`
  (Cube, Sphere, Cylinder, Cone, Plane: só leitura).
- Árvore de pastas (abre o caminho até a pasta atual), voltar/avançar (Alt+←/→, Backspace), breadcrumb clicável,
  busca recursiva (pasta atual e abaixo), tamanho dos tiles (64–192).
- Tiles com ícone por tipo, barra de cor do tipo (cores da Unreal), nome cortado com "...", tipo embaixo, tooltip com
  caminho, tamanho e data. Seleção: clique, Ctrl (alternar), Shift (intervalo), Ctrl+A, clique no fundo limpa.
- Operações: New Folder (já entra renomeando), Rename (F2 / duplo clique no nome), Delete (Del, confirma, vai para a
  Lixeira), Show in Explorer, Copy Path, Refresh. Import: diálogo do Windows ou **arrastar arquivos do Explorer**
  para a janela (copia para a pasta atual; nomes repetidos viram "Nome (2)").
- Arrastar: tile → pasta (tile, árvore ou breadcrumb) **move** (os selecionados juntos); BasicShape → **viewport**
  cria um ator Static Mesh onde o raio do mouse encontra o chão (grade de 10 cm), nome único (Cube, Cube2...).
- Tipos por extensão: `.rasset`, `.rlevel`, imagens (Texture Source), fbx/obj/gltf/glb/usd (Mesh Source), wav/ogg,
  Blueprints (`.rbp`, `.rstruct`, `.renum`, `.rinterface`, `.rdata`: [blueprints.md](blueprints.md)).
- Classes Blueprint: miniatura da classe (`bp_preview.c`, LOD 2, a câmera de 3/4 da Unreal) com o selo do tipo no canto;
  sem malha/luz/Character, o ícone do tipo. O asset é lido uma vez (`Item.bp_state`: um arquivo ilegível não é relido a
  cada frame). Clique direito: Editar..., **Criar Classe Blueprint Filha**. `content_browse_to` (o Localizar do editor de
  Blueprints) abre a pasta com o asset selecionado.

## Desempenho
- A pasta só é lida quando muda algo: abrir, navegar, buscar, operar, Refresh, a janela voltar ao foco, arquivos
  soltos. Nada de varrer disco por frame. Itens (até 4096), pastas (até 1024) e a seleção ficam numa reserva única.
- Só as linhas de tiles visíveis são desenhadas (recorte manual por scroll).
- Editor com a gaveta aberta: ~3850 fps (0,26 ms; CPU 0,17 ms) contra ~4400 fechada (RTX 3060, 2560x1417).

## Falta
- Importar de verdade (converter para `.rasset`). (As miniaturas reais já existem, abaixo; desde o Q23 seguem o arquivo
  quando ele muda no disco.)
- Coleções, renomear pela árvore, desfazer operações de arquivo. (Favoritos e filtros por tipo: abaixo, 24/11/2026.)

## Miniaturas e visualizadores (2026-10-31, `src/asset_viewer.c`)
- **Miniaturas no Content Drawer/Browser** (`asset_thumb`, chamada no bloco quando não é Blueprint):
  - **Texturas** (png, jpg, bmp, tif, gif pelo WIC; .hdr e .rudim desde o Q23): decodificadas numa **thread de trabalho** (prioridade baixa,
    estados por atômicos, o WIC com COM próprio), reduzidas a 128 px por média de caixa; a thread principal sobe a
    imagem (64 KB) quando o bloco é desenhado. Xadrez sob a imagem (o alfa).
  - **Static Mesh** e **Skeletal Mesh**: **rasterizadas uma vez na CPU** (256 px com depth buffer, luz principal + céu,
    vista da frente-esquerda de cima, ortográfica, metade = bordas suaves) a partir da cópia em CPU do renderer
    (`render_mesh_cpu`; o SK com as partes no bind, `anim_part_rows`). Uma por quadro (a thread principal é a dona da
    cópia em CPU). Formas básicas: vértices já em cm.
  - **Materiais**: a esfera da prévia (`mat_sphere_draw`, extraída do Material Editor) com a cor base assada, 24 x 12
    quads, desenhada a cada quadro.
  - Até 1024 miniaturas guardadas na sessão; liberação de imagens adiada 4 quadros (o quadro em voo ainda as desenha).
- **Visualizador de textura** (duplo clique numa imagem; `--open-asset`): zoom pela roda (no cursor), arrastar move,
  Ajustar / 1:1, canais R G B A (só A = alfa em cinza), o pixel sob o mouse (coordenada e RGBA), tamanho, KB no disco,
  se tem alfa. Imagens acima de 4096 mostradas reduzidas.
- **Visualizador de Static Mesh** (.rmesh): girar arrastando, zoom pela roda, F enquadra, grade; triângulos, vértices
  (o intervalo de índices), tamanho em cm.
- **Medido** (editor 1440p): gaveta com 5 materiais +0,13 ms de CPU por quadro (0,03 ms por esfera); com 8 assets da
  pasta do manequim (SK pronto) +0,04 ms; GPU igual (a imagem é uma textura de 128 px). Rasterizar uma miniatura de
  malha: uma vez por asset por sessão.

## Prévia das animações e renomear ao criar (2026-11-01)
- **Animation Sequence**: a miniatura é o Skeletal Mesh do esqueleto dela posado a 40% do clipe; com o ponteiro em
  cima, o clipe toca no tile (24 quadros rasterizados na CPU sob demanda, no máximo 4 ms por frame de UI, liberados
  0,25 s depois que o ponteiro sai). O enquadramento é o mesmo em todos os quadros (bounds do bind + caminho da raiz).
  Teste: `--browse <animação> --hover`.
- **Criar e renomear**: o asset novo entra com o nome editável e o editor só abre depois do Enter (ou de sair do campo).
  O campo pede o foco até o ImGui ativá-lo (um frame depois); antes isso encerrava o renomear na hora.
- O pool de texturas do ImGui cobre o limite de miniaturas (`UI_TEXTURES_MAX`): uma pasta de 243 malhas derrubava a 0.10_b.

## Import Options e menu da Unreal 5 (2026-11-02)
- Arquivo convertido (.tlend, OBJ, glTF, FBX) por Importar ou arrastar: janela **Import Options** (`content_import_options`,
  desenhada todo frame no editor). Só assets por padrão; "Colocar no Level" põe os atores. Fila de até 16 arquivos,
  Importar / Importar Todos / Cancelar. `--import` na linha de comando continua colocando no nível.
- Clique direito no fundo: Nova Pasta, Importar para <pasta>, **Create Basic Asset** (Blueprint Class, Level, Material)
  e **Create Advanced Asset** (Animation, Artificial Intelligence, Blueprint, Input, Material, User Interface,
  Miscellaneous). `bp_editor_add_menu`/`mat_editor_add_menu` recebem a parte do menu.

## Seletor de assets e Criar a partir da seleção (2026-11-17, Q6)
Pedido: "as opções de criação (Ragdoll, IK) pedem o nome do modelo direto no menu; as opções devem ser únicas e abrir
uma janela para escolher o modelo, a textura, o material, a blueprint, com prioridade para prévias e nomes".
- **Seletor** (`src/asset_picker.c/.h`, como o Asset Picker / Pick Parent Class da Unreal): janela modal com busca (cada
  palavra no nome ou na pasta), pasta (a pasta e as de dentro), botões de tipo (clique: só ele; de novo: todos; Ctrl:
  liga/desliga) e, no canto, o **olho**: Só prévia / Só nome / Prévia e nome e o tamanho Pequeno / Médio / Grande
  (`picker_view`, `picker_size` no editor.ini). Duplo clique, Enter ou Selecionar escolhem; Nenhum (campos que podem
  ficar vazios); Esc cancela. Miniaturas: as do Content Browser (`content_thumb`: classe do Blueprint, textura, malha,
  esfera do material, onda do som; a animação toca com o ponteiro em cima).
- Candidatos: os arquivos de `/Content` dos tipos pedidos, lidos **uma vez ao abrir** (reserva única de 8192 entradas,
  sem malloc depois) + os que quem abre acrescenta (`asset_picker_add`: classes nativas, formas básicas, os Light
  Function do registro). A lista visível só é refeita quando a busca, os tipos ou a pasta mudam; só as linhas visíveis
  são desenhadas.
- **Campos do Details** (`asset_field`: miniatura + nome + lupa; soltar um asset do Content Browser também vale):
  Static Mesh (formas básicas primeiro), Material, Light Function, Decal Material, Particle System, Vehicle, Skeletal
  Mesh da multidão e do componente, Anim Class.
- **Criações pelo seletor** (uma opção cada, sem listar os assets no menu): Classe Blueprint... (Escolher Classe Pai: as
  comuns primeiro, todas as nativas com a descrição, os Blueprints do projeto), Animation Blueprint..., Anim Montage...,
  Blend Space..., IK Rig..., Physics Asset..., Ragdoll... (`content_pick_and_make`).
- **Criar a partir da seleção** (`src/content_create.c`, clique direito num asset > Criar; `content_make` faz, o teste
  também chama): Skeletal Mesh → IK Rig, Physics Asset, Ragdoll (Blueprint Actor com o Skeletal Mesh, o Physics Asset
  e Simulate desde o início; o Physics Asset é feito se não houver), Anim Blueprint, Esqueleto (abre o editor: o
  esqueleto mora no `.rskm`); Static Mesh → Classe Blueprint (a malha e o material dela), Ator com Física (+ Simulate
  Physics), Gerar LODs (só sem LODs, sem cores/morphs/pele; as malhas relidas), Tecido e Veículo (a malha posta 4 m à
  frente da câmera e o Make Cloth / Make Vehicle (carro) do editor, pelo `ContentEditorHooks`); Material → Material
  Instance; Textura → Material (Image Texture → Base Color) e Light Function (domínio do Q4); Animação → Anim Montage e
  Blend Space (um Anim Blueprint com um estado Blend Space 1D pela Speed, a animação como 1ª amostra). O asset nasce na
  pasta do original, renomeando, e abre depois do nome (como Adicionar).
- O menu Adicionar > Material não lista mais os materiais para a instância: só "Material"; a instância nasce do
  clique direito no material.

### Medidas (RTX 3060, editor 1440p, `--picker-test`)
- Abrir com **1031 assets** (1000 texturas + o projeto): leitura **2,0 ms**, leitura + 1º quadro **3,7 ms**.
- Um quadro com o seletor aberto: **0,79 ms** de CPU enquanto as miniaturas chegam, **0,62 ms** depois (a maior parte
  são as esferas dos materiais visíveis, 0,03 ms cada); fechado: nada (só um `if`).
- Teste `asset_picker` (área `content-browser`, no kit rápido): abre, busca (nome; pasta + nome), as três vistas e os três
  tamanhos (Só nome: 1 por linha; Pequeno 12 por linha contra 5 no Grande; Prévia e nome mais alto que Só prévia), a
  vista relida do editor.ini, escolhe um asset, cria IK Rig, Material Instance, Material e Light Function pelo clique
  direito e um IK Rig pelo Adicionar > IK Rig... com o seletor.

### Falta
- Skeletal Mesh → Tecido e Veículo: o tecido precisa da parte (saia, capa) e o veículo das peças; ficam no Static Mesh
  e no Ator > Criar Tecido/Veículo a partir da Seleção. Blend Space como asset próprio (hoje é um estado do Anim
  Blueprint). Sound Wave → Sound Cue.
- Os outros combos de asset fora do Details do nível (editores de partícula, áudio, IA, Sequencer: ~80) seguem como
  listas com miniatura (`asset_selectable`); trocar aos poucos por `asset_field`.

## Filtros como os da Unreal (2026-11-24)
- Botão **Filtros** à esquerda da busca (`filters_button` em `src/content.c`): **Outros Filtros** (Favoritos, Abertos
  Recentemente, Modificados, Em Uso no Nível), **Tipos de Asset** (os 13 comuns) e **Mais Tipos** (os outros 14: todo
  `AssetKind` cai em algum), Limpar Filtros. Tabela `g_cbf` (`CbFilterId` em `content.h`): rótulo, ícone, família da
  cor, máscara de `AssetKind`. Widget Blueprint = classe cuja raiz é `BPN_USER_WIDGET` (lida pelo `item_bp` só com o
  filtro ligado).
- **Chips** embaixo da busca (`filter_chips`), na cor da família (`ui_asset_family_token`): clique liga/desliga, clique
  direito remove; quebram linha. Tooltip padrão (`ui_tip_item`; descrições `cb_filters`, `cbf_<chave>`).
- **Combinação**: tipos ligados em OU; Outros Filtros e a busca em E. Com filtro ligado a leitura é recursiva (pasta
  atual e as de dentro) e sem pastas, como a busca. Tudo na leitura da pasta (`filter_pass` em `walk_one`), nunca por
  quadro; `filters_prepare` junta uma vez por leitura o que os especiais pedem.
- **Favoritos** (até 256, clique direito > Adicionar aos Favoritos, estrela no bloco), **Recentes** (os últimos 32
  abertos pelo Content Browser), **Modificados** (Blueprint `dirty`, nível com mudanças pelo gancho `unsaved`, Git
  novo/alterado), **Em Uso no Nível** (gancho `used_assets` do editor: hashes de malha, material, classe, animação, IK
  Rig, Physics Asset, som, IA, partículas, veículo, decal, material físico, diálogo, item; busca binária).
- Salvos no `editor.ini` do usuário: `cb_filters=<adicionados>,<ligados>` (hex), `cb_fav=`, `cb_recent=`.
- **Medido** (`--picker-test`, RTX 3060, pasta com 1009 assets): ler sem filtro **1,04 ms**, com o filtro Textura
  **0,86 ms** (menos itens guardados); desenhar os chips: só texto e retângulos. Teste: estágio 6 do `asset_picker`
  (Textura 1003 de 1009, Textura + Material 1006, + busca 100, Favoritos 2, Favoritos + Textura 1, sem pastas com filtro,
  relidos do editor.ini).
- Falta: coleções da Unreal, filtros salvos com nome, favoritos por projeto (hoje por usuário, pelo vpath).
