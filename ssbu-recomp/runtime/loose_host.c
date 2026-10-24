/* loose_host.c - the loose-file layer (loose_files.c) as part of the exe (plan\phases\N4_own_fighter.md, section 3.3).
 * OPT-IN for now: nothing happens unless SSBU_LOOSE_FILES=1 is set (the layer is a first stage; the owner's default
 * build must not change because a characters\ folder exists). With it set: the hook on the per-file decompress is
 * installed as soon as the hook table can find the function, which is before the game opens its archive.
 * No other host file calls into this one: it starts itself from a constructor, so a link that leaves this file
 * out is simply a game without the layer. Contains nothing of the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

typedef void (*HookFn)(Ctx *c, void *user);
typedef ImportFn HookOrig;
int hook_install(uint64_t addr, HookFn repl, void *user, ImportFn *orig);      /* fighter_sdk\hook_host.c (in nnro.c) */

#include "loose_files.c"

static void loose_host_log(const char *s) { printf("%s\n", s); fflush(stdout); trace_printf("%s\n", s); }

/* ---------------------------------------------------------------- AUDIO HOT RELOAD (SSBU_AUDIO_HOTRELOAD=1)
 * A sound bank edited while the game runs (NUS3BANK-Editor, `nus3 replace --manifest`) takes effect without a restart.
 * The editor replaces each file atomically and writes <mod folder>\.nus3_changes.json LAST ({generation, files[]});
 * a thread of ours polls that file and, when the generation changes, compares every sound file of the folder with
 * what was last read (size, write time). files[] is only a hint: two edits between two polls are both seen.
 *
 * How the game keeps a fighter's bank (13.0.4, read in the decompiled code):
 *   - the app's bank table (0x7103739b90: 300 slots of 0x60 bytes, +0x28 the .nus3bank path index, +0x2c the
 *     .nus3audio's) loads both files through the resource service and calls the engine's LoadBank 0x710395fe70
 *     (x0 the .nus3bank buffer, x1 its size, x2 the slot's handle, x3 the .nus3audio buffer = the loaded-data row's);
 *   - LoadBank makes a bank object (0x2190 bytes, vtable 0x7105298c58) and links it into the engine's bank list
 *     (*0x7106dd44b8 + 0x28, next at +0x18). Its parser part at +0xc0 (vtable 0x7105298e70, 0x710396ffd0) keeps RAW
 *     POINTERS into both buffers: +0x28 the .nus3audio's start, +0x88 its PACK chunk, +0x98 its ADOF chunk
 *     ({offset, size} per stream, offsets from the file's start), +0x38 / +0x30 the DTON objects, +0x48 / +0x40 the
 *     tone cache (one pointer per TONE entry, made at the tone's first use by 0x7103970bf0);
 *   - a tone object (0x7103971220) of type 0 (+0x10) holds at *(+0x18) {pointer = audio start + ADOF[2i], size =
 *     ADOF[2i+1]} with i = +8: a voice reads its stream through that pointer; the engine never copies the streams
 *     and never frees the .nus3audio buffer (the resource service does, when the bank's files are released).
 * So a STREAMS-ONLY change (same number of streams) is swapped live: the new file goes into a NEW buffer of ours
 * (the game's buffer is left as it is: voices that play keep reading it), then the parser's three pointers and every
 * DTON / cached tone's {pointer, size} are moved to the new buffer. The order of the writes and the buffer's padding
 * keep a reader that sees half of the change inside readable memory. Voices started afterwards play the new audio.
 * The hook on LoadBank hands the new buffer to a bank that the game builds again from the same (older) buffer.
 * What is NOT live: a different stream count, the .nus3bank (tone tables, parsed at LoadBank), the .tonelabel and
 * the sli: these take effect the next time the game reads the files (the folder's current file is served then).
 * Our buffers are never freed (a voice may still read an older one): a few MB per swap. */
