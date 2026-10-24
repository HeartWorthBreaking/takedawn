/*
 * plugins_win32.c - loading a native plugin on Windows (plugins_platform.h).
 *
 * The module is opened for reading with only reading shared, so nobody can
 * write, replace or delete it until it is loaded; it is hashed through that
 * handle and then loaded by its full path with LoadLibraryExW. The search
 * flags keep its own dependencies to its own folder and System32: never the
 * current folder or PATH, and nothing process-wide changes (no
 * SetDllDirectory). After loading, the module the system mapped must be the
 * file that was hashed (GetModuleFileNameW names the same path), else it is
 * refused (it stays loaded: the system has already run its start-up code).
 */
#include "plugins_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "xr_sha256.h"

struct bo3_plugin_file {
    HANDLE h;
    wchar_t path[MAX_PATH + 1];
};

#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

int bo3_plugin_sys_supported(void)
{
    return 1;
}

static void sys_error(const char *what, char *why, size_t wn)
{
    DWORD e = GetLastError();
    char msg[160] = "";
    size_t n;

    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
    for (n = strlen(msg); n && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ' || msg[n - 1] == '.'); n--)
        msg[n - 1] = 0;
    snprintf(why, wn, "%s (Windows error %lu%s%s)", what, (unsigned long)e, msg[0] ? ": " : "", msg);
}

bo3_plugin_file *bo3_plugin_sys_hold(const char *path, uint8_t sha[32], char *why, size_t wn)
{
    bo3_plugin_file *f = calloc(1, sizeof *f);
    BY_HANDLE_FILE_INFORMATION fi;
    xr_sha256 s;
    static unsigned char buf[1 << 16];
    DWORD got;

    if (!f) {
        snprintf(why, wn, "out of memory");
        return NULL;
    }
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, f->path, MAX_PATH)) {
        snprintf(why, wn, "the path cannot be used");
        free(f);
        return NULL;
    }
    /* FILE_FLAG_OPEN_REPARSE_POINT: a reparse point is opened as itself, and
     * refused below, never followed. */
    f->h = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f->h == INVALID_HANDLE_VALUE) {
        sys_error("the module cannot be opened", why, wn);
        free(f);
        return NULL;
    }
    if (!GetFileInformationByHandle(f->h, &fi)
            || (fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
        snprintf(why, wn, "the module is not a regular file");
        bo3_plugin_sys_release(f);
        return NULL;
    }
    xr_sha256_init(&s);
    for (;;) {
        if (!ReadFile(f->h, buf, sizeof buf, &got, NULL)) {
            sys_error("the module cannot be read", why, wn);
            bo3_plugin_sys_release(f);
            return NULL;
        }
        if (!got)
            break;
        xr_sha256_update(&s, buf, got);
    }
    xr_sha256_final(&s, sha);
    return f;
}

void *bo3_plugin_sys_load(bo3_plugin_file *f, char *why, size_t wn)
{
    HMODULE m = LoadLibraryExW(f->path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    wchar_t got[MAX_PATH + 1];
    DWORD n;

    if (!m) {
        sys_error("the module does not load", why, wn);
        return NULL;
    }
    n = GetModuleFileNameW(m, got, MAX_PATH + 1);
    if (n == 0 || n > MAX_PATH || CompareStringOrdinal(got, -1, f->path, -1, TRUE) != CSTR_EQUAL) {
        snprintf(why, wn, "the system loaded another module than the one that was checked (it stays loaded)");
        return NULL;
    }
    return (void *)m;
}

void bo3_plugin_sys_release(bo3_plugin_file *f)
{
    if (!f)
        return;
    if (f->h && f->h != INVALID_HANDLE_VALUE)
        CloseHandle(f->h);
    free(f);
}

void *bo3_plugin_sys_symbol(void *lib, const char *name)
{
    FARPROC p = GetProcAddress((HMODULE)lib, name);
    void *v;
    memcpy(&v, &p, sizeof v);   /* a function's address as data: the same size on Windows */
    return v;
}
