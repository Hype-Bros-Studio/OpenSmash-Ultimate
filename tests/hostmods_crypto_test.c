/* runtime/hostmods_crypto.h on its own, for tests/test_hostmods.py:
 *   hostmods_crypto_test CODE FILE      the sealed pack FILE opened with CODE, to stdout (exit 1: it doesn't open)
 *   hostmods_crypto_test --id CODE      the pack id of CODE (as runtime/hle_hostmods.c takes it)
 *   hostmods_crypto_test --sha256 FILE  FILE's SHA-256 */
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/hostmods_crypto.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static uint8_t *slurp(const char *path, uint64_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t cap = 1 << 16, len = 0, got;
    uint8_t *b = malloc(cap);
    while ((got = fread(b + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap) b = realloc(b, cap *= 2);
    }
    fclose(f);
    *n = len;
    return b;
}

static size_t norm(const char *s, uint8_t *code) {
    size_t n = 0;
    for (; *s && n < 128; s++)
        if (*s != '-' && *s != ' ' && *s != '\t') code[n++] = (uint8_t)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    return n;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc != 3) return 2;
    uint8_t code[128], d[32];
    HmSha256 s;
    hm_sha256_init(&s);
    if (!strcmp(argv[1], "--sha256")) {
        uint64_t n;
        uint8_t *b = slurp(argv[2], &n);
        hm_sha256_update(&s, b, (size_t)n);
        hm_sha256_final(&s, d);
        for (int i = 0; i < 32; i++) printf("%02x", d[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "--id")) {
        static const char idk[] = "HypeSmash host mods id";
        size_t cl = norm(argv[2], code);
        hm_sha256_update(&s, idk, sizeof idk);
        hm_sha256_update(&s, code, cl);
        hm_sha256_final(&s, d);
        for (int i = 0; i < 10; i++) printf("%02x", d[i]);
        printf("\n");
        return 0;
    }
    uint64_t n;
    uint8_t *b = slurp(argv[2], &n);
    int64_t plain = hm_unseal(code, norm(argv[1], code), b, n);
    if (plain < 0) return 1;
    fwrite(b, 1, (size_t)plain, stdout);
    return 0;
}
