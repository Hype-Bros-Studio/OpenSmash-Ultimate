/* The game's current scenes, read once a frame, so a test can wait for a menu instead of for a time
 * (SSBU_INPUT_LIVE's "@scene:" lines in hle_hid.c, SSBU_SEQ's "@scene:<name>" marks).
 *
 * The game's screens are scenes, made and switched by scene managers (main+0x3724550 makes one,
 * main+0x3725030 runs it each frame): manager +0x28 is its scene table, whose +0x60 is the current
 * scene; a scene's name is at +8 as {fnv1a, length, char[64]} (hle_tourney_ui.c says more). The root
 * manager's address is kept at main+0x593b2d8. A sequence scene (MenuSequenceScene,
 * OnlineMeleeRoomSequenceScene, ...) has a manager of its own at +0x50, its vtable main+0x50a1070, so
 * the current scenes make a path from the root down: "MenuSequenceScene > MainMenuScene".
 *
 * On when SSBU_INPUT_LIVE, SSBU_SEQ or SSBU_SCENE_LOG is set: each change of the path is logged
 * ("[scene] now A > B at t s"), and each scene that comes into it raises the mark "scene:<name>".
 * The reads go through kernel_read64, so a scene freed under the walk is not followed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_hooks.h"
#include "host.h"
#include "input_seq.h"

#define ROOT_MANAGER 0x593b2d8   /* the root scene manager's address */
#define SEQ_MANAGER_VTABLE 0x50a1070
enum { SCENE_DEPTH = 8, SCENE_NAME = 64, SCENE_PATH = SCENE_DEPTH * (SCENE_NAME + 3) };

static SRWLOCK path_lock = SRWLOCK_INIT;
static char path_now[SCENE_PATH];   /* "A > B > C" */

static int rd(u64 addr, u64 *out) { return addr && kernel_read64(addr, out); }

/* The scene's name, or 0 if it cannot be read. */
static int scene_name(u64 scene, char out[SCENE_NAME]) {
    u64 head;
    if (!rd(scene + 8, &head)) return 0;
    u32 len = (u32)(head >> 32);
    if (!len || len >= SCENE_NAME) return 0;
    for (u32 k = 0; k < len; k += 8) {
        u64 w;
        if (!rd(scene + 0x10 + k, &w)) return 0;
        memcpy(out + k, &w, len - k < 8 ? len - k : 8);
    }
    out[len] = 0;
    for (u32 k = 0; k < len; k++) if (out[k] < 0x20 || out[k] > 0x7e) return 0;
    return 1;
}

static int walk(char names[SCENE_DEPTH][SCENE_NAME]) {
    u64 base = rt_module_main.base, mgr, table, cur, vt;
    int n = 0;
    if (!rd(base + ROOT_MANAGER, &mgr)) return 0;
    while (n < SCENE_DEPTH && rd(mgr + 0x28, &table) && rd(table + 0x60, &cur) && scene_name(cur, names[n])) {
        n++;
        if (!rd(cur + 0x50, &vt) || vt != base + SEQ_MANAGER_VTABLE) break;
        mgr = cur + 0x50;
    }
    return n;
}

static void scene_frame(Cpu *c, const u64 args[8], void *ctx) {
    (void)c, (void)args, (void)ctx;
    static char was[SCENE_DEPTH][SCENE_NAME];
    static int was_n = -1;
    char names[SCENE_DEPTH][SCENE_NAME] = {{0}};   /* (zeroed: compared whole below) */
    int n = walk(names);
    if (n == was_n && !memcmp(names, was, sizeof names[0] * n)) return;
    char path[SCENE_PATH] = "";
    for (int i = 0; i < n; i++) {
        if (i) strcat(path, " > ");
        strcat(path, names[i]);
        int known = 0;
        for (int j = 0; j < was_n && !known; j++) known = !strcmp(was[j], names[i]);
        if (!known) {
            char mark[SCENE_NAME + 8];
            snprintf(mark, sizeof mark, "scene:%s", names[i]);
            input_seq_mark(mark);
        }
    }
    memcpy(was, names, sizeof names[0] * n);
    was_n = n;
    AcquireSRWLockExclusive(&path_lock);
    memcpy(path_now, path, sizeof path);
    ReleaseSRWLockExclusive(&path_lock);
    fprintf(stderr, "[scene] now %s at %.2f s\n", n ? path : "(none)", rt_now_ns() / 1e9);
    fflush(stderr);
}

int scene_current(const char *name) {
    size_t len = strlen(name);
    int found = 0;
    AcquireSRWLockShared(&path_lock);
    for (const char *p = path_now; !found && (p = strstr(p, name)); p += len)
        found = (p == path_now || p[-1] == ' ') && (!p[len] || p[len] == ' ');
    ReleaseSRWLockShared(&path_lock);
    return found;
}

__attribute__((constructor)) static void scene_watch_register(void) {
    const char *log = getenv("SSBU_SCENE_LOG");
    if (getenv("SSBU_INPUT_LIVE") || getenv("SSBU_SEQ") || (log && strcmp(log, "0")))
        frame_hook_add(FRAME_HOOK_END, 400, NULL, scene_frame, NULL);
}
