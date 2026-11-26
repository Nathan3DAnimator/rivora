id: pause-options
kind: recipe
title: Menu de pausa e opções (gráficos, áudio, controles, idioma) | Pause menu and options (graphics, audio, controls, language) | Menú de pausa y opciones (gráficos, audio, controles, idioma)
keys: escolher dlss no menu, choose dlss in the options, upscaler no menu de opcoes, pausa, pausar, pause, menu de pausa, pause menu, set game paused, is game paused, pausar o jogo, despausar, continuar, resume, tick when paused, tick quando pausado, opcoes, options, configuracoes, settings, game user settings, game user setting, apply settings, aplicar opcoes, volume, volume da musica, volume geral, master volume, music volume, sfx, efeitos, voz, qualidade, preset, predefinicao, baixo medio alto epico, low medium high epic, resolucao, resolution, tela cheia, fullscreen, janela, windowed, vsync, limite de fps, frame rate limit, escala da resolucao, resolution scale, remapear, remapear tecla, rebind, remap key, trocar tecla, input key selector, seletor de tecla, sensibilidade, sensitivity, inverter y, invert y, idioma do jogo, language, sair do jogo, quit game, menu principal, main menu, wbp_pausemenu, wbp_options, esc pausa, salvar opcoes, gameusersettings.ini
--- pt
Como na Unreal: **Set Game Paused** para o mundo (física, animação, partículas, IA, movimento e os sons do jogo) e a UI continua. As opções do jogador ficam num arquivo (**GameUserSettings.ini**) e voltam quando o jogo abre.
1. Já vem pronto: **Esc** no jogo empacotado (no Play do editor a tecla **P**, porque lá o Esc para o Play) e o **Start** do controle abrem o **WBP_PauseMenu**: Continuar, Opções, Menu Principal, Sair. A engine cria ele e o **WBP_Options** em **Content/UI** na primeira vez: copie e mude como qualquer Widget Graph.
2. **Opções**: abas Gráficos (predefinição Baixo/Médio/Alto/Épico, resolução, modo de janela, VSync, limite de fps, escala da resolução, **Upscaler**: TSR, DLSS, FSR ou XeSS, só os instalados no PC do jogador), Áudio (Geral, Música, Efeitos, Voz), Controles (sensibilidade, Inverter Y e cada ação do Enhanced Input: clique na tecla e **pressione a nova**) e Idioma. Funciona com mouse, teclado (setas, Enter) e controle (D-pad, A, B volta).
3. Um widget seu mostra uma opção sem nó nenhum: nos Detalhes, **Game User Setting** (um Slider em MasterVolume, um Combo Box em Scalability, um **Input Key Selector** em Key:IA_Jump).
4. Um ator que precisa andar com o jogo pausado (um relógio do menu, uma câmera): **Details > Grafo > Tick When Paused**, ou o nó **Set Tickable When Paused**.
5. **Project Settings > Maps & Modes > Pause Menu**: ligar/desligar, o seu próprio widget, a tecla do Play; **Main Menu Map** é o que o botão Menu Principal abre.

```diagram steps
Esc (Play: P) | WBP_PauseMenu: o mundo para | Opções: abas, setas e Enter | Aplicar: GameUserSettings.ini | Próxima vez: as opções voltam
```
```blueprint
Event BeginPlay
  -> Set Game User Setting ( Setting = "MusicVolume", Value = 0.5 )
  -> Remap Action Key ( Action = "IA_Jump", Key = "Space", New Key = "F" )
  -> Apply Settings
Custom Event "Morreu"
  -> Set Game Paused ( Paused = true )
  -> Open Pause Menu
```
```real
class PauseHelper extends Actor
    event BeginPlay()
        SetTickableWhenPaused(true)
        var musica = GetGameUserSetting("MusicVolume")
        if musica > 0.8
            SetGameUserSetting("MusicVolume", 0.5, "")
            ApplySettings()

    event Morreu()
        SetGamePaused(true)
        OpenPauseMenu()

    event Tick(dt: float)
        if IsGamePaused()
            print("pausado")
```

