/*
 * test_module.c - the modules tests/plugins loads, one source built once per
 * TP_KIND_* (tests/plugins/CMakeLists.txt). Every one, the moment the system
 * loads it (DllMain / a constructor, before Takedawn can look at its
 * exports), appends its own full path to <TP_MARKER_DIR>/loaded: a module
 * that must not run must not be in that file. What each does in init:
 *
 *   GOOD, MARKER   both exports; init writes <dir>/init.<mod id> with what the
 *                  API gave it, logs a line, returns TD_OK
 *   NOINIT         no takedawn_plugin_init
 *   NOVERSION      no takedawn_plugin_api_version
 *   NEWVERSION     says it was built for API 2
 *   INITFAIL       init writes its marker and returns TD_ERR_INIT
 *   ORDER_A / _B   init appends A / B to <dir>/order
 */
#include "takedawn_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void append(const char *name, const char *text)
{
    const char *dir = getenv("TP_MARKER_DIR");
    char path[1024];
    FILE *f;
    if (!dir)
        return;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID reserved)
{
    (void)reserved;
    if (why == DLL_PROCESS_ATTACH) {
        char me[MAX_PATH + 2];
        DWORD n = GetModuleFileNameA(inst, me, MAX_PATH);
        me[n] = 0;
        strcat(me, "\n");
        append("loaded", me);
    }
    return TRUE;
}
#else
static void marker(void) {}
__attribute__((constructor)) static void on_load(void)
{
    Dl_info d;
    char me[1100];
    if (dladdr((void *)marker, &d) && d.dli_fname) {
        snprintf(me, sizeof me, "%s\n", d.dli_fname);
        append("loaded", me);
    }
}
#endif

#if !defined(TP_KIND_NOVERSION)
TD_EXPORT uint32_t TD_CALL takedawn_plugin_api_version(void)
{
#if defined(TP_KIND_NEWVERSION)
    return 2;
#else
    return TD_PLUGIN_API_VERSION;
#endif
}
#endif

#if !defined(TP_KIND_NOINIT)
TD_EXPORT int32_t TD_CALL takedawn_plugin_init(const td_host_api *api, td_plugin_info *info)
{
    char name[128], text[512];
    const char *id = api->mod_id(api->host);
    (void)info;
#if defined(TP_KIND_ORDER_A)
    append("order", "A");
#elif defined(TP_KIND_ORDER_B)
    append("order", "B");
#endif
    snprintf(name, sizeof name, "init.%s", id);
    snprintf(text, sizeof text, "id=%s api=%u size=%u version=%s\n", id, (unsigned)api->api_version,
             (unsigned)api->struct_size, api->host_version(api->host));
    append(name, text);
    api->log(api->host, TD_LOG_INFO, "test module started");
    api->log(api->host, TD_LOG_WARN, "a line with\na break and a tab\tin it");
#if defined(TP_KIND_INITFAIL)
    return TD_ERR_INIT;
#else
    return TD_OK;
#endif
}
#endif
