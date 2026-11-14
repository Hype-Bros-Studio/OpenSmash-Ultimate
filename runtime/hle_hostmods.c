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
 * A pack of version 3 is an open one (tools/hostmods.py serve --open): it comes unsealed (it starts
 * "SSBUHMP1", not "SSBUHME1") and also has each turned-on mod's folder. The game still plays from the
 * pack in memory; when the player has ticked F1 > Online > Download host's mods (online_host_mods_download.txt
 * "on", or SSBU_HOST_MODS_DOWNLOAD=1) the folders are also saved into this tree's mods/, turned off
 * (tools/mods.py takes a new folder as off until it's enabled). The pack is untrusted: a name or path
 * that could leave mods/<name> is refused (hm_safe_part).
 *
 * A pack of version 4 is an ARCropolis one (tools/hostmods.py pack_arcropolis): no layer map or overlay -
 * this game's own data is read as it is (its sizes and map checked against the host's) - and its folders
 * are the SD card's ultimate/mods (hle_fs.c: hostmods_sd_entry/list), which ARCropolis, run from the
 * host's plugin list, reads and lays over the game's data itself; the mods' plugin.nro files it chainloads
 * run as their Windows builds from the pack (host.c load_host_plugins, matched by build id) or as this
 * game's own built-in copies.
 *
 * The sealed pack as it came is kept in hostmods_cache/<id>.bin beside mods/ (still sealed: the code
 * opens it), with <id>.sum the depot's size and sha256 of it; the next start asks the lobby for those
 * (SUM) and reads the file instead of fetching a pack it has. SSBU_HOST_MODS_CACHE=0: no cache.
 *
 * A deterrent, not protection: the code, the key and the plain mods (plugins too) are in this
 * process's memory while it runs. An open pack is no deterrent at all. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HmSock;
#define HM_NO_SOCK INVALID_SOCKET
#define hm_close closesocket
#else
#include <dirent.h>
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

#include "config_folder.h"
#include "host.h"
#include "hostmods_crypto.h"
#include "net.h"
#include "settings.h"

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
    int open;                  /* an open pack (not sealed) */
    const u8 *folders; u32 nfolders;   /* (version 3) the mods' folders, as in the pack */
    struct HmModsFile { const char *path; u32 path_len; u64 size; const u8 *data; u32 data_len; } *mods_files;
    u32 nmods_files; int mods_index;   /* (the pack's last section, when there) mods:/ as the host's game sees it */
    int arcropolis;                    /* (version 4) the folders are the SD card's ultimate/mods */
    struct HmModsFile *sd; u32 nsd;    /* their files as "<folder>/<path>", sorted without case (hm_build_sd) */
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
static void hm_build_sd(void);

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
static void pack_free(void *p, u64 n) {
    if (!p) return;
#ifdef _WIN32
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, (size_t)n);
#endif
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

/* ---- the pack's download, shown -----------------------------------------------------------
 * The pack is fetched at the RomFS's opening, before the game has a window: a big pack took minutes
 * with nothing on screen and nothing in the log (a player saw only a background process whose memory
 * grew). So the fetch says how far it is in the log, and on Windows in a small window of its own
 * (closing it stops the game). */
static volatile u64 dl_got, dl_want;
static volatile int dl_done;

static void dl_text(char *t, size_t n) {
    double got = (double)dl_got / (1 << 20), want = (double)dl_want / (1 << 20);
    snprintf(t, n, "Getting the host's mods: %.0f of %.0f MB (%.0f%%)", got, want, want > 0 ? got * 100 / want : 0.0);
}

