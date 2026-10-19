/*
 * ot_posix.c -- the Win32 calls of the game's host code on Linux and the
 * Switch (declarations and conventions: compat/ot_posix.h).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "compat/winsock2.h"

static int    s_argc;
static char **s_argv;

/* ── Character sets ─────────────────────────────────────────────────── */

/* UTF-8 <-> wchar_t (UTF-32 here). Win32 semantics: n = -1 means "through
 * the NUL, which is counted"; dst_n = 0 asks for the size. */
int ot_MultiByteToWideChar(unsigned cp, unsigned long flags, const char *src, int n,
                           wchar_t *dst, int dst_n)
{
    const unsigned char *s = (const unsigned char *)src;
    const unsigned char *end;
    int out = 0;
    (void)cp; (void)flags;
    if (!src) return 0;
    end = s + (n < 0 ? strlen(src) + 1 : (size_t)n);
    while (s < end) {
        uint32_t c = *s++;
        int more = 0;
        if (c >= 0xF0 && c < 0xF8) { c &= 0x07; more = 3; }
        else if (c >= 0xE0)        { c &= 0x0F; more = 2; }
        else if (c >= 0xC0)        { c &= 0x1F; more = 1; }
        else if (c >= 0x80)        { c = 0xFFFD; }
        while (more-- > 0) {
            if (s >= end || (*s & 0xC0) != 0x80) { c = 0xFFFD; break; }
            c = (c << 6) | (*s++ & 0x3F);
        }
        if (dst_n) {
            if (out >= dst_n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            dst[out] = (wchar_t)c;
        }
        out++;
    }
    return out;
}

int ot_WideCharToMultiByte(unsigned cp, unsigned long flags, const wchar_t *src, int n,
                           char *dst, int dst_n, const char *def, int *used_def)
{
    const wchar_t *end;
    int out = 0;
    (void)cp; (void)flags; (void)def;
    if (used_def) *used_def = 0;
    if (!src) return 0;
    end = src + (n < 0 ? wcslen(src) + 1 : (size_t)n);
    for (; src < end; src++) {
        uint32_t c = (uint32_t)*src;
        unsigned char b[4];
        int k, len;
        if (c < 0x80)         { b[0] = (unsigned char)c; len = 1; }
        else if (c < 0x800)   { b[0] = (unsigned char)(0xC0 | (c >> 6)); b[1] = (unsigned char)(0x80 | (c & 0x3F)); len = 2; }
        else if (c < 0x10000) { b[0] = (unsigned char)(0xE0 | (c >> 12)); b[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
                                b[2] = (unsigned char)(0x80 | (c & 0x3F)); len = 3; }
        else                  { b[0] = (unsigned char)(0xF0 | (c >> 18)); b[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
                                b[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (c & 0x3F)); len = 4; }
        if (dst_n) {
            if (out + len > dst_n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            for (k = 0; k < len; k++) dst[out + k] = (char)b[k];
        }
        out += len;
    }
    return out;
}

static void to_u8(const wchar_t *w, char *out, size_t n)
{
    if (!ot_WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) && n) out[0] = 0;
}

static void to_w(const char *s, wchar_t *out, size_t n)
{
    if (!ot_MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)n) && n) out[0] = 0;
}

/* ── Paths ──────────────────────────────────────────────────────────── */

char *ot_path_slashes(char *p)
{
    char *q;
    for (q = p; *q; q++) if (*q == '\\') *q = '/';
    return p;
}

/* "/x" or "sdmc:/x" (any "device:" prefix). */
static int is_rooted(const char *p)
{
    const char *c;
    if (p[0] == '/') return 1;
    c = strchr(p, ':');
    return c && c > p && (c[1] == '/' || c[1] == '\\' || c[1] == 0);
}

/* Absolute, '/'-separated, "." and ".." collapsed, no trailing '/'. */
static int full_path(const char *in, char *out, size_t out_sz)
{
    char tmp[4096], *seg[256], *save = NULL, *t;
    size_t prefix = 0;
    int nseg = 0, i;
    if (!in || !in[0]) return 0;
    if (is_rooted(in)) {
        snprintf(tmp, sizeof tmp, "%s", in);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    }
    ot_path_slashes(tmp);
    if (tmp[0] != '/') {                          /* "sdmc:" */
        char *c = strchr(tmp, ':');
        prefix = (size_t)(c - tmp) + 1;
    }
    for (t = strtok_r(tmp + prefix, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (nseg) nseg--; continue; }
        if (nseg < 256) seg[nseg++] = t;
    }
    {
        size_t len = 0;
        char buf[4096];
        if (prefix) { memcpy(buf, tmp, prefix); len = prefix; }
        if (!nseg) buf[len++] = '/';
        for (i = 0; i < nseg; i++) {
            size_t k = strlen(seg[i]);
            if (len + k + 2 >= sizeof buf) return 0;
            buf[len++] = '/';
            memcpy(buf + len, seg[i], k);
            len += k;
        }
        buf[len] = 0;
        if (len + 1 > out_sz) return 0;
        memcpy(out, buf, len + 1);
        return (int)len;
    }
}

DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file_part)
{
    char buf[4096];
    int len = full_path(path, buf, sizeof buf);
    if (!len) return 0;
    if ((DWORD)len + 1 > n) return (DWORD)len + 1;
    memcpy(out, buf, (size_t)len + 1);
    if (file_part) { char *s = strrchr(out, '/'); *file_part = s ? s + 1 : out; }
    return (DWORD)len;
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR out, LPWSTR *file_part)
{
    char in[4096], buf[4096];
    wchar_t w[4096];
    size_t len;
    to_u8(path, in, sizeof in);
    if (!full_path(in, buf, sizeof buf)) return 0;
    to_w(buf, w, 4096);
    len = wcslen(w);
    if (len + 1 > n) return (DWORD)len + 1;
    memcpy(out, w, (len + 1) * sizeof *w);
    if (file_part) { wchar_t *s = wcsrchr(out, L'/'); *file_part = s ? s + 1 : out; }
    return (DWORD)len;
}

