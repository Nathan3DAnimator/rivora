#include "re_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

typedef struct HashCtx {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
} HashCtx;

static bool hash_begin(HashCtx* c) {
    memset(c, 0, sizeof *c);
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) return false;
    if (!NT_SUCCESS(BCryptCreateHash(c->alg, &c->h, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(c->alg, 0);
        return false;
    }
    return true;
}

static void hash_end(HashCtx* c) {
    if (c->h) BCryptDestroyHash(c->h);
    if (c->alg) BCryptCloseAlgorithmProvider(c->alg, 0);
}

bool re_sha256(const void* data, size_t len, uint8_t out[RE_SHA256_LEN]) {
    HashCtx c;
    if (!hash_begin(&c)) return false;
    bool ok = true;
    const uint8_t* p = data;
    while (ok && len) { // BCryptHashData takes a ULONG
        ULONG n = len > 0x40000000u ? 0x40000000u : (ULONG)len;
        ok = NT_SUCCESS(BCryptHashData(c.h, (PUCHAR)p, n, 0));
        p += n;
        len -= n;
    }
    ok = ok && NT_SUCCESS(BCryptFinishHash(c.h, out, RE_SHA256_LEN, 0));
    hash_end(&c);
    return ok;
}

bool re_sha256_file(const char* path_utf8, uint8_t out[RE_SHA256_LEN]) {
    wchar_t wpath[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, 1024)) return false;
    FILE* f = _wfopen(wpath, L"rb");
    if (!f) return false;
    HashCtx c;
    if (!hash_begin(&c)) {
        fclose(f);
        return false;
    }
    enum { BLOCK = 1 << 20 };
    uint8_t* buf = malloc(BLOCK);
    bool ok = buf != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, BLOCK, f)) > 0) ok = NT_SUCCESS(BCryptHashData(c.h, buf, (ULONG)n, 0));
    ok = ok && !ferror(f) && NT_SUCCESS(BCryptFinishHash(c.h, out, RE_SHA256_LEN, 0));
    free(buf);
    hash_end(&c);
    fclose(f);
    return ok;
}

// A key of the ECDSA P-256 provider from a CNG blob (header + the coordinates).
static BCRYPT_KEY_HANDLE import_key(BCRYPT_ALG_HANDLE alg, const uint8_t* bytes, int n, bool priv) {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + RE_P256_PRIV_LEN];
    BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob;
    hdr->dwMagic = priv ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    memcpy(blob + sizeof *hdr, bytes, (size_t)n);
    BCRYPT_KEY_HANDLE key = NULL;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg, NULL, priv ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                        (ULONG)(sizeof *hdr + n), 0)))
        return NULL;
    return key;
}

bool re_p256_verify(const uint8_t pub[RE_P256_PUB_LEN], const uint8_t hash[RE_SHA256_LEN], const uint8_t sig[RE_P256_SIG_LEN]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return false;
    BCRYPT_KEY_HANDLE key = import_key(alg, pub, RE_P256_PUB_LEN, false);
    bool ok = key && NT_SUCCESS(BCryptVerifySignature(key, NULL, (PUCHAR)hash, RE_SHA256_LEN, (PUCHAR)sig, RE_P256_SIG_LEN, 0));
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool re_p256_keygen(uint8_t pub[RE_P256_PUB_LEN], uint8_t priv[RE_P256_PRIV_LEN]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return false;
    BCRYPT_KEY_HANDLE key = NULL;
    bool ok = NT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 256, 0)) && NT_SUCCESS(BCryptFinalizeKeyPair(key, 0));
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + RE_P256_PRIV_LEN];
    ULONG got = 0;
    ok = ok && NT_SUCCESS(BCryptExportKey(key, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &got, 0)) &&
         got == sizeof blob;
    if (ok) {
        memcpy(priv, blob + sizeof(BCRYPT_ECCKEY_BLOB), RE_P256_PRIV_LEN);
        memcpy(pub, priv, RE_P256_PUB_LEN);
    }
    SecureZeroMemory(blob, sizeof blob);
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool re_p256_sign(const uint8_t priv[RE_P256_PRIV_LEN], const uint8_t hash[RE_SHA256_LEN], uint8_t sig[RE_P256_SIG_LEN]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return false;
    BCRYPT_KEY_HANDLE key = import_key(alg, priv, RE_P256_PRIV_LEN, true);
    ULONG got = 0;
    bool ok = key && NT_SUCCESS(BCryptSignHash(key, NULL, (PUCHAR)hash, RE_SHA256_LEN, sig, RE_P256_SIG_LEN, &got, 0)) &&
              got == RE_P256_SIG_LEN;
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

#else
bool re_sha256(const void* data, size_t len, uint8_t out[RE_SHA256_LEN]) { (void)data; (void)len; (void)out; return false; }
bool re_sha256_file(const char* p, uint8_t out[RE_SHA256_LEN]) { (void)p; (void)out; return false; }
bool re_p256_verify(const uint8_t* a, const uint8_t* b, const uint8_t* c) { (void)a; (void)b; (void)c; return false; }
bool re_p256_keygen(uint8_t* a, uint8_t* b) { (void)a; (void)b; return false; }
bool re_p256_sign(const uint8_t* a, const uint8_t* b, uint8_t* c) { (void)a; (void)b; (void)c; return false; }
#endif

void re_hex_encode(const uint8_t* in, size_t n, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int re_hex_decode(const char* text, uint8_t* out, size_t n) {
    size_t got = 0;
    int hi = -1;
    for (const char* c = text; *c; c++) {
        if (got == n && hi < 0) break; // "<hex>  file.zip" (sha256sum's form): the name after it is not read
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n' || *c == ':') continue;
        int v = hexval(*c);
        if (v < 0) break; // "<hex>  file.zip" (sha256sum's form): stop at the name
        if (hi < 0) {
            hi = v;
        } else {
            if (got >= n) return -1;
            out[got++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return got == n && hi < 0 ? (int)got : -1;
}
