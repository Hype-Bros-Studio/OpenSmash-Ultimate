/* Host-only mods (docs/HOST_MODS.md): with SSBU_HOST_MODS=<invite code>, the game's mods are the
 * room host's, sealed with that code and fetched from the lobby (tools/hostmods.py serve,
 * tools/hostmods_depot.py). The pack is opened in memory and stays there: the RomFS reads the
 * overlay out of it (hle_fs.c), the online mod set is its mods' names and hashes (net_relay.c),
 * and nothing of it is written to disk by the runtime. (Not an HLE of anything: named hle_ so
 * every build's runtime file pattern takes it.)
 *
 * A pack of version 2 also has the host's Skyline plugin list and its mods' plugins, translated
 * (tools/skyline.py) for each system the host built them for: this one's are loaded from memory
 * (host.c load_plugin_dlls, hle_memdll.c) and never written to a file.
 *
 * A deterrent, not protection: the code, the key and the plain mods (plugins too) are in this
 * process's memory while it runs. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HmSock;
#define HM_NO_SOCK INVALID_SOCKET
#define hm_close closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int HmSock;
#define HM_NO_SOCK (-1)
#define hm_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"
#include "hostmods_crypto.h"
#include "net.h"

static struct {
    u8 *mem; u64 cap;          /* the pack: sealed as it came, then plain in place */
    u32 nmods;
    char (*names)[256];
    u32 *hashes;
    const u8 *vanilla_sha;     /* (32 bytes) the game's own layer map's sha256 */
    u32 nsources;
    const u8 *sizes;           /* nsources u64s */
    const u8 *map; u64 map_len;
    const u8 *overlay; u64 overlay_len;
    const char *plugin_list;   /* (version 2) the host's plugins.txt, NUL-terminated copy; NULL for version 1 */
    u32 nplugins;              /* this system's */
    struct HmPlugin { char name[128]; const u8 *lib, *image; u64 lib_len, image_len; } *plugins;
    u32 other_os_plugins;      /* the host's plugins for other systems only */
} hm;

/* The system a pack's plugin was built for (tools/hostmods.py PLUGIN_OS) */
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define HM_OS 1
#elif defined(__linux__) && defined(__x86_64__)
#define HM_OS 2
#elif defined(__APPLE__) && defined(__aarch64__)
#define HM_OS 3
#else
#define HM_OS 0
#endif
static INIT_ONCE hm_once = INIT_ONCE_STATIC_INIT;

int hostmods_on(void) {
    const char *v = getenv("SSBU_HOST_MODS");
    return v && *v;
}

/* Memory that isn't put in a core dump (Linux); the pack is never paged to a file of ours, but
 * the OS may page it out like any memory. */
static void *pack_alloc(u64 n) {
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#ifdef MADV_DONTDUMP
    if (p) madvise(p, (size_t)n, MADV_DONTDUMP);
#endif
#endif
    if (p) hm.cap = n;
    return p;
}

/* ---- the sealed pack from the lobby ------------------------------------------------------
 * The lobby's pack service, on its port (SSBU_NET_RELAY's host and port; tools/hostmods_depot.py):
 * "SSBUPAK1" "GET " u32 id length (big-endian) id -> u64 size (big-endian), the pack. */
#define CONNECT_MS 5000
#define RECV_MS 30000

static int recv_all(HmSock s, u8 *p, u64 n) {
    while (n) {
        int r = (int)recv(s, (char *)p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (r <= 0) return 0;
        p += r; n -= (u64)r;
    }
    return 1;
}

static HmSock lobby_connect(char *why, size_t why_len) {
    const char *v = net_relay_address();
    char host[256];
    if (!v || !*v) { snprintf(why, why_len, "no lobby to ask (SSBU_NET_RELAY or online_server.txt)"); return HM_NO_SOCK; }
    snprintf(host, sizeof host, "%s", v);
    u16 port = NET_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = (u16)atoi(colon + 1); *colon = 0; }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   /* (the game's start: before its own sockets) */
#endif
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) { snprintf(why, why_len, "the lobby's address (%s) isn't found", v); return HM_NO_SOCK; }
    struct sockaddr_in at = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    at.sin_port = htons(port);
    HmSock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int ok = s != HM_NO_SOCK;
