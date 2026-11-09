/* The game's own Online mode (Quickplay, Battle Arenas, ...) against the NEX game server of
 * tools/nex, which runs inside the online lobby (tools/ssbu_online_lobby.py) at the lobby's
 * virtual address, 10.13.0.1.
 *
 * Online logs in with the network service account's id token (hle_env.c) to the game server
 * "https://g23380901-lp1.s.n.srv.nintendo.net/": NEX's PRUDP Lite, over a WebSocket, over TLS.
 * NEX has a second way in that no retail code uses: a direct server address in the NgsFacade
 * (host at +0x240, an nn::nex::String, and port at +0x268). Set, NEX opens the same WebSocket
 * without TLS to that address - a stream socket, which the stream module below carries to the
 * lobby over TCP. Pia's NAT check servers (nncs1, nncs2) resolve to the server too, and Pia's
 * matches run between the players' virtual addresses, as Local Wireless's do.
 *
 * SSBU_ONLINE_SERVER=host:port overrides the server (default 10.13.0.1:60000); "off" leaves
 * the game's own address (and Online fails, as offline). */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define SD_BOTH SHUT_RDWR
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "host.h"
#include "net.h"
#include "online.h"

#define DEFAULT_HOST "10.13.0.1"
#define DEFAULT_PORT 60000

/* The server to log in to: 0 when online play is off (it needs the online lobby: a private
 * room's relay in the host's game has no game server). */
static int online_server(char *host, size_t n, u16 *port) {
    const char *v = getenv("SSBU_ONLINE_SERVER");
    if (v && !strcmp(v, "off")) return 0;
    if (!v || !*v) {
        if (!net_configured() || !net_relay_address()) return 0;
        snprintf(host, n, "%s", DEFAULT_HOST);
        *port = DEFAULT_PORT;
        return 1;
    }
    const char *colon = strrchr(v, ':');
    size_t len = colon ? (size_t)(colon - v) : strlen(v);
    if (!len || len >= n) return 0;
    memcpy(host, v, len);
    host[len] = 0;
    *port = colon && atoi(colon + 1) > 0 && atoi(colon + 1) < 65536 ? (u16)atoi(colon + 1) : DEFAULT_PORT;
    return 1;
}

/* The server's IPv4 address (host order), 0 if it is not a dotted address. */
static u32 online_ip(void) {
    char host[64];
    u16 port;
    unsigned a, b, d, e;
    if (!online_server(host, sizeof host, &port) || sscanf(host, "%u.%u.%u.%u", &a, &b, &d, &e) != 4) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* Names of Nintendo's servers (the game server, the NAT check servers) are ours while online. */
static int nintendo_name(const char *name) { return name && strstr(name, "nintendo.net") != NULL; }

/* A dotted IPv4 address (host order), 0 if the name is not one. Station URLs carry addresses
 * this way, and the SDK's resolver (GetAddrInfo) does not work here. */
static u32 dotted_ip(const char *name) {
    unsigned a, b, d, e;
    char end;
    if (!name || sscanf(name, "%u.%u.%u.%u%c", &a, &b, &d, &e, &end) != 4 || (a | b | d | e) > 255) return 0;
    return a << 24 | b << 16 | d << 8 | e;
}

/* NEX's login job, the step that works out the game server's address (main+0x249190;
 * the job is x0, its NgsFacade at +360). Give the facade the direct address first. */
/* signature: 16 words, sha256 c9c626b15efbb3b4 */
HLE_WRAP(7100249190) {
    u64 facade = ld64(c->x[0] + 360);
    char host[64];
    u16 port;
    if (facade && online_server(host, sizeof host, &port)) {
        u64 str = facade + 576;   /* nn::nex::String { vtable; char local[16]; char *data; u64 capacity } */
        u64 data = ld64(str + 24), capacity = ld64(str + 32);
        if (data && capacity > strlen(host)) {
            memcpy(P(char, data), host, strlen(host) + 1);
            st16(facade + 616, port);
            if (net_log()) fprintf(stderr, "[online] NEX login to %s:%u\n", host, port);
        } else if (net_log()) {
            fprintf(stderr, "[online] NEX server address left as the game's: no room for \"%s\"\n", host);
        }
    }
    f_7100249190_orig(c);
}

/* NEX's InetAddress from a name (main+0x1becd0; x0 the address, x1 the name -> bool):
 * the address is at +12, in network order. */
/* signature: 16 words, sha256 324bea721ad32d31 */
HLE_WRAP(71001becd0) {
    const char *name = c->x[1] ? P(const char, c->x[1]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (net_log() && ours) fprintf(stderr, "[online] NEX resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        st32(c->x[0] + 12, _byteswap_ulong(ip));
        c->x[0] = 1;
        return;
    }
    f_71001becd0_orig(c);
}

/* Pia's name resolution (main+0xba850; x0 the name, x4 a Pia InetAddress, whose IPv4
 * address is at +8 in host order -> bool): the NAT check servers are the online server too.
 * Pia's NAT check needs its two servers at different addresses: nncs2 gets the last address of
 * the server's /16 (10.13.255.254), which the online lobby keeps for it (tools/nex/server.py). */
/* signature: 16 words, sha256 ea3c274364ccadf2 */
HLE_WRAP(71000ba850) {
    const char *name = c->x[0] ? P(const char, c->x[0]) : NULL;
    int ours = nintendo_name(name);
    u32 ip = ours ? online_ip() : dotted_ip(name);
    if (ip && ours && !strncmp(name, "nncs2", 5)) ip = (ip & 0xFFFF0000) | 0xFFFE;
    if (net_log() && ours) fprintf(stderr, "[online] Pia resolves \"%s\"%s\n", name, ip ? " -> the online server" : "");
    if (ip) {
        if (c->x[4]) {
            memset(P(u8, c->x[4] + 8), 0, 16);
            st32(c->x[4] + 8, ip);
        }
        c->x[0] = 1;
        return;
    }
    f_71000ba850_orig(c);
}

/* ---- nn::crypto: SHA-1 ------------------------------------------------------------
 * The SDK's uses the CPU's SHA-1 instructions, which the recompiler does not translate; NEX's
 * WebSocket checks the server's Sec-WebSocket-Accept with it. */
static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(u8 out[20], const u8 *data, u64 len) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u64 i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    u8 tail[128] = {0};
    u64 rest = len - i;
    memcpy(tail, data + i, (size_t)rest);
    tail[rest] = 0x80;
    u64 n = rest + 9 <= 64 ? 64 : 128, bits = len * 8;
    for (int k = 0; k < 8; k++) tail[n - 1 - k] = (u8)(bits >> (8 * k));
    sha1_block(h, tail);
    if (n == 128) sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) { out[4 * k] = (u8)(h[k] >> 24); out[4 * k + 1] = (u8)(h[k] >> 16); out[4 * k + 2] = (u8)(h[k] >> 8); out[4 * k + 3] = (u8)h[k]; }
}

