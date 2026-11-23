// JSON (RFC 8259) for the interchange formats: a DOM parser (every value in one arena, strings unescaped to UTF-8 and
// NUL-terminated, numbers as doubles) and a compact writer. Used by glTF; nothing in it is glTF specific.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum JsonType { JSON_NULL = 0, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct JsonMember JsonMember;
typedef struct JsonValue {
    uint8_t type;   // JsonType
    uint32_t count; // array items / object members / string bytes
    union {
        double num;
        const char* str;
        struct JsonValue* items;
        JsonMember* members;
    };
} JsonValue;

struct JsonMember {
    const char* key;
    JsonValue value;
};

typedef struct JsonDoc {
    JsonValue root;
    void* arena; // blocks of the values and strings (json_free)
} JsonDoc;

// Parses text (not NUL-terminated; a UTF-8 BOM is skipped). False with the reason and the line in err.
bool json_parse(const char* text, size_t len, JsonDoc* doc, char* err, size_t err_cap);
void json_free(JsonDoc* doc);

// Lookups: NULL when v is not an object/array, the key is missing or i is out of range.
const JsonValue* json_get(const JsonValue* obj, const char* key);
const JsonValue* json_at(const JsonValue* arr, size_t i);
size_t json_len(const JsonValue* arr); // items of an array (0 for anything else)
// Values with a default for a missing or mistyped value.
double json_num(const JsonValue* v, double def);
int json_int(const JsonValue* v, int def);
bool json_bool(const JsonValue* v, bool def);
const char* json_str(const JsonValue* v, const char* def);
// Shorthands: json_num(json_get(obj, key), def)...
double json_get_num(const JsonValue* obj, const char* key, double def);
int json_get_int(const JsonValue* obj, const char* key, int def);
bool json_get_bool(const JsonValue* obj, const char* key, bool def);
const char* json_get_str(const JsonValue* obj, const char* key, const char* def);
// Up to n numbers of an array into out (the rest keeps its values); returns how many were read.
int json_get_floats(const JsonValue* obj, const char* key, float* out, int n);

// --- Writer ----------------------------------------------------------------------------------
// Compact JSON into a growing buffer; commas and separators are placed automatically. Floats use the shortest form
// that reads back the same float (%.9g), without exponent noise for whole numbers.
typedef struct JsonWriter {
    char* buf;
    size_t len, cap;
    int depth;
    bool after_key;
    bool fail;          // out of memory: everything after is dropped, check at the end
    uint8_t first[256]; // per open container: nothing written in it yet
} JsonWriter;

void jw_init(JsonWriter* w);
void jw_free(JsonWriter* w);
void jw_obj_begin(JsonWriter* w);
void jw_obj_end(JsonWriter* w);
void jw_arr_begin(JsonWriter* w);
void jw_arr_end(JsonWriter* w);
void jw_key(JsonWriter* w, const char* key);
void jw_str(JsonWriter* w, const char* s);
void jw_float(JsonWriter* w, double v);
void jw_int(JsonWriter* w, long long v);
void jw_bool(JsonWriter* w, bool v);
void jw_null(JsonWriter* w);
void jw_raw(JsonWriter* w, const char* text); // a value written as is
// key + value
void jw_kstr(JsonWriter* w, const char* key, const char* s);
void jw_kfloat(JsonWriter* w, const char* key, double v);
void jw_kint(JsonWriter* w, const char* key, long long v);
void jw_kbool(JsonWriter* w, const char* key, bool v);
void jw_kfloats(JsonWriter* w, const char* key, const float* v, int n); // an array of numbers
