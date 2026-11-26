# Rivora Engine

*(chamada Real Engine até a versão 0.49 / called Real Engine up to 0.49)*

**Uma game engine gratuita e de código aberto, feita para jogos.** Escrita do zero em C11 e Vulkan 1.3, orientada a
dados, com um editor completo: Blueprints visuais, scripts em quatro linguagens, iluminação global em tempo real,
física, animação, áudio, multiplayer com Steam, VR e exportação para Android.

*A free, open source game engine made for games, written from scratch in C11 and Vulkan 1.3. English below.*

- **Site e doações:** https://nathan3dstudio.com
- **Apoiar:** [GitHub Sponsors](https://github.com/sponsors/Nathan3DAnimator) ou a seção de doação do site
- **Downloads:** a página de releases (o editor também se atualiza sozinho, com o pacote assinado)
- **Licença:** MIT (veja `LICENSE`). Os jogos feitos com a Rivora Engine são seus: venda à vontade, sem royalties.

## O que tem

- **Editor**: Outliner, Details, Content Browser, viewport, Play no editor (Alt+P), desfazer, níveis e subníveis,
  World Partition, Landscape e Foliage, Sequencer, temas.
- **Render**: PBR forward com sombras virtuais, RealGI (iluminação global híbrida com ray tracing opcional), reflexos,
  geometria em clusters com LOD na GPU, tesselação de geometria, translúcidos, cabelo em fios, pele, chuva, água e
  oceano, upscaling (TSR, DLSS, FSR, XeSS).
- **Gameplay**: Blueprints (grafo → bytecode, JIT x64), scripts em Real, C#, Python e C, IA (Behavior Tree, NavMesh,
  percepção, multidões, trânsito), física (corpos rígidos, juntas, ragdoll ativo, tecido, corpos moles, veículos),
  saúde, inventário, diálogos, missões, habilidades, HUD, salvar o progresso.
- **Animação**: Skeletal Mesh, AnimGraph, Motion Matching, Motion Warping, montages pareados, traversal, IK Rig,
  detecção automática de rig (Mixamo, Rigify, Auto-Rig Pro, Character Creator, Daz e outros).
- **Importação**: FBX, glTF/GLB, OBJ, `.tlend` e `.rasset` (do Tlender), pastas inteiras com as texturas.
- **Plataformas**: Windows (editor e jogos), Android (APK e AAB), VR por OpenXR (Meta Quest).

## Compilar

Requisitos: Windows 10/11 x64, Visual Studio 2022 (C/C++ e CMake), o Vulkan SDK 1.3 (`glslc`) e Python 3 para as
ferramentas. Uma GPU com Vulkan 1.3 para rodar.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target realengine
build\Release\rivora.exe
```

- Os assets das demos (CC0) são baixados na primeira compilação (`tools/fetch_demo_assets.cmake`).
- Os upscalers dos fabricantes são opcionais: `tools/fetch_upscalers.ps1` baixa o DLSS, o FSR e o XeSS; sem eles fica
  o TSR.
- A suíte de testes: `tools/suite.ps1` (veja `notas-dev/testes.md`).
- O pacote de distribuição: `tools/make_dist.ps1` (`-SignKey` assina o pacote para o atualizador; a chave nunca fica
  no repositório).

As notas de desenvolvimento de cada área estão em `notas-dev/` (o índice está em `RealEngine.md`).

## Avisos

- A Rivora Engine **não é afiliada nem endossada pela Epic Games**. A Unreal Engine é a referência de usabilidade
  (layout, atalhos, nomes de conceitos comuns), mas nenhum código dela foi usado. Nomes de outros produtos citados
  na documentação pertencem aos seus donos e aparecem só para descrever compatibilidade ou comparar recursos.
- O que a engine envia pela internet está em `PRIVACY.md`: sem telemetria. Relatórios de bug só com a sua confirmação.
- As licenças de terceiros estão em `THIRD_PARTY_NOTICES.md` (gerado por `tools/make_notices.py`).

---

## English

**Rivora Engine** is a free, open source game engine written from scratch in C11 and Vulkan 1.3, data-oriented, with a
full editor: visual Blueprints, scripting in four languages, real-time global illumination, physics, animation, audio,
Steam multiplayer, VR and Android export.

- **Website and donations:** https://nathan3dstudio.com
- **Support:** [GitHub Sponsors](https://github.com/sponsors/Nathan3DAnimator) or the website's donation section
- **License:** MIT (see `LICENSE`). Games made with the Rivora Engine are yours: sell them freely, no royalties.
- **Build:** Visual Studio 2022, the Vulkan SDK 1.3 and Python 3; see the commands above.
- **Not affiliated with or endorsed by Epic Games.** Unreal Engine is the reference for usability only; none of its
  code is used.
- **Privacy:** no telemetry; see `PRIVACY.md`. **Third-party licenses:** `THIRD_PARTY_NOTICES.md`.