HLE(_ZN2nn6crypto16GenerateSha1HashEPvmPKvm) {   /* (void *hash, u64 hash size, const void *data, u64 size) */
    u8 h[20];
    sha1(h, P(u8, c->x[2]), c->x[3]);
    memcpy(P(void, c->x[0]), h, c->x[1] < 20 ? (size_t)c->x[1] : 20);
}

/* ---- NEX's stream sockets ---------------------------------------------------------
 * A stream socket connected to the online server is a TCP connection to the lobby
 * (SSBU_NET_RELAY's host, same port number as its UDP), which starts with "SSBUNEX1", this
 * player's virtual address and port and the server port asked for (big-endian u32, u16, u16).
 * A thread per stream connects and then reads into the stream's buffer. */
enum { E_AGAIN_ = 11, E_MFILE_ = 24, E_PIPE_ = 32, E_CONNRESET_ = 104, E_ISCONN_ = 106, E_NOTCONN_ = 107,
       E_CONNREFUSED_ = 111, E_ALREADY_ = 114, E_INPROGRESS_ = 115 };
enum { POLLIN_ = 0x1, POLLOUT_ = 0x4, POLLERR_ = 0x8, POLLHUP_ = 0x10, POLLRDNORM_ = 0x40 };
enum { S_CONNECTING = 1, S_OPEN, S_FAILED };
#define CONNECT_MS 5000

typedef struct Stream {
    i32 fd;                  /* 0: free */
    u32 gen;                 /* (tells a reused slot from the one a thread was started for) */
    int state, err, eof, reported;
    SOCKET s;
    u8 *buf;
    u32 len, cap;
    u32 vip;
    u16 vport, dport;
    /* (rollback, online_rb_*) what the game read and sent, for a savestate's load: the last HIST bytes of each */
    u64 taken, sent, resend;   /* (resend < sent after a load: the game sends again what it had sent; left out while it matches) */
    u8 *hist, *shist;
} Stream;
#define STREAMS 16
static Stream streams[STREAMS];
static SRWLOCK stream_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE stream_change = CONDITION_VARIABLE_INIT;
static u32 stream_gen;

/* ---- rollback (SSBU_RB): a stream through a savestate's load ---------------------------------------
 * A load puts the game's memory back as it was at a save, NEX's state with it: what it read from a stream
 * since is forgotten, and what it sent since it will send again. The stream itself can't go back, so it is
 * the stream's ends that are made to agree: the bytes the game read after the save are put back in front of
 * what is waiting (it reads them again), and the bytes it sends again are left out as long as they are the
 * ones it had sent (once they differ, the rest goes: the lobby's NEX server then sees a packet twice, which
 * it takes as a repeat). Kept only once savestates are taken (online_rb_save), HIST bytes a stream. */
