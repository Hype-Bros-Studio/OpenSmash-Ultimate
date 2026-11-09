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
 * The current scene's name and the stack's depth are logged whenever they change.
 *
 * With SSBU_ARENA_BRACKET=1 (and not SSBU_TOURNEY_UI=0) the table also comes and goes by itself, from the
 * lobby's bracket (tui_auto, below); the file is then optional. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"
#include "net.h"

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
static GuestFn *show_match_orig;   /* (table_show_match's) */
static const char *tui_path;
static int tui_bracket;           /* SSBU_ARENA_BRACKET=1 (and not SSBU_TOURNEY_UI=0): the table from the lobby's bracket */
static const char *tui_text;      /* the bracket lines for the request being made (NULL: the file's) */
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
 *   name E TEXT             entrant E's name (UTF-8, up to 10 UTF-16 units shown)
 * Every round-0 match needs an entrant, and every match up to the round being played needs its children's
 * winners: the table's bracket (main+0x1d07780) does .at(0) on them.
 * Matches are made as the tourney's sequence makes them (main+0x237ee50, at 0x237f3c0): 0x60 bytes, owned by
 * shared_ptrs (control blocks {vtable 0x50b9a40, owners - 1, weak, match}) from their round's vector at
 * T+8+R*0x18 and their parent's children at +0x10, and holding their parent at +0x28. */
#define MATCH_CTRL_VTABLE 0x50b9a40
#define MAX_ROUNDS 5
#define MAX_MATCHES 32
#define MAX_ENTRANTS 8
#define MAX_EVENTS 64
#define PLAYER_INIT 0x17abd00     /* an entrant's player settings (+0x28 and +0x1f0), as the builder makes them */
typedef struct { int winner, n, e[MAX_ENTRANTS]; u64 m, ctrl; } TuiMatch;
static struct {
    int cls, events, local, me, rounds, count[MAX_ROUNDS];
    TuiMatch match[MAX_ROUNDS][MAX_MATCHES];
    char name[MAX_EVENTS][48];
} br;

static int parse_bracket(const char *text) {
    memset(&br, 0, sizeof br);
    br.cls = 1;
    br.local = -1;
    char line[256];
    for (const char *s = text; *s;) {
        size_t len = strcspn(s, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), s);
        s += len + (s[len] == '\n');
        int r, m, w, k = 0, n = 0;
        if (sscanf(line, "name %d %n", &r, &n) == 1 && n > 0 && r >= 0 && r < MAX_EVENTS) {
            snprintf(br.name[r], sizeof br.name[r], "%s", line + n);
            br.name[r][strcspn(br.name[r], "\r\n")] = 0;
            continue;
        }
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
    if (br.events > MAX_EVENTS) br.events = MAX_EVENTS;
    return br.rounds > 0 && br.events > 0;
}

static int read_bracket(void) {
    static char text[16384];
    FILE *f = tui_path ? fopen(tui_path, "r") : NULL;
    if (!f) return 0;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    return parse_bracket(text);
}

/* ---- the table from the lobby's bracket (SSBU_ARENA_BRACKET=1) -------------------------------
 * GET /bracket.json (tools/bracket.py's view: "players" in seed order, "format", "current", and
 * "matches" rows with "id" and, once decided, "winner"; rows with a bye left out) made into the lines
 * above, for this player. The game draws fixed trees (main+0x239d810's classes); the 1v1 ones are 8, 16
 * and 32 entrants with 3, 4 and 5 rounds. The winners side goes in the last rounds of the smallest one it
 * fits, each of its seats under a chain of matches already decided, at the top of its own subtree's
 * slots; the other slots are blank entrants, and byes are blank entrants that lose. While a double
 * elimination plays its losers side or grand finals, or once it is over, there is no table; a single
 * elimination's end shows its final decided (but not with 32 entrants: table_show_match).
 * (tools/bracket_tui.py makes the same from bracket.json, for SSBU_TOURNEY_UI_FILE.) */
