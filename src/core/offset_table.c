/**
 * offset_table.c - Per-version memory offset table for BG3SE-macOS
 *
 * To add support for a new BG3 version, DON'T do it by hand — run the resolver:
 *     python3 tools/port_offsets.py resolve --emit
 * and paste its output (the struct entry + the g_fn_remap_<ver>[] rows) here.
 * The recipe of every address is tools/offset_manifest.json; see docs/PORTING.md.
 * Validate any change with:  python3 tools/port_offsets.py verify
 *
 * Manual fallback (if a symbol can't be resolved):
 *   - Singleton offsets: `nm` the binary, or runtime probe (Ext.Debug.ReadPtr).
 *   - Function offsets: `nm`/Ghidra. Fields left 0 = "unknown" -> callers skip
 *     that feature gracefully rather than crash.
 *
 * All offsets are (Ghidra address - 0x100000000), i.e. offset from binary load base.
 */

#include "offset_table.h"
#include "version_detect.h"
#include "logging.h"

#include <string.h>

// ============================================================================
// Version Table
// ============================================================================

static const VersionOffsets g_offset_table[] = {

    /* ------------------------------------------------------------------
     * 4.1.1.6995620 — original verified version
     * All offsets discovered via Ghidra analysis (Dec 2025).
     * ------------------------------------------------------------------ */
    {
        .version                 = "4.1.1.6995620",

        /* Singleton pointer globals */
        .eocserver_ptr           = 0x0898e8b8,  // esv::EocServer::m_ptr
        .eocclient_ptr           = 0x0898c968,  // ecl::EocClient::m_ptr
        .spell_proto_mgr_ptr     = 0x089bac80,  // SpellPrototypeManager::m_ptr
        .rpgstats_ptr            = 0x089c5730,  // RPGStats::m_ptr
        .resource_mgr_ptr        = 0x08a8f070,  // ResourceManager::m_ptr
        .level_mgr_ptr           = 0x08a3be40,  // LevelManager::m_ptr
        .global_template_mgr_ptr = 0x08a88508,  // ls::GlobalTemplateManager::m_ptr
        .cache_template_mgr_ptr  = 0x08a309a8,  // CacheTemplateManager::m_ptr
        .level_cache_mgr_ptr     = 0x08a735d8,  // Level::s_CacheTemplateManager
        .staticdata_mstate_ptr   = 0x083c4a68,  // ImmutableDataHeadmaster::m_State
        .gst_ptr                 = 0x08aeccd8,  // ls::gGlobalStringTable
        .global_switches_ptr     = 0x08b18f30,  // EoCGlobalSwitches* (May 2026 RE: VMGameData init)
        .osiris_interface_ptr    = 0,           // osi::OsirisInterface instance — never verified
                                                // on this vintage (0 = param-defs reader disabled;
                                                // osi_dynamic_call falls back to legacy guessing)

        /* Function offsets */
        .fn_feat_getfeats        = 0x01b752b4,  // FeatManager::GetFeats
        .fn_getallfeats          = 0x0120b3e8,  // GetAllFeats
        .fn_get_background       = 0x02994834,  // Get<eoc::BackgroundManager>
        .fn_get_origin           = 0x0341c42c,  // Get<eoc::OriginManager>
        .fn_get_class            = 0x0262f184,  // Get<eoc::ClassDescriptions>
        .fn_get_progression      = 0x03697f0c,  // Get<eoc::ProgressionManager>
        .fn_get_actionresource   = 0x011a4494,  // Get<eoc::ActionResourceTypes>
        .fn_get_template_raw     = 0x05f96304,  // GlobalTemplateManager::GetTemplateRaw
        .fn_cache_template       = 0x05d31ce4,  // CacheTemplateManagerBase::CacheTemplate
        .fn_aigrid_to_tile_pos   = 0,           // not derived for this binary vintage
        .fn_aigrid_get_metadata  = 0,           // not derived for this binary vintage
        .fn_aigrid_remove_path   = 0,           // not derived for this binary vintage
        .fn_aigrid_find_path     = 0,           // not derived for this binary vintage
        .fn_aigrid_find_path_immediate = 0,     // not derived for this binary vintage

        /* Entity system */
        .fn_try_get_uuid_mapping = 0x010dc924,  // TryGetSingleton<uuid::ToHandleMappingComponent>
        .fn_storage_tryget       = 0x0636b27c,  // ecs::EntityStorageContainer::TryGet
        .fn_spell_proto_init     = 0x01f72754,  // eoc::SpellPrototype::Init
        .fn_status_proto_init    = 0,           // not derived for this binary vintage
        .component_data_shift    = -0x8000,     // compiled-in __DATA constants are 7209685;
                                                // this version's __DATA sits 0x8000 lower
    },

    /* ------------------------------------------------------------------
     * 4.1.1.7209685 — in use as of June 2026
     *
     * The entire __DATA segment shifted by a uniform +0x8000 relative to
     * 6995620. This was established by:
     *   - Export-trie diff: ls::TypeId<eoc::FeatManager,...>::m_TypeIndex
     *     moved 0x1088efd00 -> 0x1088f7d00 (exactly +0x8000).
     *   - otool ADRP/LDR reference-frequency scan: every old singleton
     *     offset + 0x8000 lands on a hot (heavily-referenced) __DATA slot.
     *   - Runtime structural validation against a loaded session:
     *       rpgstats(+0xd4)=16994 objects, fixedstrings pool at +0x348;
     *       ResourceManager banks valid at +0x28/+0x30;
     *       StaticData TypeContext traversal yields 121 named managers
     *       (FeatManager, RaceManager, BackgroundManager, ... ClassDescriptions).
     * So every singleton below is simply (6995620 value + 0x8000).
     *
     * Function offsets live in __TEXT, which did NOT shift uniformly (the
     * three template accessors moved -0x105E8, the feat funcs -0x1BAA0, the
     * Get<T> accessors ~-0x1A7A8), so they were resolved individually by
     * symbol-table lookup (nm) rather than a constant shift. The macOS
     * binary is essentially fully symbolized (765k local+global symbols),
     * so each function below is the address of its named symbol.
     * ------------------------------------------------------------------ */
    {
        .version                 = "4.1.1.7209685",

        /* Singleton pointer globals — uniform +0x8000 vs 6995620 (validated) */
        .eocserver_ptr           = 0x089968b8,  // 0x0898e8b8 + 0x8000
        .eocclient_ptr           = 0x08994968,  // 0x0898c968 + 0x8000
        .spell_proto_mgr_ptr     = 0x089c2c80,  // 0x089bac80 + 0x8000
        .rpgstats_ptr            = 0x089cd730,  // 0x089c5730 + 0x8000 (count=16994)
        .resource_mgr_ptr        = 0x08a97070,  // 0x08a8f070 + 0x8000 (banks valid)
        .level_mgr_ptr           = 0x08a43e40,  // 0x08a3be40 + 0x8000
        .global_template_mgr_ptr = 0x08a90508,  // 0x08a88508 + 0x8000
        .cache_template_mgr_ptr  = 0x08a389a8,  // 0x08a309a8 + 0x8000
        .level_cache_mgr_ptr     = 0x08a7b5d8,  // 0x08a735d8 + 0x8000
        .staticdata_mstate_ptr   = 0x083cca68,  // 0x083c4a68 + 0x8000 (121 managers)
                                                // NOTE: this is the __DATA_CONST,__got SLOT whose
                                                // contents = 0x108947ba0 (TypeContext<ImmutableData-
                                                // Headmaster>::m_State object) — correct for the
                                                // one-dereference traversal in staticdata_manager.c.
                                                // nm cannot see it; audit via otool -Iv / __got.
        .gst_ptr                 = 0x08af4cd8,  // 0x08aeccd8 + 0x8000 (val=GST ptr, validated)
        .global_switches_ptr     = 0x08af4f30,  // EoCGlobalSwitches* — NOT old+0x8000 (slot moved
                                                // -0x24000). Verified 2026-07-29: 916 disasm refs
                                                // across 593 fns incl. App::CreateGlobalSwitches()
                                                // and BaseApp::CreateGlobalSwitches().
        .osiris_interface_ptr    = 0x08a86128,  // osi::OsirisInterface instance slot. No nm symbol;
                                                // verified 2026-07-29 by otool disasm of
                                                // osi::OsirisInterface::OsirisQuery @ 0x105c093b0:
                                                //   adrp x8, 0x108a86000 ; ldr x25, [x8, #0x128]
                                                // (matches PR #93's live-verified chain; see
                                                // ghidra/offsets/OSIRIS_DATABASES.md)

        /* Function offsets (__TEXT) — resolved by nm symbol lookup on the
         * 7209685 binary; non-uniform shift, see note above. */
        .fn_feat_getfeats        = 0x01b59814,  // eoc::FeatManager::GetFeats() const
        .fn_getallfeats          = 0x011ef948,  // eoc::character_creation::GetAllFeats(Environment const&)
        .fn_get_background       = 0x0297a068,  // ImmutableDataHeadmaster::Get<eoc::BackgroundManager>() const
        .fn_get_origin           = 0x03401c84,  // ImmutableDataHeadmaster::Get<eoc::OriginManager>() const
        .fn_get_class            = 0x02614874,  // ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>() const
        .fn_get_progression      = 0x0367d764,  // ImmutableDataHeadmaster::Get<eoc::ProgressionManager>() const
        .fn_get_actionresource   = 0x011889f4,  // ImmutableDataHeadmaster::Get<eoc::ActionResourceTypes>() const
        .fn_get_template_raw     = 0x05f85d1c,  // ls::GlobalTemplateManager::GetTemplateRaw(FixedString const&) const
        .fn_cache_template       = 0x05d216fc,  // ls::CacheTemplateManagerBase::CacheTemplate(...)
        .fn_aigrid_to_tile_pos   = 0x011619c0,  // eoc::AiGrid::ToTilePos(Vector3f const&, AiTilePos&, bool) const
        .fn_aigrid_get_metadata  = 0x0114b2e4,  // eoc::AiGrid::GetMetaData(AiTilePos const&)
        .fn_aigrid_remove_path   = 0x01161ea4,  // eoc::AiGrid::RemovePath(int)
        .fn_aigrid_find_path     = 0x01162a4c,  // eoc::AiGrid::FindPath(int)
        .fn_aigrid_find_path_immediate = 0x01165cec, // eoc::AiGrid::FindPathImmediate(int, bool)

        /* Entity system (verified end-to-end).
         * fn_try_get_uuid_mapping: ecs::legacy::Helper::TryGetSingleton<
         *   uuid::ToHandleMappingComponent const> (symbol-verified, old-0x1baa0).
         * The earlier crash was NOT this offset — it was read_eocserver_from_global
         * reading a stale EocServer address, yielding a garbage EntityWorld.
         * With the EocServer source fixed (offset table eocserver_ptr) the
         * EntityWorld at EocServer+0x288 is valid (probe-confirmed: 0xc8bb74000
         * with sane sub-structure), and EocServer::StartUp's `ldr x20,[x19,#0x288]`
         * confirms +0x288 is unchanged for this version.
         * component_data_shift: uniform +0x8000 __DATA shift. */
        .fn_try_get_uuid_mapping = 0x010c0e84,
        .fn_storage_tryget       = 0x0635ac94,  // ecs::EntityStorageContainer::TryGet (old 0x0636b27c - 0x105E8)
        .fn_spell_proto_init     = 0x01f56cb4,  // eoc::SpellPrototype::Init (old 0x01f72754 - 0x1baa0)
        .fn_status_proto_init    = 0x01ff7150,  // eoc::StatusPrototype::Init(FixedString const&, bool)
        .component_data_shift    = 0,           // compiled-in __DATA constants ARE this
                                                // vintage (nm-audited 7209685) — no shift
    },

};