#define HIST (256u << 10)
static int rb_keep;
static long long rb_back_bytes, rb_resent_left_out, rb_resend_differ;
static void hist_put(u8 **ring, u64 at, const u8 *data, u64 n) {
    if (!*ring && !(*ring = malloc(HIST))) return;
    for (u64 i = 0; i < n; i++) (*ring)[(at + i) % HIST] = data[i];
}

static Stream *stream_of(i32 fd) {
    for (int i = 0; i < STREAMS; i++)
        if (streams[i].fd == fd) return &streams[i];
    return NULL;
}

static void changed(void) {
    WakeAllConditionVariable(&stream_change);
    net_wake();
}

/* The lobby's real address: host[:port], from SSBU_NET_RELAY or online_server.txt (net_relay.c). */
static int lobby_address(struct sockaddr_in *out) {
    const char *v = net_relay_address();
    char host[256];
    if (!v || !*v) return 0;
    snprintf(host, sizeof host, "%s", v);
    u16 port = NET_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = (u16)atoi(colon + 1); *colon = 0; }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}

/* Connects s to the lobby within CONNECT_MS (a lobby that does not answer fails the connect, not
 * hangs it), and leaves it blocking. */
#ifdef _WIN32
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    u_long nb = 1;
    int ok = ioctlsocket(s, FIONBIO, &nb) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = {CONNECT_MS / 1000, CONNECT_MS % 1000 * 1000};
        ok = WSAGetLastError() == WSAEWOULDBLOCK && select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
    }
    nb = 0;
    return ok && ioctlsocket(s, FIONBIO, &nb) == 0;
}
#else
static int connect_within(SOCKET s, const struct sockaddr_in *at) {
    int flags = fcntl(s, F_GETFL, 0);
    int ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
    if (ok && connect(s, (const struct sockaddr *)at, sizeof *at) != 0) {   /* (a failed one polls as writable, with SO_ERROR set) */
        struct pollfd p = {s, POLLOUT, 0};
        int err = 0;
        socklen_t n = sizeof err;
        ok = errno == EINPROGRESS && poll(&p, 1, CONNECT_MS) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && !err;
    }
    return ok && fcntl(s, F_SETFL, flags) == 0;
}
#endif

static DWORD WINAPI stream_thread(void *arg) {
    Stream *st = arg;
    AcquireSRWLockExclusive(&stream_lock);
    i32 fd = st->fd;
    u32 gen = st->gen, vip = st->vip;
    u16 vport = st->vport, dport = st->dport;
    ReleaseSRWLockExclusive(&stream_lock);
    struct sockaddr_in at;
    SOCKET s = INVALID_SOCKET;
    int ok = lobby_address(&at);
    if (ok) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = s != INVALID_SOCKET && connect_within(s, &at);
    }
    if (ok) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        u8 head[16] = {'S', 'S', 'B', 'U', 'N', 'E', 'X', '1', (u8)(vip >> 24), (u8)(vip >> 16), (u8)(vip >> 8), (u8)vip,
                       (u8)(vport >> 8), (u8)vport, (u8)(dport >> 8), (u8)dport};
        ok = send(s, (const char *)head, sizeof head, 0) == (int)sizeof head;
    }
    AcquireSRWLockExclusive(&stream_lock);
    if (st->fd != fd || st->gen != gen) {   /* (closed meanwhile) */
        ReleaseSRWLockExclusive(&stream_lock);
        if (s != INVALID_SOCKET) closesocket(s);
        return 0;
    }
    if (ok) { st->s = s; st->state = S_OPEN; }
    else { st->state = S_FAILED; st->err = E_CONNREFUSED_; if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; }
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log()) fprintf(stderr, "[online] stream %d to port %u: %s\n", fd, dport, ok ? "connected to the lobby" : "the lobby did not answer");
    changed();
    if (!ok) return 0;
    char chunk[16384];
    for (;;) {
        int n = recv(s, chunk, sizeof chunk, 0);
        AcquireSRWLockExclusive(&stream_lock);
        if (st->fd != fd || st->gen != gen) { ReleaseSRWLockExclusive(&stream_lock); break; }
        if (n <= 0) {
            st->eof = 1;
            ReleaseSRWLockExclusive(&stream_lock);
            changed();
            break;
        }
        if (st->len + (u32)n > st->cap) {
            u32 cap = st->cap ? st->cap : 65536;
            while (cap < st->len + (u32)n) cap *= 2;
            u8 *b = realloc(st->buf, cap);
            if (!b) { st->eof = 1; ReleaseSRWLockExclusive(&stream_lock); changed(); break; }
            st->buf = b;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, chunk, (size_t)n);
        st->len += (u32)n;
        ReleaseSRWLockExclusive(&stream_lock);
        changed();
    }
    return 0;
}

