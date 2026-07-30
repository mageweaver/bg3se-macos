/**
 * audio_manager.c - Audio Manager for BG3SE-macOS
 *
 * Provides access to the game's WwiseManager (WWise sound engine)
 * for Ext.Audio API (sound playback, state control, RTPC parameters).
 *
 * Access chain:
 *   ResourceManager::m_ptr -> ResourceManager* -> SoundManager (+???)
 *     -> WwiseManager* -> VMT calls for audio control
 *
 * Note: SoundManager offset from ResourceManager needs runtime discovery.
 * The WwiseManager VMT indices are from Windows BG3SE pattern analysis.
 */

#include "audio_manager.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/offset_table.h"
#include "../strings/fixed_string.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ============================================================================
// Constants and Offsets
// ============================================================================

// ResourceManager::m_ptr — sourced from offset_table at runtime
// Fallback value kept for reference: 0x08a8f070 (4.1.1.6995620)
#define OFFSET_RESOURCEMANAGER_PTR_FALLBACK  0x08a8f070

// SoundManager offset within ResourceManager (needs runtime discovery)
// Placeholder - probe ResourceManager struct to find actual offset
#define RESOURCEMANAGER_SOUNDMANAGER_OFFSET  0x88  // TBD: needs runtime probe

// WwiseManager VMT indices (from Windows BG3SE analysis, need verification)
#define WWISE_VMT_POST_EVENT        5
#define WWISE_VMT_STOP              8
#define WWISE_VMT_SET_SWITCH       10
#define WWISE_VMT_SET_STATE        12
#define WWISE_VMT_SET_RTPC         14
#define WWISE_VMT_GET_RTPC         16
#define WWISE_VMT_RESET_RTPC       18
#define WWISE_VMT_PAUSE_ALL        20
#define WWISE_VMT_RESUME_ALL       22
#define WWISE_VMT_LOAD_EVENT       24
#define WWISE_VMT_UNLOAD_EVENT     26
// Additional VMT indices — TBD: verify via Ghidra for ARM64
#define WWISE_VMT_PLAY_EXTERNAL    28   /* PlayExternalSound */
#define WWISE_VMT_GET_ID_FROM_STR  30   /* GetIDFromString (SoundNameId) */

// Wwise AKRESULT value used by the game's ww::WwiseManager wrappers.
#define AK_SUCCESS 1

// Well-known sound object IDs
#define SOUND_OBJECT_INVALID  0ULL
#define SOUND_OBJECT_GLOBAL   1ULL

// ============================================================================
// ls::STDString Construction (verified via Ghidra — see STDSTRING_ABI.md)
// ============================================================================

// 16-byte libc++ layout with ls::StringAllocator. SSO threshold = 14 chars.
typedef struct {
    union {
        struct { char *data; uint32_t size; uint32_t cap_flag; } l;
        struct { char data[15]; uint8_t size_flag; } s;
    };
} LSSTDString;

// Game's ls::STDString(const char*) constructor.
// 4.1.1.7209685 vintage, nm-verified (0x10650e718 t ls::STDString::STDString(char const*)).
// Passed through offset_table_remap_fn() (either-vintage match, fail-closed).
#define OFFSET_STDSTRING_CTOR  0x0650e718ULL
typedef void (*STDStringCtorFn)(LSSTDString *this_, const char *str);

/*
 * Exported Wwise SoundEngine entry points used by the installed macOS game.
 *
 * These signatures and argument constants are mirrored from disassembly of
 * ww::WwiseManager::{Load,Unload,Prepare,Unprepare}Bank_Blocking in build
 * 4.1.1.7209685.  Resolving the exported AK symbols avoids the unverified
 * ResourceManager -> SoundManager receiver offset and guessed VMT indices.
 */
typedef uint32_t (*AkGetIdFromStringFn)(const char *name);
typedef int32_t (*AkLoadBankFn)(const char *name, uint32_t *out_bank_id,
                                uint32_t bank_type);
typedef int32_t (*AkUnloadBankFn)(uint32_t bank_id, const void *memory,
                                  uint32_t memory_size);
typedef int32_t (*AkPrepareBankByNameFn)(uint32_t preparation_type,
                                         const char *name,
                                         uint32_t bank_content,
                                         uint32_t flags);
typedef int32_t (*AkPrepareBankByIdFn)(uint32_t preparation_type,
                                       uint32_t bank_id,
                                       uint32_t bank_content,
                                       uint32_t flags);

