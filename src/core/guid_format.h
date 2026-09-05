/**
 * guid_format.h - ls::Guid <-> text in Larian's byte order
 *
 * Upstream's Guid::ToString() (CoreLib/Base/Base.cpp) prints
 *
 *   p[3]p[2]p[1]p[0]-p[5]p[4]-p[7]p[6]-p[9]p[8]-p[11]p[10]p[13]p[12]p[15]p[14]
 *
 * EVERY adjacent byte pair is swapped, not just the first three fields -- the
 * 16 bytes read as one little-endian uint32 followed by six little-endian
 * uint16s. Printing the last eight bytes in memory order (the RFC 4122 layout)
 * gives a string that round-trips through our own parser but matches nothing a
 * mod hardcodes: Ext.StaticData.GetAll("Origin") handed back
 * a4b56492-d5ac-4a84-458e-37549dcdf3a7 for Tav where the game and every mod
 * say a4b56492-d5ac-4a84-8e45-5437cd9da7f3. Every `entry == SOME_GUID` in mod
 * code was therefore false, and CustomCompanions -- which hides every origin
 * except the one it matched -- hid all of them, leaving a blank character
 * creation screen.
 *
 * Use these two helpers for anything that crosses between engine Guid bytes
 * and a string a mod can see.
 */

#ifndef BG3SE_GUID_FORMAT_H
#define BG3SE_GUID_FORMAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GUID_STRING_SIZE 37  /* 36 chars + NUL */

static inline uint16_t guid_read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/** Format 16 raw Guid bytes. `out_size` must be >= GUID_STRING_SIZE. */
static inline void guid_bytes_to_string(const uint8_t g[16], char *out, size_t out_size) {
    if (!g || !out || out_size < GUID_STRING_SIZE) return;
    uint32_t d1;
    memcpy(&d1, g, sizeof(d1));
    snprintf(out, out_size, "%08x-%04x-%04x-%04x-%04x%04x%04x",
             d1, guid_read_u16(g + 4), guid_read_u16(g + 6), guid_read_u16(g + 8),
             guid_read_u16(g + 10), guid_read_u16(g + 12), guid_read_u16(g + 14));
}

/** Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into 16 raw Guid bytes. */
static inline bool guid_string_to_bytes(const char *s, uint8_t g[16]) {
    if (!s || !g) return false;
    unsigned int d1, d[6];
    if (sscanf(s, "%8x-%4x-%4x-%4x-%4x%4x%4x",
               &d1, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]) != 7) {
        return false;
    }
    uint32_t v1 = (uint32_t)d1;
    memcpy(g, &v1, sizeof(v1));
    for (int i = 0; i < 6; i++) {
        uint16_t v = (uint16_t)d[i];
        memcpy(g + 4 + i * 2, &v, sizeof(v));
    }
    return true;
}

#endif /* BG3SE_GUID_FORMAT_H */