int online_stream_connect(i32 fd, u32 ip, u16 port, u32 my_ip, u16 my_port, int nonblock) {
    u32 server = online_ip();
    if (!server || ip != server) return E_CONNREFUSED_;   /* (no other stream reaches anything) */
    if (net_unreachable() || !my_ip) return E_CONNREFUSED_;   /* (the lobby is gone: Online is too) */
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    if (st) {
        int e = st->state == S_OPEN ? E_ISCONN_ : E_ALREADY_;
        ReleaseSRWLockExclusive(&stream_lock);
        return e;
    }
    for (int i = 0; i < STREAMS && !st; i++)
        if (!streams[i].fd) st = &streams[i];
    if (!st) { ReleaseSRWLockExclusive(&stream_lock); return E_MFILE_; }
    memset(st, 0, sizeof *st);
    st->fd = fd;
    st->gen = ++stream_gen;
    st->state = S_CONNECTING;
    st->s = INVALID_SOCKET;
    st->vip = my_ip;
    st->vport = my_port;
    st->dport = port;
    ReleaseSRWLockExclusive(&stream_lock);
    HANDLE t = CreateThread(NULL, 0, stream_thread, st, 0, NULL);
    if (!t) { online_stream_close(fd); return E_CONNREFUSED_; }
    CloseHandle(t);
    if (nonblock) return E_INPROGRESS_;
    AcquireSRWLockExclusive(&stream_lock);
    while (st->fd == fd && st->state == S_CONNECTING) SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    int err = st->fd == fd && st->state == S_FAILED ? st->err : 0;
    if (err) st->reported = 1;
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

i64 online_stream_send(i32 fd, const void *data, u64 len, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    while (st && st->state == S_CONNECTING && !nonblock) {
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
        st = stream_of(fd);
    }
    i64 r = !st ? -E_NOTCONN_ : st->state == S_CONNECTING ? -E_AGAIN_ : st->state != S_OPEN || st->eof ? -E_PIPE_ : 0;
    SOCKET s = st && !r ? st->s : INVALID_SOCKET;
    const char *p = data;
    u64 left = len;
    if (!r && rb_keep) {   /* (after a load: what the game sends again) */
        while (left && st->resend < st->sent) {
            if (st->shist && (u8)*p == st->shist[st->resend % HIST]) { p++; left--; st->resend++; rb_resent_left_out++; continue; }
            rb_resend_differ++;
            if (rb_resend_differ <= 5) fprintf(stderr, "[online] stream %d: after a load the game sends other bytes than before (%llu of %llu again matched): the rest goes\n",
                                              fd, (unsigned long long)(st->resend), (unsigned long long)st->sent);
            st->resend = st->sent;
        }
        if (left) { hist_put(&st->shist, st->sent, (const u8 *)p, left); st->sent += left; st->resend = st->sent; }
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (r) return r;
    if (!left) return (i64)len;
    while (left) {   /* (the host socket blocks: everything goes, in order) */
        int n = send(s, p, left > 0x10000 ? 0x10000 : (int)left, 0);
        if (n <= 0) return len - left ? (i64)(len - left) : -E_CONNRESET_;
        p += n;
        left -= (u64)n;
    }
    if (net_log() > 1) fprintf(stderr, "[online] stream %d: sent %llu bytes\n", fd, (unsigned long long)len);
    return (i64)len;
}

i64 online_stream_recv(i32 fd, void *buf, u64 len, int peek, int nonblock) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st;
    for (;;) {
        st = stream_of(fd);
        if (!st || st->len || st->eof || st->state == S_FAILED || nonblock) break;
        SleepConditionVariableSRW(&stream_change, &stream_lock, INFINITE, 0);
    }
    i64 r;
    if (!st) r = -E_NOTCONN_;
    else if (st->len) {
        u32 n = st->len < len ? st->len : (u32)len;
        memcpy(buf, st->buf, n);
        if (!peek) {
            if (rb_keep) { hist_put(&st->hist, st->taken, st->buf, n); st->taken += n; }
            memmove(st->buf, st->buf + n, st->len - n); st->len -= n;
        }
        r = n;
    } else if (st->eof) r = 0;
    else if (st->state == S_FAILED) r = -st->err;
    else r = -E_AGAIN_;
    ReleaseSRWLockExclusive(&stream_lock);
    if (net_log() > 1 && r > 0) fprintf(stderr, "[online] stream %d: received %lld bytes\n", fd, (long long)r);
    return r;
}

u16 online_stream_revents(i32 fd, u16 events) {
    AcquireSRWLockShared(&stream_lock);
    Stream *st = stream_of(fd);
    u16 r = 0;
    if (!st) r = POLLHUP_;
    else {
        if (st->state == S_OPEN && !st->eof) r |= events & POLLOUT_;
        if (st->len || st->eof) r |= events & (POLLIN_ | POLLRDNORM_);
        if (st->eof) r |= POLLHUP_;
        if (st->state == S_FAILED) r |= POLLERR_ | (events & POLLOUT_);
    }
    ReleaseSRWLockShared(&stream_lock);
    return r;
}

int online_stream_error(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    int err = 0;
    if (st && st->state == S_FAILED && !st->reported) { err = st->err; st->reported = 1; }
    ReleaseSRWLockExclusive(&stream_lock);
    return err;
}

void online_stream_close(i32 fd) {
    AcquireSRWLockExclusive(&stream_lock);
    Stream *st = stream_of(fd);
    SOCKET s = INVALID_SOCKET;
    if (st) {
        s = st->s;
        free(st->buf); free(st->hist); free(st->shist);
        memset(st, 0, sizeof *st);   /* (its thread sees its fd gone and stops) */
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (s != INVALID_SOCKET) { shutdown(s, SD_BOTH); closesocket(s); }
    if (st) changed();
}

/* (hle_net.c's net_rb_save / net_rb_load, rollback) Each stream's place in what it read and sent. */
void online_rb_save(OnlineRb *o) {
    AcquireSRWLockExclusive(&stream_lock);
    rb_keep = 1;
    for (int i = 0; i < STREAMS && i < ONLINE_RB_STREAMS; i++) {
        o->s[i].fd = streams[i].fd; o->s[i].gen = streams[i].gen;
        o->s[i].taken = streams[i].taken; o->s[i].sent = streams[i].sent;
    }
    ReleaseSRWLockExclusive(&stream_lock);
}
void online_rb_load(const OnlineRb *o) {
    int any = 0;
    AcquireSRWLockExclusive(&stream_lock);
    for (int i = 0; i < STREAMS && i < ONLINE_RB_STREAMS; i++) {
        Stream *st = &streams[i];
        if (!o->s[i].fd || st->fd != o->s[i].fd || st->gen != o->s[i].gen) continue;
        u64 back = st->taken - o->s[i].taken;
        if (back && back <= HIST && st->hist) {   /* (read since the save: in front of what is waiting, to be read again) */
            if (st->len + back > st->cap) {
                u32 cap = st->cap ? st->cap : 65536;
                while (cap < st->len + back) cap *= 2;
                u8 *b = realloc(st->buf, cap);
                if (!b) continue;
                st->buf = b; st->cap = cap;
            }
            memmove(st->buf + back, st->buf, st->len);
            for (u64 k = 0; k < back; k++) st->buf[k] = st->hist[(o->s[i].taken + k) % HIST];
            st->len += (u32)back; st->taken = o->s[i].taken;
            rb_back_bytes += (long long)back; any = 1;
        } else if (back) fprintf(stderr, "[online] stream %d: a load %llu bytes back: more than kept, not read again\n", st->fd, (unsigned long long)back);
        if (o->s[i].sent < st->sent && st->sent - o->s[i].sent <= HIST) st->resend = o->s[i].sent;
    }
    ReleaseSRWLockExclusive(&stream_lock);
    if (any) changed();
}
void online_rb_counts(long long *back, long long *left_out, long long *differ) { *back = rb_back_bytes; *left_out = rb_resent_left_out; *differ = rb_resend_differ; }

/* ---- the arena bracket (tools/bracket.py) ---------------------------------------------------
 * With SSBU_ARENA_BRACKET=1, a thread asks the lobby (its HTTP shares the lobby's port) where this
 * player goes in the Battle Arena every 2 s: GET /arena/assignment?name=<this player>, of which it
 * keeps "match", "place", "role" and "game". online_arena_assignment hands the latest out, with a
 * number that changes when the answer does (runtime/hle_trace.c turns it into a place request): in a
 * best-of set the same match comes again with the next game, so its two players start it again. */
static SRWLOCK ab_lock = SRWLOCK_INIT;
static long ab_seq;
static int ab_place, ab_game;
static char ab_role[8], ab_match[24], ab_opponent[48];

static int http_request(const char *method, const char *path, const char *body, char *out, int cap) {
    struct sockaddr_in at;
    if (!lobby_address(&at)) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    if (!connect_within(s, &at)) { closesocket(s); return -1; }
#ifdef _WIN32
    DWORD tmo = 3000;
#else
    struct timeval tmo = {3, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    char req[1024];
    int blen = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: lobby\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                     method, path, blen, body ? body : "");
    int len = -1;
    if (n > 0 && n < (int)sizeof req && send(s, req, n, 0) == n) {
        len = 0;
        int r;
        while (len < cap - 1 && (r = recv(s, out + len, cap - 1 - len, 0)) > 0) len += r;
        out[len] = 0;
    }
    closesocket(s);
    if (len <= 0 || strncmp(out, "HTTP/1.1 200", 12)) return -1;
    char *start = strstr(out, "\r\n\r\n");
    if (!start) return -1;
    start += 4;
    len -= (int)(start - out);
    memmove(out, start, (size_t)len + 1);
    return len;
}

/* A JSON string or number member's text (flat objects only: bracket.py's answer). */
static int json_member(const char *js, const char *key, char *out, int cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    int n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && n < cap - 1; p++) out[n++] = *p;
    } else {
        for (; *p && *p != ',' && *p != '}' && *p != ' ' && n < cap - 1; p++) out[n++] = *p;
    }
    out[n] = 0;
    return 1;
}