#ifdef _WIN32
static LRESULT CALLBACK dl_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER) {
        if (dl_done) { DestroyWindow(h); return 0; }
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        char t[200];
        dl_text(t, sizeof t);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT tr = {16, 16, r.right - 16, 40};
        DrawTextA(dc, t, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT bar = {16, 52, r.right - 16, 72};
        FrameRect(dc, &bar, (HBRUSH)GetStockObject(GRAY_BRUSH));
        if (dl_want) {
            RECT fill = bar;
            InflateRect(&fill, -2, -2);
            fill.right = fill.left + (LONG)((double)(fill.right - fill.left) * (double)dl_got / (double)dl_want);
            HBRUSH b = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(dc, &fill, b);
            DeleteObject(b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (dl_done) { DestroyWindow(h); return 0; }
        fprintf(stderr, "[hostmods] the download window was closed: the game stops\n");
        fflush(stderr);
        ExitProcess(1);
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, msg, w, l);
}

static DWORD WINAPI dl_window(LPVOID unused) {
    (void)unused;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = dl_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = "ssbu_hostmods_download";
    RegisterClassA(&wc);
    RECT r = {0, 0, 460, 90};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    HWND h = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, "Super Smash Bros. Ultimate (native) - host's mods", style,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt,
                             NULL, NULL, wc.hInstance, NULL);
    if (!h) return 0;
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
    SetTimer(h, 1, 250, NULL);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
#endif

/* recv_all for the pack itself: its progress in the log every 10% (and in the window) */
static int recv_pack(HmSock s, u8 *p, u64 n) {
    dl_got = 0; dl_want = n; dl_done = 0;
    fprintf(stderr, "[hostmods] getting the host's pack from the lobby: %.0f MB\n", (double)n / (1 << 20));
    fflush(stderr);
#ifdef _WIN32
    const char *headless = getenv("SSBU_HEADLESS");
    HANDLE t = headless && !strcmp(headless, "1") ? NULL : CreateThread(NULL, 0, dl_window, NULL, 0, NULL);
    if (t) CloseHandle(t);
#endif
    int tenth = 0, ok = 1;
    while (n) {
        int r = (int)recv(s, (char *)p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (r <= 0) { ok = 0; break; }
        p += r; n -= (u64)r;
        dl_got += (u64)r;
        int now = (int)(dl_got * 10 / dl_want);
        if (now > tenth) {
            tenth = now;
            char line[200];
            dl_text(line, sizeof line);
            fprintf(stderr, "[hostmods] %s\n", line);
            fflush(stderr);
        }
    }
    dl_done = 1;
    return ok;
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
    else if (!recv_pack(s, buf, want)) { ok = 0; snprintf(why, why_len, "the lobby stopped sending the pack"); }
    hm_close(s);
    if (!ok) return NULL;   /* (buf: the pack's memory, left - the game is stopping) */
    *len = want;
    return buf;
}

/* The depot's size and sha256 of the pack by its id (SUM): 1, or 0 when it has none or can't say. */
static int pack_sum(const char *id, u64 *len, u8 sha[32]) {
    char why[256];
    HmSock s = lobby_connect(why, sizeof why);
    if (s == HM_NO_SOCK) return 0;
    u8 head[16 + 64], size[8];
    u32 n = (u32)strlen(id);
    if (n > 64) n = 64;
    memcpy(head, "SSBUPAK1SUM ", 12);
    head[12] = (u8)(n >> 24); head[13] = (u8)(n >> 16); head[14] = (u8)(n >> 8); head[15] = (u8)n;
    memcpy(head + 16, id, n);
    u64 want = 0;
    int ok = send(s, (const char *)head, (int)(16 + n), 0) == (int)(16 + n) && recv_all(s, size, 8);
    if (ok) for (int k = 0; k < 8; k++) want = want << 8 | size[k];
    ok = ok && want && recv_all(s, sha, 32);
    hm_close(s);
    if (ok) *len = want;
    return ok;
}

static u32 rd32(const u8 *p) { return hm_le32(p); }
static u64 rd64(const u8 *p) { return (u64)hm_le32(p) | (u64)hm_le32(p + 4) << 32; }

static i64 path_size(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) ? -1 : (i64)st.st_size;
#else
    struct stat st;
    return stat(path, &st) ? -1 : (i64)st.st_size;
#endif
}

/* ---- an open pack's mod folders, saved into mods/ (the player's choice) ------------------- */
/* Whether the player wants an open pack's mods kept: SSBU_HOST_MODS_DOWNLOAD (1/on, 0/off), else the
 * settings menu's Online > Download host's mods (online_host_mods_download.txt, read here: the pack is
 * fetched at the RomFS's opening, before the menu is). Off unless asked. */
static int hm_download_wanted(void) {
    const char *v = getenv("SSBU_HOST_MODS_DOWNLOAD");
    if (v && *v) return !(v[0] == '0' || !_strnicmp(v, "off", 3) || !_strnicmp(v, "no", 2));
    const char *path = config_path(CONFIG_ONLINE_HOST_MODS_DOWNLOAD);
    char line[16] = "";
    return path && settings_line_read(path, line, sizeof line) && !_strnicmp(line, "on", 2);
}

/* One folder or file name that stays where it is put (tools/hostmods.py _safe_part): not empty, '.' or
 * '..'; no '/', '\\', ':' (drive letters, streams) or control character; not ending in '.' or ' ' (Windows
 * drops them); not a Windows device name (CON, NUL, COM1, ...). */
static int hm_safe_part(const char *s, size_t n) {
    if (!n || n > 255 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '/' || s[i] == '\\' || s[i] == ':' || (u8)s[i] < 32 || s[i] == 127) return 0;
    if (s[n - 1] == '.' || s[n - 1] == ' ') return 0;
    char stem[8];
    size_t k = 0;
    while (k < n && s[k] != '.') k++;
    while (k && s[k - 1] == ' ') k--;
    if (k >= sizeof stem) return 1;
    for (size_t i = 0; i < k; i++) stem[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    stem[k] = 0;
    static const char *const RESERVED[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++)
        if (!strcmp(stem, RESERVED[i])) return 0;
    if (k == 4 && (!memcmp(stem, "COM", 3) || !memcmp(stem, "LPT", 3)) && stem[3] >= '0' && stem[3] <= '9') return 0;
    return 1;
}

/* A file's path in a mod's folder: relative, '/' between safe parts (so no '..', no absolute path). */
static int hm_safe_path(const char *s, size_t n) {
    if (!n || n > 1024) return 0;
    for (size_t a = 0; a <= n;) {
        size_t b = a;
        while (b < n && s[b] != '/') b++;
        if (!hm_safe_part(s + a, b - a)) return 0;
        a = b + 1;
    }
    return 1;
}

/* A folder of the pack (its place parsed and bounds-checked by hm_load). */
typedef struct { const char *name; u32 name_len, nfiles; const u8 *files, *next; } HmFolder;
static const u8 *hm_folder_at(const u8 *p, HmFolder *f) {
    f->name_len = (u32)p[0] | (u32)p[1] << 8;
    f->name = (const char *)p + 2;
    f->nfiles = rd32(p + 2 + f->name_len);
    p += 6 + f->name_len;
    f->files = p;
    for (u32 i = 0; i < f->nfiles; i++) {
        u32 l = (u32)p[0] | (u32)p[1] << 8;
        p += 10 + l + rd64(p + 2 + l);
    }
    return f->next = p;
}
/* Its files one by one: the path (not NUL-terminated), its bytes; the next file's place. */
static const u8 *hm_file_at(const u8 *p, const char **path, u32 *path_len, const u8 **data, u64 *len) {
    *path_len = (u32)p[0] | (u32)p[1] << 8;
    *path = (const char *)p + 2;
    *len = rd64(p + 2 + *path_len);
    *data = p + 10 + *path_len;
    return *data + *len;
}

static int hm_is_there(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

static int hm_make_dir(const char *path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

/* Calls fn(path, is a folder, ctx) for each entry of a folder (not '.', '..'), links not followed. */
static void hm_each_entry(const char *dir, void (*fn)(const char *, int, void *), void *ctx) {
    char child[1400];
#ifdef _WIN32
    char pattern[1400];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir, fd.cFileName) >= sizeof child) continue;
        fn(child, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir, e->d_name) >= sizeof child) continue;
        struct stat st;
        fn(child, lstat(child, &st) == 0 && S_ISDIR(st.st_mode), ctx);
    }
    closedir(d);
#endif
}

static void hm_remove_entry(const char *path, int is_dir, void *ctx) {
    (void)ctx;
    if (is_dir) {
        hm_each_entry(path, hm_remove_entry, NULL);
#ifdef _WIN32
        RemoveDirectoryA(path);
#else
        rmdir(path);
#endif
    } else {
#ifdef _WIN32
        DeleteFileA(path);
#else
        unlink(path);
#endif
    }
}
/* (only the folders this file made: a half-written one) */
static void hm_remove_tree(const char *path) {
    if (hm_is_there(path)) hm_remove_entry(path, 1, NULL);
}

static void hm_count_entry(const char *path, int is_dir, void *ctx) {
    if (is_dir) hm_each_entry(path, hm_count_entry, ctx);
    else ++*(u32 *)ctx;
}

/* Whether the folder at dir holds the pack folder's files, byte for byte, and nothing else. */
static int hm_same_folder(const char *dir, const HmFolder *f) {
    u32 files = 0;
    hm_each_entry(dir, hm_count_entry, &files);
    if (files != f->nfiles) return 0;
    const u8 *p = f->files;
    static u8 buf[1 << 16];
    for (u32 i = 0; i < f->nfiles; i++) {
        const char *rel;
        u32 rl;
        const u8 *data;
        u64 len;
        p = hm_file_at(p, &rel, &rl, &data, &len);
        char path[1400];
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)rl, rel);
        if (path_size(path) != (i64)len) return 0;
        FILE *fp = fopen(path, "rb");
        if (!fp) return 0;
        u64 at = 0;
        size_t got;
        while (at < len && (got = fread(buf, 1, sizeof buf, fp)) > 0 && got <= len - at && !memcmp(buf, data + at, got)) at += got;
        fclose(fp);
        if (at != len) return 0;
    }
    return 1;
}

