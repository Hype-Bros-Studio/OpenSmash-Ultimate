/* The Online Tourney table from the lobby's bracket (tourney_table.h): tools/bracket_tui.py's
 * table_from_view, step for step. */
#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tourney_table.h"

/* ---- JSON, as much as the view needs ------------------------------------------------------- */
static const char *json_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *json_str(const char *p, char *out, int cap) {   /* p at '"'; returns past it (NULL: bad) */
    int n = 0;
    for (p++; *p && *p != '"'; p++) {
        unsigned ch = (unsigned char)*p;
        if (ch == '\\') {
            ch = (unsigned char)*++p;
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

static void json_get_str(const char *obj, const char *key, char *out, int cap) {   /* "" when missing or not a string */
    const char *v = json_get(obj, key);
    out[0] = 0;
    if (v && *v == '"' && !json_str(v, out, cap)) out[0] = 0;
}

static const char *json_next(const char *p) {   /* past an array element and its comma */
    p = json_skip(p);
    if (p && *(p = json_ws(p)) == ',') p = json_ws(p + 1);
    return p;
}

static int same_name(const char *a, const char *b) {   /* (bracket.py's norm: no case, no surrounding spaces) */
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ') la--;
    while (lb && b[lb - 1] == ' ') lb--;
    if (la != lb || !la) return 0;
    for (size_t i = 0; i < la; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* ---- the table -------------------------------------------------------------------------------- */
#define MAX_ROUNDS 5
#define MIN_ROUNDS 3
#define MAX_ROWS 256
#define MAX_FIGHTERS 128
#define NAME 48
#define ID 24
typedef struct { char id[ID], side[12], name[2][NAME], winner[NAME], from[2][ID]; int kid[2], height; } Row;
static struct {
    Row row[MAX_ROWS];
    int rows, parent[MAX_ROWS];
    char names[1 << MAX_ROUNDS][NAME];
    int win[MAX_ROUNDS][1 << (MAX_ROUNDS - 1)], one[1 << (MAX_ROUNDS - 1)];   /* one: round 0's match has one entrant */
    char fname[MAX_FIGHTERS][NAME], fighter[MAX_FIGHTERS][24];
    int fighters;
} tt;

static int find_row(const char *id) {
    for (int i = 0; id[0] && i < tt.rows; i++)
        if (!strcmp(tt.row[i].id, id)) return i;
    return -1;
}

static int height(int r) {   /* (a bad view's cycle stops at the row being measured) */
    Row *x = &tt.row[r];
    if (x->height >= 0) return x->height;
    x->height = 0;
    int h = 0;
    for (int k = 0; k < 2; k++)
        if (x->kid[k] >= 0 && height(x->kid[k]) > h) h = height(x->kid[k]);
    return x->height = 1 + h;
}

static int chain(const char *name, int g, int i) {   /* a leaf at round g's match i */
    snprintf(tt.names[i << (g + 1)], NAME, "%s", name);
    for (int h = 0; h <= g; h++)
        for (int j = i << (g - h); j < (i + 1) << (g - h); j++) {
            tt.win[h][j] = j << (h + 1);
            if (!h) tt.one[j] = 1;
        }
    return i << (g + 1);
}

static int place(int r, int g, int i) {   /* a row at round g's match i: its winner's entrant (-1 not played) */
    Row *x = &tt.row[r];
    int got[2];
    for (int k = 0; k < 2; k++) {
        if (!g) {
            snprintf(tt.names[2 * i + k], NAME, "%s", x->name[k]);
            got[k] = 2 * i + k;
        } else if (x->kid[k] >= 0) got[k] = place(x->kid[k], g - 1, 2 * i + k);
        else got[k] = chain(x->name[k], g - 1, 2 * i + k);
    }
    int win = -1;
    for (int k = 1; k >= 0; k--)
        if (same_name(x->name[k], x->winner)) win = got[k];
    tt.win[g][i] = win;
    if (!g) tt.one[i] = 0;
    return win;
}

static int read_view(const char *js) {   /* the bracket's rows (not a pool's) and the fighters; -1: bad */
    memset(&tt, 0, sizeof tt);
    const char *p = json_get(js, "matches");
    if (!p || *p != '[') return -1;
    for (p = json_ws(p + 1); p && *p == '{' && tt.rows < MAX_ROWS; p = json_next(p)) {
        Row *x = &tt.row[tt.rows];
        const char *f;
        json_get_str(p, "side", x->side, sizeof x->side);
        if (json_get(p, "pool") || !x->side[0]) continue;
        json_get_str(p, "id", x->id, sizeof x->id);
        json_get_str(p, "a", x->name[0], NAME);
        json_get_str(p, "b", x->name[1], NAME);
        json_get_str(p, "winner", x->winner, NAME);
        if ((f = json_get(p, "from")) && *f == '[') {
            f = json_ws(f + 1);
            for (int k = 0; k < 2 && f && *f && *f != ']'; k++, f = json_next(f))
                if (*f == '{') json_get_str(f, "winner", x->from[k], ID);
        }
        if (x->id[0] && find_row(x->id) < 0) tt.rows++;
    }
    if ((p = json_get(js, "fighters")) && *p == '{') {   /* name: "0x<hash>[/<costume>]" */
        for (p = json_ws(p + 1); p && *p == '"' && tt.fighters < MAX_FIGHTERS;) {
            char *name = tt.fname[tt.fighters], *v = tt.fighter[tt.fighters];
            p = json_str(p, name, NAME);
            if (!p || *(p = json_ws(p)) != ':') break;
            p = json_ws(p + 1);
            v[0] = 0;
            if (*p == '"') json_str(p, v, 24);
            p = json_next(p);
            if (v[0] && strspn(v, "0123456789abcdefx/") == strlen(v)) tt.fighters++;
        }
    }
    return 0;
}

int tourney_table(const char *js, const char *me, int can_end, char *out, int cap) {
    static const int classes[MAX_ROUNDS + 1] = {0, 0, 0, 1, 3, 5};   /* 1v1 classes: rounds -> class */
    if (read_view(js) < 0 || !tt.rows) return 0;
    char cur[ID];
    json_get_str(js, "current", cur, sizeof cur);
    int focus = cur[0] ? find_row(cur) : tt.rows - 1;
    if (focus < 0) return 0;   /* (a pool's match being played) */

    /* The tree's links: a side whose player is the winner of a listed row on the same side (or any side,
     * for the finals) comes from that row; anything else is a leaf. */
    for (int r = 0; r < tt.rows; r++) {
        Row *x = &tt.row[r];
        x->height = -1;
        tt.parent[r] = -1;
        for (int k = 0; k < 2; k++) {
            int c = find_row(x->from[k]);
            x->kid[k] = c >= 0 && (!strcmp(tt.row[c].side, x->side) || !strcmp(x->side, "finals")) ? c : -1;
        }
    }
    for (int r = 0; r < tt.rows; r++)
        for (int k = 0; k < 2; k++) {
            int c = tt.row[r].kid[k];
            if (c >= 0 && !strcmp(tt.row[c].side, tt.row[r].side)) tt.parent[c] = r;
        }
    int path[MAX_ROUNDS] = {focus}, len = 1;   /* (the first MAX_ROUNDS of the way up) */
    for (int at = focus, steps = 0; tt.parent[at] >= 0 && steps < tt.rows; steps++) {
        at = tt.parent[at];
        if (len < MAX_ROUNDS) path[len++] = at;
    }
    int root = path[len - 1], h = height(root);
    int rounds = h < MIN_ROUNDS ? MIN_ROUNDS : h > MAX_ROUNDS ? MAX_ROUNDS : h;
    place(root, rounds - 1, 0);
    if (tt.win[rounds - 1][0] >= 0 && (rounds == MAX_ROUNDS || !can_end)) return 0;

    int now = rounds - 1, local = -1, best = -1, mine = 0;   /* the round being played: the first with a match open */
    for (int g = rounds - 1; g >= 0; g--)
        for (int i = 0; i < 1 << (rounds - 1 - g); i++)
            if (tt.win[g][i] < 0) now = g;
    for (int e = 0; e < 1 << rounds; e++) {   /* this player's entrant: the one that won the most, the later on a tie */
        if (!same_name(tt.names[e], me)) continue;
        int won = 0;
        for (int g = 0; g < rounds; g++)
            for (int i = 0; i < 1 << (rounds - 1 - g); i++) won += tt.win[g][i] == e;
        if (won >= best) local = e, best = won;
    }
    for (int i = 0; i < 1 << (rounds - 1 - now); i++) {
        int a = now ? tt.win[now - 1][2 * i] : 2 * i, b = now ? tt.win[now - 1][2 * i + 1] : tt.one[i] ? -2 : 2 * i + 1;
        if (local >= 0 && (local == a || local == b)) mine = i;
    }

    int o = snprintf(out, (size_t)cap, "class %d\nevents %d\nlocal %d\nme %d\n", classes[rounds], 1 << rounds, local, mine);
#define PUT(...) (o += snprintf(out + o, (size_t)(cap > o ? cap - o : 0), __VA_ARGS__))
    for (int g = 0; g < rounds; g++)
        for (int i = 0; i < 1 << (rounds - 1 - g); i++) {
            PUT("match %d %d %d", g, i, tt.win[g][i]);
            if (!g && tt.one[i]) PUT(" %d", 2 * i);
            else if (!g) PUT(" %d %d", 2 * i, 2 * i + 1);
            PUT("\n");
        }
    for (int e = 0; e < 1 << rounds; e++)
        if (tt.names[e][0]) PUT("name %d %s\n", e, tt.names[e]);
    for (int e = 0; e < 1 << rounds; e++) {
        int f = -1;
        for (int k = 0; tt.names[e][0] && k < tt.fighters; k++)
            if (same_name(tt.fname[k], tt.names[e])) f = k;
        if (f >= 0) PUT("fighter %d %s\n", e, tt.fighter[f]);
    }
#undef PUT
    return o < cap;
}
