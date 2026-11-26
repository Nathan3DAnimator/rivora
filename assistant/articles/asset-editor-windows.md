id: asset-editor-windows
kind: howto
title: Janela Editores e janelas soltas | The Editors window and floating windows | La ventana Editores y las ventanas flotantes
keys: editores, editors, janela editores, editors window, abas, tabs, abas de editores, editor tabs, lado a lado, side by side, dois editores, two editors, material e grafo, material and graph, janela solta, floating window, janela flutuante, janela própria, window of its own, outro monitor, second monitor, janela some, window disappears, janela atrás, window behind, janela some ao clicar, sempre na frente, always on top, barra de tarefas, taskbar, trocar de projeto, switch project, abrir de novo, reopen, editores abertos, open editors, ventana editores, pestañas, ventana flotante, otro monitor, lado a lado
--- pt
```diagram steps
Abra um asset (clique duplo no Navegador de Conteúdo) | Ele abre como aba da janela Editores | Abra outro, de qualquer tipo: vira outra aba | Arraste uma aba para fora: vira uma janela própria
```
## A janela Editores
Como na Unreal, todo editor de asset abre como uma **aba** da mesma janela, a **Editores**: um Grafo, um Material, o visualizador de Static Mesh e de Textura, o editor de Skeletal Mesh e o de Sistema de Partículas. Abrir um segundo asset, de qualquer tipo, põe a aba dele ao lado das outras: um Grafo e um Material, por exemplo, ficam na mesma janela, um do lado do outro.

- Cada material abre na **sua própria aba** (até 4 ao mesmo tempo); abrir de novo um que já está aberto só traz a aba dele para a frente.
- A janela Editores aparece com o primeiro editor e some quando a última aba sai dela.
- Na primeira vez ela ocupa o lugar do viewport, com os painéis do editor em volta.

## Arrumar as abas
- Arraste uma **aba** para outro ponto da janela Editores: os alvos aparecem e a aba se divide ao lado da outra (Grafo à esquerda, Material à direita).
- Arraste a aba para **fora do editor**: ela vira uma **janela própria do Windows** (outro monitor, por exemplo). Dá para acoplar outras abas nela de novo.
- Os painéis de dentro de cada editor (Detalhes, Meu Blueprint, Paleta...) continuam acopláveis dentro dele.

## As janelas soltas
Uma janela arrastada para fora do editor (um editor, um painel, a própria janela Editores) **pertence à janela do editor**:
- fica **sempre acima dela**: clicar no editor não esconde mais o Material que estava do lado;
- não ganha botão próprio na barra de tarefas (o da Rivora Engine vale por todas);
- minimiza e volta com o editor;
- **fecha junto** quando o editor fecha ou quando você troca de projeto.

Elas são desenhadas em SDR e sem esperar o monitor: três janelas soltas custam menos de 1 ms ao quadro do editor.

## Trocar de projeto e abrir de novo
Ao trocar de projeto, os editores do projeto anterior fecham (o aviso de salvar aparece se algo não foi salvo) e nenhuma janela solta sobra. Os editores que estavam abertos ficam guardados no layout do projeto (`Saved/Layout.rlayout`) e **abrem de novo** quando você volta a ele.
--- en
```diagram steps
Open an asset (double-click it in the Content Browser) | It opens as a tab of the Editors window | Open another, of any kind: another tab | Drag a tab out: it becomes a window of its own
```
## The Editors window
As in Unreal, every asset editor opens as a **tab** of the same window, the **Editors** window: a Graph, a Material, the Static Mesh and Texture viewer, the Skeletal Mesh editor and the Particle System editor. Opening a second asset, of any kind, puts its tab beside the others: a Graph and a Material, for instance, sit in the same window, side by side.

- Each material opens in **its own tab** (up to 4 at a time); opening one that is already open just brings its tab to the front.
- The Editors window shows with the first editor and goes when the last tab leaves it.
- The first time, it takes the viewport's place, with the editor's panels around it.

## Arranging the tabs
- Drag a **tab** to another spot of the Editors window: the targets show and the tab splits beside the other one (the Graph on the left, the Material on the right).
- Drag the tab **out of the editor**: it becomes a **real Windows window** (on another monitor, for instance). Other tabs dock into it again.
- The panels inside each editor (Details, My Blueprint, Palette...) stay dockable inside it.

## The floating windows
A window dragged out of the editor (an editor, a panel, the Editors window itself) **belongs to the editor's window**:
- it stays **always above it**: a click on the editor no longer hides the Material beside it;
- it gets no taskbar button of its own (Rivora Engine's stands for all);
- it minimizes and comes back with the editor;
- it **closes with it** when the editor closes or when you switch projects.

They are drawn in SDR without waiting for the monitor: three floating windows cost the editor's frame under 1 ms.

## Switching projects and opening again
When you switch projects, the previous project's editors close (the save prompt shows if something is unsaved) and no floating window is left behind. The editors that were open are kept in the project's layout (`Saved/Layout.rlayout`) and **open again** when you come back to it.
--- es
```diagram steps
Abre un asset (doble clic en el Explorador de contenidos) | Se abre como pestaña de la ventana Editores | Abre otro, de cualquier tipo: otra pestaña | Arrastra una pestaña afuera: se convierte en una ventana propia
```
## La ventana Editores
Como en Unreal, cada editor de asset se abre como una **pestaña** de la misma ventana, la ventana **Editores**: un Grafo, un Material, el visor de Static Mesh y de Textura, el editor de Skeletal Mesh y el de Sistema de partículas. Abrir un segundo asset, de cualquier tipo, pone su pestaña junto a las demás: un Grafo y un Material, por ejemplo, quedan en la misma ventana, uno al lado del otro.

- Cada material se abre en **su propia pestaña** (hasta 4 a la vez); abrir uno que ya está abierto solo trae su pestaña al frente.
- La ventana Editores aparece con el primer editor y se va cuando la última pestaña sale de ella.
- La primera vez ocupa el lugar de la ventana gráfica, con los paneles del editor alrededor.

## Ordenar las pestañas
- Arrastra una **pestaña** a otro punto de la ventana Editores: aparecen los destinos y la pestaña se divide junto a la otra (el Grafo a la izquierda, el Material a la derecha).
- Arrastra la pestaña **fuera del editor**: se convierte en una **ventana real de Windows** (en otro monitor, por ejemplo). Otras pestañas se pueden acoplar en ella de nuevo.
- Los paneles de dentro de cada editor (Detalles, My Blueprint, Paleta...) siguen siendo acoplables dentro de él.

## Las ventanas flotantes
Una ventana arrastrada fuera del editor (un editor, un panel, la propia ventana Editores) **pertenece a la ventana del editor**:
- queda **siempre por encima de ella**: un clic en el editor ya no esconde el Material que estaba al lado;
- no tiene botón propio en la barra de tareas (el de Rivora Engine vale por todas);
- se minimiza y vuelve con el editor;
- **se cierra con él** cuando el editor se cierra o cuando cambias de proyecto.

Se dibujan en SDR sin esperar al monitor: tres ventanas flotantes cuestan menos de 1 ms al cuadro del editor.

## Cambiar de proyecto y volver a abrir
Al cambiar de proyecto, los editores del proyecto anterior se cierran (aparece el aviso de guardar si algo no se guardó) y no queda ninguna ventana flotante. Los editores que estaban abiertos se guardan en el layout del proyecto (`Saved/Layout.rlayout`) y **se abren de nuevo** cuando vuelves a él.