DWORD GetShortPathNameW(LPCWSTR path, LPWSTR out, DWORD n)
{
    size_t len = wcslen(path);
    if (len + 1 > n) return (DWORD)len + 1;
    memcpy(out, path, (len + 1) * sizeof *path);
    return (DWORD)len;
}

/* A wide Win32 path as a host path. */
static void host_path_w(LPCWSTR w, char *out, size_t n)
{
    to_u8(w, out, n);
    ot_path_slashes(out);
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    char p[4096];
    struct stat st;
    host_path_w(path, p, sizeof p);
    if (stat(p, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL CreateDirectoryW(LPCWSTR path, void *sa)
{
    char p[4096];
    (void)sa;
    host_path_w(path, p, sizeof p);
    if (mkdir(p, 0755) == 0) return TRUE;
    SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND);
    return FALSE;
}

int SHCreateDirectoryExW(HWND owner, LPCWSTR path, void *sa)
{
    char p[4096], *q;
    struct stat st;
    (void)owner; (void)sa;
    host_path_w(path, p, sizeof p);
    if (stat(p, &st) == 0) return S_ISDIR(st.st_mode) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
    for (q = p + 1; *q; q++) {
        if (*q != '/' || q[-1] == ':') continue;
        *q = 0;
        mkdir(p, 0755);
        *q = '/';
    }
    return (mkdir(p, 0755) == 0 || errno == EEXIST) ? ERROR_SUCCESS : ERROR_PATH_NOT_FOUND;
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    char a[4096], b[4096], buf[65536];
    FILE *in, *out;
    size_t k;
    BOOL ok = TRUE;
    struct stat st;
    host_path_w(from, a, sizeof a);
    host_path_w(to, b, sizeof b);
    if (fail_if_exists && stat(b, &st) == 0) { SetLastError(ERROR_FILE_EXISTS); return FALSE; }
    if (!(in = fopen(a, "rb"))) return FALSE;
    if (!(out = fopen(b, "wb"))) { fclose(in); return FALSE; }
    while ((k = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, k, out) != k) { ok = FALSE; break; }
    fclose(in);
    if (fclose(out) != 0) ok = FALSE;
    return ok;
}

/* Case-insensitive '*' and '?' match, as FindFirstFile does. */
static int wild_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (pat[1] == '*') pat++;
            if (!pat[1]) return 1;
            for (; *s; s++) if (wild_match(pat + 1, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

typedef struct OtFind {
    DIR  *dir;
    char  base[4096];
    char  pattern[256];
} OtFind;

static BOOL find_next(OtFind *f, WIN32_FIND_DATAW *fd)
{
    struct dirent *e;
    while ((e = readdir(f->dir)) != NULL) {
        char full[4352];
        struct stat st;
        if (!wild_match(f->pattern, e->d_name)) continue;
        memset(fd, 0, sizeof *fd);
        snprintf(full, sizeof full, "%s/%s", f->base, e->d_name);
        if (stat(full, &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)st.st_size;
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        }
        to_w(e->d_name, fd->cFileName, MAX_PATH);
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd)
{
    char p[4096], *slash;
    OtFind *f = (OtFind *)calloc(1, sizeof *f);
    if (!f) return INVALID_HANDLE_VALUE;
    host_path_w(pattern, p, sizeof p);
    slash = strrchr(p, '/');
    if (slash) {
        *slash = 0;
        snprintf(f->base, sizeof f->base, "%s", p[0] ? p : "/");
        snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
    } else {
        snprintf(f->base, sizeof f->base, ".");
        snprintf(f->pattern, sizeof f->pattern, "%s", p);
    }
    if (!strcmp(f->pattern, "*.*")) snprintf(f->pattern, sizeof f->pattern, "*");
    f->dir = opendir(f->base);
    if (!f->dir || !find_next(f, fd)) {
        if (f->dir) closedir(f->dir);
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    return h && h != INVALID_HANDLE_VALUE && find_next((OtFind *)h, fd);
}

BOOL FindClose(HANDLE h)
{
    OtFind *f = (OtFind *)h;
    if (!f || h == INVALID_HANDLE_VALUE) return FALSE;
    closedir(f->dir);
    free(f);
    return TRUE;
}

const KNOWNFOLDERID FOLDERID_Documents = { 0 };

long SHGetKnownFolderPath(const KNOWNFOLDERID *id, DWORD flags, HANDLE token, LPWSTR *out)
{
    (void)id; (void)flags; (void)token;
    if (out) *out = NULL;
    return -1;      /* E_FAIL: no Documents folder; the install root is used */
}

void CoTaskMemFree(void *p) { free(p); }

/* ── Command line, process ──────────────────────────────────────────── */

void ot_set_args(int argc, char **argv)
{
    char dir[2048], full[2600];
    s_argc = argc;
    s_argv = argv;
#ifdef __SWITCH__
    /* argv[0] is the .nro ("sdmc:/switch/opentricky/SSX Tricky.nro"), when
     * the Homebrew Menu gives one. */
    if (argc > 0 && argv[0] && argv[0][0]) w32_set_module_path(argv[0]);
    else w32_set_module_path("sdmc:/switch/opentricky/SSX Tricky.nro");
#endif
    if (ot_exe_dir(dir, sizeof dir)) {
#ifdef __SWITCH__
        snprintf(full, sizeof full, "romfs:/res");
#else
        snprintf(full, sizeof full, "%s/romfs/res", dir);
#endif
        w32_set_resource_dir(full);
    }
}

int ot_exe_dir(char *out, size_t out_sz)
{
    char exe[2048], *slash;
    if (!GetModuleFileNameA(NULL, exe, sizeof exe) || !exe[0]) return 0;
    ot_path_slashes(exe);
    slash = strrchr(exe, '/');
    if (!slash) return 0;
    if (slash > exe && slash[-1] == ':') slash[1] = 0;    /* "sdmc:/" */
    else *slash = 0;
    return snprintf(out, out_sz, "%s", exe[0] ? exe : "/") < (int)out_sz;
}

int ot_find_iso(char *out, size_t out_sz)
{
    char dir[1024];
    DIR *d;
    struct dirent *e;
    int found = 0;
    if (!ot_exe_dir(dir, sizeof dir) || !(d = opendir(dir))) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".iso"))
            found = snprintf(out, out_sz, "%s%s%s", dir,
                             dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name) < (int)out_sz;
    }
    closedir(d);
    return found;
}

LPSTR GetCommandLineA(void)
{
    static char line[4096];
    size_t len = 0;
    int i;
    if (line[0]) return line;
    for (i = 0; i < s_argc && len + 4 < sizeof line; i++)
        len += (size_t)snprintf(line + len, sizeof line - len, i ? " \"%s\"" : "\"%s\"", s_argv[i]);
    return line;
}

LPWSTR GetCommandLineW(void)
{
    static wchar_t line[4096];
    if (!line[0]) to_w(GetCommandLineA(), line, 4096);
    return line;
}

/* The arguments main() received (the command line is not re-parsed): one
 * block, freed with LocalFree. */
LPWSTR *CommandLineToArgvW(LPCWSTR cmd, int *argc)
{
    size_t total = 0, off;
    int i;
    char *block;
    LPWSTR *v;
    (void)cmd;
    for (i = 0; i < s_argc; i++) total += (strlen(s_argv[i]) + 1) * sizeof(wchar_t);
    block = (char *)malloc((size_t)(s_argc + 1) * sizeof(LPWSTR) + total);
    if (!block) return NULL;
    v = (LPWSTR *)block;
    off = (size_t)(s_argc + 1) * sizeof(LPWSTR);
    for (i = 0; i < s_argc; i++) {
        size_t n = strlen(s_argv[i]) + 1;
        v[i] = (LPWSTR)(block + off);
        to_w(s_argv[i], v[i], n);
        off += n * sizeof(wchar_t);
    }
    v[s_argc] = NULL;
    if (argc) *argc = s_argc;
    return v;
}

HLOCAL LocalFree(HLOCAL p) { free(p); return NULL; }

DWORD GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD n)
{
    char a[2048];
    DWORD k = GetModuleFileNameA(m, a, sizeof a);
    if (!k || !n) return 0;
    to_w(a, out, n);
    return (DWORD)wcslen(out);
}

void *GetProcAddress(HMODULE m, LPCSTR name) { (void)m; (void)name; return NULL; }
BOOL SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }

BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, void *pa, void *ta, BOOL inherit, DWORD flags,
                    void *env, LPCWSTR dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi)
{
    (void)app; (void)cmd; (void)pa; (void)ta; (void)inherit; (void)flags;
    (void)env; (void)dir; (void)si; (void)pi;
    return FALSE;   /* there is no separate launcher here */
}

