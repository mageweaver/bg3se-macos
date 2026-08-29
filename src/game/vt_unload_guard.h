#ifndef VT_UNLOAD_GUARD_H
#define VT_UNLOAD_GUARD_H

#include <stdbool.h>

// Engine bugfix: ls::VirtualTextureManager::Unload dereferences a NULL tileset
// (refcount decrement at NULL+0x1C) when asked to unload a tileset that is not
// registered. Retargets the two not-found branches to the function's own
// unlock-and-return path. See docs/bugs/vt-unload-null-tileset.md.
bool vt_unload_guard_init(void *binary_base);

#endif
