/**
 * level_manager.c - Level Manager for BG3SE-macOS
 *
 * Provides access to the game's LevelManager, PhysicsScene, and AiGrid
 * for Ext.Level API (raycasting, tile queries, pathfinding).
 *
 * Access chain:
 *   LevelManager::m_ptr -> LevelManager* -> CurrentLevel (+0x90)
 *     -> EoCLevel* -> PhysicsScene (+0x30), AiGrid (+0x80)
 *
 * Note: PhysicsScene and AiGrid offsets are from Windows BG3SE and need
 * runtime verification on ARM64. These are best-effort values.
 */

#include "level_manager.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/offset_table.h"
#include "../core/version_detect.h"
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Offsets
// ============================================================================

// LevelManager::m_ptr — sourced from offset_table at runtime

// LevelManager internal offsets (need ARM64 runtime verification)
#define LEVELMANAGER_CURRENT_LEVEL_OFFSET   0x90   // EoCLevel* CurrentLevel

// EoCLevel offsets (need ARM64 runtime verification)
#define EOCLEVEL_PHYSICS_SCENE_OFFSET       0x30   // PhysicsSceneBase*
#define EOCLEVEL_AIGRID_OFFSET              0x80   // AiGrid*

/*
 * eoc::AiGrid / eoc::AiPath offsets verified for macOS ARM64 build
 * 4.1.1.7209685. See ghidra/offsets/AIGRID_PATHFINDING.md.
 */
#define AIGRID_PATH_MAP_OFFSET              0xf0
#define AIGRID_ACTIVE_PATHS_OFFSET          0x100

#define AIPATH_MOVING_BOUND_OFFSET          0x20
#define AIPATH_STANDING_BOUND_OFFSET        0x24
#define AIPATH_MOVING_BOUND2_OFFSET         0x40
#define AIPATH_STATE_TAIL_OFFSET            0x270
#define AIPATH_NODES_OFFSET                 0x278

#define AIMETADATA_ENTITIES_OFFSET          0x00

#define AIGRID_INVALID_PATH_ID              (-1337)
#define AIGRID_MAX_PATH_MAP_NODES           65536
#define AIGRID_MAX_PATH_MAP_BUCKETS         1048576
#define AIGRID_MAX_ACTIVE_PATHS             65536
#define AIGRID_MAX_PATH_NODES               65536
#define AIGRID_MAX_TILE_ENTITIES            65536

// Verified from the macOS ARM64 phx::PhysXScene vtable address point
// 0x108829200 in game build 4.1.1.7209685. The Itanium ABI contributes
// complete-object and deleting-destructor slots at indices 0 and 1.
#define PHYSICS_VMT_RAYCAST_CLOSEST          8
#define PHYSICS_VMT_RAYCAST_ALL              9
#define PHYSICS_VMT_RAYCAST_ANY             10
#define PHYSICS_VMT_SWEEP_SPHERE_CLOSEST    11
#define PHYSICS_VMT_SWEEP_CAPSULE_CLOSEST   12
#define PHYSICS_VMT_SWEEP_BOX_CLOSEST       13
#define PHYSICS_VMT_SWEEP_CYLINDER_CLOSEST  14
#define PHYSICS_VMT_SWEEP_SPHERE_ALL        15
#define PHYSICS_VMT_SWEEP_CAPSULE_ALL       16
#define PHYSICS_VMT_SWEEP_BOX_ALL           17
#define PHYSICS_VMT_SWEEP_CYLINDER_ALL      18
#define PHYSICS_VMT_SWEEP_SHAPE_ALL         19   /* Verified, not exposed in Lua */
#define PHYSICS_VMT_TEST_BOX                 20
#define PHYSICS_VMT_TEST_SPHERE              24

// ============================================================================
// Verified AiGrid Native Layouts and Function Types
// ============================================================================

typedef struct {
    void *data;
    int32_t capacity;
    int32_t size;
} NativeDynamicArray;

typedef struct {
    int32_t node_count;
    int32_t bucket_count;
    void **buckets;
} NativePathMapHeader;

typedef struct {
    void *next;
    int32_t path_id;
    uint32_t padding_0c;
    void *path;
} NativePathMapNode;

typedef struct {
    uint8_t search_started;
    uint8_t search_complete;
    uint8_t goal_found;
    uint8_t destination_reached;
    uint8_t in_use;
    uint8_t padding_275[3];
    NativeDynamicArray nodes;
} NativeAiPathTail;

typedef struct {
    float position[3];
    uint32_t padding_0c;
    uint64_t portal;
    float distance;
    float distance_modifier;
    uint8_t flags;
    uint8_t padding_21[7];
} NativeAiPathNode;

typedef struct {
    int16_t x;
    int16_t y;
    int32_t subgrid_id;
} NativeAiTilePos;

_Static_assert(sizeof(NativeDynamicArray) == 0x10,
               "DynamicArray header must be 0x10 bytes");
_Static_assert(sizeof(NativePathMapHeader) == 0x10,
               "PathMap header must be 0x10 bytes");
_Static_assert(sizeof(NativePathMapNode) == 0x18,
               "PathMap node must be 0x18 bytes");
_Static_assert(offsetof(NativePathMapNode, path) == 0x10,
               "PathMap node path must be at +0x10");
_Static_assert(sizeof(NativeAiPathTail) == 0x18,
               "AiPath state/nodes tail must be 0x18 bytes");