static void ls_stdstring_init(LSSTDString *out, const char *str, STDStringCtorFn ctor) {
    size_t len = str ? strlen(str) : 0;
    memset(out, 0, sizeof(*out));
    if (len <= 14) {
        // SSO path — no allocation, entirely inline
        if (len > 0) memcpy(out->s.data, str, len);
        out->s.data[len] = '\0';
        out->s.size_flag = (uint8_t)len;  // bit 7 clear = short mode
    } else if (ctor) {
        // Long path — use game's constructor for MemoryManager-compatible allocation
        ctor(out, str);
    } else {
        // Fallback: truncate to SSO (14 chars). Lossy but safe.
        memcpy(out->s.data, str, 14);
        out->s.data[14] = '\0';
        out->s.size_flag = 14;
        log_message("[Audio] WARNING: STDString ctor not resolved, truncating path to 14 chars");
    }
}

// ============================================================================
// Module State
// ============================================================================

static struct {
    bool initialized;
    void *main_binary_base;
    void **resource_manager_ptr;
    STDStringCtorFn stdstring_ctor;
    AkGetIdFromStringFn ak_get_id_from_string;
    AkLoadBankFn ak_load_bank;
    AkUnloadBankFn ak_unload_bank;
    AkPrepareBankByNameFn ak_prepare_bank_by_name;
    AkPrepareBankByIdFn ak_prepare_bank_by_id;
} g_audio = {0};

// ============================================================================
// Initialization
// ============================================================================

bool audio_manager_init(void *main_binary_base) {
    if (g_audio.initialized) {
        return true;
    }

    if (!main_binary_base) {
        log_message("[Audio] ERROR: main_binary_base is NULL");
        return false;
    }

    g_audio.main_binary_base = main_binary_base;

    const VersionOffsets *off = offset_table_get();
    if (off && off->resource_mgr_ptr) {
        g_audio.resource_manager_ptr = (void **)offset_table_resolve(off->resource_mgr_ptr);
    } else {
        g_audio.resource_manager_ptr = (void **)((uintptr_t)main_binary_base + OFFSET_RESOURCEMANAGER_PTR_FALLBACK);
    }

    // Resolve ls::STDString constructor (remap the hardcoded 6995620 address).
    // NULL if no verified address -> PlayExternalSound's string path is skipped.
    uint64_t stdstr_ghidra = offset_table_remap_fn(0x100000000ULL + OFFSET_STDSTRING_CTOR);
    g_audio.stdstring_ctor = stdstr_ghidra
        ? (STDStringCtorFn)((uintptr_t)main_binary_base + (stdstr_ghidra - 0x100000000ULL))
        : NULL;

    g_audio.ak_get_id_from_string = (AkGetIdFromStringFn)dlsym(
        RTLD_DEFAULT, "_ZN2AK11SoundEngine15GetIDFromStringEPKc");
    g_audio.ak_load_bank = (AkLoadBankFn)dlsym(
        RTLD_DEFAULT, "_ZN2AK11SoundEngine8LoadBankEPKcRjj");
    g_audio.ak_unload_bank = (AkUnloadBankFn)dlsym(
        RTLD_DEFAULT, "_ZN2AK11SoundEngine10UnloadBankEjPKvj");
    g_audio.ak_prepare_bank_by_name = (AkPrepareBankByNameFn)dlsym(
        RTLD_DEFAULT,
        "_ZN2AK11SoundEngine11PrepareBankENS0_15PreparationTypeEPKcNS0_13AkBankContentEj");
    g_audio.ak_prepare_bank_by_id = (AkPrepareBankByIdFn)dlsym(
        RTLD_DEFAULT,
        "_ZN2AK11SoundEngine11PrepareBankENS0_15PreparationTypeEjNS0_13AkBankContentEj");

    log_message("[Audio] Audio manager initialized");
    log_message("[Audio]   Base: %p", main_binary_base);
    log_message("[Audio]   ResourceManager::m_ptr -> %p", (void *)g_audio.resource_manager_ptr);
    log_message("[Audio]   STDString ctor: %p", (void *)g_audio.stdstring_ctor);
    log_message("[Audio]   Wwise bank API: getId=%p load=%p unload=%p prepareName=%p prepareId=%p",
                (void *)g_audio.ak_get_id_from_string,
                (void *)g_audio.ak_load_bank,
                (void *)g_audio.ak_unload_bank,
                (void *)g_audio.ak_prepare_bank_by_name,
                (void *)g_audio.ak_prepare_bank_by_id);

    g_audio.initialized = true;
    return true;
}

bool audio_manager_ready(void) {
    if (!g_audio.initialized || !g_audio.resource_manager_ptr) {
        return false;
    }

    void *rm = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_audio.resource_manager_ptr, &rm)) {
        return false;
    }

    return rm != NULL;
}

