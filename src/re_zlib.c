// zlib streams (RFC 1950/1951) of FBX arrays: a table-driven inflate into a buffer of known size, and a fast deflate
// (greedy LZ77 over a hash of 4 bytes, dynamic Huffman blocks, stored blocks when those are smaller), about zlib's
// level 1 in ratio.
#include <stdlib.h>
#include <string.h>

#include "re_zlib.h"
#include "platform.h"

uint32_t re_adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t k = n < 5552 ? n : 5552; // the largest run without overflowing 32 bits
        n -= k;
        while (k >= 8) {
            a += p[0]; b += a; a += p[1]; b += a; a += p[2]; b += a; a += p[3]; b += a;
            a += p[4]; b += a; a += p[5]; b += a; a += p[6]; b += a; a += p[7]; b += a;
            p += 8;
            k -= 8;
        }
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------------------------------------------------

#define FAST_BITS 10

typedef struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf;
    int cnt;
    int pad; // zero bytes fed past the end
} Bits;

static inline void bits_fill(Bits* b) {
    if (b->end - b->p >= 8) {
        uint64_t v;
        memcpy(&v, b->p, 8);
        b->buf |= v << b->cnt;
        b->p += (63 - b->cnt) >> 3;
        b->cnt |= 56;
        return;
    }
    while (b->cnt <= 56) {
        uint64_t byte = 0;
        if (b->p < b->end) byte = *b->p++;
        else b->pad++;
        b->buf |= byte << b->cnt;
        b->cnt += 8;
    }
}

static inline uint32_t bits_take(Bits* b, int n) {
    uint32_t v = (uint32_t)(b->buf & ((1ull << n) - 1));
    b->buf >>= n;
    b->cnt -= n;
    return v;
}

typedef struct Huff {
    uint16_t fast[1 << FAST_BITS]; // (length << 9) | symbol; 0: a longer code
    uint16_t count[16];
    uint16_t symbol[288];
} Huff;

static uint32_t bit_reverse(uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
    return r;
}

