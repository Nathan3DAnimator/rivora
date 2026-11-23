id: texture-viewer
kind: howto
title: Ver texturas e malhas (o visualizador) | Viewing textures and meshes (the viewer)
keys: visualizador de textura, texture viewer, texture editor, editor de textura, abrir textura, open texture, ver textura, canais, channels, canal alfa, alpha channel, mip, mips, nivel de mip, mip level, mipmap, hdr, .hdr, radiance, hdri, exposicao, exposure, exr, openexr, .exr nao abre, exr doesnt open, tiff, tif, gif, udim, rudim, miniatura, thumbnail, miniatura desatualizada, thumbnail not updating, miniatura nao atualiza, textura mudou, pintei a textura, photoshop, substance, reimportar textura, vertices, quantos vertices, vertex count, triangulos, triangle count, static mesh viewer, visualizador de malha, ver malha, textura no se actualiza, ver textura hdr
--- pt
```diagram steps
Duplo clique na textura (Navegador de Conteúdo) | Canais R G B A | Nível de Mip: como fica de longe | .hdr: Exposição HDR | Salvou de novo em outro programa: a janela e a miniatura acompanham
```
## Abrir
**Duplo clique** numa textura do Navegador de Conteúdo abre o visualizador (como o Texture Editor da Unreal, mais simples). Ele lê PNG, JPEG, TGA, BMP, TIFF, GIF, DDS, **.hdr** (Radiance, os HDRIs) e **.rudim** (o primeiro tile de uma textura UDIM). **Roda**: zoom no cursor; **arrastar**: mover; **Ajustar** e **1:1** voltam.

## Ver os canais e os mips
- **R G B A**: liga e desliga cada canal; só o **A** mostra o alfa em cinza (branco = opaco).
- **Nível de Mip**: a imagem reduzida como a GPU lê de longe (o tamanho aparece ao lado). Serve para ver se uma linha fina some ou se a textura borra demais na distância.
- O valor do pixel sob o cursor aparece no canto (R, G, B, A).

## Imagens .hdr
Um .hdr guarda luz acima de 1. A **Exposição HDR** clareia ou escurece só a visualização (+1 dobra) para ver o sol ou as sombras; o material lê a imagem em 0 EV. **OpenEXR (.exr)** ainda não é lido: a janela diz isso; salve como .hdr ou PNG/TGA.

## Editou fora da engine?
Pintou de novo no Photoshop, Krita ou Substance e salvou por cima: **a miniatura do Navegador e o visualizador aberto acompanham sozinhos** em menos de um segundo, sem reiniciar o editor (a engine olha a data dos arquivos numa thread de fundo, sem custo no quadro). Vale também para malhas exportadas de novo.

## Malhas (Static Mesh)
O duplo clique numa malha abre o viewport do asset ([o viewport](article:asset-viewport)) com **triângulos e vértices**: o número de vértices é o dos que a malha usa, cada um uma vez.
--- en
```diagram steps
Double-click the texture (Content Browser) | Channels R G B A | Mip Level: how it looks far away | .hdr: HDR Exposure | Saved again in another program: the window and the thumbnail follow
```
## Open
**Double-click** a texture in the Content Browser to open the viewer (like Unreal's Texture Editor, simpler). It reads PNG, JPEG, TGA, BMP, TIFF, GIF, DDS, **.hdr** (Radiance, the HDRIs) and **.rudim** (a UDIM texture's first tile). **Wheel**: zoom at the cursor; **drag**: pan; **Fit** and **1:1** reset.

## See the channels and the mips
- **R G B A**: turns each channel on and off; **A** alone shows the alpha in gray (white = opaque).
- **Mip Level**: the image reduced as the GPU reads it far away (its size beside it). Use it to see whether a thin line vanishes or the texture blurs too much at a distance.
- The pixel under the cursor shows in the corner (R, G, B, A).

## .hdr images
A .hdr holds light above 1. **HDR Exposure** brightens or darkens the view only (+1 doubles) to see the sun or the shadows; a material reads the image at 0 EV. **OpenEXR (.exr)** isn't read yet: the window says so; save it as .hdr or PNG/TGA.

## Edited outside the engine?
Painted again in Photoshop, Krita or Substance and saved over it: **the Content Browser's thumbnail and the open viewer follow by themselves** in under a second, without restarting the editor (the engine checks the files' dates on a background thread, at no cost to the frame). Meshes exported again too.

## Meshes (Static Mesh)
Double-clicking a mesh opens the asset viewport ([the viewport](article:asset-viewport)) with its **triangles and vertices**: the vertex count is the vertices the mesh uses, each once.
