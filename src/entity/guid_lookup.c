/**
 * guid_lookup.c - GUID to EntityHandle lookup utilities
 *
 * Implementation of HashMap operations and GUID parsing for BG3 ECS.
 */

#include "guid_lookup.h"
#include "../core/logging.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

// ============================================================================
// GUID Parsing
// ============================================================================

/**
 * Extract the 36-character UUID from a full template GUID string.
 *
 * BG3 character entities use template GUIDs with prefixes like:
 *   "S_PLA_ConflictedFlind_Hyena_01_69bc3485-8f3b-4a76-a3ca-fd9da89bb908"
 *
 * This function extracts just the UUID portion:
 *   "69bc3485-8f3b-4a76-a3ca-fd9da89bb908"
 *
 * @param guid The full template GUID string
 * @return Pointer to the 36-char UUID portion (within the same string),
 *         or the original string if already a valid UUID or not extractable
 */
const char *extract_uuid_from_guid(const char *guid) {
    if (!guid) return guid;

    size_t len = strlen(guid);

    // If it's already 36 chars (raw UUID), return as-is
    if (len == 36) return guid;

    // Must be at least 36 chars to contain a UUID
    if (len < 36) return guid;

    // Check if last 36 chars form a valid UUID pattern
    const char *uuid_start = guid + len - 36;

    // Validate: must be preceded by underscore (or be at start)
    if (uuid_start != guid && uuid_start[-1] != '_') {
        return guid;
    }

    // Validate UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    if (uuid_start[8] == '-' && uuid_start[13] == '-' &&
        uuid_start[18] == '-' && uuid_start[23] == '-') {
        return uuid_start;
    }

    return guid;
}

// Helper: Convert hex character to value
static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Helper: Parse N hex characters into a uint64_t
static bool parse_hex_bytes(const char *str, int num_chars, uint64_t *out) {
    *out = 0;
    for (int i = 0; i < num_chars; i++) {
        int val = hex_char_to_int(str[i]);
        if (val < 0) return false;
        *out = (*out << 4) | val;
    }
    return true;
}

bool guid_parse(const char *guid_str, Guid *out_guid) {
    if (!guid_str || !out_guid) return false;

    // Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
    size_t len = strlen(guid_str);
    if (len != 36) return false;

    // Validate dashes
    if (guid_str[8] != '-' || guid_str[13] != '-' ||
        guid_str[18] != '-' || guid_str[23] != '-') {
        return false;
    }

    // Parse each section as hex bytes directly
    // Format: AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE
    //
    // BG3 uses a specific byte layout for GUIDs (from Windows BG3SE):
    // The UUID is first parsed into standard format, then Val[1] gets byte-swapped.
    //
    // Standard UUID layout (after UuidFromStringA):
    //   bytes 0-3:   time_low (little-endian from AAAAAAAA)
    //   bytes 4-5:   time_mid (little-endian from BBBB)
    //   bytes 6-7:   time_hi_and_version (little-endian from CCCC)
    //   bytes 8-9:   clock_seq (DDDD - big-endian in UUID)
    //   bytes 10-15: node (EEEEEEEEEEEE - big-endian in UUID)
    //
    // Val[0] = bytes 0-7
    // Val[1] = bytes 8-15 (with BG3's byte swap applied)

    uint8_t bytes[16];

    // Parse each hex pair into the byte array
    // Section A: bytes 0-3 (little-endian, so reverse)
    for (int i = 0; i < 4; i++) {
        int v = hex_char_to_int(guid_str[i*2]) * 16 + hex_char_to_int(guid_str[i*2 + 1]);
        if (v < 0) return false;
        bytes[3 - i] = (uint8_t)v;  // Reverse for little-endian
    }

    // Section B: bytes 4-5 (little-endian, so reverse)
    for (int i = 0; i < 2; i++) {
        int v = hex_char_to_int(guid_str[9 + i*2]) * 16 + hex_char_to_int(guid_str[9 + i*2 + 1]);
        if (v < 0) return false;
        bytes[5 - i] = (uint8_t)v;  // Reverse for little-endian
    }

    // Section C: bytes 6-7 (little-endian, so reverse)
    for (int i = 0; i < 2; i++) {
        int v = hex_char_to_int(guid_str[14 + i*2]) * 16 + hex_char_to_int(guid_str[14 + i*2 + 1]);
        if (v < 0) return false;
        bytes[7 - i] = (uint8_t)v;  // Reverse for little-endian
    }

    // Section D: bytes 8-9 (big-endian in UUID, so no reverse)
    for (int i = 0; i < 2; i++) {
        int v = hex_char_to_int(guid_str[19 + i*2]) * 16 + hex_char_to_int(guid_str[19 + i*2 + 1]);
        if (v < 0) return false;
        bytes[8 + i] = (uint8_t)v;
    }

    // Section E: bytes 10-15 (big-endian in UUID, so no reverse)
    for (int i = 0; i < 6; i++) {
        int v = hex_char_to_int(guid_str[24 + i*2]) * 16 + hex_char_to_int(guid_str[24 + i*2 + 1]);
        if (v < 0) return false;
        bytes[10 + i] = (uint8_t)v;
    }

    // Now apply BG3's byte swap to the second 64-bit value
    // From Windows BG3SE:
    //   uuid.Val[1] = (((v1 >> 56) & 0xff) << 48) |
    //                 (((v1 >> 48) & 0xff) << 56) |
    //                 (((v1 >> 40) & 0xff) << 32) |
    //                 (((v1 >> 32) & 0xff) << 40) |
    //                 (((v1 >> 24) & 0xff) << 16) |
    //                 (((v1 >> 16) & 0xff) << 24) |
    //                 (((v1 >> 8) & 0xff) << 0) |
    //                 (((v1 >> 0) & 0xff) << 8);
    //
    // This swaps adjacent byte pairs: bytes 8-9, 10-11, 12-13, 14-15

    // Copy Val[0] directly (bytes 0-7)
    out_guid->lo = ((uint64_t)bytes[0]) |
                   ((uint64_t)bytes[1] << 8) |
                   ((uint64_t)bytes[2] << 16) |
                   ((uint64_t)bytes[3] << 24) |
                   ((uint64_t)bytes[4] << 32) |
                   ((uint64_t)bytes[5] << 40) |
                   ((uint64_t)bytes[6] << 48) |
                   ((uint64_t)bytes[7] << 56);

    // Build Val[1] with BG3's byte swap (swap adjacent pairs)
    // Original order: 8, 9, 10, 11, 12, 13, 14, 15
    // BG3 order:      9, 8, 11, 10, 13, 12, 15, 14
    out_guid->hi = ((uint64_t)bytes[9]) |
                   ((uint64_t)bytes[8] << 8) |
                   ((uint64_t)bytes[11] << 16) |
                   ((uint64_t)bytes[10] << 24) |
                   ((uint64_t)bytes[13] << 32) |
                   ((uint64_t)bytes[12] << 40) |
                   ((uint64_t)bytes[15] << 48) |
                   ((uint64_t)bytes[14] << 56);

    return true;
}

