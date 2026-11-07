/* SSBU_TRACE_FN=<offset>[,<offset>...] (research): logs calls of those functions of main (hex
 * offsets into main, as a disassembly of main shows them) - the first SSBU_TRACE_FN_LIMIT
 * calls of each (default 40), then calls 64, 128, 256, ... - with their first four arguments,
 * their caller and what they returned (x0). Installed once the RomFS mounts (hle_fs.c), as the
 * Skyline plugins' hooks are. An offset followed by ":rN" logs each call whose xN (low 32 bits)
 * was not seen before, ":dN" each whose u16 at [xN] was not (an event id, say), and no others;
 * ":pN.o1.o2" reads, after the call, [xN+o1] then the u32 at +o2 (up to four offsets, each but
 * the last a pointer) and logs each new value of it per object (a state field, say);
 * ":xN.o1.o2..." (up to 12) logs the u32s at [xN+o] before the call whenever one changed. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define TRACES 32
static u64 trace_off[TRACES];
static GuestFn *trace_orig[TRACES];
static long trace_calls[TRACES];
static long trace_limit = 40;
static char trace_mode[TRACES];          /* 0, 'r', 'd' or 'p' */
static int trace_reg[TRACES];
static int trace_nderef[TRACES];
static u64 trace_deref[TRACES][12];       /* 'p': [[xN+o0]+o1]... then the u32 at the last offset */
#define SEEN 1024
static u32 trace_seen[TRACES][SEEN];     /* key + 1, open addressing */

static int first_time(int i, u32 key) {
    for (u32 h = (key * 2654435761u) % SEEN, k = 0; k < SEEN; k++, h = (h + 1) % SEEN) {
        if (trace_seen[i][h] == key + 1) return 0;
        if (!trace_seen[i][h]) { trace_seen[i][h] = key + 1; return 1; }
    }
    return 0;
}

static void run(int i, Cpu *c) {
    long k = ++trace_calls[i];   /* (a count for the log: a race between threads costs nothing) */
    u64 a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
    int log;
    u32 key = 0;
    u64 rp = c->x[trace_reg[i]];
    if (trace_mode[i] == 'x') {
        /* the u32s at [xN+o] for each offset, before the call; logged when any changed */
        static u32 last[TRACES][12];
        char line[400];
        int len = 0, changed = 0;
        for (int d = 0; d < trace_nderef[i]; d++) {
            u32 v = rp ? *P(u32, rp + trace_deref[i][d]) : 0;
            if (v != last[i][d]) changed = 1;
            last[i][d] = v;
            len += snprintf(line + len, sizeof line - len, " +%llx=%x", (unsigned long long)trace_deref[i][d], v);
        }
        trace_orig[i](c);
        if (changed || k == 1)
            fprintf(stderr, "[trace] main+%llx #%ld %llx:%s\n", (unsigned long long)trace_off[i], k, (unsigned long long)rp, line), fflush(stderr);
        return;
    }
    if (trace_mode[i] == 'p') {
        log = 0;
    } else if (trace_mode[i]) {
        u64 r = rp;
        key = trace_mode[i] == 'r' ? (u32)r : (r ? *P(u16, r) : 0xfffffffeu);
        log = first_time(i, key);
    } else {
        log = k <= trace_limit || !(k & (k - 1));
    }
    trace_orig[i](c);
    if (trace_mode[i] == 'p') {
        u64 q = rp;
        for (int d = 0; q && d + 1 < trace_nderef[i]; d++) q = *P(u64, q + trace_deref[i][d]);
        u32 v = q ? *P(u32, q + trace_deref[i][trace_nderef[i] - 1]) : 0xfffffffeu;
        key = v;
        log = first_time(i, (u32)(q * 31) ^ v);
    }
    if (log) {
        char from[96];
        fprintf(stderr, "[trace] main+%llx #%ld key %x (%llx, %llx, %llx, %llx) from %s -> %llx\n",
                (unsigned long long)trace_off[i], k, key, (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)a2, (unsigned long long)a3, rt_describe(lr, from, sizeof from),
                (unsigned long long)c->x[0]);
        fflush(stderr);
    }
}
#define T(k, j) static void trace_thunk_##k##j(Cpu *c) { run((k) * 8 + (j), c); }
#define T8(k) T(k, 0) T(k, 1) T(k, 2) T(k, 3) T(k, 4) T(k, 5) T(k, 6) T(k, 7)
T8(0) T8(1) T8(2) T8(3)
#define R(k, j) trace_thunk_##k##j,
#define R8(k) R(k, 0) R(k, 1) R(k, 2) R(k, 3) R(k, 4) R(k, 5) R(k, 6) R(k, 7)
static GuestFn *const trace_thunks[TRACES] = {R8(0) R8(1) R8(2) R8(3)};