/* The value of member `key` of the object js starts with (not of the objects inside it), or NULL. */
static const char *json_key(const char *js, const char *key) {
    size_t kl = strlen(key);
    int depth = 0;
    for (const char *p = js; *p; p++) {
        if (*p == '"') {
            const char *s = ++p;
            while (*p && *p != '"') p += p[0] == '\\' && p[1] ? 2 : 1;
            if (!*p) return NULL;
            if (depth == 1 && (size_t)(p - s) == kl && !strncmp(s, key, kl)) {
                const char *v = p + 1;
                while (*v == ' ') v++;
                if (*v == ':') { for (v++; *v == ' '; v++) {} return v; }
            }
        } else if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth <= 0) return NULL;
    }
    return NULL;
}
/* A JSON string value's text (escapes taken as the character after the backslash); "" for any other. */
static void json_text(const char *v, char *out, int cap) {
    int n = 0;
    if (v && *v == '"')
        for (v++; *v && *v != '"' && n < cap - 1; v++) {
            if (*v == '\\' && v[1]) v++;
            out[n++] = *v;
        }
    out[n] = 0;
}

/* ---- stage striking (tools/bracket.py's "stages") ---------------------------------------------
 * The assignment's "stage" (the stage chosen for the game being played, "" until it is) and, while
 * its "stages" says striking goes on, whose turn it is: kept apart from the place request's number,
 * which a strike does not change. With SSBU_ARENA_STRIKE=1 (the test bots) this player takes its
 * turns itself: POST /arena/stage with the first stage still open, as many times as its turn has. */