_Static_assert(offsetof(NativeAiPathTail, nodes) == 0x08,
               "AiPath Nodes header must be at +0x278");
_Static_assert(sizeof(NativeAiPathNode) == 0x28,
               "AiPathNode must be 0x28 bytes");
_Static_assert(offsetof(NativeAiPathNode, portal) == 0x10,
               "AiPathNode Portal must be at +0x10");
_Static_assert(offsetof(NativeAiPathNode, distance) == 0x18,
               "AiPathNode Distance must be at +0x18");
_Static_assert(offsetof(NativeAiPathNode, flags) == 0x20,
               "AiPathNode Flags must be at +0x20");
_Static_assert(sizeof(NativeAiTilePos) == 0x08,
               "AiTilePos must be 0x08 bytes");

typedef bool (*AiGridToTilePosFn)(void *this_, const float world_pos[3],
                                  NativeAiTilePos *out_tile_pos, bool unknown);
typedef void *(*AiGridGetMetaDataFn)(void *this_,
                                     const NativeAiTilePos *tile_pos);
typedef void (*AiGridRemovePathFn)(void *this_, int32_t path_id);
typedef void (*AiGridFindPathFn)(void *this_, int32_t path_id);
typedef void (*AiGridFindPathImmediateFn)(void *this_, int32_t path_id,
                                          bool unused);

// ============================================================================
// Module State
// ============================================================================

static struct {
    bool initialized;
    void *main_binary_base;
    void **level_manager_ptr;  // Points to global slot
    bool aigrid_layout_verified;
    AiGridToTilePosFn aigrid_to_tile_pos;
    AiGridGetMetaDataFn aigrid_get_metadata;
    AiGridRemovePathFn aigrid_remove_path;
    AiGridFindPathFn aigrid_find_path;
    AiGridFindPathImmediateFn aigrid_find_path_immediate;
} g_level = {0};

// ============================================================================
// Initialization
// ============================================================================

bool level_manager_init(void *main_binary_base) {
    if (g_level.initialized) {
        return true;
    }

    if (!main_binary_base) {
        log_message("[Level] ERROR: main_binary_base is NULL");
        return false;
    }

    g_level.main_binary_base = main_binary_base;

    const VersionOffsets *off = offset_table_get();
    if (!off || !off->level_mgr_ptr) {
        log_message("[Level] WARN: level_mgr_ptr not available for this BG3 version — Level API disabled");
        g_level.initialized = true;
        return true;  // Not a fatal error; Level APIs will return NULL gracefully
    }

    g_level.level_manager_ptr = (void **)offset_table_resolve(off->level_mgr_ptr);

    /*
     * Presence of every 7209685 AiGrid anchor is also the per-version layout
     * gate. The older table row intentionally leaves these fields at zero.
     */
    g_level.aigrid_layout_verified =
        version_detect_addresses_safe()
        && off->fn_aigrid_to_tile_pos
        && off->fn_aigrid_get_metadata
        && off->fn_aigrid_remove_path
        && off->fn_aigrid_find_path
        && off->fn_aigrid_find_path_immediate;

    if (g_level.aigrid_layout_verified) {
        g_level.aigrid_to_tile_pos =
            (AiGridToTilePosFn)offset_table_fn(off->fn_aigrid_to_tile_pos);
        g_level.aigrid_get_metadata =
            (AiGridGetMetaDataFn)offset_table_fn(off->fn_aigrid_get_metadata);
        g_level.aigrid_remove_path =
            (AiGridRemovePathFn)offset_table_fn(off->fn_aigrid_remove_path);
        g_level.aigrid_find_path =
            (AiGridFindPathFn)offset_table_fn(off->fn_aigrid_find_path);
        g_level.aigrid_find_path_immediate =
            (AiGridFindPathImmediateFn)offset_table_fn(
                off->fn_aigrid_find_path_immediate);

        g_level.aigrid_layout_verified =
            g_level.aigrid_to_tile_pos
            && g_level.aigrid_get_metadata
            && g_level.aigrid_remove_path
            && g_level.aigrid_find_path
            && g_level.aigrid_find_path_immediate;
    }

    log_message("[Level] Level manager initialized");
    log_message("[Level]   Base: %p", main_binary_base);
    log_message("[Level]   LevelManager::m_ptr at offset 0x%llx -> %p",
                (unsigned long long)off->level_mgr_ptr, (void *)g_level.level_manager_ptr);
    log_message("[Level]   AiGrid path/tile bindings: %s",
                g_level.aigrid_layout_verified ? "enabled" : "disabled");

    g_level.initialized = true;
    return true;
}

bool level_manager_ready(void) {
    if (!g_level.initialized || !g_level.level_manager_ptr) {
        return false;
    }

    void *mgr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_level.level_manager_ptr, &mgr)) {
        return false;
    }

    return mgr != NULL;
}

// ============================================================================
// Singleton Access
// ============================================================================

void *level_get_manager(void) {
    if (!g_level.initialized || !g_level.level_manager_ptr) {
        return NULL;
    }

    void *mgr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_level.level_manager_ptr, &mgr)) {
        return NULL;
    }

    return mgr;
}

void *level_get_current(void) {
    void *mgr = level_get_manager();
    if (!mgr) return NULL;

    void *current = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)mgr + LEVELMANAGER_CURRENT_LEVEL_OFFSET, &current)) {
        return NULL;
    }

    return current;
}

