/* A plugin-shaped DLL for tools/dev/memdlltest.c (runtime/hle_memdll.c): built as tools/skyline.py
 * builds a mod's plugin (cc.library_command), with what a translated plugin's code leans on - the
 * C runtime, pointers in its data (relocations), a function bound in by the game and an exception
 * raised through its code and caught there. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int (*bound)(int);
static const char *const words[] = {"one", "two", "three"};   /* (pointers: relocated if it moves) */
static char made[64];
static int started;

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h; (void)r;
    if (why == DLL_PROCESS_ATTACH) started = 1;
    return TRUE;
}

static int filter(EXCEPTION_POINTERS *e) {
    return e->ExceptionRecord->ExceptionCode == 0xE0E4C5A0u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static __declspec(noinline) void throw_it(int v) {
    ULONG_PTR args[2] = {(ULONG_PTR)v, 0};
    RaiseException(0xE0E4C5A0u, 0, 2, args);
}

__declspec(dllexport) void ssbu_plugin_bind(int (*fn)(int)) { bound = fn; }

/* started, the words through their pointers, the C runtime, the bound function and an exception
 * caught: "<started> two three <bound(20)> <caught>" */
__declspec(dllexport) const char *ssbu_plugin_module(void) {
    int caught = 0;
    __try {
        throw_it(7);
    } __except (filter(GetExceptionInformation())) {
        caught = 7;
    }
    char *heap = malloc(16);
    snprintf(heap, 16, "%s", words[2]);
    snprintf(made, sizeof made, "%d %s %s %d %d", started, words[1], heap, bound ? bound(20) : -1, caught);
    free(heap);
    return made;
}