static char as_stage[48], as_phase[8], as_turn[48];
static int as_on;

/* Takes "stages" from an answer (the assignment, or POST /arena/stage's). */
static void arena_stages_take(const char *js) {
    const char *st = json_key(js, "stages");
    char stage[48] = "", phase[8] = "", turn[48] = "";
    if (st && *st == '{') {
        json_text(json_key(st, "stage"), stage, sizeof stage);
        json_text(json_key(st, "phase"), phase, sizeof phase);
        json_text(json_key(st, "turn"), turn, sizeof turn);
    }
    AcquireSRWLockExclusive(&ab_lock);
    int was = as_on, changed = strcmp(stage, as_stage) || strcmp(phase, as_phase) || strcmp(turn, as_turn);
    as_on = st && *st == '{';
    snprintf(as_stage, sizeof as_stage, "%s", stage);
    snprintf(as_phase, sizeof as_phase, "%s", phase);
    snprintf(as_turn, sizeof as_turn, "%s", turn);
    ReleaseSRWLockExclusive(&ab_lock);
    if (as_on && (changed || !was)) {
        fprintf(stderr, "[arena] stages: %s%s%s%s%s\n", phase, turn[0] ? ", " : "", turn, stage[0] ? ", stage " : "", stage);
        fflush(stderr);
    }
}

static void json_escape(char *out, int cap, const char *in);
/* (the bracket thread, after taking the assignment js) */
static void arena_strike(const char *js, const char *me, const char *match, int game) {
    static int on = -1;
    if (on < 0) on = getenv("SSBU_ARENA_STRIKE") && !strcmp(getenv("SSBU_ARENA_STRIKE"), "1");
    static char buf[4096];
    char body[256], m[64], i[112], first[48];
    for (int k = 0; k < 8; k++, js = buf) {   /* (a turn is 2 at most; an answer that never moves on stops after 8) */
        AcquireSRWLockExclusive(&ab_lock);
        int mine = on && as_on && as_turn[0] && !_stricmp(as_turn, me) && strcmp(as_phase, "done");
        ReleaseSRWLockExclusive(&ab_lock);
        if (!mine) return;
        const char *st = json_key(js, "stages");
        const char *opt = st ? json_key(st, "options") : NULL;
        first[0] = 0;
        if (opt && *opt == '[') json_text(opt + 1, first, sizeof first);
        if (!first[0]) return;
        json_escape(m, sizeof m, match);
        json_escape(i, sizeof i, me);
        snprintf(body, sizeof body, "{\"match\": \"%s\", \"player\": \"%s\", \"stage\": \"%s\", \"game\": %d}", m, i, first, game);
        int ok = http_request("POST", "/arena/stage", body, buf, sizeof buf) > 0;
        fprintf(stderr, "[arena] stages: %s %s\n", body, ok ? "taken" : "not taken");
        fflush(stderr);
        if (!ok) return;
        arena_stages_take(buf);
    }
}

