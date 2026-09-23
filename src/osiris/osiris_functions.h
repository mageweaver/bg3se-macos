/**
 * BG3SE-macOS - Osiris Function Cache
 *
 * Caches Osiris function metadata (name, ID, arity, type) for fast lookup.
 * Supports both enumeration at init time and dynamic caching from events.
 */

#ifndef BG3SE_OSIRIS_FUNCTIONS_H
#define BG3SE_OSIRIS_FUNCTIONS_H

#include <stdint.h>
#include "osiris_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration
// ============================================================================

/*
 * Osiris function cache capacity.
 *
 * Was 4096, which silently truncated: a vanilla install plus one mod enumerates
 * 7869 Osiris functions, so roughly half were dropped and osi_func_cache
 * returned without a word. Lookups then reported "not yet discovered", which
 * reads like a discovery failure rather than a full cache. It broke mod procs
 * and stock functions alike (DialogGetSpeaker among them).
 *
 * 32768 leaves ~4x headroom over an observed 7869 and stays within the int32_t
 * hash-table index type. At ~140 bytes per entry this costs about 4.6 MB, which
 * is a reasonable trade against silently losing half of Osiris.
 */
#define MAX_CACHED_FUNCTIONS 32768
#define FUNC_HASH_SIZE 65536
/* Sized well above MAX_CACHED_FUNCTIONS so the load factor stays low; at 8192
 * with 7869 entries almost every lookup degraded to the linear fallback. */
#define FUNC_NAME_HASH_SIZE 65536
#define MAX_SEEN_FUNC_IDS 256

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the function cache system.
 * Must be called before using any other function cache operations.
 */
void osi_func_cache_init(void);

/**
 * Set the runtime pointers needed for function enumeration.
 * These come from dlsym on libOsiris.dylib.
 *
 * @param pFunctionData Pointer to pFunctionData function
 * @param ppOsiFunctionMan Pointer to global OsiFunctionMan pointer
 */
void osi_func_cache_set_runtime(pFunctionDataFn pFunctionData, void **ppOsiFunctionMan);

/**
 * Set the known events table for static event name lookups.
 * This is a null-terminated array of KnownEvent.
 */
void osi_func_cache_set_known_events(KnownEvent *events);

// ============================================================================
// Enumeration
// ============================================================================

/**
 * Enumerate all Osiris functions by probing ID ranges.
 * Call this after runtime pointers are set and game is initialized.
 */
void osi_func_enumerate(void);

/**
 * Walk the Osiris name index (CSearchIndex in COsiFunctionMan) and cache every
 * function by name — including databases (DB_*), which the id-probe in
 * osi_func_enumerate() cannot see. Call once the story/save is loaded so
 * Osi.DB_*:Get() can resolve. Idempotent; read-only.
 */
void osi_func_enumerate_by_name(void);

/**
 * Re-run enumeration after a cache miss, rate-limited.
 *
 * The initial enumeration is latched and runs before the story loads, so
 * functions registered later never appear. Call this on a miss before
 * concluding a function does not exist. Returns true if new functions appeared.
 */
bool osi_func_refresh_if_stale(void);

/**
 * Clear the refresh rate-limit state (attempt budget and last-refresh time).
 * Call when the story is torn down: the next session's misses must be able to
 * trigger a refresh even if the previous one used the whole budget.
 */
void osi_func_refresh_reset(void);

/**
 * Case-insensitive name lookups, for the "referenced using incorrect case"
 * compatibility path (upstream keeps a lowercase legacy-name index).
 * Return the canonical spelling, or NULL when nothing matches ignoring case
 * or the given spelling was already exact.
 *   osi_func_lookup_name_ci: engine function cache (linear; miss path only)
 *   osi_db_lookup_name_ci:   story/name-index registry (hashed)
 */
const char *osi_func_lookup_name_ci(const char *name);
const char *osi_db_lookup_name_ci(const char *name);

/** Iterate the name -> def registry. Returns 0 past the end. */
int osi_db_entry(int i, const char **outName, void **outDef);

/**
 * Database registry (databases have OsiFunctionId==0 and cannot be id-cached).
 * Osiris overloads by arity (DB_Dialogs/2 .. /5 are distinct databases), so
 * entries are keyed "Name/Arity" like upstream's name index.
 * osi_db_register_arity: (name, arity, inArgs) -> COsiFunctionData* (1 if new).
 * osi_db_register: arity-less registration (filed as arity 0).
 * osi_db_lookup: bare name -> the lowest-arity overload, or NULL.
 * osi_db_lookup_arity: exact "Name/Arity", or NULL.
 * osi_db_lookup_args: the overload whose *input* parameter count equals the
 *   caller's argument count (upstream OsirisNameCache::GetFunction), or NULL.
 * osi_def_read_arity: total (in+out) and out-param counts from a def's Signature.
 */
