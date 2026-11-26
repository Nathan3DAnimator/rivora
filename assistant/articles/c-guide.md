id: c-guide
kind: concept
title: C na Rivora Engine: o que funciona | C in the Rivora Engine: what works | C en la Rivora Engine: qué funciona
keys: linguagem c, c, ansi c, include, typedef struct, globais, ponteiro, parent comment, void beginplay
--- pt
- O arquivo é a classe; o pai vem do comentário `// parent: Character` (sem ele: Actor).
- Globais são os campos (editáveis no Details); `void BeginPlay(void)`, `void Tick(float dt)`.
- `for`, `while`, `do { ... } while (...);`, `switch`, casts, `int xs[] = { 1, 2 }`, `struct` e `typedef struct`; `#include` é ignorado.
- Um ponteiro de ator (`Actor*`) é uma referência a um ator; não há `malloc` nem aritmética de ponteiros.
--- en
- The file is the class; the parent comes from the comment `// parent: Character` (without it: Actor).
- Globals are the fields (editable in Details); `void BeginPlay(void)`, `void Tick(float dt)`.
- `for`, `while`, `do { ... } while (...);`, `switch`, casts, `int xs[] = { 1, 2 }`, `struct` and `typedef struct`; `#include` is ignored.
- An actor pointer (`Actor*`) is a reference to an actor; there's no `malloc` nor pointer arithmetic.
--- es
- El archivo es la clase; el padre viene del comentario `// parent: Character` (sin él: Actor).
- Los globales son los campos (editables en Detalles); `void BeginPlay(void)`, `void Tick(float dt)`.
- `for`, `while`, `do { ... } while (...);`, `switch`, casts, `int xs[] = { 1, 2 }`, `struct` y `typedef struct`; `#include` se ignora.
- Un puntero de actor (`Actor*`) es una referencia a un actor; no hay `malloc` ni aritmética de punteros.
