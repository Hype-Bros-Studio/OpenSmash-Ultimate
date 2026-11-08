/* Console features with no counterpart here: the album's videos, data delivery, friends,
 * amiibo, play reports, the shop, SSL - plus the two small library
 * pieces that unexpectedly consult a service (language codes, secure random bytes).
 *
 * Everything reports "nothing there" or "not available" through the API's normal failure
 * paths, so the game behaves as a console that is offline, with no amiibo reader and no
 * videos in its album (its screenshots: hle_album.c). */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

#define R_UNAVAILABLE RESULT(124, 300)
#define OK { c->x[0] = 0; }
#define FAIL { c->x[0] = R_UNAVAILABLE; }
#define NOTHING { (void)c; }

/* ---- nn::settings: language codes ------------------------------------------------ */
static const char *const LANGUAGES[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                        "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};

static u64 language_code(u64 language) {
    u64 code = 0;
    if (language < sizeof LANGUAGES / sizeof *LANGUAGES) memcpy(&code, LANGUAGES[language], strlen(LANGUAGES[language]));
    return code;
}
HLE(_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE) { c->x[0] = language_code((u32)c->x[0]); }
HLE(_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE) { c->x[0] = ld64(c->x[0]) == language_code(ld32(c->x[1])); }
HLE(_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE) { c->x[0] = ld64(c->x[1]) == language_code(ld32(c->x[0])); }

/* ---- nn::crypto: the secure random source ----------------------------------------- */
int test_fixed_random(u64 buffer, u64 size, const char *who, u64 caller);   /* (hle_fight_test.c) SSBU_FIXED_SEED */
HLE(_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm) {
    if (test_fixed_random(c->x[0], c->x[1], "nn::crypto::GenerateCryptographicallyRandomBytes", c->x[30] - 4)) return;
    u8 *p = P(u8, c->x[0]);
    for (u64 i = 0; i < c->x[1]; i++) {
        unsigned int r;
        rand_s(&r);
        p[i] = (u8)r;
    }
}

/* ---- nn::album: no videos (the screenshots are hle_album.c's) ---------------------- */
HLE(_ZN2nn5album21GetExtraMovieFileListEPiPNS0_14AlbumFileEntryEi) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn5album20DeleteExtraMovieFileERKNS0_14AlbumFileEntryE) OK
HLE(_ZN2nn5album20InitializeMovieMakerEPvm) FAIL
HLE(_ZN2nn5album25PrecheckToStartMovieMakerEm) FAIL
HLE(_ZN2nn5album20OpenAlbumMovieStreamEPNS0_17MovieStreamHandleERKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album20ReadAlbumMovieStreamEPmPvmNS0_17MovieStreamHandleEl) FAIL
HLE(_ZN2nn5album23GetAlbumMovieStreamSizeEPmNS0_17MovieStreamHandleE) FAIL
HLE(_ZN2nn5album21CloseAlbumMovieStreamENS0_17MovieStreamHandleE) NOTHING

/* ---- nn::bcat: delivered data from a folder ------------------------------------------
 * SSBU_BCAT_DIR=<folder> stands for the delivery cache: <folder>/<directory>/<file>. Without it
 * the cache cannot be mounted, as on a console that never synced. Every name the game asks for is
 * logged ([bcat]) - the Online Tourney's schedule arrives this way. */
#include <dirent.h>
#include <sys/stat.h>
#define R_BCAT_NOT_FOUND RESULT(122, 1)

static const char *bcat_root(void) {
    static const char *root;
    static int done;
    if (!done) { root = getenv("SSBU_BCAT_DIR"); done = 1; if (root && !*root) root = NULL; }
    return root;
}
static void bcat_name(char *out, u64 guest) {   /* DirectoryName / FileName: char[32] */
    memcpy(out, P(char, guest), 32);
    out[32] = 0;
    for (char *p = out; *p; p++) if (*p == '/' || *p == '\\' || (p == out && *p == '.')) *p = '_';
}
static char bcat_dirs[16][33];   /* an open Directory or File keeps its slot in its first 8 bytes */
static char bcat_files[16][33];
static int bcat_slot(char names[][33], const char *name) {
    for (int i = 0; i < 16; i++) if (!names[i][0] || !strcmp(names[i], name)) { strcpy(names[i], name); return i; }
    return 0;
}