static int g_lf_hot;
static volatile LONG g_lf_hot_live;
static HookOrig g_lf_orig_loadbank;
static uint8_t lf_r8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static int lf_stat(const char *path, uint32_t *size, long long *mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    *size = a.nFileSizeLow; *mtime = ((long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return 1;
}
// a chunk of a .nus3audio ("NUS3" size "AUDI", then {magic, size, data} from 0xc, as 0x710396ffd0 walks them): its offset, 0 = none
static uint32_t lf_n3_chunk(const uint8_t *p, uint32_t n, const char *magic, uint32_t *len) {
    if (n < 0x14 || memcmp(p, "NUS3", 4) || memcmp(p + 8, "AUDI", 4)) return 0;
    for (uint32_t o = 0xc; o + 8 <= n;) {
        uint32_t s; memcpy(&s, p + o + 4, 4);
        if (!memcmp(p + o, magic, 4)) { *len = s; return s <= n - o - 8 ? o : 0; }
        if (s > n - o - 8) return 0;
        o += 8 + s;
    }
    return 0;
}
// one DTON / tone object: its stream {pointer, size} moved from the buffer [old, old + old_n) (or from an earlier
// state of the new one) to the new buffer. Only the object's own stream record (inside the object) is written.
static int lf_hot_tone(uint64_t t, uint64_t old, uint32_t old_n, uint64_t nb, uint32_t nb_n, const uint8_t *adof, uint32_t count) {
    if (!t || !lf_readable(t, 0x20) || lf_r32(t + 0x10) != 0) return 0;
    uint64_t s = lf_r64(t + 0x18);
    if (s < t + 0x20 || s + 12 > t + lf_r32(t + 0xc) || !lf_readable(s, 12)) return 0;
    uint64_t p = lf_r64(s);
    if (!((p >= old && p < old + old_n) || (p >= nb && p < nb + nb_n))) return 0;
    uint32_t i = lf_r32(t + 8), off, len, olen = lf_r32(s + 8);
    if (i >= count) return 0;
    memcpy(&off, adof + 8ull * i, 4); memcpy(&len, adof + 8ull * i + 4, 4);
    if (!len || p == nb + off) return 0;
    /* a reader may see the pointer of one state and the size of the other: the larger size goes with the buffer padded for it */
    if (len > olen) { lf_w64(s, nb + off); MemoryBarrier(); lf_w32(s + 8, len); }
    else { lf_w32(s + 8, len); MemoryBarrier(); lf_w64(s, nb + off); }
    return 1;
}
// the live swap of a .nus3audio of `nsize` bytes. -> number of banks moved to the new buffer
static int lf_hot_swap(LooseFile *f, uint32_t nsize) {
    uint64_t fsinfo, arc;
    if (!lf_tables(0, &fsinfo, &arc)) return 0;
    uint64_t row = lf_r64(fsinfo + 0x10) + 0x18ull * f->indice_index, src = lf_r64(row);
    if (!src || !lf_r8(row + 0xc) || lf_r8(row + 0xd) != 3) { lf_logf("[hot audio] %s: not loaded now: the game reads the new file at its next load", f->arc_path); return 0; }
    int again = f->hot_src == src && f->hot_buf;
    uint64_t cur = again ? f->hot_buf : src;                    // the buffer the engine's banks point into now
    uint32_t cur_n = again ? f->hot_cap : (f->served_size ? f->served_size : f->size);
    uint64_t root = lf_r64(GA(0x7106dd44b8ull));
    if (!root || !lf_readable(root + 0x28, 8)) return 0;
    uint64_t banks[8]; int nbank = 0;
    for (uint64_t b = lf_r64(root + 0x28), g = 0; b && g < 4096 && nbank < 8; b = lf_r64(b + 0x18), g++) {
        if (!lf_readable(b, 0x180)) break;
        if (lf_r64(b + 0xc0) == GA(0x7105298e70ull) && lf_r64(b + 0xe8) == cur) banks[nbank++] = b;
    }
    if (!nbank) { lf_logf("[hot audio] %s: loaded, but no bank of the sound engine uses it: the game reads the new file at its next load", f->arc_path); return 0; }
    uint64_t oadof = lf_r64(banks[0] + 0xc0 + 0x98);
    if (!oadof || !lf_readable(oadof, 8)) { lf_logf("[hot audio] %s: the bank has no ADOF chunk: not swapped", f->arc_path); return 0; }
    uint32_t ocount = lf_r32(oadof + 4) / 8, omax = 0;
    for (uint32_t i = 0; i < ocount; i++) { uint32_t l = lf_r32(oadof + 8 + 8ull * i + 4); if (l > omax) omax = l; }
    /* the new buffer: the file, padded so that an old offset or size read with the new pointer stays inside it */
    uint32_t cap = (uint32_t)(((uint64_t)(nsize > cur_n ? nsize : cur_n) + omax + 0xfff) & ~0xfffull);
    uint8_t *nb = VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!nb) return 0;
    FILE *fp = fopen(f->disk_path, "rb");
    size_t got = fp ? fread(nb, 1, nsize, fp) : 0;
    if (fp) fclose(fp);
    uint32_t alen = 0, plen = 0, aoff = got == nsize ? lf_n3_chunk(nb, nsize, "ADOF", &alen) : 0, poff = aoff ? lf_n3_chunk(nb, nsize, "PACK", &plen) : 0;
    const char *why = got != nsize ? "the file could not be read whole" : !aoff || !poff ? "no ADOF / PACK chunk" : alen / 8 != ocount ? "the number of streams changed" : NULL;
    for (uint32_t i = 0; !why && i < ocount; i++) {
        uint32_t o, l; memcpy(&o, nb + aoff + 8 + 8ull * i, 4); memcpy(&l, nb + aoff + 12 + 8ull * i, 4);
        if ((uint64_t)o + l > nsize) why = "a stream lies outside the file";
    }
    if (why) {
        VirtualFree(nb, 0, MEM_RELEASE);
        lf_logf("[hot audio] %s: NOT swapped live (%s: %u streams in the bank, %u in the file): the game reads the new file at its next load", f->arc_path, why, ocount, alen / 8);
        return 0;
    }
    uint64_t n = (uint64_t)(uintptr_t)nb;
    const uint8_t *adof = nb + aoff + 8;
    int moved = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nbank; k++) {
            uint64_t P = banks[k] + 0xc0;
            if (!pass) {    /* the start first: an object made meanwhile reads old offsets with the new start, inside the padded buffer */
                lf_w64(P + 0x28, n); MemoryBarrier();
                lf_w64(P + 0x98, n + aoff); lf_w64(P + 0x88, n + poff); MemoryBarrier();
            }
            uint32_t nd = lf_r32(P + 0x30), nt = lf_r32(P + 0x40);
            uint64_t dt = lf_r64(P + 0x38), tt = lf_r64(P + 0x48);
            for (uint32_t i = 0; dt && i < nd && i < 0x10000; i++) moved += lf_hot_tone(lf_r64(dt + 8ull * i), cur, cur_n, n, cap, adof, ocount);
            for (uint32_t i = 0; tt && i < nt && i < 0x10000; i++) moved += lf_hot_tone(lf_r64(tt + 8ull * i), cur, cur_n, n, cap, adof, ocount);
        }
        if (!pass) Sleep(50);       /* second pass: a tone made while the first ran */
    }
    f->hot_buf = n; f->hot_size = nsize; f->hot_cap = cap; MemoryBarrier(); f->hot_src = src;
    InterlockedIncrement(&g_lf_hot_live);
    lf_logf("[hot audio] %s: SWAPPED LIVE: %u streams, %u bytes in a new buffer %llx (was %llx); %d bank(s), %d tone records moved; voices that start now play the new audio",
            f->arc_path, ocount, nsize, (unsigned long long)n, (unsigned long long)cur, nbank, moved);
    return nbank;
}
// how many tone records of the engine's banks point into the swapped buffer and into the game's (older) one; logged when it changes
static void lf_hot_census(LooseFile *f) {
    static long last[LF_MAX];
    uint64_t root = lf_r64(GA(0x7106dd44b8ull)), src = f->hot_src, nb = f->hot_buf;
    if (!src || !nb || !root || !lf_readable(root + 0x28, 8)) return;
    long in_new = 0, in_old = 0; int nbank = 0;
    for (uint64_t b = lf_r64(root + 0x28), g = 0; b && g < 4096; b = lf_r64(b + 0x18), g++) {
        if (!lf_readable(b, 0x180)) break;
        if (lf_r64(b + 0xc0) != GA(0x7105298e70ull) || lf_r64(b + 0xe8) != nb) continue;
        nbank++;
        uint64_t P = b + 0xc0, tt = lf_r64(P + 0x48);
        for (uint32_t i = 0, nt = lf_r32(P + 0x40); tt && i < nt && i < 0x10000; i++) {
            uint64_t t = lf_r64(tt + 8ull * i);
            if (!t || !lf_readable(t, 0x20) || lf_r32(t + 0x10) != 0 || !lf_readable(lf_r64(t + 0x18), 8)) continue;
            uint64_t p = lf_r64(lf_r64(t + 0x18));
            if (p >= nb && p < nb + f->hot_cap) in_new++; else if (p >= src && p < src + f->served_size) in_old++;
        }
    }
    long key = in_new * 100000 + in_old * 10 + nbank;
    if (key == last[f - g_lf]) return;
    last[f - g_lf] = key;
    lf_logf("[hot audio] %s: %d bank(s) on the swapped buffer; tone records in use: %ld on the new audio, %ld on the old", f->arc_path, nbank, in_new, in_old);
}
// LoadBank 0x710395fe70: a bank built again from a buffer whose file was swapped gets the newer buffer
static void lf_loadbank(Ctx *c, void *user) {
    (void)user;
    if (g_lf_hot_live && c->x[3]) {
        for (int k = 0; k < g_lf_count; k++) {
            LooseFile *f = &g_lf[k];
            if (f->hot_src != c->x[3] || !f->hot_buf) continue;
            uint64_t fsinfo, arc;
            if (lf_tables(0, &fsinfo, &arc) && lf_r64(lf_r64(fsinfo + 0x10) + 0x18ull * f->indice_index) == c->x[3]) {
                lf_logf("[hot audio] %s: the bank is built again: it gets the swapped buffer %llx", f->arc_path, (unsigned long long)f->hot_buf);
                c->x[3] = f->hot_buf;
            } else f->hot_src = 0;
            break;
        }
    }
    g_lf_orig_loadbank(c);
}
// a folder's sound files against the disk: what changed is swapped live (.nus3audio) or left for the next load
static void lf_hot_check(const char *root, size_t rn) {
    int changed = 0;
    for (int k = 0; k < g_lf_count; k++) {
        LooseFile *f = &g_lf[k];
        if (strncmp(f->arc_path, "sound/", 6) || strncmp(f->disk_path, root, rn) || f->disk_path[rn] != '\\') continue;
        uint32_t size; long long mt;
        if (!lf_stat(f->disk_path, &size, &mt) || (size == f->size && mt == f->mtime)) continue;
        changed++;
        f->mtime = mt;
        uint64_t fsinfo, arc;
        if (f->path_index >= 0 && lf_tables(0, &fsinfo, &arc)) {    /* the next load's buffer must hold the new size */
            uint64_t d = lf_r64(arc + 0xa0) + 16ull * f->data_index;
            if (size > lf_r32(d + 8)) lf_w32(d + 8, size);
        }
        uint32_t was = f->size; f->size = size;
        const char *e = strrchr(f->arc_path, '.');
        lf_logf("[hot audio] changed: %s (%u -> %u bytes)", f->arc_path, was, size);
        if (f->path_index < 0) lf_logf("[hot audio] %s: not served (no row in the archive): nothing to reload", f->arc_path);
        else if (e && !strcmp(e, ".nus3audio")) lf_hot_swap(f, size);
        else lf_logf("[hot audio] %s: read by the game only when the bank (or, for the sli, the game) loads it: takes effect at its next load", f->arc_path);
    }
    lf_logf("[hot audio] %.*s: %d sound file(s) changed", (int)rn, root, changed);
}
// the generation of a .nus3_changes.json, -1 = no such file / unreadable; *mtime = its write time when it changed
static long lf_hot_generation(const char *mf, long long *mtime) {
    char js[1024]; uint32_t sz; long long mt;
    if (!lf_stat(mf, &sz, &mt)) return -1;
    *mtime = mt;
    FILE *fp = fopen(mf, "rb");
    if (!fp) return -1;
    size_t n = fread(js, 1, sizeof js - 1, fp); fclose(fp); js[n] = 0;
    const char *g = strstr(js, "\"generation\"");
    return g ? strtol(g + 13 + strspn(g + 13, " \t:"), NULL, 10) : -1;
}
static DWORD WINAPI lf_hot_thread(LPVOID arg) {
    (void)arg;
    enum { NR = 64 };
    static struct { char path[MAX_PATH * 2]; size_t n; long long mtime; long gen; } r[NR];
    int nr = 0; unsigned tick = 0;
    for (int k = 0; k < g_lf_count && nr < NR; k++) {      /* the mod folders: disk path minus "\" + archive path */
        size_t dl = strlen(g_lf[k].disk_path), al = strlen(g_lf[k].arc_path);
        if (strncmp(g_lf[k].arc_path, "sound/", 6) || dl <= al + 1) continue;
        size_t n = dl - al - 1; int seen = 0;
        for (int i = 0; i < nr; i++) if (r[i].n == n && !strncmp(r[i].path, g_lf[k].disk_path, n)) seen = 1;
        if (seen || n + 24 >= sizeof r[0].path) continue;
        memcpy(r[nr].path, g_lf[k].disk_path, n); r[nr].path[n] = 0; r[nr].n = n; r[nr].mtime = 0;
        /* the manifest as it is now is the base (the scan's sizes and times go with it); none yet = generation 0 */
        char mf[MAX_PATH * 2 + 32]; snprintf(mf, sizeof mf, "%s\\.nus3_changes.json", r[nr].path);
        r[nr].gen = lf_hot_generation(mf, &r[nr].mtime);
        if (r[nr].gen < 0) r[nr].gen = 0;
        nr++;
    }
    lf_logf("[hot audio] watching %d folder(s) for .nus3_changes.json (SSBU_AUDIO_HOTRELOAD=1)", nr);
    if (!nr) return 0;
    for (;;) {
        for (int i = 0; i < nr; i++) {
            char mf[MAX_PATH * 2 + 32]; uint32_t sz; long long mt;
            snprintf(mf, sizeof mf, "%s\\.nus3_changes.json", r[i].path);
            if (!lf_stat(mf, &sz, &mt) || mt == r[i].mtime) continue;
            long gen = lf_hot_generation(mf, &r[i].mtime);
            if (gen < 0 || gen == r[i].gen) continue;
            r[i].gen = gen;
            lf_logf("[hot audio] %s: generation %ld", mf, gen);
            lf_hot_check(r[i].path, r[i].n);
        }
        if (g_lf_hot_live && ++tick % 8 == 0) for (int k = 0; k < g_lf_count; k++) if (g_lf[k].hot_src) lf_hot_census(&g_lf[k]);
        Sleep(250);
    }
    return 0;
}
// with the decompress hook: the LoadBank hook (13.0.4 only: no 13.0.5 address is known) and the watching thread
static void loose_files_hot_start(int (*install)(uint64_t, HookFn, void *, HookOrig *)) {
    const char *e = getenv("SSBU_AUDIO_HOTRELOAD");
    if (!(e && e[0] == '1') || g_lf_hot) return;
    g_lf_hot = 1;
    if (g_game_build == GAME_13_0_4) {
        int ok = install(GA(0x710395fe70ull), lf_loadbank, NULL, &g_lf_orig_loadbank);
        lf_logf("[hot audio] hook on the sound engine's LoadBank 0x710395fe70: %s", ok ? "installed" : "FAILED (a bank built again from its old buffer gets the old audio)");
    } else lf_logf("[hot audio] not 13.0.4: no LoadBank hook (a bank built again from its old buffer gets the old audio)");
    HANDLE h = CreateThread(NULL, 0, lf_hot_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static DWORD WINAPI loose_host_wait(LPVOID arg) {
    (void)arg;
    /* the hook table answers "not the entry of a lifted function" until the game's function table is set up */
    if (!loose_files_scan(loose_host_log)) return 0;    /* no folder / no files: nothing to wait for */
    /* N7: the same thread polls every millisecond for the game's tables and grows them as soon as they are published
     * (loose_files_early), before the first file request when it wins; SSBU_LOOSE_EARLY=0: only the first decompress */
    const char *ee = getenv("SSBU_LOOSE_EARLY");
    int hooked = 0, early = !(ee && ee[0] == '0');
    /* the hook addresses go through GA(), which needs to know the build: wait until main.elf has been read (hostrt.c
     * load_layout). Before this wait the thread asked GA() first and the runtime stopped ("asked for before main.elf
     * was read") whenever a characters folder was given. */
    for (int i = 0; g_game_build < 0 && i < 120000; i++) Sleep(1);
    if (g_game_build < 0) { loose_host_log("[loose] main.elf was not read in 120 s: the layer is OFF"); return 0; }
    for (int i = 0; i < 120000; i++) {
        if (!hooked && i % 10 == 0 && (hooked = loose_files_install(hook_install, 1))) loose_files_hot_start(hook_install);
        if (early && loose_files_early()) early = 0;
        if (hooked && !early) return 0;
        if (!hooked && i == 3000) loose_host_log("[loose] the hook could not be installed in 30 s: the layer is OFF");
        Sleep(1);
    }
    return 0;
}

extern int (*g_nm_stage2_has)(uint64_t path_hash);      /* fighter_sdk\newmodule_host.c: the registry's path fallback asks stage 2 */

__attribute__((constructor)) static void loose_host_start(void) {
    const char *e = getenv("SSBU_LOOSE_FILES"), *ch = getenv("SSBU_CHARACTERS");
    /* N6: the fighter registry (SSBU_CHARACTERS) switches the layer on too, unless SSBU_LOOSE_FILES=0 */
    if (e && e[0] == '0') return;
    if (!(e && e[0]) && !(ch && ch[0])) return;
    g_nm_stage2_has = loose_files_has;
    HANDLE h = CreateThread(NULL, 0, loose_host_wait, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