// ============================================================================
// Internal Helpers
// ============================================================================

static void *get_resource_manager(void) {
    if (!g_audio.initialized || !g_audio.resource_manager_ptr) {
        return NULL;
    }

    void *rm = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_audio.resource_manager_ptr, &rm)) {
        return NULL;
    }

    return rm;
}

static void *get_sound_manager(void) {
    void *rm = get_resource_manager();
    if (!rm) return NULL;

    void *sm = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)rm + RESOURCEMANAGER_SOUNDMANAGER_OFFSET, &sm)) {
        return NULL;
    }

    return sm;
}

/**
 * Read a function pointer from a VMT at a given index.
 */
static void *read_vmt_entry(void *object, int index) {
    if (!object) return NULL;

    void *vmt = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)object, &vmt)) {
        return NULL;
    }

    void *func = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)vmt + (index * sizeof(void *)), &func)) {
        return NULL;
    }

    return func;
}

// ============================================================================
// Sound Object ID Resolution
// ============================================================================

uint64_t audio_resolve_sound_object(const char *name) {
    if (!name) return SOUND_OBJECT_INVALID;

    // Well-known sound objects
    if (strcasecmp(name, "Global") == 0 || strcasecmp(name, "Music") == 0) {
        return SOUND_OBJECT_GLOBAL;
    }

    // Listener objects (Listener0-Listener3)
    if (strncasecmp(name, "Listener", 8) == 0 && name[8] >= '0' && name[8] <= '3') {
        return 100ULL + (name[8] - '0');
    }

    // Ambient objects (Ambient0-Ambient3)
    if (strncasecmp(name, "Ambient", 7) == 0 && name[7] >= '0' && name[7] <= '3') {
        return 200ULL + (name[7] - '0');
    }

    // Numeric ID passed as string
    char *endptr = NULL;
    unsigned long long val = strtoull(name, &endptr, 0);
    if (endptr && *endptr == '\0' && val > 0) {
        return (uint64_t)val;
    }

    log_message("[Audio] Unknown sound object: %s", name);
    return SOUND_OBJECT_INVALID;
}

// ============================================================================
// Playback Control
// ============================================================================

typedef void (*WwisePostEventFn)(void *this_, uint64_t sound_object, const char *event_name);
typedef void (*WwiseStopFn)(void *this_, uint64_t sound_object);
typedef void (*WwisePauseAllFn)(void *this_);
typedef void (*WwiseResumeAllFn)(void *this_);

bool audio_post_event(uint64_t sound_object_id, const char *event_name) {
    if (!event_name) return false;

    void *sm = get_sound_manager();
    if (!sm) {
        log_message("[Audio] SoundManager not available");
        return false;
    }

    void *func = read_vmt_entry(sm, WWISE_VMT_POST_EVENT);
    if (!func) {
        log_message("[Audio] PostEvent VMT entry not found");
        return false;
    }

    WwisePostEventFn post = (WwisePostEventFn)func;
    post(sm, sound_object_id, event_name);
    return true;
}

bool audio_stop(uint64_t sound_object_id) {
    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_STOP);
    if (!func) return false;

    WwiseStopFn stop = (WwiseStopFn)func;
    stop(sm, sound_object_id);
    return true;
}

bool audio_pause_all(void) {
    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_PAUSE_ALL);
    if (!func) return false;

    WwisePauseAllFn pause = (WwisePauseAllFn)func;
    pause(sm);
    return true;
}

bool audio_resume_all(void) {
    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_RESUME_ALL);
    if (!func) return false;

    WwiseResumeAllFn resume = (WwiseResumeAllFn)func;
    resume(sm);
    return true;
}

// ============================================================================
// State/Switch Control
// ============================================================================

typedef void (*WwiseSetSwitchFn)(void *this_, uint64_t sound_object,
                                  const char *switch_group, const char *state);
typedef void (*WwiseSetStateFn)(void *this_, const char *state_group, const char *state);

bool audio_set_switch(uint64_t sound_object_id, const char *switch_group, const char *state) {
    if (!switch_group || !state) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_SET_SWITCH);
    if (!func) return false;

    WwiseSetSwitchFn set_switch = (WwiseSetSwitchFn)func;
    set_switch(sm, sound_object_id, switch_group, state);
    return true;
}

bool audio_set_state(const char *state_group, const char *state) {
    if (!state_group || !state) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_SET_STATE);
    if (!func) return false;

    WwiseSetStateFn set_state = (WwiseSetStateFn)func;
    set_state(sm, state_group, state);
    return true;
}

