/* Host-only mods (runtime/hle_hostmods.c) through the RomFS (runtime/hle_fs.c), with no game: the pack
 * fetched from a real lobby, opened, checked against this game's data and read through, as the game's
 * start does. tests/test_hostmods.py makes the data and the lobby, and runs it.
 *
 *   Linux: clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/hostmodstest.c runtime/linux/win32.c -lpthread -o build/hostmodstest
 *
 *   hostmodstest <data folder> <out file>
 *       SSBU_HOST_MODS and SSBU_NET_RELAY as a player sets them: the RomFS image as the game would
 *       read it, whole, to <out file>; the pack's mods (hash, name) on stdout. Exit 3 and why on
 *       stderr when the runtime would stop the game (rt_fatal). An open pack's mods are saved into
 *       <data folder>/../../mods as the game saves them (SSBU_HOST_MODS_DOWNLOAD, or the settings file
 *       HOSTMODSTEST_SETTING names: online_host_mods_download.txt's stand-in). */
#define FS_TEST
#include <stdarg.h>
#include "../../runtime/hle_fs.c"
#include "../../runtime/hle_hostmods.c"

/* ---- what hle_fs.c and hle_hostmods.c need from the rest of the runtime (as tools/dev/fstest.c) ---- */
void skyline_load_plugins(Cpu *c) { (void)c; }
void native_mods_load(void) {}
void select_screen_install(void) {}
void trace_install(void) {}
void tourney_ui_install(void) {}
int skyline_provides(const char *path) { (void)path; return 0; }
void skyline_mods_mounted(void) {}
u64 rt_symbol(const char *name) { (void)name; return 0; }
int kernel_rb_switch(void) { return 0; }
void rb_guest_writing(void *p, u64 n) { (void)p; (void)n; }
u64 rt_native(GuestFn *fn, const char *name) { (void)fn; (void)name; return 0; }
u64 rt_hook_function(u64 target, u64 replace) { (void)target; (void)replace; return 0; }
void account_save_id(u64 uid[2]) { (void)uid; }
char rt_romfs_warning[320];   /* (host.c's, not linked here: hle_fs.c fills it) */
/* (hle_config.c, hle_settings.c: the download setting's file and its first line) */
const char *config_path(enum ConfigFile file) {
    return file == CONFIG_ONLINE_HOST_MODS_DOWNLOAD && getenv("HOSTMODSTEST_SETTING") ? getenv("HOSTMODSTEST_SETTING") : "/nonexistent/setting";
}
int settings_line_read(const char *path, char *out, int size) {
    FILE *f = fopen(path, "r");
    char line[256];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#') { snprintf(out, (size_t)size, "%s", line); found = 1; }
    }
    if (f) fclose(f);
    return found;
}
const char *rt_data_dir;
const char *net_relay_address(void) { return getenv("SSBU_NET_RELAY"); }   /* (net_relay.c: the setting itself) */
void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    exit(3);
}

static void sd_print(void *ctx, const char *name, size_t len, int dir, u64 size) {
    (void)ctx;
    printf("  %s %.*s %llu\n", dir ? "dir" : "file", (int)len, name, (unsigned long long)size);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    rt_data_dir = argv[1];
    ready();
    FILE *out = fopen(argv[2], "wb");
    if (!out) return 2;
    static u8 buf[1 << 16];
    for (u64 at = 0; at < rom.size;) {
        u64 n = rom.size - at < sizeof buf ? rom.size - at : sizeof buf;
        if (!rom_read(at, buf, n)) { fprintf(stderr, "rom_read failed at %llu\n", (unsigned long long)at); return 1; }
        fwrite(buf, 1, (size_t)n, out);
        at += n;
    }
    fclose(out);
    static char names[64][256];
    u32 hashes[64];
    int n = hostmods_on() ? hostmods_mods(names, hashes, 64) : 0;
    for (int i = 0; i < n; i++) printf("%08x %s\n", hashes[i], names[i]);
    /* this system's plugins from the pack, and the host's plugin list */
    const u8 *lib, *image;
    u64 lib_len, image_len;
    const char *name;
    for (u32 i = 0; hostmods_on() && (name = hostmods_plugin(i, &lib, &lib_len, &image, &image_len)); i++)
        printf("plugin %s %.*s %.*s\n", name, (int)lib_len, (const char *)lib, (int)image_len, (const char *)image);
    const char *list = hostmods_on() ? hostmods_plugin_list() : NULL;
    if (list) printf("list %s", list);
    /* HOSTMODSTEST_SD="a;b/c": what each path under the SD card's ultimate/mods is under an ARCropolis pack
     * (hostmods_sd_entry: "sd <path> <0 none, 1 file, 2 folder> <size> <bytes>"), and a folder's entries */
    const char *sd = getenv("HOSTMODSTEST_SD");
    while (sd && hostmods_on()) {
        char path[1024];
        size_t l = strcspn(sd, ";");
        snprintf(path, sizeof path, "%.*s", (int)l, sd);
        const u8 *data = NULL;
        u64 size = 0;
        int e = hostmods_sd_entry(path, &data, &size);
        printf("sd %s %d %llu %.*s\n", path, e, (unsigned long long)size, e == 1 && size < 64 ? (int)size : 0, data ? (const char *)data : "");
        if (e == 2) hostmods_sd_list(path, sd_print, NULL);
        if (!sd[l]) break;
        sd += l + 1;
    }
    if (hostmods_on()) printf("arcropolis %d\n", hostmods_arcropolis());
    return 0;
}