/* ---- the tourney table (runtime/hle_tourney_ui.c) -------------------------------------------
 * The bracket as GET /bracket.json has it, fetched when the table should show: when this player is
 * first placed in a bracket, and once a result it reported is decided ("confirmed" in the answer, or
 * else asked again every 2 s until the match is "done", or its set has gone on to the next game: no
 * table then). online_arena_table hands the latest out with
 * a number that changes each time, and the assignment's number from before it (the table goes again
 * once the assignment has moved on: the next place request). */
#define TABLE_JSON (64 * 1024)
static char *at_json;
static long at_seq, at_after;

/* 1 fetched; 0 not (yet); 2 not, for good: the match's set is past game GAME and goes on. */
static int arena_table_fetch(long after, const char *match, int game) {
    char *js = malloc(TABLE_JSON);
    if (!js) return 0;
    int ok = http_request("GET", "/bracket.json", NULL, js, TABLE_JSON) > 0;
    if (ok && match) {   /* (its row: {"id": M, "label", "a", "b", "state", ..., "game": N while not done}) */
        char pat[48];
        snprintf(pat, sizeof pat, "\"id\": \"%s\"", match);
        const char *row = strstr(js, pat), *st = row ? strstr(row, "\"state\": \"") : NULL;
        ok = st && !strncmp(st + 10, "done\"", 5);
        if (!ok && st && game) {
            const char *next = strstr(st, "\"id\": "), *g = strstr(st, "\"game\": ");
            if (g && (!next || g < next) && atoi(g + 8) > game) { free(js); return 2; }
        }
    }
    if (!ok) { free(js); return 0; }
    AcquireSRWLockExclusive(&ab_lock);
    free(at_json);
    at_json = js;
    at_after = after;
    at_seq++;
    ReleaseSRWLockExclusive(&ab_lock);
    fprintf(stderr, "[arena] bracket fetched for the tourney table%s%s\n", match ? " after " : "", match ? match : "");
    fflush(stderr);
    return 1;
}

long online_arena_table(long have, char *out, int cap, long *after) {
    AcquireSRWLockExclusive(&ab_lock);
    long seq = at_seq;
    if (seq != have) snprintf(out, (size_t)cap, "%s", at_json ? at_json : "");
    *after = at_after;
    ReleaseSRWLockExclusive(&ab_lock);
    return seq;
}

static DWORD WINAPI arena_bracket_thread(void *arg) {
    (void)arg;
    static char buf[4096];
    for (;;) {
        const char *name = net_player_name();
        char path[200], enc[120];
        int e = 0;
        for (const unsigned char *c = (const unsigned char *)(name ? name : ""); *c && e < (int)sizeof enc - 4; c++)
            e += isalnum(*c) || *c == '-' || *c == '_' || *c == '.' ? snprintf(enc + e, sizeof enc - e, "%c", *c)
                                                                     : snprintf(enc + e, sizeof enc - e, "%%%02X", *c);
        enc[e] = 0;
        snprintf(path, sizeof path, "/arena/assignment?name=%s", enc);
        char place[8] = "", role[8] = "", match[24] = "", opponent[48] = "", game[8] = "";
        if (http_request("GET", path, NULL, buf, sizeof buf) > 0 && json_member(buf, "place", place, sizeof place)) {
            json_member(buf, "role", role, sizeof role);
            json_member(buf, "match", match, sizeof match);
            json_member(buf, "opponent", opponent, sizeof opponent);
            json_member(buf, "game", game, sizeof game);
            if (!strcmp(match, "null")) match[0] = 0;
            long placed = -1;   /* (the number before this player's first place in a bracket) */
            AcquireSRWLockExclusive(&ab_lock);
            if (atoi(place) != ab_place || strcmp(role, ab_role) || strcmp(match, ab_match) || atoi(game) != ab_game) {
                if (!ab_place && atoi(place)) placed = ab_seq;
                ab_place = atoi(place);
                ab_game = atoi(game);
                snprintf(ab_role, sizeof ab_role, "%s", role);
                snprintf(ab_match, sizeof ab_match, "%s", match);
                snprintf(ab_opponent, sizeof ab_opponent, "%s", opponent);
                ab_seq++;
            }
            ReleaseSRWLockExclusive(&ab_lock);
            if (placed >= 0) arena_table_fetch(placed, NULL, 0);
            arena_stages_take(buf);
            if (match[0]) arena_strike(buf, name ? name : "", match, atoi(game));
        }
        Sleep(2000);
    }
    return 0;
}

