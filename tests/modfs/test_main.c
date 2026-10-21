/* The mod folders' lookup (src/game/modfs.c) on folders the test makes in the
 * temporary directory: no game data, no guest. The test stands in for the
 * toolkit: it owns xbox_PathRedirectHook, puts a hook of its own there first
 * (as the soundtrack's is) and asks what the chain answers for guest paths.
 * The same cases run on every host; the ones a host cannot show (two names
 * that differ only in case on a volume that folds case, symbolic links on
 * Windows) say SKIP. */
#include "kernel.h"   /* xbox_PathRedirectHook */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  include <process.h>
#  define SETENV(k, v) _putenv_s(k, v)
#  define UNSETENV(k) _putenv_s(k, "")
#  define SEP "\\"
#  define dup _dup
#  define dup2 _dup2
#  define close _close
#  define fileno _fileno
#else
#  include <limits.h>
#  include <pthread.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#  define SETENV(k, v) setenv(k, v, 1)
#  define UNSETENV(k) unsetenv(k)
#  define SEP "/"
#endif

const char *(*xbox_PathRedirectHook)(const char *xbox_path);
void bo3_modfs_init(void);
/* The toolkit's print gate (platform/xr_log.h, compiled into modfs.c): 0 is a
 * release build with diagnostic logging off. This file does not include it,
 * so its own lines are never dropped. */
extern int g_xr_log_diag;

static int s_fail, s_pass, s_skip;
#define CHECK(c, ...) do { if (c) s_pass++; else { s_fail++; fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define SKIP(...) do { s_skip++; printf("SKIP "); printf(__VA_ARGS__); putchar('\n'); } while (0)

/* snprintf that must fit: a path too long for the test's buffers stops the
 * test (exit 2) rather than testing a cut-off path. */
static void fit(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) {
        fprintf(stderr, "a path is too long for the test's buffers\n");
        exit(2);
    }
}

/* ---- the folders ---- */

static char s_root[512];
static char s_made[128][700];   /* made, removed in reverse at the end */
static int  s_kind[128];        /* 0 file, 1 folder, 2 link (or junction), 3 folder chmod 000 */
static int  s_nmade;

static const char *at(const char *rel)
{
    /* A few at a time: never more than 8 at() in one expression (the order
     * C evaluates arguments in is not fixed). */
    static char b[8][700];
    static int k;
    char *o = b[k++ & 7];
    fit(o, sizeof b[0], "%s" SEP "%s", s_root, rel);
#if defined(_WIN32)
    for (char *p = o; *p; p++)
        if (*p == '/')
            *p = '\\';
#endif
    return o;
}

static void remember(const char *path, int kind)
{
    if (s_nmade < (int)(sizeof s_made / sizeof s_made[0])) {
        fit(s_made[s_nmade], sizeof s_made[0], "%s", path);
        s_kind[s_nmade++] = kind;
    }
}

/* 1 when the folder is there afterwards (made now, or already). */
static int make_dir(const char *path)
{
#if defined(_WIN32)
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0755);
#endif
    if (r == 0)
        remember(path, 1);
    return r == 0 || errno == EEXIST;
}

/* rel's folders, then the file with the given text; 0 when any step fails. */
static int make_file(const char *rel, const char *text)
{
    char p[700];
    fit(p, sizeof p, "%s", at(rel));
    for (char *s = p + strlen(s_root) + 1; *s; s++) {
        if (*s == SEP[0]) {
            char c = *s;
            *s = 0;
            if (!make_dir(p))
                return 0;
            *s = c;
        }
    }
    FILE *f = fopen(p, "wb");
    if (!f)
        return 0;
    fputs(text, f);
    fclose(f);
    remember(p, 0);
    return 1;
}

/* Everything made, in reverse; also at exit (fit's exit(2), a failed start). */
static void remove_all(void)
{
    static int done;
    if (done || !s_root[0])
        return;
    done = 1;
    for (int i = s_nmade - 1; i >= 0; i--) {
#if defined(_WIN32)
        if (s_kind[i] == 0)
            DeleteFileA(s_made[i]);
        else
            RemoveDirectoryA(s_made[i]);   /* a junction goes, not what it points at */
#else
        if (s_kind[i] == 3)
            chmod(s_made[i], 0755);
        if (s_kind[i] == 1 || s_kind[i] == 3)
            rmdir(s_made[i]);
        else
            unlink(s_made[i]);
#endif
    }
#if defined(_WIN32)
    RemoveDirectoryA(s_root);
#else
    rmdir(s_root);
#endif
}

