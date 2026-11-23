id: window-layouts
kind: howto
title: Janelas modulares e layouts | Modular windows and layouts
keys: maximizar, maximize, maximizar janela, maximize window, clique duplo, duplo clique, double click, double-click, restaurar janela, restore window, modo janela, janela, janelas, window, windows, painel, painéis, paineis, panel, panels, acoplar, encaixar, dock, docking, desacoplar, undock, arrastar janela, drag window, layout, layouts, salvar layout, save layout, carregar layout, load layout, layout padrão, default layout, resetar layout, reset layout, outro monitor, second monitor, níveis ao lado, levels, organizador, outliner, detalhes, details, maximizar, maximize, fechar janela, close window, ventana, ventanas, acoplar ventana, diseño, guardar diseño, monitor secundario, panel
--- pt
```diagram steps
Arraste a aba de um painel | Os alvos aparecem: centro, lados, bordas e os vãos entre painéis | Solte num alvo: o painel encaixa ali | Janela > Layout > Salvar Layout Como
```
## Mover os painéis
Cada painel do editor (Posicionar Atores, Organizador, Níveis, Detalhes e o Navegador de Conteúdo acoplado) é uma janela. Arraste a **aba** dele para outro lugar:
- enquanto arrasta, **todos os alvos aparecem**: o centro e os 4 lados de cada painel, as 4 bordas do editor e os **vãos entre dois painéis**;
- soltar no **centro** junta o painel como aba; num **lado**, divide o painel; numa **borda**, ocupa a altura ou a largura inteira; num **vão**, cria um painel novo entre os dois;
- solto fora de um alvo, o painel flutua. Arrastado para fora da janela da engine, vira uma janela própria (outro monitor, por exemplo).

O viewport fica sempre no meio: os painéis se encaixam em volta dele.

O layout padrão (0.44): **Modos** e **Posicionar Atores** à esquerda, o viewport no meio, o **Organizador** (com **Níveis** numa aba ao lado) sobre **Detalhes** à direita, e embaixo o **Navegador de Conteúdo** (com o **Registro de Saída** ao lado) e o **Suporte a Plataformas** no canto direito. Todos podem ser fechados, movidos e abertos de novo pelo menu **Janela**.

## Fechar e maximizar
O **X** de um painel ou de uma janela própria fecha só aquela janela, nunca a engine (mesmo maximizada pelo Windows: Win+Seta para cima ou o menu da barra de tarefas). Para abrir de novo: menu **Janela**.

**Clique duas vezes na barra de título** (ou nas abas) de uma janela solta para **maximizar**: fora do editor ela ocupa o monitor; sobre o editor, a área dos painéis (o menu e a barra de ferramentas continuam à vista). Outro clique duplo a devolve ao tamanho e ao lugar de antes.

## Layouts
Em [Janela > Layout](show:Window>Layout):
- **Salvar Layout Como...**: dê um nome e escolha **Este projeto** (fica em `Config/Layouts` do projeto e vai junto com ele) ou **Todos os meus projetos** (fica em `%APPDATA%\RealEngine\Layouts`);
- clique num layout salvo para carregá-lo;
- **Layout Padrão** volta à arrumação de fábrica.

O último layout volta sozinho quando você abre o editor ou o projeto.
--- en
```diagram steps
Drag a panel's tab | The targets show: center, sides, edges and the gaps between panels | Drop on a target: the panel docks there | Window > Layout > Save Layout As
```
## Moving the panels
Each editor panel (Place Actors, Outliner, Levels, Details and the docked Content Browser) is a window. Drag its **tab** somewhere else:
- while you drag, **every target shows**: each panel's center and 4 sides, the editor's 4 edges and the **gaps between two panels**;
- dropping on the **center** joins the panel as a tab; on a **side**, it splits that panel; on an **edge**, it takes the whole height or width; on a **gap**, it makes a new panel between the two;
- dropped away from a target, the panel floats. Dragged out of the engine's window, it becomes a window of its own (on another monitor, for instance).

The viewport always stays in the middle: the panels dock around it.

The default layout (0.44): **Modes** and **Place Actors** on the left, the viewport in the middle, the **Outliner** (with **Levels** in a tab beside it) over **Details** on the right, and at the bottom the **Content Browser** (with the **Output Log** beside it) and **Platform Support** in the right corner. Every one can be closed, moved and opened again from the **Window** menu.

## Closing and maximizing
The **X** of a panel or of a window of its own closes only that window, never the engine (even maximized by Windows: Win+Up or the taskbar's menu). To open it again: the **Window** menu.

**Double-click the title bar** (or the tabs) of a floating window to **maximize** it: out of the editor it fills its monitor; over the editor, the panels' area (the menu and the toolbar stay in sight). Another double-click puts it back to its size and place.

## Layouts
In [Window > Layout](show:Window>Layout):
- **Save Layout As...**: name it and choose **This project** (kept in the project's `Config/Layouts`, it goes with the project) or **All my projects** (kept in `%APPDATA%\RealEngine\Layouts`);
- click a saved layout to load it;
- **Default Layout** goes back to the factory arrangement.

The last layout comes back by itself when you open the editor or the project.
--- es
```diagram steps
Arrastra la pestaña de un panel | Aparecen los destinos: centro, lados, bordes y los huecos entre paneles | Suelta en un destino: el panel se acopla allí | Ventana > Diseño > Guardar Diseño Como
```
## Mover los paneles
Cada panel del editor (Colocar Actores, Organizador, Niveles, Detalles y el Explorador de Contenido acoplado) es una ventana. Arrastra su **pestaña**: mientras arrastras **aparecen todos los destinos** (el centro y los 4 lados de cada panel, los 4 bordes del editor y los **huecos entre dos paneles**). En el **centro** se une como pestaña; en un **lado** divide el panel; en un **borde** ocupa todo el alto o ancho; en un **hueco** crea un panel nuevo entre los dos. Fuera de un destino, flota; fuera de la ventana del motor, se vuelve una ventana propia (otro monitor).

El diseño predeterminado (0.44): **Modos** y **Colocar Actores** a la izquierda, el viewport en el centro, el **Organizador** (con **Niveles** al lado) sobre **Detalles** a la derecha y, abajo, el **Explorador de Contenido** (con el **Registro de Salida**) y **Soporte de Plataformas** en la esquina. La **X** de un panel o de una ventana propia cierra solo esa ventana, nunca el motor (aunque esté maximizada).

**Haz doble clic en la barra de título** (o en las pestañas) de una ventana suelta para **maximizarla**: fuera del editor ocupa el monitor; sobre el editor, el área de los paneles. Otro doble clic la devuelve a su tamaño y lugar.

## Diseños
En [Ventana > Diseño](show:Window>Layout): **Guardar Diseño Como...** (de **Este proyecto** o de **Todos mis proyectos**), los guardados y **Diseño Predeterminado**. El último diseño vuelve solo al abrir el editor o el proyecto.
