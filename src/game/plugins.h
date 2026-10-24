/*
 * plugins.h - native plugins in installed mods (docs/native-plugins.md).
 *
 * A mod that declares [plugin] in its mod.toml brings a shared library. At
 * the start, after bo3_modfs_init, bo3_plugins_init loads the plugins that may
 * run, once, on the main thread, before the game itself starts, in the mods'
 * order (modfs.h). A plugin runs only when all of these hold:
 *
 *   - plugins are not turned off (TAKEDAWN_PLUGINS=0) and this system loads
 *     them (Windows, Linux; macOS only with TAKEDAWN_PLUGINS=1; never Android);
 *   - its mod is usable and on (the mod list, modfs.h);
 *   - its mod.toml names a module for this system, for a plugin API version
 *     this Takedawn has;
 *   - the module is a regular file in the mod's folder, reached without a
 *     link or reparse point;
 *   - the approval file lists the mod's id with the SHA-256 of the module's
 *     bytes as they are now (changed bytes need a new approval).
 *
 * A plugin is never unloaded; a change takes effect at the next start. The
 * list is made once and does not change afterwards: readers need no lock.
 */
#ifndef BO3_PLUGINS_H
#define BO3_PLUGINS_H

#include <stddef.h>

typedef enum {
    BO3_PLUGIN_NONE = 0,        /* the mod declares no native code */
    BO3_PLUGIN_NO_MODULE,       /* [plugin] without a module for this system */
    BO3_PLUGIN_OFF,             /* the mod is off or not usable */
    BO3_PLUGIN_DISABLED,        /* plugins off (TAKEDAWN_PLUGINS) or not loaded on this system */
    BO3_PLUGIN_NOT_APPROVED,    /* not in the approval file for these bytes */
    BO3_PLUGIN_REFUSED,         /* checked and not run: why says */
    BO3_PLUGIN_FAILED,          /* loaded, its init returned an error */
    BO3_PLUGIN_RUNNING          /* loaded and initialised */
} bo3_plugin_state;

/* The approval file, before bo3_plugins_init. NULL or "": none (no plugin
 * can be approved). */
void bo3_plugins_approval_file(const char *path);
/* Load the plugins that may run (above). host_version: Takedawn's version
 * for td_host_api.host_version. The unit test calls it more than once. */
void bo3_plugins_init(const char *host_version);
/* Mod i's plugin (modfs.h numbering): its state, why (when REFUSED / FAILED /
 * NOT_APPROVED, else "") into why, and the module's SHA-256 as 64 hex digits
 * into sha ("" when not hashed). Either buffer may be NULL. */
bo3_plugin_state bo3_plugins_state(int i, char *why, size_t wn, char *sha, size_t sn);
/* Plugins running in this session. */
int bo3_plugins_running(void);

#endif