void guid_to_string(const Guid *guid, char *out_str) {
    if (!guid || !out_str) return;

    // Invert guid_parse()'s exact transformation.  The first three textual
    // fields occupy lo after their per-field little-endian parsing.  The
    // final two fields occupy hi after adjacent byte pairs were swapped;
    // each resulting 16-bit word therefore has the textual byte order, but
    // the three words making up E must be read from low to high.
    uint32_t a = (uint32_t)(guid->lo & 0xFFFFFFFFULL);
    uint16_t b = (uint16_t)((guid->lo >> 32) & 0xFFFFULL);
    uint16_t c = (uint16_t)((guid->lo >> 48) & 0xFFFFULL);
    uint16_t d = (uint16_t)(guid->hi & 0xFFFFULL);
    uint16_t e0 = (uint16_t)((guid->hi >> 16) & 0xFFFFULL);
    uint16_t e1 = (uint16_t)((guid->hi >> 32) & 0xFFFFULL);
    uint16_t e2 = (uint16_t)((guid->hi >> 48) & 0xFFFFULL);

    snprintf(out_str, 37, "%08x-%04x-%04x-%04x-%04x%04x%04x",
             (unsigned int)a, (unsigned int)b, (unsigned int)c,
             (unsigned int)d, (unsigned int)e0, (unsigned int)e1,
             (unsigned int)e2);
}

// ============================================================================
// HashMap Lookup
// ============================================================================

// Debug flag - set to 1 to enable verbose GUID lookup logging
static int g_guid_lookup_debug = 1;

