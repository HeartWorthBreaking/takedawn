/* Native plugins (src/game/plugins.c over modfs.c and modmanifest.c): which
 * modules are loaded, approved, started and refused, on mod folders the test
 * makes in the temporary directory. The modules are tests/plugins/test_module.c
 * built per kind, and the example plugin; each records its own path in a
 * marker file the moment the system loads it, so "never loaded" is checked
 * directly. No game data, no guest.
 *   plugins_test <folder of the built test modules> <example plugin> [log copy] */
#include "modfs.h"
#include "plugins.h"
#include "xr_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define EXT ".asi"
#define MKDIR(p) _mkdir(p)
static void set_env(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }
#else
#include <sys/stat.h>
#include <unistd.h>
#define EXT ".so"
#define MKDIR(p) mkdir(p, 0755)
static void set_env(const char *k, const char *v) { if (v) setenv(k, v, 1); else unsetenv(k); }
#endif

/* The toolkit's, which modfs.c chains onto (tests/modfs does the same). */
const char *(*xbox_PathRedirectHook)(const char *xbox_path);

static int s_fail, s_pass;
#define CHECK(c, ...) do { if (c) s_pass++; else { s_fail++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); putchar('\n'); } } while (0)

static char s_tmp[600], s_modules[600], s_hello[600], s_log[700];

static void mkdirs(const char *path)
{
    char p[900];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
            MKDIR(p);
            *c = k;
        }
    MKDIR(p);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    b[n] = 0;
    fclose(f);
    return b;
}

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[8192];
    size_t n;
    if (!a || !b) {
        printf("cannot copy %s -> %s\n", from, to);
        exit(2);
    }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
}

static void sha_of(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    unsigned char buf[8192], d[32];
    xr_sha256 s;
    size_t n;
    xr_sha256_init(&s);
    while (f && (n = fread(buf, 1, sizeof buf, f)) > 0)
        xr_sha256_update(&s, buf, n);
    if (f)
        fclose(f);
    xr_sha256_final(&s, d);
    xr_sha256_hex(d, hex);
}

/* ---- a scenario: <tmp>/<name>/{mods,markers}, a mod list, approvals ---- */

static char s_dir[700], s_approvals[4096];

static void scenario(const char *name)
{
    char p[800];
    snprintf(s_dir, sizeof s_dir, "%s/%s", s_tmp, name);
    snprintf(p, sizeof p, "%s/mods", s_dir);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/markers", s_dir);
    mkdirs(p);
    set_env("TP_MARKER_DIR", p);
    set_env("TAKEDAWN_MODS", NULL);
    set_env("TAKEDAWN_PLUGINS", NULL);
    s_approvals[0] = 0;
}

/* A mod <id> in folder <sub>: its mod.toml has body after the id and name;
 * kind (a built test module, or NULL) is copied to rel; approve: 1 for its
 * real hash, 2 for a wrong one. */
static void mod(const char *sub, const char *id, const char *body, const char *kind, const char *rel, int approve)
{
    char dir[800], p[1000], t[1500], hex[65];
    snprintf(dir, sizeof dir, "%s/mods/%s", s_dir, sub);
    mkdirs(dir);
    snprintf(t, sizeof t, "id = \"%s\"\nname = \"%s\"\n%s", id, id, body);
    snprintf(p, sizeof p, "%s/mod.toml", dir);
    write_file(p, t);
    if (kind) {
        char from[800], parent[1000], *e;
        snprintf(p, sizeof p, "%s/%s", dir, rel);
        snprintf(parent, sizeof parent, "%s", p);
        e = strrchr(parent, '/');
        if (e) {
            *e = 0;
            mkdirs(parent);
        }
        if (!strcmp(kind, "hello"))
            snprintf(from, sizeof from, "%s", s_hello);
        else
            snprintf(from, sizeof from, "%s/%s" EXT, s_modules, kind);
        copy_file(from, p);
        sha_of(p, hex);
        if (approve) {
            size_t n = strlen(s_approvals);
            snprintf(s_approvals + n, sizeof s_approvals - n, "%s\"%s sha256:%s\"", n ? ", " : "", id,
                     approve == 1 ? hex : "0000000000000000000000000000000000000000000000000000000000000000");
        }
    }
}

