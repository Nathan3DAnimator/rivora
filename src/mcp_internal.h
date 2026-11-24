// Shared by mcp_server.c (transport, protocol) and mcp_tools.c (the tools, run on the main thread)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "io_json.h"

struct Editor;

typedef struct McpCall {
    char method[32];       // "tools/call" or "resources/read"
    char name[160];        // the tool's name / the resource's uri
    JsonDoc doc;           // the request (its own copy)
    const JsonValue* args; // the tool's arguments (NULL: none)
    const JsonValue* id;
    // the answer: the tool's result as a JSON object (the text the AI reads), a picture, a resource's text
    JsonWriter body;
    bool is_error;
    char* image_b64;       // a PNG in base64 (screenshot)
    char* res_text;        // resources/read: Markdown
    char* answer;          // the JSON-RPC answer composed (mcp_compose_answer)
    // a call over several frames (a picture, Play for N seconds)
    int stage;
    double t_start, t_until;
    int frames;
    // the threads'
    void* done; // HANDLE (an event)
    bool finished, abandoned;
} McpCall;

// mcp_tools.c
const char* mcp_instructions(void);
const char* mcp_tools_list_json(void);       // {"tools": [...]}
const char* mcp_prompts_list_json(void);     // {"prompts": [...]}
char* mcp_prompt_get(const JsonValue* params); // the result of prompts/get (malloc'd) or NULL
const char* mcp_resources_list_json(void);
const char* mcp_resource_templates_json(void);
bool mcp_tools_run(McpCall* c, struct Editor* ed, double now); // false: not done yet (called again next frame)
void mcp_compose_answer(McpCall* c);

// helpers
char* mcp_jw_take(JsonWriter* w);                    // the text written (malloc'd, NUL-terminated); NULL on failure
void mcp_write_id(JsonWriter* w, const JsonValue* id); // the request's id as it came (number or string)
