id: scripts-basics
kind: howto
title: Scripts em Rivora Script, C#, Python, C e C++ | Scripts in Rivora Script, C#, Python, C and C++ | Scripts: lo básico
keys: script, codigo, programar, csharp, c#, python, linguagem real, rivora script, rvs, c++, cpp, cplusplus, editor de codigo, ide, programacao, classe em codigo
--- pt
```diagram steps
Adicionar > Script | Escolher a linguagem | Escrever (Ctrl+Espaço completa) | Ctrl+S compila | Arrastar para o nível
```
1. Navegador de Conteúdo: **Adicionar > Script** e escolha a linguagem: **Rivora Script** (a da engine, `.rvs`; o antigo `.real` também abre), **C#**, **Python**, **C** ou **C++**.
   - **C++** (`.cpp`; `.rcpp` também): `class X : public Actor { public: ... };`, campos (aparecem no Detalhes), métodos com `override`/`const`, `auto`, `for (auto x : xs)`, `std::vector`, `std::string`, `std::to_string`, `static_cast`, `FVector`/`FRotator`/`FString` como na Unreal. É compilado para o mesmo bytecode das outras: roda no editor, no jogo e no Android. Sem templates, exceções nem ponteiros (para código nativo de verdade, um **Plugin C++**: Editar > Plugins > Novo Plugin > C++).
2. Duplo clique abre o **Editor de Código**: cores, completar (**Ctrl+Espaço**), erros enquanto digita, **F12** vai à definição, **F1** abre a referência da API.
3. **Ctrl+S** salva e compila. O script é uma classe: arraste-o para o nível como um Grafo.

Toda a API da engine está disponível: cada nó de Grafo é uma função (`Play Sound 2D` → `PlaySound2D`). [Abrir a referência](open:api)

```real
class Hello extends Actor
    event BeginPlay()
        print("Olá, {GetActorLocation()}")
```
--- en
```diagram steps
Add > Script | Pick the language | Write (Ctrl+Space completes) | Ctrl+S compiles | Drag into the level
```
1. Content Browser: **Add > Script** and pick the language: **Rivora Script** (the engine's own, `.rvs`; the old `.real` still opens), **C#**, **Python**, **C** or **C++**.
   - **C++** (`.cpp`; `.rcpp` too): `class X : public Actor { public: ... };`, fields (shown in Details), methods with `override`/`const`, `auto`, `for (auto x : xs)`, `std::vector`, `std::string`, `std::to_string`, `static_cast`, `FVector`/`FRotator`/`FString` as in Unreal. Compiled to the same bytecode as the others: runs in the editor, the game and on Android. No templates, exceptions or pointers (for real native code, a **C++ Plugin**: Edit > Plugins > New Plugin > C++).
2. Double-click opens the **Code Editor**: colors, completion (**Ctrl+Space**), errors as you type, **F12** goes to the definition, **F1** opens the API reference.
3. **Ctrl+S** saves and compiles. The script is a class: drag it into the level like a Graph.

The whole engine API is there: every Graph node is a function (`Play Sound 2D` → `PlaySound2D`). [Open the reference](open:api)

```real
class Hello extends Actor
    event BeginPlay()
        print("Hello, {GetActorLocation()}")
```
--- es
```diagram steps
Añadir > Script | Elige el lenguaje | Escribe (Ctrl+Space completa) | Ctrl+S compila | Arrástralo al nivel
```
1. Explorador de contenidos: **Añadir > Script** y elige el lenguaje: **Rivora Script** (el propio del motor, `.rvs`; el antiguo `.real` todavía se abre), **C#**, **Python**, **C** o **C++**.
   - **C++** (`.cpp`; también `.rcpp`): `class X : public Actor { public: ... };`, campos (se muestran en Detalles), métodos con `override`/`const`, `auto`, `for (auto x : xs)`, `std::vector`, `std::string`, `std::to_string`, `static_cast`, `FVector`/`FRotator`/`FString` como en Unreal. Se compila al mismo bytecode que los demás: corre en el editor, en el juego y en Android. Sin templates, excepciones ni punteros (para código nativo real, un **Plugin C++**: Editar > Plugins > Nuevo Plugin > C++).
2. Doble clic abre el **Editor de Código**: colores, autocompletado (**Ctrl+Space**), errores mientras escribes, **F12** va a la definición, **F1** abre la referencia de la API.
3. **Ctrl+S** guarda y compila. El script es una clase: arrástralo al nivel como un Grafo.

Toda la API del motor está ahí: cada nodo de Grafo es una función (`Play Sound 2D` → `PlaySound2D`). [Abrir la referencia](open:api)

```real
class Hello extends Actor
    event BeginPlay()
        print("Hello, {GetActorLocation()}")
```
