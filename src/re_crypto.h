// SHA-256 and ECDSA P-256 through Windows' CNG (bcrypt): the updater checks that a downloaded package is the one the
// release was signed with (updater.h). Also in the tests and in realengine_updater.exe (key pair and signature of a
// package: tools/make_dist.ps1 -SignKey). No secret ever lives in the program: only the public key.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RE_SHA256_LEN 32
#define RE_P256_PUB_LEN 64  // X || Y, big-endian
#define RE_P256_PRIV_LEN 96 // X || Y || d (the CNG private blob without its header)
#define RE_P256_SIG_LEN 64  // r || s

bool re_sha256(const void* data, size_t len, uint8_t out[RE_SHA256_LEN]);
// The whole file, read in blocks (a package of hundreds of MB).
bool re_sha256_file(const char* path_utf8, uint8_t out[RE_SHA256_LEN]);

// The signature of a SHA-256 by the private key's owner; false when it does not match (or the key is invalid).
bool re_p256_verify(const uint8_t pub[RE_P256_PUB_LEN], const uint8_t hash[RE_SHA256_LEN], const uint8_t sig[RE_P256_SIG_LEN]);
bool re_p256_keygen(uint8_t pub[RE_P256_PUB_LEN], uint8_t priv[RE_P256_PRIV_LEN]);
bool re_p256_sign(const uint8_t priv[RE_P256_PRIV_LEN], const uint8_t hash[RE_SHA256_LEN], uint8_t sig[RE_P256_SIG_LEN]);

// Hex text <-> bytes (lower case out; any case and spaces in). re_hex_decode returns the bytes read (exactly n or -1).
void re_hex_encode(const uint8_t* in, size_t n, char* out); // out: 2n + 1
int re_hex_decode(const char* text, uint8_t* out, size_t n);