/* SSBU_TRACE_ARENA=1 (research): the Battle Arena room's per-frame logic (main+0x1c50320, x0 =
 * the room) logs its 8 slots (state bytes at +0xaf8 + slot * 0x1e8), its fighter queue (u16s
 * from +0x4f8 to +0x500) and the network manager's per-station state (+0xa080 + i * 0x78)
 * whenever any of them changed. */
static GuestFn *arena_orig;
static void arena_room(Cpu *c) {
    static char last[1024];
    char line[1024];
    u64 room = c->x[0];
    int len = 0;
    len += snprintf(line + len, sizeof line - len, "room %llx own %u slots", (unsigned long long)room, *P(u8, room + 0x1920));
    for (int i = 0; i < 8; i++) {
        u64 sl = room + 0xaf8 + (u64)i * 0x1e8;
        len += snprintf(line + len, sizeof line - len, " %02x%02x%02x%02x%02x%02x%02x", *P(u8, sl), *P(u8, sl + 1),
                        *P(u8, sl + 2), *P(u8, sl + 3), *P(u8, sl + 4), *P(u8, sl + 5), *P(u8, sl + 6));
    }
    u64 q0 = *P(u64, room + 0x4f8), q1 = *P(u64, room + 0x500);
    len += snprintf(line + len, sizeof line - len, " queue");
    for (u64 q = q0; q && q < q1 && q < q0 + 32; q += 2) len += snprintf(line + len, sizeof line - len, " %04x", *P(u16, q));
    u64 g = *P(u64, rt_module_main.base + 0x52c4c58);
    u64 mgr = g ? *P(u64, *P(u64, g + 8) + 0x88) : 0;
    len += snprintf(line + len, sizeof line - len, " st");
    for (int i = 0; mgr && i < 8; i++) len += snprintf(line + len, sizeof line - len, " %x", *P(u32, mgr + 0xa080 + (u64)i * 0x78));
    len += snprintf(line + len, sizeof line - len, " 2c0=%x 18e6=%x 1b27=%x 1854=%x 166c=%x",
                    *P(u32, room + 0x2c0), *P(u8, room + 0x18e6), *P(u8, room + 0x1b27), *P(u8, room + 0x1854), *P(u8, room + 0x166c));
    if (strcmp(line, last)) {
        strcpy(last, line);
        fprintf(stderr, "[arena] %s\n", line);
        fflush(stderr);
    }
    arena_orig(c);
}

void trace_install(void) {
    static int done;
    const char *v = getenv("SSBU_TRACE_FN");
    const char *a = getenv("SSBU_TRACE_ARENA");
    if (done) return;
    done = 1;
    if (a && *a == '1') {
        u64 orig = rt_hook_function(rt_module_main.base + 0x1c50320, rt_native(arena_room, "SSBU_TRACE_ARENA hook"));
        if (orig) arena_orig = rt_lookup(orig);
        fprintf(stderr, "[arena] room logic %s\n", orig ? "traced" : "not found");
    }
    if (!v || !*v) return;
    const char *l = getenv("SSBU_TRACE_FN_LIMIT");
    if (l && *l) trace_limit = atol(l);
    int n = 0;
    for (const char *p = v; *p && n < TRACES;) {
        char *end;
        u64 off = strtoull(p, &end, 16);
        if (end == p) break;
        u64 orig = rt_hook_function(rt_module_main.base + off, rt_native(trace_thunks[n], "SSBU_TRACE_FN hook"));
        trace_mode[n] = 0;
        if (*end == ':' && (end[1] == 'r' || end[1] == 'd' || end[1] == 'p' || end[1] == 'x') && end[2] >= '0' && end[2] <= '9') {
            trace_mode[n] = end[1];
            trace_reg[n] = (int)strtol(end + 2, &end, 10) & 31;
            trace_nderef[n] = 0;
            while ((trace_mode[n] == 'p' || trace_mode[n] == 'x') && *end == '.' && trace_nderef[n] < (trace_mode[n] == 'x' ? 12 : 4))
                trace_deref[n][trace_nderef[n]++] = strtoull(end + 1, &end, 16);
            if ((trace_mode[n] == 'p' || trace_mode[n] == 'x') && !trace_nderef[n]) trace_mode[n] = 0;
        }
        if (orig) {
            trace_off[n] = off;
            trace_orig[n] = rt_lookup(orig);
            n++;
            fprintf(stderr, "[trace] main+%llx traced\n", (unsigned long long)off);
        } else {
            fprintf(stderr, "[trace] main+%llx is not the start of a function of main\n", (unsigned long long)off);
        }
        p = *end ? end + 1 : end;
    }
}
