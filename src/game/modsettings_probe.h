/**
 * modsettings_probe.h - Name the module that makes BG3 reset the load order.
 */
#ifndef BG3SE_MODSETTINGS_PROBE_H
#define BG3SE_MODSETTINGS_PROBE_H
#include <stdbool.h>
bool modsettings_probe_init(void *binary_base);

/** Remove a leftover ModCrashSanityCheck folder (see modsettings_probe.c).
 *  Call from the dylib constructor, before the game reads its config. */
void modsettings_clear_mod_crash_marker(void);
#endif
