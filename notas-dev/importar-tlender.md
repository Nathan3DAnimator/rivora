# Importar do Tlender (`src/tlend_import.c`)

**Conteúdo > Importar** (ou arrastar o arquivo do Explorer) aceita **`.tlend`**: em vez de copiar, ele vira assets
numa pasta com o nome do arquivo e os objetos são colocados no nível onde estavam no Tlender (um passo de Desfazer).
Linha de comando: `realengine --import-tlend <arquivo.tlend>` (para `/Content/Imported`).

## O que vira o quê
| Tlender | Real Engine |
|---|---|
| Malha (sem armature) | **Static Mesh** `.rmesh` por slot de material (`Meshes/SM_<objeto>[_slot]`), ator Static na transformação do objeto |
| Material (nodes) | **Material** `.rmat` refeito nó a nó pelo idname do Blender (os mesmos sockets do Tlender), links e valores; imagens copiadas para `Textures/` |
| Armature + as malhas que o modificador Armature dela deforma | **um Skeletal Mesh por armature** `.rskm` (`SK_<armature>`; ossos com a pose de repouso) e uma **parte por osso e material** de todas as malhas juntas (`SKP_<armature>_<osso>[_<material>].rmesh`, no espaço do osso) |
| Action (F-curves dos ossos) | **Animation Sequence** `.ranim` por action (`Animations/A_<action>`): o rig avaliado quadro a quadro (FK + constraints + IK) e o resultado dos ossos de deformação assado |
| Objeto com armature | Ator com o componente Skeletal Mesh tocando a primeira animação |

## Conversões
- Espaço: metros → centímetros e Y espelhado (`C = diag(100, -100, 100)`, matrizes `C M C⁻¹`, triângulos com a ordem
  invertida). O que olhava para −Y no Blender olha para +Y.
- Ossos: a matriz de repouso do Blender (`vec_roll_to_mat3`: Y da cabeça para a cauda, X/Z pelo roll), pose =
  `pai · (repouso_pai⁻¹ · repouso) · base(loc, quat, escala)`, convertida e decomposta em local por osso.
- Skinning: cada vértice segue o grupo (osso) de maior peso; cada face, o osso da maioria dos cantos. Até 40 partes
  por malha (os ossos com mais faces); os outros vão com o ancestral mais próximo que tem parte. É *skinning
  segmentado*: cada parte é uma instância comum (culling, LOD, sombra e GI por raio) — milhares de personagens.
- UV2 (lightmap e cache de superfície): os triângulos numa grade (dois por célula, com margem).
- Normais suaves por vértice (área das faces).

## Renderer: registro de malhas
O renderer desenha, além das 5 formas, até **250 malhas importadas** (`RE_MAX_MESHES` 255: o culling guarda
malha×LOD em 12 bits e conta 2 × 255 × 4 draws em memória compartilhada; antes 63, num byte). Elas são lidas de `Content/**/*.rmesh` na inicialização e de novo depois de uma importação
(`render_meshes_reload`: espera a GPU, refaz vértices/índices, BLAS e a tabela de malhas). O id de uma malha importada
é o hash do caminho `/Content/...` com o bit alto; o slot nunca é salvo.

## Teste (o personagem "Bonnie" de uma cena do Tlender 0.09, v85)
53 grupos de vértices, 66 ossos, 15 148 vértices, 7 materiais com 25 texturas, 2 actions → 40 partes, 1 Skeletal Mesh,
2 animações, 7 materiais (4 nós sem equivalente pulados), aparece texturizado com sombras e GI.

## Limites / próximos passos
- Skinning suave por pesos (vértices entre dois ossos) e LODs das malhas importadas.
- (Feitos desde então: Spline IK em 2026-11-03; shape keys; drivers, modificadores, luzes e câmeras do .tlend no Q19,
  abaixo.)
- Link ao vivo com o Tlender (o Tlender exportando direto) e o leitor do `.rasset`.

## Rig de controle e constraints (2026-10-31)
Rigs de jogo costumam ter **duas armatures**: a de deformação (os ossos que as malhas seguem, cada um com Copy
Transforms de um osso do rig) e o **rig de controle** (IK de pernas e braços, Child Of, Transformation de foot roll,
limites), que é quem a action anima. O import avalia o rig como o Tlender (`rig_eval.c` copiado e adaptado):
- Por quadro da action: canais das curvas (os não animados ficam com a pose do arquivo), FK com herança total, as
  constraints de cada osso na ordem (espaços World / Pose / Local with Parent / Local, influência por slerp), o IK
  FABRIK com pólo. Ordem dos ossos por dependência (pai, alvos, e o fim de uma cadeia de IK antes de quem lê a
  cadeia); armatures que leem outras depois delas.
- Avaliadas: Copy Location / Rotation / Scale / Transforms (com os modos de mistura), Damped Track, Stretch To,
  Limit Location / Rotation / Scale, Child Of (com o inverso salvo), Transformation, IK. **Não avaliadas** (o osso
  fica como as anteriores deixaram): drivers (as propriedades ficam com o valor do arquivo), Spline IK, Armature,
  Action, Track To, Locked Track, Limit Distance, Maintain Volume. Euler só na ordem XYZ.
- Qual action: a do objeto (linha `anim`: primeira camada) de cada armature que o Skeletal Mesh lê, mais as que
  animam ossos dele pelo nome.
- (Atualizado em 2026-11-03, abaixo: unidades, Standard IK, Spline IK, Stretch To com Keep X/Z.)
- Teste (Delight.tlend do usuário, 2 armatures de 126 e 194 ossos, 249 constraints, 24 malhas): antes 24 Skeletal
  Meshes, 58 de 251 malhas carregadas e 0 animações; agora **1 Skeletal Mesh**, 243 malhas (todas carregadas),
  **1 animação** (37 quadros: quadril a ~98 cm, cabeça e punhos balançando, tornozelos parados no chão pelo IK).
- Uma armature de mais de 128 ossos (`ANIM_MAX_BONES`) que deforma malhas é pulada (aviso no relatório); partes por
  Skeletal Mesh: até 256 (`ANIM_MAX_PARTS`): acima, cada osso fica com o material principal, depois os menores vão
  para o ancestral.

## OBJ (+ MTL) (2026-10-31)
O mesmo caminho do `.tlend`: `obj_import` lê o OBJ para a estrutura em memória (`TFile`) e `tfile_assets` gera os
assets — então o OBJ ganha tudo o que o `.tlend` já tinha (Static Mesh por slot, materiais por nodes, texturas copiadas,
atores no nível, um passo de Desfazer). **Conteúdo > Importar** ou arrastar um `.obj`; linha de comando
`realengine --import <arquivo>` (`--import-tlend` continua). Importadores por extensão: `content_set_importer` guarda
até 8 (`tlend`, `obj`).
- **Objetos**: um por `o`; sem nenhum `o` no arquivo, um por `g`. Um `o`/`g` antes de qualquer face só renomeia.
  Vértices do arquivo são remapeados para cada objeto (só os que ele usa). Polígonos (até 64 cantos), índices
  negativos, `v/vt`, `v//vn`, `v/vt/vn`. Os pivôs ficam na origem (o OBJ não tem transformações), como no Blender.
