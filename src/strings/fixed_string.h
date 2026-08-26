/**
 * fixed_string.h - FixedString Resolution for macOS BG3
 *
 * On macOS ARM64, FixedString is a 32-bit index into GlobalStringTable,
 * not a direct pointer. This module resolves indices to actual strings.
 *
 * Index encoding:
 *   subTableIdx = (id & 0x0F)        // bits 0-3: sub-table selector (0-10)
 *   bucketIdx   = (id >> 4) & 0xFFFF // bits 4-19: bucket index
 *   entryIdx    = (id >> 20)         // bits 20+: entry within bucket
 */

#ifndef FIXED_STRING_H
#define FIXED_STRING_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// FixedString Index Constants
// ============================================================================

#define FS_NULL_INDEX        0xFFFFFFFF
#define FS_SUBTABLE_MASK     0x0F
#define FS_BUCKET_MASK       0xFFFF
#define FS_BUCKET_SHIFT      4
#define FS_ENTRY_SHIFT       20

// ============================================================================
// StringEntry Header (24 bytes before string data)
// From BG3SE CoreLib/Base/BaseString.h:111-118
// ============================================================================

#define STRING_ENTRY_HEADER_SIZE 0x18

typedef struct {
    uint32_t Hash;           // +0x00: FNV-1a hash of string
    uint32_t RefCount;       // +0x04: Reference count
    uint32_t Length;         // +0x08: String length
    uint32_t Id;             // +0x0C: FixedString index
    uint64_t NextFreeIndex;  // +0x10: Free list pointer
    // char Str[] follows at +0x18
} StringEntryHeader;

// ============================================================================
// SubTable Field Offsets (Windows x64 from BG3SE - may need ARM64 adjustment)
// From BG3SE CoreLib/Base/BaseString.h:326-352
// ============================================================================

// These are Windows x64 offsets - will probe at runtime for ARM64
#define SUBTABLE_OFFSET_ENTRY_SIZE       0x1088
#define SUBTABLE_OFFSET_ENTRIES_PER_BKT  0x1090
#define SUBTABLE_OFFSET_NUM_BUCKETS      0x10C0
#define SUBTABLE_OFFSET_BUCKETS          0x1140
#define SUBTABLE_SIZE                    0x1200  // ~4.5 KB per SubTable

// ============================================================================
// GlobalStringTable Layout
// ============================================================================

#define GST_NUM_SUBTABLES                11
#define GST_OFFSET_MAINTABLE             0xC600  // MainTable at end

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize the FixedString resolution system.
 * Call once at startup after main binary is loaded.
 *
 * @param main_binary_base  Base address of the main game binary (for pattern scanning)
 */
void fixed_string_init(void *main_binary_base);

/**
 * Resolve a FixedString index to its string value.
 *
 * @param index  The FixedString index (e.g., 0x20200011)
 * @return Pointer to the string, or NULL if invalid/not found
 *
 * Example:
 *   uint32_t name_idx = 0x20200011;
 *   const char* name = fixed_string_resolve(name_idx);
 *   // name might be "Longsword" or similar
 */
const char *fixed_string_resolve(uint32_t index);

/**
 * Check if a FixedString index is valid (not null index).
 */
bool fixed_string_is_valid(uint32_t index);

/**
 * Check if the FixedString system is ready for use.
 */
bool fixed_string_is_ready(void);

/** Drop every memoised resolve. Only needed if the string table is rebuilt. */
void fixed_string_cache_clear(void);

/**
 * Intern a string into the GlobalStringTable and return its FixedString index.
 * This calls the game's ls::FixedString::Create() function.
 *
 * @param str     The string to intern (null-terminated)
 * @param len     Length of the string, or -1 to use strlen()
 * @return FixedString index, or FS_NULL_INDEX on failure
 *
 * Example:
 *   uint32_t fs = fixed_string_intern("MyCustomSpell", -1);
 *   // fs is now a valid FixedString index usable in RefMaps
 */
uint32_t fixed_string_intern(const char *str, int len);

/**
 * Check if FixedString interning is available.
 * Returns true if fixed_string_intern() can be called.
 */
bool fixed_string_intern_ready(void);

/**
 * Get the pre-computed hash value for a FixedString.
 * This retrieves the murmur3a32 hash stored in the GlobalStringTable entry.
 *
 * @param index  The FixedString index
 * @return Hash value, or 0 if invalid index
 */
uint32_t fixed_string_get_hash(uint32_t index);

// ============================================================================
// Debug API
// ============================================================================

/**
 * Dump information about a specific SubTable for debugging.
 */
void fixed_string_dump_subtable_info(int subtable_idx);

/**
 * Probe and discover SubTable field offsets at runtime.
 * Returns true if successful.
 */
bool fixed_string_probe_offsets(void);

/**
 * Get debug statistics about the FixedString system.
 */
void fixed_string_get_stats(uint32_t *out_resolved, uint32_t *out_failed);

/**
 * Probe for GlobalStringTable near a known pointer (like RPGStats).
 * Call this after SessionLoaded when stats_ptr is valid.
 *
 * @param stats_ptr  The actual runtime RPGStats pointer
 * @return true if GlobalStringTable was found
 */
bool fixed_string_probe_near_ptr(void *stats_ptr);

#endif /* FIXED_STRING_H */