/* The pack folder's files into dir (made new). */
static int hm_write_folder(const char *dir, const HmFolder *f) {
    if (!hm_make_dir(dir)) return 0;
    const u8 *p = f->files;
    for (u32 i = 0; i < f->nfiles; i++) {
        const char *rel;
        u32 rl;
        const u8 *data;
        u64 len;
        p = hm_file_at(p, &rel, &rl, &data, &len);
        char path[1400];
        int base = snprintf(path, sizeof path, "%s/", dir);
        if (base < 0 || (size_t)base + rl >= sizeof path) return 0;
        memcpy(path + base, rel, rl);
        path[base + rl] = 0;
        for (char *c = path + base; *c; c++)   /* (its folders first) */
            if (*c == '/') {
                *c = 0;
                int ok = hm_make_dir(path);
                *c = '/';
                if (!ok) return 0;
            }
        FILE *fp = fopen(path, "wb");
        if (!fp) return 0;
        int ok = !len || fwrite(data, 1, (size_t)len, fp) == (size_t)len;
        if (fclose(fp) != 0) ok = 0;
        if (!ok) return 0;
    }
    return 1;
}

/* Each of the pack's mod folders into <tree>/mods/<name>: written next to it first, then renamed into place.
 * One that is there with the same files is left; another folder of that name: "<name> (host)". */