void *level_get_physics_scene(void) {
    void *level = level_get_current();
    if (!level) return NULL;

    void *physics = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)level + EOCLEVEL_PHYSICS_SCENE_OFFSET, &physics)) {
        return NULL;
    }

    return physics;
}

void *level_get_aigrid(void) {
    void *level = level_get_current();
    if (!level) return NULL;

    void *aigrid = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)level + EOCLEVEL_AIGRID_OFFSET, &aigrid)) {
        return NULL;
    }

    return aigrid;
}

// ============================================================================
// AiGrid Safe Memory Helpers
// ============================================================================

static bool checked_address_add(const void *base, size_t offset,
                                mach_vm_address_t *out_address) {
    if (!base || !out_address) return false;

    uintptr_t address = (uintptr_t)base;
    if (address > UINTPTR_MAX - offset) return false;

    *out_address = (mach_vm_address_t)(address + offset);
    return true;
}

static bool readable_range(mach_vm_address_t address, size_t size) {
    if (!address || size == 0) return false;
    if (address > UINTPTR_MAX - (size - 1)) return false;

    mach_vm_address_t end = address + size - 1;
    if (safe_memory_is_gpu_region(address)
        || safe_memory_is_gpu_region(end)) {
        return false;
    }

    SafeMemoryInfo info = safe_memory_check_address(address);
    if (!info.is_valid || !info.is_readable
        || address < info.region_start) {
        return false;
    }

    mach_vm_size_t region_offset = address - info.region_start;
    return region_offset <= info.region_size
        && (mach_vm_size_t)size <= info.region_size - region_offset;
}

static bool safe_read_at(const void *base, size_t offset,
                         void *out, size_t size) {
    mach_vm_address_t address = 0;
    if (!out || !checked_address_add(base, offset, &address)
        || !readable_range(address, size)) {
        return false;
    }

    return safe_memory_read(address, out, size);
}

static bool checked_array_bytes(int32_t count, size_t element_size,
                                size_t *out_bytes) {
    if (!out_bytes || count < 0 || element_size == 0) return false;
    if ((size_t)count > SIZE_MAX / element_size) return false;
    *out_bytes = (size_t)count * element_size;
    return true;
}

static bool validate_dynamic_array(const NativeDynamicArray *array,
                                   int32_t max_count,
                                   size_t element_size) {
    if (!array || max_count < 0
        || array->capacity < 0 || array->size < 0
        || array->size > array->capacity
        || array->capacity > max_count) {
        return false;
    }

    if (array->size == 0) return true;
    if (!array->data) return false;

    size_t bytes = 0;
    return checked_array_bytes(array->size, element_size, &bytes)
        && readable_range((mach_vm_address_t)array->data, bytes);
}

/*
 * Copy a DynamicArray/LEGACY_Set payload only when its header is unchanged
 * before and after the copy. A second attempt handles a benign concurrent
 * append/removal; repeated mutation fails closed.
 */
static bool snapshot_dynamic_array(const void *owner, size_t header_offset,
                                   int32_t max_count, size_t element_size,
                                   void **out_data, size_t *out_count) {
    if (!out_data || !out_count) return false;
    *out_data = NULL;
    *out_count = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        NativeDynamicArray before = {0};
        NativeDynamicArray after = {0};
        if (!safe_read_at(owner, header_offset, &before, sizeof(before))
            || !validate_dynamic_array(&before, max_count, element_size)) {
            return false;
        }

        size_t bytes = 0;
        if (!checked_array_bytes(before.size, element_size, &bytes)) {
            return false;
        }

        void *copy = NULL;
        if (bytes > 0) {
            copy = malloc(bytes);
            if (!copy) return false;
            if (!safe_memory_read((mach_vm_address_t)before.data, copy, bytes)) {
                free(copy);
                return false;
            }
        }

        if (!safe_read_at(owner, header_offset, &after, sizeof(after))
            || !validate_dynamic_array(&after, max_count, element_size)) {
            free(copy);
            return false;
        }

        if (memcmp(&before, &after, sizeof(before)) == 0) {
            *out_data = copy;
            *out_count = (size_t)before.size;
            return true;
        }

        free(copy);
    }

    return false;
}

static bool aigrid_bindings_available(void) {
    return g_level.initialized
        && g_level.aigrid_layout_verified
        && version_detect_addresses_safe();
}

static bool read_path_map_header(void *aigrid,
                                 NativePathMapHeader *out_header) {
    if (!aigrid || !out_header
        || !safe_read_at(aigrid, AIGRID_PATH_MAP_OFFSET,
                         out_header, sizeof(*out_header))) {
        return false;
    }

    if (out_header->node_count < 0
        || out_header->node_count > AIGRID_MAX_PATH_MAP_NODES
        || out_header->bucket_count <= 0
        || out_header->bucket_count > AIGRID_MAX_PATH_MAP_BUCKETS
        || !out_header->buckets) {
        return false;
    }

    size_t bucket_bytes = 0;
    return checked_array_bytes(out_header->bucket_count, sizeof(void *),
                               &bucket_bytes)
        && readable_range((mach_vm_address_t)out_header->buckets,
                          bucket_bytes);
}

/*
 * Authoritative path-ID lookup. The integer hash exactly mirrors the verified
 * ARM64 sxtw + udiv/msub sequence in AiGrid::{FindPath,RemovePath}.
 */