static int make_root(void)
{
#if defined(_WIN32)
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof tmp, tmp);
    if (!n || n >= sizeof tmp)
        return 0;
    fit(s_root, sizeof s_root, "%sbo3_modfs_%lu_%lu", tmp, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    return _mkdir(s_root) == 0;
#else
    const char *t = getenv("TMPDIR");
    char tmpl[512], real[PATH_MAX];
    size_t n;
    fit(tmpl, sizeof tmpl, "%s", t && *t ? t : "/tmp");
    for (n = strlen(tmpl); n > 1 && tmpl[n - 1] == '/'; n--)
        tmpl[n - 1] = 0;                            /* macOS: TMPDIR ends in '/' */
    fit(tmpl + n, sizeof tmpl - n, "/bo3_modfs_XXXXXX");
    if (!mkdtemp(tmpl) || !realpath(tmpl, real))
        return 0;
    fit(s_root, sizeof s_root, "%s", real);   /* macOS: /var -> /private/var */
    return 1;
#endif
}

/* Whether the temporary directory's volume tells Aa from AA. */
static int case_sensitive(void)
{
    int sensitive;
    make_file("probe/Aa", "x");
#if defined(_WIN32)
    sensitive = GetFileAttributesA(at("probe/AA")) == INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    sensitive = stat(at("probe/AA"), &st) != 0;
#endif
    return sensitive;
}

/* ---- asking the chain ---- */

static volatile int s_counting;
static int s_prev_calls;

/* The hook that was there before (the soundtrack's, in the game). */
static const char *prev_hook(const char *p)
{
    if (s_counting)
        s_prev_calls++;
    return p && strcmp(p, "E:\\chain") == 0 ? "PREV" : NULL;
}

static const char *ask(const char *guest)
{
    return xbox_PathRedirectHook ? xbox_PathRedirectHook(guest) : NULL;
}

/* What the file the chain names for guest holds ("" when it names none). */
static const char *read_back(const char *guest)
{
    static _Thread_local char text[64];
    const char *host = ask(guest);
    FILE *f;
    size_t n;

    text[0] = 0;
    if (!host || strcmp(host, "PREV") == 0)
        return text;
    f = fopen(host, "rb");
    if (!f)
        return "(cannot open)";
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    return text;
}

static int refused(const char *guest)
{
    const char *r = ask(guest);
    return r == NULL;   /* prev_hook answers NULL for every disc path */
}

/* What the toolkit does with an answer no path rule claims (kernel_path.c,
 * "Unrecognized Xbox path"): on Windows it converts it into a MAX_PATH wide
 * buffer with its terminator, XBOX_GUEST_CP, and does not check the result;
 * elsewhere it copies it into MAX_PATH characters. A model of that hand-off,
 * not the toolkit itself: 1 when host survives it whole. */
static int handoff_ok(const char *host)
{
#if defined(_WIN32)
    WCHAR w[MAX_PATH];
    return MultiByteToWideChar(XBOX_GUEST_CP, 0, host, -1, w, MAX_PATH) != 0;
#else
    return strlen(host) < MAX_PATH;
#endif
}

/* stderr into a file between capture_start and capture_end, then the text. */
static int s_saved_err = -1;
static int capture_start(const char *path)
{
    FILE *f;
    fflush(stderr);
    s_saved_err = dup(fileno(stderr));
    f = fopen(path, "wb");
    if (s_saved_err < 0 || !f || dup2(fileno(f), fileno(stderr)) < 0) {
        if (f)
            fclose(f);
        return 0;
    }
    fclose(f);
    remember(path, 0);
    return 1;
}

static void capture_end(const char *path, char *out, size_t n)
{
    FILE *f;
    size_t got = 0;
    fflush(stderr);
    dup2(s_saved_err, fileno(stderr));
    close(s_saved_err);
    f = fopen(path, "rb");
    if (f) {
        got = fread(out, 1, n - 1, f);
        fclose(f);
    }
    out[got] = 0;
}