- **Eixos**: Y para cima → Z do Blender `(x, -z, y)` (rotação: mantém o sentido das faces), depois a conversão do `.tlend`
  (metros → cm).
- **Materiais** (MTL, todos os do `mtllib`): Principled BSDF → Material Output; `Kd` cor base, `map_Kd` Image Texture
  na cor base (as opções `-s 1 1 1`, `-bm`, `-clamp on` antes do nome são puladas; nomes com espaço funcionam), `Ns`
  → rugosidade `1 - sqrt(Ns/1000)` (como o importador do Blender), `Pr` rugosidade, `Pm` metálico, `d` / `Tr` alfa.
  Nós montados com `prop = -1` ficam com as propriedades padrão do nó (o `import_material` passou a respeitar isso).
- **Normais**: as do arquivo (`vn`) são ignoradas; normais suaves calculadas pela área (o mesmo do `.tlend`). Modelos
  de faces planas ficam suavizados — próximo passo: arestas duras pelo ângulo/`s off`.
- **Projeto novo + import** na linha de comando: o projeto abre na hora (antes ficava para o laço e o import caía no
  Content da build); `--import` roda antes do `--save`; falha do import → código de saída 1.
- **Teste** `import_obj` (`tests/data/import/`): dois objetos, quads, índices negativos, MTL com opções e textura com
  espaço no nome → precisa sair "3 meshes, 3 materials (1 textures, 0 nodes skipped) ... 3 actors". Medido: cubo +
  pirâmide no nível vazio 513 fps (1,95 ms; 542 sem eles — ruído e 3 malhas a mais); o teste inteiro (abrir, importar,
1 s de medida) leva 2,8 s.

## glTF 2.0 (.gltf / .glb) (2026-10-31)
`src/import_gltf.inc` (incluído no fim do `tlend_import.c`, para usar a mesma `TFile` e o `tfile_assets`). JSON pelo
`io_json.c`. **Conteúdo > Importar**, arrastar, ou `realengine --import arquivo.glb`.
- **Buffers**: bloco BIN do `.glb`, arquivo `.bin` ao lado (URI com `%20` decodificado) ou data URI base64.
  Accessors float, u8/u16/u32, i8/i16, normalizados, `byteStride`; um accessor sem bufferView vale zeros (sparse: não).
- **Cena**: a `scene` (ou os nós que não são filhos de ninguém); a hierarquia é achatada — cada nó com malha vira um
  objeto na transformação de mundo (`matrix` ou TRS), convertida por `C G C⁻¹` (glTF → Blender `(x, -z, y)`), depois a
  mesma conversão do `.tlend`. Uma malha usada por vários nós vira uma `TMesh` só (os assets saem por objeto). Nomes
  repetidos ganham `_2`, `_3` (os arquivos levam o nome do objeto).
- **Primitivas**: triângulos, strips e fans (pontos/linhas: ignorados); sem `indices`, os vértices em ordem; uma
  primitiva = um slot de material. Os vértices do glTF já vêm separados nas arestas duras, então as normais suaves
  calculadas respeitam as quinas. `TEXCOORD_0` com V invertido (o V do glTF desce).
- **Materiais** (metallic-roughness, como o importador do Blender monta): `baseColorFactor` × `baseColorTexture` (nó
  Mix multiplicar só quando o fator não é branco), alfa da textura no Alpha quando `alphaMode` não é OPAQUE;
  `metallicRoughnessTexture` (Non-Color) → Separate Color: B metálico, G rugosidade (× fator num Math quando ≠ 1);
  `normalTexture` → Normal Map (força = `scale`); emissão (`emissiveFactor` × textura, `KHR_materials_emissive_strength`);
  `KHR_materials_ior`, `_transmission`, `_clearcoat`, `_sheen`, `_specular` (IOR Level = 0,5 × fator). Sampler: clamp/
  mirror/repeat e filtro nearest. Primitivas sem material usam `<arquivo>_Default` (só gravado se alguém usa).
- **Imagens**: arquivo externo é copiado como no `.tlend`; embutida (bufferView ou data URI) é gravada em
  `<pasta>/Textures/<nome>.png|jpg`.
- **Luzes** (`KHR_lights_punctual`): ponto, spot e direcional viram atores de luz Stationary com sombra. As unidades
  do glTF já são as da Engine (candela; lux para a direcional). Direção: o −Z do nó vira o +X da luz (Y do nó → Z
  para cima). Alcance = `range`, ou onde cai abaixo de 0,05 lux (5 a 50 m).
- **Skins → Skeletal Mesh** (`gl_skin`): as juntas viram os ossos (pai = a junta mais próxima acima; ordem pais antes
  dos filhos), o repouso de cada um = inversa da matriz de bind. Como os Skeletal Meshes da Engine são partes rígidas,
  cada face vai para a junta em que a maioria dos cantos mais pesa (JOINTS_0/WEIGHTS_0), uma parte por (junta,
  material), no espaço da junta; acima de 256 partes junta materiais e depois ossos pequenos no pai (como o `.tlend`).
  Normais do arquivo (`NORMAL`) quando há. Uma malha sem skin **filha de uma junta** (chapéu na cabeça, arma na mão)
  vira parte rígida dessa junta. Skin com mais de 128 juntas: a malha entra estática.
- **Animações**: as que movem uma junta (ou um ancestral dela) são assadas a 30 fps direto da hierarquia de nós
  (LINEAR com slerp curto, STEP, CUBICSPLINE por Hermite), cada junta relativa à junta pai; `A_<animação>` (com mais
  de uma skin no arquivo: `A_<skin>_<animação>`). O ator do Skeletal Mesh toca a primeira.
- **Câmeras**: `node.camera` vira um ator Camera no lugar e na direção do nó (o −Z do nó vira o +X da câmera, Y → Z).
  O `yfov` (vertical, radianos) vira o campo de visão horizontal da Engine: 2·atan(tan(yfov/2)·aspectRatio), 16:9
  sem aspectRatio; uma ortográfica vira perspectiva de 60°.
- **Morph targets** (blend shapes): desde 2026-11-08 ficam na malha e valem em tempo de execução (notas-dev/animacao.md
  "Morph targets"): nomes de `extras.targetNames`, `mesh.weights` como pesos padrão; uma malha com morph sem skin vira
  um Skeletal Mesh de um osso. As shape keys do `.tlend` também. O relatório conta os alvos guardados.
- **Ainda não**: KHR_texture_transform, texturas KTX2/WebP (o decodificador WIC não lê KTX2), blend shapes do FBX.
  (Morphs animados, o canal `weights`: viram curvas de morph desde 2026-11-09, animacao.md "Curvas de morph".)
