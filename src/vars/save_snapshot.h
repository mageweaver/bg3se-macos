/**
 * BG3SE-macOS - Per-savegame snapshots of mod state
 *
 * Upstream keeps ModVars, UserVars and PersistentVars *inside* the savegame, so
 * loading an older save rolls mod state back with it. This port keeps them in
 * per-campaign files (campaign_key.h), which fixes bleed between playthroughs
 * but not reloads inside one: load an earlier save and every mod still sees the
 * state of the timeline you left. Observed 2026-09-28:
 *   - AV Item Shipment Framework had Shipments[...] = true and a Mailboxes
 *     entry naming a container that did not exist in the loaded save, so the
 *     Dye Rack and camp clothes were never delivered;
 *   - TransmogEnhanced's ControlItems listed UUIDs from other reloads, none of
 *     which existed, so it handed out a fresh set of control items every load.
 *
 * Fix: tie a copy of the three stores to each save. A save is identified by a
 * 64-bit FNV-1a hash of the Osiris story bytes -- COsiris::Save streams them out
 * and COsiris::Load streams the same bytes back in -- so no save-file path or
 * game-side hook is needed.
 *
 *   save: the stores are flushed on Running -> Save (main.c); COsiris::Save is
 *         hooked here, the story bytes hashed, and the flushed campaign files
 *         copied to savestate/<hash>/.
 *   load: fake_Load hashes the story before COsiris::Load parses it; session
 *         init then copies savestate/<hash>/ over the campaign files and forces
 *         a re-read, before any mod handler runs.
 *
 * A save with no snapshot (made before this existed) keeps the old behaviour.
 */

#ifndef BG3SE_VARS_SAVE_SNAPSHOT_H
#define BG3SE_VARS_SAVE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>

/** Hook COsiris::Save. Idempotent; needs libOsiris to be loaded. */
bool save_snapshot_install(void);

/** Call from fake_Load immediately before / after the original COsiris::Load. */
void save_snapshot_story_load_begin(void *smart_buf);
void save_snapshot_story_load_end(void *smart_buf, int result);

/**
 * If the story just loaded has a snapshot for `campaign_key`, copy it over the
 * campaign's store files and return true -- the caller must then reload the
 * stores. Consumes the pending load either way.
 */
bool save_snapshot_apply_pending(const char *campaign_key);

/**
 * The campaign the pending (just-loaded) save belongs to, read from its
 * snapshot. A snapshot is keyed by the save's own story bytes, so this is an
 * exact answer even when DB_Avatars cannot be read yet (it reads empty on a
 * second load in one session). Returns false if there is no pending snapshot.
 */
bool save_snapshot_pending_campaign(char *out, size_t out_size);

#endif // BG3SE_VARS_SAVE_SNAPSHOT_H