static bool path_map_lookup(void *aigrid, int32_t path_id, void **out_path) {
    if (out_path) *out_path = NULL;
    if (!out_path || path_id == AIGRID_INVALID_PATH_ID) return false;

    NativePathMapHeader header = {0};
    if (!read_path_map_header(aigrid, &header)
        || header.node_count == 0) {
        return false;
    }

    uint64_t hash = (uint64_t)(int64_t)path_id;
    size_t bucket = (size_t)(hash % (uint32_t)header.bucket_count);

    void *node = NULL;
    if (!safe_read_at(header.buckets, bucket * sizeof(void *),
                      &node, sizeof(node))) {
        return false;
    }

    for (int32_t visited = 0;
         node && visited < header.node_count;
         visited++) {
        NativePathMapNode entry = {0};
        if (!safe_read_at(node, 0, &entry, sizeof(entry))) {
            return false;
        }

        if (entry.path_id == path_id) {
            if (!entry.path
                || !readable_range((mach_vm_address_t)entry.path, 0x2a8)) {
                return false;
            }

            NativePathMapHeader after = {0};
            if (!read_path_map_header(aigrid, &after)
                || memcmp(&header, &after, sizeof(header)) != 0) {
                return false;
            }

            *out_path = entry.path;
            return true;
        }

        if (entry.next == node) return false;
        node = entry.next;
    }

    return false;
}

/*
 * Active AiGrid::Paths entries contain pointers, not IDs. Resolve each pointer
 * back through PathMap so an active-list index is never mistaken for a PathId.
 */
static bool path_map_find_id_by_pointer(void *aigrid, void *target_path,
                                        int32_t *out_path_id) {
    if (!target_path || !out_path_id
        || !readable_range((mach_vm_address_t)target_path, 0x2a8)) {
        return false;
    }

    NativePathMapHeader header = {0};
    if (!read_path_map_header(aigrid, &header)
        || header.node_count == 0) {
        return false;
    }

    int32_t visited = 0;
    for (int32_t bucket = 0;
         bucket < header.bucket_count && visited < header.node_count;
         bucket++) {
        void *node = NULL;
        if (!safe_read_at(header.buckets, (size_t)bucket * sizeof(void *),
                          &node, sizeof(node))) {
            return false;
        }

        while (node && visited < header.node_count) {
            NativePathMapNode entry = {0};
            if (!safe_read_at(node, 0, &entry, sizeof(entry))) {
                return false;
            }
            visited++;

            if (entry.path == target_path
                && entry.path_id != AIGRID_INVALID_PATH_ID) {
                NativePathMapHeader after = {0};
                if (!read_path_map_header(aigrid, &after)
                    || memcmp(&header, &after, sizeof(header)) != 0) {
                    return false;
                }

                *out_path_id = entry.path_id;
                return true;
            }

            if (entry.next == node) return false;
            node = entry.next;
        }
    }

    return false;
}