- **Conferido** com a cena de teste do Tlender exportada pelo Blender (pasta `v501`: `scene.glb`, `scene_sep.gltf`,
  `scene_emb.gltf`): os três dão 5 malhas, 4 materiais, 4 texturas, 5 atores, 3 luzes; posição e direção do spot batem
  com o arquivo (−400, 400, 500; pitch −44 / yaw −44 = o −Z do nó). Nível com a cena: 383 fps (2,6 ms, GPU 2,5 ms,
  com 3 luzes com sombra a mais).
- **Testes** `import_glb` / `import_gltf` (`tests/data/import/make_gltf.js` gera os arquivos; o `.glb` tem um corpo com
  skin de 2 juntas, a animação "Bend" e uma malha presa à junta 2). Conferido na tela: o Rig da cena do Blender
  (Root/Spine/Head/Arm) com `Wave`/`Idle` e o chapéu preso na cabeça; o "Bend" dobrando na junta.
- **`--shot arquivo.png`** (`vkctx_request_shot`): a própria engine grava a janela em PNG perto do fim do `--bench`
  (copia a imagem do swapchain, uma espera da GPU, deflate do `re_zlib`). Só a janela da engine, nunca a tela.

## FBX (2026-10-31)
`src/fbx_read.c` (+ `fbx_read.h`): o parser do Tlender (`io_fbx_read.c`) copiado e adaptado — binário 7.x (registros de
32 e 64 bits, arrays com zlib pelo `re_inflate`) e ASCII. `src/import_fbx.inc`: objetos e conexões, templates de
propriedades e a ordem de transformação da Maya (T Roff Rp Rpre R Rpost⁻¹ Rp⁻¹ Soff Sp S Sp⁻¹, a geométrica não herdada)
copiados do `io_fbx_import.c` do Tlender; o resto é da Engine.
- **Eixos e unidade**: GlobalSettings `UpAxis`/`CoordAxis` (com sinais) → Z para cima / X do Blender (Y = Z × X);
  `UnitScaleFactor` (cm por unidade). Matrizes por conjugação `A M Aᵀ` (translação × unidade), como a Unreal faz: um
  arquivo da Maya (Y para cima) entra com transformações limpas; um FBX do Blender em Y-up traz nos atores a rotação e a
  escala 100 que o exportador gravou (a geometria fica certa — igual na Unreal).
- **Malhas estáticas** → `TFile` → `tfile_assets`: uma Static Mesh por slot (os materiais do modelo na ordem das
  conexões, `LayerElementMaterial` por polígono); **vértices separados onde as normais do arquivo diferem** (quinas
  duras continuam duras), UV `ByPolygonVertex`/`IndexToDirect`.
- **Skins** → `import_skin.inc` (a mesma escrita do glTF): um Skeletal Mesh por raiz de esqueleto (os ossos dos
  clusters e os LimbNodes entre eles e a raiz); repouso = `TransformLink`; a malha no bind = `TransformLink × Transform`
  (o Blender grava o `Transform` no espaço do osso e lê assim); cada face no osso que mais pesa na maioria dos cantos;
  malha filha de um osso vira parte rígida dele.
- **Ossos sem escala** (`sk_unscaled`, vale para glTF e FBX): as sequências da Engine guardam rotação e posição; uma
  armature escalada (o FBX do Blender vem com 100) encolhia os filhos — agora repouso e quadros perdem a escala antes do
  `local_xf`, e as posições ficam em cm.
- **Animações**: cada AnimationStack (a primeira camada), curvas de `Lcl Translation/Rotation/Scaling` dos ossos e dos
  ancestrais, assadas a 30 fps pela ordem de transformação (`KTime` 46186158000 ou 141120000/s, `TCDefinition`).
- **Materiais** (Phong/Lambert → Principled): `DiffuseColor` e textura, `Shininess` → rugosidade (1 − √s / 10, como o
  Blender), `Opacity`/`TransparencyFactor`, `ReflectionFactor` → metálico, `NormalMap`/`Bump` → Normal Map, emissão;
  `ShininessExponent` (brilho) invertido na rugosidade. Texturas embutidas (`Video` → `Content`) gravadas em
  `<pasta>/Textures`; as externas por `RelativeFilename`, `FileName`, ao lado do arquivo ou em `<arquivo>.fbm`.
- **Luzes e câmeras** (Models `Light`/`Camera` com o seu NodeAttribute): as luzes do FBX brilham pelo −Y local, as
  câmeras olham pelo +X com Y para cima (o exportador do Blender gira as dele para isso). Luz: `LightType` (0 ponto,
  1 direcional, 2 spot; área e volume viram ponto), `Color`, `Intensity`, `OuterAngle`/`InnerAngle` (cone inteiro
  → metade, o da Engine), `CastShadows`, `FarAttenuationEnd`. A `Intensity` é um percentual e o Blender grava watts
  × 100 (W/m² no sol): vira candela por W·683/4π e lux por W/m²·683, a mesma regra do exportador glTF do Blender.
  Câmera: campo de visão horizontal pela `FocalLength` e `FilmWidth` (polegadas), senão `FieldOfView`.
- **Ainda não**: blend shapes do FBX, animação de objetos que não são ossos, FBX < 7.1.
- **Conferido** com as exportações do Blender dos testes do Tlender (`v503`): `scene.fbx` e `zup.fbx` dão as mesmas
  posições; `rig.fbx` (armature a 100, corpo com skin, chapéu filho do osso, ação) anima certo no editor de animação.
- **Testes** `import_fbx_rig` e `import_fbx_embed` (`tests/data/import/rig.fbx`, `embed.fbx`).
- **Dica de teste**: no Git Bash, argumentos como `/Content/...` viram `C:/Program Files/Git/Content/...` (conversão do
  MSYS) — use PowerShell ou `MSYS_NO_PATHCONV=1` com `--open-asset`.

## Auto Smooth nas malhas estáticas (2026-10-31)
- O caminho comum de malhas estáticas (Tlender e OBJ) calcula a normal de cada canto pelas faces do vértice que giram
  menos de 30° em relação à face dele (o Auto Smooth do Blender): caixas e quinas ficam duras, esferas continuam
  suaves. Antes era sempre suave (um cubo de OBJ parecia "inflado").

## Câmeras, morph targets e luzes/câmeras do FBX (2026-10-31)
- glTF: câmeras (`node.camera`) e morph targets com os pesos padrão; FBX: luzes e câmeras (detalhes nas seções acima).
  O código de direção (de onde olha + para cima → rotator) é um só para os dois: `imp_facing_rotator`.
- Testes: `import_gltf` (o `test.gltf` de `make_gltf.js` ganhou um morph a meio peso e uma câmera: "1 morph targets,
  1 cameras") e `import_fbx_lights` (`lights.fbx`, exportado do Blender 5.0 por `make_fbx_lights.py`: cubo, spot,
  sol e câmera → "2 lights, 1 cameras").
