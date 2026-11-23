id: asset-picker
kind: howto
title: Seletor de assets e Criar a partir da seleção | Asset picker and Create from the selection
keys: seletor de assets, seletor, asset picker, escolher asset, escolher modelo, escolher malha, escolher material, escolher textura, escolher classe pai, pick parent class, classe pai, parent class, pick asset, campo de asset, prévia, previa, miniatura, miniaturas, thumbnail, só prévia, so previa, só nome, so nome, prévia e nome, tamanho da prévia, pequeno, médio, grande, criar a partir da seleção, create from selection, clique direito, right click, criar ik rig, create ik rig, criar physics asset, criar ragdoll, criar anim blueprint, criar material instance, material instance, instância de material, instancia de material, criar material da textura, light function da textura, criar montage, criar blend space, gerar lods, generate lods, blueprint da malha, ator com física, tecido da malha, veículo da malha
--- pt
A **janela de seleção de asset** (como o Asset Picker da Unreal) abre em todo campo de asset do **Details** (Static Mesh, Material, Light Function, Skeletal Mesh, Anim Class, Particle System, Vehicle, Decal) e em toda criação que precisa de um asset de base: **Adicionar > Classe Blueprint...** (Escolher Classe Pai), **Animação > Animation Blueprint... / Anim Montage... / Blend Space...**, **IK Rig e Física > IK Rig... / Physics Asset... / Ragdoll...**.
- **Buscar**: cada palavra no nome ou na pasta ("mann sk" acha SK_Mannequin). **Pasta**: só a pasta e as de dentro. **Tipos**: um clique mostra só aquele tipo, Ctrl+clique liga ou desliga.
- O **olho** no canto: **Só prévia**, **Só nome** (lista com tipo e pasta) ou **Prévia e nome**; tamanho **Pequeno / Médio / Grande**. Fica lembrado nas preferências.
- Duplo clique, Enter ou **Selecionar** escolhe; **Nenhum** esvazia o campo; Esc cancela. Arrastar um asset do Content Browser para o campo também vale.
- **Clique direito num asset > Criar**: Skeletal Mesh → IK Rig, Physics Asset, Ragdoll, Anim Blueprint, Esqueleto; Static Mesh → Classe Blueprint, Ator com Física, Gerar LODs, Tecido, Veículo; Material → **Material Instance**; Textura → Material, Light Function; Animação → Anim Montage, Blend Space. O asset novo nasce na mesma pasta, já renomeando.
- A Material Instance não está mais no menu Adicionar: ela nasce do clique direito no material.

```diagram steps
Clique direito no asset | Criar | IK Rig (ou Material Instance...) | digite o nome | o editor abre
```
```diagram bars ms
Ler 1031 assets: 2.0
Abrir (ler + 1º quadro): 3.7
Um quadro aberto (CPU): 0.6
```
--- en
The **asset picker** (like Unreal's Asset Picker) opens on every asset field of the **Details** (Static Mesh, Material, Light Function, Skeletal Mesh, Anim Class, Particle System, Vehicle, Decal) and on every creation that needs a base asset: **Add > Blueprint Class...** (Pick Parent Class), **Animation > Animation Blueprint... / Anim Montage... / Blend Space...**, **IK Rig and Physics > IK Rig... / Physics Asset... / Ragdoll...**.
- **Search**: every word in the name or the folder ("mann sk" finds SK_Mannequin). **Folder**: only that folder and the ones under it. **Types**: a click shows only that type, Ctrl+click turns one on or off.
- The **eye** in the corner: **Preview only**, **Name only** (a list with the type and folder) or **Preview and name**; size **Small / Medium / Large**. It is kept in the preferences.
- Double-click, Enter or **Select** picks; **None** empties the field; Esc cancels. Dragging an asset from the Content Browser onto the field works too.
- **Right click an asset > Create**: Skeletal Mesh → IK Rig, Physics Asset, Ragdoll, Anim Blueprint, Skeleton; Static Mesh → Blueprint Class, Physics Actor, Generate LODs, Cloth, Vehicle; Material → **Material Instance**; Texture → Material, Light Function; Animation → Anim Montage, Blend Space. The new asset is made in the same folder, being renamed.
- Material Instance is no longer in the Add menu: it is made from the material's right click.

```diagram steps
Right click the asset | Create | IK Rig (or Material Instance...) | type its name | its editor opens
```
```diagram bars ms
Reading 1031 assets: 2.0
Opening (read + first frame): 3.7
A frame while open (CPU): 0.6
```