// ============================================================================
// RTPC (Real-Time Parameter Control)
// ============================================================================

typedef void (*WwiseSetRtpcFn)(void *this_, uint64_t sound_object,
                                const char *name, float value);
typedef float (*WwiseGetRtpcFn)(void *this_, uint64_t sound_object, const char *name);
typedef void (*WwiseResetRtpcFn)(void *this_, uint64_t sound_object, const char *name);

bool audio_set_rtpc(uint64_t sound_object_id, const char *name, float value) {
    if (!name) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_SET_RTPC);
    if (!func) return false;

    WwiseSetRtpcFn set = (WwiseSetRtpcFn)func;
    set(sm, sound_object_id, name, value);
    return true;
}

float audio_get_rtpc(uint64_t sound_object_id, const char *name) {
    if (!name) return 0.0f;

    void *sm = get_sound_manager();
    if (!sm) return 0.0f;

    void *func = read_vmt_entry(sm, WWISE_VMT_GET_RTPC);
    if (!func) return 0.0f;

    WwiseGetRtpcFn get = (WwiseGetRtpcFn)func;
    return get(sm, sound_object_id, name);
}

bool audio_reset_rtpc(uint64_t sound_object_id, const char *name) {
    if (!name) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_RESET_RTPC);
    if (!func) return false;

    WwiseResetRtpcFn reset = (WwiseResetRtpcFn)func;
    reset(sm, sound_object_id, name);
    return true;
}

// ============================================================================
// Event/Bank Management
// ============================================================================

typedef void (*WwiseLoadEventFn)(void *this_, const char *event_name);
typedef void (*WwiseUnloadEventFn)(void *this_, const char *event_name);

bool audio_load_event(const char *event_name) {
    if (!event_name) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_LOAD_EVENT);
    if (!func) return false;

    WwiseLoadEventFn load = (WwiseLoadEventFn)func;
    load(sm, event_name);
    return true;
}

bool audio_unload_event(const char *event_name) {
    if (!event_name) return false;

    void *sm = get_sound_manager();
    if (!sm) return false;

    void *func = read_vmt_entry(sm, WWISE_VMT_UNLOAD_EVENT);
    if (!func) return false;

    WwiseUnloadEventFn unload = (WwiseUnloadEventFn)func;
    unload(sm, event_name);
    return true;
}

// ============================================================================
// Extended Bank/External Sound Management
// ============================================================================

/**
 * GetIDFromString — get a SoundNameId (uint32_t) from an event/bank name string.
 * Returns UINT32_MAX on failure (no valid ID found).
 *
 * On Windows: SoundNameId is a 4-byte integer returned by WwiseManager::GetIDFromString.
 * The function has simple 4-byte return so no x8 buffer needed.
 */
typedef uint32_t (*WwiseGetIdFromStringFn)(void *this_, const char *name);

static uint32_t get_sound_name_id(void *sm, const char *name) {
    void *func = read_vmt_entry(sm, WWISE_VMT_GET_ID_FROM_STR);
    if (!func) return UINT32_MAX;
    WwiseGetIdFromStringFn get_id = (WwiseGetIdFromStringFn)func;
    return get_id(sm, name);
}

/**
 * PlayExternalSound — play a sound from a file path via a Wwise event.
 *
 * Windows signature:
 *   bool PlayExternalSound(SoundObjectId obj, SoundNameId eventId,
 *                          STDString& path, uint8_t codec,
 *                          float positionSec, bool loop, void* callback)
 *
 * STDString ABI verified via Ghidra (see STDSTRING_ABI.md):
 *   16 bytes, SSO threshold = 14 chars, constructor at 0x10651fb60.
 *   We construct a proper LSSTDString on the stack and pass its address.
 */
typedef bool (*WwisePlayExternalFn)(void *this_,
                                     uint64_t sound_object,
                                     uint32_t event_id,
                                     LSSTDString *path,
                                     uint8_t codec,
                                     float position_sec,
                                     bool loop,
                                     void *callback);

