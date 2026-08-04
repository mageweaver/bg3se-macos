/**
 * level_manager.h - Level Manager for BG3SE-macOS
 *
 * Provides access to the game's LevelManager, PhysicsScene, and AiGrid
 * for Ext.Level API (raycasting, tile queries, pathfinding).
 */

#ifndef LEVEL_MANAGER_H
#define LEVEL_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Initialization
// ============================================================================

bool level_manager_init(void *main_binary_base);
bool level_manager_ready(void);

// ============================================================================
// Singleton Access (lazy pointer refresh)
// ============================================================================

void* level_get_manager(void);        // LevelManager*
void* level_get_current(void);        // EoCLevel*
void* level_get_physics_scene(void);  // PhysicsSceneBase*
void* level_get_aigrid(void);         // AiGrid*

// ============================================================================
// Physics Raycast Results
// ============================================================================

typedef struct {
    float normal[3];
    float position[3];
    float distance;
    uint32_t physics_group;
} LevelPhysicsHit;

/**
 * PhysicsHitAll — mirrors phx::PhysicsHitAll from Windows BG3SE.
 * Each field is an Array<T>: ptr (8) + size (4) + capacity (4) = 16 bytes per field.
 * 6 fields × 16 = 96 bytes total. Passed by pointer to RaycastAll VMT function.
 * The callee populates ptr/size (heap-allocated by game engine).
 * Caller must NOT free the inner arrays — they are owned by the game.
 */
typedef struct {
    /* Array<vec3> Normals */
    float   *normals_ptr;
    uint32_t normals_size;
    uint32_t normals_capacity;
    /* Array<vec3> Positions */
    float   *positions_ptr;
    uint32_t positions_size;
    uint32_t positions_capacity;
    /* Array<float> Distances */
    float   *distances_ptr;
    uint32_t distances_size;
    uint32_t distances_capacity;
    /* Array<uint32_t> PhysicsGroup */
    uint32_t *physics_group_ptr;
    uint32_t  physics_group_size;
    uint32_t  physics_group_capacity;
    /* Array<uint32_t> PhysicsExtraFlags */
    uint32_t *extra_flags_ptr;
    uint32_t  extra_flags_size;
    uint32_t  extra_flags_capacity;
    /* Array<void*> Shapes */
    void   **shapes_ptr;
    uint32_t shapes_size;
    uint32_t shapes_capacity;
} LevelPhysicsHitAll;

// ============================================================================
// Physics Functions
// ============================================================================

/**
 * Fail-closed internal shim. The macOS ABI is quarantined at the Lua binding
 * until the trailing ls::Function parameter can be constructed safely.
 */
bool level_raycast_closest(const float src[3], const float dst[3],
                           LevelPhysicsHit *hit,
                           uint32_t physics_type,
                           uint32_t include_group,
                           uint32_t exclude_group,
                           int context);

/**
 * Fail-closed internal shim. The macOS optional read-lock ABI is not proven.
 */
bool level_raycast_all(const float src[3], const float dst[3],
                       LevelPhysicsHitAll *out,
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group,
                       int context);

/**
 * Dispatch RaycastAny through the verified macOS ARM64 VMT slot with a
 * disengaged optional read lock. Fails closed off ARM64 or when the verified
 * game version and Mach-O UUID gates do not both match.
 */
bool level_raycast_any(const float src[3], const float dst[3],
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group,
                       int context);

// ============================================================================
// Sweep Functions
// ============================================================================

/** Sweep sphere along path, return closest hit. */
bool level_sweep_sphere_closest(const float src[3], const float dst[3],
                                 float radius,
                                 LevelPhysicsHit *hit,
                                 uint32_t physics_type,
                                 uint32_t include_group,
                                 uint32_t exclude_group,
                                 int context);

/** Sweep capsule along path, return closest hit. */
bool level_sweep_capsule_closest(const float src[3], const float dst[3],
                                  float radius, float half_height,
                                  LevelPhysicsHit *hit,
                                  uint32_t physics_type,
                                  uint32_t include_group,
                                  uint32_t exclude_group,
                                  int context);

/** Sweep box along path, return closest hit. */
bool level_sweep_box_closest(const float src[3], const float dst[3],
                              const float extents[3],
                              LevelPhysicsHit *hit,
                              uint32_t physics_type,
                              uint32_t include_group,
                              uint32_t exclude_group,
                              int context);

/** Sweep cylinder along path, return closest hit. */
bool level_sweep_cylinder_closest(const float src[3], const float dst[3],
                                   const float extents[3],
                                   LevelPhysicsHit *hit,
                                   uint32_t physics_type,
                                   uint32_t include_group,
                                   uint32_t exclude_group,
                                   int context);

/** Sweep sphere along path, return all hits. */
bool level_sweep_sphere_all(const float src[3], const float dst[3],
                             float radius,
                             LevelPhysicsHitAll *out,
                             uint32_t physics_type,
                             uint32_t include_group,
                             uint32_t exclude_group,
                             int context);

