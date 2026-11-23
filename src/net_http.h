// HTTP(S) requests through Windows' WinHTTP (the updater and the report uploads; any thread). No UI; the tests talk to
// a server of their own on 127.0.0.1. Redirects are followed (GitHub's release assets redirect to its storage).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HttpResponse {
    int status;     // HTTP status (0: no answer)
    uint8_t* body;  // malloc'd, NUL-terminated (len does not count it); NULL when the body went to a file
    size_t len;
    char error[256]; // why it failed (the connection, a timeout...), "" when the server answered
} HttpResponse;

// The download's progress: done and total bytes (total 0: unknown). Return false to cancel.
typedef bool (*HttpProgress)(void* user, uint64_t done, uint64_t total);

typedef struct HttpRequest {
    const char* method;  // "GET" (NULL), "POST"...
    const char* url;     // http:// or https://
    const char* headers; // extra headers, "Name: value\r\n" each (NULL: none)
    const void* body;
    size_t body_len;
    const char* out_path; // the body goes into this file (written as .part, then renamed) instead of memory
    HttpProgress progress;
    void* user;
    int timeout_ms;       // per step (resolve, connect, send, receive); 0: 30 s
} HttpRequest;

// True when the server answered (any status: check out->status). Free with http_response_free.
bool http_request(const HttpRequest* req, HttpResponse* out);
void http_response_free(HttpResponse* r);

// Percent-encoding of a query value (UTF-8 in, ASCII out): the browser fallback of the uploads.
void http_url_encode(const char* in, char* out, size_t cap);
// Base64 (the attachments of an upload in JSON). out: 4 * ceil(n / 3) + 1.
size_t http_base64(const uint8_t* in, size_t n, char* out);
