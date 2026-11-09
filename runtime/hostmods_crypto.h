/* SHA-256 and ChaCha20-Poly1305 (RFC 8439) for host-only mods (hle_hostmods.c), plain C with no
 * library: tests/hostmods_crypto_test.c checks them against Python's (pycryptodomex). Not fast,
 * not hardened against side channels - a pack is opened once, at the game's start. */
#ifndef HOSTMODS_CRYPTO_H
#define HOSTMODS_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t hm_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void hm_put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- SHA-256 ------------------------------------------------------------------------ */
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; } HmSha256;

static inline void hm_sha256_block(uint32_t h[8], const uint8_t *b) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define HM_ROR(x, r) ((x) >> (r) | (x) << (32 - (r)))
    uint32_t w[64], a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = HM_ROR(w[i - 15], 7) ^ HM_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = HM_ROR(w[i - 2], 17) ^ HM_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (HM_ROR(e, 6) ^ HM_ROR(e, 11) ^ HM_ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (HM_ROR(a, 2) ^ HM_ROR(a, 13) ^ HM_ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
#undef HM_ROR
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static inline void hm_sha256_init(HmSha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->n = 0;
    s->used = 0;
}
static inline void hm_sha256_update(HmSha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->n += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += (uint32_t)k; p += k; n -= k;
        if (s->used == 64) { hm_sha256_block(s->h, s->buf); s->used = 0; }
    }
}
static inline void hm_sha256_final(HmSha256 *s, uint8_t out[32]) {
    uint64_t bits = s->n * 8;
    uint8_t one = 0x80, zero = 0, len[8];
    hm_sha256_update(s, &one, 1);
    while (s->used != 56) hm_sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    hm_sha256_update(s, len, 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- ChaCha20 ----------------------------------------------------------------------- */
#define HM_QR(a, b, c, d) \
    a += b; d ^= a; d = d << 16 | d >> 16; c += d; b ^= c; b = b << 12 | b >> 20; \
    a += b; d ^= a; d = d << 8 | d >> 24;  c += d; b ^= c; b = b << 7 | b >> 25;
static inline void hm_chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574}, x[16];
    for (int i = 0; i < 8; i++) in[4 + i] = hm_le32(key + 4 * i);
    in[12] = counter;
    for (int i = 0; i < 3; i++) in[13 + i] = hm_le32(nonce + 4 * i);
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        HM_QR(x[0], x[4], x[8], x[12]) HM_QR(x[1], x[5], x[9], x[13]) HM_QR(x[2], x[6], x[10], x[14]) HM_QR(x[3], x[7], x[11], x[15])
        HM_QR(x[0], x[5], x[10], x[15]) HM_QR(x[1], x[6], x[11], x[12]) HM_QR(x[2], x[7], x[8], x[13]) HM_QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) hm_put_le32(out + 4 * i, x[i] + in[i]);
}
#undef HM_QR
static inline void hm_chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t *data, size_t n) {
    uint8_t ks[64];
    for (; n; counter++) {
        size_t k = n < 64 ? n : 64;
        hm_chacha_block(key, counter, nonce, ks);
        for (size_t i = 0; i < k; i++) data[i] ^= ks[i];
        data += k; n -= k;
    }
    memset(ks, 0, sizeof ks);
}

/* ---- Poly1305 (26-bit limbs) ------------------------------------------------------------ */
typedef struct { uint32_t r[5], h[5], pad[4]; uint8_t buf[16]; size_t left; } HmPoly;

