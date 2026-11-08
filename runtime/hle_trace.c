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
#include <sys/stat.h>

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

/* Arena requests (research, with SSBU_TRACE_ARENA=1): "PLACE [start|join]" for this player, from
 * SSBU_ARENA_PLACE_FILE=<path> (taken each time the file changes, its mtime) or, with
 * SSBU_ARENA_BRACKET=1, from the lobby's bracket (tools/bracket.py /arena/assignment: "place" and
 * "role", taken each time they or the match change; hle_online.c asks every 2 s).
 * PLACE is 1 or 2 the ring, 3 the Spectator Stands, 4 the Waiting Area: while this player's icon is
 * elsewhere, ask the game to move it there as a drag and drop does (room controller room+0x1950, its
 * virtual +0x30 = main+0x189c160 with &own slot, &place), at most 5 times, and never again after it
 * got there, so the game's own kicks (a cancelled start, the rotation after a battle) are left alone
 * until the next request.
 * "start" then presses + (Smash!) on player 1, as a player would, once this station has shown two
 * players in the ring for 8 s (a lone press, or one before the other stations see both, gets "Could
 * not begin the battle." and both are sent away). "join" is for the second player: it presses once
 * this station's room logic has stopped for 1 s, which it does while another player's start is
 * pending. Each station sees the others' moves seconds late, so a fixed gap between the two presses
 * doesn't hold up; a second press 6 s or more after the first, or in the same frame, cancels both.
 * After either, A confirms the fighter in fighter select (3 times, 2 s apart, only while the room
 * logic is stopped), or its timer sends both back. The requests are read and the presses made from
 * the pad read (arena_poll), since the room logic stops while another start is pending. */
void hid_press(int button, double secs);   /* (hle_hid.c) */
static volatile int a_want, a_start, a_tries;   /* the request: place, "start", placement tries */
static volatile u8 a_cur;   /* this player's place as the room last showed it */
static volatile int a_join;   /* "join": waiting for another station's start */
static volatile double a_room;   /* when the room logic last ran */
static volatile double a_ring_since;   /* since when the room shows two players in the ring */
static int a_confirm;   /* A presses left for fighter select after a start or join */
static double a_confirm_at;
/* Reads the request file (every 1/4 s) and does the start press. Called with every read of player 1's
 * pad, not from the room: while another player's start is pending this station's room update stops
 * running, and the second press has to land then. */
static void arena_request(int want, const char *word, const char *from, double now) {
    if (want < 1 || want > 4) want = 0;
    a_start = a_join = a_confirm = 0;
    a_tries = 0;
    a_want = want;
    a_start = want && want <= 2 && !strcmp(word, "start");
    a_join = want && want <= 2 && !strcmp(word, "join");
    fprintf(stderr, "[arena] place request (%s): %d%s at %.2f s\n", from, want, a_start ? " start" : a_join ? " join" : "", now);
    fflush(stderr);
}
long online_arena_assignment(int *place, char *role, int rcap);   /* (hle_online.c) */
void arena_poll(void) {
    static const char *path;
    static long long seen = -1;
    static long bracket_seen;
    static double next;
    if (!path) path = getenv("SSBU_ARENA_PLACE_FILE") ? getenv("SSBU_ARENA_PLACE_FILE") : "";
    double now = rt_now_ns() / 1e9;
    if (now < next) return;
    next = now + 0.25;
    if (*path) {
        struct stat st;
#ifdef _WIN32
        long long m = stat(path, &st) ? -1 : (long long)st.st_mtime * 1000000ll + st.st_size;   /* (seconds only: the size tells "1" and "1 start" apart) */
#else
        long long m = stat(path, &st) ? -1 : (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
        if (m != seen) {
            seen = m;
            int want = 0;
            char word[16] = "";
            FILE *f = m < 0 ? NULL : fopen(path, "r");
            if (f) {
                if (fscanf(f, "%d %15s", &want, word) < 1) want = 0;
                fclose(f);
            }
            arena_request(want, word, "file", now);
        }
    }
    int place;
    char role[8];
    long seq = online_arena_assignment(&place, role, sizeof role);
    if (seq != bracket_seen) {
        bracket_seen = seq;
        arena_request(place, role, "bracket", now);
    }
    if (a_start && a_want && a_cur == a_want && a_ring_since && now >= a_ring_since + 8.0) {   /* both in the ring, as this station sees it, for 8 s */
        hid_press(10, 0.15);   /* B_PLUS */
        a_start = 0;
        a_confirm = 3;
        a_confirm_at = now + 2.0;
        fprintf(stderr, "[arena] start pressed at %.2f s\n", now);
        fflush(stderr);
    }
    if (a_join && a_cur == a_want && a_room && now >= a_room + 1.0) {   /* the room went quiet: another player's start is up */
        hid_press(10, 0.15);   /* B_PLUS */
        a_join = 0;
        a_confirm = 3;
        a_confirm_at = now + 2.0;
        fprintf(stderr, "[arena] joined the start at %.2f s\n", now);
        fflush(stderr);
    }
    /* Then fighter select (the room logic stops under it) waits for A from each player, or sends both
     * back when its timer runs out: confirm the fighter on the cursor. */
    if (a_confirm && now >= a_confirm_at && a_room && now >= a_room + 1.0) {
        hid_press(0, 0.1);   /* B_A */
        a_confirm--;
        a_confirm_at = now + 2.0;
        fprintf(stderr, "[arena] fighter confirmed at %.2f s\n", now);
        fflush(stderr);
    }
}
static void arena_place(Cpu *c, u64 room) {
    static double next;
    a_room = rt_now_ns() / 1e9;
    u8 own = *P(u8, room + 0x1920);
    if (own >= 8) return;
    u8 cur = *P(u8, room + 0xaf8 + (u64)own * 0x1e8);
    int ring = 0;
    for (int i = 0; i < 8; i++) ring += *P(u8, room + 0xaf8 + (u64)i * 0x1e8) == 1;
    if (ring < 2) a_ring_since = 0;
    else if (!a_ring_since) a_ring_since = a_room;
    if (cur != a_cur) { fprintf(stderr, "[arena] own place %u -> %u at %.2f s\n", a_cur, cur, rt_now_ns() / 1e9); fflush(stderr); a_cur = cur; }
    int want = a_want;
    if (!want) return;
    if (cur == want) a_tries = 5;   /* there: leave it to the game from now on */
    u64 ctl = *P(u64, room + 0x1950);
    double now = rt_now_ns() / 1e9;
    if (cur != want && a_tries < 5 && ctl && now >= next) {
        next = now + 0.5;
        a_tries++;
        u64 fn = *P(u64, *P(u64, ctl) + 0x30);
        Cpu saved = *c;
        u64 sp = (c->sp - 0x40) & ~15ull, a = sp + 0x20, b = sp + 0x24;
        *P(u8, a) = own;
        *P(u8, b) = (u8)want;
        c->sp = sp;
        c->x[0] = ctl;
        c->x[1] = a;
        c->x[2] = b;
        rt_call(c, fn);
        u64 r = c->x[0];
        *c = saved;
        fprintf(stderr, "[arena] place: slot %u was %u, asked %d -> %llx\n", own, cur, want, (unsigned long long)r);
        fflush(stderr);
    }
}
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
    arena_place(c, room);
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
