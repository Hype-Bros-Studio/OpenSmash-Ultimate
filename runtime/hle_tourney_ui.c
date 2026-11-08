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
 *           first (the tourney's own factory, main+0x50b93c8), and the tourney's data holder made first
 *           when there is none (deleted again once the room is back with nothing stacked)
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
#define HOLDER 0x5329740          /* the tourney's data holder H (H+8 = T), made by the tourney sequence's init */
#define HOLDER_VTABLE 0x50b9a20
#define LIST_VTABLE 0x5181b40     /* T+0xa0's object */
#define ALLOC 0x392e290           /* (align, size) */
#define T_ORDER 0x239c2f0         /* over T's deque of 0..3 */
#define T_INIT_568 0x23990d0
#define T_RESET 0x239c450
#define T_LOAD_PARAMS 0x2385bf0   /* the tourney sequence's vfunc +0x50: loads the tourney param file into T+0xe0..0x2c8 */

static GuestFn *tui_orig;
static const char *tui_path;
static long long tui_seen;
static int tui_polled;
static double tui_next;
static u64 tui_registered;        /* the sequence the table scene was added to */
static char tui_cur[64];
static long tui_depth;
static u64 tui_holder;            /* the H made here, deleted once the room is back */

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

static u64 galloc(Cpu *c, u64 size, u64 sp) {
    u64 p = call4(c, rt_module_main.base + ALLOC, 0x10, size, 0, 0, sp);
    if (p) memset(P(u8, p), 0, size);
    return p;
}

/* The Table scene reads the tourney's holder (main+0x5329740), which only the tourney sequence's init
 * (main+0x22d6fd0, inline at 0x22d74a0..0x22d7b30) makes; this makes it the same way when there is none. */
static int make_holder(Cpu *c, u64 sp) {
    u64 base = rt_module_main.base;
    if (*P(u64, base + HOLDER)) return 1;
    u64 h = galloc(c, 0x10, sp), t = galloc(c, 0x750, sp), s = galloc(c, 0x80, sp);
    u64 map = galloc(c, 8, sp), blk = galloc(c, 0x1000, sp);
    if (!h || !t || !s || !map || !blk) { fprintf(stderr, "[tourney-ui] no memory for the holder\n"); return 0; }
    *P(u64, h) = base + HOLDER_VTABLE;
    *P(u32, t) = 3;
    *P(u64, s) = base + LIST_VTABLE;   /* four empty lists, +0x68 = 2 */
    for (int i = 0; i < 4; i++) *P(u64, s + 8 + i * 0x18) = s + 0x10 + i * 0x18;
    *P(u32, s + 0x68) = 2;
    *P(u64, t + 0xa0) = s;
    *P(u64, t + 0xb0) = t + 0xb0;
    *P(u64, t + 0xb8) = base + 0x4742dbb;
    *P(u32, t + 0xc0) = 0xffffff;
    *P(u64, map) = blk;                /* deque<int> {0, 1, 2, 3}: map first/begin/end/cap, start, size */
    *P(u64, t + 0x420) = map;
    *P(u64, t + 0x428) = map;
    *P(u64, t + 0x430) = map + 8;
    *P(u64, t + 0x438) = map + 8;
    *P(u64, t + 0x448) = 4;
    for (u32 i = 0; i < 4; i++) *P(u32, blk + i * 4) = i;
    call4(c, base + T_ORDER, map, blk, map, blk + 16, sp);
    for (int i = 0; i < 5; i++) *P(u64, t + 0x450 + i * 0x18) = t + 0x458 + i * 0x18;   /* five empty trees */
    call4(c, base + T_INIT_568, t + 0x568, 0, 0, 0, sp);
    call4(c, base + T_RESET, t, 0, 0, 0, sp);
    *P(u64, h + 8) = t;
    *P(u64, base + HOLDER) = h;
    tui_holder = h;
    fprintf(stderr, "[tourney-ui] tourney holder made (%llx, data %llx)\n", (unsigned long long)h, (unsigned long long)t);
    call4(c, base + T_LOAD_PARAMS, 0, 0, 0, 0, sp);   /* the Table build reads T+0x1a0[0], which only this fills */
    fprintf(stderr, "[tourney-ui] tourney params loaded (T+0x1a0 has %lld)\n",
            (long long)((*P(u64, t + 0x1a8) - *P(u64, t + 0x1a0)) / 4));
    return 1;
}

/* As the tourney sequence's teardown (main+0x2385810) does: its deleting destructor, then the global nulled. */
static void drop_holder(Cpu *c, u64 sp) {
    u64 base = rt_module_main.base, h = *P(u64, base + HOLDER);
    if (!tui_holder || h != tui_holder) { tui_holder = 0; return; }
    call4(c, *P(u64, *P(u64, h) + 8), h, 0, 0, 0, sp);
    *P(u64, base + HOLDER) = 0;
    tui_holder = 0;
    fprintf(stderr, "[tourney-ui] tourney holder deleted\n");
}

/* The bracket, from the lines after "table" in the file:
 *   class C                 the tourney's size (main+0x239d810's table: {entrants, rounds, group sizes})
 *   events N                entrants (T+0x80, 0x820 bytes each)
 *   local E                 this player's entrant
 *   me M                    this player's match in the round being played
 *   match R M W E...        round R's match M: winner entrant W (-1: not played), then (round 0) its entrants
 * Matches are made as the tourney's sequence makes them (main+0x237ee50, at 0x237f3c0): 0x60 bytes, owned by
 * shared_ptrs (control blocks {vtable 0x50b9a40, owners - 1, weak, match}) from their round's vector at
 * T+8+R*0x18 and their parent's children at +0x10, and holding their parent at +0x28. */
#define MATCH_CTRL_VTABLE 0x50b9a40
#define MAX_ROUNDS 5
#define MAX_MATCHES 32
#define MAX_ENTRANTS 8
typedef struct { int winner, n, e[MAX_ENTRANTS]; u64 m, ctrl; } TuiMatch;
static struct {
    int cls, events, local, me, rounds, count[MAX_ROUNDS];
    TuiMatch match[MAX_ROUNDS][MAX_MATCHES];
} br;

static int read_bracket(void) {
    FILE *f = fopen(tui_path, "r");
    if (!f) return 0;
    memset(&br, 0, sizeof br);
    br.cls = 1;
    br.local = -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int r, m, w, k = 0, n = 0;
        if (sscanf(line, "class %d", &br.cls) == 1 || sscanf(line, "events %d", &br.events) == 1 ||
            sscanf(line, "local %d", &br.local) == 1 || sscanf(line, "me %d", &br.me) == 1) continue;
        if (sscanf(line, "match %d %d %d%n", &r, &m, &w, &n) < 3 || r < 0 || r >= MAX_ROUNDS || m < 0 || m >= MAX_MATCHES)
            continue;
        TuiMatch *t = &br.match[r][m];
        t->winner = w;
        for (char *p = line + n; k < MAX_ENTRANTS && sscanf(p, "%d%n", &t->e[k], &n) == 1; p += n) k++;
        t->n = k;
        if (r + 1 > br.rounds) br.rounds = r + 1;
        if (m + 1 > br.count[r]) br.count[r] = m + 1;
    }
    fclose(f);
    return br.rounds > 0 && br.events > 0;
}

