/**
 * Disk cache of compiled shader bytecode (see d3d8_shcache.h).
 *
 * One file per shader, "<key>.bin": a 48-byte header (magic, format, key,
 * second source hash, compiler build, payload size and checksum) and the
 * bytecode. A write goes to "<key>.<pid>.<n>.tmp" and is renamed over the
 * final name, so a crash leaves at worst a temporary file, which the next
 * start deletes. Everything shared is under one lock: the pump's lookups,
 * its writes and the preload thread.
 */
#include "d3d8_shcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define SHC_SEP '\\'
typedef CRITICAL_SECTION shc_lock_t;
static void lk_init(shc_lock_t *l) { InitializeCriticalSection(l); }
static void lk_free(shc_lock_t *l) { DeleteCriticalSection(l); }
static void lk(shc_lock_t *l) { EnterCriticalSection(l); }
static void ulk(shc_lock_t *l) { LeaveCriticalSection(l); }
static unsigned long shc_pid(void) { return (unsigned long)GetCurrentProcessId(); }
static void shc_mkdir(const char *p) { CreateDirectoryA(p, NULL); }
static int shc_rename(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define SHC_SEP '/'
typedef pthread_mutex_t shc_lock_t;
static void lk_init(shc_lock_t *l) { pthread_mutex_init(l, NULL); }
static void lk_free(shc_lock_t *l) { pthread_mutex_destroy(l); }
static void lk(shc_lock_t *l) { pthread_mutex_lock(l); }
static void ulk(shc_lock_t *l) { pthread_mutex_unlock(l); }
static unsigned long shc_pid(void) { return (unsigned long)getpid(); }
static void shc_mkdir(const char *p) { mkdir(p, 0777); }
static int shc_rename(const char *from, const char *to) { return rename(from, to); }
#endif

#define SHC_MAGIC   0x43485358u     /* "XSHC" */
#define SHC_FORMAT  1u
#define SHC_MEM     8192            /* slots of the memory copy (power of two) */
#define SHC_PATH    1024
#define SHC_DIR     960             /* the folder, with room for a file name */

typedef struct {
    uint32_t magic, format;
    uint64_t key, check, compiler;
    uint32_t size, sum;
    uint64_t reserved;
} ShcHeader;                        /* 48 bytes */

typedef struct { uint64_t key, check; size_t size; void *data; } ShcSlot;

static struct {
    int open;
    char dir[SHC_DIR];
    uint64_t compiler;
    size_t max_bytes;
    shc_lock_t lock;
    ShcSlot mem[SHC_MEM];
    size_t mem_bytes;
    unsigned long tmp_n;
    int told_full;
    ShcStats st;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
    int has_thread;
#endif
} S;

/* ---- hashes ------------------------------------------------------------ */

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

/* A second, unrelated hash: a collision of the key must not also match it. */
static uint64_t mix64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) { h = (h + *b++) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}

