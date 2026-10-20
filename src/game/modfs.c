/*
 * modfs.c - optional mod folders: files the title reads from its disc can be
 * replaced by files in folders the player names (docs/mod-folders.md).
 *
 * TAKEDAWN_MODS=<folder>[;<folder>...]: up to eight folders, the first one
 * named wins. Each mirrors the disc: <folder>\Tracks\US\C3_V1\static.dat is
 * read instead of the disc's Tracks\US\C3_V1\static.dat. A file a folder does
 * not have is read from the disc as before. Unset (the default): nothing is
 * installed and no path changes.
 *
 * How: xbox_PathRedirectHook (the toolkit's kernel.h), chained to the hook
 * already there (the soundtrack's). For a D:\ or \Device\CdRom0\ path whose
 * file a mod folder has, the hook returns "\\?\<host path>". No translation
 * rule claims that form, so xbox_translate_path hands it to the host as it is
 * (kernel_path.c, "Unrecognized Xbox path"). Every other path goes on down the
 * chain unchanged. The title's own loaders then read the file; nothing here
 * parses or checks what is in it.
 *
 * What is refused (the disc's file is read instead):
 *   - a path part that is empty, "." or "..", ends in a dot or a space, or
 *     has a drive, ':', a character Windows does not allow in a name or one
 *     that is not plain ASCII;
 *   - a reparse point (a symbolic link or a junction) anywhere below a mod
 *     folder, so a folder cannot point outside itself;
 *   - anything that is not a regular file.
 * A mod folder whose full path is not printable ASCII is ignored: the path
 * handed back is read in the guest's code page.
 *
 * Each replaced file is logged once: "[MOD] <guest path> -> <host path>".
 * Windows only for now (the build adds this file on WIN32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "kernel.h"   /* xbox_PathRedirectHook */

#define MODFS_MAX_DIRS 8
#define MODFS_SEEN     512

static const char *(*s_prev)(const char *);
static char  s_dirs[MODFS_MAX_DIRS][MAX_PATH];
static int   s_ndirs;
static char  s_seen[MODFS_SEEN][160];
static int   s_nseen;
static CRITICAL_SECTION s_lock;
/* Per thread: several threads open files at once, and the caller copies the
 * path before its next translation on the same thread. */
static _Thread_local char t_out[MAX_PATH + 8];

static const char *strip_device(const char *p)
{
    static const char *const pre[] = { "D:\\", "\\Device\\CdRom0\\" };
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        size_t n = strlen(pre[i]);
        if (_strnicmp(p, pre[i], n) == 0)
            return p + n;
    }
    return NULL;
}

static int printable_ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E)
            return 0;
    return 1;
}

/* <dir>\<rel> into out, '/' made '\'; 0 unless every part of rel is a plain
 * name and every part below dir exists without being a reparse point, and
 * the last one is a regular file. */
static int mod_file(const char *dir, const char *rel, char *out, size_t n)
{
    size_t dl = strlen(dir), i, start;
    const char *sep = dl && dir[dl - 1] == '\\' ? "" : "\\";   /* a drive root (E:\) has its own */
    int len = snprintf(out, n, "%s%s%s", dir, sep, rel);

    if (len <= 0 || (size_t)len >= n)
        return 0;
    start = dl + strlen(sep);
    for (i = start; out[i]; i++)
        if (out[i] == '/')
            out[i] = '\\';
    for (i = start;; i++) {
        char c = out[i];
        if (c == '\\' || c == 0) {
            size_t pl = i - start;
            DWORD a;
            if (pl == 0 || (pl == 1 && out[start] == '.')
                    || (pl == 2 && out[start] == '.' && out[start + 1] == '.')
                    || out[i - 1] == '.' || out[i - 1] == ' ')
                return 0;      /* Windows drops a trailing dot or space, "\\?\" does not */
            out[i] = 0;
            a = GetFileAttributesA(out);
            out[i] = c;
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT))
                return 0;
            if (c == 0)
                return !(a & FILE_ATTRIBUTE_DIRECTORY);
            if (!(a & FILE_ATTRIBUTE_DIRECTORY))
                return 0;
            start = i + 1;
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E || strchr("<>:\"|?*", c))
            return 0;      /* the disc's names are plain ASCII */
    }
}

static void note_once(const char *guest, const char *host)
{
    int fresh = 1;
    EnterCriticalSection(&s_lock);
    for (int i = 0; i < s_nseen && fresh; i++)
        if (_stricmp(s_seen[i], guest) == 0)
            fresh = 0;
    if (fresh && s_nseen < MODFS_SEEN) {
        strncpy(s_seen[s_nseen], guest, sizeof s_seen[0] - 1);
        s_seen[s_nseen][sizeof s_seen[0] - 1] = 0;
        s_nseen++;
    }
    LeaveCriticalSection(&s_lock);
    if (fresh) {
        fprintf(stderr, "[MOD] %s -> %s\n", guest, host);
        fflush(stderr);
    }
}

static const char *redirect(const char *path)
{
    const char *rel = path ? strip_device(path) : NULL;

    if (rel && *rel) {
        for (int i = 0; i < s_ndirs; i++) {
            char host[MAX_PATH];
            if (mod_file(s_dirs[i], rel, host, sizeof host)) {
                snprintf(t_out, sizeof t_out, "\\\\?\\%s", host);
                note_once(path, host);
                return t_out;
            }
        }
    }
    return s_prev ? s_prev(path) : NULL;
}

/* After xbox_path_init and the soundtrack's hook, before the title runs. */
void bo3_modfs_init(void)
{
    const char *e = getenv("TAKEDAWN_MODS");
    char buf[MODFS_MAX_DIRS * MAX_PATH];

    if (!e || !*e)
        return;
    strncpy(buf, e, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        char full[MAX_PATH];
        DWORD n, a;
        size_t len;

        if (s_ndirs == MODFS_MAX_DIRS) {
            fprintf(stderr, "[MOD] more than %d folders: %s and the rest ignored\n",
                    MODFS_MAX_DIRS, tok);
            break;
        }
        n = GetFullPathNameA(tok, MAX_PATH, full, NULL);
        a = n && n < MAX_PATH ? GetFileAttributesA(full) : INVALID_FILE_ATTRIBUTES;
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            fprintf(stderr, "[MOD] ignored (not a folder): %s\n", tok);
            continue;
        }
        if (!printable_ascii(full)) {
            fprintf(stderr, "[MOD] ignored (the path is not plain ASCII): %s\n", tok);
            continue;
        }
        len = strlen(full);
        while (len > 3 && (full[len - 1] == '\\' || full[len - 1] == '/'))
            full[--len] = 0;
        strcpy(s_dirs[s_ndirs++], full);
        fprintf(stderr, "[MOD] folder %d: %s\n", s_ndirs, full);
    }
    if (!s_ndirs)
        return;
    InitializeCriticalSection(&s_lock);
    s_prev = xbox_PathRedirectHook;
    xbox_PathRedirectHook = redirect;
    fflush(stderr);
}
