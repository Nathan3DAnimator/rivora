id: mesh-material-sockets
kind: howto
title: Material padrão e sockets de uma Static Mesh | A Static Mesh's default material and sockets | Material predeterminado y sockets de una Static Mesh
keys: material da malha, mesh material, material padrao, default material, trocar material da malha, change mesh material, salvar material, save material, sempre usar material, always use material, socket, sockets, static mesh socket, socket em malha, socket on mesh, ponto de encaixe, attach point, anexar ao socket, attach to socket, get socket transform, arma na mao, prender objeto
--- pt
```diagram steps
Duplo clique na malha | Material: escolha | Salvar como Material da Malha | Sockets: Adicionar Socket, arraste o gizmo
```
## Material padrão
- Abra a **Static Mesh** (duplo clique no Content Browser). Em **Material**, escolha outro.
- **Salvar como Material da Malha** guarda essa escolha para sempre: arrastada para um nível, a malha vem com ele. Os atores do nível que usavam o material antigo também passam a usar o novo (Ctrl+Z desfaz).

## Sockets
- Em **Sockets**, **Adicionar Socket** cria um na origem da malha. Arraste as setas do gizmo na vista ou digite **Posição** e **Rotação Relativas**; renomeie, escolha uma **Malha de Prévia** (só no editor) ou **Apague**. Tudo fica salvo na hora, num arquivo oculto ao lado da malha.
- No jogo, os sockets funcionam como os de Skeletal Mesh: **Get Socket Location**, **Get Socket Rotation** e **Attach Actor To Socket** pelo nome, no ator com a malha.
--- en
```diagram steps
Double-click the mesh | Material: pick one | Save as the Mesh's Material | Sockets: Add Socket, drag the gizmo
```
## Default material
- Open the **Static Mesh** (double-click it in the Content Browser). Under **Material**, pick another one.
- **Save as the Mesh's Material** keeps that choice for good: dragged into a level, the mesh comes with it. The level's actors that had its old material take the new one too (Ctrl+Z undoes it).

## Sockets
- Under **Sockets**, **Add Socket** makes one at the mesh's origin. Drag the gizmo's arrows in the view or type its **Relative Location** and **Rotation**; rename it, pick a **Preview Mesh** (editor only) or **Delete** it. Everything is saved at once, in a hidden file beside the mesh.
- In the game, the sockets work like a Skeletal Mesh's: **Get Socket Location**, **Get Socket Rotation** and **Attach Actor To Socket** by name, on the actor with the mesh.
--- es
```diagram steps
Doble clic en la malla | Material: elige uno | Guardar como Material de la malla | Sockets: Add Socket, arrastra el gizmo
```
## Material predeterminado
- Abre la **Static Mesh** (doble clic en el Explorador de contenidos). En **Material**, elige otro.
- **Guardar como Material de la malla** conserva esa elección para siempre: al arrastrarla a un nivel, la malla viene con él. Los actores del nivel que tenían su material anterior toman también el nuevo (Ctrl+Z lo deshace).

## Sockets
- En **Sockets**, **Add Socket** crea uno en el origen de la malla. Arrastra las flechas del gizmo en la vista o escribe su **Relative Location** y **Rotation**; renómbralo, elige una **Preview Mesh** (solo en el editor) o **Delete** para borrarlo. Todo se guarda de una vez, en un archivo oculto junto a la malla.
- En el juego, los sockets funcionan como los de una Skeletal Mesh: **Get Socket Location**, **Get Socket Rotation** y **Attach Actor To Socket** por nombre, en el actor con la malla.