#define OSI_DB_MAX_OUT_PARAMS 8
int   osi_db_register_arity(const char *name, unsigned arity, unsigned inArgs, void *def);
int   osi_db_register(const char *name, void *def);
void *osi_db_lookup(const char *name);
void *osi_db_lookup_arity(const char *name, unsigned arity);

/* The database registered under `name` when exactly ONE is, with its arity in
 * *out_arity. NULL when the name is unknown or carries several overloads.
 *
 * Callers need to tell "this name does not exist" apart from "it exists but not
 * at the arity you asked for" -- the two demand different answers, and both
 * were reporting the missing-database error. Restricting this to an
 * unambiguous name keeps it from silently choosing between overloads. */
void *osi_db_lookup_sole(const char *name, unsigned *out_arity);
void *osi_db_lookup_args(const char *name, unsigned nargs);
bool  osi_def_read_arity(void *def, unsigned *outArity, unsigned *outOutParams);
int   osi_db_count(void);
void  osi_db_clear(void);   // wipe registry (defs go stale across save reloads)

// ============================================================================
// Caching
// ============================================================================

/**
 * Cache a function with known metadata.
 * Used when we observe function calls and already know the details.
 */
void osi_func_cache(const char *name, uint32_t funcId, uint8_t arity, uint8_t type);

/**
 * Try to cache a function by probing its ID.
 * Uses pFunctionData to get metadata if available.
 * @return 1 if successfully cached, 0 otherwise
 */
int osi_func_cache_by_id(uint32_t funcId);

/**
 * Try to cache a function from an observed event.
 * Only caches if not already in cache.
 */
void osi_func_cache_from_event(uint32_t funcId);

// ============================================================================
// Lookup
// ============================================================================

/**
 * Get function name from function ID.
 * @return Function name, or NULL if not found
 */
const char *osi_func_get_name(uint32_t funcId);

/**
 * Look up function ID by name.
 * @return Function ID, or INVALID_FUNCTION_ID if not found
 */
uint32_t osi_func_lookup_id(const char *name);

/**
 * Get function info (arity and type) by name.
 * @return 1 on success, 0 if not found
 */
int osi_func_get_info(const char *name, uint8_t *out_arity, uint8_t *out_type);

/**
 * Every cached engine overload of `name`, in cache order.
 *
 * Osiris overloads by arity: BG3 registers MakePlayer/1, /2 and /3 (and
 * ApplyStatus/4 and /5, 84 such names) as distinct functions with distinct
 * ids. osi_func_lookup_id() returns whichever was cached first, which is how
 * Osi.MakePlayer(guid) dispatched the 3-parameter overload with a blank owner.
 * Upstream keys its name cache by input-parameter count
 * (Lua/Osiris/NameCache.inl) and picks by lua_gettop(); the dispatcher does
 * the same over this list. Returns the number of entries written (<= max).
 */
#define OSI_MAX_OVERLOADS 8
int osi_func_lookup_overloads(const char *name, const CachedFunction **out, int max);

/**
 * Get the encoded OsirisFunctionHandle for a function by name.
 * @return Encoded handle, or 0 if not found/not yet computed
 */
uint32_t osi_func_get_handle(const char *name);

/**
 * Set the encoded handle for a cached function.
 */
void osi_func_cache_set_handle(uint32_t funcId, uint32_t handle);

/**
 * Probe and dump OsiFunctionDef layout for the first N cached functions.
 * Writes hex dumps to log for offset discovery/validation.
 */
void osi_func_probe_layout(int count);

/**
 * Probe a function by name and print detailed info to console.
 * Shows cached arity/type/handle, known table match, and re-probes
 * the pointer chain (Signature→ParamList→Size) for live offset validation.
 * @param name Function name to probe
 * @param out Function pointer for console output (must not be NULL)
 */
void osi_func_probe_info(const char *name, void (*out)(const char *fmt, ...));

/**
 * Update a known event's function ID when discovered at runtime.
 * This fixes placeholder entries (funcId=0) in the known events table.
 */
void osi_func_update_known_event_id(const char *name, uint32_t funcId);

// ============================================================================
// Statistics
// ============================================================================

/**
 * Get the number of cached functions.
 */
int osi_func_get_cache_count(void);

/**
 * Track a seen function ID (for analysis/debugging).
 */
void osi_func_track_seen(uint32_t funcId, uint8_t arity);

/**
 * Get the count of unique function IDs seen.
 */
int osi_func_get_seen_count(void);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_OSIRIS_FUNCTIONS_H
