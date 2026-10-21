/*
 * modfs.c - optional mod folders: files the title reads from its disc can be
 * replaced by files in folders the player names (docs/mod-folders.md).
 *
 * TAKEDAWN_MODS=<folder>[;<folder>...]: up to eight folders, the first one
 * named wins. ';' separates them on every system (a ':' is part of a path).
 * Each mirrors the disc: <folder>/Tracks/US/C3_V1/static.dat is read instead
 * of the disc's Tracks\US\C3_V1\static.dat. A file a folder does not have is
 * read from the disc as before. Unset (the default): nothing is installed and
 * no path changes. On Android, where there is no shell to set it from, unset
 * means <the app's files folder>/mods when that folder exists
 * (RECOMP_ANDROID_DIR, which GameActivity sets); a TAKEDAWN_MODS line in
 * env.txt still wins, and an empty one turns the folder off.
 *
 * How: xbox_PathRedirectHook (the toolkit's kernel.h), chained to the hook
 * already there (the soundtrack's). For a D:\ or \Device\CdRom0\ path whose
 * file a mod folder has, the hook returns the host path: "\\?\<host path>" on
 * Windows, the absolute path elsewhere. No translation rule claims either
 * form, so xbox_translate_path hands it to the host as it is (kernel_path.c,
 * "Unrecognized Xbox path"). Every other path goes on down the chain
 * unchanged. The title's own loaders then read the file; nothing here parses
 * or checks what is in it.
 *
 * Case: the disc's names are not case-sensitive and the title does not spell
 * them the way the disc does (D:\Sound\DSP\... for sound/DSP). Windows and
 * macOS volumes usually fold case themselves. Elsewhere (Linux, the Steam
 * Deck, any case-sensitive volume) each part of the path is taken as written
 * when the folder has it, else the one entry of its directory that matches
 * without case (ASCII only). Two or more such entries and none as written
 * (Data and DATA for "data"): refused, whichever readdir lists first.
 *
 * What is refused (the disc's file is read instead), the same on every system:
 *   - a path part that is empty, "." or "..", ends in a dot or a space, or
 *     has ':', a character Windows does not allow in a name or one that is
 *     not plain ASCII;
 *   - a link anywhere below a mod folder (a reparse point on Windows: a
 *     symbolic link or a junction; a symbolic link elsewhere), so a folder
 *     cannot point outside itself. The folder itself may be reached through
 *     links: its full path is taken once, at the start;
 *   - anything that is not a regular file (a directory, a pipe, a device);
 *   - a host path of MAX_PATH characters or more (the toolkit's buffer).
 * A mod folder whose full path is not printable ASCII is ignored: on Windows
 * the path handed back is read in the guest's code page; elsewhere a '\' in
 * it is refused too, the toolkit turns it into '/'.
 *
 * Each replaced file is logged once: "[MOD] <guest path> -> <host path>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "kernel.h"   /* xbox_PathRedirectHook */

#if !defined(_WIN32)
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

#define MODFS_MAX_DIRS 8
#define MODFS_SEEN     512

static const char *(*s_prev)(const char *);
static char  s_dirs[MODFS_MAX_DIRS][MAX_PATH];
static int   s_ndirs;
static char  s_seen[MODFS_SEEN][160];
static int   s_nseen;
#if defined(_WIN32)
static CRITICAL_SECTION s_lock;
static int   s_lock_made;
#define MODFS_LOCK()   EnterCriticalSection(&s_lock)
#define MODFS_UNLOCK() LeaveCriticalSection(&s_lock)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define MODFS_LOCK()   pthread_mutex_lock(&s_lock)
#define MODFS_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif
/* Per thread: several threads open files at once, and the caller copies the
 * path before its next translation on the same thread. */
static _Thread_local char t_out[MAX_PATH + 8];

/* ASCII only, whatever the locale: the disc's names are plain ASCII. */
static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int same_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        if (!*a)
            return 1;
    }
}

static const char *strip_device(const char *p)
{
    static const char *const pre[] = { "D:\\", "\\Device\\CdRom0\\" };
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        size_t n = 0;
        while (pre[i][n] && ascii_lower((unsigned char)p[n]) == ascii_lower((unsigned char)pre[i][n]))
            n++;
        if (!pre[i][n])
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

/* One part of a disc path, the n bytes at p: a plain name. The same rule on
 * every system, so a mod folder means the same everywhere. */
static int part_ok(const char *p, size_t n)
{
    if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')
            || p[n - 1] == '.' || p[n - 1] == ' ')
        return 0;      /* Windows drops a trailing dot or space, "\\?\" does not */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7E || strchr("<>:\"|?*", p[i]))
            return 0;  /* the disc's names are plain ASCII */
    return 1;
}