static bool copy_path_state(void *path, int32_t path_id,
                            LevelAiPathState *out_state,
                            NativeDynamicArray *out_nodes_header) {
    if (!path || !out_state) return false;

    NativeAiPathTail before = {0};
    NativeAiPathTail after = {0};
    float moving_bound = 0.0f;
    float standing_bound = 0.0f;
    float moving_bound2 = 0.0f;

    if (!safe_read_at(path, AIPATH_STATE_TAIL_OFFSET,
                      &before, sizeof(before))
        || !validate_dynamic_array(&before.nodes, AIGRID_MAX_PATH_NODES,
                                   sizeof(NativeAiPathNode))
        || !safe_read_at(path, AIPATH_MOVING_BOUND_OFFSET,
                         &moving_bound, sizeof(moving_bound))
        || !safe_read_at(path, AIPATH_STANDING_BOUND_OFFSET,
                         &standing_bound, sizeof(standing_bound))
        || !safe_read_at(path, AIPATH_MOVING_BOUND2_OFFSET,
                         &moving_bound2, sizeof(moving_bound2))
        || !safe_read_at(path, AIPATH_STATE_TAIL_OFFSET,
                         &after, sizeof(after))
        || memcmp(&before, &after, sizeof(before)) != 0) {
        return false;
    }

    if (before.search_started > 1
        || before.search_complete > 1
        || before.goal_found > 1
        || before.destination_reached > 1
        || before.in_use != 1) {
        return false;
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->path_id = path_id;
    out_state->search_started = before.search_started != 0;
    out_state->search_complete = before.search_complete != 0;
    out_state->goal_found = before.goal_found != 0;
    out_state->destination_reached = before.destination_reached != 0;
    out_state->moving_bound = moving_bound;
    out_state->standing_bound = standing_bound;
    out_state->moving_bound2 = moving_bound2;
    out_state->node_count = before.nodes.size;

    if (out_nodes_header) *out_nodes_header = before.nodes;
    return true;
}

static bool copy_path_result(void *path, int32_t path_id,
                             LevelAiPathResult *out_result) {
    if (!out_result) return false;
    memset(out_result, 0, sizeof(*out_result));

    NativeDynamicArray initial_nodes = {0};
    LevelAiPathState initial_state = {0};
    if (!copy_path_state(path, path_id, &initial_state, &initial_nodes)) {
        return false;
    }

    out_result->state = initial_state;
    if (!initial_state.search_complete || !initial_state.goal_found) {
        return true;
    }

    NativeAiPathNode *native_nodes = NULL;
    size_t native_count = 0;
    if (!snapshot_dynamic_array(path, AIPATH_NODES_OFFSET,
                                AIGRID_MAX_PATH_NODES,
                                sizeof(NativeAiPathNode),
                                (void **)&native_nodes, &native_count)
        || native_count != (size_t)initial_nodes.size) {
        free(native_nodes);
        return false;
    }

    LevelAiPathNode *nodes = NULL;
    if (native_count > 0) {
        if (native_count > SIZE_MAX / sizeof(*nodes)) {
            free(native_nodes);
            return false;
        }

        nodes = calloc(native_count, sizeof(*nodes));
        if (!nodes) {
            free(native_nodes);
            return false;
        }

        for (size_t i = 0; i < native_count; i++) {
            memcpy(nodes[i].position, native_nodes[i].position,
                   sizeof(nodes[i].position));
            nodes[i].portal = native_nodes[i].portal;
            nodes[i].distance = native_nodes[i].distance;
            nodes[i].distance_modifier =
                native_nodes[i].distance_modifier;
            nodes[i].flags = native_nodes[i].flags;
        }
    }
    free(native_nodes);

    /*
     * Do not publish nodes if completion state or the node count changed while
     * copying. This guarantees the Lua table represents one stable result.
     */
    LevelAiPathState final_state = {0};
    if (!copy_path_state(path, path_id, &final_state, NULL)
        || !final_state.search_complete
        || !final_state.goal_found
        || final_state.node_count != (int32_t)native_count) {
        free(nodes);
        memset(out_result, 0, sizeof(*out_result));
        return false;
    }

    out_result->state = final_state;
    out_result->nodes = nodes;
    out_result->nodes_count = native_count;
    return true;
}

// ============================================================================
// AiGrid Public Snapshot API
// ============================================================================

bool level_aigrid_get_path_by_id(int32_t path_id,
                                 LevelAiPathState *out_state) {
    if (out_state) memset(out_state, 0, sizeof(*out_state));
    if (!out_state || path_id == AIGRID_INVALID_PATH_ID
        || !aigrid_bindings_available()) {
        return false;
    }

    void *aigrid = level_get_aigrid();
    void *path = NULL;
    return aigrid
        && path_map_lookup(aigrid, path_id, &path)
        && copy_path_state(path, path_id, out_state, NULL);
}

bool level_aigrid_release_path(int32_t path_id) {
    /*
     * The sentinel check and authoritative lookup must happen before the native
     * RemovePath call; the native routine assumes a valid map entry.
     */
    if (path_id == AIGRID_INVALID_PATH_ID
        || !aigrid_bindings_available()
        || !g_level.aigrid_remove_path) {
        return false;
    }

    void *aigrid = level_get_aigrid();
    void *validated_path = NULL;
    LevelAiPathState validated_state = {0};
    if (!aigrid
        || !path_map_lookup(aigrid, path_id, &validated_path)
        || !copy_path_state(validated_path, path_id,
                            &validated_state, NULL)) {
        return false;
    }

    (void)validated_state;
    g_level.aigrid_remove_path(aigrid, path_id);
    return true;
}

bool level_aigrid_get_active_pathfinding_requests(
    LevelAiPathState **out_states, size_t *out_count) {
    if (!out_states || !out_count) return false;
    *out_states = NULL;
    *out_count = 0;

    if (!aigrid_bindings_available()) return false;

    void *aigrid = level_get_aigrid();
    if (!aigrid) return false;

    void **active_paths = NULL;
    size_t active_count = 0;
    if (!snapshot_dynamic_array(aigrid, AIGRID_ACTIVE_PATHS_OFFSET,
                                AIGRID_MAX_ACTIVE_PATHS, sizeof(void *),
                                (void **)&active_paths, &active_count)) {
        return false;
    }

    if (active_count == 0) {
        free(active_paths);
        return true;
    }

    if (active_count > SIZE_MAX / sizeof(LevelAiPathState)) {
        free(active_paths);
        return false;
    }

    LevelAiPathState *states =
        calloc(active_count, sizeof(LevelAiPathState));
    if (!states) {
        free(active_paths);
        return false;
    }

    size_t copied = 0;
    for (size_t i = 0; i < active_count; i++) {
        void *active_path = active_paths[i];
        int32_t path_id = 0;
        void *resolved_path = NULL;

        if (!path_map_find_id_by_pointer(aigrid, active_path, &path_id)
            || !path_map_lookup(aigrid, path_id, &resolved_path)
            || resolved_path != active_path
            || !copy_path_state(resolved_path, path_id,
                                &states[copied], NULL)) {
            continue;
        }
        copied++;
    }
    free(active_paths);

    if (copied == 0) {
        free(states);
        return true;
    }

    *out_states = states;
    *out_count = copied;
    return true;
}

void level_aigrid_free_path_states(LevelAiPathState *states) {
    free(states);
}

bool level_aigrid_find_path(int32_t path_id, bool immediate,
                            LevelAiPathResult *out_result) {
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    if (!out_result || path_id == AIGRID_INVALID_PATH_ID
        || !aigrid_bindings_available()
        || !g_level.aigrid_find_path
        || (immediate && !g_level.aigrid_find_path_immediate)) {
        return false;
    }

    void *aigrid = level_get_aigrid();
    void *validated_path = NULL;
    LevelAiPathState validated_state = {0};
    if (!aigrid
        || !path_map_lookup(aigrid, path_id, &validated_path)
        || !copy_path_state(validated_path, path_id,
                            &validated_state, NULL)) {
        return false;
    }

    /*
     * Do not retain validated_path across either call: FindPath mutates Paths
     * and node containers, and FindPathImmediate drives global grid updates.
     */
    validated_path = NULL;
    g_level.aigrid_find_path(aigrid, path_id);

    if (immediate) {
        if (!path_map_lookup(aigrid, path_id, &validated_path)
            || !copy_path_state(validated_path, path_id,
                                &validated_state, NULL)) {
            return false;
        }
        validated_path = NULL;
        g_level.aigrid_find_path_immediate(aigrid, path_id, false);
    }

    void *result_path = NULL;
    return path_map_lookup(aigrid, path_id, &result_path)
        && copy_path_result(result_path, path_id, out_result);
}

void level_aigrid_free_path_result(LevelAiPathResult *result) {
    if (!result) return;
    free(result->nodes);
    memset(result, 0, sizeof(*result));
}

bool level_aigrid_get_entities_on_tile(
    const float world_pos[3], uint64_t **out_handles, size_t *out_count) {
    if (!out_handles || !out_count) return false;
    *out_handles = NULL;
    *out_count = 0;

    if (!world_pos || !aigrid_bindings_available()
        || !g_level.aigrid_to_tile_pos
        || !g_level.aigrid_get_metadata) {
        return false;
    }

    void *aigrid = level_get_aigrid();
    if (!aigrid) return false;

    NativeAiTilePos tile_pos = {0};
    if (!g_level.aigrid_to_tile_pos(
            aigrid, world_pos, &tile_pos, false)) {
        return true;
    }

    if (tile_pos.x < 0 || tile_pos.y < 0 || tile_pos.subgrid_id == -1) {
        return true;
    }

    void *metadata = g_level.aigrid_get_metadata(aigrid, &tile_pos);
    if (!metadata) return true;

    uint64_t *handles = NULL;
    size_t handle_count = 0;
    if (!snapshot_dynamic_array(metadata, AIMETADATA_ENTITIES_OFFSET,
                                AIGRID_MAX_TILE_ENTITIES, sizeof(uint64_t),
                                (void **)&handles, &handle_count)) {
        return false;
    }

    *out_handles = handles;
    *out_count = handle_count;
    return true;
}

void level_aigrid_free_entity_handles(uint64_t *handles) {
    free(handles);
}

// ============================================================================
// VMT Call Helper
// ============================================================================

/**
 * Read a function pointer from a VMT at a given index.
 */
static void *read_vmt_entry(void *object, int index) {
    if (!object) return NULL;

    // Read VMT pointer at +0x00
    void *vmt = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)object, &vmt)) {
        return NULL;
    }

    // Read function pointer at VMT[index]
    void *func = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)vmt + (index * sizeof(void *)), &func)) {
        return NULL;
    }

    return func;
}

