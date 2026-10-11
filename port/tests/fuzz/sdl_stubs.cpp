/* Link-time stubs for the SDL calls of ot_update.cpp / ot_util.cpp. The JSON
 * harness never reaches the file / clock code; each stub fails (or aborts),
 * so nothing is read or written on disk. No SDL library is needed. */
#include <SDL3/SDL.h>
#include <cstdlib>

extern "C" {
Uint64 SDL_GetTicksNS(void) { return 0; }
SDL_IOStream *SDL_IOFromFile(const char *, const char *) { return nullptr; }
size_t SDL_WriteIO(SDL_IOStream *, const void *, size_t) { abort(); }
size_t SDL_ReadIO(SDL_IOStream *, void *, size_t) { abort(); }
bool SDL_CloseIO(SDL_IOStream *) { abort(); }
Sint64 SDL_GetIOSize(SDL_IOStream *) { abort(); }
Sint64 SDL_SeekIO(SDL_IOStream *, Sint64, SDL_IOWhence) { abort(); }
bool SDL_RemovePath(const char *) { return false; }
bool SDL_RenamePath(const char *, const char *) { return false; }
bool SDL_GetPathInfo(const char *, SDL_PathInfo *) { return false; }
void *SDL_LoadFile(const char *, size_t *) { return nullptr; }
bool SDL_SaveFile(const char *, const void *, size_t) { return false; }
bool SDL_EnumerateDirectory(const char *, SDL_EnumerateDirectoryCallback, void *) { return false; }
bool SDL_GetCurrentTime(SDL_Time *t) { *t = 0; return true; }
bool SDL_TimeToDateTime(SDL_Time, SDL_DateTime *, bool) { return false; }
void SDL_free(void *p) { free(p); }
const char *SDL_getenv(const char *) { return nullptr; }
}
