id: texture-settings
kind: howto
title: Configurações da textura: sRGB, Linear, Normal Map e qualidade | Texture settings: sRGB, Linear, Normal Map and quality | Ajustes de textura: sRGB, Linear, Normal Map y calidad
keys: configuracoes da textura, texture settings, srgb, linear, non-color, nao cor, nao-cor, espaco de cor, color space, normal map, roughness errado, roughness wrong, metallic cinza, textura lavada, washed out texture, qualidade da textura, texture quality, tamanho maximo, max size, compressao, compression, bc7, sem compressao, uncompressed, filtro, filter, pixel art, closest, nearest, repeticao, wrap, tile, mirror, memoria de textura, texture memory
--- pt
```diagram steps
Duplo clique na textura | Configurações da Textura | Espaço de Cor, Tamanho Máximo, Compressão, Filtro, Repetição | Aplicar aos Materiais
```
## Pelo nome, sozinho
Toda textura já chega organizada pelo **nome**: `_Roughness`, `_Metallic`, `_ORM`, `_AO`, `_Height` e `_Opacity` ficam **Linear (Não-Cor)**; `_Normal` vira **Normal Map**; `_BaseColor`, `_Albedo`, `_Emissive` e nomes sem final conhecido ficam **sRGB**. Os nós Image Texture criados com ela (arrastando, ligando pelo nome ou importando) já usam isso.

## Mudar
Abra a textura (duplo clique no Content Browser). Em **Configurações da Textura**:
- **Espaço de Cor**: sRGB para cores, Linear para dados (o material lê como número), Normal Map para normais.
- **Tamanho Máximo**: o maior lado com que os materiais a leem. Menor gasta menos memória e o bake fica mais rápido.
- **Compressão**: BC7 (um quarto da memória) ou Sem Compressão (a mais nítida, 4× a memória).
- **Filtro**: Linear suaviza; Closest deixa os pixels duros (pixel art). **Repetição**: Repeat, Extend, Clip, Mirror.
- A mudança fica salva ao lado da textura. **Aplicar aos Materiais** põe espaço de cor, filtro e repetição em todos os materiais do projeto que a usam. A memória que ela ocupa aparece embaixo.
--- en
```diagram steps
Double-click the texture | Texture Settings | Color Space, Max Size, Compression, Filter, Wrap | Apply to Materials
```
## By its name, by itself
Every texture comes in already organized by its **name**: `_Roughness`, `_Metallic`, `_ORM`, `_AO`, `_Height` and `_Opacity` are **Linear (Non-Color)**; `_Normal` is a **Normal Map**; `_BaseColor`, `_Albedo`, `_Emissive` and names without a known ending are **sRGB**. The Image Texture nodes made with it (dragging, linking by name or importing) use that already.

## Change them
Open the texture (double-click it in the Content Browser). Under **Texture Settings**:
- **Color Space**: sRGB for colors, Linear for data (the material reads it as numbers), Normal Map for normals.
- **Max Size**: the largest side the materials read it at. Smaller uses less memory and bakes faster.
- **Compression**: BC7 (a quarter of the memory) or Uncompressed (the sharpest, 4× the memory).
- **Filter**: Linear smooths; Closest keeps the pixels hard (pixel art). **Wrap**: Repeat, Extend, Clip, Mirror.
- The change is saved beside the texture. **Apply to Materials** sets the color space, filter and wrap in every material of the project that uses it. The memory it takes shows below.
--- es
```diagram steps
Doble clic en la textura | Texture Settings | Color Space, Max Size, Compression, Filter, Wrap | Apply to Materials
```
## Por su nombre, solas
Toda textura llega ya organizada por su **nombre**: `_Roughness`, `_Metallic`, `_ORM`, `_AO`, `_Height` y `_Opacity` son **Linear (Non-Color)**; `_Normal` es un **Normal Map**; `_BaseColor`, `_Albedo`, `_Emissive` y los nombres sin una terminación conocida son **sRGB**. Los nodos Image Texture creados con ella (arrastrando, enlazando por nombre o importando) ya usan eso.

## Cámbialas
Abre la textura (doble clic en el Explorador de contenidos). En **Texture Settings**:
- **Color Space**: sRGB para colores, Linear para datos (el material la lee como números), Normal Map para normales.
- **Max Size**: el lado más grande con el que los materiales la leen. Menor usa menos memoria y se precalcula más rápido.
- **Compression**: BC7 (una cuarta parte de la memoria) o Uncompressed (la más nítida, 4× la memoria).
- **Filter**: Linear suaviza; Closest mantiene los píxeles duros (pixel art). **Wrap**: Repeat, Extend, Clip, Mirror.
- El cambio se guarda junto a la textura. **Apply to Materials** ajusta el espacio de color, el filtro y el wrap en todos los materiales del proyecto que la usan. La memoria que ocupa se muestra abajo.
