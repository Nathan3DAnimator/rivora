id: error-type-mismatch
kind: error
title: Erro: Type mismatch (tipos diferentes) | Error: Type mismatch | Error: Type mismatch (tipos distintos)
keys: tipo errado, tipos diferentes, conversao, type mismatch, wrong type
match: Type mismatch
--- pt
Um valor de um tipo foi para um lugar que espera outro (um texto onde vai um número, um vetor onde vai um float).
- Converta: `int(x)`, `float(x)`, `str(x)`.
- De um vetor, pegue um componente: `v.x`.
- A mensagem diz o esperado e o recebido: "Float expected, this is Vector".
--- en
A value of one type went where another is expected (text where a number goes, a vector where a float goes).
- Convert: `int(x)`, `float(x)`, `str(x)`.
- From a vector, take a component: `v.x`.
- The message says what was expected and what came: "Float expected, this is Vector".
--- es
Un valor de un tipo fue donde se espera otro (texto donde va un número, un vector donde va un float).
- Convierte: `int(x)`, `float(x)`, `str(x)`.
- De un vector, toma un componente: `v.x`.
- El mensaje dice qué se esperaba y qué llegó: "Float expected, this is Vector".