#if defined(_WIN32)
/* <dir>\<rel> into out, '/' made '\'; 0 unless every part of rel is a plain
 * name and every part below dir exists without being a reparse point, and
 * the last one is a regular file. NTFS folds case itself. */
static int mod_file(const char *dir, const char *rel, char *out, size_t n, int *ambiguous)
{
    size_t dl = strlen(dir), i, start;
    const char *sep = dl && dir[dl - 1] == '\\' ? "" : "\\";   /* a drive root (E:\) has its own */
    int len = snprintf(out, n, "%s%s%s", dir, sep, rel);

    *ambiguous = 0;
    if (len <= 0 || (size_t)len >= n)
        return 0;
    start = dl + strlen(sep);
    for (i = start; out[i]; i++)
        if (out[i] == '/')
            out[i] = '\\';
    for (i = start;; i++) {
        char c = out[i];
        DWORD a;
        if (c != '\\' && c != 0)
            continue;
        if (!part_ok(out + start, i - start))
            return 0;
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
    }
}

/* The folder tok names, as a full path without a trailing separator. */
static int mod_folder(const char *tok, char *full)
{
    DWORD n = GetFullPathNameA(tok, MAX_PATH, full, NULL);
    DWORD a = n && n < MAX_PATH ? GetFileAttributesA(full) : INVALID_FILE_ATTRIBUTES;
    size_t len;

    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "[MOD] ignored (not a folder): %s\n", tok);
        return 0;
    }
    if (!printable_ascii(full)) {
        fprintf(stderr, "[MOD] ignored (the path is not plain ASCII): %s\n", tok);
        return 0;
    }
    len = strlen(full);
    while (len > 3 && (full[len - 1] == '\\' || full[len - 1] == '/'))
        full[--len] = 0;
    return 1;
}
#else
/* The entry name (NAME_MAX + 1 bytes) of the open directory d: as written
 * when d has it, else the only entry that matches without case, whose
 * spelling replaces name. Its type into *st, the entry itself (a link is a
 * link). 0 when d has none, or has two or more and none as written
 * (*ambiguous set). */