- Conferido: câmera do FBX em (0, 500, 100) com yaw −90 (olha para o cubo), FOV 39,6° (50 mm em 36 mm); spot a 3 m
  com pitch −90, cone 22,5°/30° (60° com 25% de blend); sol pitch −45; câmera do glTF 65,8° (40° de altura em 16:9).
- Custo: só na importação (um passe pelos nós; o morph soma um delta por vértice e target com peso).

## Alfa, dois lados e resolução (2026-11-01)
- Principled BSDF com o **Alpha** ligado ou < 1 → material **Masked** + **Two Sided** (o Blender desenha as faces de
  trás: mechas de cabelo, cílios, folhas). Material com imagens: assado em **2048** (1024 borrava os mapas 4K).
- Conferido com o `Delight.tlend` (243 malhas, 11 materiais, cabelo em .tga com alfa) lado a lado com o Tlender.

## Rig igual ao do Tlender: Standard IK, unidades, Spline IK (2026-11-03, relatório #48)
Relatório: a animação feita no Tlender importada com a Delight saía com o IK dos pés e das mãos errado. Comparado osso a
osso com o Tlender (um dumper ligado aos `.obj` da build do Tlender: `scene_load_file` + `anim_evaluate_scene` +
`scene_evaluate` por quadro, cabeça/cauda/eixo Z de cada osso no mundo) contra a mesma avaliação da Engine:
**antes 128 de 320 ossos > 1 mm, até 51 cm** (joelho, quadril, ombro e cotovelo; a saia inteira); **agora 0 de 320
nos 101 quadros (máx. 0,1 mm)**. Quatro causas, todas do port antigo:
- **Solver**: o IK tem a linha `cik <solver> <loc> <rot> <peso> <peso de orientação>`; 0 é o **Standard IK do Blender**
  (Jacobiano com mínimos quadrados amortecidos, `ik_solver.c` copiado do Tlender sem mudança), 1 o FABRIK (arquivos
  sem a linha). A Engine ignorava a linha e rodava sempre FABRIK, que nem aplica pólo numa cadeia de 1 osso (o quadril e
  o ombro da Delight são IK de 1 osso com pólo). Agora `solve_ik_standard` (do `rig_eval.c`): a árvore no espaço do pai
  da raiz, pólo, influência, alongamento (`boneik`: travas, limites, rigidez, stretch por osso), e o resultado como o
  Blender aplica (pose = pai novo × offset de antes × mudança de base). Sem `use tail`, a cadeia do Standard conta um
  osso a mais (`ik_chain`).
- **Ordem**: o Tlender avalia por **unidades** — um osso só, ou todos os ossos das cadeias de IK/Spline IK que se
  cruzam (o IK de 1 osso do quadril e o de 2 do joelho viram uma unidade); na unidade, FK + constraints de todos (pais
  primeiro), depois os solvers da raiz mais rasa para a mais funda; unidades em ordem topológica (pai, alvo, pólo,
  controles). Antes era osso a osso, e o IK do joelho rodava antes do do quadril terminar.
- **Spline IK** (`cxs`/`cxsn`: modos de escala e os ossos de controle): a curva Bézier pelos controles (alças pelo Y
  de cada um, ou Catmull-Rom), arco-comprimento, cada osso girado do FK para a curva, torção dos controles espalhada.
  Era "não avaliada": a saia (8 cadeias de 6 ossos) ficava no FK.
- **Stretch To** com Keep X / Keep Z (e os limites de volume do `cxb`): só o Swing existia; os ponteiros de esticar de
  braço e perna usam Keep X (erro de 1–3 cm, que puxava a mão).
- Também: um osso conectado ignora a localização dos canais (o `BKE_pchan_to_mat4` do Blender), e a rotação dos
  canais de cada osso fica guardada (a torção do Spline IK).
- Folha de esqueletos (8 quadros, frente e lado; Tlender branco, Engine antes vermelho, depois verde por cima): o
  vermelho com as pernas em zigue-zague e o braço esquerdo dobrado para trás; o verde cobre o branco inteiro. Folha do
  asset (`--anim-sheet .../A_ArmatureAction_001.ranim`, precisa da extensão): antes uma perna na diagonal e o corpo
  inclinado, depois em pé.
- Custo: só no import (unidades montadas uma vez; o Standard IK aloca por cadeia, fora do quadro do jogo). Delight
  (320 ossos, 101 quadros, 8 IK de 500 iterações): import inteiro 5,3 s (antes 5,8 s; o tempo é das texturas e malhas).
- Ainda não: drivers (ficam com o valor salvo; os da Delight são IK/FK constantes), Armature, Action, Track To,
  Locked Track, Limit Distance, Maintain Volume; Euler só XYZ; o polar decomposition do Tlender na mistura por
  influência (aqui slerp + escala linear: igual sem cisalhamento).

## Malhas grandes (2026-11-06)
As malhas importadas passam por `mesh_build.c` (render.md, "Malhas pesadas: fases 0 e 1"):
- acima de 65 536 triângulos: vértices soldados, sem UV2 (sem lightmap nem cache de superfície);
- ordem otimizada e LODs automáticos (simplificação, ou desbaste para cards);
- `.rmesh` v2.

O limite antigo de ~1,33 M triângulos acabou: o novo é 16 M vértices soldados por arquivo.

## De-lighting (2026-11-09, plano de melhorias A9.5e)
Ferramentas > **Tirar a Luz da Textura do Ator Selecionado** (`src/delight.c`): a textura de Base Color (a Image Texture
ligada nela, senão a primeira) de uma malha escaneada sem a luz do dia da foto. As normais da malha (`.rmesh` pelo
`render_mesh_vpath`) desenhadas no espaço UV; harmônicos esféricos de 9 termos ajustados ao brilho linear por mínimos
quadrados e divididos fora (normalizados pela média); o resto da sombra larga (log do brilho borrado com raio 1/8 da
textura, 3 caixas) sai pela metade; matiz mantido. Grava `<textura>_delit.png` (RGB) e troca o nó. Sem malha que cubra
a textura: só a sombra larga. Teste `delight` (`--delight-test`: uma esfera xadrez acesa por um lado: a variação da luz
0,528 → 0,081 em log). **Falta:** de-light na importação (opção), AO pela malha, manter o alfa.

## O material da malha importada (2026-11-10, "as previews têm que mostrar as meshes com as texturas")
- O `.rmesh` não guarda material; a importação punha o material só no ator criado. Agora grava ao lado um oculto
  `.<nome>.rmesh.mat` (o hash do material em hex, `content_mesh_material_set_disk`): o visualizador de Static Mesh usa
  esse material (prévia da GPU com as texturas; antes adivinhava pelo nome ou ficava cinza), o Static Mesh arrastado do
  Content Browser para o nível já vem com ele, e a miniatura (rasterizada na CPU) pinta cada triângulo com a cor média
  do material (`avg_base`; das partes e skins de um Skeletal Mesh também), em vez de cinza.