/* Online services switched on. The game asks 0x7101739120 (its service table, x0, and a
 * service id, x1) whether each Online mode is open: a param table (key, value) the game holds,
 * where 1 is on. The Online menu's tiles ask it through the Lua binding is_invalid_online_sequence
 * (0x7101bef500): Online Tourney (sequence 0x7010300) is service 0x232c, which the game's own
 * table has off - the tile shows "This service is not active". With a delivery folder the
 * services in SSBU_SERVICES_ON (hex ids, comma separated; default 232c, Online Tourney) answer
 * 1; the rest keep the game's answer. */
#define SERVICE_CHECK 0x7101739120ull
static u64 service_check_original;
static u32 services_on[16];
static int services_on_count;
static void service_check(Cpu *c) {
    u32 id = (u32)c->x[1];
    for (int i = 0; i < services_on_count; i++)
        if (services_on[i] == id) { c->x[0] = 1; return; }
    rt_call(c, service_check_original);
}
static void services_install(void) {
    static int done;
    if (done || !bcat_root()) return;
    done = 1;
    const char *list = getenv("SSBU_SERVICES_ON");
    if (!list) list = "232c";
    for (const char *p = list; *p && services_on_count < 16;) {
        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        services_on[services_on_count++] = (u32)v;
        p = *end == ',' ? end + 1 : end;
    }
    if (!services_on_count) return;
    service_check_original = rt_hook_function(SERVICE_CHECK, rt_native(service_check, "online services on"));
    fprintf(stderr, "[bcat] services on: %s%s\n", list, service_check_original ? "" : " (hook missed)");
}

HLE(_ZN2nn4bcat10InitializeEv) NOTHING
HLE(_ZN2nn4bcat25MountDeliveryCacheStorageEv) {
    fprintf(stderr, "[bcat] mount (%s)\n", bcat_root() ? bcat_root() : "none");
    services_install();
    c->x[0] = bcat_root() ? 0 : R_UNAVAILABLE;
}
HLE(_ZN2nn4bcat27UnmountDeliveryCacheStorageEv) NOTHING
HLE(_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi) {
    int n = 0, max = (int)c->x[2];
    DIR *d = bcat_root() ? opendir(bcat_root()) : NULL;
    for (struct dirent *e; d && n < max && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 31) continue;
        memset(P(char, c->x[1] + 32 * n), 0, 32);
        memcpy(P(char, c->x[1] + 32 * n), e->d_name, strlen(e->d_name));
        n++;
    }
    if (d) closedir(d);
    fprintf(stderr, "[bcat] enumerate: %d directories\n", n);
    st32(c->x[0], n);
    c->x[0] = bcat_root() ? 0 : R_UNAVAILABLE;
}
/* A sync finishes at once: the folder already is the cache. The SDK's DeliveryCacheProgress keeps
 * its state inline (status at +0x58, result at +0x5c, read by GetStatus/GetResult; Update does
 * nothing without the service object at +0x20), so the sync leaves it Done (9) and OK. */
