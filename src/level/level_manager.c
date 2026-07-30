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
// Module State
// ============================================================================

static struct {
    bool initialized;
    void *main_binary_base;
    void **level_manager_ptr;  // Points to global slot
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

    log_message("[Level] Level manager initialized");
    log_message("[Level]   Base: %p", main_binary_base);
    log_message("[Level]   LevelManager::m_ptr at offset 0x%llx -> %p",
                (unsigned long long)off->level_mgr_ptr, (void *)g_level.level_manager_ptr);

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
