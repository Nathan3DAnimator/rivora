// The MCP server's transport and protocol (mcp.h, notas-dev/mcp.md): Streamable HTTP on 127.0.0.1 (POST /mcp with one
// JSON-RPC message, answered with JSON), a thread per connection, the tool calls queued for the main thread
// (mcp_server_frame) and their answers sent when it is done with them. The tools themselves: mcp_tools.c.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "mcp.h"
#include "mcp_internal.h"
#include "platform.h"

static const char* const k_versions[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };

static struct {
    SOCKET listener;
    HANDLE thread;
    volatile LONG running;
    int port;
    char error[200];
    char client[64];
    volatile LONG calls;
    CRITICAL_SECTION lock; // the queue, the client's name
    bool lock_ready;
    McpCall* queue[64];
    int qhead, qcount;
    McpCall* current; // the main thread's (a call can take several frames: a picture, a Play)
} S = { INVALID_SOCKET };

static void lock_init(void) {
    if (!S.lock_ready) InitializeCriticalSection(&S.lock), S.lock_ready = true;
}

bool mcp_server_running(void) { return S.running != 0; }
int mcp_server_port(void) { return S.port; }
const char* mcp_server_error(void) { return S.error; }
unsigned mcp_server_calls(void) { return (unsigned)S.calls; }
const char* mcp_server_client(void) { return S.client; }

// --- The calls handed to the main thread ---
static void call_free(McpCall* c) {
    if (!c) return;
    json_free(&c->doc);
    jw_free(&c->body);
    free(c->image_b64);
    free(c->answer);
    if (c->done) CloseHandle(c->done);
    free(c);
}

// The main thread is done with it: its answer made; the waiting thread woken (or, when it gave up, the call freed)
static void call_finish(McpCall* c) {
    EnterCriticalSection(&S.lock);
    bool abandoned = c->abandoned;
    c->finished = true;
    LeaveCriticalSection(&S.lock);
    if (abandoned) call_free(c);
    else SetEvent(c->done);
}

// Queued and waited for (timeout_ms): 0 answered; 1 never taken (the caller frees it); 2 the main thread has it and
// frees it when done (the caller must not touch it again)
static int call_wait(McpCall* c, DWORD timeout_ms) {
    EnterCriticalSection(&S.lock);
    bool queued = S.qcount < (int)(sizeof S.queue / sizeof S.queue[0]);
    if (queued) S.queue[(S.qhead + S.qcount++) % 64] = c;
    LeaveCriticalSection(&S.lock);
    if (!queued) return 1;
    if (WaitForSingleObject(c->done, timeout_ms) == WAIT_OBJECT_0) return 0;
    int r = 2;
    EnterCriticalSection(&S.lock);
    if (c->finished) r = 0;
    else {
        for (int i = 0; i < S.qcount && r == 2; i++)
            if (S.queue[(S.qhead + i) % 64] == c) {
                for (int k = i; k < S.qcount - 1; k++) S.queue[(S.qhead + k) % 64] = S.queue[(S.qhead + k + 1) % 64];
                S.qcount--, r = 1;
            }
        if (r == 2) c->abandoned = true;
    }
    LeaveCriticalSection(&S.lock);
    return r;
}

void mcp_server_frame(struct Editor* ed, double now) {
    if (!S.lock_ready) return;
    double t0 = re_time_seconds();
    for (int n = 0; n < 32; n++) { // (quick calls one after another in a frame, ~6 ms at most)
        if (!S.current) {
            EnterCriticalSection(&S.lock);
            if (S.qcount) S.current = S.queue[S.qhead], S.qhead = (S.qhead + 1) % 64, S.qcount--;
            LeaveCriticalSection(&S.lock);
            if (!S.current) return;
            S.current->t_start = now;
        }
        McpCall* c = S.current;
        if (!mcp_tools_run(c, ed, now)) return; // (more frames: a picture, a Play, a bake)
        mcp_compose_answer(c);
        S.current = NULL;
        InterlockedIncrement(&S.calls);
        call_finish(c);
        if ((re_time_seconds() - t0) * 1000.0 > 6.0) return;
    }
}

// --- JSON-RPC ---
static char* rpc_error(const JsonValue* id, int code, const char* msg) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_key(&w, "id");
    mcp_write_id(&w, id);
    jw_key(&w, "error");
    jw_obj_begin(&w);
    jw_kint(&w, "code", code);
    jw_kstr(&w, "message", msg);
    jw_obj_end(&w);
    jw_obj_end(&w);
    char* out = mcp_jw_take(&w);
    return out;
}