/* Writes the approvals (unless own is given), the mod list, and starts. */
static void start(const char *disabled, const char *own_approvals, int no_file)
{
    char p[800], t[4400];
    snprintf(p, sizeof p, "%s/mods", s_dir);
    set_env("TAKEDAWN_MODS", p);
    snprintf(p, sizeof p, "%s/mod-list.toml", s_dir);
    snprintf(t, sizeof t, "disabled = [%s]\n", disabled ? disabled : "");
    write_file(p, t);
    bo3_modfs_state_file(p);
    bo3_modfs_init();
    snprintf(p, sizeof p, "%s/approvals.toml", s_dir);
    if (!no_file) {
        snprintf(t, sizeof t, "%s", own_approvals ? own_approvals : "");
        if (!own_approvals)
            snprintf(t, sizeof t, "# test\napproved = [%s]\n", s_approvals);
        write_file(p, t);
    }
    bo3_plugins_approval_file(p);
    fflush(stderr);
    bo3_plugins_init("9.9.9-test");
    fflush(stderr);
}

static int index_of(const char *id)
{
    for (int i = 0; i < bo3_modfs_mod_count(); i++)
        if (!strcmp(bo3_modfs_mod(i)->manifest.id, id))
            return i;
    return -1;
}

static bo3_plugin_state state(const char *id, char *why)
{
    return bo3_plugins_state(index_of(id), why, 200, NULL, 0);
}

/* 1 when a module in mod folder <sub> of this scenario was loaded. */
static int loaded(const char *sub)
{
    char p[800], want[800], *t;
    int hit;
    snprintf(p, sizeof p, "%s/markers/loaded", s_dir);
    t = read_file(p);
    if (!t)
        return 0;
    snprintf(want, sizeof want, "%cmods%c%s%c", '\\', '\\', sub, '\\');
    hit = strstr(t, want) != NULL;
    snprintf(want, sizeof want, "/mods/%s/", sub);
    hit |= strstr(t, want) != NULL;
    free(t);
    return hit;
}

static char *marker(const char *name)
{
    char p[800];
    snprintf(p, sizeof p, "%s/markers/%s", s_dir, name);
    return read_file(p);
}

static const char *plat(void)
{
    return bo3_manifest_platform();
}

static void declare(char *out, size_t n, int api, const char *rel)
{
    snprintf(out, n, "[plugin]\napi = %d\n%s = \"%s\"\n", api, plat(), rel);
}

