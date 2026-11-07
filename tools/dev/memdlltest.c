/* runtime/hle_memdll.c with no game: tools/dev/memdll_plugin.c's DLL read into memory and loaded
 * from there twice (the second copy can't have its own address: relocated), each run. Prints each
 * copy's answer; tests/test_hostmods.py builds both and checks them.
 *
 *   memdlltest <dll> */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../runtime/hle_memdll.c"

static int twice(int v) { return v * 2 + 2; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    for (int k = 0; k < 2; k++) {
        char why[256];
        MemDll *d = memdll_load(data, (size_t)n, why, sizeof why);
        if (!d) { printf("load %d failed: %s\n", k, why); return 1; }
        void (*bind)(int (*)(int)) = (void *)memdll_proc(d, "ssbu_plugin_bind");
        const char *(*run)(void) = (void *)memdll_proc(d, "ssbu_plugin_module");
        if (!bind || !run || memdll_proc(d, "nothing")) { printf("exports %d wrong\n", k); return 1; }
        bind(twice);
        printf("copy %d at %s: %s\n", k, d->base == (u8 *)d->nt->OptionalHeader.ImageBase ? "its address" : "another address", run());
    }
    /* (nothing was written: the DLL's file is the test's own) */
    return 0;
}
