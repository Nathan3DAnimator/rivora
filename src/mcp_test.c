// --mcp-test (ctest mcp; mcp.h, notas-dev/mcp.md): the MCP server tested as a client uses it. A thread talks HTTP to
// the editor's server (a port of its own) while the editor runs its frames: the handshake, the tools' list, every kind
// of tool (state, spawn in a batch with a wrong type, modify, get, find, scripts that run and that fail, the API, the
// console, the camera and a picture, delete and undo, Play for a second), a web page's Origin refused, and the stdio
// bridge (rivora_mcp.exe) through pipes. The time of a call there and back is measured. Prints "mcp: ok".
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "content.h"
#include "editor.h"
#include "io_json.h"
#include "mcp.h"
#include "platform.h"
#include "project.h"

static struct {
    HANDLE thread;
    volatile LONG done;
    bool failed;
    int frames;
    double t_start;
} T;

static void bad(const char* what, const char* got) {
    printf("[RealEngine] FAIL: mcp: %s%s%.400s\n", what, got ? " | got: " : "", got ? got : "");
    T.failed = true;
}

// One HTTP request to the server: the status and the body (malloc'd)
static char* http(const char* method, const char* body, const char* extra_headers, int* status) {
    *status = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET, a.sin_port = htons((u_short)mcp_server_port()), a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr*)&a, sizeof a)) {
        if (s != INVALID_SOCKET) closesocket(s);
        return NULL;
    }
    size_t bl = body ? strlen(body) : 0;
    char head[600];
    int hn = snprintf(head, sizeof head, "%s /mcp HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n%s\r\n", method, mcp_server_port(), bl, extra_headers ? extra_headers : "");
    send(s, head, hn, 0);
    if (bl) send(s, body, (int)bl, 0);
    size_t cap = 1 << 20, n = 0;
    char* buf = malloc(cap + 1);
    int k;
    while (buf && (k = recv(s, buf + n, (int)(cap - n), 0)) > 0) {
        n += (size_t)k;
        if (n == cap) {
            char* nb = realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb, cap *= 2;
        }
    }
    closesocket(s);
    if (!buf) return NULL;
    buf[n] = 0;
    *status = n > 12 ? atoi(buf + 9) : 0;
    char* b = strstr(buf, "\r\n\r\n");
    char* out = _strdup(b ? b + 4 : "");
    free(buf);
    return out;
}

static int g_id = 1;
// A tools/call: the result's text (the tool's JSON, malloc'd); is_error and has_image set
static char* call(const char* tool, const char* args, bool* is_error, bool* has_image) {
    char* msg = malloc(strlen(args) + 256);
    sprintf(msg, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}", g_id++, tool, args);
    int st = 0;
    char* r = http("POST", msg, NULL, &st);
    free(msg);
    *is_error = true;
    if (has_image) *has_image = false;
    if (!r || st != 200) {
        char what[120];
        snprintf(what, sizeof what, "%s: HTTP %d", tool, st);
        bad(what, r);
        free(r);
        return _strdup("");
    }
    JsonDoc d;
    char err[120];
    char* text = NULL;
    if (json_parse(r, strlen(r), &d, err, sizeof err)) {
        const JsonValue* res = json_get(&d.root, "result");
        *is_error = json_get_bool(res, "isError", true);
        const JsonValue* content = json_get(res, "content");
        for (size_t i = 0; i < json_len(content); i++) {
            const JsonValue* c = json_at(content, i);
            if (!strcmp(json_get_str(c, "type", ""), "text") && !text) text = _strdup(json_get_str(c, "text", ""));
            if (has_image && !strcmp(json_get_str(c, "type", ""), "image") && strlen(json_get_str(c, "data", "")) > 100) *has_image = true;
        }
        if (!res) bad(tool, r);
        json_free(&d);
    } else bad("an answer isn't JSON", r);
    free(r);
    return text ? text : _strdup("");
}

static bool has(const char* s, const char* part) { return s && strstr(s, part) != NULL; }