static uint32_t sum32(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static uint64_t both(uint64_t (*f)(uint64_t, const void *, size_t), uint64_t h,
                     const char *src, size_t len, const char *name, const char *profile,
                     const char *entry, uint32_t flags, uint64_t compiler)
{
    uint32_t fmt = SHC_FORMAT;
    uint64_t l = (uint64_t)len;
    h = f(h, &fmt, sizeof fmt);
    h = f(h, &compiler, sizeof compiler);
    h = f(h, name, strlen(name) + 1);
    h = f(h, profile, strlen(profile) + 1);
    h = f(h, entry, strlen(entry) + 1);
    h = f(h, &flags, sizeof flags);
    h = f(h, &l, sizeof l);
    return f(h, src, len);
}

ShcId shc_id(const char *src, size_t len, const char *name, const char *profile,
             const char *entry, uint32_t flags)
{
    ShcId id;
    id.key = both(fnv64, 0xCBF29CE484222325ull, src, len, name ? name : "", profile, entry,
                  flags, S.compiler);
    id.check = both(mix64, 0x243F6A8885A308D3ull, src, len, name ? name : "", profile, entry,
                    flags, S.compiler);
    return id;
}

/* ---- files -------------------------------------------------------------- */

static void entry_path(char *out, uint64_t key)
{
    snprintf(out, SHC_PATH, "%s%c%016llx.bin", S.dir, SHC_SEP, (unsigned long long)key);
}

/* The bytecode of the file at path if it is a valid entry (key 0 = any key),
 * else NULL; *bad = 1 when the file exists but is not valid. */
static void *read_entry(const char *path, uint64_t key, uint64_t check, size_t *size,
                        uint64_t *key_out, uint64_t *check_out, int *bad)
{
    FILE *f = fopen(path, "rb");
    ShcHeader h;
    void *data = NULL;
    long flen;

    *bad = 0;
    if (!f) return NULL;
    *bad = 1;
    if (fseek(f, 0, SEEK_END) != 0 || (flen = ftell(f)) < (long)sizeof h || fseek(f, 0, SEEK_SET) != 0)
        goto out;
    if (fread(&h, sizeof h, 1, f) != 1) goto out;
    if (h.magic != SHC_MAGIC || h.format != SHC_FORMAT || h.compiler != S.compiler) goto out;
    if (key && (h.key != key || h.check != check)) goto out;
    if (h.size == 0 || (long)h.size != flen - (long)sizeof h) goto out;
    data = malloc(h.size);
    if (!data) { *bad = 0; goto out; }
    if (fread(data, 1, h.size, f) != h.size || sum32(data, h.size) != h.sum) {
        free(data);
        data = NULL;
        goto out;
    }
    *bad = 0;
    *size = h.size;
    if (key_out) *key_out = h.key;
    if (check_out) *check_out = h.check;
out:
    fclose(f);
    return data;
}

/* ---- memory copy (preload) ------------------------------------------------ */

static ShcSlot *mem_slot(uint64_t key, int insert)
{
    unsigned i = (unsigned)(key ^ (key >> 32)) & (SHC_MEM - 1), n;
    for (n = 0; n < SHC_MEM; n++, i = (i + 1) & (SHC_MEM - 1)) {
        if (S.mem[i].key == key && (S.mem[i].data || !insert)) return &S.mem[i];
        if (!S.mem[i].key) return insert ? &S.mem[i] : NULL;
    }
    return NULL;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* One file of the folder at start: a leftover temporary file of another run
 * or an invalid entry (another compiler, damaged) is deleted; a valid one is
 * counted and, with load, kept in memory within the bound. */
static void scan_one(const char *name, int load)
{
    char path[SHC_DIR + 320], mine[32];
    size_t size = 0;
    uint64_t key = 0, check = 0;
    int bad;
    void *data;

    snprintf(path, sizeof path, "%s%c%s", S.dir, SHC_SEP, name);
    if (has_suffix(name, ".tmp")) {
        snprintf(mine, sizeof mine, ".%lu.", shc_pid());
        if (!strstr(name, mine)) remove(path);
        return;
    }
    if (!has_suffix(name, ".bin")) return;
    data = read_entry(path, 0, 0, &size, &key, &check, &bad);
    if (!data) {
        if (bad) { remove(path); lk(&S.lock); S.st.bad++; ulk(&S.lock); }
        return;
    }
    lk(&S.lock);
    S.st.disk_bytes += size + sizeof(ShcHeader);
    if (load && key && S.mem_bytes + size <= S.max_bytes) {
        ShcSlot *sl = mem_slot(key, 1);
        if (sl && !sl->data) {
            sl->key = key; sl->check = check; sl->size = size; sl->data = data;
            S.mem_bytes += size;
            S.st.preloaded++;
            data = NULL;
        }
    }
    ulk(&S.lock);
    free(data);
}

static void scan(int load)
{
#ifdef _WIN32
    char pat[SHC_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", S.dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) scan_one(fd.cFileName, load);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(S.dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') scan_one(e->d_name, load);
    closedir(d);
#endif
}

#ifdef _WIN32
static DWORD WINAPI preload_thread(LPVOID arg) { (void)arg; scan(1); return 0; }
#else
static void *preload_thread(void *arg) { (void)arg; scan(1); return NULL; }
#endif

/* ---- interface -------------------------------------------------------------- */

int shc_env_on(void)
{
    const char *e = getenv("XBOX_SHADER_CACHE");
    return !(e && e[0] == '0');
}

static void make_dirs(char *p)
{
    char *s;
    for (s = p + 1; *s; s++) {
        if ((*s == '/' || *s == '\\') && s[-1] != ':') {
            char c = *s;
            *s = '\0';
            shc_mkdir(p);
            *s = c;
        }
    }
    shc_mkdir(p);
}

int shc_open(const char *dir, uint64_t compiler_id, size_t max_bytes, int preload)
{
    const char *e;
    size_t n;
    if (S.open) return 1;
    memset(&S.st, 0, sizeof S.st);
    if (!dir) dir = getenv("XBOX_SHADER_CACHE_DIR");
    if (!dir || !dir[0]) dir = "cache" "/" "shaders";
    if (strlen(dir) >= sizeof S.dir) return 0;
    snprintf(S.dir, sizeof S.dir, "%s", dir);
    n = strlen(S.dir);
    while (n > 1 && (S.dir[n - 1] == '/' || S.dir[n - 1] == '\\')) S.dir[--n] = '\0';
    if (!max_bytes) {
        long mb = (e = getenv("XBOX_SHADER_CACHE_MB")) ? atol(e) : 64;
        if (mb <= 0) mb = 64;
        max_bytes = (size_t)mb << 20;
    }
    S.max_bytes = max_bytes;
    S.compiler = compiler_id;
    S.mem_bytes = 0;
    S.told_full = 0;
    memset(S.mem, 0, sizeof S.mem);
    make_dirs(S.dir);
    lk_init(&S.lock);
    S.open = 1;
    if (preload == 1 && (e = getenv("XBOX_SHADER_CACHE_PRELOAD")) && e[0] == '0') preload = 0;
#ifdef _WIN32
    S.thread = NULL;
    if (preload == 1) {
        S.thread = CreateThread(NULL, 0, preload_thread, NULL, 0, NULL);
        if (S.thread) SetThreadPriority(S.thread, THREAD_PRIORITY_BELOW_NORMAL);
        else scan(1);
    }
#else
    S.has_thread = 0;
    if (preload == 1) {
        S.has_thread = pthread_create(&S.thread, NULL, preload_thread, NULL) == 0;
        if (!S.has_thread) scan(1);
    }
#endif
    if (preload == 2) scan(1);
    if (preload == 0) scan(0);      /* the count and the cleanup, nothing kept */
    return 1;
}

void shc_close(void)
{
    int i;
    if (!S.open) return;
#ifdef _WIN32
    if (S.thread) { WaitForSingleObject(S.thread, INFINITE); CloseHandle(S.thread); S.thread = NULL; }
#else
    if (S.has_thread) { pthread_join(S.thread, NULL); S.has_thread = 0; }
#endif
    for (i = 0; i < SHC_MEM; i++) free(S.mem[i].data);
    memset(S.mem, 0, sizeof S.mem);
    lk_free(&S.lock);
    S.open = 0;
}

void *shc_get(ShcId id, size_t *size)
{
    char path[SHC_PATH];
    void *data = NULL;
    int bad = 0;
    ShcSlot *sl;

    if (!S.open || !id.key) return NULL;
    lk(&S.lock);
    sl = mem_slot(id.key, 0);
    if (sl && sl->data && sl->check == id.check) {
        data = sl->data;                    /* handed over: each shader is asked for once */
        *size = sl->size;
        S.mem_bytes -= sl->size;
        sl->data = NULL;
        sl->size = 0;
        S.st.hits++;
    }
    ulk(&S.lock);
    if (data) return data;

    entry_path(path, id.key);
    data = read_entry(path, id.key, id.check, size, NULL, NULL, &bad);
    if (bad) remove(path);
    lk(&S.lock);
    if (data) S.st.hits++;
    else { S.st.misses++; if (bad) S.st.bad++; }
    ulk(&S.lock);
    return data;
}

int shc_put(ShcId id, const void *code, size_t size)
{
    char tmp[SHC_PATH], path[SHC_PATH];
    ShcHeader h;
    FILE *f;
    unsigned long n;
    int ok;

    if (!S.open || !id.key || !code || !size || size > 0xFFFFFFFFu) return 0;
    lk(&S.lock);
    if (S.st.disk_bytes + size + sizeof h > S.max_bytes) {
        S.st.skipped++;
        if (!S.told_full) {
            S.told_full = 1;
            fprintf(stderr, "[SHC] %s is full (%llu KB): no more shaders written\n",
                    S.dir, (unsigned long long)(S.st.disk_bytes >> 10));
        }
        ulk(&S.lock);
        return 0;
    }
    n = ++S.tmp_n;
    ulk(&S.lock);

    memset(&h, 0, sizeof h);
    h.magic = SHC_MAGIC;
    h.format = SHC_FORMAT;
    h.key = id.key;
    h.check = id.check;
    h.compiler = S.compiler;
    h.size = (uint32_t)size;
    h.sum = sum32(code, size);
    snprintf(tmp, sizeof tmp, "%s%c%016llx.%lu.%lu.tmp", S.dir, SHC_SEP,
             (unsigned long long)id.key, shc_pid(), n);
    entry_path(path, id.key);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(code, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok || shc_rename(tmp, path) != 0) { remove(tmp); return 0; }
    lk(&S.lock);
    S.st.writes++;
    S.st.disk_bytes += size + sizeof h;
    ulk(&S.lock);
    return 1;
}

void shc_free(void *p) { free(p); }

void shc_stats(ShcStats *out)
{
    if (!S.open) { memset(out, 0, sizeof *out); return; }
    lk(&S.lock);
    *out = S.st;
    ulk(&S.lock);
}
