# Engine bug: NULL-tileset deref in ls::VirtualTextureManager::Unload

**Build:** 4.1.1.7398727 (macOS arm64) · **Status:** patched by the extender
(`src/game/vt_unload_guard.c`) since 2026-08-29 · **Kill switch:** `BG3SE_NO_VT_GUARD=1`

## Symptom

SIGSEGV at faulting address 0x1c on GameThread, top frames always:

    ls::VirtualTextureManager::Unload(ls::VirtualTextureLayerConfig::Type, ls::Path const&)  +0x625e108
    ls::UnregisterVirtualTextureTileSet(ls::ModuleShortDesc const&)
    ecs SystemUpdate<ecl::ModuleUnloadSystem>

Observed six times across 2026-08-28/29, on New Game / module transitions with
Deadlier Honour Mode (a 1,954-file Levels overhaul) in the order. **Reproduced
with the extender fully absent** (crash-report image list verified) — this is
the game's own code. Intermittent: the same order both loaded fine and crashed,
with and without the extender, before and after load-order fixes.

## Root cause (disassembly)

The tileset hash-table lookup has two not-found exits, and both fall into an
unconditional atomic refcount decrement on the (NULL) result:

    10625e044  cbz w20, 0x10625e0fc   ; table empty -> mov x20,#0 -> falls through
    10625e0a8  cbz x20, 0x10625e100   ; bucket chain miss, x20 already NULL
    10625e0fc  mov x20, #0x0
    10625e100  add x8, x20, #0x1c
    10625e108  ldaddal w9, w8, [x8]   ; <- SIGSEGV, addr 0x1c

Unloading a tileset that was never registered (or already unregistered — a
timing race, hence the intermittency) is therefore a guaranteed crash.

## Fix

Retarget the two not-found branches to 0x10625e26c — the function's own
skip-destruction continuation (reloads the lock pointer from [sp,#0x8], SRW
write-unlock, epilogue; never reads x20):

    0x10625e044: 0x340005D4 -> 0x34001154   (cbz w20, 0x10625e26c)
    0x10625e0a8: 0xB40002D4 -> 0xB4000E34   (cbz x20, 0x10625e26c)

Unload of an unregistered tileset becomes a no-op. The found path
(0x10625e0f8 -> 0x100) is untouched. Both original encodings are verified
before either write; any mismatch (other build) leaves the binary untouched.

## History of wrong diagnoses (kept deliberately)

1. "Too many mods / VT volume" — disproven; crash reproduced with 2 mods.
2. "Malformed load order (missing GustavDev)" — the order WAS broken and fixing
   it produced one clean run, but the crash returned with the correct order.
   One clean run is not evidence; the same lesson as bg3se-spellsystem-load-crash.
3. "Native engine bug" — correct, but only proven once the faulting basic block
   was read: the null path is structural, not data corruption.
