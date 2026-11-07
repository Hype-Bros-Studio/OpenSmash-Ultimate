/* A DLL loaded from memory, never from a file (hle_memdll.c; Windows x64 only). */
#ifndef MEMDLL_H
#define MEMDLL_H
#include <stddef.h>
#include <stdint.h>

typedef struct MemDll MemDll;
/* The DLL in data[0..len) mapped, linked and started; NULL, and why, if it can't be. */
MemDll *memdll_load(const uint8_t *data, size_t len, char *why, size_t why_len);
/* An exported function by name, or NULL. */
void *memdll_proc(MemDll *d, const char *name);
#endif