static void bcat_sync_done(u64 progress) {
    st32(progress + 0x58, 9);
    st32(progress + 0x5c, 0);
}
HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE) {
    fprintf(stderr, "[bcat] sync\n");
    if (bcat_root()) bcat_sync_done(c->x[0]);
    c->x[0] = bcat_root() ? 0 : R_UNAVAILABLE;
}
HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressEPKc) {
    char name[33];
    bcat_name(name, c->x[1]);
    fprintf(stderr, "[bcat] sync directory %s\n", name);
    if (bcat_root()) bcat_sync_done(c->x[0]);
    c->x[0] = bcat_root() ? 0 : R_UNAVAILABLE;
}
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE) {
    char name[33], path[1024];
    struct stat st;
    bcat_name(name, c->x[1]);
    snprintf(path, sizeof path, "%s/%s", bcat_root() ? bcat_root() : "", name);
    int ok = bcat_root() && !stat(path, &st) && S_ISDIR(st.st_mode);
    fprintf(stderr, "[bcat] open directory %s%s\n", name, ok ? "" : ": not there");
    st64(c->x[0], bcat_slot(bcat_dirs, name));
    c->x[0] = ok ? 0 : R_BCAT_NOT_FOUND;
}
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi) {
    /* entry: FileName[32], s64 size, Digest[16] = 0x38 bytes */
    const char *name = bcat_dirs[ld64(c->x[0]) & 15];
    char path[1024];
    int n = 0, max = (int)c->x[3];
    snprintf(path, sizeof path, "%s/%s", bcat_root() ? bcat_root() : "", name);
    DIR *d = bcat_root() ? opendir(path) : NULL;
    for (struct dirent *e; d && n < max && (e = readdir(d));) {
        char f[1100];
        struct stat st;
        if (e->d_name[0] == '.' || strlen(e->d_name) > 31) continue;
        snprintf(f, sizeof f, "%s/%s", path, e->d_name);
        if (stat(f, &st) || !S_ISREG(st.st_mode)) continue;
        u64 at = c->x[2] + 0x38 * (u64)n;
        memset(P(char, at), 0, 0x38);
        memcpy(P(char, at), e->d_name, strlen(e->d_name));
        st64(at + 32, (u64)st.st_size);
        n++;
    }
    if (d) closedir(d);
    fprintf(stderr, "[bcat] read directory %s: %d files\n", name, n);
    st32(c->x[1], n);
    c->x[0] = d ? 0 : R_BCAT_NOT_FOUND;
}
HLE(_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv) NOTHING
HLE(_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev) NOTHING
HLE(_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE) {
    char dir[33], file[33], path[1100], both[70];
    struct stat st;
    bcat_name(dir, c->x[1]);
    bcat_name(file, c->x[2]);
    snprintf(path, sizeof path, "%s/%s/%s", bcat_root() ? bcat_root() : "", dir, file);
    int ok = bcat_root() && !stat(path, &st) && S_ISREG(st.st_mode);
    fprintf(stderr, "[bcat] open file %s/%s%s\n", dir, file, ok ? "" : ": not there");
    snprintf(both, sizeof both, "%s/%s", dir, file);
    both[32] = 0;   /* (slot names hold 32 characters: long pairs are cut, the log has them whole) */
    int slot = 0;
    for (int i = 0; i < 16; i++) if (!bcat_files[i][0] || !strcmp(bcat_files[i], both)) { slot = i; break; }
    static char full[16][70];
    strcpy(bcat_files[slot], both);
    snprintf(full[slot], sizeof full[slot], "%s/%s", dir, file);
    st64(c->x[0], slot);
    st64(c->x[0] + 8, (u64)(uintptr_t)full[slot]);   /* (host pointer kept beside the slot) */
    c->x[0] = ok ? 0 : R_BCAT_NOT_FOUND;
}
HLE(_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm) {
    const char *rel = (const char *)(uintptr_t)ld64(c->x[0] + 8);
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", bcat_root() ? bcat_root() : "", rel ? rel : "");
    FILE *f = bcat_root() && rel ? fopen(path, "rb") : NULL;
    u64 got = 0;
    if (f) {
        fseek(f, (long)c->x[2], SEEK_SET);
        got = fread(P(char, c->x[3]), 1, c->x[4], f);
        fclose(f);
    }
    fprintf(stderr, "[bcat] read %s @%llu: %llu of %llu bytes\n", rel ? rel : "?", (unsigned long long)c->x[2],
            (unsigned long long)got, (unsigned long long)c->x[4]);
    st64(c->x[1], got);
    c->x[0] = f ? 0 : R_BCAT_NOT_FOUND;
}
HLE(_ZN2nn4bcat17DeliveryCacheFile5CloseEv) NOTHING
HLE(_ZN2nn4bcat17DeliveryCacheFileD1Ev) NOTHING

