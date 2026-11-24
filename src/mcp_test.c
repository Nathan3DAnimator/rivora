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

#include "editor.h"
#include "io_json.h"
#include "mcp.h"
#include "platform.h"

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
    if (ntools < 20) bad("tools/list", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/list\"}", NULL, &st);
    if (!has(r, "build_scene")) bad("prompts/list", r);
    free(r);
    r = http("POST", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{\"uri\":\"rivora://guide\"}}", NULL, &st);
    if (!has(r, "Conventions")) bad("resources/read rivora://guide", r);
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