int main(int argc, char **argv)
{
    char why[200], b[400], *t;

    if (argc < 3) {
        printf("usage: plugins_test <modules folder> <example plugin> [log copy]\n");
        return 2;
    }
    snprintf(s_modules, sizeof s_modules, "%s", argv[1]);
    snprintf(s_hello, sizeof s_hello, "%s", argv[2]);
#if defined(_WIN32)
    {
        char base[MAX_PATH];
        GetTempPathA(sizeof base, base);
        snprintf(s_tmp, sizeof s_tmp, "%sbo3_plugins_%lu_%lu", base, (unsigned long)GetCurrentProcessId(),
                 (unsigned long)GetTickCount());
        for (char *c = s_tmp; *c; c++)
            if (*c == '\\')
                *c = '/';
    }
#else
    snprintf(s_tmp, sizeof s_tmp, "/tmp/bo3_plugins_XXXXXX");
    if (!mkdtemp(s_tmp))
        return 2;
#endif
    mkdirs(s_tmp);
    snprintf(s_log, sizeof s_log, "%s/log.txt", s_tmp);
    if (!freopen(s_log, "w", stderr)) {
        printf("cannot redirect stderr\n");
        return 2;
    }
    set_env("TAKEDAWN_MOD_LIST", NULL);

    /* s0: installed mods without native code, and no mods at all: nothing is said, nothing runs */
    scenario("s0");
    mod("asset", "a.asset", "", NULL, NULL, 0);
    start(NULL, NULL, 0);
    CHECK(bo3_plugins_running() == 0 && state("a.asset", why) == BO3_PLUGIN_NONE, "s0: a data mod has no plugin");
    t = read_file(s_log);
    CHECK(t && !strstr(t, "[PLUGIN"), "s0: no [PLUGIN] line without a declared plugin");
    free(t);

    /* s1: the main matrix */
    scenario("s1");
    mod("asset", "a.asset", "", NULL, NULL, 0);
    declare(b, sizeof b, 1, "bin/good" EXT);
    mod("good", "t.good", b, "good", "bin/good" EXT, 1);
    mod("notapproved", "t.notapproved", b, "marker", "bin/good" EXT, 0);
    mod("wronghash", "t.wronghash", b, "marker", "bin/good" EXT, 2);
    {
        char mixed[600];
        snprintf(mixed, sizeof mixed, "%s", b);
        mod("disabled", "t.disabled", mixed, "marker", "bin/good" EXT, 1);
        snprintf(mixed, sizeof mixed, "%s/mods/disabled/files", s_dir);
        mkdirs(mixed);
        strcat(mixed, "/x.dat");
        write_file(mixed, "data");
    }
    mod("stray", "t.stray", b, "good", "bin/good" EXT, 1);
    {
        char p[900], src[700];
        snprintf(src, sizeof src, "%s/marker" EXT, s_modules);   /* undeclared modules: must never load */
        snprintf(p, sizeof p, "%s/mods/stray/stray" EXT, s_dir);
        copy_file(src, p);
        snprintf(p, sizeof p, "%s/mods/stray/files", s_dir);
        mkdirs(p);
        strcat(p, "/evil" EXT);
        copy_file(src, p);
        snprintf(p, sizeof p, "%s/mods/stray/bin/other" EXT, s_dir);
        copy_file(src, p);
    }
    mod("noinit", "t.noinit", b, "noinit", "bin/good" EXT, 1);
    mod("noversion", "t.noversion", b, "noversion", "bin/good" EXT, 1);
    mod("newversion", "t.newversion", b, "newversion", "bin/good" EXT, 1);
    {
        char b2[400];
        declare(b2, sizeof b2, 2, "bin/good" EXT);
        mod("api2", "t.api2", b2, "marker", "bin/good" EXT, 1);
    }
    mod("initfail", "t.initfail", b, "initfail", "bin/good" EXT, 1);
    {
        char p[900];
        snprintf(p, sizeof p, "%s/mods/initfail/files", s_dir);
        mkdirs(p);
        strcat(p, "/y.dat");
        write_file(p, "data");
    }
    {
        char b2[400];
        declare(b2, sizeof b2, 1, "bin/none" EXT);
        mod("missing", "t.missing", b2, NULL, NULL, 0);
        declare(b2, sizeof b2, 1, "bin/dir" EXT);
        mod("dirmod", "t.dirmod", b2, NULL, NULL, 0);
        snprintf(b2, sizeof b2, "%s/mods/dirmod/bin/dir" EXT, s_dir);
        mkdirs(b2);
    }
    mod("order_b", "t.order.b", b, "order_b", "bin/good" EXT, 1);
    mod("order_a", "t.order.a", b, "order_a", "bin/good" EXT, 1);
    mod("changed", "t.changed", b, "marker", "bin/good" EXT, 1);
    {
        char p[900];
        FILE *f;
        snprintf(p, sizeof p, "%s/mods/changed/bin/good" EXT, s_dir);
        f = fopen(p, "ab");              /* the bytes change after the approval */
        if (f) {
            fputc(0, f);
            fclose(f);
        }
    }
    mod("otheros", "t.otheros", "[plugin]\napi = 1\nandroid = \"lib/x.so\"\n", NULL, NULL, 0);
    if (!strcmp(plat(), "android"))
        mod("otheros", "t.otheros", "[plugin]\napi = 1\nmacos = \"lib/x.dylib\"\n", NULL, NULL, 0);
    mod("invalid", "t.invalid", "[plugin]\napi = 1\nwindows = \"../good.asi\"\n", NULL, NULL, 0);
    {
        char b2[400];
        declare(b2, sizeof b2, 1, "hello" EXT);
        mod("hello", "example.hello-plugin", b2, "hello", "hello" EXT, 1);
    }
    start("\"t.disabled\"", NULL, 0);

    CHECK(state("a.asset", why) == BO3_PLUGIN_NONE, "asset-only mod: no plugin");
    CHECK(bo3_modfs_mod(index_of("a.asset"))->usable && bo3_modfs_mod(index_of("a.asset"))->enabled, "asset-only mod on");
    CHECK(state("t.good", why) == BO3_PLUGIN_RUNNING && loaded("good"), "approved plugin runs: %s", why);
    t = marker("init.t.good");
    CHECK(t && strstr(t, "id=t.good api=1") && strstr(t, "version=9.9.9-test"), "init got its mod id, API 1, host version: %s",
          t ? t : "(no marker)");
    free(t);
    CHECK(state("t.notapproved", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("notapproved"), "not approved: never loaded");
    CHECK(state("t.wronghash", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("wronghash"), "approved for other bytes: never loaded");
    CHECK(state("t.disabled", why) == BO3_PLUGIN_OFF && !loaded("disabled"), "a mod that is off: its module never loaded");
    CHECK(!bo3_modfs_mod(index_of("t.disabled"))->enabled, "the mixed mod is off");
    CHECK(state("t.stray", why) == BO3_PLUGIN_RUNNING, "declared module of the stray mod runs: %s", why);
    t = marker("loaded");
    CHECK(t && !strstr(t, "stray" EXT) && !strstr(t, "evil" EXT) && !strstr(t, "other" EXT),
          "undeclared modules beside it, in bin and in files are never loaded");
    free(t);
    CHECK(state("t.noinit", why) == BO3_PLUGIN_REFUSED && strstr(why, "takedawn_plugin_init"), "no init export: refused (%s)", why);
    CHECK(loaded("noinit"), "no init export: the library was loaded (its start-up code ran) - documented");
    CHECK(state("t.noversion", why) == BO3_PLUGIN_REFUSED && strstr(why, "takedawn_plugin_api_version"), "no version export: %s", why);
    CHECK(state("t.newversion", why) == BO3_PLUGIN_REFUSED && strstr(why, "built for plugin API 2"), "version mismatch: %s", why);
    t = marker("init.t.newversion");
    CHECK(!t, "version mismatch: init never called");
    free(t);
    CHECK(state("t.api2", why) == BO3_PLUGIN_REFUSED && strstr(why, "needs plugin API 2") && !loaded("api2"),
          "a newer plugin.api: refused before loading (%s)", why);
    CHECK(state("t.initfail", why) == BO3_PLUGIN_FAILED && strstr(why, "returned 1"), "init failure: %s", why);
    CHECK(bo3_modfs_mod(index_of("t.initfail"))->usable && bo3_modfs_mod(index_of("t.initfail"))->enabled,
          "init failure: the mod's files are still in use");
    CHECK(state("t.missing", why) == BO3_PLUGIN_REFUSED && strstr(why, "not a regular file"), "missing module: %s", why);
    CHECK(state("t.dirmod", why) == BO3_PLUGIN_REFUSED, "a folder named like a module: refused (%s)", why);
    t = marker("order");
    CHECK(t && !strcmp(t, "AB"), "load order is the mods' order (by id): %s", t ? t : "(none)");
    free(t);
    CHECK(state("t.changed", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("changed"), "changed bytes need a new approval");
    CHECK(state("t.otheros", why) == BO3_PLUGIN_NO_MODULE && bo3_modfs_mod(index_of("t.otheros"))->usable,
          "a module only for another system: no module here, the mod still works");
    CHECK(index_of("t.invalid") < 0 && !loaded("invalid"), "an unsafe module path makes the mod invalid, nothing loaded");
    CHECK(state("example.hello-plugin", why) == BO3_PLUGIN_RUNNING, "the example plugin runs: %s", why);
    CHECK(bo3_plugins_running() == 5, "running: good, stray, order a, order b, hello and none else (%d)", bo3_plugins_running());
    {
        char sha[65];
        bo3_plugins_state(index_of("t.notapproved"), NULL, 0, sha, sizeof sha);
        CHECK(strlen(sha) == 64, "the hash of a refused module is kept for the approval line");
    }

    /* s2: TAKEDAWN_PLUGINS=0 */
    scenario("s2");
    declare(b, sizeof b, 1, "bin/good" EXT);
    mod("good", "t.good", b, "marker", "bin/good" EXT, 1);
    set_env("TAKEDAWN_PLUGINS", "0");
    start(NULL, NULL, 0);
    CHECK(state("t.good", why) == BO3_PLUGIN_DISABLED && !loaded("good") && bo3_plugins_running() == 0,
          "TAKEDAWN_PLUGINS=0: nothing loaded (%s)", why);
    set_env("TAKEDAWN_PLUGINS", NULL);

    /* s3: an approval file that is not a list approves nothing */
    scenario("s3");
    mod("good", "t.good", b, "marker", "bin/good" EXT, 1);
    start(NULL, "approved = 5\n", 0);
    CHECK(state("t.good", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("good"), "a bad approval file approves nothing");

    /* s4: no approval file at all, and an empty TAKEDAWN_PLUGIN_APPROVALS */
    scenario("s4");
    mod("good", "t.good", b, "marker", "bin/good" EXT, 1);
    start(NULL, NULL, 1);
    CHECK(state("t.good", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("good"), "no approval file: not approved");
    bo3_plugins_approval_file("");
    bo3_plugins_init("x");
    CHECK(state("t.good", why) == BO3_PLUGIN_NOT_APPROVED && !loaded("good"), "approvals off: not approved");

#if defined(_WIN32)
    /* s5: the module reached through a junction is refused */
    scenario("s5");
    {
        char cmd[2000], real[900], link[900];
        declare(b, sizeof b, 1, "lnk/good" EXT);
        mod("junction", "t.junction", b, "marker", "real/good" EXT, 1);
        snprintf(real, sizeof real, "%s/mods/junction/real", s_dir);
        snprintf(link, sizeof link, "%s/mods/junction/lnk", s_dir);
        for (char *c = real; *c; c++) if (*c == '/') *c = '\\';
        for (char *c = link; *c; c++) if (*c == '/') *c = '\\';
        snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", link, real);
        if (system(cmd) == 0) {
            /* the approval is for real/good: the same bytes, so only the path rule refuses it */
            start(NULL, NULL, 0);
            CHECK(state("t.junction", why) == BO3_PLUGIN_REFUSED && !loaded("junction"), "a junction on the path: refused (%s)", why);
        } else {
            printf("SKIP: junction (mklink /J failed)\n");
        }
    }
#endif

    fflush(stderr);
    t = read_file(s_log);
    CHECK(t && strstr(t, "[PLUGIN example.hello-plugin] hello from API 1"), "the example's line is in the log");
    CHECK(t && strstr(t, "[PLUGIN t.good] warning: a line with?a break and a tab?in it"), "log lines are one line, prefixed, levelled");
    CHECK(t && strstr(t, "add \"t.notapproved sha256:"), "the log names the approval line to add");
    CHECK(t && strstr(t, "approval file not used"), "a bad approval file is reported");
    CHECK(t && strstr(t, "plugins are off (TAKEDAWN_PLUGINS=0)"), "the kill switch is reported");
    if (argc > 3 && t)
        write_file(argv[3], t);
    free(t);
    printf("plugins: %d passed, %d failed (temp %s)\n", s_pass, s_fail, s_tmp);
    return s_fail ? 1 : 0;
}
