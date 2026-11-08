/* SSBU_TOURNEY_UI_FILE=<path> (research): switches the Battle Arena's scene by name, to show the
 * Online Tourney bracket (OnlineTournamentTableScene) inside an Arena room and back.
 *
 * The Arena room is the sequence OnlineMeleeRoomSequenceScene (its per-frame update main+0x23797c0,
 * x0 = the sequence). Its sub-scene manager is at sequence+0x50: +0x20 the list of change requests,
 * +0x28 the scene table (+0x20 the scenes it can make, by name; +0x60 the current one, its name at
 * +0x10). Scenes are named in registries, arrays of {name, factory} ended by a null name; main+0x3723a90
 * (manager, registry) adds one, and main+0x3726240 (request list, &to, &from, params) asks for a change
 * (to and from are {fnv1a, length, char[64]}). Both copy what they keep, so their arguments live on the
 * guest stack for the call only.
 *
 * The manager (main+0x3725030, each frame) takes a request only in two cases. In state 1 (manager+0x30),
 * it pushes the current scene on a stack (scene table +0x30..+0x38) without stopping it and makes the
 * requested one. Otherwise it waits until there is no current scene: main+0x3726af0(manager, -1), which
 * the scenes call to end themselves, sets state 2, and the current scene exits (its vfunc +0x18, then
 * +0x20 until done) and is destroyed. With no current scene, it pops the stack first (resuming that scene,
 * vfunc +0x38), and only then takes a request.
 *
 * Each time the file changes (its mtime) its first word is taken:
 *   table   OnlineTournamentTableScene pushed over the room (state 1), adding it to this Arena's scenes
 *           first (the tourney's own factory, main+0x50b93c8)
 *   swap    the same scene in place of the current one (request, then end the current one)
 *   room    the current scene ended: the room comes back off the stack, or is asked for when the stack is empty
 *   <name>  that scene in place of the current one
 * The current scene's name and the stack's depth are logged whenever they change. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"

#define SEQ_UPDATE 0x23797c0      /* OnlineMeleeRoomSequenceScene, per frame */
#define REGISTRY_ADD 0x3723a90
#define SCENE_REQUEST 0x3726240
#define SCENE_END 0x3726af0
#define TABLE_FACTORY 0x50b93c8   /* OnlineTournamentTableScene's factory (tourney registry 0x53294e8) */
#define TABLE_NAME "OnlineTournamentTableScene"
#define ROOM_NAME "OnlineMeleeRoomScene"

static GuestFn *tui_orig;
static const char *tui_path;
static long long tui_seen;
static int tui_polled;
static double tui_next;
static u64 tui_registered;        /* the sequence the table scene was added to */
static char tui_cur[64];
static long tui_depth;

static void put_name(u64 rec, const char *name) {
    u32 h = 0x811c9dc5u;
    size_t n = strlen(name);
    if (n > 63) n = 63;
    for (size_t i = 0; i < n; i++) h = (h * 0x89) ^ (u8)name[i];   /* (the game's own: FNV-1a 32 with a byte multiply) */
    *P(u32, rec) = h;
    *P(u32, rec + 4) = (u32)n;
    memcpy(P(char, rec + 8), name, n);
    *P(char, rec + 8 + n) = 0;
}

/* Calls fn(x0, x1, x2, x3) with a 0x400-byte scratch area below the guest stack (x1..x3 may point into it). */
static u64 call4(Cpu *c, u64 fn, u64 x0, u64 x1, u64 x2, u64 x3, u64 sp) {
    Cpu saved = *c;
    c->sp = sp;
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
    c->x[3] = x3;
    rt_call(c, fn);
    u64 r = c->x[0];
    *c = saved;
    return r;
}

static long stack_depth(u64 seq) {
    u64 table = *P(u64, seq + 0x78);
    return table ? (long)(*P(u64, table + 0x38) - *P(u64, table + 0x30)) / 8 : 0;
}

