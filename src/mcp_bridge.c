// rivora_mcp.exe (mcp.h, notas-dev/mcp.md): the MCP stdio transport for the clients that start a program (Claude
// Desktop...). Each line of stdin (a JSON-RPC message) is POSTed to the editor's server (http://127.0.0.1:<port>/mcp)
// and its answer written to stdout as a line. When the editor isn't running it opens it (rivora.exe beside this program,
// with --mcp: the server on) and waits for it. Nothing of the editor is linked in: a few KB.
//   rivora_mcp.exe [--port N] [--no-launch]        (RIVORA_MCP_PORT also sets the port)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_port = 47860;
static bool g_no_launch, g_launched;
static char g_session[128];

static SOCKET connect_editor(void) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD tmo = 11 * 60 * 1000; // (the server gives up on a call after 10 minutes)
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    return s;
}

// rivora.exe (beside this program) opened with the server on; true once the port answers (90 s at most)
static bool launch_editor(void) {
    if (g_no_launch || g_launched) return false;
    g_launched = true;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* sl = wcsrchr(exe, L'\\');
    if (!sl) return false;
    wcscpy_s(sl + 1, MAX_PATH - (size_t)(sl + 1 - exe), L"rivora.exe");
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --mcp --mcp-port %d", exe, g_port);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    // (no handles inherited: the editor must not hold the client's pipes)
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    fprintf(stderr, "rivora_mcp: opening the Rivora editor...\n");
    for (int i = 0; i < 180; i++) {
        SOCKET s = connect_editor();
        if (s != INVALID_SOCKET) return closesocket(s), true;
        Sleep(500);
    }
    return false;
}

static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = send(s, p, (int)(n > 0x40000000 ? 0x40000000 : n), 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}

// One message posted; the answer's body (malloc'd; "" for 202) or NULL when the editor can't be reached
static char* post(const char* msg, size_t len, int* status) {
    SOCKET s = connect_editor();
    if (s == INVALID_SOCKET && launch_editor()) s = connect_editor();
    if (s == INVALID_SOCKET) return NULL;
    char head[512];
    int hn = snprintf(head, sizeof head,
                      "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n%s%s%s\r\n",
                      g_port, len, g_session[0] ? "Mcp-Session-Id: " : "", g_session, g_session[0] ? "\r\n" : "");
    if (!send_all(s, head, (size_t)hn) || !send_all(s, msg, len)) {
        closesocket(s);
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    char* buf = malloc(cap + 1);
    for (;;) {
        if (!buf) break;
        if (n == cap) {
            char* nb = realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb, cap *= 2;
        }
        int k = recv(s, buf + n, (int)(cap - n), 0);
        if (k <= 0) break;
        n += (size_t)k;
    }
    closesocket(s);
    if (!buf) return NULL;
    buf[n] = 0;
    char* body = strstr(buf, "\r\n\r\n");
    if (!body) {
        free(buf);
        return NULL;
    }
    *body = 0, body += 4;
    *status = atoi(buf + 9);
    char* sid = strstr(buf, "Mcp-Session-Id: ");
    if (sid) {
        sid += 16;
        size_t l = strcspn(sid, "\r\n");
        if (l < sizeof g_session) memcpy(g_session, sid, l), g_session[l] = 0;
    }
    char* out = _strdup(body);
    free(buf);
    return out;
}

// The request's "id" as written (a number or a string), for the error answer
static void id_of(const char* msg, char* out, size_t cap) {
    snprintf(out, cap, "null");
    const char* p = strstr(msg, "\"id\"");
    if (!p) return;
    p = strchr(p + 4, ':');
    if (!p) return;
    p++;
    while (*p == ' ') p++;
    size_t l = *p == '"' ? strcspn(p + 1, "\"") + 2 : strcspn(p, ",}] ");
    if (l && l < cap) memcpy(out, p, l), out[l] = 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-launch")) g_no_launch = true;
    }
    const char* ep = getenv("RIVORA_MCP_PORT");
    if (ep && atoi(ep) > 0) g_port = atoi(ep);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    size_t cap = 1 << 16;
    char* line = malloc(cap);
    while (line) {
        size_t n = 0;
        int ch;
        while ((ch = getchar()) != EOF && ch != '\n') {
            if (n + 1 >= cap) {
                char* nl = realloc(line, cap * 2);
                if (!nl) return 1;
                line = nl, cap *= 2;
            }
            line[n++] = (char)ch;
        }
        if (n && line[n - 1] == '\r') n--;
        line[n] = 0;
        if (n) {
            int status = 0;
            char* out = post(line, n, &status);
            bool is_request = strstr(line, "\"method\"") && strstr(line, "\"id\"");
            if (out && out[0]) fputs(out, stdout), fputc('\n', stdout);
            else if (!out && is_request) {
                char id[96];
                id_of(line, id, sizeof id);
                printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32000,\"message\":\"The Rivora editor isn't reachable on port %d. Open it and "
                       "turn on Tools > AI Connection (MCP) (or start it with --mcp).\"}}\n",
                       id, g_port);
            }
            fflush(stdout);
            free(out);
        }
        if (ch == EOF) break;
    }
    free(line);
    WSACleanup();
    return 0;
}
