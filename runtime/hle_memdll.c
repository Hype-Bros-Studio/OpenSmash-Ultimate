/* A DLL loaded from memory (Windows x64): for host-only mods' plugins (hle_hostmods.c), whose
 * translated code comes in the host's sealed pack and is never written to a file. What the
 * system's loader does for a plugin DLL (tools/skyline.py cache_mod_plugins builds them), by hand:
 * the sections mapped, relocated, the imports bound (by LoadLibrary / GetProcAddress, the system's
 * own DLLs), each section's protection, the unwind tables registered (C++ exceptions and SEH run
 * through the plugin's code) and the entry point run. A DLL with thread-local data is refused (the
 * system's loader alone can give it a TLS slot); TLS callbacks alone are run.
 *
 * (Not an HLE of anything: named hle_ so every build's runtime file pattern takes it. Nothing on
 * other systems yet: their builds have no plugin libraries.) */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "memdll.h"

struct MemDll {
    u8 *base;
    size_t size;
    IMAGE_NT_HEADERS64 *nt;
};

#define DIR(nt, k) ((nt)->OptionalHeader.DataDirectory[k])

static int in_image(const struct MemDll *d, u64 rva, u64 n) { return rva <= d->size && n <= d->size - rva; }

static DWORD protection(DWORD ch) {
    int x = !!(ch & IMAGE_SCN_MEM_EXECUTE), r = !!(ch & IMAGE_SCN_MEM_READ), w = !!(ch & IMAGE_SCN_MEM_WRITE);
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
}

