/**
 * component_typeid.c - TypeId<T>::m_TypeIndex global discovery
 *
 * Reads TypeId globals from the game binary to discover component type indices.
 * These globals are initialized at game startup with the actual indices.
 */

#include "component_typeid.h"
#include "component_registry.h"
#include "component_property.h"  // For property system linkage
#include "entity_storage.h"  // For GHIDRA_BASE_ADDRESS
#include "generated_typeids.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/version_detect.h"

#include <string.h>

// ============================================================================
// Curated Component Metadata
// ============================================================================

/**
 * Curated size/proxy/one-frame metadata, keyed only by component name.
 * Preferred VAs and mangled symbols are supplied by the generated authority.
 */
typedef struct {
    const char *componentName;  // Full component name (e.g., "ecl::Character")
    uint16_t expectedSize;      // Expected component size (0 = unknown)
    bool isProxy;               // Is this a proxy component?
    const char *context;        // Exact generated TypeId context
    bool isOneFrame;            // Register in the one-frame component pool
} TypeIdEntry;

static const TypeIdEntry g_KnownTypeIds[] = {
    // =====================================================================
    // ecl:: namespace (client components)
    // Layout metadata remains curated; addresses resolve by exact generated
    // symbol for GENERATED_TYPEIDS_BUILD_ID.
    // =====================================================================
    { "ecl::Character", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ecl::Item", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // esv:: proxy components: the ECS slot holds a pointer to the object
    // (esv::Item is 0xb0 bytes; upstream GameDefinitions/Item.h). Exposed as
    // ServerItem for Windows parity. (esv::Character stays unregistered until
    // its layout is verified; the placeholder in component_offsets.h is not.)
    // =====================================================================
    { "esv::Item", 0, true, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // eoc:: namespace (engine of combat)
    // =====================================================================
    { "eoc::HealthComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::StatsComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ArmorComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::BaseHpComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::DataComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // ls:: namespace (base Larian components)
    // =====================================================================
    { "ls::TransformComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::LevelComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::VisualComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::PhysicsComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Components (Issue #33)
    // =====================================================================
    { "eoc::LevelComponent", 0, false, "ecs::ComponentTypeIdContext", false },  // Character level (different from ls::LevelComponent)
    { "eoc::exp::ExperienceComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::exp::AvailableLevelComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::PassiveComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::PassiveContainerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ResistancesComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::TagComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::RaceComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::OriginComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ClassesComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::MovementComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 2 Components (Issue #33)
    // =====================================================================
    { "eoc::BackgroundComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::god::GodComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ValueComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::TurnBasedComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 3 Components (Issue #33) - High-priority gameplay
    // =====================================================================
    { "eoc::WeaponComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::spell::BookComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::status::ContainerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::ContainerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ActionResourcesComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 4 Components (Issue #33) - Inventory relationships
    // =====================================================================
    { "eoc::inventory::OwnerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::MemberComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::IsOwnedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::EquipableComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 5 Components (Issue #33) - Spell and boost components
    // =====================================================================
    { "eoc::spell::ContainerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::concentration::ConcentrationComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::BoostsContainerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::DisplayNameComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 6 Components (Issue #33) - Simple components
    // =====================================================================
    { "eoc::death::StateComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::death::DeathTypeComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::WeightComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::combat::ThreatRangeComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::combat::IsInCombatComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 7 Components (Issue #33) - Combat components
    // =====================================================================
    { "eoc::combat::ParticipantComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::combat::StateComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Phase 2 Batch 8 Components (Issue #33) - Tag components (115 total)
    // Tag components have no fields - their presence on an entity is the data
    // Generated by tools/generate_tag_components.py
    // =====================================================================

    // ecl:: tag components
    { "ecl::camera::IsInSelectorModeComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ecl::camera::SpellTrackingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ecl::dummy::IsCopyingFullPoseComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ecl::dummy::LoadedComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // eoc:: tag components
    { "eoc::CanTriggerRandomCastsComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ClientControlComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::GravityDisabledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::IsInTurnBasedModeComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::OffStageComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::PickingStateComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::PlayerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::SimpleCharacterComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::active_roll::InProgressComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ambush::AmbushingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::camp::PresenceComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::character::CharacterComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::combat::DelayedFanfareComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::exp::CanLevelUpComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::falling::IsFallingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ftb::IsFtbPausedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ftb::IsInFtbComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::heal::BlockComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::heal::MaxIncomingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::heal::MaxOutgoingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::improvised_weapon::CanBeWieldedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::CanBeInComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::CannotBePickpocketedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::CannotBeTakenOutComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::DropOnDeathBlockedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::IsLockedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::NewItemsInsideComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::inventory::NonTradableComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::DestroyingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::DoorComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::ExamineDisabledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::HasMovedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::HasOpenedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::InUseComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::IsGoldComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::IsPoisonedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::ItemComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::NewInInventoryComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item::ShouldDestroyOnSpellCastComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::CanMoveComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::ClimbOnComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::DestroyedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::InteractionDisabledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::IsStoryItemComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::LadderComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::item_template::WalkOnComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::multiplayer::HostComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::ownership::OwnedAsLootComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::party::BlockFollowComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::party::CurrentlyFollowingPartyComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::pickup::PickUpExecutingComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::rest::LongRestInScriptPhase", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::rest::ShortRestComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::spell_cast::CanBeTargetedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::status::IndicateDarknessComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tadpole_tree::FullIllithidComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tadpole_tree::HalfIllithidComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tadpole_tree::TadpoledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tag::AvatarComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tag::HasExclamationDialogComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::tag::TraderComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::through::CanSeeThroughComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::through::CanShootThroughComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::through::CanWalkThroughComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "eoc::trade::CanTradeComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // esv:: tag components
    { "esv::IsMarkedForDeletionComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::NetComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::ScriptPropertyCanBePickpocketedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::ScriptPropertyIsDroppedOnDeathComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::ScriptPropertyIsTradableComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::TurnOrderSkippedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::VariableManagerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::boost::StatusBoostsProcessedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::character_creation::IsCustomComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::combat::CanStartCombatComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::combat::FleeBlockedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::combat::ImmediateJoinComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::combat::LeaveRequestComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::cover::IsLightBlockerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::cover::IsVisionBlockerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::darkness::DarknessActiveComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::death::DeathContinueComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::escort::HasStragglersComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::hotbar::OrderComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::inventory::CharacterHasGeneratedTradeTreasureComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::inventory::EntityHasGeneratedTreasureComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::inventory::IsReplicatedWithComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::inventory::ReadyToBeAddedToInventoryComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::level::InventoryItemDataPopulatedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::rest::ShortRestConsumeResourcesComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::sight::EventsEnabledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::spell_cast::ClientInitiatedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::status::ActiveComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::status::AddedFromSaveLoadComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::status::AuraComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::summon::IsUnsummoningComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::trigger::LoadedHandledComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "esv::trigger::TriggerWorldAutoTriggeredComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Template Components (Issue #41 - Ext.Template support)
    // =====================================================================
    { "eoc::templates::OriginalTemplateComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // ls:: tag components
    { "ls::AlwaysUpdateEffectComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::AnimationUpdateComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::IsGlobalComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::IsSeeThroughComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::LevelIsOwnerComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::LevelPrepareUnloadBusyComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::LevelUnloadBusyComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::SavegameComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::VisualLoadedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::game::PauseComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::game::PauseExcludedComponent", 0, false, "ecs::ComponentTypeIdContext", false },
    { "ls::level::LevelInstanceUnloadingComponent", 0, false, "ecs::ComponentTypeIdContext", false },

    // =====================================================================
    // Issue #51 Event Components - One-frame event components for Ext.Events
    // Their old anonymous pointer slots are no longer used. Both exact
    // OneFrameComponentTypeIdContext symbols remain exported on 7398727.
    // =====================================================================
    { "esv::TurnStartedEventOneFrameComponent", 0, false, "ecs::OneFrameComponentTypeIdContext", true },  // TypeIndex 167
    { "esv::TurnEndedEventOneFrameComponent", 0, false, "ecs::OneFrameComponentTypeIdContext", true },  // TypeIndex 164

    // Sentinel
    { NULL, 0, false, NULL, false }
};

// ============================================================================
// Global State
// ============================================================================

static void *g_BinaryBase = NULL;
static bool g_Initialized = false;

// ============================================================================
// Initialization
// ============================================================================

bool component_typeid_init(void *binaryBase) {
    if (!binaryBase) {
        LOG_ENTITY_DEBUG("ERROR: binaryBase is NULL");
        return false;
    }

    g_BinaryBase = binaryBase;
    g_Initialized = true;

    LOG_ENTITY_DEBUG("Initialized with binary base: %p", binaryBase);
    return true;
}

bool component_typeid_ready(void) {
    return g_Initialized && g_BinaryBase != NULL;
}

// ============================================================================
// TypeId Reading
// ============================================================================

static bool component_typeid_runtime_build_matches(void) {
    const char *detected_build = version_detect_get_version();
    return detected_build != NULL && version_detect_matches() &&
           strcmp(detected_build, GENERATED_TYPEIDS_BUILD_ID) == 0;
}

static bool component_typeid_runtime_address(uint64_t preferred_va,
                                             mach_vm_address_t *out_address) {
    if (!out_address || preferred_va < GHIDRA_BASE_ADDRESS || !g_BinaryBase) {
        return false;
    }
    *out_address = preferred_va - GHIDRA_BASE_ADDRESS +
                   (mach_vm_address_t)g_BinaryBase;
    return true;
}

bool component_typeid_read(uint64_t preferred_va, uint16_t *outIndex) {
    if (!component_typeid_ready() || !outIndex ||
        !component_typeid_runtime_build_matches()) {
        return false;
    }

    mach_vm_address_t runtimeAddr = 0;
    if (!component_typeid_runtime_address(preferred_va, &runtimeAddr)) {
        return false;
    }

    /* Validate the runtime address before attempting to read */
    SafeMemoryInfo info = safe_memory_check_address(runtimeAddr);
    if (!info.is_valid || !info.is_readable) {
        LOG_ENTITY_DEBUG("  Address 0x%llx (preferred VA 0x%llx) is not readable",
                   (unsigned long long)runtimeAddr,
                   (unsigned long long)preferred_va);
        return false;
    }

    /* Check for GPU carveout region */
    if (safe_memory_is_gpu_region(runtimeAddr)) {
        LOG_ENTITY_DEBUG("  Address 0x%llx (preferred VA 0x%llx) is in GPU region",
                   (unsigned long long)runtimeAddr,
                   (unsigned long long)preferred_va);
        return false;
    }

    /* Safely read the 4-byte type index
     * TypeId<T>::m_TypeIndex is typically a 32-bit integer */
    int32_t rawValue = -1;
    if (!safe_memory_read_i32(runtimeAddr, &rawValue)) {
        LOG_ENTITY_DEBUG("  Failed to safely read from 0x%llx (preferred VA 0x%llx)",
                   (unsigned long long)runtimeAddr,
                   (unsigned long long)preferred_va);
        return false;
    }

    /* Check for uninitialized (-1 or very large values indicate not yet registered) */
    if (rawValue < 0 || rawValue > 0xFFFF) {
        LOG_ENTITY_DEBUG("  Invalid TypeIndex value %d at 0x%llx (expected 0-65535)",
                   rawValue, (unsigned long long)runtimeAddr);
        return false;
    }

    *outIndex = (uint16_t)rawValue;
    LOG_ENTITY_DEBUG("  TypeIndex=%u at 0x%llx (preferred VA 0x%llx)",
               *outIndex, (unsigned long long)runtimeAddr,
               (unsigned long long)preferred_va);
    return true;
}

// ============================================================================
// Discovery
// ============================================================================

bool component_typeid_is_curated(const char *name) {
    if (!name) return false;
    for (int i = 0; g_KnownTypeIds[i].componentName != NULL; i++) {
        if (strcmp(g_KnownTypeIds[i].componentName, name) == 0) return true;
    }
    return false;
}

int component_typeid_discover(void) {
    if (!component_typeid_ready()) {
        LOG_ENTITY_DEBUG("ERROR: Not initialized, cannot discover");
        return 0;
    }
    if (!component_typeid_runtime_build_matches()) {
        const char *detected_build = version_detect_get_version();
        LOG_ENTITY_DEBUG("TypeId build gate closed: generated=%s detected=%s",
                         GENERATED_TYPEIDS_BUILD_ID,
                         detected_build ? detected_build : "unknown");
        return 0;
    }

    LOG_ENTITY_DEBUG("Discovering component type indices from TypeId globals...");

    int discovered = 0;

    for (int i = 0; g_KnownTypeIds[i].componentName != NULL; i++) {
        const TypeIdEntry *entry = &g_KnownTypeIds[i];

        uint64_t preferred_va = 0;
        if (!component_typeid_generated_lookup(entry->componentName,
                                               entry->context,
                                               &preferred_va)) {
            LOG_ENTITY_DEBUG("  %s: generated symbol authority missing",
                             entry->componentName);
            continue;
        }
        uint16_t typeIndex = 0;
        bool success = component_typeid_read(preferred_va, &typeIndex);

        if (success) {
            // One-frame components need the high storage-pool bit set.
            // This tells the storage lookup to search the OneFrameComponents pool
            uint16_t registeredIndex = typeIndex;
            if (entry->isOneFrame) {
                registeredIndex = typeIndex | 0x8000;  // Set one-frame bit
            }

            LOG_ENTITY_DEBUG("  %s: index=%u (registered=%u, from 0x%llx, oneFrame=%s)",
                       entry->componentName, typeIndex, registeredIndex,
                       (unsigned long long)preferred_va,
                       entry->isOneFrame ? "yes" : "no");

            // Update the component registry with the proper index (with 0x8000 for one-frame)
            bool registered = component_registry_register(
                entry->componentName,
                registeredIndex,
                entry->expectedSize,
                entry->isProxy
            );

            if (registered) {
                // Also update the property system so layouts can be looked up by TypeIndex
                component_property_set_type_index(entry->componentName, typeIndex);
                discovered++;
            }
        } else {
            LOG_ENTITY_DEBUG("  %s: FAILED to read generated VA 0x%llx",
                       entry->componentName,
                       (unsigned long long)preferred_va);
        }
    }

    LOG_ENTITY_DEBUG("Discovered %d component type indices from known list", discovered);

    // Also discover the remainder of the 2004 generated component surface.
    int generated = component_typeid_discover_all_generated();
    discovered += generated;

    LOG_ENTITY_DEBUG("Discovered %d total component type indices", discovered);
    return discovered;
}

// ============================================================================
// Debug
// ============================================================================

// Forward declaration for console output
extern void console_printf(const char *format, ...);

/**
 * Dump TypeId status to console.
 * Shows resolved/unresolved count and details for each component.
 */
void component_typeid_dump_to_console(void) {
    if (!component_typeid_ready()) {
        console_printf("TypeId system not initialized");
        return;
    }
    if (!component_typeid_runtime_build_matches()) {
        const char *detected_build = version_detect_get_version();
        console_printf("TypeId build gate closed (generated=%s, detected=%s)",
                       GENERATED_TYPEIDS_BUILD_ID,
                       detected_build ? detected_build : "unknown");
        return;
    }

    console_printf("Component TypeId Resolution Status:");
    console_printf("------------------------------------");

    int resolved = 0;
    int total = 0;

    for (int i = 0; g_KnownTypeIds[i].componentName != NULL; i++) {
        const TypeIdEntry *entry = &g_KnownTypeIds[i];
        total++;

        uint64_t preferred_va = 0;
        uint16_t type_index = 0;
        if (!component_typeid_generated_lookup(entry->componentName,
                                               entry->context,
                                               &preferred_va)) {
            console_printf("%-55s NO_GENERATED_SYMBOL", entry->componentName);
        } else if (component_typeid_read(preferred_va, &type_index)) {
            console_printf("%-55s RESOLVED (typeIndex=%u)",
                           entry->componentName, type_index);
            resolved++;
        } else {
            console_printf("%-55s UNRESOLVED (preferredVA=0x%llx)",
                           entry->componentName,
                           (unsigned long long)preferred_va);
        }
    }

    console_printf("------------------------------------");
    console_printf("Resolved: %d/%d (%d%%)", resolved, total, total > 0 ? (resolved * 100 / total) : 0);
}

void component_typeid_dump(void) {
    if (!component_typeid_ready()) {
        LOG_ENTITY_DEBUG("Not initialized");
        return;
    }
    if (!component_typeid_runtime_build_matches()) {
        const char *detected_build = version_detect_get_version();
        LOG_ENTITY_DEBUG("TypeId build gate closed (generated=%s, detected=%s)",
                         GENERATED_TYPEIDS_BUILD_ID,
                         detected_build ? detected_build : "unknown");
        return;
    }

    LOG_ENTITY_DEBUG("=== TypeId<T>::m_TypeIndex Dump ===");
    LOG_ENTITY_DEBUG("Binary base: %p", g_BinaryBase);

    for (int i = 0; g_KnownTypeIds[i].componentName != NULL; i++) {
        const TypeIdEntry *entry = &g_KnownTypeIds[i];

        LOG_ENTITY_DEBUG("  %s:", entry->componentName);
        uint64_t preferred_va = 0;
        if (!component_typeid_generated_lookup(entry->componentName,
                                               entry->context,
                                               &preferred_va)) {
            LOG_ENTITY_DEBUG("    => NO GENERATED SYMBOL");
            continue;
        }
        mach_vm_address_t runtime_address = 0;
        component_typeid_runtime_address(preferred_va, &runtime_address);
        LOG_ENTITY_DEBUG("    Preferred VA: 0x%llx",
                         (unsigned long long)preferred_va);
        LOG_ENTITY_DEBUG("    Runtime addr: 0x%llx",
                         (unsigned long long)runtime_address);

        uint16_t type_index = 0;
        if (component_typeid_read(preferred_va, &type_index)) {
            LOG_ENTITY_DEBUG("    => TypeIndex: %u", type_index);
        } else {
            LOG_ENTITY_DEBUG("    => UNRESOLVED");
        }
    }
}