EntityHandle hashmap_lookup_guid(const HashMapGuidEntityHandle *hashmap, const Guid *guid) {
    if (!hashmap || !guid) {
        if (g_guid_lookup_debug) {
            LOG_ENTITY_DEBUG("[GuidLookup] NULL hashmap or guid");
        }
        return ENTITY_HANDLE_INVALID;
    }

    // Log HashMap state on first lookup (or always for debugging)
    if (g_guid_lookup_debug) {
        LOG_ENTITY_DEBUG("[GuidLookup] HashMap state: HashKeys.buf=%p size=%u, Keys.buf=%p cap=%u size=%u, Values.buf=%p size=%u",
            (void*)hashmap->HashKeys.buf, hashmap->HashKeys.size,
            (void*)hashmap->Keys.buf, hashmap->Keys.capacity, hashmap->Keys.size,
            (void*)hashmap->Values.buf, hashmap->Values.size);

        LOG_ENTITY_DEBUG("[GuidLookup] Searching for GUID: lo=%016llx hi=%016llx",
            (unsigned long long)guid->lo, (unsigned long long)guid->hi);

        // Show first few HashMap entries for comparison
        if (hashmap->Keys.size > 0 && hashmap->Keys.buf) {
            int show_count = (hashmap->Keys.size < 3) ? hashmap->Keys.size : 3;
            for (int i = 0; i < show_count; i++) {
                Guid *k = &hashmap->Keys.buf[i];
                LOG_ENTITY_DEBUG("[GuidLookup] HashMap key[%d]: lo=%016llx hi=%016llx",
                    i, (unsigned long long)k->lo, (unsigned long long)k->hi);
            }
        }
    }

    // Validate structure
    if (!hashmap->HashKeys.buf || hashmap->HashKeys.size == 0) {
        if (g_guid_lookup_debug) {
            LOG_ENTITY_DEBUG("[GuidLookup] FAIL: HashKeys empty or NULL");
        }
        return ENTITY_HANDLE_INVALID;
    }

    // Hash the GUID (simple XOR hash matching BG3's implementation)
    uint64_t hash = guid->lo ^ guid->hi;
    uint32_t bucket = (uint32_t)(hash % hashmap->HashKeys.size);

    if (g_guid_lookup_debug) {
        LOG_ENTITY_DEBUG("[GuidLookup] hash=%016llx bucket=%u",
            (unsigned long long)hash, bucket);
    }

    // Get initial index from bucket
    int32_t keyIndex = hashmap->HashKeys.buf[bucket];

    if (g_guid_lookup_debug) {
        LOG_ENTITY_DEBUG("[GuidLookup] Initial keyIndex from bucket: %d", keyIndex);
    }

    // Follow collision chain
    int iterations = 0;
    while (keyIndex >= 0) {
        iterations++;
        // Bounds check
        if ((uint32_t)keyIndex >= hashmap->Keys.size) {
            if (g_guid_lookup_debug) {
                LOG_ENTITY_DEBUG("[GuidLookup] BOUNDS: keyIndex %d >= Keys.size %u",
                    keyIndex, hashmap->Keys.size);
            }
            break;
        }

        // Compare GUID
        const Guid *key = &hashmap->Keys.buf[keyIndex];

        if (g_guid_lookup_debug && iterations <= 3) {
            LOG_ENTITY_DEBUG("[GuidLookup] Comparing with key[%d]: lo=%016llx hi=%016llx",
                keyIndex, (unsigned long long)key->lo, (unsigned long long)key->hi);
        }

        if (key->lo == guid->lo && key->hi == guid->hi) {
            // Found it!
            EntityHandle result = hashmap->Values.buf[keyIndex];
            if (g_guid_lookup_debug) {
                LOG_ENTITY_DEBUG("[GuidLookup] FOUND at index %d: handle=0x%llx",
                    keyIndex, (unsigned long long)result);
            }
            return result;
        }

        // Also check with SWAPPED byte order for debugging
        if (g_guid_lookup_debug && key->lo == guid->hi && key->hi == guid->lo) {
            LOG_ENTITY_DEBUG("[GuidLookup] WOULD MATCH with SWAPPED lo/hi!");
        }

        // Follow collision chain
        if ((uint32_t)keyIndex >= hashmap->NextIds.size) {
            if (g_guid_lookup_debug) {
                LOG_ENTITY_DEBUG("[GuidLookup] BOUNDS: keyIndex %d >= NextIds.size %u",
                    keyIndex, hashmap->NextIds.size);
            }
            break;
        }
        keyIndex = hashmap->NextIds.buf[keyIndex];
    }

    if (g_guid_lookup_debug) {
        LOG_ENTITY_DEBUG("[GuidLookup] NOT FOUND after %d iterations", iterations);
    }

    return ENTITY_HANDLE_INVALID;
}

// ============================================================================
// Debug Functions
// ============================================================================

void hashmap_dump(const HashMapGuidEntityHandle *hashmap, int max_entries) {
    if (!hashmap) {
        LOG_ENTITY_DEBUG("HashMap is NULL");
        return;
    }

    LOG_ENTITY_DEBUG("HashMap dump:");
    LOG_ENTITY_DEBUG("  HashKeys: buf=%p, size=%u",
                (void*)hashmap->HashKeys.buf, hashmap->HashKeys.size);
    LOG_ENTITY_DEBUG("  NextIds: buf=%p, capacity=%u, size=%u",
                (void*)hashmap->NextIds.buf, hashmap->NextIds.capacity, hashmap->NextIds.size);
    LOG_ENTITY_DEBUG("  Keys: buf=%p, capacity=%u, size=%u",
                (void*)hashmap->Keys.buf, hashmap->Keys.capacity, hashmap->Keys.size);
    LOG_ENTITY_DEBUG("  Values: buf=%p, size=%u",
                (void*)hashmap->Values.buf, hashmap->Values.size);

    // Dump some entries
    int count = (max_entries > 0 && (uint32_t)max_entries < hashmap->Keys.size)
                ? max_entries : (int)hashmap->Keys.size;
    if (count > 20) count = 20;  // Safety limit

    for (int i = 0; i < count; i++) {
        Guid *key = &hashmap->Keys.buf[i];
        EntityHandle value = hashmap->Values.buf[i];
        LOG_ENTITY_DEBUG("  [%d] GUID: %016llx-%016llx -> Handle: 0x%llx",
                    i, (unsigned long long)key->lo, (unsigned long long)key->hi,
                    (unsigned long long)value);
    }

    if ((int)hashmap->Keys.size > count) {
        LOG_ENTITY_DEBUG("  ... (%u more entries)", hashmap->Keys.size - count);
    }
}
