// (2026-11-24, the user: "MCP support, an API that guides and speeds up the AI in control") The Model Context Protocol
// server of the editor (notas-dev/mcp.md): an AI client (Claude Code, Claude Desktop, Cursor...) drives the editor
// through tools: what is in the level, actors placed / changed / removed in batches, scripts run on the level in any of
// the 4 languages (the whole engine API), the viewport's picture, the console, Play, the docs and the API reference.
//   - Transport: Streamable HTTP on 127.0.0.1 (POST /mcp, JSON answers, no SSE); the clients that only speak stdio go
//     through rivora_mcp.exe (src/mcp_bridge.c), which forwards each line and opens the editor when it isn't running.
//   - Threads: a listener and a thread per connection parse the requests; what reads or changes the editor is queued and
//     run by the main thread between frames (mcp_server_frame), a call at a time, the answer sent when it's done.
//   - Safety: only 127.0.0.1, the Origin / Host checked (no web page can reach it), off until the user turns it on
//     (Tools > AI Connection (MCP), Editor Preferences, --mcp).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct Editor;

#define MCP_DEFAULT_PORT 47860
#define MCP_PROTOCOL "2025-06-18"

bool mcp_server_start(int port);  // false: the port is taken (another editor?) or the sockets failed
void mcp_server_stop(void);
bool mcp_server_running(void);
int mcp_server_port(void);
const char* mcp_server_error(void);   // why the last start failed ("" none)
unsigned mcp_server_calls(void);      // tool calls answered since the start
const char* mcp_server_client(void);  // the last client's name ("claude-code"...), "" none

// Main thread, every frame inside the UI's frame (main.c, after editor_frame): the queued calls run here
void mcp_server_frame(struct Editor* ed, double now);

// One JSON-RPC message handled as the HTTP endpoint does (any thread but the main one waits for it): the answer
// (malloc'd, NULL for a notification). The test calls it; the server threads too
char* mcp_handle_message(const char* body, size_t len);

// Tools > AI Connection (MCP): the switch, the port, the clients' setup to copy; Editor Preferences' fields
void mcp_ui_draw(struct Editor* ed);
bool* mcp_ui_show_flag(void);
bool mcp_pref_enabled(void);
void mcp_pref_set(bool enabled, int port); // started / stopped to match; editor.ini written
void mcp_write_prefs(FILE* f);
bool mcp_read_pref(const char* key, const char* value);
void mcp_apply_prefs(bool force_on, int port_override); // after editor.ini was read (main.c: --mcp, --mcp-port)

// --mcp-test (mcp_test.c): the protocol, every tool and the stdio bridge, against this editor
bool mcp_test_frame(struct Editor* ed, bool* ok);
