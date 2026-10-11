/*
 * Host test of the shader disk cache (d3d8_shcache.c): no D3D,
 * no game. Linux:
 *   cc -O2 -Wall -Wextra -pthread -I.. shcache_test.c ../d3d8_shcache.c -o shcache_test && ./shcache_test
 * Windows: cmake -DXBOX_BUILD_TESTS=ON ... ; cmake --build ... --target shcache_test
 */
#include "d3d8_shcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_dir[512];
static const char k_src[] = "float4 main(float4 p : POSITION) : SV_POSITION { return p; }";
static const unsigned char k_code[] = { 'D', 'X', 'B', 'C', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

static ShcId id_of(const char *src, const char *profile, unsigned flags)
{
    return shc_id(src, strlen(src), "t", profile, "main", flags);
}

static void entry_file(char *out, size_t n, ShcId id)
{
    snprintf(out, n, "%s/%016llx.bin", g_dir, (unsigned long long)id.key);
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void get_is(ShcId id, int want_hit)
{
    size_t n = 0;
    void *p = shc_get(id, &n);
    if (want_hit) {
        CHECK(p != NULL);
        CHECK(n == sizeof k_code);
        CHECK(p && !memcmp(p, k_code, sizeof k_code));
    } else {
        CHECK(p == NULL);
    }
    shc_free(p);
}

int main(void)
{
    ShcId id, other;
    ShcStats st;
    char path[600];
    FILE *f;

    snprintf(g_dir, sizeof g_dir, "shcache_test_%ld/cache/shaders", (long)getpid());

    /* Miss, then hit after a put, in the same session. */
    CHECK(shc_open(g_dir, 47, 1 << 20, 0));
    id = id_of(k_src, "vs_5_0", 0);
    get_is(id, 0);
    CHECK(shc_put(id, k_code, sizeof k_code));
    get_is(id, 1);
    /* The key covers the source, the profile and the flags. */
    other = id_of(k_src, "ps_5_0", 0);
    CHECK(other.key != id.key);
    get_is(other, 0);
    other = id_of(k_src, "vs_5_0", 1);
    CHECK(other.key != id.key);
    other = id_of("float4 main() : SV_TARGET { return 0; }", "vs_5_0", 0);
    CHECK(other.key != id.key);
    shc_stats(&st);
    CHECK(st.hits == 1 && st.misses == 2 && st.writes == 1 && st.bad == 0);
    shc_close();

    /* Next session: preloaded in memory, then read from disk without preload. */
    CHECK(shc_open(g_dir, 47, 1 << 20, 2));
    shc_stats(&st);
    CHECK(st.preloaded == 1);
    get_is(id, 1);
    shc_close();
    CHECK(shc_open(g_dir, 47, 1 << 20, 1));     /* background thread */
    get_is(id, 1);                              /* from memory or disk, either way a hit */
    shc_close();
    CHECK(shc_open(g_dir, 47, 1 << 20, 0));
    get_is(id, 1);
    shc_close();

    /* Another compiler build: a miss, and the old file is deleted. */
    CHECK(shc_open(g_dir, 48, 1 << 20, 0));
    id = id_of(k_src, "vs_5_0", 0);
    CHECK(id.key != 0);
    entry_file(path, sizeof path, id);
    get_is(id, 0);
    CHECK(shc_put(id, k_code, sizeof k_code));
    shc_close();
    CHECK(shc_open(g_dir, 47, 1 << 20, 0));     /* the scan drops the build-48 file */
    shc_stats(&st);
    CHECK(st.bad == 1);
    CHECK(!file_exists(path));
    shc_close();

    /* Corruption: a flipped payload byte, then a truncated file, then garbage. */
    CHECK(shc_open(g_dir, 47, 1 << 20, 0));
    id = id_of(k_src, "vs_5_0", 0);
    entry_file(path, sizeof path, id);
    CHECK(shc_put(id, k_code, sizeof k_code));
    f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) { fseek(f, 48 + 3, SEEK_SET); fputc('Z', f); fclose(f); }
    get_is(id, 0);
    CHECK(!file_exists(path));                  /* deleted: compiled again next time */
    CHECK(shc_put(id, k_code, sizeof k_code));
    get_is(id, 1);
    f = fopen(path, "wb");
    if (f) { fwrite("XSHC", 1, 4, f); fclose(f); }
    get_is(id, 0);
    CHECK(!file_exists(path));
    f = fopen(path, "wb");
    if (f) { fwrite(k_code, 1, sizeof k_code, f); fwrite(k_code, 1, sizeof k_code, f);
             fwrite(k_code, 1, sizeof k_code, f); fwrite(k_code, 1, sizeof k_code, f); fclose(f); }
    get_is(id, 0);
    shc_stats(&st);
    CHECK(st.bad == 3);
    /* A file under the right name holding another shader (a key collision). */
    other = id_of("other", "vs_5_0", 0);
    CHECK(shc_put(other, k_code, sizeof k_code));
    {
        char from[600];
        entry_file(from, sizeof from, other);
        CHECK(rename(from, path) == 0);
    }
    get_is(id, 0);
    shc_close();

    /* A leftover temporary file of another run is deleted at start. */
    snprintf(path, sizeof path, "%s/0000000000000001.999999999.1.tmp", g_dir);
    f = fopen(path, "wb");
    if (f) fclose(f);
    CHECK(shc_open(g_dir, 47, 1 << 20, 0));
    CHECK(!file_exists(path));
    shc_close();

    /* The bound: past it, nothing more is written. */
    CHECK(shc_open(g_dir, 47, 3 * (48 + sizeof k_code), 0));
    {
        char src[32];
        int i, written = 0;
        for (i = 0; i < 6; i++) {
            snprintf(src, sizeof src, "bound %d", i);
            written += shc_put(id_of(src, "vs_5_0", 0), k_code, sizeof k_code);
        }
        shc_stats(&st);
        CHECK(written == 3);
        CHECK(st.skipped == 3);
        CHECK(st.disk_bytes <= 3 * (48 + sizeof k_code));
    }
    shc_close();

    /* XBOX_SHADER_CACHE: on unless "0". */
    CHECK(shc_env_on() == (getenv("XBOX_SHADER_CACHE") == NULL || getenv("XBOX_SHADER_CACHE")[0] != '0'));

    printf("shcache_test: %d checks, %d failed (folder %s)\n", g_n, g_fail, g_dir);
    return g_fail != 0;
}