- Falta: miniaturas com textura de verdade (a prévia da GPU lida de volta para o cache das miniaturas).

## Tlender completo (Q19, 2026-11-18)
Pedido: um `.tlend` importado sem perder nada — modificadores aplicados ou convertidos, todos os nós de shader no grafo
de material (o que não tem equivalente assado na GPU), texturas, drivers e Spline IK, e um relatório do que entrou
igual, foi convertido, assado ou perdido, com o motivo.

### Modificadores (`src/import_mods.inc`)
O objeto lê a pilha (`mods N`, `md <tipo do Blender> <flags> <nome>`, `mp chave=valor;...`) e ela roda em ordem numa
**cópia da malha do objeto** (outros objetos que usam a mesma malha ficam com a deles), no espaço do Blender. Cada
vértice novo é uma soma ponderada dos antigos (`MB`, `mb_vert`): os **grupos de vértices** e as **shape keys** seguem
(o Mirror troca os grupos `.L`/`.R`, `_L`/`_R`, `Left`/`Right` como o Blender).
| Modificador | Na Engine |
|---|---|
| Mirror (eixos, objeto do espelho, merge, UV espelhada e deslocada) | aplicado |
| Array (Fixed Count / Fit Length, deslocamento relativo, constante e por objeto, merge, deslocamento de UV) | aplicado (Fit Curve usa o Count; tampas: perdidas) |
| Subdivision Surface (Catmull-Clark ou Simple, `levels` da viewport, borda suave ou com cantos) | aplicado (UV linear; vincos do arquivo como no Tlender, sem a superfície limite) |
| Solidify (Simple: espessura, offset, borda, Flip, material offset) | aplicado (Even Thickness: casca simples) |
| Weld, Displace (sem textura no Tlender), Mask (grupo), Nonlinear do Tlender (Bend, Twist, Flare, Squash, Sine, Wave) | aplicados |
| Decimate (Collapse) | aplicado pelo meshoptimizer (costuras de UV coerentes); Un-Subdivide/Planar: a malha fica como está |
| Armature | o Skeletal Mesh (igual) |
| Triangulate | igual (a Engine desenha triângulos) |
| Edge Split | o ângulo das arestas duras do objeto (`split_cos`, no lugar dos 30° do Auto Smooth) |
| Weighted Normal | normais pela área (convertido) |
| Cloth | o componente **Tecido** num ator `<obj>_Cloth` com a malha (faixa de cima presa; o Static Mesh escondido) |
| Soft Body | o componente Tecido de tipo **Corpo Mole** no próprio ator (almofada presa embaixo) |
| Collision | o ator já bloqueia (malha complexa); convertido |
| Particle System | o Groom (cabelo, já existia) |
| Delta Mush, Tension | igual no repouso |
| Bevel, Boolean, Remesh, Geometry Nodes, Shrinkwrap, Lattice, Surface/Mesh Deform, Wire, Spline Wrap, Cluster, Soft Mod, Proximity Wrap, Screw | **perdidos** sem a malha avaliada; exatos com ela (abaixo) |
| Mesh Sequence Cache, Liquid, Voxel Fluid, Granular, Grass, Rope, Burn, Wet Layer, Tissue | perdidos, com o equivalente da Engine no motivo |

**Malha avaliada (formato novo, para o Tlender gravar):** um bloco opcional `evalmeshes <n>`, e por objeto
`evalmesh <índice do objeto> <nv> <nf>` seguido das linhas `v` / `f` / `t` de um bloco `mesh` (e o `vgroups` dela): a
pilha inteira **antes do Armature**, como o Tlender mostra. Com ela, todo modificador que não é Armature/Cloth/Soft
Body/Collision/Edge Split vira **Assado** ("na malha avaliada que o Tlender gravou"). Fica antes do `end`; o leitor do
Tlender precisa saltar o bloco (versão 98).

### Nós de shader (`src/import_matconv.inc`, `material_gpu.inc`, `mat_vm.glsl`, `mat_nodes.c`)
O importador mapeia cada socket do Tlender para sockets da Engine (`McMap`) e liga pelos mapas — um nó pode virar
vários. Três destinos:
- **Igual:** os 41 que a Engine já tinha.
- **Assado na GPU** (novos na Engine, só no `material_bake.comp`, sob `MAT_BAKE`: o `mesh.frag` não muda; o resultado
  vai para as texturas do material): **Brick**, **Magic**, **Vector Rotate**, **Blackbody** e as texturas do Tlender
  **Weave**, **Cracks**, **Stones**, **Grains**, **Ocean**, **Fibers** (`shaders/tl/kernel_svm_sim.h`, cópia do Tlender;
  Stones e Grains são o mesmo `NODE_TEX_CRACKS` com as entradas na ordem das células). Também entram no menu Adicionar
  do editor de material. As saídas por instrução subiram de 3 para 8 (`IrIns.out`).
- **Convertido** (reescrita no grafo): Metallic → Principled (Metallic 1); Glass/Refraction → Principled com Transmission 1
  (vira vidro translúcido); Sheen → Principled Sheen 1; Translucent → Subsurface 1; Subsurface Scattering → Principled
  Subsurface (raio, escala, IOR); Hair/Principled Hair (cor direta ou pela melanina) e o Hair Toon do Tlender →
  Principled (Toon: Shading Model Toon); Toon BSDF → Shading Model Toon; Background → Emission; Layer do Tlender → Mix
  Shader; Wet Layer do Tlender → **Wet Surface** da Engine; Wetness → Mix/Math/Map Range; **RGB/Vector/Float Curves →
  Color Ramps** de até 16 paradas escolhidas onde a reta mais erra sobre a tabela da curva do Blender (a mesma do
  Tlender; maior diferença no relatório, 0,002 no teste; RGB aplica a curva combinada antes); Light Path → constantes
  de um raio de câmera; Object Info → constantes (Random pelo nome do material); Particle/Hair/Volume Info →
  constantes; Geometry → saídas do Texture Coordinate (o bake é no espaço UV); Attribute → Vertex Color (ou UV Map
  pelo nome); Normal → constante + Vector Math (Dot); AO → a cor passa e AO 1 (a GI e o contato da Engine em tempo
  real); Vector Displacement → Displacement pela altura; Environment Texture → Image Texture.
- **Perdido** (com o motivo): shaders de volume, o céu do mundo, Bevel (a normal da malha), Wireframe (0) e os
  materiais procedurais do Tlender (Water, Fabric, Soil, Mud, Sand, Snow, Ice, Rock, Bark, Leaf, Lava...: um Principled
  da cor fica no lugar).

**Assado pelo Tlender (formato novo):** depois das linhas de um nó, `bake <saída> <espaço de cor: 0 sRGB, 1 Non-Color>
<arquivo>` (até 4): a saída que o Tlender assou na GPU dele numa imagem (relativa ao .tlend). O nó vira esses Image
Textures. É o caminho para o que depende da malha (AO, Bevel, Pointiness, Wireframe) e para os procedurais do Tlender.

