id: variable-concept
kind: concept
title: Variáveis e tipos | Variables and types | Variables y tipos
keys: variavel, variaveis, tipo, tipos, int, float, bool, string, vector, rotator, guardar valor, variable, types, instance editable, editable, valor diferente em cada ator, valor por ator, cada ator com seu valor, ajustar no details, different value per actor, per instance value
--- pt
Uma variável guarda um valor do ator:
- **Bool** (sim/não), **Integer** (inteiro), **Float** (com vírgula), **String** (texto), **Vector** (x, y, z), **Rotator** (pitch, yaw, roll), **Actor** (outro ator), arrays e structs.
- **Instance Editable** (`@editable` em Rivora Script, `[Editable]` em C#): o valor aparece no Details de cada ator, para ajustar sem mexer no código. Cada ator guarda o seu (salvo no nível); a **seta amarela** volta ao padrão da classe.
- Em código: `var vida = 100` (Rivora Script), `int vida = 100;` (C#/C), `vida = 100` (Python).
--- en
A variable keeps a value of the actor:
- **Bool** (yes/no), **Integer**, **Float** (decimals), **String** (text), **Vector** (x, y, z), **Rotator** (pitch, yaw, roll), **Actor** (another actor), arrays and structs.
- **Instance Editable** (`@editable` in Rivora Script, `[Editable]` in C#): the value shows in each actor's Details, to tune it without touching the code. Each actor keeps its own (saved in the level); the **yellow arrow** goes back to the class default.
- In code: `var health = 100` (Rivora Script), `int health = 100;` (C#/C), `health = 100` (Python).
--- es
Una variable guarda un valor del actor:
- **Bool** (sí/no), **Integer**, **Float** (decimales), **String** (texto), **Vector** (x, y, z), **Rotator** (pitch, yaw, roll), **Actor** (otro actor), arrays y structs.
- **Instance Editable** (`@editable` en Rivora Script, `[Editable]` en C#): el valor aparece en los Detalles de cada actor, para ajustarlo sin tocar el código. Cada actor guarda el suyo (guardado en el nivel); la **flecha amarilla** vuelve al valor predeterminado de la clase.
- En código: `var health = 100` (Rivora Script), `int health = 100;` (C#/C), `health = 100` (Python).