// ============================================================================
// Physics Functions
// ============================================================================

/*
 * RaycastClosest (macOS ARM64 VMT[8]), RaycastAll (macOS ARM64 VMT[9],
 * Windows declaration slot 8), and RaycastAny (macOS ARM64 VMT[10]) are
 * quarantined below. Their named symbols prove Vector3f const& parameters,
 * but the audited signatures include trailing ls::Function/optional lock
 * values whose C representation and ownership are not proven.
 */

/*
 * These overlap-query signatures are complete in the audited macOS symbols.
 * Vector3f const& and PhysicsHitAll& are pointers under AAPCS64.
 */
typedef bool (*PhysicsTestBoxFn)(void *this_,
                                  const float position[3],
                                  const float extents[3],
                                  LevelPhysicsHitAll *hits,
                                  uint32_t physType,
                                  uint32_t includeGroup,
                                  uint32_t excludeGroup);

typedef bool (*PhysicsTestSphereFn)(void *this_,
                                     const float position[3],
                                     float radius,
                                     LevelPhysicsHitAll *hits,
                                     uint32_t physType,
                                     uint32_t includeGroup,
                                     uint32_t excludeGroup);

// ============================================================================
// Sweep Function Types
// ============================================================================

/*
 * The complete macOS ARM64 named-symbol signatures prove the vector-reference
 * shape, argument order, hit output, flags, context, and two object indices.
 * Vector3f const& parameters are pointers; scalar radius values remain floats.
 */
typedef bool (*PhysicsSweepSphereClosestFn)(void *this_,
                                             float radius,
                                             const float src[3],
                                             const float dst[3],
                                             LevelPhysicsHit *hit,
                                             uint32_t physType,
                                             uint32_t includeGroup,
                                             uint32_t excludeGroup,
                                             uint32_t context,
                                             uint32_t physObjIdx,
                                             uint32_t excludePhysObjIdx);

typedef bool (*PhysicsSweepCapsuleClosestFn)(void *this_,
                                              float radius,
                                              float halfHeight,
                                              const float src[3],
                                              const float dst[3],
                                              LevelPhysicsHit *hit,
                                              uint32_t physType,
                                              uint32_t includeGroup,
                                              uint32_t excludeGroup,
                                              uint32_t context,
                                              uint32_t physObjIdx,
                                              uint32_t excludePhysObjIdx);

typedef bool (*PhysicsSweepBoxClosestFn)(void *this_,
                                          const float extents[3],
                                          const float src[3],
                                          const float dst[3],
                                          LevelPhysicsHit *hit,
                                          uint32_t physType,
                                          uint32_t includeGroup,
                                          uint32_t excludeGroup,
                                          uint32_t context,
                                          uint32_t physObjIdx,
                                          uint32_t excludePhysObjIdx);

