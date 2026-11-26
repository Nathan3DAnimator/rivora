id: error-unused-value
kind: error
title: Aviso: a linha calcula um valor e não faz nada | Warning: the line computes a value and does nothing | Error: valor sin usar
keys: valor nao usado, linha sem efeito, unused value, warning
match: computes a value and does nothing
--- pt
A linha só calcula algo (`x + 1`) sem guardar nem usar. Provavelmente faltou atribuir: `x = x + 1` ou `x += 1`.
--- en
The line only computes something (`x + 1`) without keeping or using it. Most likely an assignment is missing: `x = x + 1` or `x += 1`.
--- es
La línea solo calcula algo (`x + 1`) sin guardarlo ni usarlo. Lo más probable es que falte una asignación: `x = x + 1` o `x += 1`.