static char* rpc_result(const JsonValue* id, const char* result_json) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_key(&w, "id");
    mcp_write_id(&w, id);
    jw_key(&w, "result");
    jw_raw(&w, result_json);
    jw_obj_end(&w);
    return mcp_jw_take(&w);
}

static char* handle_initialize(const JsonValue* id, const JsonValue* params) {
    const char* want = json_get_str(params, "protocolVersion", MCP_PROTOCOL);
    const char* ver = MCP_PROTOCOL;
    for (size_t i = 0; i < sizeof k_versions / sizeof k_versions[0]; i++)
        if (!strcmp(want, k_versions[i])) ver = k_versions[i];
    const JsonValue* ci = json_get(params, "clientInfo");
    lock_init();
    EnterCriticalSection(&S.lock);
    snprintf(S.client, sizeof S.client, "%s", json_get_str(ci, "name", "?"));
    LeaveCriticalSection(&S.lock);
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "protocolVersion", ver);
    jw_key(&w, "capabilities");
    jw_obj_begin(&w);
    jw_key(&w, "tools"), jw_obj_begin(&w), jw_kbool(&w, "listChanged", false), jw_obj_end(&w);
    jw_key(&w, "resources"), jw_obj_begin(&w), jw_kbool(&w, "listChanged", false), jw_obj_end(&w);
    jw_key(&w, "prompts"), jw_obj_begin(&w), jw_kbool(&w, "listChanged", false), jw_obj_end(&w);
    jw_obj_end(&w);
    jw_key(&w, "serverInfo");
    jw_obj_begin(&w);
    jw_kstr(&w, "name", "rivora-engine");
    jw_kstr(&w, "title", "Rivora Engine");
    jw_kstr(&w, "version", RE_VERSION_STRING);
    jw_obj_end(&w);
    jw_kstr(&w, "instructions", mcp_instructions());
    jw_obj_end(&w);
    char* res = mcp_jw_take(&w);
    char* out = res ? rpc_result(id, res) : NULL;
    free(res);
    return out;
}

// A request the main thread answers (a tool, a resource, a prompt that reads the editor): queued and waited for
static char* handle_on_main(const JsonValue* id, const char* method, const JsonValue* params, const char* body, size_t len) {
    McpCall* c = calloc(1, sizeof *c);
    if (!c) return rpc_error(id, -32603, "out of memory");
    // its own copy of the message (the arguments are read on the main thread)
    char err[160];
    if (!json_parse(body, len, &c->doc, err, sizeof err)) {
        free(c);
        return rpc_error(id, -32700, err);
    }
    const JsonValue* p = json_get(&c->doc.root, "params");
    snprintf(c->method, sizeof c->method, "%s", method);
    snprintf(c->name, sizeof c->name, "%s", json_get_str(p, "name", json_get_str(p, "uri", "")));
    c->args = json_get(p, "arguments");
    c->id = json_get(&c->doc.root, "id");
    (void)params;
    jw_init(&c->body);
    c->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    lock_init();
    // (a Play of N seconds or a bake can take long: 10 minutes before giving up)
    int r = call_wait(c, 600000);
    if (r) {
        if (r == 1) call_free(c);
        return rpc_error(id, -32603, "The editor didn't answer in time: it may be busy (a dialog, the Project Browser, a long load). Try again in a moment.");
    }
    char* out = c->answer;
    c->answer = NULL;
    call_free(c);
    return out;
}