long online_arena_assignment(int *place, char *role, int rcap) {
    static int started;
    if (!started) {
        started = 1;
        const char *v = getenv("SSBU_ARENA_BRACKET");
        if (!v || strcmp(v, "1")) started = 2;
        else {
            HANDLE t = CreateThread(NULL, 0, arena_bracket_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            else started = 2;
        }
    }
    if (started == 2) return 0;
    AcquireSRWLockExclusive(&ab_lock);
    long seq = ab_seq;
    *place = ab_place;
    snprintf(role, (size_t)rcap, "%s", ab_role);
    ReleaseSRWLockExclusive(&ab_lock);
    return seq;
}

/* Stage striking for the match this player is assigned: 0 when it has none (no match, or the bracket
 * strikes no stages); else the stage chosen for its game into stage ("" while it is not chosen yet)
 * and 1, or 2 while striking goes on. */
int online_arena_stage(char *stage, int cap) {
    AcquireSRWLockExclusive(&ab_lock);
    int r = !as_on ? 0 : strcmp(as_phase, "done") ? 2 : 1;
    snprintf(stage, (size_t)cap, "%s", r ? as_stage : "");
    ReleaseSRWLockExclusive(&ab_lock);
    return r;
}

/* This player's result of the match (the set's game) it was assigned (won or lost), with its fighter
 * ("0x<hash40>/<costume>", or "": not known), to POST /arena/result: once, from a thread of its own; 0
 * when there is no match to report. Then the bracket for the tourney table, once the match is decided
 * (2 minutes at most: bracket.py takes a lone report after 60 s). */
typedef struct { char body[512], match[24]; long after; int game; } ArenaReport;
static DWORD WINAPI arena_report_thread(void *arg) {
    ArenaReport *r = arg;
    char out[2048], state[16] = "";
    int ok = http_request("POST", "/arena/result", r->body, out, sizeof out) > 0;
    fprintf(stderr, "[arena] result %s: %s -> %s\n", r->body, ok ? "sent" : "not taken", ok ? out : "");
    fflush(stderr);
    if (ok) json_member(out, "state", state, sizeof state);
    if (ok && !strcmp(state, "confirmed")) arena_table_fetch(r->after, r->match, r->game);
    else if (ok)
        for (int i = 0; i < 60; i++) {
            Sleep(2000);
            if (arena_table_fetch(r->after, r->match, r->game)) break;
        }
    free(r);
    return 0;
}
static void json_escape(char *out, int cap, const char *in) {
    int n = 0;
    for (; *in && n < cap - 7; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, (size_t)(cap - n), "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
}
int online_arena_report(int won, const char *fighter) {
    char match[24], opp[48], me[48], m[64], o[112], i[112], g[24] = "", f[48] = "";
    long after;
    int game;
    AcquireSRWLockExclusive(&ab_lock);
    snprintf(match, sizeof match, "%s", ab_match);
    snprintf(opp, sizeof opp, "%s", ab_opponent);
    after = ab_seq;
    game = ab_game;
    ReleaseSRWLockExclusive(&ab_lock);
    snprintf(me, sizeof me, "%s", net_player_name() ? net_player_name() : "");
    if (!match[0] || !opp[0] || !me[0]) return 0;
    json_escape(m, sizeof m, match);
    json_escape(o, sizeof o, opp);
    json_escape(i, sizeof i, me);
    ArenaReport *r = malloc(sizeof *r);
    if (!r) return 0;
    if (game > 0) snprintf(g, sizeof g, ", \"game\": %d", game);
    if (fighter && fighter[0]) snprintf(f, sizeof f, ", \"fighter\": \"%.30s\"", fighter);
    snprintf(r->body, sizeof r->body, "{\"match\": \"%s\", \"winner\": \"%s\", \"loser\": \"%s\", \"reporter\": \"%s\"%s%s}",
             m, won ? i : o, won ? o : i, i, g, f);
    snprintf(r->match, sizeof r->match, "%s", match);
    r->after = after;
    r->game = game;
    HANDLE t = CreateThread(NULL, 0, arena_report_thread, r, 0, NULL);
    if (!t) { free(r); return 0; }
    CloseHandle(t);
    return 1;
}
