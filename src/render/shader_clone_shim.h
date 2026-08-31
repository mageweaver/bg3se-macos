#ifndef SHADER_CLONE_SHIM_H
#define SHADER_CLONE_SHIM_H

#include <stdbool.h>

// Aliases clone-named shader lookups (CHAR_Hair_<uuid>_STI_DEF) to their base
// shader on miss; fixes the modded-hair/head pipeline-wait freeze on macOS.
// See the .c for the full mechanism.
bool shader_clone_shim_init(void *binary_base);

#endif
