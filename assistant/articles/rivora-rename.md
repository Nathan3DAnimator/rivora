id: rivora-rename
kind: concept
title: A Real Engine agora é Rivora Engine | Real Engine is now Rivora Engine | Real Engine ahora es Rivora Engine
keys: real engine, realengine, rivora, rivora engine, nome novo, new name, mudou de nome, renamed, cade a real engine, where is real engine, realengine.exe, rivora.exe, atalho, shortcut, minhas configuracoes, my settings, appdata
--- pt
```diagram steps
Real Engine 0.49 | Atualizar | Rivora Engine 0.50 | Projetos e configurações iguais
```
- A **Real Engine** mudou de nome para **Rivora Engine** a partir da versão **0.50**. É o mesmo programa, gratuito e de código aberto.
- **A atualização é automática:** a 0.49 acha a 0.50 e instala como sempre. O programa agora é o `rivora.exe`; o `realengine.exe` continua na pasta só para abrir o `rivora.exe`, então os atalhos antigos funcionam.
- **Os projetos abrem iguais:** `.rproject`, `.rlevel`, `.rasset` e os outros formatos não mudaram.
- **As configurações vêm junto:** na primeira vez, as preferências, layouts, temas e a base offline do Assistente são copiados de `%APPDATA%\RealEngine` para `%APPDATA%\Rivora`.
- Os scripts continuam com `using RealEngine;` (C#), `from realengine import *` (Python) e `#include "realengine.h"` (C): nada para mudar nos seus códigos.
--- en
```diagram steps
Real Engine 0.49 | Update | Rivora Engine 0.50 | Same projects and settings
```
- **Real Engine** is called **Rivora Engine** from version **0.50** on. It is the same program, free and open source.
- **Updating is automatic:** 0.49 finds 0.50 and installs it as always. The program is now `rivora.exe`; `realengine.exe` stays in the folder only to open `rivora.exe`, so old shortcuts keep working.
- **Projects open the same:** `.rproject`, `.rlevel`, `.rasset` and the other formats did not change.
- **Your settings come along:** the first time, the preferences, layouts, themes and the Assistant's offline base are copied from `%APPDATA%\RealEngine` to `%APPDATA%\Rivora`.
- Scripts keep `using RealEngine;` (C#), `from realengine import *` (Python) and `#include "realengine.h"` (C): nothing to change in your code.
--- es
```diagram steps
Real Engine 0.49 | Actualizar | Rivora Engine 0.50 | Mismos proyectos y ajustes
```
- **Real Engine** se llama **Rivora Engine** a partir de la versión **0.50**. Es el mismo programa, gratuito y de código abierto.
- **La actualización es automática:** la 0.49 encuentra la 0.50 y la instala como siempre. El programa ahora es `rivora.exe`; `realengine.exe` queda en la carpeta solo para abrir `rivora.exe`, así que los accesos directos antiguos siguen funcionando.
- **Los proyectos se abren igual:** `.rproject`, `.rlevel`, `.rasset` y los demás formatos no cambiaron.
- **Tus ajustes vienen contigo:** la primera vez, las preferencias, los diseños, los temas y la base sin conexión del Asistente se copian de `%APPDATA%\RealEngine` a `%APPDATA%\Rivora`.
- Los scripts mantienen `using RealEngine;` (C#), `from realengine import *` (Python) y `#include "realengine.h"` (C): nada que cambiar en tu código.