static const char *json_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *json_str(const char *p, char *out, int cap) {   /* p at '"'; returns past it (NULL: bad) */
    int n = 0;
    for (p++; *p && *p != '"'; p++) {
        u32 ch = (u8)*p;
        if (ch == '\\') {
            ch = (u8)*++p;
            if (ch == 'u') {
                unsigned v;
                if (sscanf(p + 1, "%4x", &v) != 1) return NULL;
                ch = v;
                p += 4;
            } else ch = ch == 'n' || ch == 'r' || ch == 't' ? ' ' : ch;
            if (ch < 0x20) ch = ' ';
            char u[3];
            int k = ch < 0x80 ? 0 : ch < 0x800 ? 1 : 2;
            if (!k) u[0] = (char)ch;
            else if (k == 1) { u[0] = (char)(0xc0 | ch >> 6); u[1] = (char)(0x80 | (ch & 0x3f)); }
            else { u[0] = (char)(0xe0 | ch >> 12); u[1] = (char)(0x80 | ((ch >> 6) & 0x3f)); u[2] = (char)(0x80 | (ch & 0x3f)); }
            for (int i = 0; i <= k; i++) if (n < cap - 1) out[n++] = u[i];
        } else if (n < cap - 1) out[n++] = (char)ch;
    }
    if (cap) out[n] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *json_skip(const char *p) {   /* past one value */
    p = json_ws(p);
    if (*p == '"') return json_str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char end = *p == '{' ? '}' : ']';
        p = json_ws(p + 1);
        while (p && *p && *p != end) {
            if (end == '}') {
                p = json_str(p, NULL, 0);
                if (!p || *(p = json_ws(p)) != ':') return NULL;
                p++;
            }
            p = json_skip(p);
            if (p && *(p = json_ws(p)) == ',') p = json_ws(p + 1);
        }
        return p && *p == end ? p + 1 : NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static const char *json_get(const char *obj, const char *key) {   /* an object member's value, or NULL */
    char k[24];
    const char *p = json_ws(obj);
    if (*p != '{') return NULL;
    for (p = json_ws(p + 1); p && *p == '"';) {
        p = json_str(p, k, sizeof k);
        if (!p || *(p = json_ws(p)) != ':') return NULL;
        p = json_ws(p + 1);
        if (!strcmp(k, key)) return p;
        p = json_skip(p);
        if (p && *(p = json_ws(p)) == ',') p = json_ws(p + 1);
    }
    return NULL;
}

static int same_name(const char *a, const char *b) {   /* (bracket.py's norm: no case, no surrounding spaces) */
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ') la--;
    while (lb && b[lb - 1] == ' ') lb--;
    if (la != lb || !la) return 0;
    for (size_t i = 0; i < la; i++)
        if (tolower((u8)a[i]) != tolower((u8)b[i])) return 0;
    return 1;
}

#define VIEW_SEATS 32
static int table_from_view(const char *js, const char *me, char *out, int cap) {
    static const int classes[][2] = {{3, 1}, {4, 3}, {5, 5}};   /* 1v1 classes: rounds, class */
    static char name[MAX_EVENTS][48], won[MAX_ROUNDS][VIEW_SEATS / 2][48];
    char fmt[16] = "", cur[24] = "", id[24];
    int n = 0;
    memset(won, 0, sizeof won);
    const char *p = json_get(js, "players");
    if (!p || *p != '[') return 0;
    for (p = json_ws(p + 1); p && *p == '"' && n < VIEW_SEATS + 1;) {
        p = json_str(p, name[n++], sizeof name[0]);
        if (p && *(p = json_ws(p)) == ',') p = json_ws(p + 1);
    }
    if (!p || n < 2 || n > VIEW_SEATS) return 0;
    if ((p = json_get(js, "format")) && *p == '"') json_str(p, fmt, sizeof fmt);
    if ((p = json_get(js, "current")) && *p == '"') json_str(p, cur, sizeof cur);
    if (cur[0] ? cur[0] != 'W' : strcmp(fmt, "single")) return 0;   /* (a double elimination's losers side, or its end) */
    if ((p = json_get(js, "matches")) && *p == '[')
        for (p = json_ws(p + 1); p && *p == '{';) {
            const char *row = p, *v;
            int r, i;
            id[0] = 0;
            if ((v = json_get(row, "id")) && *v == '"') json_str(v, id, sizeof id);
            if (sscanf(id, "W%d-%d", &r, &i) == 2 && r >= 1 && r <= MAX_ROUNDS && i >= 1 && i <= VIEW_SEATS / 2 &&
                (v = json_get(row, "winner")) && *v == '"')
                json_str(v, won[r - 1][i - 1], sizeof won[0][0]);
            p = json_skip(row);
            if (p && *(p = json_ws(p)) == ',') p = json_ws(p + 1);
        }

    int k = 1, size = 2;
    while (size < n) size *= 2, k++;
    int order[VIEW_SEATS] = {1}, len = 1;   /* seeds in bracket order (bracket.py's seed_order) */
    while (len < size) {
        for (int i = len - 1; i >= 0; i--) { order[2 * i + 1] = 2 * len + 1 - order[i]; order[2 * i] = order[i]; }
        len *= 2;
    }
    int local = -1;   /* the seats: round 1's sides in order */
    static char seat[MAX_EVENTS][48];
    memset(seat, 0, sizeof seat);
    for (int s = 0; s < size; s++) {
        if (order[s] - 1 < n) snprintf(seat[s], sizeof seat[s], "%s", name[order[s] - 1]);
        if (same_name(seat[s], me)) local = s;
    }
    int rounds = 0, cls = 0;
    for (int c = 0; c < 3 && !rounds; c++)
        if (classes[c][0] >= k) rounds = classes[c][0], cls = classes[c][1];
    if (!rounds) return 0;
    /* The table draws entrant E in its slot E (of the class's 1 << rounds), so seat S is entrant S << off,
     * at the top of its own subtree; the other entrants are blank. */
    int off = rounds - k, events = 1 << rounds, win[MAX_ROUNDS][MAX_MATCHES];
    if (local >= 0) local <<= off;
    for (int g = 0; g < rounds; g++)
        for (int i = 0; i < 1 << (rounds - 1 - g); i++) {
            if (g < off) {   /* a chain of decided matches above a seat, or above a blank entrant */
                win[g][i] = g ? win[g - 1][2 * i] : 2 * i;
                continue;
            }
            const char *w = won[g - off][i];
            win[g][i] = -1;
            for (int s = 0; w[0] && s < size; s++)
                if (same_name(seat[s], w)) win[g][i] = s << off;
            if (!w[0] && g == off && !seat[2 * i][0] != !seat[2 * i + 1][0])   /* (a bye: the other side goes on) */
                win[g][i] = (seat[2 * i][0] ? 2 * i : 2 * i + 1) << off;
        }
    /* With the final decided the table moves on past it (fill_bracket's after_final, table_show_match), which
     * a 5-round class has no room for. */
    if (win[rounds - 1][0] >= 0 && (rounds == MAX_ROUNDS || !show_match_orig)) return 0;
    int now = rounds - 1, mine = 0;   /* the round being played: the first with a match open */
    for (int g = rounds - 1; g >= 0; g--)
        for (int i = 0; i < 1 << (rounds - 1 - g); i++)
            if (win[g][i] < 0) now = g;
    for (int i = 0; i < 1 << (rounds - 1 - now); i++) {
        int a = now ? win[now - 1][2 * i] : 2 * i, b = now ? win[now - 1][2 * i + 1] : off ? -2 : 2 * i + 1;
        if (local >= 0 && (local == a || local == b)) mine = i;
    }
    int o = snprintf(out, (size_t)cap, "class %d\nevents %d\nlocal %d\nme %d\n", cls, events, local, mine);
    for (int g = 0; g < rounds; g++)
        for (int i = 0; i < 1 << (rounds - 1 - g); i++) {
            o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), "match %d %d %d", g, i, win[g][i]);
            if (g == 0 && off) o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), " %d", 2 * i);
            else if (g == 0) o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), " %d %d", 2 * i, 2 * i + 1);
            o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), "\n");
        }
    for (int s = 0; s < size; s++)
        if (seat[s][0]) o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), "name %d %s\n", s << off, seat[s]);
    return o < cap;
}