// False when the lengths over-subscribe the code space (incomplete codes are allowed, as zlib does for one distance).
static bool huff_build(Huff* h, const uint8_t* lengths, int n) {
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    uint16_t offs[16], next[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    uint32_t code = 0;
    for (int len = 1; len < 16; len++) { // canonical first code of each length
        next[len] = (uint16_t)code;
        code = (code + h->count[len]) << 1;
    }
    for (int i = 0; i < n; i++) {
        int len = lengths[i];
        if (!len) continue;
        h->symbol[offs[len]++] = (uint16_t)i;
        uint32_t c = next[len]++;
        if (len <= FAST_BITS) {
            uint32_t r = bit_reverse(c, len);
            for (uint32_t k = r; k < (1u << FAST_BITS); k += 1u << len) h->fast[k] = (uint16_t)((len << 9) | i);
        }
    }
    return true;
}

static inline int huff_decode(Bits* b, const Huff* h) {
    uint16_t e = h->fast[b->buf & ((1u << FAST_BITS) - 1)];
    if (e) {
        bits_take(b, e >> 9);
        return e & 511;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) { // canonical walk, a bit at a time (codes longer than FAST_BITS)
        code |= (int)((b->buf >> (len - 1)) & 1u);
        int count = h->count[len];
        if (code - count < first) {
            bits_take(b, len);
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LBASE[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool inflate_codes(Bits* b, const Huff* lit, const Huff* dist, uint8_t* out, size_t cap, size_t* pos) {
    size_t o = *pos;
    for (;;) {
        if (b->cnt < 48) bits_fill(b);
        if (b->pad > 16) return false;
        int sym = huff_decode(b, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (o >= cap) return false;
            out[o++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) break;
        sym -= 257;
        if (sym >= 29) return false;
        size_t len = LBASE[sym] + bits_take(b, LEXT[sym]);
        int ds = huff_decode(b, dist);
        if (ds < 0 || ds >= 30) return false;
        size_t d = DBASE[ds] + bits_take(b, DEXT[ds]);
        if (d > o || len > cap - o) return false;
        uint8_t* dst = out + o;
        const uint8_t* src = dst - d;
        if (d >= len) memcpy(dst, src, len);
        else
            for (size_t i = 0; i < len; i++) dst[i] = src[i];
        o += len;
    }
    *pos = o;
    return true;
}

static bool inflate_any(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, bool zlib);

bool re_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) { return inflate_any(in, in_len, out, out_len, true); }
bool re_inflate_raw(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) { return inflate_any(in, in_len, out, out_len, false); }

static bool inflate_any(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, bool zlib) {
    if (zlib) {
        if (in_len < 2) return false;
        uint32_t cmf = in[0], flg = in[1];
        if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 32)) return false;
    }
    Bits b = { in + (zlib ? 2 : 0), in + in_len, 0, 0, 0 };
    size_t o = 0;
    static Huff fixed_lit, fixed_dist;
    static volatile int32_t fixed_ready = 0;
    if (!re_atomic_load(&fixed_ready)) { // built once (racing threads build the same tables)
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        Huff fl, fd;
        huff_build(&fl, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        huff_build(&fd, l, 30);
        fixed_lit = fl;
        fixed_dist = fd;
        re_atomic_store(&fixed_ready, 1);
    }
    Huff* dyn = malloc(sizeof(Huff) * 2);
    if (!dyn) return false;
    bool ok = true;
    int last = 0;
    do {
        bits_fill(&b);
        last = (int)bits_take(&b, 1);
        int type = (int)bits_take(&b, 2);
        if (type == 0) { // stored: back to the byte boundary
            bits_take(&b, b.cnt & 7);
            const uint8_t* at = b.p - b.cnt / 8 + b.pad;
            if (at < in || at + 4 > b.end) {
                ok = false;
                break;
            }
            uint32_t len = (uint32_t)at[0] | ((uint32_t)at[1] << 8), nlen = (uint32_t)at[2] | ((uint32_t)at[3] << 8);
            at += 4;
            if ((len ^ 0xffffu) != nlen || (size_t)(b.end - at) < len || len > out_len - o) {
                ok = false;
                break;
            }
            memcpy(out + o, at, len);
            o += len;
            b.p = at + len;
            b.buf = 0;
            b.cnt = 0;
            b.pad = 0;
        } else if (type == 1) {
            ok = inflate_codes(&b, &fixed_lit, &fixed_dist, out, out_len, &o);
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = (int)bits_take(&b, 5) + 257, ndist = (int)bits_take(&b, 5) + 1, ncode = (int)bits_take(&b, 4) + 4;
            if (nlen > 286 || ndist > 30) {
                ok = false;
                break;
            }
            uint8_t lengths[320];
            memset(lengths, 0, sizeof lengths);
            bits_fill(&b);
            for (int i = 0; i < ncode; i++) { // up to 57 bits: refill on the way
                if (b.cnt < 3) bits_fill(&b);
                lengths[order[i]] = (uint8_t)bits_take(&b, 3);
            }
            if (!huff_build(&dyn[0], lengths, 19)) {
                ok = false;
                break;
            }
            int index = 0;
            memset(lengths, 0, sizeof lengths);
            while (index < nlen + ndist) {
                if (b.cnt < 32) bits_fill(&b);
                int sym = huff_decode(&b, &dyn[0]);
                if (sym < 0 || b.pad > 16) {
                    ok = false;
                    break;
                }
                if (sym < 16) {
                    lengths[index++] = (uint8_t)sym;
                    continue;
                }
                int len = 0, rep;
                if (sym == 16) {
                    if (index == 0) {
                        ok = false;
                        break;
                    }
                    len = lengths[index - 1];
                    rep = 3 + (int)bits_take(&b, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)bits_take(&b, 3);
                } else {
                    rep = 11 + (int)bits_take(&b, 7);
                }
                if (index + rep > nlen + ndist) {
                    ok = false;
                    break;
                }
                while (rep--) lengths[index++] = (uint8_t)len;
            }
            if (!ok) break;
            if (lengths[256] == 0 || !huff_build(&dyn[0], lengths, nlen) || !huff_build(&dyn[1], lengths + nlen, ndist)) {
                ok = false;
                break;
            }
            ok = inflate_codes(&b, &dyn[0], &dyn[1], out, out_len, &o);
        } else {
            ok = false;
        }
        if (b.pad > 16) ok = false;
    } while (ok && !last);
    free(dyn);
    if (!ok || o != out_len) return false;
    if (!zlib) return true; // raw deflate (zip entries): no trailer
    // Adler-32 of the data, big-endian, at the next byte
    bits_take(&b, b.cnt & 7);
    const uint8_t* at = b.p - b.cnt / 8 + b.pad;
    if (at < in || at + 4 > b.end) return true; // some writers leave it out
    uint32_t want = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
    return want == re_adler32(out, out_len);
}

// ---------------------------------------------------------------------------------------------------------------------
// Deflate
// ---------------------------------------------------------------------------------------------------------------------

typedef struct BitOut {
    uint8_t* p;
    size_t len, cap;
    uint64_t buf;
    int cnt;
    bool oom;
} BitOut;

static void out_reserve(BitOut* o, size_t extra) {
    if (o->len + extra <= o->cap) return;
    size_t nc = o->cap * 2 + extra + 1024;
    uint8_t* n = realloc(o->p, nc);
    if (!n) {
        o->oom = true;
        return;
    }
    o->p = n;
    o->cap = nc;
}

static inline void put_bits(BitOut* o, uint32_t v, int n) {
    o->buf |= (uint64_t)v << o->cnt;
    o->cnt += n;
    if (o->cnt >= 32) {
        if (o->len + 8 > o->cap) out_reserve(o, 64);
        if (o->oom) return;
        uint32_t w = (uint32_t)o->buf;
        memcpy(o->p + o->len, &w, 4);
        o->len += 4;
        o->buf >>= 32;
        o->cnt -= 32;
    }
}

static void put_align(BitOut* o) {
    while (o->cnt > 0) {
        out_reserve(o, 1);
        if (o->oom) return;
        o->p[o->len++] = (uint8_t)o->buf;
        o->buf >>= 8;
        o->cnt = o->cnt > 8 ? o->cnt - 8 : 0;
    }
    o->buf = 0;
    o->cnt = 0;
}

// Huffman code lengths limited to `limit` bits: the lengths of a Huffman tree over the frequencies, halving the
// frequencies until the tree fits (rare: needs very skewed counts).
static void code_lengths(const uint32_t* freq, int n, int limit, uint8_t* len) {
    uint32_t f[288];
    memcpy(f, freq, sizeof(uint32_t) * (size_t)n);
    for (;;) {
        int idx[288], m = 0;
        for (int i = 0; i < n; i++) {
            len[i] = 0;
            if (f[i]) idx[m++] = i;
        }
        if (m == 0) return;
        if (m == 1) {
            len[idx[0]] = 1;
            return;
        }
        // Two-queue Huffman: leaves sorted by frequency, internal nodes created in order
        for (int i = 1; i < m; i++) { // insertion sort (m <= 288)
            int k = idx[i], j = i - 1;
            while (j >= 0 && f[idx[j]] > f[k]) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = k;
        }
        uint64_t w[576];
        int parent[576];
        for (int i = 0; i < m; i++) w[i] = f[idx[i]];
        int leaf = 0, node = m, inner = m;
        while (inner < 2 * m - 1) {
            int pick[2];
            for (int k = 0; k < 2; k++) {
                if (leaf < m && (node >= inner || w[leaf] <= w[node])) pick[k] = leaf++;
                else pick[k] = node++;
            }
            w[inner] = w[pick[0]] + w[pick[1]];
            parent[pick[0]] = parent[pick[1]] = inner;
            inner++;
        }
        int depth[576];
        depth[2 * m - 2] = 0;
        int maxd = 0;
        for (int i = 2 * m - 3; i >= 0; i--) {
            depth[i] = depth[parent[i]] + 1;
            if (i < m && depth[i] > maxd) maxd = depth[i];
        }
        if (maxd <= limit) {
            for (int i = 0; i < m; i++) len[idx[i]] = (uint8_t)depth[i];
            return;
        }
        for (int i = 0; i < n; i++)
            if (f[i]) f[i] = (f[i] >> 1) | 1u;
    }
}

static void code_values(const uint8_t* len, int n, uint16_t* code) {
    uint16_t count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (int l = 1; l < 16; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = (uint16_t)c;
    }
    for (int i = 0; i < n; i++)
        if (len[i]) code[i] = (uint16_t)bit_reverse(next[len[i]]++, len[i]);
}

static int len_code(int len) { // 3..258 -> 0..28
    int i = 0;
    while (i < 28 && LBASE[i + 1] <= len) i++;
    return i;
}

static int dist_code(int d) { // 1..32768 -> 0..29
    int i = 0;
    while (i < 29 && DBASE[i + 1] <= d) i++;
    return i;
}

typedef struct Sym { uint16_t lit_or_len; uint16_t dist; } Sym; // dist 0: a literal

// One dynamic block of the symbols (or stored blocks of the raw bytes when smaller).
static void write_block(BitOut* o, const Sym* syms, int ns, const uint8_t* raw, size_t raw_len, bool last,
                        const uint8_t* lcode_of, const uint8_t* dcode_of) {
    uint32_t lf[286] = { 0 }, df[30] = { 0 };
    for (int i = 0; i < ns; i++) {
        if (syms[i].dist == 0) lf[syms[i].lit_or_len]++;
        else {
            lf[257 + lcode_of[syms[i].lit_or_len]]++;
            df[dcode_of[syms[i].dist <= 256 ? syms[i].dist - 1 : 256 + ((syms[i].dist - 1) >> 7)]]++;
        }
    }
    lf[256] = 1;
    uint8_t ll[286], dl[30];
    code_lengths(lf, 286, 15, ll);
    code_lengths(df, 30, 15, dl);
    int nlen = 286, ndist = 30;
    while (nlen > 257 && !ll[nlen - 1]) nlen--;
    while (ndist > 1 && !dl[ndist - 1]) ndist--;
    if (ndist == 1 && !dl[0]) dl[0] = 1; // one (unused) distance code
    // Run-length coded lengths
    uint8_t all[316];
    memcpy(all, ll, (size_t)nlen);
    memcpy(all + nlen, dl, (size_t)ndist);
    int total = nlen + ndist;
    uint8_t rle[316], rle_extra[316];
    int nr = 0;
    uint32_t cf[19] = { 0 };
    for (int i = 0; i < total;) {
        int v = all[i], run = 1;
        while (i + run < total && all[i + run] == v) run++;
        if (v == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            rle[nr] = r >= 11 ? 18 : 17;
            rle_extra[nr] = (uint8_t)(r >= 11 ? r - 11 : r - 3);
            cf[rle[nr++]]++;
            i += r;
        } else if (v != 0 && run >= 4) {
            rle[nr] = (uint8_t)v;
            cf[v]++;
            nr++;
            int r = run - 1 > 6 ? 6 : run - 1;
            rle[nr] = 16;
            rle_extra[nr] = (uint8_t)(r - 3);
            cf[16]++;
            nr++;
            i += 1 + r;
        } else {
            rle[nr++] = (uint8_t)v;
            cf[v]++;
            i++;
        }
    }
    uint8_t cl[19];
    code_lengths(cf, 19, 7, cl);
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int ncode = 19;
    while (ncode > 4 && !cl[order[ncode - 1]]) ncode--;
    // Size in bits: dynamic vs stored
    uint64_t bits = 3 + 14 + 3 * (uint64_t)ncode;
    for (int i = 0; i < nr; i++) bits += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    for (int i = 0; i < 286; i++) bits += (uint64_t)lf[i] * ll[i];
    for (int i = 0; i < 29; i++) bits += (uint64_t)lf[257 + i] * LEXT[i];
    for (int i = 0; i < 30; i++) bits += (uint64_t)df[i] * (dl[i] + DEXT[i]);
    uint64_t stored = ((raw_len + 65534) / 65535) * 40 + raw_len * 8 + 8;
    if (bits >= stored || raw_len == 0) {
        size_t pos = 0;
        do {
            size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
            put_bits(o, (last && pos + n >= raw_len) ? 1u : 0u, 1);
            put_bits(o, 0, 2);
            put_align(o);
            out_reserve(o, n + 4);
            if (o->oom) return;
            o->p[o->len++] = (uint8_t)n;
            o->p[o->len++] = (uint8_t)(n >> 8);
            o->p[o->len++] = (uint8_t)~n;
            o->p[o->len++] = (uint8_t)(~n >> 8);
            memcpy(o->p + o->len, raw + pos, n);
            o->len += n;
            pos += n;
        } while (pos < raw_len);
        return;
    }
    uint16_t lc[286] = { 0 }, dc[30] = { 0 }, cc[19] = { 0 };
    code_values(ll, 286, lc);
    code_values(dl, 30, dc);
    code_values(cl, 19, cc);
    put_bits(o, last ? 1u : 0u, 1);
    put_bits(o, 2, 2);
    put_bits(o, (uint32_t)(nlen - 257), 5);
    put_bits(o, (uint32_t)(ndist - 1), 5);
    put_bits(o, (uint32_t)(ncode - 4), 4);
    for (int i = 0; i < ncode; i++) put_bits(o, cl[order[i]], 3);
    for (int i = 0; i < nr; i++) {
        put_bits(o, cc[rle[i]], cl[rle[i]]);
        if (rle[i] == 16) put_bits(o, rle_extra[i], 2);
        else if (rle[i] == 17) put_bits(o, rle_extra[i], 3);
        else if (rle[i] == 18) put_bits(o, rle_extra[i], 7);
    }
    for (int i = 0; i < ns; i++) {
        if (syms[i].dist == 0) {
            put_bits(o, lc[syms[i].lit_or_len], ll[syms[i].lit_or_len]);
            continue;
        }
        int len = syms[i].lit_or_len, lci = lcode_of[len];
        put_bits(o, lc[257 + lci], ll[257 + lci]);
        if (LEXT[lci]) put_bits(o, (uint32_t)(len - LBASE[lci]), LEXT[lci]);
        int d = syms[i].dist, dci = dcode_of[d <= 256 ? d - 1 : 256 + ((d - 1) >> 7)];
        put_bits(o, dc[dci], dl[dci]);
        if (DEXT[dci]) put_bits(o, (uint32_t)(d - DBASE[dci]), DEXT[dci]);
    }
    put_bits(o, lc[256], ll[256]);
}

#define HASH_BITS 15
#define BLOCK_SYMS 32768

// The match table and the symbols of a thread, kept between calls (session S3): OpenVDB files compress every 512-value
// leaf on its own, and filling a 128 KB table per leaf of 2 KB took most of a bake's time. The table's entries carry a
// running base: positions of earlier calls are below it and count as empty (the same output as a fresh table).
static __declspec(thread) int32_t* t_head;
static __declspec(thread) Sym* t_syms;
static __declspec(thread) int64_t t_base;

void re_deflate_thread_free(void) {
    free(t_head);
    free(t_syms);
    t_head = NULL;
    t_syms = NULL;
    t_base = 0;
}

uint8_t* re_deflate(const uint8_t* in, size_t n, size_t* out_len) {
    // Codes of each length and distance: distances up to 256 directly, larger ones by (d - 1) >> 7 (their code ranges
    // are aligned to 128, as zlib's _dist_code table uses)
    static uint8_t lcode_of[259], dcode_of[512];
    static volatile int32_t ready = 0;
    if (!re_atomic_load(&ready)) {
        for (int l = 3; l <= 258; l++) lcode_of[l] = (uint8_t)len_code(l);
        for (int d = 1; d <= 256; d++) dcode_of[d - 1] = (uint8_t)dist_code(d);
        for (int k = 2; k < 256; k++) dcode_of[256 + k] = (uint8_t)dist_code((k << 7) + 1);
        re_atomic_store(&ready, 1);
    }
    BitOut o = { NULL, 0, 0, 0, 0, false };
    out_reserve(&o, n + n / 16 + 128);
    if (!t_head) t_head = malloc(sizeof(int32_t) << HASH_BITS), t_base = -1;
    if (!t_syms) t_syms = malloc(sizeof(Sym) * (BLOCK_SYMS + 2));
    int32_t* head = t_head;
    Sym* syms = t_syms;
    if (o.oom || !head || !syms) {
        free(o.p);
        return NULL;
    }
    if (t_base < 0 || t_base + (int64_t)n + 65536 > 0x7fff0000) { // a fresh table
        for (int i = 0; i < (1 << HASH_BITS); i++) head[i] = -1;
        t_base = 0;
    }
    const int32_t base = (int32_t)t_base;
    t_base += (int64_t)n + 1;
    o.p[o.len++] = 0x78; // deflate, 32K window
    o.p[o.len++] = 0x01;
    size_t pos = 0, block_start = 0;
    int ns = 0;
    while (pos < n) {
        int best = 0;
        size_t dist = 0;
        if (pos + 4 <= n) {
            uint32_t v;
            memcpy(&v, in + pos, 4);
            uint32_t h = (v * 2654435761u) >> (32 - HASH_BITS);
            int32_t cand = head[h];
            head[h] = base + (int32_t)pos;
            if (cand >= base && pos - (size_t)(cand - base) <= 32768) {
                cand -= base;
                const uint8_t* a = in + cand;
                const uint8_t* b = in + pos;
                size_t max = n - pos > 258 ? 258 : n - pos;
                size_t l = 0;
                while (l + 8 <= max) {
                    uint64_t x, y;
                    memcpy(&x, a + l, 8);
                    memcpy(&y, b + l, 8);
                    if (x != y) break;
                    l += 8;
                }
                while (l < max && a[l] == b[l]) l++;
                if (l >= 4) {
                    best = (int)l;
                    dist = pos - (size_t)cand;
                }
            }
        }
        if (best) {
            syms[ns].lit_or_len = (uint16_t)best;
            syms[ns].dist = (uint16_t)dist;
            ns++;
            if (best <= 8) // the positions inside short matches too (zlib's level 1 does the same)
                for (size_t k = 1; k < (size_t)best && pos + k + 4 <= n; k++) {
                    uint32_t v;
                    memcpy(&v, in + pos + k, 4);
                    head[(v * 2654435761u) >> (32 - HASH_BITS)] = base + (int32_t)(pos + k);
                }
            pos += (size_t)best;
        } else {
            syms[ns].lit_or_len = in[pos];
            syms[ns].dist = 0;
            ns++;
            pos++;
        }
        if (ns >= BLOCK_SYMS || pos >= n) {
            write_block(&o, syms, ns, in + block_start, pos - block_start, pos >= n, lcode_of, dcode_of);
            if (o.oom) break;
            ns = 0;
            block_start = pos;
        }
    }
    if (n == 0) write_block(&o, syms, 0, in, 0, true, lcode_of, dcode_of);
    put_align(&o);
    out_reserve(&o, 4);
    if (o.oom) {
        free(o.p);
        return NULL;
    }
    uint32_t ad = re_adler32(in, n);
    o.p[o.len++] = (uint8_t)(ad >> 24);
    o.p[o.len++] = (uint8_t)(ad >> 16);
    o.p[o.len++] = (uint8_t)(ad >> 8);
    o.p[o.len++] = (uint8_t)ad;
    *out_len = o.len;
    return o.p;
}