HANDLE GetStdHandle(DWORD which)
{
    return (HANDLE)(uintptr_t)(which == STD_INPUT_HANDLE ? 1 : which == STD_OUTPUT_HANDLE ? 2 : 3);
}

/* Output that goes to a terminal counts as a console. On the Switch there is
 * none, and nothing is redirected either. */
BOOL GetConsoleMode(HANDLE h, DWORD *mode)
{
    if (mode) *mode = 0;
#ifdef __SWITCH__
    (void)h;
    return TRUE;
#else
    return isatty((int)(uintptr_t)h - 1);
#endif
}

DWORD GetFileType(HANDLE h)
{
#ifdef __SWITCH__
    (void)h;
    return FILE_TYPE_UNKNOWN;
#else
    struct stat st;
    if (fstat((int)(uintptr_t)h - 1, &st) != 0) return FILE_TYPE_UNKNOWN;
    if (S_ISFIFO(st.st_mode)) return FILE_TYPE_PIPE;
    if (S_ISREG(st.st_mode)) return FILE_TYPE_DISK;
    return FILE_TYPE_CHAR;
#endif
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    (void)f;
    return NULL;
}

unsigned timeBeginPeriod(unsigned ms) { (void)ms; return TIMERR_NOERROR; }
unsigned timeEndPeriod(unsigned ms) { (void)ms; return TIMERR_NOERROR; }