#define NUM_VERSIONS (sizeof(g_offset_table) / sizeof(g_offset_table[0]))

// ============================================================================
// Per-version function-address remap
// ============================================================================
//
// Several subsystems hold hardcoded game-function Ghidra addresses (called as
// function pointers). Rather than thread ~20 fields through VersionOffsets,
// callers wrap their hardcoded address with offset_table_remap_fn(), which
// matches EITHER column (the codebase carries mixed vintages) and returns the
// active version's column. Fail-closed: unknown version, or an address absent
// from the table, returns 0 — the caller disables that feature instead of
// jumping to a stale address. All values are full Ghidra addresses
// (>= 0x100000000); every 7209685 value is symbol-verified via nm.

typedef struct { uint64_t addr_6995620; uint64_t addr_7209685; } FnRemap;

static const FnRemap g_fn_remap[] = {
    { 0x1064b9ebc, 0x1064a8a74 },  // ls::FixedString::Create(char const*, int)   (ABI verified)
    { 0x1060cc608, 0x1060bc020 },  // ls::ResourceContainer::GetResource
    { 0x105783a38, 0x10577399c },  // ExecuteStatsFunctor (main dispatcher)
    { 0x105787918, 0x10577787c },  // esv::functor::ExecuteStatsFunctors<AttackTarget>
    { 0x105787c6c, 0x105777bd0 },  //   <AttackPosition>
    { 0x10578975c, 0x1057796c0 },  //   <Move>
    { 0x10578a918, 0x10577a87c },  //   <Target>
    { 0x10578e4d8, 0x10577e43c },  //   <NearbyAttacked>
    { 0x10578fba8, 0x10577fb0c },  //   <NearbyAttacking>
    { 0x105790a28, 0x10578098c },  //   <Equip>
    { 0x105792a90, 0x1057829f4 },  //   <Source>
    { 0x1057965e4, 0x105786548 },  //   <Interrupt> (7209685: leading ecs::EntityWorld&)
    { 0x10538f374, 0x10537e8b4 },  // (anonymous)::ProcessDealDamageFunctors
    { 0x106534d54, 0x10652390c },  // ls::TranslatedStringRepository::TryGet
    { 0x106535148, 0x106523d00 },  // ls::TranslatedStringRepository::Get
    { 0x106532590, 0x106521148 },  // ls::TranslatedStringRepository::AddTranslatedString
    { 0x1063d5998, 0x1063c4550 },  // net::MessageFactory::GetFreeMessage
    { 0x10651fb60, 0x10650e718 },  // ls::STDString::STDString(char const*)  (audio PlayExternalSound)
    // NOTE: the functor rows above are additionally gated by
    // FUNCTOR_ADDRS_VERIFIED_BUILD in functor_types.h. Symbol resolution proves
    // addresses, while that exact-build gate protects the wrapper ABIs.
    // bik::BinkManager::LoadVideo (0x10390b6cc) is unchanged across versions.
    // GetComponent<T> template addresses are intentionally NOT in this table:
    // that path is dead on macOS (templates inlined) — remap returns 0 on
    // every version and the path is skipped.
};