bool audio_play_external_sound(uint64_t sound_object_id, const char *event_name,
                                const char *file_path, uint8_t codec,
                                float position_sec) {
    if (!event_name || !file_path) return false;

    void *sm = get_sound_manager();
    if (!sm) {
        log_message("[Audio] SoundManager not available for PlayExternalSound");
        return false;
    }

    uint32_t event_id = get_sound_name_id(sm, event_name);
    if (event_id == UINT32_MAX) {
        log_message("[Audio] PlayExternalSound: could not resolve event ID for '%s'", event_name);
        return false;
    }

    void *func = read_vmt_entry(sm, WWISE_VMT_PLAY_EXTERNAL);
    if (!func) {
        log_message("[Audio] PlayExternalSound VMT entry not found at index %d", WWISE_VMT_PLAY_EXTERNAL);
        return false;
    }

    // Construct LSSTDString on stack (SSO for <=14 chars, game ctor for longer)
    LSSTDString path_str;
    ls_stdstring_init(&path_str, file_path, g_audio.stdstring_ctor);

    WwisePlayExternalFn play = (WwisePlayExternalFn)func;
    bool result = play(sm, sound_object_id, event_id, &path_str, codec, position_sec, false, NULL);

    log_message("[Audio] PlayExternalSound('%s', path='%s', codec=%u, pos=%.2f) -> %s",
                event_name, file_path, codec, position_sec, result ? "OK" : "FAIL");
    return result;
}

static bool bank_api_ready(void) {
    return g_audio.ak_get_id_from_string
        && g_audio.ak_load_bank
        && g_audio.ak_unload_bank
        && g_audio.ak_prepare_bank_by_name
        && g_audio.ak_prepare_bank_by_id;
}

static bool bank_id_valid(uint32_t bank_id) {
    return bank_id != 0 && bank_id != UINT32_MAX;
}

static bool require_bank_api(const char *operation) {
    static bool warned = false;
    if (bank_api_ready()) {
        return true;
    }

    if (!warned) {
        log_message("[Audio] %s deferred: exported Wwise bank entry points are unavailable",
                    operation);
        warned = true;
    }
    return false;
}

bool audio_load_bank(const char *bank_name) {
    if (!bank_name || !*bank_name || !require_bank_api("LoadBank")) return false;

    uint32_t bank_id = UINT32_MAX;
    int32_t result;

    if (strcmp(bank_name, "Init") == 0) {
        result = g_audio.ak_load_bank(bank_name, &bank_id, 0);
    } else {
        result = g_audio.ak_prepare_bank_by_name(0, bank_name, 1, 0);
        if (result == AK_SUCCESS) {
            bank_id = g_audio.ak_get_id_from_string(bank_name);
        }
    }

    bool success = result == AK_SUCCESS && bank_id_valid(bank_id);
    log_message("[Audio] LoadBank('%s') -> %s (AKRESULT=%d, id=%u)",
                bank_name, success ? "OK" : "FAIL", result, bank_id);
    return success;
}

/**
 * UnloadBank — unload a Wwise sound bank.
 *
 * Windows: UnloadBank(SoundNameId bankId)
 */
bool audio_unload_bank(const char *bank_name) {
    if (!bank_name || !*bank_name || !require_bank_api("UnloadBank")) return false;

    uint32_t bank_id = g_audio.ak_get_id_from_string(bank_name);
    if (!bank_id_valid(bank_id)) {
        log_message("[Audio] UnloadBank: could not resolve bank ID for '%s'", bank_name);
        return false;
    }

    int32_t result = strcmp(bank_name, "Init") == 0
        ? g_audio.ak_unload_bank(bank_id, NULL, 0)
        : g_audio.ak_prepare_bank_by_id(1, bank_id, 1, 0);
    bool success = result == AK_SUCCESS;
    log_message("[Audio] UnloadBank('%s') -> %s (AKRESULT=%d, id=%u)",
                bank_name, success ? "OK" : "FAIL", result, bank_id);
    return success;
}

/**
 * PrepareBank — pre-load bank metadata for streaming.
 * Same signature as LoadBank: callee sets bankId.
 */
bool audio_prepare_bank(const char *bank_name) {
    if (!bank_name || !*bank_name || !require_bank_api("PrepareBank")) return false;

    int32_t result = g_audio.ak_prepare_bank_by_name(0, bank_name, 0, 0);
    uint32_t bank_id = result == AK_SUCCESS
        ? g_audio.ak_get_id_from_string(bank_name)
        : UINT32_MAX;
    bool success = result == AK_SUCCESS && bank_id_valid(bank_id);
    log_message("[Audio] PrepareBank('%s') -> %s (AKRESULT=%d, id=%u)",
                bank_name, success ? "OK" : "FAIL", result, bank_id);
    return success;
}

/**
 * UnprepareBank — release prepared bank metadata.
 * Windows BG3SE's Lua wrapper resolves the name and calls UnloadBank rather
 * than SoundManager::UnprepareBank. Preserve that observable behavior here.
 */
bool audio_unprepare_bank(const char *bank_name) {
    return audio_unload_bank(bank_name);
}
