/**
 * game_memory.c - Allocate and free through the game's own allocator
 */

#include "game_memory.h"
#include "offset_table.h"
#include "logging.h"

// ls::MemoryManager::Allocate(size_t size, ls::AllocType, int, size_t)
// Argument convention proven live by the AnimationSet insert path
// (plans/animationset-writable-resources.md); alignment > 16 takes the
// posix_memalign branch, everything else is malloc.
typedef void *(*GameAllocateFunc)(size_t size, uint32_t allocType, int a3, size_t a4);

// ls::MemoryManager::Deallocate(void *ptr, ls::AllocType, int)
// Disassembly: null-checks ptr, then tail-calls free().
typedef void (*GameDeallocateFunc)(void *ptr, uint32_t allocType, int a3);

bool game_memory_available(void) {
    return offset_table_game_fn(GAME_FN_MEMORY_ALLOCATE) != NULL
        && offset_table_game_fn(GAME_FN_MEMORY_DEALLOCATE) != NULL;
}

void *game_memory_alloc(size_t size) {
    GameAllocateFunc alloc = (GameAllocateFunc)offset_table_game_fn(GAME_FN_MEMORY_ALLOCATE);
    if (!alloc) {
        LOG_CORE_WARN("ls::MemoryManager::Allocate has no address for this build");
        return NULL;
    }
    return alloc(size, 0, 0, 0);
}

void game_memory_free(void *ptr) {
    if (!ptr) return;
    GameDeallocateFunc dealloc =
        (GameDeallocateFunc)offset_table_game_fn(GAME_FN_MEMORY_DEALLOCATE);
    if (!dealloc) {
        // Leaking is the safe failure: handing a game pointer to the wrong
        // free would corrupt the heap.
        LOG_CORE_WARN("ls::MemoryManager::Deallocate has no address for this build; leaking %p", ptr);
        return;
    }
    dealloc(ptr, 0, 0);
}