// ============================================================================
// State
// ============================================================================

static const VersionOffsets *g_active = NULL;

// ============================================================================
// Public API
// ============================================================================

void offset_table_init(void) {
    const char *version = version_detect_get_version();
    if (!version) {
        log_message("[OffsetTable] Version not detected — all address-dependent features disabled");
        return;
    }

    for (int i = 0; i < (int)NUM_VERSIONS; i++) {
        if (strcmp(g_offset_table[i].version, version) == 0) {
            g_active = &g_offset_table[i];
            log_message("[OffsetTable] Loaded offsets for %s", version);
            return;
        }
    }

    log_message("[OffsetTable] Version %s not in table — running in degraded mode. "
                "Add a new entry to src/core/offset_table.c to enable full features.",
                version);
}

const VersionOffsets *offset_table_get(void) {
    return g_active;
}

void *offset_table_resolve(uintptr_t offset) {
    if (!offset) return NULL;
    void *base = version_detect_get_binary_base();
    if (!base) return NULL;
    return (void *)((uintptr_t)base + offset);
}

void *offset_table_fn(uintptr_t offset) {
    return offset_table_resolve(offset);
}

uint64_t offset_table_remap_fn(uint64_t ghidra_addr) {
    // Fail closed: no active version row -> every remapped feature disabled.
    if (!g_active) return 0;

    int col;
    if (strcmp(g_active->version, "4.1.1.6995620") == 0)      col = 0;
    else if (strcmp(g_active->version, "4.1.1.7209685") == 0) col = 1;
    else return 0;  // version in table but no remap column -> disabled

    for (size_t i = 0; i < sizeof(g_fn_remap) / sizeof(g_fn_remap[0]); i++) {
        if (g_fn_remap[i].addr_6995620 == ghidra_addr ||
            g_fn_remap[i].addr_7209685 == ghidra_addr) {
            return col ? g_fn_remap[i].addr_7209685 : g_fn_remap[i].addr_6995620;
        }
    }
    return 0;  // unmapped -> disabled (graceful)
}
