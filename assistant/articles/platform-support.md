id: platform-support
kind: howto
title: Suporte a Plataformas e plataforma alvo | Platform Support and the target platform
keys: suporte a plataformas, platform support, plataformas, platforms, plataforma alvo, target platform, alvo, target, pc, android, vr, quest, meta quest, android sdk, sdk, jdk, java_home, android_home, ndk, adb, aparelhos, devices, device manager, gerenciador de aparelhos, celular conectado, phone connected, openxr, runtime, steamvr, quest link, pronto, ready, falta, missing, empacotar para, package for, fps, quadro, frame, compilação de shaders, shader compile, dados derivados, derived data, cache, barra de status, status bar, soporte de plataformas, plataforma objetivo, dispositivos, administrador de dispositivos, datos derivados, compilación de shaders
--- pt
```diagram steps
Clique em PC, Android ou VR na barra de menu | O ponto mostra se o alvo está pronto | Suporte a Plataformas: o que falta e onde resolver | Empacotar para o alvo
```
## A plataforma alvo
No canto direito da barra de menu ficam **PC**, **Android** e **VR**. Clique num deles para escolher o **alvo**: o menu **Plataformas** da barra de ferramentas mostra os pacotes dele primeiro, e com o alvo **VR** o botão **Jogar** abre o **VR Preview** (o nível no headset). O pontinho ao lado de cada um diz se ele está pronto: verde (pronto), amarelo (pronto, com avisos), vermelho (falta algo). Passe o mouse para ver a lista.

Ao lado fica a leitura do quadro: **FPS** e **Quadro (ms)**, medidos na média de meio segundo; as barras mudam de cor (verde a partir de 120 fps). Clique nela para abrir o Debug Viewer (F10).

## O painel Suporte a Plataformas
[Janela > Suporte a Plataformas](open:platforms) (por padrão embaixo à direita, ao lado do Navegador de Conteúdo; pode ser fechado). Para o alvo escolhido ele confere de verdade:
- **PC**: a GPU e o Vulkan 1.3, o ray tracing por hardware (sem ele a GI e as sombras usam o caminho por software), a pasta do pacote;
- **Android**: o empacotador da engine, o **Android SDK** (`ANDROID_HOME` ou `%LOCALAPPDATA%\Android\Sdk`, com build-tools e uma platform), um **JDK** (`JAVA_HOME`), a parte Android da engine (pronta ou o NDK para compilar), a pasta do pacote e a chave de upload do Google Play;
- **VR**: o **runtime OpenXR** do Windows (app Meta Quest com Link ou SteamVR), as plataformas de VR do projeto e, para o Quest autônomo, o Android.

Cada linha tem um ✓, um aviso ou um ✗; quando dá para resolver no editor, um botão leva lá (**Configurações** abre as Configurações de Empacotamento ou do Projeto, **Abrir** mostra a pasta). **Empacotar para ...** roda o empacotador do alvo em segundo plano.

Em **Aparelhos**, **Atualizar** pergunta ao `adb` (do Android SDK) quais celulares e headsets estão conectados por USB (com a depuração USB ligada); no alvo VR aparece o runtime OpenXR.

## A barra de status
À esquerda: **Gaveta de Conteúdo**, **Registro de Saída**, **Estatísticas** (as do viewport), **Profiler** (o Debug Viewer) e **Gerenciador de Aparelhos** (este painel). À direita:
- **Pronto** (ou Jogando, ou Compilando com um indicador girando);
- **Compilação de Shaders: N**: os pipelines ainda sendo feitos nas threads deles mais os materiais compilando. O editor nunca espera: a cena é desenhada com o que já está pronto;
- **Dados Derivados**: o tamanho do cache neste PC (pipelines compilados, malhas e texturas processadas), medido a cada minuto; clique para abrir a pasta;
- a **versão** da engine: clique para o Sobre.
--- en
```diagram steps
Click PC, Android or VR on the menu bar | The dot shows whether the target is ready | Platform Support: what is missing and where to fix it | Package for the target
```
## The target platform
At the menu bar's right end are **PC**, **Android** and **VR**. Click one to choose the **target**: the toolbar's **Platforms** menu shows its packages first, and with the **VR** target the **Play** button opens the **VR Preview** (the level on the headset). The dot beside each says whether it is ready: green (ready), yellow (ready, with notes), red (something missing). Hover it for the list.