/** Sweep capsule along path, return all hits. */
bool level_sweep_capsule_all(const float src[3], const float dst[3],
                              float radius, float half_height,
                              LevelPhysicsHitAll *out,
                              uint32_t physics_type,
                              uint32_t include_group,
                              uint32_t exclude_group,
                              int context);

/** Sweep box along path, return all hits. */
bool level_sweep_box_all(const float src[3], const float dst[3],
                          const float extents[3],
                          LevelPhysicsHitAll *out,
                          uint32_t physics_type,
                          uint32_t include_group,
                          uint32_t exclude_group,
                          int context);

/** Sweep cylinder along path, return all hits. */
bool level_sweep_cylinder_all(const float src[3], const float dst[3],
                               const float extents[3],
                               LevelPhysicsHitAll *out,
                               uint32_t physics_type,
                               uint32_t include_group,
                               uint32_t exclude_group,
                               int context);

/**
 * Test a box and populate the caller-owned PhysicsHitAll view.
 * @return true when the engine produced overlap results
 */
bool level_test_box(const float pos[3], const float extents[3],
                    LevelPhysicsHitAll *out,
                    uint32_t physics_type,
                    uint32_t include_group,
                    uint32_t exclude_group);

/**
 * Test a sphere and populate the caller-owned PhysicsHitAll view.
 * @return true when the engine produced overlap results
 */
bool level_test_sphere(const float pos[3], float radius,
                       LevelPhysicsHitAll *out,
                       uint32_t physics_type,
                       uint32_t include_group,
                       uint32_t exclude_group);

// ============================================================================
// Tile Queries
// ============================================================================

/**
 * Copy every non-blocker AiGrid MaxHeight layer at world x/z.
 * The caller owns the returned array and releases it with level_free_heights().
 */
bool level_get_heights_at(float x, float z,
                          float **out_heights, size_t *out_count);
void level_free_heights(float *heights);

// ============================================================================
// AiGrid Pathfinding and Tile Snapshots
// ============================================================================

/**
 * Lua-safe copy of the verified AiPath state used by Ext.Level.
 * This struct contains no game-owned pointers.
 */
typedef struct {
    int32_t path_id;
    bool search_started;
    bool search_complete;
    bool goal_found;
    bool destination_reached;
    float moving_bound;
    float standing_bound;
    float moving_bound2;
    int32_t node_count;
} LevelAiPathState;

/** Lua-safe copy of one verified AiPathNode. */
typedef struct {
    float position[3];
    uint64_t portal;
    float distance;
    float distance_modifier;
    uint8_t flags;
} LevelAiPathNode;

/**
 * Result of queueing/finishing a path request.
 * nodes is populated only when state.search_complete && state.goal_found.
 */
typedef struct {
    LevelAiPathState state;
    LevelAiPathNode *nodes;
    size_t nodes_count;
} LevelAiPathResult;

/** Lua-safe raw diagnostic snapshot of one verified AiGridTile. */
typedef struct {
    uint64_t raw_flags;
    uint8_t ground_mask;
    uint8_t cloud_mask;
    float min_height;
    float max_height;
} LevelAiTileRawDebugInfo;

/** Resolve path_id only through AiGrid::PathMap and copy its public state. */
bool level_aigrid_get_path_by_id(int32_t path_id, LevelAiPathState *out_state);

/** Resolve path_id through PathMap, then invoke AiGrid::RemovePath(int). */
bool level_aigrid_release_path(int32_t path_id);

/**
 * Snapshot AiGrid::Paths and return copied path IDs/states.
 * The caller owns *out_states and must release it with
 * level_aigrid_free_path_states().
 */
bool level_aigrid_get_active_pathfinding_requests(
    LevelAiPathState **out_states, size_t *out_count);
void level_aigrid_free_path_states(LevelAiPathState *states);

/**
 * Validate path_id through PathMap, invoke AiGrid::FindPath(int), optionally
 * finish it synchronously, then copy state and any completed goal polyline.
 */
bool level_aigrid_find_path(int32_t path_id, bool immediate,
                            LevelAiPathResult *out_result);
void level_aigrid_free_path_result(LevelAiPathResult *result);

/**
 * Convert world_pos with AiGrid::ToTilePos, look up AiMetaData, and copy the
 * validated LEGACY_Set<EntityHandle>. The caller owns *out_handles.
 */
bool level_aigrid_get_entities_on_tile(
    const float world_pos[3], uint64_t **out_handles, size_t *out_count);
void level_aigrid_free_entity_handles(uint64_t *handles);

/**
 * Copy raw tile diagnostics at world x/z. This is intentionally distinct
 * from Windows-compatible GetTileDebugInfo flag conversion.
 */
bool level_aigrid_get_tile_raw_debug_info(
    float x, float z, LevelAiTileRawDebugInfo *out_info);

#endif // LEVEL_MANAGER_H