#ifdef _WIN32   /* (connected within CONNECT_MS: a lobby that does not answer fails, not hangs, the game's start) */
    u_long nb = 1;
    ok = ok && ioctlsocket(s, FIONBIO, &nb) == 0;
    if (ok && connect(s, (const struct sockaddr *)&at, sizeof at) != 0) {
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = {CONNECT_MS / 1000, CONNECT_MS % 1000 * 1000};
        ok = WSAGetLastError() == WSAEWOULDBLOCK && select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
    }
    nb = 0;
    ok = ok && ioctlsocket(s, FIONBIO, &nb) == 0;
    DWORD timeout = RECV_MS;
#else
    int flags = ok ? fcntl(s, F_GETFL, 0) : -1;
    ok = ok && flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    if (ok && connect(s, (const struct sockaddr *)&at, sizeof at) != 0) {
        struct pollfd p = {s, POLLOUT, 0};
        int err = 0;
        socklen_t n = sizeof err;
        ok = errno == EINPROGRESS && poll(&p, 1, CONNECT_MS) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && !err;
    }
    ok = ok && fcntl(s, F_SETFL, flags) == 0;
    struct timeval timeout = {RECV_MS / 1000, 0};
#endif
    if (!ok) {
        if (s != HM_NO_SOCK) hm_close(s);
        snprintf(why, why_len, "the lobby (%s) doesn't answer", v);
        return HM_NO_SOCK;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    return s;
}

/* The pack by its id, into memory from alloc; NULL, and why, when there is none. */
static u8 *fetch_pack(const char *id, u64 *len, void *(*alloc)(u64), char *why, size_t why_len) {
    HmSock s = lobby_connect(why, why_len);
    if (s == HM_NO_SOCK) return NULL;
    u8 head[16 + 64], size[8];
    u32 n = (u32)strlen(id);
    if (n > 64) n = 64;
    memcpy(head, "SSBUPAK1GET ", 12);
    head[12] = (u8)(n >> 24); head[13] = (u8)(n >> 16); head[14] = (u8)(n >> 8); head[15] = (u8)n;
    memcpy(head + 16, id, n);
    u8 *buf = NULL;
    u64 want = 0;
    int ok = send(s, (const char *)head, (int)(16 + n), 0) == (int)(16 + n) && recv_all(s, size, 8);
    if (ok) for (int k = 0; k < 8; k++) want = want << 8 | size[k];
    if (!ok) snprintf(why, why_len, "the lobby (%s) has no pack service (an older lobby?)", net_relay_address());
    else if (!want) { ok = 0; snprintf(why, why_len, "no host has a pack up for that invite code (is the host's tools/hostmods.py serve running?)"); }
    else if (want > (8ull << 30) || !(buf = alloc(want))) { ok = 0; snprintf(why, why_len, "a pack of %llu bytes: no memory for it", (unsigned long long)want); }
    else if (!recv_all(s, buf, want)) { ok = 0; snprintf(why, why_len, "the lobby stopped sending the pack"); }
    hm_close(s);
    if (!ok) return NULL;   /* (buf: the pack's memory, left - the game is stopping) */
    *len = want;
    return buf;
}

static u32 rd32(const u8 *p) { return hm_le32(p); }
static u64 rd64(const u8 *p) { return (u64)hm_le32(p) | (u64)hm_le32(p + 4) << 32; }