static void set_mods(const char *list)
{
    if (list)
        SETENV("TAKEDAWN_MODS", list);
    else
        UNSETENV("TAKEDAWN_MODS");
    bo3_modfs_init();
}

static double now_us(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
#endif
}

/* ---- threads ---- */

#define THREADS 8
#define ROUNDS  2000
static int s_thread_bad[THREADS];

static void thread_body(int k)
{
    for (int i = 0; i < ROUNDS; i++) {
        const char *want = (i + k) % 3 == 0 ? "A static" : (i + k) % 3 == 1 ? "B only" : "";
        const char *guest = (i + k) % 3 == 0 ? "D:\\Tracks\\US\\C3_V1\\static.dat"
                          : (i + k) % 3 == 1 ? "D:\\Data\\only_b.bin" : "D:\\Data\\missing.bin";
        if (strcmp(read_back(guest), want) != 0)
            s_thread_bad[k]++;
    }
}

#if defined(_WIN32)
static unsigned __stdcall thread_main(void *a) { thread_body((int)(intptr_t)a); return 0; }
#else
static void *thread_main(void *a) { thread_body((int)(intptr_t)a); return NULL; }
#endif

static void run_threads(void)
{
#if defined(_WIN32)
    HANDLE t[THREADS];
    for (int k = 0; k < THREADS; k++)
        t[k] = (HANDLE)_beginthreadex(NULL, 0, thread_main, (void *)(intptr_t)k, 0, NULL);
    WaitForMultipleObjects(THREADS, t, TRUE, INFINITE);
    for (int k = 0; k < THREADS; k++)
        CloseHandle(t[k]);
#else
    pthread_t t[THREADS];
    for (int k = 0; k < THREADS; k++)
        pthread_create(&t[k], NULL, thread_main, (void *)(intptr_t)k);
    for (int k = 0; k < THREADS; k++)
        pthread_join(t[k], NULL);
#endif
}

