/*
 * plugins_posix.c - loading a native plugin on Linux and macOS
 * (plugins_platform.h); Android builds say plugins are not supported.
 *
 * The module is opened without following a link (O_NOFOLLOW), checked to be
 * a regular file and hashed through that descriptor, then loaded by its
 * absolute path with dlopen(RTLD_NOW | RTLD_LOCAL): every symbol is resolved
 * at once, none is shared with what loads later. Nothing stops another
 * program from replacing the file between the hash and the load, so after
 * loading the path must still name the file that was hashed (same device and
 * inode), else it is refused (it stays loaded: dlopen has already run its
 * constructors). Its own dependencies are found the system's way (rpath,
 * LD_LIBRARY_PATH): docs/native-plugins.md says so.
 */
#include "plugins_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)

int bo3_plugin_sys_supported(void) { return 0; }
bo3_plugin_file *bo3_plugin_sys_hold(const char *path, uint8_t sha[32], char *why, size_t wn)
{
    (void)path;
    (void)sha;
    snprintf(why, wn, "this system does not load plugins");
    return NULL;
}
void *bo3_plugin_sys_load(bo3_plugin_file *f, char *why, size_t wn)
{
    (void)f;
    snprintf(why, wn, "this system does not load plugins");
    return NULL;
}
void bo3_plugin_sys_release(bo3_plugin_file *f) { (void)f; }
void *bo3_plugin_sys_symbol(void *lib, const char *name)
{
    (void)lib;
    (void)name;
    return NULL;
}

#else

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xr_sha256.h"

struct bo3_plugin_file {
    int fd;
    struct stat st;
    char path[4096];
};

int bo3_plugin_sys_supported(void)
{
    return 1;
}

bo3_plugin_file *bo3_plugin_sys_hold(const char *path, uint8_t sha[32], char *why, size_t wn)
{
    bo3_plugin_file *f;
    xr_sha256 s;
    static unsigned char buf[1 << 16];
    ssize_t got;

    if (path[0] != '/' || strlen(path) >= sizeof f->path) {
        snprintf(why, wn, "the module path is not absolute, or too long");
        return NULL;
    }
    f = calloc(1, sizeof *f);
    if (!f) {
        snprintf(why, wn, "out of memory");
        return NULL;
    }
    strcpy(f->path, path);
    f->fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (f->fd < 0) {
        snprintf(why, wn, "the module cannot be opened (%s)", strerror(errno));
        free(f);
        return NULL;
    }
    if (fstat(f->fd, &f->st) != 0 || !S_ISREG(f->st.st_mode)) {
        snprintf(why, wn, "the module is not a regular file");
        bo3_plugin_sys_release(f);
        return NULL;
    }
    xr_sha256_init(&s);
    while ((got = read(f->fd, buf, sizeof buf)) != 0) {
        if (got < 0) {
            if (errno == EINTR)
                continue;
            snprintf(why, wn, "the module cannot be read (%s)", strerror(errno));
            bo3_plugin_sys_release(f);
            return NULL;
        }
        xr_sha256_update(&s, buf, (size_t)got);
    }
    xr_sha256_final(&s, sha);
    return f;
}

void *bo3_plugin_sys_load(bo3_plugin_file *f, char *why, size_t wn)
{
    void *lib = dlopen(f->path, RTLD_NOW | RTLD_LOCAL);
    struct stat now;

    if (!lib) {
        const char *e = dlerror();
        snprintf(why, wn, "the module does not load (%s)", e ? e : "no reason given");
        return NULL;
    }
    if (lstat(f->path, &now) != 0 || !S_ISREG(now.st_mode) || now.st_dev != f->st.st_dev || now.st_ino != f->st.st_ino) {
        snprintf(why, wn, "the module changed while it was loaded (it stays loaded)");
        return NULL;
    }
    return lib;
}

void bo3_plugin_sys_release(bo3_plugin_file *f)
{
    if (!f)
        return;
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

void *bo3_plugin_sys_symbol(void *lib, const char *name)
{
    return dlsym(lib, name);
}

#endif
