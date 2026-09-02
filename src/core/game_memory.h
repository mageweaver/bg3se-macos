/**
 * game_memory.h - Allocate and free through the game's own allocator
 *
 * Anything linked into a game-owned container (LegacyRefMap nodes, Array<T>
 * buffers, STDString heap blocks) is eventually freed by the game, so it has
 * to come from ls::MemoryManager. On this build Allocate/Deallocate wrap libc
 * malloc/posix_memalign/free (read from the disassembly), but the game's entry
 * points remain the contract; the offset table carries both.
 *
 * Mirrors upstream CoreLib/Base/BaseMemory.h GameAllocRaw/GameFree.
 */

#ifndef BG3SE_GAME_MEMORY_H
#define BG3SE_GAME_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when both Allocate and Deallocate have addresses for this build. */
bool game_memory_available(void);

/** ls::MemoryManager::Allocate(size, 0, 0, 0). NULL if unavailable or OOM. */
void *game_memory_alloc(size_t size);

/** ls::MemoryManager::Deallocate(ptr, 0, 0). NULL is a no-op. */
void game_memory_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_GAME_MEMORY_H