/* For the low-address reader log's offsets (main.c). */
void *__ImageBase;

/* ── Network ───────────────────────────────────────────────────────── */

int WSAStartup(unsigned short version, WSADATA *data)
{
    (void)version;
    if (data) data->wVersion = 0x0202;
#ifdef __SWITCH__
    {
        static int up;
        if (!up && R_FAILED(socketInitializeDefault())) return -1;
        up = 1;
    }
#endif
    return 0;
}

/* ── Display and messages ──────────────────────────────────────────── */

void ot_display_size(int *w, int *h)
{
    *w = 1920;
    *h = 1080;
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Handheld) { *w = 1280; *h = 720; }
#else
    {
        SDL_DisplayMode m;
        int was = SDL_WasInit(SDL_INIT_VIDEO);
        if (was || SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
            if (SDL_GetDesktopDisplayMode(0, &m) == 0 && m.w >= 640 && m.h >= 480) { *w = m.w; *h = m.h; }
            if (!was) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }
#endif
}

BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW *dm)
{
    int w, h;
    (void)dev; (void)mode;
    ot_display_size(&w, &h);
    dm->dmPelsWidth = (DWORD)w;
    dm->dmPelsHeight = (DWORD)h;
    dm->dmDisplayFrequency = 60;
    return TRUE;
}