Beside them is the frame's readout: **FPS** and **Frame (ms)**, averaged over half a second; the bars change color (green from 120 fps). Click it to open the Debug Viewer (F10).

## The Platform Support panel
[Window > Platform Support](open:platforms) (by default at the bottom right, beside the Content Browser; it may be closed). For the chosen target it really checks:
- **PC**: the GPU and Vulkan 1.3, hardware ray tracing (without it the GI and the shadows use their software paths), the package folder;
- **Android**: the engine's packager, the **Android SDK** (`ANDROID_HOME` or `%LOCALAPPDATA%\Android\Sdk`, with build-tools and a platform), a **JDK** (`JAVA_HOME`), the engine's Android part (prebuilt, or the NDK to build it), the package folder and the Google Play upload key;
- **VR**: Windows' **OpenXR runtime** (the Meta Quest app with Link, or SteamVR), the project's VR platforms and, for the standalone Quest, Android.

Each line has a ✓, a warning or an ✗; when it can be fixed in the editor, a button goes there (**Settings** opens the Packaging or Project Settings, **Open** shows the folder). **Package for ...** runs the target's packager in the background.

Under **Devices**, **Refresh** asks `adb` (the Android SDK's) which phones and headsets are connected by USB (USB debugging on); with the VR target, the OpenXR runtime shows.

## The status bar
On the left: **Content Drawer**, **Output Log**, **Statistics** (the viewport's), **Profiler** (the Debug Viewer) and **Device Manager** (this panel). On the right:
- **Ready** (or Playing, or Compiling with a spinner);
- **Shader Compile: N**: the pipelines still being made on their threads plus the materials compiling. The editor never waits: the scene draws with what is ready;
- **Derived Data**: the cache's size on this PC (compiled pipelines, processed meshes and textures), measured every minute; click to open its folder;
- the engine's **version**: click for About.
--- es
```diagram steps
Haz clic en PC, Android o VR en la barra de menú | El punto indica si el objetivo está listo | Soporte de Plataformas: qué falta y dónde arreglarlo | Empaquetar para el objetivo
```
## La plataforma objetivo
En el extremo derecho de la barra de menú están **PC**, **Android** y **VR**. Haz clic en uno para elegir el **objetivo**: el menú **Plataformas** muestra sus paquetes primero y, con el objetivo **VR**, **Jugar** abre la **VR Preview**. El punto de cada uno indica si está listo (verde), listo con avisos (amarillo) o si falta algo (rojo). Al lado, **FPS** y **Fotograma (ms)**; un clic abre el Debug Viewer (F10).

## El panel Soporte de Plataformas
[Ventana > Soporte de Plataformas](open:platforms) comprueba de verdad: en **PC** la GPU, el ray tracing por hardware y la carpeta del paquete; en **Android** el empaquetador, el **Android SDK**, un **JDK**, la parte Android del motor y la clave de subida de Google Play; en **VR** el runtime **OpenXR** y las plataformas VR del proyecto. Cada línea con ✓, aviso o ✗ y, cuando se puede resolver en el editor, un botón. **Dispositivos** > **Actualizar** pregunta a `adb` qué teléfonos y visores están conectados.

## La barra de estado
A la izquierda: Cajón de Contenido, Registro de Salida, Estadísticas, Profiler y Administrador de Dispositivos. A la derecha: **Listo**, **Compilación de Shaders: N** (pipelines y materiales que aún compilan; el editor nunca espera), **Datos Derivados** (el tamaño de la caché; un clic abre la carpeta) y la **versión**.