static u64 vec_alloc(Cpu *c, u64 at, u64 n, u64 size, u64 sp) {   /* a vector's begin/end/cap at `at` */
    u64 p = n ? galloc(c, n * size, sp) : 0;
    *P(u64, at) = p;
    *P(u64, at + 8) = p + n * size;
    *P(u64, at + 16) = p + n * size;
    return p;
}

static void fill_bracket(Cpu *c, u64 sp) {
    u64 base = rt_module_main.base, t = *P(u64, *P(u64, base + HOLDER) + 8);
    const u8 *shape = P(u8, call4(c, base + 0x239d810, br.cls, 0, 0, 0, sp));
    *P(u32, t) = br.cls;
    *P(u32, t + 4) = br.rounds;
    *P(u32, t + 0x98) = shape[0];
    u64 ev = vec_alloc(c, t + 0x80, br.events, 0x820, sp);
    for (int i = 0; i < br.events; i++) {
        *P(u32, ev + i * 0x820 + 0x81c) = i + 1;   /* (stands for the player's id) */
        *P(u8, ev + i * 0x820 + 0x810) = i == br.local;
    }
    for (int r = 0; r < br.rounds; r++)
        for (int m = 0; m < br.count[r]; m++) {
            TuiMatch *x = &br.match[r][m];
            x->m = galloc(c, 0x60, sp);
            x->ctrl = galloc(c, 0x20, sp);
            *P(u32, x->m) = shape[2 + r];
            *P(u32, x->m + 4) = r;
            *P(u32, x->m + 8) = m;
            *P(i32, x->m + 0x50) = x->winner;
            *P(u8, x->m + 0x54) = r == br.rounds - 1;
            *P(i32, x->m + 0x58) = -1;
            *P(u64, x->ctrl) = base + MATCH_CTRL_VTABLE;
            *P(u64, x->ctrl + 0x18) = x->m;
            u64 e = vec_alloc(c, x->m + 0x38, x->n, 4, sp);
            for (int k = 0; k < x->n; k++) *P(i32, e + k * 4) = x->e[k];
        }
    for (int r = 0; r < br.rounds; r++) {
        u64 v = vec_alloc(c, t + 8 + r * 0x18, br.count[r], 0x10, sp);
        for (int m = 0; m < br.count[r]; m++) {
            TuiMatch *x = &br.match[r][m];
            *P(u64, v + m * 0x10) = x->m;
            *P(u64, v + m * 0x10 + 8) = x->ctrl;
            /* its children: the previous round's matches (group size of them each), in order */
            int kids = 0;
            if (r > 0) {
                int g = br.count[r - 1] / (br.count[r] ? br.count[r] : 1), first = m * g;
                if (m == br.count[r] - 1) g = br.count[r - 1] - first;
                u64 cv = vec_alloc(c, x->m + 0x10, g > 0 ? g : 0, 0x10, sp);
                for (int k = 0; k < g; k++) {
                    TuiMatch *ch = &br.match[r - 1][first + k];
                    *P(u64, cv + k * 0x10) = ch->m;
                    *P(u64, cv + k * 0x10 + 8) = ch->ctrl;
                    *P(u64, ch->m + 0x28) = x->m;
                    *P(u64, ch->m + 0x30) = x->ctrl;
                    *P(i64, ch->ctrl + 8) += 1;   /* (owned by its parent too) */
                    kids++;
                }
            }
            *P(i64, x->ctrl + 8) += kids;          /* (and by each child's parent pointer; owners - 1 counts the round's own) */
        }
    }
    fprintf(stderr, "[tourney-ui] bracket: class %d, %d entrants, %d rounds, local %d in match %d\n", br.cls, br.events,
            br.rounds, br.local, br.me);
}