### Texturas
Lista `images` lida (nome → arquivo, espaço de cor, origem): um nó que guarda o **nome** da imagem acha o arquivo;
imagem **gerada** (`generated:<id>:<tipo>:<l>:<a>:<cor>`: Blank, UV Grid, Color Grid) vira PNG em `Textures/`; arquivo
que não está onde o .tlend diz é procurado pelo nome (pasta do .tlend, `textures/`, `Textures/`, `tex/`, `images/`, a
pasta de cima); UDIM (`<UDIM>`/`<UVTILE>`, `.1001.`) vira um `.rudim` com todos os tiles (Q18, juntado no lote 6); sequência/vídeo pelo primeiro quadro; formato que a
Engine não lê (pelo `mat_load_image_rgba`) vira Perdido.

### Drivers (`src/import_drivers.inc`, `src/driver_expr.c`)
O bloco `rig` (a linha de 4 números; a dos ossos tem 15) dá as propriedades (`pr`/`prb`/`prn`) e os drivers (`dr`,
`drb`, `drp`, `dre`, chaves da curva, `dv`/`dvn`/`dvb`/`dvp`, `drg` Generator, `drx`). As expressões são as do Blender
(`driver_expr.c` = `rig_expr.c` do Tlender, mensagens em inglês com tradução). Tipos: Average, Sum, Min, Max,
Expression; variáveis Single Property (canal de osso/objeto como chaveado, propriedade, valor de shape key),
Transform Channel (Transform, Local, Pose, World; Euler XYZ ou quatérnio), Rotational Difference, Distance. Saídas:
canal de osso (sobre o `bone_channels`), influência de constraint (`con<N>` ou `constraints["nome"].influence`; o `cn`
agora é lido), shape key (`key_blocks["nome"].value`, presa na faixa do slider como o Tlender), propriedade
personalizada, canal de objeto (no import). A cada quadro assado: os drivers, o rig, e de novo os que leem o rig
posado (duas passadas). Resultado no relatório: constante → **expressão avaliada** (o valor no asset); variando →
**assado** nas poses e nas **curvas de morph** da animação (`anim_curve_set`). Perdidos: propriedades da cena e o que a
Engine não tem. Use Self e F-modifiers além do Generator: avaliados sem eles.

### Rig
Track To, Locked Track, Limit Distance e Maintain Volume passam a ser avaliados (cópia do `rig_eval.c` do Tlender;
`cxx` up/target Z, `cxl`, `cxv` lidos). Armature e Action continuam fora (Perdido, com o motivo).

### Luzes, câmeras e o resto da cena
Objetos de luz (`o` com potência, cor, raio; linha `light`: tipo, cone, blend, área) viram atores: ponto/spot em
candela (W × 683/4π), sol em lux (W/m² × 683), área como Rect (W × 683/π, largura/altura); câmeras pelo `lens` num
sensor de 36 mm. Empty: igual (sem ator). Curva e volume: Perdido com o motivo.

### Relatório de conversão (`src/import_report.c`, `src/import_report_ui.c`)
Cada importador (.tlend, OBJ, glTF, FBX) começa um relatório; os itens iguais se juntam (contagem). No fim:
`<projeto>/Saved/ImportReports/<nome do arquivo>.txt` (no idioma do editor; desde 24/11/2026 fora do Content, onde o
`ImportReport.txt` aparecia no Content Browser: teste `import_report_saved`) e a linha do import ganha `; report: N same, N converted, N baked, N
lost`; os perdidos também vão para o log. **Janela > Relatório de Importação** (como o Message Log da Unreal): filtros
por resultado, busca, Mostrar o Arquivo do Relatório; abre sozinha quando algo se perdeu. `--import-budget <ms>` falha o
import que passar do tempo (os testes).

### Testes e medidas
- `import_tlend_q19` (`tests/data/import/q19_full.tlend`): Mirror + Array + Solidify + Bevel num plano, Cloth numa
  cópia, Subdivision + Displace + Decimate + Edge Split num cubo, Brick/RGB Curves/Light Path/Blackbody/Mix/AO/Volume num
  material e Stones/UV Grid gerada/imagem faltando/Metallic/Attribute no outro, rig com Limit Rotation e action,
  driver de shape key por osso (curva de morph), driver de influência por propriedade, driver da cena, luz e câmera →
  `9 same, 16 converted, 5 baked, 4 lost`, import em 44 ms (orçamento 4000).
- `import_tlend_q19_baked` (`make_q19_baked.py`): a malha avaliada de um Boolean e dois nós assados pelo Tlender →
  `4 same, 0 converted, 4 baked, 0 lost`, 28 ms.
- Custo por modificador no teste < 0,1 ms; Subdivision nível 2 de um cubo 0,1 ms; Decimate 192 → 96 triângulos 0,1 ms.
- Delight.tlend (usuário): mesmos números de antes (5 malhas, 11 materiais, 36 texturas, 1 Skeletal Mesh, 1 animação,
  2 morphs) + relatório (395 iguais, 27 convertidos: os drivers de IK/FK constantes; 6 perdidos: drivers de shape keys
  dos widgets dos ossos); import 5,2 s (antes 5,3 s); modificadores 10 ms. O driver da shape key "Hide" dá −1 e fica
  em 0 pela faixa do slider (sem isso o vestido saía por cima do corpo).
- Nada roda por quadro: o custo é só do import; os nós novos existem só no shader de bake (o `mesh.frag` é o mesmo).

### O exportador do Tlender e o que a Engine faz com ele (2026-11-19)
O Tlender ganhou **Arquivo > Exportar > Real Engine (.tlend)** (main do Tlender 21bc0153, `src/tlend_engine.c`, nota
`../notas-dev/tlend-para-a-real-engine.md`). Ctrl+S e o autosave não gravam nada disto.
- **O que o exportador grava:**
  - `evalmeshes`: os objetos com Bevel, Boolean, Remesh, Geometry Nodes, Shrinkwrap, Lattice, deformadores etc., e a
    Subdivision quando a malha tem vincos.
  - As linhas `bake`, com os arquivos em `<nome>_assados/<material>_<nó>_<canal>.png`.
- **Material próprio do Tlender** (TlShaderNode* com saída de shader): vem em 3 ou 4 mapas, todos na saída 0.
  - Os mapas, pelo fim do nome: `_basecolor` (sRGB), `_roughness`, `_normal` (espaço tangente, OpenGL) e `_emission`
    (sRGB) ou `_metallic`.
  - O importador monta um **Principled BSDF** com eles (`tlend_import.c`, bloco do `nbake`). A normal entra por um
    **Normal Map** (Tangente, OpenGL). A emissão entra com Emission Strength 1 (o PNG é de 8 bits, preso em 1).
