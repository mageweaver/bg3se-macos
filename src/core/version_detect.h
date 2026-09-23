/**
 * BG3SE-macOS - Game Binary Version Detection
 *
 * Detects the BG3 game binary version and compares against the version
 * that our hardcoded addresses were extracted from. When mismatched,
 * address-dependent features are disabled to prevent crashes from
 * shifted offsets (Issue #73).
 */

#ifndef VERSION_DETECT_H
#define VERSION_DETECT_H

#include "build_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The game version our addresses were verified against, from
 * src/gen/build_identity.h; every supported store ships this version. All
 * TypeId addresses, singleton pointers and function offsets in
 * generated_typeids.h and component_typeid.c are for it.
 *
 * Not the whole identity on its own -- see build_identity.h for why.
 */
#define BG3_KNOWN_VERSION BG3SE_TARGET_VERSION

/**
 * Detect the game binary version.
 * Reads CFBundleShortVersionString from BG3's Info.plist.
 * Falls back to scanning the binary for version patterns.
 *
 * @param app_bundle_path Path to the .app bundle (or NULL for auto-detect)
 * @return true if version was detected (check version_detect_get_version())
 */
bool version_detect_init(const char *app_bundle_path);

/**
 * Get the detected game version string.
 * Returns NULL if version_detect_init() hasn't been called or failed.
 */
const char *version_detect_get_version(void);

/**
 * Check if the detected version matches our known-good version.
 * Returns true if versions match or if detection failed (optimistic).
 */
bool version_detect_matches(void);

/**
 * Set the main binary base address for sentinel probing.
 * Must be called after finding the BG3 binary in loaded images.
 *
 * @param base The runtime base address of the main BG3 binary
 */
void version_detect_set_binary_base(void *base);

/**
 * As above, but also records the image path so the store can be identified.
 * Prefer this: the store is what distinguishes two builds sharing a version.
 *
 * @param base       The runtime base address of the main BG3 binary
 * @param image_path The loaded image's path (from _dyld_get_image_name)
 */
void version_detect_set_binary_image(void *base, const char *image_path);

/**
 * Which store a game executable path belongs to: "steam", "gog" or "unknown".
 * Never NULL. Larian suffixes the executable per store ("Baldur's Gate 3 GOG");
 * the Steam layout has no suffix, so an unsuffixed name reads as "steam".
 */
const char *version_detect_store_for_image_path(const char *image_path);

/**
 * Read LC_UUID from a mapped Mach-O header into `out`, uppercase-hyphenated.
 * False if the header is not 64-bit Mach-O or has no LC_UUID. `out_size` >= 37.
 */
bool version_detect_uuid_from_image(const void *mach_header, char *out, size_t out_size);

/**
 * __TEXT vmsize for a mapped Mach-O header, or 0 if undeterminable.
 */
uint64_t version_detect_text_vmsize(const void *mach_header);

/**
 * Whether a mapped Mach-O header is a launcher stub rather than the game.
 *
 * True only for an MH_EXECUTE with a __TEXT far too small to be BG3 (~138MB
 * against a stub's 32KB). The filetype test is load-bearing: this dylib's own
 * __TEXT is also small, and passing the wrong image here disables the extender
 * in the process it was supposed to run in.
 *
 * False for anything undeterminable, so an unreadable header fails open.
 */
bool version_detect_is_launcher_stub(const void *mach_header);

/**
 * Whether `build_id` names the game build now running.
 *
 * Generated tables stamp a build id that may carry a "-<store>" suffix
 * ("4.1.1.7398727-gog"), while the detected version comes from Info.plist and
 * never does. Comparing them raw closes every gate that guards generated data
 * on a non-Steam build -- which silently disabled all 2004 component TypeIds on
 * GOG, and with them every entity query.
 *
 * Only the version half is compared. The store half is already enforced, and
 * far more strictly, by the build-identity check in
 * version_detect_addresses_safe().
 *
 * False for NULL, or when the version has not been detected.
 */
bool version_detect_build_id_matches(const char *build_id);

/**
 * The main executable's mach header, found by MH_EXECUTE filetype.
 *
 * NOT _dyld_get_image_header(0): with DYLD_INSERT_LIBRARIES the inserted
 * library takes index 0, and Steam's overlay has taken it since 2026-08-29.
 * Returns NULL if no main executable is mapped.
 */
const void *version_detect_main_executable(void);

/**
 * The store variant of the running game: "steam", "gog", or "unknown".
 * Derived from the game executable's filename -- Larian suffixes it per store
 * ("Baldur's Gate 3 GOG"); an unsuffixed name is the Steam layout.
 */
const char *version_detect_get_store(void);

/**
 * The running binary's arm64 LC_UUID as an uppercase-hyphenated string, or
 * NULL if it could not be read. Requires the binary base to have been set.
 */
const char *version_detect_get_binary_uuid(void);

/**
 * Get the runtime base address of the main BG3 binary.
 * Returns NULL if version_detect_set_binary_base() hasn't been called.
 */
void *version_detect_get_binary_base(void);

/**
 * Check if address-dependent features should be enabled.
 *
 * Fails CLOSED on a build-identity mismatch: wrong store, or a UUID that does
 * not match the one recorded at build time. The sentinel probes cannot rescue
 * either, because they only prove an address is READABLE, not that it holds
 * what we think -- a Steam dylib on the GOG build passes all three (GOG's
 * __DATA spans those addresses) and then reads the wrong objects.
 *
 * Past those gates, returns true if:
 *   - Version matches exactly, OR
 *   - Version mismatches but sentinel address probes pass
 *     (binary layout unchanged despite version string change)
 *
 * BG3SE_FORCE_ADDRESSES=1 overrides every check above.
 */
bool version_detect_addresses_safe(void);

#ifdef __cplusplus
}
#endif

#endif // VERSION_DETECT_H