/**
 * SweepCylinderClosest — macOS ARM64 VMT[14]
 * macOS ARM64 symbol:
 *   phx::PhysXScene::SweepCylinderClosest(Vector3f const&, Vector3f const&,
 *     Vector3f const&, ls::PhysicsHit&, ...)
 * The first vector is the cylinder extents. Vector3f const& parameters are
 * pointers in the ARM64 ABI; the bool return is direct and needs no x8 buffer.
 */
typedef bool (*PhysicsSweepCylinderClosestFn)(void *this_,
                                               const float extents[3],
                                               const float src[3],
                                               const float dst[3],
                                               LevelPhysicsHit *hit,
                                               uint32_t physType,
                                               uint32_t includeGroup,
                                               uint32_t excludeGroup,
                                               int context,
                                               int physObjIdx,
                                               int excludePhysObjIdx);

/* SweepSphereAll is macOS ARM64 VMT[15]. */
typedef bool (*PhysicsSweepSphereAllFn)(void *this_,
                                         float radius,
                                         const float src[3],
                                         const float dst[3],
                                         LevelPhysicsHitAll *hits,
                                         uint32_t physType,
                                         uint32_t includeGroup,
                                         uint32_t excludeGroup,
                                         uint32_t context,
                                         uint32_t physObjIdx,
                                         uint32_t excludePhysObjIdx);

typedef bool (*PhysicsSweepCapsuleAllFn)(void *this_,
                                          float radius,
                                          float halfHeight,
                                          const float src[3],
                                          const float dst[3],
                                          LevelPhysicsHitAll *hits,
                                          uint32_t physType,
                                          uint32_t includeGroup,
                                          uint32_t excludeGroup,
                                          uint32_t context,
                                          uint32_t physObjIdx,
                                          uint32_t excludePhysObjIdx);

typedef bool (*PhysicsSweepBoxAllFn)(void *this_,
                                      const float extents[3],
                                      const float src[3],
                                      const float dst[3],
                                      LevelPhysicsHitAll *hits,
                                      uint32_t physType,
                                      uint32_t includeGroup,
                                      uint32_t excludeGroup,
                                      uint32_t context,
                                      uint32_t physObjIdx,
                                      uint32_t excludePhysObjIdx);

/**
 * SweepCylinderAll — macOS ARM64 VMT[18], same extents/source/destination
 * vector order as SweepCylinderClosest with PhysicsHitAll passed by reference.
 */
typedef bool (*PhysicsSweepCylinderAllFn)(void *this_,
                                           const float extents[3],
                                           const float src[3],
                                           const float dst[3],
                                           LevelPhysicsHitAll *hits,
                                           uint32_t physType,
                                           uint32_t includeGroup,
                                           uint32_t excludeGroup,
                                           int context,
                                           int physObjIdx,
                                           int excludePhysObjIdx);

// ============================================================================
// Sweep Implementations — Closest (single hit)
// ============================================================================