/* ---- nn::friends: nobody --------------------------------------------------------- */
HLE(_ZN2nn7friends10InitializeEv) NOTHING
/* The notification queue: its event exists (the game polls it) but nothing ever arrives. */
static u64 notification_event(Cpu *c) {
    static u64 event;   /* (published once it is made: another thread may take it and use it) */
    static const u8 zero[0x80];
    u64 e = __atomic_load_n(&event, __ATOMIC_ACQUIRE);
    if (!e) {
        e = rt_guest_copy(zero, sizeof zero);
        rt_callsym(c, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb", 3, e, 0, 0);
        __atomic_store_n(&event, e, __ATOMIC_RELEASE);
    }
    return e;
}
HLE(_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE) { notification_event(c); c->x[0] = 0; }
HLE(_ZN2nn7friends17NotificationQueue14GetSystemEventEv) { c->x[0] = notification_event(c); }
HLE(_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE) FAIL
HLE(_ZN2nn7friends17NotificationQueue8FinalizeEv) NOTHING
HLE(_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_) OK
/* The token a player hands the others in a local match, for their play history (which goes
 * nowhere here). Pia needs one for every local player before it sets up a room; its contents
 * are the console's own business, so this one just names the user. */
HLE(_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb) {
    u8 *key = P(u8, c->x[0]);
    memset(key, 0, 0x40);
    key[0] = 1;                          /* type */
    key[3] = (u8)(c->x[2] & 1);          /* local play */
    memcpy(key + 0x10, P(void, c->x[1]), 16);
    c->x[0] = 0;
}
HLE(_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi) FAIL
HLE(_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi) FAIL

/* ---- nn::nfp: no amiibo reader --------------------------------------------------- */
HLE(_ZN2nn3nfp10InitializeEv) NOTHING
HLE(_ZN2nn3nfp8FinalizeEv) NOTHING
HLE(_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE) FAIL
HLE(_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp7RestoreERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE) FAIL
/* the availability, activate and deactivate events exist and never fire */
static void nfp_event(Cpu *c, u64 event) {
    rt_callsym(c, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb", 3, event, 0, 0);
    c->x[0] = 0;
}
HLE(_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp8GetStateEv) { c->x[0] = 1; }                                     /* initialized */
HLE(_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii) { st32(c->x[1], 0); c->x[0] = RESULT(115, 64); }   /* no device */
HLE(_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE) { c->x[0] = 5; }          /* unavailable */
HLE(_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE) OK
HLE(_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE) OK
HLE(_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj) FAIL
HLE(_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm) FAIL
HLE(_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm) FAIL
HLE(_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE) FAIL
HLE(_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_) FAIL

/* ---- play reports, shop, profanity filter (local wireless: hle_net.c) ----------- */
HLE(_ZN2nn5prepo10InitializeEv) NOTHING
HLE(_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE) OK
HLE(_ZN2nn5prepo28RequestImmediateTransmissionEv) OK
HLE(_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm) NOTHING
HLE(_ZN2nn3ngc15ProfanityFilter10InitializeEPvm) OK
HLE(_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj) { if (c->x[1]) st32(c->x[1], 0); c->x[0] = 0; }
HLE(_ZN2nn3ngc15ProfanityFilterD1Ev) NOTHING

/* ---- SSL: initialised, never used (no servers; sockets are in hle_net.c) ------------- */
HLE(nnsslInitialize) OK
HLE(nnsslFinalize) OK