int main(void)
{
    char list[4096];
    int sensitive;

    if (!make_root()) {
        fprintf(stderr, "cannot make a temporary folder\n");
        return 1;
    }
    atexit(remove_all);
    sensitive = case_sensitive();
    printf("folders under %s (%s volume)\n", s_root, sensitive ? "case-sensitive" : "case-folding");

    /* Every fixture is checked: a case whose files are not there would pass
     * for the wrong reason. */
    CHECK(make_file("modA/Tracks/US/C3_V1/static.dat", "A static"), "setup modA");
    CHECK(make_dir(at("modA/Tracks/dirfile")), "setup modA/Tracks/dirfile");
    CHECK(make_file("modB/Tracks/US/C3_V1/static.dat", "B static"), "setup modB");
    CHECK(make_file("modB/Data/only_b.bin", "B only"), "setup modB/Data");
    CHECK(make_file("lower/tracks/us/c3_v1/lower.dat", "lower"), "setup lower");
    CHECK(make_file("outside/secret.dat", "outside"), "setup outside");
    CHECK(make_file("plainfile", "not a folder"), "setup plainfile");
    CHECK(make_file("modN/Data/x.dat", "N plain"), "setup modN");

    /* 1. Unset or empty: no hook at all, whatever was there stays. */
    set_mods(NULL);
    CHECK(xbox_PathRedirectHook == NULL, "unset: no hook installed");
    set_mods("");
    CHECK(xbox_PathRedirectHook == NULL, "empty: no hook installed");
    xbox_PathRedirectHook = prev_hook;
    set_mods(NULL);
    CHECK(xbox_PathRedirectHook == prev_hook, "unset: the earlier hook is left alone");
    set_mods(at("missing"));
    CHECK(xbox_PathRedirectHook == prev_hook, "only a missing folder: no hook installed");

    /* 2. Several folders, the first one named wins; a missing folder and a
     *    file named as one are skipped. */
    {
        static const char *const names[] = { "missing", "plainfile", "modA", "modB", "lower" };
        list[0] = 0;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            fit(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ";" : "", at(names[i]));
    }
    set_mods(list);
    CHECK(xbox_PathRedirectHook != prev_hook && xbox_PathRedirectHook != NULL, "set: the hook is installed");
    CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "A static"), "first folder wins");
    CHECK(!strcmp(read_back("\\Device\\CdRom0\\Tracks\\US\\C3_V1\\static.dat"), "A static"),
          "\\Device\\CdRom0\\ is the disc too");
    CHECK(!strcmp(read_back("d:\\Tracks\\US\\C3_V1\\static.dat"), "A static"), "d:\\ as well as D:\\");
    CHECK(!strcmp(read_back("D:\\Data\\only_b.bin"), "B only"), "a file only the second folder has");
    CHECK(!strcmp(read_back("D:\\Data/only_b.bin"), "B only"), "'/' in the guest path is a separator");
    {
        const char *h = ask("D:\\Tracks\\US\\C3_V1\\static.dat");
        CHECK(h && handoff_ok(h), "the path handed back fits the toolkit's buffer: %s", h ? h : "(none)");
    }

    /* 3. Case: the folder's own spelling, from any spelling the title uses. */
    CHECK(!strcmp(read_back("d:\\tracks\\us\\c3_v1\\STATIC.DAT"), "A static"), "mixed case finds modA's file");
    CHECK(!strcmp(read_back("D:\\TRACKS\\US\\C3_V1\\LOWER.DAT"), "lower"), "upper case finds a lower-case tree");
    {
        const char *h = ask("D:\\TRACKS\\Us\\C3_v1\\Lower.dat");
        CHECK(h && strstr(h, sensitive ? SEP "tracks" SEP "us" SEP "c3_v1" SEP "lower.dat" : "ower.dat"),
              "the host path has the folder's spelling: %s", h ? h : "(none)");
    }

    /* 4. Missing files, and everything that is not a disc path, go down the
     *    chain; a redirected one does not. */
    s_counting = 1;
    s_prev_calls = 0;
    CHECK(refused("D:\\Tracks\\US\\C3_V1\\missing.dat") && s_prev_calls == 1, "missing file: the disc's (chain asked)");
    CHECK(ask("E:\\chain") && !strcmp(ask("E:\\chain"), "PREV"), "a non-disc path reaches the earlier hook");
    CHECK(refused("T:\\Tracks\\US\\C3_V1\\static.dat"), "T:\\ is not the disc");
    CHECK(refused("D:\\"), "the disc's root itself");
    CHECK(refused(NULL), "no path");
    s_prev_calls = 0;
    ask("D:\\Tracks\\US\\C3_V1\\static.dat");
    CHECK(s_prev_calls == 0, "a redirected path is not passed on");
    s_counting = 0;

    /* 5. Names that are refused, the same on every host. Real files stand
     *    behind the non-ASCII and the control-character name (where the file
     *    system takes them), so only the name rule refuses those. */
    if (!make_file("modA/Tracks/US/C3_V1/st\xc3\xa9.dat", "non-ASCII name"))
        SKIP("a file with a non-ASCII name could not be made here");
#if !defined(_WIN32)
    if (!make_file("modA/Tracks/US/C3_V1/stat\x01ic.dat", "control name"))
        SKIP("a file with a control character in its name could not be made here");