static void hm_save_folders(void) {
    char mods[1100];
    snprintf(mods, sizeof mods, "%s/../../mods", rt_data_dir ? rt_data_dir : ".");
    if (!hm_make_dir(mods)) { fprintf(stderr, "[hostmods] cannot make %s: the host's mods aren't saved\n", mods); return; }
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    const u8 *p = hm.folders;
    u32 saved = 0, had = 0;
    for (u32 i = 0; i < hm.nfolders; i++) {
        HmFolder f;
        p = hm_folder_at(p, &f);
        int safe = hm_safe_part(f.name, f.name_len) && f.name[0] != '.';
        const u8 *q = f.files;
        for (u32 k = 0; safe && k < f.nfiles; k++) {
            const char *rel;
            u32 rl;
            const u8 *data;
            u64 len;
            q = hm_file_at(q, &rel, &rl, &data, &len);
            if (!hm_safe_path(rel, rl)) {
                fprintf(stderr, "[hostmods] not saved: the host's %.*s has a file at \"%.*s\", which would leave its folder\n",
                        (int)f.name_len, f.name, (int)(rl > 200 ? 200 : rl), rel);
                safe = -1;
            }
        }
        if (!safe) fprintf(stderr, "[hostmods] not saved: a mod folder named \"%.*s\" (not a folder name of mods/)\n",
                           (int)(f.name_len > 200 ? 200 : f.name_len), f.name);
        if (safe != 1) continue;
        char name[300], dest[1400];
        int found = 0;
        for (int k = 0; k < 10 && !found; k++) {
            if (!k) snprintf(name, sizeof name, "%.*s", (int)f.name_len, f.name);
            else if (k == 1) snprintf(name, sizeof name, "%.*s (host)", (int)f.name_len, f.name);
            else snprintf(name, sizeof name, "%.*s (host %d)", (int)f.name_len, f.name, k);
            if (!hm_safe_part(name, strlen(name))) break;
            snprintf(dest, sizeof dest, "%s/%s", mods, name);
            if (!hm_is_there(dest)) found = 1;
            else if (hm_same_folder(dest, &f)) found = 2;
        }
        if (found == 2) { had++; fprintf(stderr, "[hostmods] mods/%s is the host's already\n", name); continue; }
        if (!found) { fprintf(stderr, "[hostmods] not saved: the host's %.*s (mods/ has other folders of its names)\n", (int)f.name_len, f.name); continue; }
        char tmp[1400];
        snprintf(tmp, sizeof tmp, "%s/.hostmods-%lu-%u.tmp", mods, pid, i);   /* (a leading dot: tools/mods.py passes it over) */
        hm_remove_tree(tmp);
        int ok = hm_write_folder(tmp, &f);
#ifdef _WIN32
        ok = ok && MoveFileExA(tmp, dest, 0);
#else
        ok = ok && !hm_is_there(dest) && rename(tmp, dest) == 0;
#endif
        if (!ok) {
            hm_remove_tree(tmp);
            fprintf(stderr, "[hostmods] cannot save the host's %.*s into mods/%s\n", (int)f.name_len, f.name, name);
            continue;
        }
        saved++;
        fprintf(stderr, "[hostmods] saved the host's mod into mods/%s (%u file(s); off until you enable it)\n", name, f.nfiles);
    }
    fprintf(stderr, "[hostmods] the host's mods: %u saved into mods/, %u there already\n", saved, had);
}