BOOL SystemParametersInfoW(UINT action, UINT p1, void *p2, UINT ini)
{
    (void)p1; (void)ini;
    if (action == SPI_GETWORKAREA && p2) {
        RECT *r = (RECT *)p2;
        int w, h;
        ot_display_size(&w, &h);
        r->left = r->top = 0;
        r->right = w;
        r->bottom = h;
        return TRUE;
    }
    return FALSE;
}

BOOL AdjustWindowRect(RECT *r, DWORD style, BOOL menu) { (void)r; (void)style; (void)menu; return TRUE; }
BOOL SetProcessDPIAware(void) { return TRUE; }

HINSTANCE ShellExecuteW(HWND h, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show)
{
    (void)h; (void)op; (void)file; (void)params; (void)dir; (void)show;
    return (HINSTANCE)(uintptr_t)2;     /* <= 32: failed */
}

/* A message the player has to read: a dialog on Linux; on the Switch a text
 * screen until a button is pressed (only before the game draws: the
 * renderer owns the screen after that). */
int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "SSX Tricky", text ? text : "");
    fflush(stderr);
#ifdef __SWITCH__
    {
        PadState pad;
        consoleInit(NULL);
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        printf("\n  %s\n\n", caption ? caption : "SSX Tricky");
        printf("%s\n\n  Press + to close.\n", text ? text : "");
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_A | HidNpadButton_B)) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, caption ? caption : "SSX Tricky", text ? text : "", NULL);
#endif
    return 1;   /* IDOK */
}

int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type)
{
    char t[4096], c[256];
    to_u8(text ? text : L"", t, sizeof t);
    to_u8(caption ? caption : L"SSX Tricky", c, sizeof c);
    return MessageBoxA(h, t, c, type);
}

/* The window with the keyboard (the game's SDL window when it has focus). */
HWND GetForegroundWindow(void)
{
    return SDL_WasInit(SDL_INIT_VIDEO) ? (HWND)SDL_GetKeyboardFocus() : NULL;
}

UINT MapVirtualKeyW(UINT code, UINT type) { (void)code; (void)type; return 0; }

/* ── Start ─────────────────────────────────────────────────────────── */

#ifdef __SWITCH__
/* An abort (a failed check, a stub, the C library giving up) would otherwise
 * just drop back to the Homebrew Menu: say so on screen first. */
static void on_abort(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[SSX Tricky] stopped (abort)\n");
    fflush(stderr);
    fsync(STDERR_FILENO);
    MessageBoxA(NULL, "The game stopped with an error.\n\n"
                "SSX Tricky.log, in the same folder as the game, says where.", "SSX Tricky", MB_ICONERROR);
    signal(SIGABRT, SIG_DFL);
    _exit(1);
}
#endif

#ifdef __SWITCH__
#include <fcntl.h>
#include <sys/iosupport.h>

/* The log: stdout and stderr both write here. A file on the SD card only
 * grows on the card when its data is committed, and a program the system
 * stops leaves nothing it has not committed: the first lines (the start, where
 * a port fails first) are written and committed one by one. After that, lines
 * collect in memory and a thread writes and commits them four times a second,
 * so the game never waits on the card; a crash report commits what is left
 * (fsync on stderr). */
static int s_log_fd = -1;
static volatile unsigned s_log_writes;
#define LOG_SYNC_EACH 1500
#define LOG_BUF_SIZE (256 * 1024)
static char s_log_buf[LOG_BUF_SIZE];
static size_t s_log_len;
static Mutex s_log_mx;