static void tui_request(Cpu *c, u64 seq, const char *word) {
    u64 base = rt_module_main.base, mgr = seq + 0x50;
    u64 sp = (c->sp - 0x400) & ~15ull;
    u64 str = sp + 0x100, reg = sp + 0x140, to = sp + 0x180, from = sp + 0x1d0;
    int push = !strcmp(word, "table"), back = !strcmp(word, "room");
    const char *target = push || !strcmp(word, "swap") ? TABLE_NAME : back ? ROOM_NAME : word;
    if (back && stack_depth(seq) > 0) {   /* (ending the scene on top resumes the one under it) */
        call4(c, base + SCENE_END, mgr, (u64)-1, 0, 0, sp);
        fprintf(stderr, "[tourney-ui] ended %s, back to the stacked scene at %.2f s\n", tui_cur, rt_now_ns() / 1e9);
        fflush(stderr);
        return;
    }
    if (!strcmp(target, TABLE_NAME) && tui_registered != seq) {
        strcpy(P(char, str), target);
        *P(u64, reg) = str;
        *P(u64, reg + 8) = base + TABLE_FACTORY;
        *P(u64, reg + 16) = 0;
        *P(u64, reg + 24) = 0;
        call4(c, base + REGISTRY_ADD, mgr, reg, 0, 0, sp);
        tui_registered = seq;
        fprintf(stderr, "[tourney-ui] %s added to the Arena's scenes\n", target);
    }
    put_name(to, target);
    put_name(from, tui_cur);
    u64 list = *P(u64, mgr + 0x20);
    if (!list) { fprintf(stderr, "[tourney-ui] no request list\n"); fflush(stderr); return; }
    call4(c, base + SCENE_REQUEST, list, to, from, 0, sp);
    if (push) *P(u32, mgr + 0x30) = 1;
    else call4(c, base + SCENE_END, mgr, (u64)-1, 0, 0, sp);
    fprintf(stderr, "[tourney-ui] asked %s -> %s (%s) at %.2f s\n", tui_cur, target, push ? "pushed over it" : "in its place",
            rt_now_ns() / 1e9);
    fflush(stderr);
}

static void tui_update(Cpu *c) {
    u64 seq = c->x[0];
    tui_orig(c);
    u64 table = *P(u64, seq + 0x78);
    u64 cur = table ? *P(u64, table + 0x60) : 0;
    const char *name = cur ? P(char, cur + 0x10) : "";
    long depth = stack_depth(seq);
    if (strncmp(name, tui_cur, sizeof tui_cur - 1) || depth != tui_depth) {
        snprintf(tui_cur, sizeof tui_cur, "%s", name);
        tui_depth = depth;
        fprintf(stderr, "[tourney-ui] scene now %s, %ld stacked (sequence %llx) at %.2f s\n", tui_cur, depth,
                (unsigned long long)seq, rt_now_ns() / 1e9);
        fflush(stderr);
    }
    double now = rt_now_ns() / 1e9;
    if (now < tui_next) return;
    tui_next = now + 0.25;
    struct stat st;
#ifdef _WIN32
    long long m = stat(tui_path, &st) ? -1 : (long long)st.st_mtime * 1000000ll + st.st_size;
#else
    long long m = stat(tui_path, &st) ? -1 : (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    if (tui_polled && m == tui_seen) return;
    int first = !tui_polled;
    tui_polled = 1;
    tui_seen = m;
    char word[64] = "";
    FILE *f = m < 0 ? NULL : fopen(tui_path, "r");
    if (f) {
        if (fscanf(f, "%63s", word) < 1) word[0] = 0;
        fclose(f);
    }
    if (first || !word[0]) return;   /* (what the file held before this room: not a request) */
    tui_request(c, seq, word);
}

void tourney_ui_install(void) {
    static int done;
    if (done) return;
    done = 1;
    tui_path = getenv("SSBU_TOURNEY_UI_FILE");
    if (!tui_path || !*tui_path) return;
    u64 orig = rt_hook_function(rt_module_main.base + SEQ_UPDATE, rt_native(tui_update, "SSBU_TOURNEY_UI_FILE hook"));
    if (orig) tui_orig = rt_lookup(orig);
    fprintf(stderr, "[tourney-ui] Arena sequence %s\n", orig ? "hooked" : "not found");
    fflush(stderr);
}