/* ---- the sealed pack kept between starts (hostmods_cache/<id>.bin, <id>.sum) ------------------- */
static int hm_cache_dir(char *out, size_t n) {
    const char *v = getenv("SSBU_HOST_MODS_CACHE");
    if (v && (*v == '0' || !_strnicmp(v, "off", 3))) return 0;
    snprintf(out, n, "%s/../../hostmods_cache", rt_data_dir ? rt_data_dir : ".");
    return 1;
}
/* The kept pack, when the lobby's (by its SUM) is the same one; NULL otherwise. */
static u8 *cache_read(const char *dir, const char *id, const u8 *sum, u64 want, u64 *len) {
    char path[1300];
    u8 had[40];
    snprintf(path, sizeof path, "%s/%s.sum", dir, id);
    FILE *f = fopen(path, "rb");
    int same = f && fread(had, 1, 40, f) == 40;
    if (f) fclose(f);
    u64 have = 0;
    for (int k = 0; k < 8; k++) have = have << 8 | had[k];
    if (!same || have != want || memcmp(had + 8, sum, 32)) return NULL;
    snprintf(path, sizeof path, "%s/%s.bin", dir, id);
    if (path_size(path) != (i64)want || !(f = fopen(path, "rb"))) return NULL;
    u8 *buf = pack_alloc(want);
    u64 at = 0;
    while (buf && at < want) {
        size_t got = fread(buf + at, 1, (size_t)(want - at > (64u << 20) ? (64u << 20) : want - at), f);
        if (!got) break;
        at += got;
    }
    fclose(f);
    if (at != want) { pack_free(buf, want); return NULL; }
    *len = want;
    return buf;
}
static void cache_drop(const char *dir, const char *id) {
    char path[1300];
    snprintf(path, sizeof path, "%s/%s.sum", dir, id);
    remove(path);
    snprintf(path, sizeof path, "%s/%s.bin", dir, id);
    remove(path);
}
/* Keeps the pack as it came (before it's opened in place), and only it: other codes' packs go. */
static void cache_write(const char *dir, const char *id, const u8 *pack, u64 len, const u8 *sum) {
    if (!hm_make_dir(dir)) return;
    hm_each_entry(dir, hm_remove_entry, NULL);
    char path[1300], tmp[1300];
    snprintf(tmp, sizeof tmp, "%s/%s.bin.tmp", dir, id);
    FILE *f = fopen(tmp, "wb");
    u64 at = 0;
    while (f && at < len) {
        size_t put = fwrite(pack + at, 1, (size_t)(len - at > (64u << 20) ? (64u << 20) : len - at), f);
        if (!put) break;
        at += put;
    }
    if (f && fclose(f)) at = 0;
    snprintf(path, sizeof path, "%s/%s.bin", dir, id);
    remove(path);
    if (!f || at != len || rename(tmp, path)) {
        remove(tmp);
        fprintf(stderr, "[hostmods] cannot keep the host's pack in %s: it is fetched again next time\n", dir);
        return;
    }
    u8 head[40];
    for (int k = 0; k < 8; k++) head[k] = (u8)(len >> (56 - 8 * k));
    memcpy(head + 8, sum, 32);
    snprintf(path, sizeof path, "%s/%s.sum", dir, id);
    if (!(f = fopen(path, "wb")) || fwrite(head, 1, 40, f) != 40) remove(path);
    if (f) fclose(f);
    fprintf(stderr, "[hostmods] the host's sealed pack is kept in %s for the next start\n", dir);
}

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
    u64 len = 0, sum_len = 0;
    u8 sum[32];
    char cache[1100];
    int cached = 0, keep = hm_cache_dir(cache, sizeof cache) && pack_sum(id, &sum_len, sum);
    i64 plain = -1;
    for (int tries = 0; tries < 2; tries++) {   /* (a kept pack that doesn't open: fetched again, once) */
        hm.mem = keep && !tries ? cache_read(cache, id, sum, sum_len, &len) : NULL;
        cached = hm.mem != NULL;
        if (cached) fprintf(stderr, "[hostmods] the host's pack is the one kept in %s: not fetched again\n", cache);
        else {
            hm.mem = fetch_pack(id, &len, pack_alloc, why, sizeof why);
            if (!hm.mem) rt_fatal("host mods (SSBU_HOST_MODS): cannot get the host's mods from the lobby: %s", why);
            u64 again_len = 0;
            u8 again[32];
            if (keep && len == sum_len && pack_sum(id, &again_len, again) && again_len == sum_len && !memcmp(again, sum, 32))
                cache_write(cache, id, hm.mem, len, sum);   /* (the same pack before and after the fetch) */
        }
        /* an open pack (hostmods.py serve --open) is the plain pack itself: nothing to unseal */
        hm.open = len >= 8 && !memcmp(hm.mem, "SSBUHMP1", 8);
        plain = hm.open ? (i64)len : hm_unseal(code, cl, hm.mem, len);
        if (plain >= 0 || !cached) break;
        fprintf(stderr, "[hostmods] the kept pack doesn't open: fetching it again\n");
        cache_drop(cache, id);
        pack_free(hm.mem, len);
    }
    memset(code, 0, sizeof code);
    if (plain < 0) rt_fatal("host mods (SSBU_HOST_MODS): the host's pack doesn't open with that invite code");

    const u8 *p = hm.mem, *end = hm.mem + plain;
