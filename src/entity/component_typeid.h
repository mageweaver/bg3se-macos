/**
 * component_typeid.h - TypeId<T>::m_TypeIndex global discovery
 *
 * On macOS, component type indices are stored in global variables with mangled names:
 *   __ZN2ls6TypeIdIN3ecl4ItemEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE
 *
 * These variables hold the actual ComponentTypeIndex for each component type.
 * Generated records retain the exact symbol and preferred VA for one build;
 * no uniform data-segment shift is applied.
 */

#ifndef COMPONENT_TYPEID_H
#define COMPONENT_TYPEID_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the TypeId discovery system.
 * @param binaryBase Base address of the main executable
 * @return true if initialization succeeded
 */
bool component_typeid_init(void *binaryBase);

/**
 * Check if the TypeId system is ready.
 */
bool component_typeid_ready(void);

// ============================================================================
// Discovery
// ============================================================================

/**
 * Discover component type indices by reading TypeId globals.
 * This reads known TypeId<T>::m_TypeIndex addresses and updates the component registry.
 * @return Number of components discovered
 */
int component_typeid_discover(void);

/**
 * Discover TypeIds for all generated components (2004 components).
 * Called after component_typeid_discover() to fill in the rest.
 * @return Number of additional components discovered
 */
int component_typeid_discover_all_generated(void);

/**
 * Read a specific TypeId global address.
 * @param preferred_va Build-specific preferred VA of the m_TypeIndex global
 * @param outIndex Output: the type index value
 * @return true if read succeeded and index is valid
 */
bool component_typeid_read(uint64_t preferred_va, uint16_t *outIndex);

/**
 * Look up a generated preferred VA by exact component name and TypeId context.
 * The generated implementation also retains the raw mangled symbol and build
 * identity for offline audit.
 */
bool component_typeid_generated_lookup(const char *name, const char *context,
                                       uint64_t *out_va);

/** Return true when a component's layout metadata is curated in this module. */
bool component_typeid_is_curated(const char *name);

// ============================================================================
// Debug
// ============================================================================

/**
 * Dump all known TypeId addresses and their values (to log file).
 */
void component_typeid_dump(void);

/**
 * Dump TypeId status to console.
 * Shows resolved/unresolved count for each known component TypeId.
 * Used by the !typeids console command.
 */
void component_typeid_dump_to_console(void);

#endif // COMPONENT_TYPEID_H