static BOOL CALLBACK hm_load(PINIT_ONCE o, PVOID a, PVOID *ctx) {
    (void)o; (void)a; (void)ctx;
    /* the code as tools/hostmods.py normalize takes it: upper case, without '-' and blanks */
    u8 code[128];
    size_t cl = 0;
    for (const char *s = getenv("SSBU_HOST_MODS"); s && *s && cl < sizeof code; s++)
        if (*s != '-' && *s != ' ' && *s != '\t') code[cl++] = (u8)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    static const char idk[] = "HypeSmash host mods id";
    HmSha256 s;
    u8 d[32];
    hm_sha256_init(&s);
    hm_sha256_update(&s, idk, sizeof idk);
    hm_sha256_update(&s, code, cl);
    hm_sha256_final(&s, d);
    char id[21], why[256] = "";
    for (int i = 0; i < 10; i++) snprintf(id + 2 * i, 3, "%02x", d[i]);
    u64 len = 0;
    hm.mem = fetch_pack(id, &len, pack_alloc, why, sizeof why);
    if (!hm.mem) rt_fatal("host mods (SSBU_HOST_MODS): cannot get the host's mods from the lobby: %s", why);
    i64 plain = hm_unseal(code, cl, hm.mem, len);
    memset(code, 0, sizeof code);
    if (plain < 0) rt_fatal("host mods (SSBU_HOST_MODS): the host's pack doesn't open with that invite code");

    const u8 *p = hm.mem, *end = hm.mem + plain;
#define NEED(n) do { if ((u64)(end - p) < (u64)(n)) rt_fatal("host mods: the host's pack is cut short"); } while (0)
    NEED(16);
    u32 version = rd32(p + 8);
    if (memcmp(p, "SSBUHMP1", 8) || (version != 1 && version != 2)) rt_fatal("host mods: the host's pack is of another version (update the game)");
    hm.nmods = rd32(p + 12);
    p += 16;
    if (hm.nmods > 4096) rt_fatal("host mods: %u mods in the host's pack", hm.nmods);
    hm.names = calloc(hm.nmods ? hm.nmods : 1, sizeof *hm.names);
    hm.hashes = calloc(hm.nmods ? hm.nmods : 1, sizeof *hm.hashes);
    for (u32 i = 0; i < hm.nmods; i++) {
        NEED(6);
        u32 l = (u32)p[4] | (u32)p[5] << 8;
        hm.hashes[i] = rd32(p);
        NEED(6 + l);
        memcpy(hm.names[i], p + 6, l < 255 ? l : 255);
        p += 6 + l;
    }
    NEED(36);
    hm.vanilla_sha = p;
    hm.nsources = rd32(p + 32);
    p += 36;
    NEED((u64)hm.nsources * 8 + 4);
    hm.sizes = p;
    p += (u64)hm.nsources * 8;
    hm.map_len = rd32(p);
    NEED(4 + hm.map_len + 8);
    hm.map = p + 4;
    p += 4 + hm.map_len;
    hm.overlay_len = rd64(p);
    p += 8;
    NEED(hm.overlay_len);
    hm.overlay = p;
    p += hm.overlay_len;
    if (version >= 2) {
        /* u32 length and the host's plugins.txt; u32 plugin count, then each plugin's u16 length and
         * module name, u8 system, u64 length and library, u64 length and image (<module>.reloc.bin) */
        NEED(4);
        u32 ll = rd32(p);
        NEED(4 + (u64)ll + 4);
        char *list = malloc((size_t)ll + 1);
        if (!list) rt_fatal("host mods: no memory");
        memcpy(list, p + 4, ll);
        list[ll] = 0;
        hm.plugin_list = list;
        p += 4 + ll;
        u32 n = rd32(p);
        p += 4;
        if (n > 64) rt_fatal("host mods: %u plugins in the host's pack", n);
        hm.plugins = calloc(n ? n : 1, sizeof *hm.plugins);
        for (u32 i = 0; i < n; i++) {
            NEED(2);
            u32 l = (u32)p[0] | (u32)p[1] << 8;
            NEED(2 + (u64)l + 1 + 8);
            struct HmPlugin pl = {0};
            memcpy(pl.name, p + 2, l < sizeof pl.name - 1 ? l : sizeof pl.name - 1);
            u8 os = p[2 + l];
            p += 3 + l;
            pl.lib_len = rd64(p);
            NEED(8 + pl.lib_len + 8);
            pl.lib = p + 8;
            p += 8 + pl.lib_len;
            pl.image_len = rd64(p);
            NEED(8 + pl.image_len);
            pl.image = p + 8;
            p += 8 + pl.image_len;
            if (os == HM_OS) hm.plugins[hm.nplugins++] = pl;
            else hm.other_os_plugins++;
        }
    }
#undef NEED
    fprintf(stderr, "[hostmods] the host's %u mod(s) and %u plugin(s), %.1f MB, in memory only (pack %s)\n", hm.nmods,
            hm.nplugins, (double)plain / 1048576.0, id);
    if (!hm.nplugins && hm.other_os_plugins)
        rt_fatal("host mods: the host's mods have plugins built for other systems than this one only "
                 "(the host has to build them for this one too: docs/HOST_MODS.md)");
    return TRUE;
}
static void load(void) { InitOnceExecuteOnce(&hm_once, hm_load, NULL, NULL); }

