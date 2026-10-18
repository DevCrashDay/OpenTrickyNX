/*
 * ot_posix.h -- the Win32 calls the game's host code (port/src) makes beyond
 * what xboxrecomp's platform layer already provides, for Linux and the
 * Switch. Implemented in port/src/posix/ot_posix.c.
 *
 * Host code is compiled with XBOX_HOST_WCHAR, so WCHAR is wchar_t here and
 * L"..." literals and the C library's wcs* functions work as on Windows. The
 * platform layer's wide-character conversions take 16-bit WCHAR; the ones
 * below take wchar_t and replace them for this code.
 *
 * Paths: Windows code joins with '\'. Every function here that takes a path
 * accepts either separator, and GetFullPathName returns '/' only.
 */
#ifndef OT_POSIX_H
#define OT_POSIX_H

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The command line (main() records it before anything else runs). */
void ot_set_args(int argc, char **argv);
/* Host services the game needs before anything else: on the Switch the
 * romfs and controllers, elsewhere the controllers. */
void ot_host_init(void);
void ot_pad_init(void);
/* The executable's folder, '/'-separated, without the trailing '/'. On the
 * Switch it is the folder of the .nro (sdmc:/switch/opentricky). */
int  ot_exe_dir(char *out, size_t out_sz);
/* The display's size in pixels (the desktop on Linux; 1920x1080 docked or
 * 1280x720 handheld on the Switch). */
void ot_display_size(int *w, int *h);
/* A path with '\' turned into '/', in place. */
char *ot_path_slashes(char *p);

/* ── Character sets (UTF-8 is the code page) ────────────────────────── */
#undef CP_ACP
#define CP_ACP 0
#define WC_NO_BEST_FIT_CHARS 0x00000400
int ot_MultiByteToWideChar(unsigned cp, unsigned long flags, const char *src, int n,
                           wchar_t *dst, int dst_n);
int ot_WideCharToMultiByte(unsigned cp, unsigned long flags, const wchar_t *src, int n,
                           char *dst, int dst_n, const char *def, int *used_def);
#define MultiByteToWideChar(cp, fl, s, n, d, dn) ot_MultiByteToWideChar((cp), (fl), (s), (n), (d), (dn))
#define WideCharToMultiByte(cp, fl, s, n, d, dn, def, used) \
    ot_WideCharToMultiByte((cp), (fl), (s), (n), (d), (dn), (def), (int *)(used))

#ifndef ZeroMemory
#define ZeroMemory(p, n) memset((p), 0, (n))
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD 0x100
#endif

#define _wcsicmp   wcscasecmp
#define _wcsnicmp  wcsncasecmp
#define _snwprintf swprintf
#define _putenv_s(name, value) setenv((name), (value), 1)

/* ── Command line, process ─────────────────────────────────────────── */
LPSTR   GetCommandLineA(void);
LPWSTR  GetCommandLineW(void);
LPWSTR *CommandLineToArgvW(LPCWSTR cmd, int *argc);
HLOCAL  LocalFree(HLOCAL p);
DWORD   GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD n);
void   *GetProcAddress(HMODULE m, LPCSTR name);
BOOL    SetConsoleTitleA(LPCSTR t);

typedef struct _STARTUPINFOW { DWORD cb; } STARTUPINFOW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, void *pa, void *ta, BOOL inherit, DWORD flags,
                    void *env, LPCWSTR dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi);

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK    1
#define FILE_TYPE_CHAR    2
#define FILE_TYPE_PIPE    3
HANDLE GetStdHandle(DWORD which);
BOOL   GetConsoleMode(HANDLE h, DWORD *mode);
DWORD  GetFileType(HANDLE h);

/* Crashes: the platform layer's vectored handlers are not used by the host
 * code here (host_fault takes the trapped registers); these keep the
 * declarations compiling. */
typedef struct _EXCEPTION_POINTERS EXCEPTION_POINTERS;
typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
#ifndef EXCEPTION_INT_OVERFLOW
#define EXCEPTION_INT_OVERFLOW           0xC0000095u
#define EXCEPTION_PRIV_INSTRUCTION       0xC0000096u
#define EXCEPTION_DATATYPE_MISALIGNMENT  0x80000002u
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED  0xC000008Cu
#define EXCEPTION_IN_PAGE_ERROR          0xC0000006u
#endif

/* ── Timer resolution (always 1 ms here) ───────────────────────────── */
#define TIMERR_NOERROR 0
unsigned timeBeginPeriod(unsigned ms);
unsigned timeEndPeriod(unsigned ms);

/* ── Files and folders ─────────────────────────────────────────────── */
#ifndef FILE_ATTRIBUTE_REPARSE_POINT
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#endif
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
    WCHAR cFileName[MAX_PATH];
} WIN32_FIND_DATAW;
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
BOOL   FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL   FindClose(HANDLE h);
DWORD  GetFileAttributesW(LPCWSTR path);
BOOL   CreateDirectoryW(LPCWSTR path, void *sa);
int    SHCreateDirectoryExW(HWND owner, LPCWSTR path, void *sa);
BOOL   CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
DWORD  GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file_part);
DWORD  GetFullPathNameW(LPCWSTR path, DWORD n, LPWSTR out, LPWSTR *file_part);
DWORD  GetShortPathNameW(LPCWSTR path, LPWSTR out, DWORD n);

/* Known folders: none on this host (settings.c has its own answer). */
typedef struct _GUID_OT { int unused; } KNOWNFOLDERID;
extern const KNOWNFOLDERID FOLDERID_Documents;
#ifndef FAILED
#define FAILED(hr) (((long)(hr)) < 0)
#endif
long SHGetKnownFolderPath(const KNOWNFOLDERID *id, DWORD flags, HANDLE token, LPWSTR *out);
void CoTaskMemFree(void *p);

/* ── Display and UI ─────────────────────────────────────────────────── */
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
typedef struct _DEVMODEW {
    WORD  dmSize;
    DWORD dmPelsWidth, dmPelsHeight, dmDisplayFrequency;
} DEVMODEW;
BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW *dm);
#define SPI_GETWORKAREA     0x0030
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
BOOL SystemParametersInfoW(UINT action, UINT p1, void *p2, UINT ini);
BOOL AdjustWindowRect(RECT *r, DWORD style, BOOL menu);
BOOL SetProcessDPIAware(void);
#define SW_SHOW       5
#define SW_SHOWNORMAL 1
HINSTANCE ShellExecuteW(HWND h, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show);
int  MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type);
HWND GetForegroundWindow(void);
#ifndef MB_TOPMOST
#define MB_TOPMOST 0x00040000L
#endif
#define MAPVK_VSC_TO_VK_EX 3
UINT MapVirtualKeyW(UINT code, UINT type);

#ifdef __cplusplus
}
#endif
#endif /* OT_POSIX_H */