// The bridge: rivora_mcp.exe beside the editor, two lines in, two lines out
static void test_bridge(void) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* sl = wcsrchr(exe, L'\\');
    if (!sl) return;
    wcscpy_s(sl + 1, MAX_PATH - (size_t)(sl + 1 - exe), L"rivora_mcp.exe");
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --port %d --no-launch", exe, mcp_server_port());
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    CreatePipe(&in_r, &in_w, &sa, 0), CreatePipe(&out_r, &out_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0), SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES, si.hStdInput = in_r, si.hStdOutput = out_w, si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        bad("rivora_mcp.exe didn't start (not built beside the editor?)", NULL);
        return;
    }
    CloseHandle(in_r), CloseHandle(out_w);
    const char* lines = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"bridge-test\",\"version\":\"1\"}}}\n"
                        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    DWORD wr;
    WriteFile(in_w, lines, (DWORD)strlen(lines), &wr, NULL);
    CloseHandle(in_w); // (EOF: the bridge ends after answering)
    char buf[1 << 16];
    size_t n = 0;
    DWORD rd;
    while (n < sizeof buf - 1 && ReadFile(out_r, buf + n, (DWORD)(sizeof buf - 1 - n), &rd, NULL) && rd) n += rd;
    buf[n] = 0;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread), CloseHandle(out_r);
    int nl = 0;
    for (size_t i = 0; i < n; i++) nl += buf[i] == '\n';
    bool ok = nl == 2 && has(buf, "\"rivora-engine\"") && has(buf, "\"spawn_actors\"");
    printf("[RealEngine] mcp: the stdio bridge answered %d lines (%zu bytes): %s\n", nl, n, ok ? "initialize + tools/list" : "WRONG");
    if (!ok) bad("the bridge's answers", buf);
}

// (2026-11-24) The guide: the checks, recipe, batch, inspect_level, tool_help, search, "next"; and the measure of a task
// done before (the 22 tools) and now (one call)
static int actors_now(void) {
    bool e;
    char* t = call("get_editor_state", "{}", &e, NULL);
    const char* p = t ? strstr(t, "\"actors\":") : NULL;
    int n = p ? atoi(p + 9) : -1;
    free(t);
    return n;
}
static size_t g_bytes; // (the answers' size in a measure)
static char* callm(const char* tool, const char* args, bool* e) {
    char* t = call(tool, args, e, NULL);
    g_bytes += t ? strlen(t) : 0;
    return t;
}
static bool write_bmp(const char* path, uint8_t r, uint8_t g, uint8_t b) { // a 4 x 4 picture
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint8_t h[54] = { 'B', 'M' };
    uint32_t size = 54 + 4 * 4 * 3;
    memcpy(h + 2, &size, 4), h[10] = 54, h[14] = 40, h[18] = 4, h[22] = 4, h[26] = 1, h[28] = 24;
    fwrite(h, 1, 54, f);
    for (int i = 0; i < 16; i++) fputc(b, f), fputc(g, f), fputc(r, f);
    fclose(f);
    return true;
}

static int jnum(const char* t, const char* key) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char* p = t ? strstr(t, k) : NULL;
    return p ? atoi(p + strlen(k)) : -1;
}
// (2026-11-24, the tester's block of item 6) a recipe's dry run: its plan whole (no step left empty: playable_character's
// spawn had no actor while its class wasn't made, an error), no error, nothing kept (the actors, no_file not written);
// the actors it says the call would leave (actors_would_be), checked against the real call by recipe_real
static int recipe_dry(const char* label, const char* args, const char* no_file) {
    bool e;
    char dry[600];
    snprintf(dry, sizeof dry, "%.*s,\"dry_run\":true}", (int)strlen(args) - 1, args);
    int base = actors_now();
    char p[1300] = "";
    if (no_file) snprintf(p, sizeof p, "%s%s", content_root(), no_file + 8);
    bool had = no_file && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; // (made by an earlier step: not this dry run's)
    char* t = call("recipe", dry, &e, NULL);
    printf("[RealEngine] mcp: recipe %s dry_run -> %.300s\n", label, t);
    if (e || !has(t, "\"plan\"") || has(t, "\"actors\":[]") || has(t, "\"failed\":true") || jnum(t, "actors_would_be") < 0 || actors_now() != base ||
        (no_file && !had && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES)) {
        char what[160];
        snprintf(what, sizeof what, "recipe %s dry_run (the whole plan, no error, nothing kept)", label);
        bad(what, t);
    }
    int would = jnum(t, "actors_would_be");
    free(t);
    return would;
}
// the real call's answer t: its actors_after what the level has after (the Mannequin's parts counted), and what the
// dry run said it would be
static void recipe_real(const char* label, const char* t, int would) {
    int after = jnum(t, "actors_after"), now = actors_now();
    printf("[RealEngine] mcp: recipe %s: actors_after %d, the level %d, the dry run said %d\n", label, after, now, would);
    if (after != now || would != now) {
        char what[160];
        snprintf(what, sizeof what, "recipe %s: actors_after %d, the level then %d, its dry run %d (all three the same)", label, after, now, would);
        bad(what, NULL);
    }
}