/* A player's name in an entrant: {FNV hash of the UTF-16, length, UTF-16 (10 at most) and its end}. */
static void put_name16(u64 rec, const char *s) {
    u32 h = 0x811c9dc5u, n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n < 10) {
        u32 ch = *p++;
        if (ch >= 0xc0) {   /* (UTF-8: the BMP only) */
            int more = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : 1;
            ch &= 0x3f >> more;
            for (; more && (*p & 0xc0) == 0x80; more--) ch = (ch << 6) | (*p++ & 0x3f);
            if (ch > 0xffff) ch = '?';
        }
        *P(u16, rec + 0xc + n * 2) = (u16)ch;
        h = (h * 0x89) ^ ch;
        n++;
    }
    *P(u16, rec + 0xc + n * 2) = 0;
    *P(u32, rec + 4) = h;
    *P(u32, rec + 8) = n;
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
        u64 e = ev + i * 0x820;
        for (int k = 0; k < 2; k++) {              /* its two players (the second for teams), at +0 and +0x1c8 */
            u64 rec = e + k * 0x1c8;
            *P(u8, rec) = 0xff;
            put_name16(rec, k ? "" : br.name[i]);
            *P(u32, rec + 0x24) = 1;
            call4(c, base + PLAYER_INIT, rec + 0x28, 0, 0, 0, sp);
        }
        *P(u32, e + 0x814) = br.name[i][0] != 0;   /* its players (main+0x239dd10 loads each one's fighter); 0: a bye */
        *P(u32, e + 0x81c) = i + 1;   /* (stands for the player's id) */
        *P(u8, e + 0x810) = i == br.local;
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
    /* The final's parent: once the match in view is decided the table moves on to its parent's (main+0x18bafc0,
     * scene+0x1d0 += 1 and +0x1d4 = the parent's +8), and the final has none. This one, with no owners, is
     * match 0 of the round after the last, which has no matches (with 5 rounds that vector would be the
     * entrants': table_from_view shows no table then). */
    static u64 after_final;
    if (!after_final) after_final = galloc(c, 0x60, sp);
    for (int r = br.rounds; r < MAX_ROUNDS; r++) vec_alloc(c, t + 8 + r * 0x18, 0, 0x10, sp);
    if (after_final && br.rounds < MAX_ROUNDS && br.count[br.rounds - 1]) {
        for (int k = 0; k < 0x60; k += 8) *P(u64, after_final + k) = 0;
        *P(u32, after_final + 4) = br.rounds;
        *P(i32, after_final + 0x50) = -1;
        *P(i32, after_final + 0x58) = -1;
        *P(u64, br.match[br.rounds - 1][0].m + 0x28) = after_final;
        *P(u64, br.match[br.rounds - 1][0].m + 0x30) = 0;
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
        if (fresh && (tui_text ? parse_bracket(tui_text) : read_bracket())) fill_bracket(c, sp);
        else if (fresh) fprintf(stderr, "[tourney-ui] no bracket in %s\n", tui_text ? "the lobby's" : tui_path);
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

/* With SSBU_ARENA_BRACKET=1: the table over the room each time hle_online.c fetches the bracket for it
 * (this player first placed in a bracket, a match it played decided), and the room back once the
 * assignment has moved on since (the next place request), after TABLE_SHOWN s at least. */
#define TABLE_SHOWN 6.0
long online_arena_table(long have, char *out, int cap, long *after);   /* (hle_online.c) */
long online_arena_assignment(int *place, char *role, int rcap);
static void tui_auto(Cpu *c, u64 seq, double now) {
    static char js[64 * 1024], text[8192];
    static long seen, after, shown_after;
    static double shown;
    int place;
    char role[8];
    long aseq = online_arena_assignment(&place, role, sizeof role);
    if (shown && !strcmp(tui_cur, TABLE_NAME)) {
        if (aseq != shown_after && now >= shown + TABLE_SHOWN) {
            shown = 0;
            tui_request(c, seq, "room");
        }
        return;
    }
    shown = 0;
    if (strcmp(tui_cur, ROOM_NAME) || tui_depth || *P(u64, rt_module_main.base + HOLDER)) return;   /* (over the room alone) */
    long s = online_arena_table(seen, js, sizeof js, &after);
    if (s == seen) return;
    seen = s;
    const char *me = net_player_name();
    if (!table_from_view(js, me ? me : "", text, sizeof text)) {
        fprintf(stderr, "[tourney-ui] no table for the lobby's bracket now (a losers round, the bracket over, or no 1v1 class fits)\n");
        fflush(stderr);
        return;
    }
    tui_text = text;
    tui_request(c, seq, "table");
    tui_text = NULL;
    shown = now;
    shown_after = after;
}

/* The Table scene's state that shows the match in view (main+0x18bafc0, a state handler the scene calls
 * through a pointer): once that match is decided it moves the view on to the match's parent (scene+0x1d0
 * round, +0x1d4 match). Past the final that is fill_bracket's after_final, in a round the table has no
 * places for (its next state asks the bracket view for them, and .at() aborts): back on the final it
 * stays, the winner's line lit up to the trophy. */
#define TABLE_SHOW_MATCH 0x18bafc0
static void table_show_match(Cpu *c) {
    u64 scene = c->x[0];
    show_match_orig(c);
    if (tui_holder && br.rounds && *P(u32, scene + 0x1d0) >= (u32)br.rounds) {
        *P(u32, scene + 0x1d0) = (u32)br.rounds - 1;
        *P(u32, scene + 0x1d4) = 0;
    }
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
    if (tui_bracket) tui_auto(c, seq, now);
    if (!tui_path) return;
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
    if (tui_path && !*tui_path) tui_path = NULL;
    const char *b = getenv("SSBU_ARENA_BRACKET"), *off = getenv("SSBU_TOURNEY_UI");
    tui_bracket = b && !strcmp(b, "1") && !(off && !strcmp(off, "0"));
    if (!tui_path && !tui_bracket) return;
    u64 orig = rt_hook_function(rt_module_main.base + SEQ_UPDATE, rt_native(tui_update, "SSBU_TOURNEY_UI_FILE hook"));
    if (orig) tui_orig = rt_lookup(orig);
    u64 show = rt_hook_function(rt_module_main.base + TABLE_SHOW_MATCH, rt_native(table_show_match, "tourney-ui show match"));
    if (show) show_match_orig = rt_lookup(show);
    else fprintf(stderr, "[tourney-ui] the table's match state not found: no table once the final is decided\n");
    fprintf(stderr, "[tourney-ui] Arena sequence %s%s\n", orig ? "hooked" : "not found",
            tui_bracket ? " (the table from the lobby's bracket)" : "");
    fflush(stderr);
}