/* The Table scene's param (the request clones it, vfunc +0x20): the build asks its type (vfunc 0, a chain
 * that has to reach the scene params' root, main+0x5324758) and reads this player's match at +8. */
static u64 param_vtable;
static void param_type(Cpu *c) { c->x[0] = rt_module_main.base + 0x5324758; }
static void param_name(Cpu *c) { static u64 s; if (!s) s = rt_guest_copy("TourneyUiParam", 15); c->x[0] = s; }
static void param_dtor(Cpu *c) { (void)c; }
static void param_delete(Cpu *c) {
    u64 p = c->x[0];
    if (p) call4(c, rt_module_main.base + 0x392eb40, p, 0, 0, 0, (c->sp - 0x100) & ~15ull);
}
static void param_clone(Cpu *c) {
    u64 from = c->x[0], p = galloc(c, 0x10, (c->sp - 0x100) & ~15ull);
    if (p) { *P(u64, p) = *P(u64, from); *P(u32, p + 8) = *P(u32, from + 8); }
    c->x[0] = p;
}
static u64 make_param(u64 at, u32 me) {
    if (!param_vtable) {
        u64 v[5] = {rt_native(param_type, "tourney-ui param type"), rt_native(param_name, "tourney-ui param name"),
                    rt_native(param_dtor, "tourney-ui param dtor"), rt_native(param_delete, "tourney-ui param delete"),
                    rt_native(param_clone, "tourney-ui param clone")};
        param_vtable = rt_guest_copy(v, sizeof v);
    }
    *P(u64, at) = param_vtable;
    *P(u64, at + 8) = me;
    return at;
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
    int table = !strcmp(target, TABLE_NAME);
    if (table) {
        int fresh = !*P(u64, base + HOLDER);
        if (!make_holder(c, sp)) return;
        if (fresh && read_bracket()) fill_bracket(c, sp);
        else if (fresh) fprintf(stderr, "[tourney-ui] no bracket in %s\n", tui_path);
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
    call4(c, base + SCENE_REQUEST, list, to, from, table ? make_param(sp + 0x240, br.me) : 0, sp);
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
        if (tui_holder && depth == 0 && !strcmp(tui_cur, ROOM_NAME)) drop_holder(c, (c->sp - 0x400) & ~15ull);
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