static void test_guide(void) {
    bool e;
    char* t;
    // the checks: a wrong key answers the right one, nothing runs
    int n0 = actors_now();
    t = call("spawn_actors", "{\"actors\":[{\"type\":\"Cube\",\"position\":[0,0,0]}]}", &e, NULL);
    printf("[RealEngine] mcp: a wrong key -> %.240s\n", t);
    if (!e || !has(t, "did you mean 'location'") || actors_now() != n0) bad("the check of spawn_actors' keys", t);
    free(t);
    t = call("play", "{\"action\":\"jump\"}", &e, NULL);
    if (!e || !has(t, "one of: start, stop, simulate, status")) bad("the check of an enum", t);
    free(t);
    t = call("tool_help", "{\"tool\":\"spawn_actors\"}", &e, NULL);
    if (e || !has(t, "\"examples\"") || !has(t, "\"schema\"")) bad("tool_help spawn_actors", t);
    free(t);
    t = call("tool_help", "{}", &e, NULL);
    if (e || !has(t, "\"recipe\"") || !has(t, "\"batch\"")) bad("tool_help (every tool)", t);
    free(t);

    // the measure: a test level with the 22 tools (what an AI did: the guide, the types, one spawn of everything, the
    // camera) and with recipe (one call)
    int base = actors_now();
    g_bytes = 0;
    double t0 = re_time_seconds();
    free(callm("get_started", "{}", &e));
    free(callm("list_actor_types", "{}", &e));
    t = callm("spawn_actors", "{\"actors\":[{\"type\":\"Plane\",\"name\":\"OldGround\",\"scale\":[40,40,1]},{\"type\":\"Player Start\",\"location\":[0,0,100]},"
                              "{\"type\":\"Clear Day\"},{\"type\":\"Cube\",\"location\":[1200,0,50]},{\"type\":\"Cube\",\"location\":[-1200,0,50]},"
                              "{\"type\":\"Cube\",\"location\":[0,1200,50]},{\"type\":\"Cube\",\"location\":[0,-1200,50]}]}", &e);
    if (e) bad("the test level with the old tools", t);
    free(t);
    free(callm("set_camera", "{\"location\":[-2200,-2200,1400],\"look_at\":[0,0,0]}", &e));
    double before_ms = (re_time_seconds() - t0) * 1000.0;
    size_t before_bytes = g_bytes;
    if (actors_now() <= base) bad("the old way placed nothing", NULL);
    base = actors_now(); // (the old way's actors stay: a spawn of types takes an undo step per actor)
    int would = recipe_dry("test_level", "{\"recipe\":\"test_level\",\"params\":{\"size_m\":40,\"obstacles\":6,\"lighting\":\"day\"}}", NULL);
    g_bytes = 0;
    t0 = re_time_seconds();
    t = callm("recipe", "{\"recipe\":\"test_level\",\"params\":{\"size_m\":40,\"obstacles\":6,\"lighting\":\"day\"}}", &e);
    double after_ms = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] mcp: recipe test_level -> %.300s\n", t);
    int made = actors_now() - base;
    if (e || !has(t, "\"results\"") || !has(t, "\"next\"") || made < 8) bad("recipe test_level (ground, start, sky, 6 obstacles)", t);
    recipe_real("test_level", t, would);
    free(t);
    printf("[RealEngine] mcp: measure: a test level: before 4 calls %.1f ms (%zu bytes read), after 1 call %.1f ms (%zu bytes), %d actors\n",
           before_ms, before_bytes, after_ms, g_bytes, made);
    free(call("undo", "{}", &e, NULL)); // (one undo step for the whole recipe)
    if (actors_now() != base) bad("one undo takes the whole recipe back", NULL);

    // dry run: the plan, nothing kept
    t = call("recipe", "{\"recipe\":\"lighting\",\"params\":{\"style\":\"night\"},\"dry_run\":true}", &e, NULL);
    if (e || !has(t, "dry_run") || !has(t, "\"plan\"") || actors_now() != base) bad("recipe lighting dry_run", t);
    free(t);
    t = call("recipe", "{\"recipe\":\"lighting\",\"params\":{\"style\":\"noon\"}}", &e, NULL);
    if (!e || !has(t, "day, morning, sunset")) bad("recipe lighting with a wrong style", t);
    free(t);

    // batch: atomic rolls back, a good one is one undo step
    t = call("batch", "{\"steps\":[{\"tool\":\"spawn_actors\",\"args\":{\"actors\":[{\"type\":\"Cube\",\"name\":\"BatchA\"}]}},"
                      "{\"tool\":\"modify_actors\",\"args\":{\"actors\":[{\"actor\":\"Ghost\",\"offset\":[0,0,1]}]}}]}", &e, NULL);
    printf("[RealEngine] mcp: batch with a failing step -> %.240s\n", t);
    if (!e || !has(t, "rolled_back") || actors_now() != base) bad("an atomic batch rolled back", t);
    free(t);
    t = call("batch", "{\"steps\":[{\"tool\":\"level\",\"args\":{\"action\":\"save\"}}]}", &e, NULL);
    if (!e || !has(t, "can't be in a batch")) bad("a batch refuses level", t);
    free(t);
    t = call("batch", "{\"steps\":[{\"tool\":\"spawn_actors\",\"args\":{\"actors\":[{\"type\":\"Cube\",\"name\":\"BatchB\"},{\"type\":\"Sphere\",\"name\":\"BatchC\"}]}},"
                      "{\"tool\":\"modify_actors\",\"args\":{\"actors\":[{\"actor\":\"BatchB\",\"offset\":[0,0,300],\"components\":[\"Rotating Movement\"]}]}},"
                      "{\"tool\":\"get_actor\",\"args\":{\"actor\":\"BatchB\",\"properties\":false}}]}", &e, NULL);
    if (e || actors_now() != base + 2 || !has(t, "Rotating Movement") || !has(t, "one undo step")) bad("a batch of three steps", t);
    free(t);
    free(call("undo", "{}", &e, NULL));
    if (actors_now() != base) bad("one undo takes the batch back", NULL);


    // a material from texture maps found by their names
    char dir[1200], p[1300];
    snprintf(dir, sizeof dir, "%s/MCPTex", content_root());
    CreateDirectoryA(dir, NULL);
    snprintf(p, sizeof p, "%s/T_Brick_BaseColor.bmp", dir), write_bmp(p, 160, 70, 50);
    snprintf(p, sizeof p, "%s/T_Brick_Normal.bmp", dir), write_bmp(p, 128, 128, 255);
    snprintf(p, sizeof p, "%s/T_Brick_Roughness.bmp", dir), write_bmp(p, 200, 200, 200);
    free(call("spawn_actors", "{\"actors\":[{\"type\":\"Cube\",\"name\":\"Wall\"}]}", &e, NULL));
    would = recipe_dry("pbr_material", "{\"recipe\":\"pbr_material\",\"params\":{\"name\":\"Brick\",\"folder\":\"/Content/MCPTex\",\"apply_to\":[\"Wall\"]}}",
                       "/Content/MCPTex/M_Brick.rmat");
    t = call("recipe", "{\"recipe\":\"pbr_material\",\"params\":{\"name\":\"Brick\",\"folder\":\"/Content/MCPTex\",\"apply_to\":[\"Wall\"]}}", &e, NULL);
    printf("[RealEngine] mcp: recipe pbr_material -> %.300s\n", t);
    snprintf(p, sizeof p, "%s/MCPTex/M_Brick.rmat", content_root());
    if (e || !has(t, "M_Brick.rmat") || !has(t, "normal:") || !has(t, "roughness:") || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) bad("recipe pbr_material", t);
    recipe_real("pbr_material", t, would);
    free(t);
    t = call("get_actor", "{\"actor\":\"Wall\",\"properties\":false}", &e, NULL);
    if (!has(t, "M_Brick")) bad("the material applied to Wall", t);
    free(t);

    // a playable character and the HUD
    // (its class not made yet: the dry run's case the tester found; the Default Pawn an undo leaves, said in "next")
    would = recipe_dry("playable_character", "{\"recipe\":\"playable_character\",\"params\":{\"name\":\"BP_McpHero\",\"view\":\"first\"}}",
                       "/Content/FirstPerson/BP_McpHero.rbp");
    if (!strcmp(project()->default_pawn, "BP_McpHero")) bad("recipe playable_character dry_run set the Default Pawn", NULL);
    t = call("recipe", "{\"recipe\":\"playable_character\",\"params\":{\"name\":\"BP_McpHero\",\"view\":\"first\"}}", &e, NULL);
    printf("[RealEngine] mcp: recipe playable_character -> %.240s\n", t);
    if (e || !has(t, "BP_McpHero.rbp")) bad("recipe playable_character", t);
    if (!has(t, "undo takes back the actors, not the Default Pawn")) bad("recipe playable_character: next says an undo leaves the Default Pawn", t);
    recipe_real("playable_character", t, would);
    free(t);
    would = recipe_dry("hud", "{\"recipe\":\"hud\"}", "/Content/UI/WBP_GameHUD.rbp");
    t = call("recipe", "{\"recipe\":\"hud\"}", &e, NULL);
    if (e || !has(t, "GameHUD")) bad("recipe hud", t);
    recipe_real("hud", t, would);
    free(t);

    // inspect_level against the old diagnosis (the state, the actors, the log)
    g_bytes = 0;
    t0 = re_time_seconds();
    free(callm("get_editor_state", "{}", &e));
    { // (2026-11-25) the open project by its name (MCPTEST: --new-project), and said open
        char* st = call("get_editor_state", "{}", &e, NULL);
        if (e || !has(st, "\"project\":\"MCPTEST\"") || !has(st, "\"project_open\":true")) bad("get_editor_state names the open project", st);
        free(st);
    }
    free(callm("find_actors", "{\"limit\":200}", &e));
    free(callm("get_log", "{}", &e));
    double diag_before = (re_time_seconds() - t0) * 1000.0;
    size_t diag_bytes = g_bytes;
    g_bytes = 0;
    t0 = re_time_seconds();
    t = callm("inspect_level", "{}", &e);
    double diag_after = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] mcp: inspect_level -> %.300s\n", t);
    if (e || !has(t, "\"issues\"") || !has(t, "\"next_steps\"") || !has(t, "BP_McpHero")) bad("inspect_level", t);
    free(t);
    printf("[RealEngine] mcp: measure: what is wrong: before 3 calls %.1f ms (%zu bytes, the AI works it out), after 1 call %.1f ms (%zu bytes, each issue with its fix)\n",
           diag_before, diag_bytes, diag_after, g_bytes);

    // search over everything
    t = call("search", "{\"query\":\"light\"}", &e, NULL);
    if (e || !has(t, "Point Light") || !has(t, "\"functions\"") || !has(t, "\"docs\"")) bad("search light", t);
    free(t);
    t = call("search", "{\"query\":\"component\"}", &e, NULL);
    if (e || !has(t, "HUD Layout")) bad("search component (the components)", t);
    free(t);
    // (2026-11-25, the tester's gap) the Procedural World: found by search, placed by its group's name (Terrain) and as a
    // Facility with its properties, edited, its kind in get_actor. Last, and checked by the actors' names: a Procedural
    // World grows actors of its own (thousands in the tester's run), the counts above would count them
    t = call("search", "{\"query\":\"Procedural World\"}", &e, NULL);
    if (e || !has(t, "Procedural World: Facility")) bad("search finds the Procedural World's kinds", t);
    free(t);
    t = call("spawn_actors", "{\"actors\":[{\"type\":\"Procedural World\",\"name\":\"PW_Terrain\"},"
                             "{\"type\":\"Procedural World: Facility\",\"name\":\"PW_Facility\",\"properties\":{\"Rooms\":12,\"Seed\":7}}]}", &e, NULL);
    printf("[RealEngine] mcp: spawn_actors Procedural World -> %.200s\n", t);
    if (e || !has(t, "PW_Terrain") || !has(t, "PW_Facility")) bad("spawn_actors places the Procedural World (Terrain and Facility)", t);
    free(t);
    free(call("modify_actors", "{\"actors\":[{\"actor\":\"PW_Facility\",\"properties\":{\"Room Size\":900}}]}", &e, NULL));
    if (e) bad("modify_actors edits the Procedural World", NULL);
    t = call("get_actor", "{\"actor\":\"PW_Facility\"}", &e, NULL);
    if (e || !has(t, "\"procedural_mode\":\"Facility\"") || !has(t, "Rooms\":12") || !has(t, "Room Size\":900") || !has(t, "Seed\":7"))
        bad("get_actor shows the Facility's kind and the properties set", t);
    free(t);
    t = call("get_actor", "{\"actor\":\"PW_Terrain\",\"properties\":false}", &e, NULL);
    if (e || !has(t, "\"procedural_mode\":\"Terrain\"")) bad("the group's name places its first kind (Terrain)", t);
    free(t);
    free(call("undo", "{}", &e, NULL)), free(call("undo", "{}", &e, NULL)); // (the edit, then the placing)
    t = call("get_actor", "{\"actor\":\"PW_Facility\",\"properties\":false}", &e, NULL);
    if (!e) bad("two undos take the Procedural Worlds back", t);
    free(t);
    // (2026-11-25, the tester) select_actors checks its entries: {"id": n} objects taken, a wrong one named in errors
    t = call("select_actors", "{\"actors\":[{\"id\":999999},\"NoSuchActor\"]}", &e, NULL);
    if (!e || !has(t, "no actor '999999'") || !has(t, "no actor 'NoSuchActor'")) bad("select_actors names the actors it can't find", t);
    free(t);
}

