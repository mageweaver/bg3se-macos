/**
 * modsettings_probe.h - Name the module that makes BG3 reset the load order.
 */
#ifndef BG3SE_MODSETTINGS_PROBE_H
#define BG3SE_MODSETTINGS_PROBE_H
#include <stdbool.h>
bool modsettings_probe_init(void *binary_base);
#endif