#endif
    {
        static const char *const bad[] = {
            "D:\\..\\outside\\secret.dat",
            "D:\\Tracks\\..\\..\\outside\\secret.dat",
            "D:\\Tracks\\US\\..\\US\\C3_V1\\static.dat",
            "D:\\.\\Tracks\\US\\C3_V1\\static.dat",
            "D:\\Tracks\\\\US\\C3_V1\\static.dat",
            "D:\\\\Tracks\\US\\C3_V1\\static.dat",
            "D:\\/Tracks/US/C3_V1/static.dat",
            "D:\\Tracks\\US\\C3_V1\\static.dat\\",
            "D:\\C:\\Windows\\win.ini",
            "D:\\Tracks\\US\\C3_V1\\static.dat:stream",
            "D:\\Tracks\\US\\C3_V1\\static.dat.",
            "D:\\Tracks\\US\\C3_V1\\static.dat ",
            "D:\\Tracks\\US\\C3_V1\\*.dat",
            "D:\\Tracks\\US\\C3_V1\\stat?c.dat",
            "D:\\Tracks\\US\\C3_V1\\st\"atic.dat",
            "D:\\Tracks\\US\\C3_V1\\st<atic.dat",
            "D:\\Tracks\\US\\C3_V1\\st|atic.dat",
            "D:\\Tracks\\US\\C3_V1\\stat\x01ic.dat",
            "D:\\Tracks\\US\\C3_V1\\st\xc3\xa9.dat",
            "D:\\Tracks",
            "D:\\Tracks\\dirfile",
            "D:\\Tracks\\US\\C3_V1",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(refused(bad[i]), "refused: %s", bad[i]);
    }
    {
        char longpart[400], deep[1200];
        memset(longpart, 'a', 300);
        longpart[300] = 0;
        fit(deep, sizeof deep, "D:\\%s", longpart);
        CHECK(refused(deep), "a 300-character name (longer than any file system allows)");
    }

    /* 5b. The longest host path handed back: on Windows 255 characters (the
     *     toolkit puts "\\?\" in front and converts into MAX_PATH wide
     *     characters with the terminator), elsewhere MAX_PATH - 1. Real files
     *     just under, at and just over it: the ones over are refused (the
     *     disc's file is read), the others found and fit the hand-off. */
    {
#if defined(_WIN32)
        const int limit = MAX_PATH - 5, prefix = 4;
#else
        const int limit = MAX_PATH - 1, prefix = 0;
#endif
        char dir[700], name[300], rel[400], guest[400], text[32];
        int lengths[4], made[4];
        fit(dir, sizeof dir, "%s", at("len/Data"));
        for (int i = 0; i < 4; i++) {
            int t = limit - 1 + i, k = t - (int)strlen(dir) - 1;   /* host = dir + SEP + name */
            lengths[i] = t;
            made[i] = 0;
            if (k < 8 || k > 250)
                continue;
            memset(name, 'n', (size_t)k - 4);
            memcpy(name + k - 4, ".bin", 5);
            fit(rel, sizeof rel, "len/Data/%s", name);
            fit(text, sizeof text, "len %d", t);
            made[i] = make_file(rel, text);
        }
        set_mods(at("len"));
        for (int i = 0; i < 4; i++) {
            int t = lengths[i], k = t - (int)strlen(dir) - 1;
            const char *h;
            if (!made[i]) {
                SKIP("host path of %d characters: its file could not be made here", t);
                continue;
            }
            memset(name, 'n', (size_t)k - 4);
            memcpy(name + k - 4, ".bin", 5);
            fit(guest, sizeof guest, "D:\\Data\\%s", name);
            fit(text, sizeof text, "len %d", t);
            h = ask(guest);
            if (t <= limit)
                CHECK(h && (int)strlen(h) == t + prefix && handoff_ok(h) && !strcmp(read_back(guest), text),
                      "host path of %d characters (limit %d): found, fits the hand-off", t, limit);
            else
                CHECK(!h, "host path of %d characters (limit %d): refused, not handed on cut off (%s)",
                      t, limit, h ? h : "none");
        }
    }

    /* 6. Links below a folder are refused; a folder reached through a link
     *    is fine. */
#if defined(_WIN32)
    {
        char cmd[1600];
        CHECK(make_dir(at("modJ")), "setup modJ");
        fit(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL 2>&1", at("modJ/Tracks"), at("outside"));
        if (system(cmd) == 0) {
            remember(at("modJ/Tracks"), 2);
            set_mods(at("modJ"));
            /* Positive control: through the junction the file is there. */
            CHECK(GetFileAttributesA(at("modJ/Tracks/secret.dat")) != INVALID_FILE_ATTRIBUTES,
                  "setup: the junction leads to a real file");
            CHECK(refused("D:\\Tracks\\secret.dat"), "a junction below the folder");
        } else {
            SKIP("junction: mklink /J failed");
        }
        SKIP("symbolic links, pipes, unreadable folders: POSIX cases");
    }

    /* Only folders on a drive letter. The same folder by its drive path is
     * the positive control; written as a network share or with \\?\ or \\.\
     * it would be handed back as "\\?\\\..." and name nothing, so it is
     * ignored and the disc's file is read. */
    {
        static const char *const pre[] = { "\\\\?\\", "\\\\.\\", "//?/" };
        char p[900];
        set_mods(at("modA"));
        CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "A static"), "modA by its drive path");
        for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
            fit(p, sizeof p, "%s%s", pre[i], at("modA"));
            set_mods(p);
            CHECK(refused("D:\\Tracks\\US\\C3_V1\\static.dat"), "ignored: %s", p);
        }
        fit(p, sizeof p, "\\\\localhost\\%c$%s", at("modA")[0], at("modA") + 2);
        set_mods(p);
        CHECK(refused("D:\\Tracks\\US\\C3_V1\\static.dat"), "ignored (network share): %s", p);
        set_mods("\\\\no-such-host.invalid\\share\\mods");
        CHECK(refused("D:\\Tracks\\US\\C3_V1\\static.dat"), "ignored (network share, without asking the network)");
        fit(p, sizeof p, "\\\\localhost\\%c$%s;%s", at("modA")[0], at("modA") + 2, at("modB"));
        set_mods(p);
        CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "B static"),
              "an ignored share does not stop the next folder");
    }
