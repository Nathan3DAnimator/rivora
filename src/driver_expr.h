// (Q19, notas-dev/importar-tlender.md) Blender's / the Tlender's simple driver expressions (driver_expr.c, copied from
// the Tlender's rig_expr.c): numbers, variables, `frame`, pi, True / False, + - * / // % ** (right associative, above
// unary minus), comparisons (chained), not / and / or, `a if cond else b`, and the functions min max abs fabs floor ceil
// trunc round int sqrt pow exp log (1 or 2 args) sin cos tan asin acos atan atan2 radians degrees fmod sign clamp lerp
// smoothstep (and the rest of Python's math module). Compiled once to a small stack bytecode; no allocation to run.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RE_EXPR_CODE 192
#define RE_DRIVER_VAR_LEN 64

typedef struct DriverExpr {
    uint8_t op[RE_EXPR_CODE];
    double arg[RE_EXPR_CODE]; // doubles: Python compares in double precision
    int count;
    int stack; // depth it needs
} DriverExpr;

// vars: the variable names (the expression's identifiers resolve to their index). On error: false, a message with the
// column (1-based) in err.
bool driver_expr_compile(const char* text, const char* const* vars, int var_count, DriverExpr* out, char* err, int err_cap);
// The `self` references of an expression (Blender's Use Self), without spaces, each once. Returns how many (at most cap).
int driver_expr_self_refs(const char* text, char (*out)[RE_DRIVER_VAR_LEN], int cap);
// NaN-free result; *ok false on a domain error (division by zero, sqrt of a negative...), which gives 0.
float driver_expr_eval(const DriverExpr* e, const float* vars, float frame, bool* ok);