MemDll *memdll_load(const u8 *data, size_t len, char *why, size_t why_len) {
#define FAIL(...) do { snprintf(why, why_len, __VA_ARGS__); goto fail; } while (0)
    struct MemDll *d = calloc(1, sizeof *d);
    if (!d) { snprintf(why, why_len, "no memory"); return NULL; }
    const IMAGE_DOS_HEADER *dos = (const void *)data;
    if (len < sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE || (u64)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > len)
        FAIL("not a DLL");
    const IMAGE_NT_HEADERS64 *nt = (const void *)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL))
        FAIL("not an x64 DLL");
    const IMAGE_OPTIONAL_HEADER64 *oh = &nt->OptionalHeader;
    if (oh->SizeOfHeaders > len || oh->SizeOfHeaders > oh->SizeOfImage) FAIL("a DLL of the wrong size");
    d->size = oh->SizeOfImage;
    d->base = VirtualAlloc((void *)oh->ImageBase, d->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!d->base) d->base = VirtualAlloc(NULL, d->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!d->base) FAIL("no memory for its %zu bytes", d->size);
    memcpy(d->base, data, oh->SizeOfHeaders);
    d->nt = (IMAGE_NT_HEADERS64 *)(d->base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(d->nt);
    u32 nsec = d->nt->FileHeader.NumberOfSections;
    if ((u8 *)(sec + nsec) > d->base + oh->SizeOfHeaders) FAIL("a DLL of the wrong size");
    for (u32 i = 0; i < nsec; i++) {
        u32 raw = sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize;
        if (!raw) continue;
        if ((u64)sec[i].PointerToRawData + raw > len || !in_image(d, sec[i].VirtualAddress, raw)) FAIL("a section out of bounds");
        memcpy(d->base + sec[i].VirtualAddress, data + sec[i].PointerToRawData, raw);
    }

    /* relocations */
    i64 delta = (i64)((u64)d->base - oh->ImageBase);
    IMAGE_DATA_DIRECTORY rd = DIR(d->nt, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (delta && rd.Size) {
        if (!in_image(d, rd.VirtualAddress, rd.Size)) FAIL("relocations out of bounds");
        for (u32 at = 0; at + sizeof(IMAGE_BASE_RELOCATION) <= rd.Size;) {
            IMAGE_BASE_RELOCATION *b = (void *)(d->base + rd.VirtualAddress + at);
            if (b->SizeOfBlock < sizeof *b || at + b->SizeOfBlock > rd.Size) FAIL("bad relocations");
            WORD *e = (WORD *)(b + 1);
            for (u32 k = 0; k < (b->SizeOfBlock - sizeof *b) / 2; k++) {
                u32 type = e[k] >> 12, off = b->VirtualAddress + (e[k] & 0xfff);
                if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                if (type != IMAGE_REL_BASED_DIR64 || !in_image(d, off, 8)) FAIL("a relocation of type %u", type);
                *(u64 *)(d->base + off) += (u64)delta;
            }
            at += b->SizeOfBlock;
        }
    } else if (delta) {
        FAIL("it can't move from its address and that is taken");
    }

    /* imports: the system's DLLs */
    IMAGE_DATA_DIRECTORY id = DIR(d->nt, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (id.Size) {
        if (!in_image(d, id.VirtualAddress, id.Size)) FAIL("imports out of bounds");
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (void *)(d->base + id.VirtualAddress); imp->Name; imp++) {
            if (!in_image(d, imp->Name, 1) || !in_image(d, imp->FirstThunk, 8)) FAIL("imports out of bounds");
            const char *lib_name = (const char *)(d->base + imp->Name);
            HMODULE lib = LoadLibraryA(lib_name);
            if (!lib) FAIL("it needs %s, which didn't load (error %lu)", lib_name, (unsigned long)GetLastError());
            u64 *iat = (u64 *)(d->base + imp->FirstThunk);
            const u64 *names = (const u64 *)(d->base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (; *names; names++, iat++) {
                FARPROC fn;
                if (IMAGE_SNAP_BY_ORDINAL64(*names)) fn = GetProcAddress(lib, (LPCSTR)(uintptr_t)IMAGE_ORDINAL64(*names));
                else {
                    if (!in_image(d, (u32)*names + 2, 1)) FAIL("imports out of bounds");
                    fn = GetProcAddress(lib, ((IMAGE_IMPORT_BY_NAME *)(d->base + (u32)*names))->Name);
                }
                if (!fn) FAIL("%s has no function it needs", lib_name);
                *iat = (u64)fn;
            }
        }
    }
    if (DIR(d->nt, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT).Size) FAIL("it delay-loads DLLs");

    /* thread-local data: the system's loader only */
    IMAGE_DATA_DIRECTORY td = DIR(d->nt, IMAGE_DIRECTORY_ENTRY_TLS);
    IMAGE_TLS_DIRECTORY64 *tls = NULL;
    if (td.Size) {
        if (!in_image(d, td.VirtualAddress, sizeof *tls)) FAIL("TLS out of bounds");
        tls = (void *)(d->base + td.VirtualAddress);
        if (tls->EndAddressOfRawData != tls->StartAddressOfRawData || tls->SizeOfZeroFill)
            FAIL("it has thread-local data (only the system's loader can load it)");
    }

    /* each section's protection */
    for (u32 i = 0; i < nsec; i++) {
        u32 vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        DWORD old;
        if (vs && sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;
        if (vs && !VirtualProtect(d->base + sec[i].VirtualAddress, vs, protection(sec[i].Characteristics), &old))
            FAIL("its sections' protection (error %lu)", (unsigned long)GetLastError());
    }
    DWORD old;
    VirtualProtect(d->base, oh->SizeOfHeaders, PAGE_READONLY, &old);
    FlushInstructionCache(GetCurrentProcess(), d->base, d->size);

    /* unwind tables: exceptions through its code */
    IMAGE_DATA_DIRECTORY xd = DIR(d->nt, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (xd.Size && in_image(d, xd.VirtualAddress, xd.Size) &&
        !RtlAddFunctionTable((PRUNTIME_FUNCTION)(d->base + xd.VirtualAddress), xd.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)d->base))
        FAIL("its unwind tables weren't taken");

    if (tls && tls->AddressOfCallBacks)
        for (PIMAGE_TLS_CALLBACK *cb = (PIMAGE_TLS_CALLBACK *)(tls->AddressOfCallBacks + (u64)delta); *cb; cb++)
            (*cb)(d->base, DLL_PROCESS_ATTACH, NULL);
    if (oh->AddressOfEntryPoint) {
        BOOL(WINAPI *entry)(HINSTANCE, DWORD, LPVOID) = (void *)(d->base + oh->AddressOfEntryPoint);
        if (!entry((HINSTANCE)d->base, DLL_PROCESS_ATTACH, NULL)) FAIL("its start failed");
    }
    return d;
fail:
    if (d->base) VirtualFree(d->base, 0, MEM_RELEASE);
    free(d);
    return NULL;
#undef FAIL
}

void *memdll_proc(MemDll *d, const char *name) {
    IMAGE_DATA_DIRECTORY ed = DIR(d->nt, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!ed.Size || !in_image(d, ed.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return NULL;
    IMAGE_EXPORT_DIRECTORY *ex = (void *)(d->base + ed.VirtualAddress);
    if (!in_image(d, ex->AddressOfNames, 4ull * ex->NumberOfNames) || !in_image(d, ex->AddressOfNameOrdinals, 2ull * ex->NumberOfNames) ||
        !in_image(d, ex->AddressOfFunctions, 4ull * ex->NumberOfFunctions))
        return NULL;
    const u32 *names = (const u32 *)(d->base + ex->AddressOfNames);
    const u16 *ords = (const u16 *)(d->base + ex->AddressOfNameOrdinals);
    const u32 *fns = (const u32 *)(d->base + ex->AddressOfFunctions);
    for (u32 i = 0; i < ex->NumberOfNames; i++)
        if (in_image(d, names[i], 1) && !strcmp((const char *)(d->base + names[i]), name) && ords[i] < ex->NumberOfFunctions)
            return d->base + fns[ords[i]];
    return NULL;
}
#endif