char* mcp_handle_message(const char* body, size_t len) {
    JsonDoc doc;
    char err[160];
    if (!json_parse(body, len, &doc, err, sizeof err)) return rpc_error(NULL, -32700, err);
    const JsonValue* root = &doc.root;
    char* out = NULL;
    if (root->type == JSON_ARRAY) {
        out = rpc_error(NULL, -32600, "Batches aren't supported: send one message at a time");
        json_free(&doc);
        return out;
    }
    const char* method = json_get_str(root, "method", NULL);
    const JsonValue* id = json_get(root, "id");
    const JsonValue* params = json_get(root, "params");
    if (!method || !id) out = NULL; // (an answer from the client, or a notification: initialized, cancelled...)
    else if (!strcmp(method, "initialize")) out = handle_initialize(id, params);
    else if (!strcmp(method, "ping")) out = rpc_result(id, "{}");
    else if (!strcmp(method, "tools/list")) out = rpc_result(id, mcp_tools_list_json());
    else if (!strcmp(method, "prompts/list")) out = rpc_result(id, mcp_prompts_list_json());
    else if (!strcmp(method, "prompts/get")) {
        char* r = mcp_prompt_get(params);
        out = r ? rpc_result(id, r) : rpc_error(id, -32602, "Unknown prompt");
        free(r);
    } else if (!strcmp(method, "resources/list")) out = rpc_result(id, mcp_resources_list_json());
    else if (!strcmp(method, "resources/templates/list")) out = rpc_result(id, mcp_resource_templates_json());
    else if (!strcmp(method, "tools/call") || !strcmp(method, "resources/read")) {
        if (!json_get_str(params, !strcmp(method, "tools/call") ? "name" : "uri", NULL)) out = rpc_error(id, -32602, "Missing the tool's name / the resource's uri");
        else out = handle_on_main(id, method, params, body, len);
    } else if (!strcmp(method, "logging/setLevel") || !strcmp(method, "completion/complete")) out = rpc_result(id, "{}");
    else out = rpc_error(id, -32601, "Method not found");
    json_free(&doc);
    return out;
}

// --- HTTP ---
static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = send(s, p, n > 0x40000000 ? 0x40000000 : (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}

static void reply(SOCKET s, int status, const char* reason, const char* type, const char* body, size_t len, const char* extra) {
    char head[512];
    int hn = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n%s%s%s%sCache-Control: no-store\r\n\r\n", status, reason, len,
                      type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "", extra ? extra : "");
    send_all(s, head, (size_t)hn);
    if (len) send_all(s, body, len);
}

// "localhost" / "127.0.0.1" / "[::1]" with or without a port (the Host and the Origin a client on this computer sends)
static bool local_name(const char* v) {
    while (*v == ' ') v++;
    const char* p = v;
    if (!_strnicmp(p, "http://", 7)) p += 7;
    else if (!_strnicmp(p, "https://", 8)) p += 8;
    size_t n = strcspn(p, ":/\r\n ");
    if (!strncmp(p, "[::1]", 5)) return true;
    return (n == 9 && !_strnicmp(p, "localhost", 9)) || (n == 9 && !strncmp(p, "127.0.0.1", 9));
}

static const char* header(const char* head, const char* name, char* out, size_t cap) {
    size_t nl = strlen(name);
    for (const char* p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char* l = p + 2;
        if (!_strnicmp(l, name, nl) && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ') l++;
            size_t n = strcspn(l, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, l, n), out[n] = 0;
            return out;
        }
    }
    return NULL;
}