#else
    {
        struct stat st;
        int ok;
        CHECK(make_dir(at("modL")) && make_dir(at("modL/Data")), "setup modL");
        CHECK(make_file("modL/Data/real.dat", "L real"), "setup modL/Data/real.dat");
        fit(list, sizeof list, "%s", at("modL"));
        /* Each link is made, then shown to reach a real file when followed
         * (what the lookup would return without the rule), then refused. */
        ok = symlink(at("outside"), at("modL/Tracks")) == 0;
        if (ok)
            remember(at("modL/Tracks"), 2);
        if (!ok) {
            SKIP("a folder link: symlink failed (%s)", strerror(errno));
        } else {
            set_mods(list);
            CHECK(stat(at("modL/Tracks/secret.dat"), &st) == 0, "setup: the folder link leads to a real file");
            CHECK(refused("D:\\Tracks\\secret.dat"), "a folder link below the folder");
            CHECK(refused("D:\\tracks\\secret.dat"), "a folder link found by case");
        }
        ok = symlink(at("outside/secret.dat"), at("modL/Data/link.dat")) == 0;
        if (ok)
            remember(at("modL/Data/link.dat"), 2);
        if (!ok) {
            SKIP("a file link: symlink failed (%s)", strerror(errno));
        } else {
            set_mods(list);
            CHECK(stat(at("modL/Data/link.dat"), &st) == 0 && S_ISREG(st.st_mode), "setup: the file link leads to a file");
            CHECK(refused("D:\\Data\\link.dat"), "a file link (absolute)");
        }
        ok = symlink("../../outside/secret.dat", at("modL/Data/rel.dat")) == 0;
        if (ok)
            remember(at("modL/Data/rel.dat"), 2);
        if (!ok) {
            SKIP("a relative file link: symlink failed (%s)", strerror(errno));
        } else {
            set_mods(list);
            CHECK(stat(at("modL/Data/rel.dat"), &st) == 0 && S_ISREG(st.st_mode), "setup: the relative link leads to a file");
            CHECK(refused("D:\\Data\\rel.dat"), "a file link (relative, ../..)");
        }
        ok = mkfifo(at("modL/Data/pipe.dat"), 0644) == 0;
        if (ok)
            remember(at("modL/Data/pipe.dat"), 0);
        if (!ok) {
            SKIP("a pipe: mkfifo failed (%s)", strerror(errno));
        } else {
            set_mods(list);
            CHECK(lstat(at("modL/Data/pipe.dat"), &st) == 0 && S_ISFIFO(st.st_mode), "setup: the pipe is there");
            CHECK(refused("D:\\Data\\pipe.dat"), "a pipe (and the lookup did not block on it)");
        }
        set_mods(list);
        CHECK(!strcmp(read_back("D:\\Data\\real.dat"), "L real"), "a real file beside them");
        ok = symlink(at("modA"), at("rootlink")) == 0;
        if (ok)
            remember(at("rootlink"), 2);
        if (!ok) {
            SKIP("a folder named through a link: symlink failed (%s)", strerror(errno));
        } else {
            set_mods(at("rootlink"));
            CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "A static"), "a folder named through a link");
            {
                const char *h = ask("D:\\Tracks\\US\\C3_V1\\static.dat");
                CHECK(h && strstr(h, SEP "modA" SEP), "the link is resolved once, at the start: %s", h ? h : "(none)");
            }
        }

        CHECK(make_file("modP/Data/x.dat", "P"), "setup modP");
        if (geteuid() == 0) {
            SKIP("unreadable folder: running as root, permissions do not stop it");
        } else if (chmod(at("modP/Data"), 0) == 0) {
            for (int i = s_nmade - 1; i >= 0; i--)
                if (!strcmp(s_made[i], at("modP/Data")))
                    s_kind[i] = 3;
            set_mods(at("modP"));
            CHECK(refused("D:\\Data\\x.dat"), "an unreadable folder: refused, no crash");
            CHECK(refused("D:\\data\\x.dat"), "an unreadable folder by case: refused");
            chmod(at("modP/Data"), 0755);
        }

        /* A '\' in a folder's name: the same tree as modN, which is found. */
        if (!make_file("mod\\x/Data/x.dat", "backslash")) {
            SKIP("a folder with a '\\': it could not be made (%s)", strerror(errno));
        } else {
            set_mods(at("modN"));
            CHECK(!strcmp(read_back("D:\\Data\\x.dat"), "N plain"), "the plain twin modN is found");
            set_mods(at("mod\\x"));
            CHECK(refused("D:\\Data\\x.dat"), "a folder with a '\\' is ignored");
        }
    }
