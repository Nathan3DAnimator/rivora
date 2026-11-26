id: android-export
kind: howto
title: Exportar o jogo para Android (APK) | Export the game to Android (APK) | Exportar el juego a Android (APK)
keys: android, apk, celular, smartphone, telefone, phone, mobile, tablet, google play, play store, exportar android, package for android, empacotar para android, adb, instalar no celular, install on phone, keystore, assinar, signing, orientacao, orientation, retrato, portrait, paisagem, landscape, version code, aab
--- pt
```diagram steps
Configurações do Projeto > Plataformas > Android (nome do pacote, orientação, assinatura) | Empacotar Jogo: escolha a Pasta de Saída | Compilar > Empacotar para Android: APK (sem Play Store) | <pasta>/<jogo>.apk (o cartão no canto mostra o progresso) | adb install <jogo>.apk, ou copie para o aparelho
```
## Como funciona
**Compilar > Empacotar para Android: APK (sem Play Store)** faz o mesmo pacote do Windows (nível, luz assada, Grafos, assets) e o transforma num **APK** assinado ao lado da pasta. O jogo roda no Android pelo mesmo runtime em C e Vulkan (`librealengine_game.so`, arm64), sem Java no caminho do jogo.

São dois caminhos: o **APK (sem Play Store)** se instala direto (adb, cabo, link ou arquivo, com "fontes desconhecidas" liberado no aparelho); para publicar na loja use **Compilar > Empacotar para Google Play Store: AAB**, que gera o pacote da loja e um APK para testar.

Em **Configurações do Projeto > Plataformas > Android**:
- **Nome do Pacote**: `com.empresa.jogo` (vazio: `com.realengine.<nome do jogo>`).
- **Orientação**: Paisagem, Retrato ou as duas.
- **Version Code**: suba a cada envio para a loja.
- **Assinatura**: vazio usa uma chave de depuração criada uma vez (instala nos seus aparelhos). Para a loja, use sua própria keystore, o alias e o arquivo com a senha.
- **Controles de Toque**: adiciona o [WBP_TouchControls](article:touch-controls) em telas de toque.

## O que precisa
- Aparelho com **Android 11+** e **Vulkan 1.3** (a maioria dos celulares de 2021 em diante).
- No PC: o **Android SDK** (build-tools 34 e platform 34) e um **JDK**. O runtime arm64 já vem pronto em `android/prebuilt`.
- O registro do empacotador fica em `<pasta>/<jogo>_android.log`.

Linha de comando: `realengine --open Level.rlevel --package-android <pasta>`. Veja também [Empacotar o jogo](article:package-game).
--- en
```diagram steps
Project Settings > Platforms > Android (package name, orientation, signing) | Package Game: choose the Output Folder | Build > Package for Android: APK (no Play Store) | <folder>/<game>.apk (the corner card shows the progress) | adb install <game>.apk, or copy it to the device
```
## How it works
**Build > Package for Android: APK (no Play Store)** makes the same package as for Windows (the level, its baked light, Graphs, assets) and turns it into a signed **APK** next to the folder. The game runs on Android through the same C and Vulkan runtime (`librealengine_game.so`, arm64), no Java in the game's path.

There are two routes: the **APK (no Play Store)** installs directly (adb, cable, a link or the file, with "unknown sources" allowed on the device); to publish on the store use **Build > Package for Google Play Store: AAB**, which makes the store's bundle and an APK to test.

In **Project Settings > Platforms > Android**:
- **Package Name**: `com.company.game` (empty: `com.realengine.<game name>`).
- **Orientation**: Landscape, Portrait or either.
- **Version Code**: raise it with each store upload.
- **Signing**: empty uses a debug key made once (it installs on your own devices). For the store, use your own keystore, its alias and the file holding its password.
- **Touch Controls**: adds [WBP_TouchControls](article:touch-controls) on touch screens.

## What it needs
- A device with **Android 11+** and **Vulkan 1.3** (most phones from 2021 on).
- On the PC: the **Android SDK** (build-tools 34 and platform 34) and a **JDK**. The arm64 runtime ships ready in `android/prebuilt`.
- The packager's log is `<folder>/<game>_android.log`.

Command line: `realengine --open Level.rlevel --package-android <folder>`. See also [Package the game](article:package-game).
--- es
```diagram steps
Project Settings > Platforms > Android (nombre del paquete, orientación, firma) | Package Game: elige la carpeta de salida | Build > Package for Android: APK (sin Play Store) | <carpeta>/<juego>.apk (la tarjeta de la esquina muestra el progreso) | adb install <juego>.apk, o cópialo al dispositivo
```
## Cómo funciona
**Build > Package for Android: APK (sin Play Store)** crea el mismo paquete que para Windows (el nivel, su luz precalculada, Grafos, assets) y lo convierte en un **APK** firmado junto a la carpeta. El juego se ejecuta en Android con el mismo runtime en C y Vulkan (`librealengine_game.so`, arm64), sin Java en su camino.

Hay dos rutas: el **APK (sin Play Store)** se instala directamente (adb, cable, un enlace o el archivo, con "orígenes desconocidos" permitidos en el dispositivo); para publicar en la tienda usa **Build > Package for Google Play Store: AAB**, que crea el bundle de la tienda y un APK para probar.

En **Project Settings > Platforms > Android**:
- **Package Name**: `com.company.game` (vacío: `com.realengine.<nombre del juego>`).
- **Orientation**: Landscape, Portrait o cualquiera.
- **Version Code**: súbelo en cada envío a la tienda.
- **Signing**: vacío usa una clave de depuración creada una sola vez (se instala en tus propios dispositivos). Para la tienda, usa tu propio keystore, su alias y el archivo que contiene su contraseña.
- **Touch Controls**: añade [WBP_TouchControls](article:touch-controls) en las pantallas táctiles.

## Qué necesita
- Un dispositivo con **Android 11+** y **Vulkan 1.3** (la mayoría de los móviles desde 2021).
- En el PC: el **Android SDK** (build-tools 34 y platform 34) y un **JDK**. El runtime arm64 viene listo en `android/prebuilt`.
- El registro del empaquetador es `<carpeta>/<juego>_android.log`.

Línea de comandos: `realengine --open Level.rlevel --package-android <carpeta>`. Consulta también [Empaquetar el juego](article:package-game).
