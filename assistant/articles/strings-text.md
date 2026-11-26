id: strings-text
kind: howto
title: Textos: juntar, formatar, converter | Texts: join, format, convert | Textos: unir, formatear, convertir
keys: string, texto, juntar texto, concatenar, formatar, interpolacao, fstring, numero para texto, to string, upper, lower, replace, substring
--- pt
- Interpolação: `"Vida: {hp}"` (Rivora Script), `$"Vida: {hp}"` (C#), `f"Vida: {self.hp}"` (Python).
- Juntar: `"a" + "b"`; número para texto: `str(x)`; texto para número: `int(s)`, `float(s)`.
- Métodos: `ToUpper`, `ToLower`, `Contains`, `Replace`, `Substring`, `Length`.
- Em Grafo: **Append**, **To String**, **Format Text** (o texto com `{0}` `{1}` `{2}` `{3}` trocados pelas entradas: "Pontos: {0} / {1}"; qualquer valor ligado vira texto; `{{` escreve uma chave).
--- en
- Interpolation: `"Health: {hp}"` (Rivora Script), `$"Health: {hp}"` (C#), `f"Health: {self.hp}"` (Python).
- Join: `"a" + "b"`; number to text: `str(x)`; text to number: `int(s)`, `float(s)`.
- Methods: `ToUpper`, `ToLower`, `Contains`, `Replace`, `Substring`, `Length`.
- In a Graph: **Append**, **To String**, **Format Text** (the text with `{0}` `{1}` `{2}` `{3}` replaced by the inputs: "Score: {0} / {1}"; any value wired in turns to text; `{{` writes a brace).
--- es
- Interpolación: `"Health: {hp}"` (Rivora Script), `$"Health: {hp}"` (C#), `f"Health: {self.hp}"` (Python).
- Unir: `"a" + "b"`; número a texto: `str(x)`; texto a número: `int(s)`, `float(s)`.
- Métodos: `ToUpper`, `ToLower`, `Contains`, `Replace`, `Substring`, `Length`.
- En un Grafo: **Append**, **To String**, **Format Text** (el texto con `{0}` `{1}` `{2}` `{3}` reemplazados por las entradas: "Score: {0} / {1}"; cualquier valor conectado se convierte en texto; `{{` escribe una llave).