- **Bevel**: a normal assada (`_normal`) também passa por um Normal Map.
- **AO e Wireframe**: um Image Texture por saída, como antes.
- **Vincos** (`ecrease N` + trios `a b valor`, v29): agora são lidos para a `TMesh` (`crease_e`/`crease_s`).
  - A regra é a do Tlender e do OpenSubdiv: nitidez = vinco² × 10 (10 é infinito), 1 a menos por nível.
  - A aresta semi-nítida mistura o ponto suave e o do meio.
  - O vértice segue a regra Smooth (0–1 vincos), Crease (2) ou Corner (mais); quando a regra muda nas filhas, mistura
    as duas pela nitidez média das arestas fracionárias.
  - As arestas filhas herdam o vinco. Use Creases desligado ignora os vincos.
  - Conferido com o cubo de `tlender_tests tlend-engine`. O ponto da aresta com vinco sai em (−1, 0, −1); projetado na
    superfície limite dá (−0,852, 0, −0,852), o mesmo da malha avaliada do Tlender. Sem vinco sairia em −0,75.
  - **Ainda diferente**: o Tlender leva todos os vértices à superfície limite (Use Limit Surface, o padrão). A Engine
    para no último nível, uma diferença média de 0,15 num cubo de 2 m em um nível.
- **Arestas sharp** (`h` com pares no `shade`, v18): também lidas para a `TMesh` e passadas às arestas filhas da
  Subdivision.
  - Nas normais (Auto Smooth), o canto de um vértice com aresta sharp só soma as faces que alcança em volta sem
    atravessar uma delas (`sharp_fan`), como as normais divididas do Tlender e do Blender.
- Outro modificador que renumera os vértices (Mirror, Array, Solidify...) descarta as marcas.
- **Testes** (área importar-tlender):
  - `import_tlend_engine`: o arquivo do teste do Tlender (`tests/data/import/q19_tlender.tlend` + os assados reduzidos
    a 100 KB). Esperado: 7 same, 2 converted, 4 baked, 0 lost.
  - `import_tlend_creases`: o mesmo sem as malhas avaliadas (como o Ctrl+S grava). Esperado: 8 same, 4 converted,
    2 baked, 1 lost; o Bevel fica perdido sem a malha avaliada.