#endif

    /* A folder whose path is not plain ASCII, with a real file in it: the
     * same tree as modN (found), so only the folder's name stops it. */
    {
#if defined(_WIN32)
        const char *na = "mod\xe9";           /* the ANSI code page's e-acute */
#else
        const char *na = "mod\xc3\xa9";       /* UTF-8 */
#endif
        char rel[64];
        fit(rel, sizeof rel, "%s/Data/x.dat", na);
        if (!make_file(rel, "non-ASCII")) {
            SKIP("a non-ASCII folder: it could not be made here");
        } else {
            set_mods(at("modN"));
            CHECK(!strcmp(read_back("D:\\Data\\x.dat"), "N plain"), "the plain twin modN is found");
            set_mods(at(na));
            CHECK(refused("D:\\Data\\x.dat"), "a non-ASCII folder is ignored");
        }
    }

    /* 7. Two names that differ only in case: the one as written, else none. */
    if (sensitive) {
        make_file("amb/Data/x.bin", "amb Data");
        make_file("amb/DATA/x.bin", "amb DATA");
        make_file("amb/Exact/Y.bin", "Y");
        make_file("amb/Exact/y.bin", "y");
        fit(list, sizeof list, "%s;%s", at("amb"), at("modB"));
        set_mods(list);
        CHECK(refused("D:\\data\\x.bin"), "Data and DATA for data: refused");
        CHECK(refused("D:\\data\\only_b.bin"), "...and not taken from a later folder either");
        CHECK(!strcmp(read_back("D:\\Data\\x.bin"), "amb Data"), "Data as written");
        CHECK(!strcmp(read_back("D:\\DATA\\x.bin"), "amb DATA"), "DATA as written");
        CHECK(!strcmp(read_back("D:\\Exact\\y.bin"), "y") && !strcmp(read_back("D:\\Exact\\Y.bin"), "Y"),
              "files: as written");
        CHECK(refused("D:\\exact\\Y.BIN"), "Y.bin and y.bin for Y.BIN: refused");
        {
            int same = 1;
            for (int i = 0; i < 200; i++)
                same &= refused("D:\\data\\x.bin") && !strcmp(read_back("D:\\Data\\x.bin"), "amb Data");
            CHECK(same, "the same answer every time");
        }
    } else {
        SKIP("names that differ only in case: this volume folds case");
    }

    /* 8. The list: ';' only, empty items, trailing separators, a relative
     *    folder, more than eight. */
    fit(list, sizeof list, ";;%s" SEP SEP ";;", at("modB"));
    set_mods(list);
    CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "B static"), "empty items and trailing separators");
    fit(list, sizeof list, "%s:%s", at("modA"), at("modB"));
    set_mods(list);
    CHECK(refused("D:\\Tracks\\US\\C3_V1\\static.dat"), "':' does not separate folders");
    {
        char here[1024];
#if defined(_WIN32)
        if (_getcwd(here, sizeof here) && _chdir(s_root) == 0) {
            set_mods("modB");
            _chdir(here);
#else
        if (getcwd(here, sizeof here) && chdir(s_root) == 0) {
            set_mods("modB");
            if (chdir(here) != 0)
                perror("chdir");
#endif
            CHECK(!strcmp(read_back("D:\\Data\\only_b.bin"), "B only"), "a relative folder, taken at the start");
        }
    }
    list[0] = 0;
    for (int i = 0; i < 8; i++)
        fit(list + strlen(list), sizeof list - strlen(list), "%s;", at("modB"));
    fit(list + strlen(list), sizeof list - strlen(list), "%s", at("modA"));
    set_mods(list);
    CHECK(!strcmp(read_back("D:\\Tracks\\US\\C3_V1\\static.dat"), "B static"), "a ninth folder is ignored");

    /* 9. Threads: each gets its own answer. */
    fit(list, sizeof list, "%s;%s", at("modA"), at("modB"));
    set_mods(list);
    run_threads();
    {
        int bad = 0;
        for (int k = 0; k < THREADS; k++)
            bad += s_thread_bad[k];
        CHECK(bad == 0, "%d threads x %d lookups: %d wrong", THREADS, ROUNDS, bad);
    }

    /* 10. What a lookup costs (not checked: printed for the record). */
    {
        static const char *const kind[] = { "hit, as written", "hit, by case", "miss, first part",
                                             "miss, last part" };
        static const char *const guest[] = { "D:\\Tracks\\US\\C3_V1\\static.dat", "D:\\TRACKS\\us\\c3_v1\\Static.Dat",
                                             "D:\\Sound\\DSP\\B3XboxFx.bin", "D:\\Tracks\\US\\C3_V1\\enviro.dat" };
        fit(list, sizeof list, "%s;%s;%s", at("modA"), at("modB"), at("lower"));
        set_mods(list);
        for (int g = 0; g < 4; g++) {
            double t0 = now_us();
            for (int i = 0; i < 2000; i++)
                ask(guest[g]);
            printf("cost: %-17s %6.2f us a lookup (3 folders)\n", kind[g], (now_us() - t0) / 2000);
        }
    }

    /* 11. What a release build says. The print gate drops plain stderr lines
     *     while diagnostic logging is off (g_xr_log_diag 0, what xr_log_init
     *     sets for a release build): the folders taken and ignored must still
     *     be there (XR_NOTICE), the per-file lines only with diagnostics. */
    {
        static char log[8192];
        char path[700];
        fit(path, sizeof path, "%s", at("log_release.txt"));
        fit(list, sizeof list, "%s;%s", at("missing"), at("modA"));
        g_xr_log_diag = 0;
        if (!capture_start(path)) {
            g_xr_log_diag = 1;
            SKIP("release log: stderr could not be captured");
        } else {
            set_mods(list);
            ask("D:\\Tracks\\US\\C3_V1\\static.dat");
#if defined(_WIN32)
            set_mods("\\\\no-such-host.invalid\\share\\mods");
#endif
            capture_end(path, log, sizeof log);
            g_xr_log_diag = 1;
            CHECK(strstr(log, "[MOD] ignored (not a folder): "), "release: an ignored folder is said:\n%s", log);
            CHECK(strstr(log, "[MOD] folder 1: "), "release: the folder taken is said:\n%s", log);
#if defined(_WIN32)
            CHECK(strstr(log, "[MOD] ignored (not on a drive letter"), "release: a network share is said:\n%s", log);
#endif
            CHECK(!strstr(log, "static.dat -> "), "release: no per-file line without diagnostics:\n%s", log);
        }
        fit(path, sizeof path, "%s", at("log_diag.txt"));
        if (capture_start(path)) {
            set_mods(list);   /* a new start: the log-once list is empty again */
            ask("D:\\Tracks\\US\\C3_V1\\static.dat");
            capture_end(path, log, sizeof log);
            CHECK(strstr(log, "[MOD] folder 1: ") && strstr(log, "static.dat -> "),
                  "diagnostics on: the folder and the per-file line:\n%s", log);
        } else {
            SKIP("diagnostic log: stderr could not be captured");
        }
    }

    remove_all();
    printf("modfs: %d passed, %d failed, %d skipped\n", s_pass, s_fail, s_skip);
    return s_fail ? 1 : 0;
}