bool level_sweep_sphere_closest(const float src[3], const float dst[3],
                                 float radius,
                                 LevelPhysicsHit *hit,
                                 uint32_t physics_type,
                                 uint32_t include_group,
                                 uint32_t exclude_group,
                                 int context) {
    if (!hit) return false;
    memset(hit, 0, sizeof(*hit));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_SPHERE_CLOSEST);
    if (!func) {
        log_message("[Level] SweepSphereClosest VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_SPHERE_CLOSEST);
        return false;
    }

    PhysicsSweepSphereClosestFn sweep = (PhysicsSweepSphereClosestFn)func;
    return sweep(physics, radius, src, dst, hit,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_capsule_closest(const float src[3], const float dst[3],
                                  float radius, float half_height,
                                  LevelPhysicsHit *hit,
                                  uint32_t physics_type,
                                  uint32_t include_group,
                                  uint32_t exclude_group,
                                  int context) {
    if (!hit) return false;
    memset(hit, 0, sizeof(*hit));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_CAPSULE_CLOSEST);
    if (!func) {
        log_message("[Level] SweepCapsuleClosest VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_CAPSULE_CLOSEST);
        return false;
    }

    PhysicsSweepCapsuleClosestFn sweep =
        (PhysicsSweepCapsuleClosestFn)func;
    return sweep(physics, radius, half_height, src, dst, hit,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_box_closest(const float src[3], const float dst[3],
                              const float extents[3],
                              LevelPhysicsHit *hit,
                              uint32_t physics_type,
                              uint32_t include_group,
                              uint32_t exclude_group,
                              int context) {
    if (!hit) return false;
    memset(hit, 0, sizeof(*hit));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_BOX_CLOSEST);
    if (!func) {
        log_message("[Level] SweepBoxClosest VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_BOX_CLOSEST);
        return false;
    }

    PhysicsSweepBoxClosestFn sweep = (PhysicsSweepBoxClosestFn)func;
    return sweep(physics, extents, src, dst, hit,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_cylinder_closest(const float src[3], const float dst[3],
                                   const float extents[3],
                                   LevelPhysicsHit *hit,
                                   uint32_t physics_type,
                                   uint32_t include_group,
                                   uint32_t exclude_group,
                                   int context) {
    if (!hit) return false;
    memset(hit, 0, sizeof(*hit));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_CYLINDER_CLOSEST);
    if (!func) {
        log_message("[Level] SweepCylinderClosest VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_CYLINDER_CLOSEST);
        return false;
    }

    PhysicsSweepCylinderClosestFn sweep = (PhysicsSweepCylinderClosestFn)func;
    return sweep(physics,
                 extents, src, dst,
                 hit,
                 physics_type, include_group, exclude_group,
                 context, -1, -1);
}

// ============================================================================
// Sweep Implementations — All (multiple hits)
// ============================================================================

bool level_sweep_sphere_all(const float src[3], const float dst[3],
                             float radius,
                             LevelPhysicsHitAll *out,
                             uint32_t physics_type,
                             uint32_t include_group,
                             uint32_t exclude_group,
                             int context) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_SPHERE_ALL);
    if (!func) {
        log_message("[Level] SweepSphereAll VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_SPHERE_ALL);
        return false;
    }

    PhysicsSweepSphereAllFn sweep = (PhysicsSweepSphereAllFn)func;
    return sweep(physics, radius, src, dst, out,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_capsule_all(const float src[3], const float dst[3],
                              float radius, float half_height,
                              LevelPhysicsHitAll *out,
                              uint32_t physics_type,
                              uint32_t include_group,
                              uint32_t exclude_group,
                              int context) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_CAPSULE_ALL);
    if (!func) {
        log_message("[Level] SweepCapsuleAll VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_CAPSULE_ALL);
        return false;
    }

    PhysicsSweepCapsuleAllFn sweep = (PhysicsSweepCapsuleAllFn)func;
    return sweep(physics, radius, half_height, src, dst, out,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_box_all(const float src[3], const float dst[3],
                          const float extents[3],
                          LevelPhysicsHitAll *out,
                          uint32_t physics_type,
                          uint32_t include_group,
                          uint32_t exclude_group,
                          int context) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_BOX_ALL);
    if (!func) {
        log_message("[Level] SweepBoxAll VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_BOX_ALL);
        return false;
    }

    PhysicsSweepBoxAllFn sweep = (PhysicsSweepBoxAllFn)func;
    return sweep(physics, extents, src, dst, out,
                 physics_type, include_group, exclude_group,
                 (uint32_t)context, UINT32_MAX, UINT32_MAX);
}

bool level_sweep_cylinder_all(const float src[3], const float dst[3],
                               const float extents[3],
                               LevelPhysicsHitAll *out,
                               uint32_t physics_type,
                               uint32_t include_group,
                               uint32_t exclude_group,
                               int context) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_SWEEP_CYLINDER_ALL);
    if (!func) {
        log_message("[Level] SweepCylinderAll VMT entry not found at index %d",
                    PHYSICS_VMT_SWEEP_CYLINDER_ALL);
        return false;
    }

    PhysicsSweepCylinderAllFn sweep = (PhysicsSweepCylinderAllFn)func;
    return sweep(physics,
                 extents, src, dst,
                 out,
                 physics_type, include_group, exclude_group,
                 context, -1, -1);
}

bool level_raycast_all(const float src[3], const float dst[3],
                       LevelPhysicsHitAll *out,
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group,
                       int context) {
    if (out) memset(out, 0, sizeof(*out));
    (void)src;
    (void)dst;
    (void)physics_type;
    (void)include_group;
    (void)exclude_group;
    (void)context;
    return false;
}

bool level_raycast_closest(const float src[3], const float dst[3],
                           LevelPhysicsHit *hit,
                           uint32_t physics_type,
                           uint32_t include_group,
                           uint32_t exclude_group,
                           int context) {
    if (hit) memset(hit, 0, sizeof(*hit));
    (void)src;
    (void)dst;
    (void)physics_type;
    (void)include_group;
    (void)exclude_group;
    (void)context;
    return false;
}

bool level_raycast_any(const float src[3], const float dst[3],
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group,
                       int context) {
    (void)src;
    (void)dst;
    (void)physics_type;
    (void)include_group;
    (void)exclude_group;
    (void)context;
    return false;
}

bool level_test_box(const float pos[3], const float extents[3],
                    LevelPhysicsHitAll *out,
                    uint32_t physics_type,
                    uint32_t include_group,
                    uint32_t exclude_group) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_TEST_BOX);
    if (!func) return false;

    PhysicsTestBoxFn test = (PhysicsTestBoxFn)func;
    return test(physics, pos, extents, out,
                physics_type, include_group, exclude_group);
}

bool level_test_sphere(const float pos[3], float radius,
                       LevelPhysicsHitAll *out,
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    void *physics = level_get_physics_scene();
    if (!physics) return false;

    void *func = read_vmt_entry(physics, PHYSICS_VMT_TEST_SPHERE);
    if (!func) return false;

    PhysicsTestSphereFn test = (PhysicsTestSphereFn)func;
    return test(physics, pos, radius, out,
                physics_type, include_group, exclude_group);
}

// ============================================================================
// Tile Queries
// ============================================================================

int level_get_heights_at(float x, float z, float *out_heights, int max_heights) {
    if (!out_heights || max_heights <= 0) return 0;

    void *aigrid = level_get_aigrid();
    if (!aigrid) {
        log_message("[Level] AiGrid not available");
        return 0;
    }

    // AiGrid tile height lookup requires internal structure knowledge.
    // This is a stub until AiGrid offsets are verified at runtime.
    // The AiGrid typically stores tile data in a grid indexed by (x,z) coords.
    (void)x;
    (void)z;
    log_message("[Level] GetHeightsAt: AiGrid offsets not yet verified (stub)");
    return 0;
}