### Falta
Do lado da Engine: drivers ao vivo no jogo (expressões de morph avaliadas a cada quadro com outras animações),
constraints Armature e Action. (O leitor do `.rasset` entrou no #164, abaixo.)

## Rigs rígidos, controles do rig, texturas 4K e o leitor do `.rasset` (2026-11-22, relatório #164)
O usuário importou `Capsula.tlend` (uma cápsula com rig do Auto-Rig Pro) e não ganhou Skeletal Mesh, a textura 4K
UDIM saiu borrada e o `.rasset` exportado pelo Tlender não abria.
- **Peça presa a osso** (Parent > Bone, sem modificador Armature: robôs, máquinas, a Capsula): a linha `parent` do
  .tlend traz o tipo (1 = osso) e o nome do osso, antes ignorados. `obj_world` agora põe o objeto no osso posado, na
  cauda (como o Blender e o `scene_parent_world` do Tlender); `rides_bone` acha o osso subindo pelos pais (peça sobre
  peça), `sk_source`/`sk_owner` fazem da malha uma **parte do Skeletal Mesh** da armature com todos os vértices em peso 1
  no osso (na pose de repouso: a pose do arquivo descontada). As actions da armature movem as peças.
- **O que não aparece no jogo fica de fora** (`mark_hidden`): objetos só em coleções excluídas ou desligadas no render
  (câmera do Outliner, os pais contam) e as formas de osso (`bsobj`: os `cs_*` do Auto-Rig Pro, widgets do Rigify).
  Antes viravam 121 Static Meshes soltas na Capsula. Uma linha no relatório conta quantas.
- **Resolução do bake pelo tamanho da textura**: um material com imagens era assado sempre em 2048; agora na potência
  de 2 da maior imagem que lê (cabeçalho do PNG/JPEG, maior tile do `.rudim`), de 2048 a 4096 (`image_side`).
- **FBX com textura em outro drive**: o Blender grava `RelativeFilename` absoluto (`F:\...`) quando a textura está em
  outro disco; o importador grudava ele na pasta do FBX. Agora usa o caminho inteiro, e o `FileName` quando o relativo
  não existe.
- **Leitor do `.rasset`** (`src/import_rasset.inc`): os registros do `io_rasset.h` do Tlender copiados com prefixo Ra,
  validados como o `rasset_view` dele (offsets, índices, tamanhos; registro maior de versão nova lido pelo próprio tamanho,
  bloco desconhecido pulado) e convertidos para o mesmo `TFile` do .tlend: malhas soldadas pelo vértice de origem do
  Tlender (`source`; sem ele, pela posição e o skin), arestas duras onde as normais do arquivo se separam (sharp edges,
  ângulo só acima de 90°), malha com skin no espaço do esqueleto pelo `bind` e grupos de vértice por osso, material
  Principled pelos termos PBR com as imagens gravadas em `Textures/` (canal por Separate Color, Mapping quando a
  textura é deslocada, Normal Map), esqueleto como armature (repouso pelo inverso do `inv_bind`: head, tail e roll),
  animação amostrada como action (um key linear por amostra, a base relativa ao repouso, quaternions no mesmo
  hemisfério), morph targets como shape keys, luzes e câmeras. Entra por Importar, arrastar o arquivo, ou arrastar um
  `.rasset` já no Content para o viewport.
- Pesos de morph animados (`MWGT`) viram **curvas de morph** da animação (`TAction.mcurves`, um key por amostra,
  amostradas no `import_skeletal` como as dos drivers). O nó com o olho fechado (flag hidden) entra, como no .tlend.
- Fora (no relatório): trilhas de objeto que se movem, menos as de malhas com skin (o movimento vem dos ossos).
- **Do lado do Tlender** (`tlender/src/io_rasset.c`, nota `../notas-dev/formato-rasset.md`): a peça presa a osso
  (direto ou por objetos comuns acima) sai como **pele de peso 1** no osso, com o `bind` da peça com o osso em repouso;
  malhas desligadas no render e formas de osso (widgets) ficam fora do arquivo. Teste `tlender_tests rasset`
  (`ra_rigid`: duas peças, uma sobre a outra, e um widget; a pele comparada com o Tlender em três quadros; 179
  verificações). Antes a `Capsula.rasset` do usuário virava 4 Static Meshes e um esqueleto sem pele.
- **Conjunto de texturas "Real Engine (empacotado)"** do Tlender (preset `realengine` do `texexport`: `_BaseColor`,
  `_NormalRoughnessMetallic`, `_Emissive`): Criar > Material a partir do `_BaseColor` acha as outras duas na mesma pasta
  e liga rugosidade (B), metálico (A), normal (X e Y, o Z refeito por `sqrt(1 - x² - y²)` num Normal Map) e emissão
  (`content_create.c`, conferido no `asset_picker`).
- O `Exportar > Real Engine (.tlend)` (`tlend_engine.c`: malhas avaliadas e nós assados) segue coberto por
  `import_tlend_engine` e `import_tlend_creases`, sem mudança.
- **Medidas** (i7-11700F, RTX 3060): `Capsula.tlend` lida em 77 ms, 1 Skeletal Mesh em vez de 0 (+121 malhas a menos);
  bake do material 4096 em 720 ms (antes 2048 em 295 ms, em segundo plano); `Capsula.rasset` (59 MB, 4 malhas, 5
  texturas 4K) importada em 1,25 s, o material 4096 assado em 2,5 s em segundo plano; `skin.rasset` do teste em 31 ms.
  Ida e volta com o exportador corrigido (`Capsula.tlend` → `.rasset` de 60 MB, 1 malha, 250 quadros → Engine): **1
  Skeletal Mesh**, 1 ator, material 4096 (assado em 162 ms), 0 perdidos, importada em 0,99 s.
- **Testes** (área importar-tlender): `import_tlend_bone_parent` (`tests/data/import/bone_parent.tlend`: "Piece" no
  osso "Arm" com a action "Spin", um widget `bsobj` e uma malha numa coleção desligada: 1 Skeletal Mesh, 1 animação, 2
  de fora) e `import_rasset` (`tests/data/import/skin.rasset`, escrito por `make_rasset.py`: tira com skin soldada de 7
  para 6 vértices, rig de 2 ossos com a animação "Bend", morph "Bulge" com o peso animado de 0 a 1 (curva na animação),
  cubo de arestas duras, PNG embutido, luz e câmera).

## Capsula de novo: UDIM de um tile e Relative Parenting (2026-11-23)
O usuário importou `Capsula.tlend` no 0.46: a textura 4K saiu borrada e o `Capsula.rasset` "não ficou igual" ao .tlend.
- **UDIM de um tile só** (só o `UV_*.1001.png`, o `<UDIM>` do Substance/Tlender): o `.rudim` sobe à GPU em 64² e só
  cresce quando um bake pede; com um tile o material não ganha `MATF_UDIM` (`ud_on` pede mais de um) e era assado como
  comum, lendo a fonte de 64² — 4K assado de uma fonte de 64². Agora `ud_lone` dá o tile e o `bake_variant` (e a subida
  do streaming dos materiais, `st_pick`/`stream_step`) sobem a fonte ao tamanho do bake e voltam ao mínimo depois, como
  no caminho de vários tiles (notas-dev/udim.md). Teste: `udim_seams` assa também `M_UDIM_Lone` (um tile de 512, assado
  em 1024) e confere a fonte lida (`MatStats.udim_lone_src`: 512; antes 64).
- **Relative Parenting** (`boneinh`, 4º campo; o `c_traj` do Auto-Rig Pro): a peça presa ao osso segue a deformação dele
  (`pose × rest⁻¹`, o `bone_deform_matrix` do Tlender), sem ir à cauda. O importador ignorava a linha e punha a Capsula
  49 cm (o comprimento do `c_traj`) fora do lugar; o `.rasset` (que leva o mundo avaliado do Tlender) já estava certo.
  Teste `import_tlend_bone_relative` (`bone_relative.tlend` = `bone_parent.tlend` com Relative Parenting no "Arm": a
  peça em repouso a 30 cm, não 200 cm acima na cauda). O import do .tlend agora registra os limites da pele do Skeletal
  Mesh em repouso ("Skeletal Mesh SK_x: N bones, M skins, rest bounds ...").
- O `Capsula.rasset` da pasta do usuário é de 21/11, anterior à correção do exportador (Tlender 0.44): vira 52 Static
  Meshes (com os `cs_*`) e nenhum Skeletal Mesh — precisa ser exportado de novo. Exportado pelo Tlender atual: 1 Skeletal
  Mesh, a mesma malha (9949 vértices) e, com as correções acima, no mesmo lugar e com a mesma nitidez que o .tlend.
- **Medidas** (i7-11700F, RTX 3060): bake do `M_Capsula` (4096) 64,7 → 180 ms (agora lê as fontes 4K: 4 × 21 MB a mais na
  GPU só durante o bake); importação inteira igual (3,5 s, as 5 conversões UDIM dominam); 335 fps no close da Capsula.

## Texturas em volta e pasta inteira (2026-11-23, pedidos do usuário)
"FBX procurar as texturas nas pastas em volta" e "arrastar uma pasta do Explorer e importar tudo, com as subpastas em
ordem com os nomes, ajustando texturas e materiais".
- **Texturas em volta** (`import_matconv.inc`, vale para todos os importadores: o `mc_image` é comum): quando o caminho
  do arquivo não existe, `mc_find_near` procura num índice das imagens em volta (`mc_near_index`: a pasta do arquivo e
  as subpastas até 4 níveis, a pasta de cima e as irmãs até 3; 20 mil arquivos no máximo; refeito a cada importação no
  `tfile_assets`) pelo nome sem diferenciar maiúsculas e pela mesma raiz com outra extensão (`.tga` no arquivo, `.png`
  ao lado); a mais perto ganha. Corrigido junto: o nome do arquivo achado era um ponteiro para o caminho antigo.
- **Mapas pelo nome** (`mc_auto_maps`, depois da conversão do material): as entradas livres do Principled BSDF ganham
  os mapas cujo nome é o do material (sem `M_`/`MI_`/`MAT_`, só letras e números) ou o da textura de cor sem o final,
  mais um final conhecido: cor (BaseColor, Albedo, Diffuse, Color...), normal (NormalGL, Normal, Nrm; NormalDX com a
  convenção DirectX), Roughness, Glossiness (Invert), Metallic, ORM/ARM (Separate Color: verde na rugosidade, azul no
  metálico), Emissive (força ≥ 1) e Opacity (Masked, dois lados); resolução e números no fim aceitos (`_2k`, `4096`).
- **Pasta inteira** (`content.c content_import_folder`; soltar uma pasta no Content Browser, `--import-folder` nos
  testes): espelhada na pasta aberta (subpastas com os nomes); modelos na fila da janela de opções com a pasta de cada
  um (a fila ganhou pasta por arquivo, 256 itens); imagens e sons copiados (sons pelo importador de áudio); UDIM como
  antes; `.blend`/`.max`/`.ma`/`.mb`/`.c4d` avisados como não suportados. As texturas de dentro da pasta usadas por um
  modelo ficam no lugar espelhado (`tlend_import_set_mirror`, por thread: o trabalho da importação guarda o espelho de
  quando entrou na fila), não copiadas de novo para `<modelo>/Textures`.
- **Testes** `import_near` (OBJ cuja textura é um `.tga` de outro disco achada como `.png` numa pasta irmã + `M_Barrel`
  com cor, normal e ORM pelo nome: 4 texturas) e `import_folder` (7 pastas, 2 modelos, 5 texturas, 1 som, 1 `.blend`
  avisado; os materiais apontam para os arquivos espelhados).

