/**
 * Disk cache of compiled shader bytecode (XBOX_SHADER_CACHE).
 *
 * The NV2A draw path compiles a vertex program or a combiner pixel shader
 * the first time it sees one, synchronously on the pump (d3d8_nv2a.c). This
 * keeps the bytecode on disk, one file per shader, so a later session reads
 * it back instead of calling D3DCompile. The key covers the HLSL source, the
 * profile, the entry point, the compile flags and the d3dcompiler build; a
 * file that does not match it in every field (or whose payload checksum
 * fails) is deleted and the shader compiled again, silently.
 *
 * XBOX_SHADER_CACHE      1 (default) on; 0 = the previous path, no file touched.
 * XBOX_SHADER_CACHE_DIR  the folder (the launcher sets <settings.ini folder>\cache\shaders).
 * XBOX_SHADER_CACHE_MB   bound of the folder, in MB (default 64): past it, nothing more is written.
 * XBOX_SHADER_CACHE_PRELOAD  1 (default): a background thread reads the folder at start
 *                        into memory, so a hit during a race does not wait on the disk.
 *
 * Plain C with no D3D type: the host test (tests/shcache_test.c) builds it on Linux.
 */
#ifndef D3D8_SHCACHE_H
#define D3D8_SHCACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t key, check; } ShcId;

/* XBOX_SHADER_CACHE: 1 unless it is "0". */
int shc_env_on(void);

/* Open the cache. dir NULL = XBOX_SHADER_CACHE_DIR (or "cache/shaders");
 * max_bytes 0 = XBOX_SHADER_CACHE_MB; preload: 0 none, 1 background thread,
 * 2 at once (tests). compiler_id names the compiler build. 1 = ready. */
int shc_open(const char *dir, uint64_t compiler_id, size_t max_bytes, int preload);
void shc_close(void);           /* joins the preload thread, frees the memory copy */

ShcId shc_id(const char *src, size_t len, const char *name, const char *profile,
             const char *entry, uint32_t flags);
/* The bytecode of id (malloc'd, shc_free) or NULL: miss, or a bad file (deleted). */
void *shc_get(ShcId id, size_t *size);
/* Atomic write (temporary file, then rename). 1 = written. */
int shc_put(ShcId id, const void *code, size_t size);
void shc_free(void *p);

typedef struct {
    long hits, misses, bad, writes, skipped;   /* skipped: over the bound */
    long preloaded;
    uint64_t disk_bytes;
} ShcStats;
void shc_stats(ShcStats *out);

#endif