Nós: **Set Game Paused**, **Is Game Paused**, **Set Tickable When Paused**, **Get / Set Game User Setting** (nomes: Scalability, Resolution, WindowMode, VSync, FrameRateLimit, ResolutionScale, MasterVolume, MusicVolume, SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language, Key:IA_Jump), **Apply Settings** (aplica e salva), **Load Settings**, **Set Settings to Defaults**, **Remap Action Key**, **Get Action Key**, **Open / Close Pause Menu**, **Return to Main Menu**, **Quit Game**. Veja também [Interface do jogo](article:game-ui).
--- en
As in Unreal: **Set Game Paused** stops the world (physics, animation, particles, AI, movement and the game's sounds) and the UI goes on. The player's options are kept in a file (**GameUserSettings.ini**) and come back when the game opens.
1. It comes ready: **Esc** in the packaged game (in the editor's Play the **P** key, because there Esc stops the Play) and a gamepad's **Start** open **WBP_PauseMenu**: Resume, Options, Main Menu, Quit. The engine makes it and **WBP_Options** in **Content/UI** the first time: copy and change them like any Widget Graph.
2. **Options**: tabs Graphics (preset Low/Medium/High/Epic, resolution, window mode, VSync, frame rate limit, resolution scale, **Upscaler**: TSR, DLSS, FSR or XeSS, only those installed on the player's PC), Audio (Master, Music, Effects, Voice), Controls (sensitivity, Invert Y and each Enhanced Input action: click its key and **press the new one**) and Language. Works with the mouse, the keyboard (arrows, Enter) and a gamepad (D-pad, A, B goes back).
3. A widget of yours shows an option with no node: in Details, **Game User Setting** (a Slider on MasterVolume, a Combo Box on Scalability, an **Input Key Selector** on Key:IA_Jump).
4. An actor that must go on while paused (a menu's clock, a camera): **Details > Graph > Tick When Paused**, or the **Set Tickable When Paused** node.
5. **Project Settings > Maps & Modes > Pause Menu**: on/off, your own widget, the Play's key; **Main Menu Map** is what the Main Menu button opens.

```diagram steps
Esc (Play: P) | WBP_PauseMenu: the world stops | Options: tabs, arrows and Enter | Apply: GameUserSettings.ini | Next time: the options are back
```
```blueprint
Event BeginPlay
  -> Set Game User Setting ( Setting = "MusicVolume", Value = 0.5 )
  -> Remap Action Key ( Action = "IA_Jump", Key = "Space", New Key = "F" )
  -> Apply Settings
Custom Event "Died"
  -> Set Game Paused ( Paused = true )
  -> Open Pause Menu
```
```real
class PauseHelper extends Actor
    event BeginPlay()
        SetTickableWhenPaused(true)
        var music = GetGameUserSetting("MusicVolume")
        if music > 0.8
            SetGameUserSetting("MusicVolume", 0.5, "")
            ApplySettings()

    event Died()
        SetGamePaused(true)
        OpenPauseMenu()

    event Tick(dt: float)
        if IsGamePaused()
            print("paused")
```

Nodes: **Set Game Paused**, **Is Game Paused**, **Set Tickable When Paused**, **Get / Set Game User Setting** (names: Scalability, Resolution, WindowMode, VSync, FrameRateLimit, ResolutionScale, MasterVolume, MusicVolume, SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language, Key:IA_Jump), **Apply Settings** (applies and saves), **Load Settings**, **Set Settings to Defaults**, **Remap Action Key**, **Get Action Key**, **Open / Close Pause Menu**, **Return to Main Menu**, **Quit Game**. See also [The game's UI](article:game-ui).
--- es
Como en Unreal: **Set Game Paused** detiene el mundo (física, animación, partículas, IA, movimiento y los sonidos del juego) y la UI sigue. Las opciones del jugador se guardan en un archivo (**GameUserSettings.ini**) y regresan cuando el juego se abre.
1. Viene listo: **Esc** en el juego empaquetado (en el Jugar del editor la tecla **P**, porque allí Esc detiene el Jugar) y el **Start** de un gamepad abren **WBP_PauseMenu**: Resume, Options, Main Menu, Quit. El motor lo crea, junto con **WBP_Options**, en **Content/UI** la primera vez: cópialos y cámbialos como cualquier Widget Graph.
2. **Options**: pestañas Gráficos (preset Bajo/Medio/Alto/Épico, resolución, modo de ventana, VSync, límite de cuadros por segundo, escala de resolución, **Upscaler**: TSR, DLSS, FSR o XeSS, solo los instalados en la PC del jugador), Audio (Master, Music, Effects, Voice), Controles (sensibilidad, Invert Y y cada acción de Enhanced Input: haz clic en su tecla y **pulsa la nueva**) e Idioma. Funciona con el mouse, el teclado (flechas, Enter) y un gamepad (cruceta, A, B retrocede).
3. Un widget tuyo muestra una opción sin nodo: en Detalles, **Game User Setting** (un Slider en MasterVolume, un Combo Box en Scalability, un **Input Key Selector** en Key:IA_Jump).
4. Un actor que debe seguir mientras el juego está en pausa (el reloj de un menú, una cámara): **Detalles > Graph > Tick When Paused**, o el nodo **Set Tickable When Paused**.
5. **Project Settings > Maps & Modes > Pause Menu**: activar/desactivar, tu propio widget, la tecla del Jugar; **Main Menu Map** es lo que abre el botón Main Menu.

```diagram steps
Esc (Jugar: P) | WBP_PauseMenu: el mundo se detiene | Options: pestañas, flechas y Enter | Apply: GameUserSettings.ini | La próxima vez: las opciones regresan
```
```blueprint
Event BeginPlay
  -> Set Game User Setting ( Setting = "MusicVolume", Value = 0.5 )
  -> Remap Action Key ( Action = "IA_Jump", Key = "Space", New Key = "F" )
  -> Apply Settings
Custom Event "Died"
  -> Set Game Paused ( Paused = true )
  -> Open Pause Menu
```
```real
class PauseHelper extends Actor
    event BeginPlay()
        SetTickableWhenPaused(true)
        var music = GetGameUserSetting("MusicVolume")
        if music > 0.8
            SetGameUserSetting("MusicVolume", 0.5, "")
            ApplySettings()

    event Died()
        SetGamePaused(true)
        OpenPauseMenu()

    event Tick(dt: float)
        if IsGamePaused()
            print("paused")
```

Nodos: **Set Game Paused**, **Is Game Paused**, **Set Tickable When Paused**, **Get / Set Game User Setting** (nombres: Scalability, Resolution, WindowMode, VSync, FrameRateLimit, ResolutionScale, MasterVolume, MusicVolume, SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language, Key:IA_Jump), **Apply Settings** (aplica y guarda), **Load Settings**, **Set Settings to Defaults**, **Remap Action Key**, **Get Action Key**, **Open / Close Pause Menu**, **Return to Main Menu**, **Quit Game**. Consulta también [La UI del juego](article:game-ui).
