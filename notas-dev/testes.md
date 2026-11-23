# Testes: a suíte e o kit rápido

A suíte (`ctest`, ~200 testes) leva ~20 min em série; quase todo teste com janela custa ~2 s só para abrir o editor
(Vulkan com validação, malhas, cena) mais os segundos do `--bench`. Para o dia a dia existe o **kit rápido**: pelo
menos um teste por área, o mais barato que passa pelo caminho principal dela.

## Como rodar (`tools/suite.ps1`, sempre escondido: `REALENGINE_BACKGROUND=1`)
| Comando | O quê |
|---|---|
| `tools/suite.ps1` | o kit rápido (etiqueta `quick`): todas as áreas |
| `tools/suite.ps1 -Areas fisica,ia` | o kit + as áreas inteiras que a mudança tocou |
| `tools/suite.ps1 -Areas ia -Somente` | só essas áreas |
| `tools/suite.ps1 -Completa` | a suíte inteira (antes de build/release) |
| `tools/suite.ps1 -Falhas` | só o que falhou na última execução (`--rerun-failed`) |
| `tools/suite.ps1 -Listar` | cada área, quantos testes tem e quais estão no kit |

Opções: `-Parar` (para na 1ª falha), `-Build build-release`, `-Jobs 8`, `-Visivel` (janelas no 2º monitor, `-Monitor 2` por padrão: `REALENGINE_MONITOR`, o tamanho do primário e sem foco; `-Monitor 0` volta ao primário).
O log inteiro vai para `<build>/Testing/suite.log`; na tela só o resumo, a lista de falhas e as linhas `FAIL`/`VUID`.
Avisa quando um `realengine.exe` de fora da build (o editor instalado) está aberto: a GPU dividida dá falhas falsas
de fps. Direto pelo ctest: `ctest -C Release -L quick -j 8` (as etiquetas são regex: `-L "^(quick|fisica)$"`).

Fluxo sugerido: mudou uma área → `-Areas <área>`; antes do commit → o kit; antes de build/release → `-Completa`.

## Como funciona (`tools/suite_areas.cmake`, incluído no fim do `CMakeLists.txt`)
- **Áreas = etiquetas (LABELS)** com o nome da nota em `notas-dev/` (`fisica`, `render`, `regras-de-jogo`...). Um teste
  pode ter mais de uma (`gpu_marks` é `servicos` e `tecido`; `ai_squad_mm` é `ia` e `animacao`).
- **Kit**: a lista `RE_QUICK`. Os testes de preparação (fixtures) vêm sozinhos: `game_fps` traz `game_package`, os
  testes de projeto trazem `projects_clean`, o `assistant` traz o `assistant_pack`.
- **Trava da GPU**: todo teste que abre janela ganha `RESOURCE_LOCK gpu`; os sem janela (lista `RE_CPU`, etiqueta
  `cpu`: testes de unidade e `--*-test`) rodam ao lado. Assim `-j 8` nunca põe duas janelas na GPU ao mesmo tempo
  (fps e cintilação medem o mesmo que sozinhos) e vale também para a suíte completa.
- **Checagem `suite_areas`** (no kit): falha se algum teste não tem área ou se uma área não tem teste no kit.

## Teste novo
Coloque o nome na linha `re_area(<área> ...)` da área em `tools/suite_areas.cmake`. Entra em `RE_QUICK` só se a área
não tiver nada mais rápido que cubra o mesmo caminho; em `RE_CPU` se não abre janela. Área nova: uma linha `re_area`
com o nome da nota nova e pelo menos um teste no kit.

## Escolhas do kit (o que ficou de fora fica para `-Areas`/`-Completa`)
- Por área, o teste mais barato com validação que passa pelo caminho principal: `gi_house`, `spec_leak`,
  `render_style_photoreal` (denoiser e RT do Fotorrealista), `materials`, `rt_clusters`, `budget_house`,
  `constraints`, `ai`, `crowd`, `anim_foot_ik`, `import_gltf`, `demo_fighting`, `project_third_person`...
- Áreas com um teste só entram inteiras (os módulos R7, `world_partition`, `async`, `umg`, `scripts`, `proto`).
- Fora do kit: variações de escalabilidade (`*_low`, `*_epic`), estresses longos (`physics_stress*`, `mm_*`,
  `particles_5m`, `crowd_20000`, `scale_rt`), rede com sessões (`net_session*`, `pie_multiplayer`: o kit tem o
  protocolo), `cinematics` (42 s), `game_first_run` (45 s), `update_download` (internet), `hang_watchdog` (20 s).

## Medidas
- Suíte completa (2026-11-10, série, RTX 3060): 202 testes, 1179 s de testes (~20 min).
- Kit rápido: 66 testes + 2 de preparação + `suite_areas` (69), 51 áreas. Pelos tempos da suíte acima somam **237 s
  (~4 min, 5× menos)**; os sem janela (~25, < 1 s cada) correm ao lado.
- 1ª execução do kit (2026-11-10): 6:05, com o editor instalado aberto usando a GPU a 99%: 19 falhas, todas de tempo
  (fps abaixo do limite, um "hitch" no `world_partition`, o `async` sem terminar a tempo), as mesmas que passaram na
  suíte das 12:41 com a GPU livre. A trava funcionou: só um teste com janela por vez durante a execução toda.
- 2ª execução (2026-11-10, GPU livre): **4:05 (245 s)**, 67 de 69 passaram; a falha era a desta nota sem artigo no
  Assistente (`assistant_pack`, e o `assistant` que depende dele não rodou). Corrigido com o artigo `test-suite`; os
  dois passam (4,3 s).
- `-Listar`: 2,8 s (uma chamada `ctest --show-only=json-v1`).
