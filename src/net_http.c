#include "net_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

void http_response_free(HttpResponse* r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

void http_url_encode(const char* in, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* c = (const unsigned char*)in; *c && o + 4 < cap; c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' ||
            *c == '.' || *c == '~') {
            out[o++] = (char)*c;
        } else {
            out[o++] = '%';
            out[o++] = hex[*c >> 4];
            out[o++] = hex[*c & 15];
        }
    }
    out[o] = 0;
}

size_t http_base64(const uint8_t* in, size_t n, char* out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

static wchar_t* widen(const char* s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w) {
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
        else w[0] = 0;
    }
    return w;
}

static void set_error(HttpResponse* out, const char* what) {
    DWORD e = GetLastError();
    const char* why = "";
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: why = "tempo esgotado"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: why = "endereço não encontrado (sem internet?)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: why = "não foi possível conectar"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: why = "a conexão caiu"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: why = "certificado inválido"; break;
    case ERROR_WINHTTP_OPERATION_CANCELLED: why = "cancelado"; break;
    default: break;
    }
    snprintf(out->error, sizeof out->error, "%s: %s (%lu)", what, why[0] ? why : "erro", (unsigned long)e);
}

bool http_request(const HttpRequest* req, HttpResponse* out) {
    memset(out, 0, sizeof *out);
    wchar_t* wurl = widen(req->url);
    if (!wurl) return false;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(out->error, sizeof out->error, "URL inválida: %s", req->url);
        free(wurl);
        return false;
    }
    free(wurl);
    wchar_t object[4096];
    _snwprintf(object, 4096, L"%ls%ls", path[0] ? path : L"/", extra);
    object[4095] = 0;
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    int timeout = req->timeout_ms > 0 ? req->timeout_ms : 30000;

    bool ok = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    wchar_t* wout = NULL;
    wchar_t wpart[1100] = L"";
    HINTERNET ses = WinHttpOpen(L"RealEngine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = NULL, rq = NULL;
    if (!ses) {
        set_error(out, "WinHttpOpen");
        goto done;
    }
    WinHttpSetTimeouts(ses, timeout, timeout, timeout, timeout);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        set_error(out, "conectar");
        goto done;
    }
    wchar_t* wmethod = widen(req->method ? req->method : "GET");
    rq = WinHttpOpenRequest(con, wmethod, object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                            https ? WINHTTP_FLAG_SECURE : 0);
    free(wmethod);
    if (!rq) {
        set_error(out, "abrir o pedido");
        goto done;
    }
    wchar_t* wheaders = widen(req->headers);
    BOOL sent = WinHttpSendRequest(rq, wheaders ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS, wheaders ? (DWORD)-1L : 0,
                                   (LPVOID)req->body, (DWORD)req->body_len, (DWORD)req->body_len, 0);
    free(wheaders);
    if (!sent || !WinHttpReceiveResponse(rq, NULL)) {
        set_error(out, "enviar o pedido");
        goto done;
    }
    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl,
                        WINHTTP_NO_HEADER_INDEX);
    out->status = (int)status;
    uint64_t total = 0;
    wchar_t clen[32];
    DWORD cl = sizeof clen;
    if (WinHttpQueryHeaders(rq, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &cl, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, NULL, 10);

    bool to_file = req->out_path && status >= 200 && status < 300;
    if (to_file) {
        wout = widen(req->out_path);
        _snwprintf(wpart, 1100, L"%ls.part", wout ? wout : L"");
        wpart[1099] = 0;
        file = CreateFileW(wpart, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            snprintf(out->error, sizeof out->error, "não foi possível gravar %s", req->out_path);
            goto done;
        }
    }
    size_t cap = 0;
    uint64_t done_bytes = 0;
    uint8_t* chunk = malloc(1 << 16);
    if (!chunk) goto done;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(rq, chunk, 1 << 16, &got)) {
            set_error(out, "receber");
            free(chunk);
            goto done;
        }
        if (!got) break;
        if (to_file) {
            DWORD w = 0;
            if (!WriteFile(file, chunk, got, &w, NULL) || w != got) {
                snprintf(out->error, sizeof out->error, "disco cheio? não foi possível gravar %s", req->out_path);
                free(chunk);
                goto done;
            }
        } else {
            if (out->len + got + 1 > cap) {
                size_t nc = cap ? cap * 2 : 1 << 16;
                while (nc < out->len + got + 1) nc *= 2;
                uint8_t* nb = realloc(out->body, nc);
                if (!nb) {
                    snprintf(out->error, sizeof out->error, "sem memória");
                    free(chunk);
                    goto done;
                }
                out->body = nb;
                cap = nc;
            }
            memcpy(out->body + out->len, chunk, got);
            out->len += got;
            out->body[out->len] = 0;
        }
        done_bytes += got;
        if (req->progress && !req->progress(req->user, done_bytes, total)) {
            snprintf(out->error, sizeof out->error, "cancelado");
            free(chunk);
            goto done;
        }
    }
    free(chunk);
    if (to_file) {
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (total && done_bytes != total) {
            snprintf(out->error, sizeof out->error, "download incompleto (%llu de %llu bytes)",
                     (unsigned long long)done_bytes, (unsigned long long)total);
            goto done;
        }
        if (!MoveFileExW(wpart, wout, MOVEFILE_REPLACE_EXISTING)) {
            snprintf(out->error, sizeof out->error, "não foi possível gravar %s", req->out_path);
            goto done;
        }
        wpart[0] = 0;
    }
    if (!out->body && !to_file) {
        out->body = calloc(1, 1);
    }
    ok = true;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (wpart[0]) DeleteFileW(wpart);
    free(wout);
    if (rq) WinHttpCloseHandle(rq);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!ok) http_response_free(out);
    return ok;
}

#else
bool http_request(const HttpRequest* req, HttpResponse* out) {
    (void)req;
    memset(out, 0, sizeof *out);
    snprintf(out->error, sizeof out->error, "HTTP só no Windows");
    return false;
}
#endif