static inline void hm_poly_init(HmPoly *st, const uint8_t key[32]) {
    st->r[0] = hm_le32(key + 0) & 0x3ffffff;
    st->r[1] = (hm_le32(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (hm_le32(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (hm_le32(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (hm_le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = hm_le32(key + 16 + 4 * i);
    st->left = 0;
}
static inline void hm_poly_blocks(HmPoly *st, const uint8_t *m, size_t n, uint32_t hibit) {
    const uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    for (; n >= 16; m += 16, n -= 16) {
        h0 += hm_le32(m + 0) & 0x3ffffff;
        h1 += (hm_le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (hm_le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (hm_le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (hm_le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}
static inline void hm_poly_update(HmPoly *st, const uint8_t *m, size_t n) {
    if (st->left) {
        size_t k = 16 - st->left < n ? 16 - st->left : n;
        memcpy(st->buf + st->left, m, k);
        st->left += k; m += k; n -= k;
        if (st->left < 16) return;
        hm_poly_blocks(st, st->buf, 16, 1u << 24);
        st->left = 0;
    }
    size_t whole = n & ~(size_t)15;
    if (whole) hm_poly_blocks(st, m, whole, 1u << 24);
    m += whole; n -= whole;
    if (n) { memcpy(st->buf, m, n); st->left = n; }
}
static inline void hm_poly_final(HmPoly *st, uint8_t mac[16]) {
    if (st->left) {
        st->buf[st->left] = 1;
        for (size_t i = st->left + 1; i < 16; i++) st->buf[i] = 0;
        hm_poly_blocks(st, st->buf, 16, 0);
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;   /* all ones when h >= p: take g */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = h0 | h1 << 26;
    h1 = h1 >> 6 | h2 << 20;
    h2 = h2 >> 12 | h3 << 14;
    h3 = h3 >> 18 | h4 << 8;
    uint64_t f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
    hm_put_le32(mac, h0); hm_put_le32(mac + 4, h1); hm_put_le32(mac + 8, h2); hm_put_le32(mac + 12, h3);
    memset(st, 0, sizeof *st);
}

/* ---- ChaCha20-Poly1305: open in place (1: the tag was right, data is the plain text; 0: data
 * is left as it was) ------------------------------------------------------------------------ */
static inline int hm_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                        uint8_t *data, size_t n, const uint8_t tag[16]) {
    static const uint8_t zeros[16];
    uint8_t otk[64], mac[16], lens[16];
    HmPoly p;
    hm_chacha_block(key, 0, nonce, otk);
    hm_poly_init(&p, otk);
    hm_poly_update(&p, aad, aad_len);
    if (aad_len % 16) hm_poly_update(&p, zeros, 16 - aad_len % 16);
    hm_poly_update(&p, data, n);
    if (n % 16) hm_poly_update(&p, zeros, 16 - n % 16);
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)aad_len >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)n >> (8 * i)); }
    hm_poly_update(&p, lens, 16);
    hm_poly_final(&p, mac);
    memset(otk, 0, sizeof otk);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(mac[i] ^ tag[i]);
    if (diff) return 0;
    hm_chacha_xor(key, 1, nonce, data, n);
    return 1;
}

/* A sealed pack (tools/hostmods.py seal), opened in place: the plain pack ends up at the buffer's
 * start. Returns its size, or -1 (a wrong code, a changed pack, not a pack). */
#define HM_HEAD 40
static inline int64_t hm_unseal(const uint8_t *code, size_t code_len, uint8_t *buf, uint64_t len) {
    static const char kdf[] = "HypeSmash host mods v1";   /* (and its 0) */
    if (len < HM_HEAD || memcmp(buf, "SSBUHME1", 8)) return -1;
    uint8_t head[HM_HEAD + 1], key[32];
    memcpy(head, buf, HM_HEAD);
    uint64_t size = 0;
    for (int i = 0; i < 8; i++) size |= (uint64_t)head[24 + i] << (8 * i);
    uint32_t chunk = hm_le32(head + 32);
    if (!chunk) return -1;
    uint64_t n = size ? (size + chunk - 1) / chunk : 1;
    if (size > len || n > len || len != HM_HEAD + size + n * 16) return -1;
    HmSha256 s;
    hm_sha256_init(&s);
    hm_sha256_update(&s, kdf, sizeof kdf);
    hm_sha256_update(&s, head + 8, 16);
    hm_sha256_update(&s, code, code_len);
    hm_sha256_final(&s, key);
    uint64_t in = HM_HEAD, out = 0;
    int ok = 1;
    for (uint64_t i = 0; i < n && ok; i++) {
        uint64_t m = size - i * chunk < chunk ? size - i * chunk : chunk;
        uint8_t nonce[12] = {0};
        for (int k = 0; k < 8; k++) nonce[4 + k] = (uint8_t)(i >> (8 * k));
        head[HM_HEAD] = (uint8_t)(i == n - 1);
        ok = hm_aead_open(key, nonce, head, HM_HEAD + 1, buf + in, (size_t)m, buf + in + m);
        if (ok) memmove(buf + out, buf + in, (size_t)m);
        in += m + 16; out += m;
    }
    memset(key, 0, sizeof key);
    memset(&s, 0, sizeof s);
    return ok ? (int64_t)size : -1;
}
#endif
