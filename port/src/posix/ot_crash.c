/*
 * ot_crash.c -- crashreport.h on Linux and the Switch.
 *
 * The Windows crash reports (crashreport.c: minidumps, the freeze watchdog,
 * the crash window) are built on Windows' debugging API. Here a crash is
 * reported by the system (a core file on Linux, the Switch's error screen),
 * and the game's log file still records what led up to it.
 */
#include <windows.h>
#include <stdio.h>
#include "crashreport.h"

void crashreport_install(void) {}
void crashreport_set_output(int player, const char *log_path, int output_redirected)
{
    (void)player; (void)log_path; (void)output_redirected;
}
void crashreport_game_start(void) {}
void crashreport_private_folder(const char *utf8) { (void)utf8; }
void crashreport_private_file_acp(const char *path) { (void)path; }
int crashreport_on_crash(EXCEPTION_POINTERS *ep, const char *details,
                         wchar_t *dir_out, size_t dir_out_len)
{
    (void)ep; (void)details; (void)dir_out; (void)dir_out_len;
    return 0;
}
void crashreport_fatal_overflow(EXCEPTION_POINTERS *ep) { (void)ep; }
void crashreport_crash_dialog(DWORD code, const wchar_t *dir)
{
    (void)dir;
    fprintf(stderr, "SSX Tricky stopped because of an error (0x%08lX).\n", (unsigned long)code);
}
void crashreport_open_folder(const wchar_t *dir) { (void)dir; }
