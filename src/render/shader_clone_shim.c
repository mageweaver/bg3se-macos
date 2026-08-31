/**
 * shader_clone_shim.c — alias clone-named shaders to their base shader.
 *
 * The Windows toolkit clones a base material per preset (hair packs, head
 * packs: `CHAR_Hair_<uuid>`), and the engine derives the 15 shader names from
 * the REGISTERED material name — so a clone requests
 * `CHAR_Hair_<uuid>_STI_DEF` from ls::ShaderManager. On Windows the clone's
 * DXBC files self-register; on macOS shaders are 170-byte descriptors into a
 * prebuilt Materials.metallib, the registry only knows base names, and
 * GetShader misses. The miss produces a pipeline cache entry whose compile
 * never completes, and rf::IAppStage::AddPipelineState waits on it in an
 * unbounded pthread_yield loop — the "select a modded hair and the game
 * freezes at 300% CPU" hang (sampled live, twice, 8s apart, identical stack).
 *
 * Fix at the source: hook GetShader; on a miss where the name embeds a UUID
 * segment, retry with the segment stripped. The clone's shaders ARE the base
 * shaders (byte-identical recompiles — verified against the Windows
 * Materials.pak), so aliasing is semantically exact. No pak edits needed.
 */

#include "shader_clone_shim.h"
#include "../core/logging.h"
#include "../strings/fixed_string.h"
#include <dobby.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ls::ShaderManager::GetShader(FixedString const&) — local symbol, exact-build
// address (4.1.1.7398727). Returns a 64-bit ShaderID; a miss returns the
// sentinel below (observed at the AppliedMaterial::LoadShaders call sites:
// `mov x8, #-0x40000000000000; cmp x0, x8`).
#define ADDR_GETSHADER          0x106134840ULL
#define SHADERID_INVALID        0xFFC0000000000000ULL

// First instruction of GetShader on the verified build, checked before
// patching so an unknown binary gets no hook (fail closed).
#define GETSHADER_PROLOGUE_0    0xD10303FFu   /* sub sp, sp, #0xc0 */

typedef uint64_t (*GetShaderFn)(void *mgr, const uint32_t *name_fs);
static GetShaderFn s_orig = NULL;
static bool s_installed = false;

/* Strip one `_<8-4-4-4-12 hex uuid>` segment from a shader name.
 * Returns true and writes the shortened name when a segment was removed. */
static bool strip_uuid_segment(const char *name, char *out, size_t out_size) {
    static const int groups[] = {8, 4, 4, 4, 12};
    size_t len = strlen(name);
    for (size_t i = 0; i + 37 <= len; i++) {
        if (name[i] != '_') continue;
        size_t p = i + 1;
        bool ok = true;
        for (int g = 0; g < 5 && ok; g++) {
            for (int k = 0; k < groups[g]; k++, p++) {
                char c = name[p];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                      || (c >= 'A' && c <= 'F'))) { ok = false; break; }
            }
            if (ok && g < 4) {
                if (name[p] != '-') { ok = false; }
                p++;
            }
        }
        if (!ok) continue;
        // Segment must end the name or be followed by another underscore part.
        if (name[p] != '\0' && name[p] != '_') continue;
        size_t base_len = i + (len - p);
        if (base_len + 1 > out_size) return false;
        memcpy(out, name, i);
        memcpy(out + i, name + p, len - p + 1);
        return true;
    }
    return false;
}

static uint64_t fake_GetShader(void *mgr, const uint32_t *name_fs) {
    uint64_t id = s_orig(mgr, name_fs);
    if (id != SHADERID_INVALID || !name_fs) {
        return id;
    }

    const char *name = fixed_string_resolve(*name_fs);
    if (!name) return id;

    char base[256];
    if (!strip_uuid_segment(name, base, sizeof(base))) {
        return id;   // not a clone-shaped name; a genuine miss stays a miss
    }

    uint32_t base_fs = fixed_string_intern(base, -1);
    if (base_fs == FS_NULL_INDEX) return id;

    uint64_t base_id = s_orig(mgr, &base_fs);
    if (base_id != SHADERID_INVALID) {
        LOG_CORE_INFO("ShaderCloneShim: '%s' -> '%s'", name, base);
        return base_id;
    }
    return id;
}

bool shader_clone_shim_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t addr = (uintptr_t)binary_base + (ADDR_GETSHADER - 0x100000000ULL);
    uint32_t first = *(const uint32_t *)addr;
    if (first != GETSHADER_PROLOGUE_0) {
        LOG_CORE_INFO("ShaderCloneShim: NOT installed — GetShader prologue "
                      "reads 0x%08x, expected 0x%08x (different build?)",
                      first, GETSHADER_PROLOGUE_0);
        return false;
    }

    if (DobbyHook((void *)addr, (void *)fake_GetShader,
                  (void **)&s_orig) != 0 || !s_orig) {
        LOG_CORE_INFO("ShaderCloneShim: DobbyHook failed");
        return false;
    }

    s_installed = true;
    LOG_CORE_INFO("ShaderCloneShim: clone-named shader lookups now fall back "
                  "to their base shader");
    return true;
}