#define NEED(n) do { if ((u64)(end - p) < (u64)(n)) rt_fatal("host mods: the host's pack is cut short"); } while (0)
    NEED(16);
    u32 version = rd32(p + 8);
    if (memcmp(p, "SSBUHMP1", 8) || version < 1 || version > 4) rt_fatal("host mods: the host's pack is of another version (update the game)");
    hm.nmods = rd32(p + 12);
    hm.arcropolis = version == 4;
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
    if (version >= 3) {
        /* u32 folder count, then each turned-on mod's folder: u16 length and name, u32 file count, then each
         * file's u16 length and path in the folder, u64 length and bytes (checked whole here: hm_folder_at) */
        NEED(4);
        hm.nfolders = rd32(p);
        p += 4;
        if (hm.nfolders > 4096) rt_fatal("host mods: %u mod folders in the host's pack", hm.nfolders);
        hm.folders = p;
        for (u32 i = 0; i < hm.nfolders; i++) {
            NEED(2);
            u32 l = (u32)p[0] | (u32)p[1] << 8;
            NEED(2 + (u64)l + 4);
            u32 nf = rd32(p + 2 + l);
            p += 6 + l;
            for (u32 k = 0; k < nf; k++) {
                NEED(2);
                u32 pl = (u32)p[0] | (u32)p[1] << 8;
                NEED(2 + (u64)pl + 8);
                u64 dl = rd64(p + 2 + pl);
                p += 10 + pl;
                NEED(dl);
                p += dl;
            }
        }
    }
    if ((u64)(end - p) >= 12 && !memcmp(p, "SSBUHMI1", 8)) {
        /* "SSBUHMI1", u32 count, then each file of the mods' one tree: u16 path length, u64 size, u32 length of
         * the bytes carried (the whole file, or none for a big one), the UTF-8 path ('/' between folders), the bytes */
        u32 n = rd32(p + 8);
        p += 12;
        if (n > (1u << 20)) rt_fatal("host mods: %u files in the host's mods:/", n);
        hm.mods_files = calloc(n ? n : 1, sizeof *hm.mods_files);
        if (!hm.mods_files) rt_fatal("host mods: no memory");
        for (u32 i = 0; i < n; i++) {
            NEED(14);
            struct HmModsFile f = {0};
            f.path_len = (u32)p[0] | (u32)p[1] << 8;
            f.size = rd64(p + 2);
            f.data_len = rd32(p + 10);
            NEED(14 + (u64)f.path_len + f.data_len);
            if (f.data_len && f.data_len != f.size) rt_fatal("host mods: a mods:/ file cut short in the host's pack");
            f.path = (const char *)p + 14;
            f.data = p + 14 + f.path_len;
            p += 14 + (u64)f.path_len + f.data_len;
            hm.mods_files[hm.nmods_files++] = f;
        }
        hm.mods_index = 1;
    }
#undef NEED
    if (hm.arcropolis) hm_build_sd();
    fprintf(stderr, "[hostmods] the host's %u mod(s) and %u plugin(s), %.1f MB, %s (pack %s)\n", hm.nmods,
            hm.nplugins, (double)plain / 1048576.0, hm.open ? "an open pack (not sealed), played from memory" : "in memory only", id);
    if (hm_download_wanted() && !hm.open)
        fprintf(stderr, "[hostmods] the host didn't allow downloads (a sealed pack): its mods aren't saved\n");
    else if (hm_download_wanted() && hm.nfolders)
        hm_save_folders();
    else if (hm.nfolders)
        fprintf(stderr, "[hostmods] the host lets players keep its mods: tick F1 > Online > Download host's mods "
                        "(or set SSBU_HOST_MODS_DOWNLOAD=1) and start the game again\n");
    if (!hm.nplugins && hm.other_os_plugins && hm.arcropolis)
        fprintf(stderr, "[hostmods] the host's plugins are built for other systems: this game runs its own built-in copies "
                        "of them (the mods' plugin.nro files, by build id), where it has them\n");
    else if (!hm.nplugins && hm.other_os_plugins)
        rt_fatal("host mods: the host's mods have plugins built for other systems than this one only "
                 "(the host has to build them for this one too: docs/HOST_MODS.md)");
    return TRUE;
}
static void load(void) { InitOnceExecuteOnce(&hm_once, hm_load, NULL, NULL); }

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
    *map = hm.map; *map_len = hm.map_len;   /* (version 4: none - ARCropolis lays the mods over this game's data) */
    *overlay = hm.overlay; *overlay_len = hm.overlay_len;
}