static DWORD WINAPI client(LPVOID arg) {
    (void)arg;
    int st;
    bool e, img;
    // the handshake
    char* r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"mcp-test\",\"version\":\"1\"}}}", NULL, &st);
    if (st != 200 || !has(r, "\"2025-06-18\"") || !has(r, "\"rivora-engine\"") || !has(r, "get_started")) bad("initialize", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL, &st);
    if (st != 202) bad("a notification isn't 202", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", NULL, &st);
    int ntools = 0;
    for (const char* p = r; p && (p = strstr(p, "\"inputSchema\"")); p++) ntools++;
    printf("[RealEngine] mcp: initialize ok, %d tools listed\n", ntools);
    if (ntools < 27) bad("tools/list (27 tools: the 22 and recipe, batch, inspect_level, tool_help, search)", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/list\"}", NULL, &st);
    if (!has(r, "build_scene") || !has(r, "make_playable")) bad("prompts/list", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{\"uri\":\"rivora://guide\"}}", NULL, &st);
    if (!has(r, "Conventions") || !has(r, "recipe")) bad("resources/read rivora://guide", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/read\",\"params\":{\"uri\":\"rivora://recipes\"}}", NULL, &st);
    if (!has(r, "test_level") || !has(r, "pbr_material")) bad("resources/read rivora://recipes", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/read\",\"params\":{\"uri\":\"rivora://state\"}}", NULL, &st);
    if (!has(r, "Issues")) bad("resources/read rivora://state", r);
    free(r);
    // a web page's request (DNS rebinding) refused
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}", "Origin: http://evil.example\r\n", &st);
    if (st != 403) bad("a foreign Origin wasn't refused", r);
    free(r);
    r = http("GET", NULL, NULL, &st);
    if (st != 405) bad("GET /mcp isn't 405", r);
    free(r);

    // the tools
    char* t = call("get_started", "{}", &e, NULL);
    if (e || !has(t, "\"guide\"") || !has(t, "\"state\"")) bad("get_started", t);
    free(t);
    t = call("spawn_actors", "{\"actors\":[{\"type\":\"Cube\",\"name\":\"MCP_Cube\",\"location\":[100,200,50],\"scale\":[2,2,2]},"
                             "{\"type\":\"point light\",\"name\":\"MCP_Light\",\"location\":[0,0,300],\"properties\":{\"Intensity\":1234}},"
                             "{\"type\":\"Nope\"}]}", &e, NULL);
    printf("[RealEngine] mcp: spawn_actors -> %.300s\n", t);
    if (e || !has(t, "MCP_Cube") || !has(t, "MCP_Light") || !has(t, "no type 'Nope'")) bad("spawn_actors", t);
    free(t);
    t = call("modify_actors", "{\"actors\":[{\"actor\":\"MCP_Cube\",\"offset\":[0,0,100]},{\"actor\":\"MCP_Light\",\"properties\":{\"Light Color\":[1,0.5,0.25]}},{\"actor\":\"Ghost\"}]}", &e, NULL);
    if (e || !has(t, "\"modified\":2") || !has(t, "no actor 'Ghost'")) bad("modify_actors", t);
    free(t);
    t = call("get_actor", "{\"actor\":\"MCP_Light\",\"filter\":\"Intensity\"}", &e, NULL);
    printf("[RealEngine] mcp: get_actor -> %.300s\n", t);
    if (e || !has(t, "1234")) bad("get_actor (Intensity 1234)", t);
    free(t);
    t = call("get_actor", "{\"actor\":\"MCP_Cube\",\"properties\":false}", &e, NULL);
    if (e || !has(t, "[100,200,150]")) bad("get_actor (the cube moved up 100)", t);
    free(t);
    t = call("find_actors", "{\"query\":\"MCP_\"}", &e, NULL);
    if (e || !has(t, "\"total\":2")) bad("find_actors", t);
    free(t);
    t = call("run_script", "{\"code\":\"for i in range(3):\\n    print(f\\\"n={i}\\\")\"}", &e, NULL);
    printf("[RealEngine] mcp: run_script (python) -> %.300s\n", t);
    if (e || !has(t, "n=2")) bad("run_script printed", t);
    free(t);
    t = call("run_script", "{\"language\":\"real\",\"code\":\"var x = = 1\"}", &e, NULL);
    printf("[RealEngine] mcp: run_script (an error) -> %.300s\n", t);
    if (!e || !has(t, "\"line\":1")) bad("run_script's error on line 1", t);
    free(t);
    t = call("api_reference", "{\"query\":\"actor location\",\"limit\":3}", &e, NULL);
    if (e || !has(t, "signature")) bad("api_reference", t);
    free(t);
    t = call("console", "{\"command\":\"r.ScreenPercentage\"}", &e, NULL);
    if (e || !has(t, "\"ok\":true")) bad("console", t);
    free(t);
    t = call("set_camera", "{\"location\":[-600,0,400],\"look_at\":[0,0,100]}", &e, NULL);
    if (e) bad("set_camera", t);
    free(t);
    t = call("screenshot", "{\"max_width\":320}", &e, &img);
    printf("[RealEngine] mcp: screenshot -> %.200s, a picture: %d\n", t, img);
    if (e || !img || !has(t, "\"width\":320")) bad("screenshot", t);
    free(t);
    t = call("delete_actors", "{\"actors\":[\"MCP_Cube\"]}", &e, NULL);
    if (e || !has(t, "\"deleted\":1")) bad("delete_actors", t);
    free(t);
    t = call("undo", "{}", &e, NULL);
    free(t);
    t = call("find_actors", "{\"query\":\"MCP_Cube\"}", &e, NULL);
    if (!has(t, "\"total\":1")) bad("undo brought the cube back", t);
    free(t);
    t = call("play", "{\"action\":\"start\",\"seconds\":1}", &e, NULL);
    printf("[RealEngine] mcp: play 1 s -> %.200s\n", t);
    if (e || !has(t, "stopped")) bad("play for a second", t);
    free(t);
    test_guide();
    t = call("no_such_tool", "{}", &e, NULL);
    if (!e) bad("an unknown tool isn't an error", t);
    free(t);
    // the time of a call there and back (find_actors: the HTTP, the queue, a frame at most, the answer)
    double t0 = re_time_seconds();
    for (int i = 0; i < 20; i++) free(call("get_editor_state", "{}", &e, NULL));
    double ms = (re_time_seconds() - t0) * 1000.0 / 20.0;
    printf("[RealEngine] mcp: a call there and back takes %.2f ms (20 get_editor_state)\n", ms);
    test_bridge();
    InterlockedExchange(&T.done, 1);
    return 0;
}

bool mcp_test_frame(Editor* ed, bool* ok) {
    (void)ed;
    T.frames++;
    if (T.frames == 30) {
        if (!mcp_server_running()) {
            bad("the server didn't start", mcp_server_error());
            *ok = false;
            return true;
        }
        T.thread = CreateThread(NULL, 0, client, NULL, 0, NULL);
        T.t_start = re_time_seconds();
    }
    if (T.frames > 30 && T.done) {
        WaitForSingleObject(T.thread, 5000);
        *ok = !T.failed;
        printf("[RealEngine] mcp: %s\n", T.failed ? "FAILED" : "ok");
        return true;
    }
    if (T.frames > 30 && re_time_seconds() - T.t_start > 240.0) {
        bad("timed out", NULL);
        *ok = false;
        return true;
    }
    return false;
}
