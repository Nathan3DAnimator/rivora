id: import-folder
kind: howto
title: Importar uma pasta inteira e achar as texturas sozinho | Import a whole folder and find the textures by itself | Importar una carpeta entera
keys: importar pasta, import folder, arrastar pasta, drag folder, pasta do explorer, explorer folder, subpastas, subfolders, importar tudo, import everything, texturas faltando, missing textures, textura nao encontrada, texture not found, fbx sem textura, fbx without textures, textura rosa, achar texturas, find textures, mapas pbr, pbr maps, basecolor, normal, roughness, orm, substance, quixel, nomenclatura, naming
--- pt
```diagram steps
Arraste a pasta do Explorer | Subpastas espelhadas com os nomes | Modelos importados, imagens e sons copiados | Texturas achadas e ligadas pelo nome
```
## Uma pasta inteira
- Arraste uma **pasta** do Explorer para o **Content Browser**: ela aparece na pasta aberta com todas as **subpastas, na mesma ordem e com os mesmos nomes**.
- Os modelos (**FBX, OBJ, glTF/GLB, .tlend, .rasset**) entram na janela de **Opções de Importação** (use **Importar Tudo**); imagens e sons são copiados para o lugar deles.
- As texturas que um modelo usa de dentro da pasta ficam **onde estavam** (não são copiadas de novo para a pasta Textures do modelo).
- Arquivos de cena de outros programas (**.blend**, .max, .ma) ficam de fora com um aviso: exporte como FBX, glTF ou .tlend.
- Uma pasta só com os tiles de um **UDIM** continua virando um `.rudim`.

## Texturas achadas sozinhas (todos os importadores)
- Se o arquivo aponta uma textura que **não existe** (outro disco, outra pasta, `.tga` em vez de `.png`), a engine procura pelo nome **nas pastas em volta**: a do arquivo e as subpastas, a de cima e as irmãs. A mais perto ganha.
- Material **sem textura** (ou só com a cor): a engine liga os mapas que tiverem o **nome do material** ou o da textura de cor. Por exemplo, `M_Chair` ou `Chair_BaseColor.png` puxam `Chair_Normal`, `Chair_Roughness`, `Chair_Metallic`, `Chair_ORM`, `Chair_Emissive`, `Chair_Opacity`.
- Também reconhece `Albedo`/`Diffuse`, `NormalGL`/`NormalDX` (o verde do DirectX é invertido), `Gloss` (invertido em rugosidade) e `ORM`/`ARM` (verde na rugosidade, azul no metálico). O Relatório de Importação diz o que foi achado.
--- en
```diagram steps
Drag the folder from Explorer | Subfolders mirrored with their names | Models imported, images and sounds copied | Textures found and linked by name
```
## A whole folder
- Drag a **folder** from Explorer into the **Content Browser**: it appears in the open folder with all its **subfolders, in the same order and with the same names**.
- The models (**FBX, OBJ, glTF/GLB, .tlend, .rasset**) go into the **Import Options** window (use **Import All**); images and sounds are copied to their place.
- The textures a model uses from inside the folder stay **where they were** (not copied again into the model's Textures folder).
- Other programs' scene files (**.blend**, .max, .ma) are left out with a notice: export them as FBX, glTF or .tlend.
- A folder of only a **UDIM** set's tiles still becomes one `.rudim`.

## Textures found by themselves (every importer)
- If the file names a texture that **isn't there** (another drive, another folder, a `.tga` instead of a `.png`), the engine looks for it by name **in the folders around**: the file's folder and its subfolders, the one above and its siblings. The nearest wins.
- A material **without textures** (or with only its color): the engine links the maps named after the **material** or its color texture. For example, `M_Chair` or `Chair_BaseColor.png` pull in `Chair_Normal`, `Chair_Roughness`, `Chair_Metallic`, `Chair_ORM`, `Chair_Emissive`, `Chair_Opacity`.
- It also knows `Albedo`/`Diffuse`, `NormalGL`/`NormalDX` (DirectX's green flipped), `Gloss` (inverted into roughness) and `ORM`/`ARM` (green into roughness, blue into metallic). The Import Report says what was found.
--- es
```diagram steps
Arrastra la carpeta desde el Explorador de archivos | Subcarpetas reflejadas con sus nombres | Modelos importados, imágenes y sonidos copiados | Texturas encontradas y enlazadas por el nombre
```
## Una carpeta entera
- Arrastra una **carpeta** del Explorador de archivos al **Explorador de contenidos**: aparece en la carpeta abierta con todas sus **subcarpetas, en el mismo orden y con los mismos nombres**.
- Los modelos (**FBX, OBJ, glTF/GLB, .tlend, .rasset**) van a la ventana **Import Options** (usa **Import All**); las imágenes y los sonidos se copian a su lugar.
- Las texturas que un modelo usa desde dentro de la carpeta se quedan **donde estaban** (no se copian de nuevo a la carpeta Textures del modelo).
- Los archivos de escena de otros programas (**.blend**, .max, .ma) se omiten con un aviso: expórtalos como FBX, glTF o .tlend.
- Una carpeta solo con los mosaicos de un conjunto **UDIM** sigue convirtiéndose en un solo `.rudim`.

## Texturas encontradas por sí solas (todos los importadores)
- Si el archivo nombra una textura que **no está** (otro disco, otra carpeta, un `.tga` en lugar de un `.png`), el motor la busca por el nombre **en las carpetas de alrededor**: la carpeta del archivo y sus subcarpetas, la de arriba y sus hermanas. Gana la más cercana.
- Un material **sin texturas** (o solo con su color): el motor enlaza los mapas con el nombre del **material** o de su textura de color. Por ejemplo, `M_Chair` o `Chair_BaseColor.png` traen `Chair_Normal`, `Chair_Roughness`, `Chair_Metallic`, `Chair_ORM`, `Chair_Emissive`, `Chair_Opacity`.
- También conoce `Albedo`/`Diffuse`, `NormalGL`/`NormalDX` (el verde de DirectX invertido), `Gloss` (invertido a roughness) y `ORM`/`ARM` (verde a roughness, azul a metallic). El Informe de importación dice lo que se encontró.