static int find_entry(int d, char *name, struct stat *st, int *ambiguous)
{
    char hit[NAME_MAX + 1];
    struct dirent *de;
    DIR *dir;
    int hits = 0, fd;

    if (fstatat(d, name, st, AT_SYMLINK_NOFOLLOW) == 0)
        return 1;
    if (errno != ENOENT)
        return 0;
    fd = fcntl(d, F_DUPFD_CLOEXEC, 0);   /* fdopendir takes it over */
    if (fd < 0)
        return 0;
    dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return 0;
    }
    rewinddir(dir);
    while ((de = readdir(dir)) != NULL)
        if (same_nocase(de->d_name, name) && ++hits == 1)
            memcpy(hit, de->d_name, strlen(name) + 1);   /* the same length: ASCII folding */
    closedir(dir);
    if (hits != 1) {
        *ambiguous = hits > 1;
        return 0;
    }
    memcpy(name, hit, strlen(name) + 1);
    return fstatat(d, name, st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* <dir>/<rel> into out, each part of rel in the folder's own spelling; 0
 * unless every part is a plain name, every directory below dir is one (not a
 * link to one) and the last part is a regular file. The walk goes from
 * directory descriptor to directory descriptor (openat with O_NOFOLLOW), so a
 * part replaced by a link during the walk is refused too. */
static int mod_file(const char *dir, const char *rel, char *out, size_t n, int *ambiguous)
{
    size_t len = strlen(dir);
    const char *p = rel;
    int d, ok = 0;

    *ambiguous = 0;
    if (len >= n)
        return 0;
    memcpy(out, dir, len + 1);
    if (len == 1)
        len = 0;        /* "/": the parts follow its own '/' */
    d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d < 0)
        return 0;
    for (;;) {
        const char *e = p;
        char name[NAME_MAX + 1];
        struct stat st;
        size_t pl;
        int sub;

        while (*e && *e != '\\' && *e != '/')
            e++;
        pl = (size_t)(e - p);
        if (!part_ok(p, pl) || pl > NAME_MAX || len + 1 + pl >= n)
            break;
        memcpy(name, p, pl);
        name[pl] = 0;
        if (!find_entry(d, name, &st, ambiguous))
            break;
        out[len++] = '/';
        memcpy(out + len, name, pl + 1);
        len += pl;
        if (!*e) {
            ok = S_ISREG(st.st_mode);
            break;
        }
        if (!S_ISDIR(st.st_mode))
            break;      /* a file, or a link */
        sub = openat(d, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(d);
        d = sub;
        if (d < 0)
            return 0;
        p = e + 1;
    }
    close(d);
    return ok;
}

/* The folder tok names, as a full path with every link in it resolved. */
static int mod_folder(const char *tok, char *full)
{
    char real[PATH_MAX];
    struct stat st;

    if (!realpath(tok, real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[MOD] ignored (not a folder): %s\n", tok);
        return 0;
    }
    if (!printable_ascii(real) || strchr(real, '\\')) {
        fprintf(stderr, "[MOD] ignored (the path is not plain ASCII, or has a '\\'): %s\n", tok);
        return 0;
    }
    if (strlen(real) >= MAX_PATH) {
        fprintf(stderr, "[MOD] ignored (the path is too long): %s\n", tok);
        return 0;
    }
    strcpy(full, real);
    return 1;
}
#endif

/* host NULL: guest's file was refused because names in a folder differ only
 * in case. */
static void note_once(const char *guest, const char *host)
{
    int fresh = 1;
    MODFS_LOCK();
    for (int i = 0; i < s_nseen && fresh; i++)
        if (same_nocase(s_seen[i], guest))
            fresh = 0;
    if (fresh && s_nseen < MODFS_SEEN) {
        strncpy(s_seen[s_nseen], guest, sizeof s_seen[0] - 1);
        s_seen[s_nseen][sizeof s_seen[0] - 1] = 0;
        s_nseen++;
    }
    MODFS_UNLOCK();
    if (fresh) {
        if (host)
            fprintf(stderr, "[MOD] %s -> %s\n", guest, host);
        else
            fprintf(stderr, "[MOD] %s: refused, names in a mod folder differ only in case; "
                    "the disc's file is read\n", guest);
        fflush(stderr);
    }
}

static const char *redirect(const char *path)
{
    const char *rel = path ? strip_device(path) : NULL;

    if (rel && *rel) {
        for (int i = 0; i < s_ndirs; i++) {
            char host[MAX_PATH];
            int ambiguous;
            if (mod_file(s_dirs[i], rel, host, sizeof host, &ambiguous)) {
#if defined(_WIN32)
                snprintf(t_out, sizeof t_out, "\\\\?\\%s", host);
#else
                snprintf(t_out, sizeof t_out, "%s", host);
#endif
                note_once(path, host);
                return t_out;
            }
            if (ambiguous) {
                note_once(path, NULL);
                break;      /* not a later folder's file either: the disc's */
            }
        }
    }
    return s_prev ? s_prev(path) : NULL;
}

/* After xbox_path_init and the soundtrack's hook, before the title runs.
 * Called again (the unit test), it replaces the folder list; the hook is
 * installed once. */
void bo3_modfs_init(void)
{
    const char *e = getenv("TAKEDAWN_MODS");
    char buf[MODFS_MAX_DIRS * MAX_PATH];
#if defined(__ANDROID__)
    char app_mods[MAX_PATH];
    if (!e) {
        const char *app = getenv("RECOMP_ANDROID_DIR");
        struct stat st;
        if (app && *app && snprintf(app_mods, sizeof app_mods, "%s/mods", app) < (int)sizeof app_mods
                && stat(app_mods, &st) == 0 && S_ISDIR(st.st_mode))
            e = app_mods;
    }
#endif

    s_ndirs = 0;
    s_nseen = 0;
    if (!e || !*e)
        return;
    strncpy(buf, e, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        if (s_ndirs == MODFS_MAX_DIRS) {
            fprintf(stderr, "[MOD] more than %d folders: %s and the rest ignored\n",
                    MODFS_MAX_DIRS, tok);
            break;
        }
        if (!mod_folder(tok, s_dirs[s_ndirs]))
            continue;
        s_ndirs++;
        fprintf(stderr, "[MOD] folder %d: %s\n", s_ndirs, s_dirs[s_ndirs - 1]);
    }
    fflush(stderr);
    if (!s_ndirs || xbox_PathRedirectHook == redirect)
        return;
#if defined(_WIN32)
    if (!s_lock_made) {
        InitializeCriticalSection(&s_lock);
        s_lock_made = 1;
    }
#endif
    s_prev = xbox_PathRedirectHook;
    xbox_PathRedirectHook = redirect;
}