static DWORD WINAPI conn_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    DWORD tmo = 15 * 60 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    size_t cap = 1 << 16, len = 0;
    char* buf = malloc(cap + 1);
    while (buf && S.running) {
        // the head
        char* end = NULL;
        while (!(end = (len ? strstr(buf, "\r\n\r\n") : NULL))) {
            if (len >= cap) goto done; // (a 64 KB head: not a client of ours)
            int k = recv(s, buf + len, (int)(cap - len), 0);
            if (k <= 0) goto done;
            len += (size_t)k, buf[len] = 0;
        }
        size_t head_len = (size_t)(end - buf) + 4;
        char hv[256], method[16] = "", path[256] = "";
        sscanf(buf, "%15s %255s", method, path);
        size_t body_len = 0;
        if (header(buf, "Content-Length", hv, sizeof hv)) body_len = (size_t)_strtoui64(hv, NULL, 10);
        if (body_len > (64u << 20)) goto done;
        if (head_len + body_len > cap) {
            size_t nc = head_len + body_len;
            char* nb = realloc(buf, nc + 1);
            if (!nb) goto done;
            buf = nb, cap = nc;
        }
        while (len < head_len + body_len) {
            int k = recv(s, buf + len, (int)(cap - len), 0);
            if (k <= 0) goto done;
            len += (size_t)k;
        }
        char saved = buf[head_len - 2];
        buf[head_len - 2] = 0; // (the head as a string for the lookups)
        // (DNS rebinding: a web page reaching 127.0.0.1 under another name; only this computer's own names and origins)
        bool ok_host = !header(buf, "Host", hv, sizeof hv) || local_name(hv);
        bool ok_origin = !header(buf, "Origin", hv, sizeof hv) || !strcmp(hv, "null") || local_name(hv);
        char sid[96] = "";
        header(buf, "Mcp-Session-Id", sid, sizeof sid);
        bool close_after = header(buf, "Connection", hv, sizeof hv) && !_stricmp(hv, "close");
        buf[head_len - 2] = saved;
        const char* body = buf + head_len;
        char extra[160] = "";
        if (!ok_host || !ok_origin) {
            static const char m[] = "Only clients on this computer can reach the Rivora editor.";
            reply(s, 403, "Forbidden", "text/plain", m, sizeof m - 1, NULL);
        } else if (strncmp(path, "/mcp", 4) && strcmp(path, "/")) {
            static const char m[] = "Rivora Engine MCP: POST JSON-RPC to /mcp";
            reply(s, 404, "Not Found", "text/plain", m, sizeof m - 1, NULL);
        } else if (!strcmp(method, "GET")) {
            if (!strcmp(path, "/")) {
                static const char m[] = "Rivora Engine MCP server: POST JSON-RPC to /mcp";
                reply(s, 200, "OK", "text/plain", m, sizeof m - 1, NULL);
            } else reply(s, 405, "Method Not Allowed", NULL, NULL, 0, "Allow: POST, DELETE\r\n"); // (no server-sent stream)
        } else if (!strcmp(method, "DELETE")) reply(s, 200, "OK", NULL, NULL, 0, NULL);
        else if (!strcmp(method, "OPTIONS")) reply(s, 204, "No Content", NULL, NULL, 0, "Allow: POST, DELETE\r\n");
        else if (!strcmp(method, "POST")) {
            bool is_init = body_len && strstr(body, "\"initialize\"") && memchr(body, '{', body_len);
            if (is_init || !sid[0]) snprintf(sid, sizeof sid, "rivora-%08lx%08lx", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId());
            snprintf(extra, sizeof extra, "Mcp-Session-Id: %s\r\n", sid);
            char* out = mcp_handle_message(body, body_len);
            if (out) reply(s, 200, "OK", "application/json", out, strlen(out), extra);
            else reply(s, 202, "Accepted", NULL, NULL, 0, extra);
            free(out);
        } else reply(s, 405, "Method Not Allowed", NULL, NULL, 0, "Allow: POST, DELETE\r\n");
        if (close_after) break; // (the client asked: one request on this connection)
        // the next request on this connection (keep-alive)
        size_t used = head_len + body_len;
        memmove(buf, buf + used, len - used);
        len -= used, buf[len] = 0;
    }
done:
    free(buf);
    shutdown(s, SD_BOTH);
    closesocket(s);
    return 0;
}

static DWORD WINAPI listen_thread(LPVOID arg) {
    (void)arg;
    while (S.running) {
        SOCKET c = accept(S.listener, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (!S.running) break;
            Sleep(20);
            continue;
        }
        HANDLE t = CreateThread(NULL, 0, conn_thread, (LPVOID)(uintptr_t)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    return 0;
}

bool mcp_server_start(int port) {
    if (S.running) {
        if (port == S.port) return true;
        mcp_server_stop();
    }
    lock_init();
    S.error[0] = 0;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) {
        snprintf(S.error, sizeof S.error, "WSAStartup failed");
        return false;
    }
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        snprintf(S.error, sizeof S.error, "socket() failed (%d)", WSAGetLastError());
        return false;
    }
    BOOL excl = TRUE; // (a second editor can't take the same port from under the first)
    setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(l, (struct sockaddr*)&a, sizeof a) || listen(l, 16)) {
        int e = WSAGetLastError();
        snprintf(S.error, sizeof S.error, e == WSAEADDRINUSE ? "Port %d is in use (another Rivora editor?)" : "Couldn't listen on port %d (%d)", port, e);
        closesocket(l);
        return false;
    }
    S.listener = l, S.port = port;
    InterlockedExchange(&S.running, 1);
    S.thread = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
    if (!S.thread) {
        InterlockedExchange(&S.running, 0);
        closesocket(l), S.listener = INVALID_SOCKET;
        snprintf(S.error, sizeof S.error, "Couldn't start the server's thread");
        return false;
    }
    printf("[RealEngine] MCP server on http://127.0.0.1:%d/mcp\n", port);
    return true;
}

void mcp_server_stop(void) {
    if (!S.running) return;
    InterlockedExchange(&S.running, 0);
    closesocket(S.listener), S.listener = INVALID_SOCKET; // (accept returns)
    if (S.thread) WaitForSingleObject(S.thread, 2000), CloseHandle(S.thread), S.thread = NULL;
    printf("[RealEngine] MCP server stopped\n");
}