/* ---- mods:/ (hle_fs.c): ARCropolis' view of the mods, the host's under host mods ---------------------
 * Plugins look there for their marker files (mods:/fighter/<name>/model/body/cNN/<x>.marker) to know
 * which costume slots they run on; this game's own mods aren't the ones running. */
static int hm_path_eq(const char *a, u32 al, const char *b, size_t bl) {
    return al == bl && !_strnicmp(a, b, bl);
}
/* What `rest` is in the host's mods:/ (leading '/' left out): 0 nothing, 1 a file (*data NULL: its bytes
 * aren't in the pack - a big one), 2 a folder. */
int hostmods_mods_entry(const char *rest, const u8 **data, u64 *size) {
    load();
    while (*rest == '/') rest++;
    size_t l = strlen(rest);
    while (l && rest[l - 1] == '/') l--;
    if (!l) return 2;
    for (u32 i = 0; i < hm.nmods_files; i++) {
        const struct HmModsFile *f = &hm.mods_files[i];
        if (hm_path_eq(f->path, f->path_len, rest, l)) {
            *data = f->data_len || !f->size ? f->data : NULL;
            *size = f->size;
            return 1;
        }
    }
    for (u32 i = 0; i < hm.nmods_files; i++) {
        const struct HmModsFile *f = &hm.mods_files[i];
        if (f->path_len > l && f->path[l] == '/' && !_strnicmp(f->path, rest, l)) return 2;
    }
    return 0;
}
/* A folder's entries in the host's mods:/: add(ctx, name, length, is a folder, size) for each, once. */
int hostmods_mods_list(const char *rest, void (*add)(void *, const char *, size_t, int, u64), void *ctx) {
    load();
    while (*rest == '/') rest++;
    size_t l = strlen(rest);
    while (l && rest[l - 1] == '/') l--;
    int found = !l;
    for (u32 i = 0; i < hm.nmods_files; i++) {
        const struct HmModsFile *f = &hm.mods_files[i];
        if (l && !(f->path_len > l && f->path[l] == '/' && !_strnicmp(f->path, rest, l))) continue;
        found = 1;
        const char *name = f->path + (l ? l + 1 : 0);
        u32 left = f->path_len - (u32)(name - f->path);
        const char *slash = memchr(name, '/', left);
        size_t nl = slash ? (size_t)(slash - name) : left;
        int seen = 0;   /* (a folder: listed at its first file) */
        for (u32 k = 0; k < i && !seen; k++) {
            const struct HmModsFile *g = &hm.mods_files[k];
            if (l && !(g->path_len > l && g->path[l] == '/' && !_strnicmp(g->path, rest, l))) continue;
            const char *gn = g->path + (l ? l + 1 : 0);
            u32 gleft = g->path_len - (u32)(gn - g->path);
            seen = gleft >= nl && !_strnicmp(gn, name, nl) && (gleft == nl || gn[nl] == '/');
        }
        if (!seen) add(ctx, name, nl, slash != NULL, slash ? 0 : f->size);
    }
    return found;
}