static i64 path_size(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) ? -1 : (i64)st.st_size;
#else
    struct stat st;
    return stat(path, &st) ? -1 : (i64)st.st_size;
#endif
}

/* The RomFS from the pack (hle_fs.c rom_open): this game's own layer map and sources (romfs_vanilla.cfg's,
 * or romfs.cfg's for a game without mods) must be the ones the host's mods were applied over. */
void hostmods_romfs(const char *vanilla_map, const char (*sources)[1024], u32 n, const u8 **map, u64 *map_len,
                    const u8 **overlay, u64 *overlay_len) {
    load();
    FILE *f = fopen(vanilla_map, "rb");
    if (!f) rt_fatal("host mods: cannot read %s", vanilla_map);
    HmSha256 s;
    u8 buf[65536], d[32];
    size_t got;
    hm_sha256_init(&s);
    while ((got = fread(buf, 1, sizeof buf, f))) hm_sha256_update(&s, buf, got);
    fclose(f);
    hm_sha256_final(&s, d);
    if (memcmp(d, hm.vanilla_sha, 32) || n != hm.nsources)
        rt_fatal("host mods: the host's game isn't this one (another version or update): its mods can't be read over this game's data");
    for (u32 i = 0; i < n; i++)
        if (path_size(sources[i]) != (i64)rd64(hm.sizes + 8 * i))
            rt_fatal("host mods: %s isn't the host's (its size): the host's mods can't be read over it", sources[i]);
    *map = hm.map; *map_len = hm.map_len;
    *overlay = hm.overlay; *overlay_len = hm.overlay_len;
}

/* The pack's mods, for the online mod set (net_relay.c mod_set_make): names and the hashes the host's
 * own game took of them. */
int hostmods_mods(char (*names)[256], u32 *hashes, int max) {
    load();
    int n = (int)hm.nmods < max ? (int)hm.nmods : max;
    for (int i = 0; i < n; i++) {
        memcpy(names[i], hm.names[i], 256);
        hashes[i] = hm.hashes[i];
    }
    return n;
}

/* The host's plugins (version 2 packs): this system's, by index; NULL past the last. */
const char *hostmods_plugin(u32 i, const u8 **lib, u64 *lib_len, const u8 **image, u64 *image_len) {
    load();
    if (i >= hm.nplugins) return NULL;
    if (lib) { *lib = hm.plugins[i].lib; *lib_len = hm.plugins[i].lib_len; }
    if (image) { *image = hm.plugins[i].image; *image_len = hm.plugins[i].image_len; }
    return hm.plugins[i].name;
}

/* The host's Skyline plugin list (its plugins.txt), or NULL from a version 1 pack. */
const char *hostmods_plugin_list(void) {
    load();
    return hm.plugin_list;
}