static void log_flush(void)
{
    mutexLock(&s_log_mx);
    if (s_log_len && s_log_fd >= 0) write(s_log_fd, s_log_buf, s_log_len);
    s_log_len = 0;
    mutexUnlock(&s_log_mx);
}

static ssize_t log_write_r(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    if (s_log_writes < LOG_SYNC_EACH) {
        ssize_t n = write(s_log_fd, ptr, len);
        if (s_log_writes++ < LOG_SYNC_EACH) fsync(s_log_fd);
        return n < 0 ? 0 : n;
    }
    mutexLock(&s_log_mx);
    if (s_log_len + len > LOG_BUF_SIZE) {      /* full: write it out here */
        if (s_log_len) write(s_log_fd, s_log_buf, s_log_len);
        s_log_len = 0;
    }
    if (len > LOG_BUF_SIZE) write(s_log_fd, ptr, len);
    else { memcpy(s_log_buf + s_log_len, ptr, len); s_log_len += len; }
    mutexUnlock(&s_log_mx);
    return len;
}

/* Also the crash report's commit: if the crashed thread holds the lock, the
 * lines in memory are lost rather than the report waiting forever. */
static int log_fsync_r(struct _reent *r, void *fd)
{
    int tries;
    (void)r; (void)fd;
    for (tries = 0; tries < 50; tries++) {
        if (mutexTryLock(&s_log_mx)) {
            if (s_log_len && s_log_fd >= 0) write(s_log_fd, s_log_buf, s_log_len);
            s_log_len = 0;
            mutexUnlock(&s_log_mx);
            break;
        }
        svcSleepThread(1000000LL);
    }
    return s_log_fd >= 0 ? fsync(s_log_fd) : 0;
}

static const devoptab_t s_log_dev = { .name = "log", .write_r = log_write_r, .fsync_r = log_fsync_r };

static void on_exit_log(void) { printf("[SSX Tricky] exit\n"); log_flush(); if (s_log_fd >= 0) fsync(s_log_fd); }

static void *log_sync_thread(void *arg)
{
    (void)arg;
    for (;;) {
        svcSleepThread(250000000LL);
        log_flush();
        if (s_log_fd >= 0) fsync(s_log_fd);
    }
    return NULL;
}
#endif


void ot_host_init(void)
{
#ifdef __SWITCH__
    /* Output to a PC when started with nxlink (nxlink -s); otherwise to
     * "SSX Tricky.log" beside the game from the very start, unbuffered, so a
     * run that stops early still leaves a trace. LogFile=1 in settings.ini
     * later reopens the same file. */
    if (__nxlink_host.s_addr) {
        socketInitializeDefault();
        nxlinkStdio();
    } else {
        char dir[1024], path[1100];
        if (ot_exe_dir(dir, sizeof dir)) {
            snprintf(path, sizeof path, "%s%sSSX Tricky.log", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
            s_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
            if (s_log_fd >= 0) {
                devoptab_list[STD_OUT] = &s_log_dev;
                devoptab_list[STD_ERR] = &s_log_dev;
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
            }
        }
    }
    printf("SSX Tricky (OpenTrickyNX) starting: %s\n", s_argc > 0 ? s_argv[0] : "?");
    ot_switch_env_load();
    {
        pthread_t t;
        if (pthread_create(&t, NULL, log_sync_thread, NULL) == 0) pthread_detach(t);
    }
    signal(SIGABRT, on_abort);
    atexit(on_exit_log);

    /* Started from the album, homebrew gets a few hundred MB at most: not
     * enough for the Xbox's memory plus the GPU's and the textures. */
    {
        AppletType t = appletGetAppletType();
        printf("applet type %d\n", (int)t);
        if (t != AppletType_Application && t != AppletType_SystemApplication) {
            MessageBoxA(NULL, "SSX Tricky needs more memory than the album gives.\n\n"
                        "  Start the Homebrew Menu over a game: hold R while starting any\n"
                        "  installed game, then start SSX Tricky from it.", "SSX Tricky", MB_ICONERROR);
            exit(1);
        }
    }
    ot_switch_clocks();
    romfsInit();
#endif
    ot_pad_init();
}