/* ---- (version 4) the SD card's ultimate/mods: the pack's folders --------------------------------- */
static int hm_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* a and b compared without case, byte by byte, the shorter first when one starts the other */
static int hm_cmp_ci(const char *a, u32 al, const char *b, u32 bl) {
    u32 n = al < bl ? al : bl;
    for (u32 i = 0; i < n; i++) {
        int d = hm_lower((u8)a[i]) - hm_lower((u8)b[i]);
        if (d) return d;
    }
    return al < bl ? -1 : al > bl;
}
static int hm_sd_order(const void *x, const void *y) {
    const struct HmModsFile *a = x, *b = y;
    return hm_cmp_ci(a->path, a->path_len, b->path, b->path_len);
}
/* (sorted so: the files under any folder side by side, found by halving) */
static void hm_build_sd(void) {
    u64 total = 0;
    const u8 *p = hm.folders;
    for (u32 i = 0; i < hm.nfolders; i++) {
        HmFolder f;
        p = hm_folder_at(p, &f);
        total += f.nfiles;
    }
    if (total > (1u << 22)) rt_fatal("host mods: %llu files in the host's mod folders", (unsigned long long)total);
    hm.sd = calloc(total ? total : 1, sizeof *hm.sd);
    if (!hm.sd) rt_fatal("host mods: no memory");
    p = hm.folders;
    for (u32 i = 0; i < hm.nfolders; i++) {
        HmFolder f;
        p = hm_folder_at(p, &f);
        if (!hm_safe_part(f.name, f.name_len)) rt_fatal("host mods: a mod folder named \"%.*s\" in the host's pack", (int)(f.name_len > 200 ? 200 : f.name_len), f.name);
        const u8 *q = f.files;
        for (u32 k = 0; k < f.nfiles; k++) {
            const char *rel;
            u32 rl;
            const u8 *data;
            u64 len;
            q = hm_file_at(q, &rel, &rl, &data, &len);
            if (!hm_safe_path(rel, rl)) continue;
            char *path = malloc((size_t)f.name_len + 1 + rl);
            if (!path) rt_fatal("host mods: no memory");
            memcpy(path, f.name, f.name_len);
            path[f.name_len] = '/';
            memcpy(path + f.name_len + 1, rel, rl);
            hm.sd[hm.nsd++] = (struct HmModsFile){path, f.name_len + 1 + rl, len, data, 0};
        }
    }
    qsort(hm.sd, hm.nsd, sizeof *hm.sd, hm_sd_order);
    fprintf(stderr, "[hostmods] an ARCropolis pack: %u mod folder(s), %u file(s) on the SD card's ultimate/mods\n", hm.nfolders, hm.nsd);
}
/* The first file at or after (path, l) in that order. */
static u32 hm_sd_at(const char *path, u32 l) {
    u32 lo = 0, hi = hm.nsd;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (hm_cmp_ci(hm.sd[mid].path, hm.sd[mid].path_len, path, l) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static int hm_sd_under(u32 i, const char *dir, u32 l) {   /* (dir: with its '/') */
    return i < hm.nsd && hm.sd[i].path_len > l && !_strnicmp(hm.sd[i].path, dir, l);
}

int hostmods_arcropolis(void) {
    load();
    return hm.arcropolis;
}
/* What `rest` (under ultimate/mods/) is on the SD card of a version 4 pack: 0 nothing, 1 a file, 2 a folder. */
int hostmods_sd_entry(const char *rest, const u8 **data, u64 *size) {
    load();
    while (*rest == '/') rest++;
    size_t l = strlen(rest);
    while (l && rest[l - 1] == '/') l--;
    if (!l) return 2;
    if (l > 2000) return 0;
    char dir[2002];
    memcpy(dir, rest, l);
    u32 i = hm_sd_at(dir, (u32)l);
    if (i < hm.nsd && hm_path_eq(hm.sd[i].path, hm.sd[i].path_len, dir, l)) {
        *data = hm.sd[i].data;
        *size = hm.sd[i].size;
        return 1;
    }
    dir[l] = '/';
    return hm_sd_under(hm_sd_at(dir, (u32)l + 1), dir, (u32)l + 1) ? 2 : 0;
}
/* A folder's entries there: add(ctx, name, length, is a folder, size) for each, once; 0 when it isn't one. */
int hostmods_sd_list(const char *rest, void (*add)(void *, const char *, size_t, int, u64), void *ctx) {
    load();
    while (*rest == '/') rest++;
    size_t l = strlen(rest);
    while (l && rest[l - 1] == '/') l--;
    if (l > 2000) return 0;
    char dir[2002];
    memcpy(dir, rest, l);
    if (l) dir[l++] = '/';
    u32 i = l ? hm_sd_at(dir, (u32)l) : 0;
    int found = !l;
    const char *last = NULL;
    size_t last_len = 0;
    for (; l ? hm_sd_under(i, dir, (u32)l) : i < hm.nsd; i++) {
        found = 1;
        const char *name = hm.sd[i].path + l;
        u32 left = hm.sd[i].path_len - (u32)l;
        const char *slash = memchr(name, '/', left);
        size_t nl = slash ? (size_t)(slash - name) : left;
        if (last && last_len == nl && !_strnicmp(last, name, nl)) continue;   /* (a folder's later files) */
        last = name;
        last_len = nl;
        add(ctx, name, nl, slash != NULL, slash ? 0 : hm.sd[i].size);
    }
    return found;
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
