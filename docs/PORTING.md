# Porting BG3SE-macOS to a new game version

When Larian patches Baldur's Gate 3, the macOS Script Extender breaks because the
game's functions and global variables move to new addresses. This guide + the
`tools/port_offsets.py` tool turn that re-port from a multi-hour reverse‑engineering
session into, usually, **run a script and paste the output**.

## Why this is easy on macOS (and hard on Windows)

The macOS BG3 binary **ships with its symbol table** — ~725,000 named symbols.
So almost every address the extender needs can be found by name with `nm`, no
Ghidra required. (Norbyte's Windows extender needs heavy pattern‑scanning because
the Windows binary is stripped.)

There are two kinds of address, and only one kind changes per patch:

| Kind | Example | Changes per patch? | How to find |
|------|---------|--------------------|-------------|
| **Absolute address** | `SpellPrototype::Init` entry point; `RPGStats::m_ptr` slot | **Yes** | `nm` by symbol (plain `nm` sees local symbols too); `otool -Iv` for `__got` slots; disassembly scan for the rare anonymous slot |
| **Struct field offset** | EocServer → EntityWorld is at `+0x288` | No (until a class is restructured) | One‑time Ghidra/runtime; carried forward |

`tools/offset_manifest.json` lists every address in both categories with its
symbol and resolution method. It is the source of truth — the recipe.

## TL;DR — re-port to your installed version

```bash
# 1. See what resolves and what (if anything) needs attention:
python3 tools/port_offsets.py resolve

# 2. Print copy-pasteable C for src/core/offset_table.c:
python3 tools/port_offsets.py resolve --emit

# 3. Paste the generated VersionOffsets entry into g_offset_table[]. Each row
#    contains a GameFunctionId-indexed address array, so no schema change is
#    needed when a fourth or later game version is added. Then:
cd build && cmake --build .

# 4. Launch, load a save, and run the regression suite:
#    !test        (in the SE console)  -> expect 109/109
#    !test_ingame
```

The tool auto-detects your version from the app's `Info.plist`. Override with
`--binary PATH` or `--version X.Y.Z` if needed.

## Verifying the tool against a known-good version

```bash
python3 tools/port_offsets.py verify
```

This resolves every address against your binary and **diffs against the values
already in `offset_table.c`**. On the version the table was built for it prints
`✓ all N fields + M game functions match`. That's the proof the automation
reproduces hand-done work before you trust it on a new version.

## Reading the output / fixing flags

The resolver classifies every item:

- `[INFO] component_data_shift validated ...` — every shared TypeId anchor has
  one common signed delta from the 7209685-vintage constants.
- `[WARN] component_data_shift REJECTED ...` — shared TypeId anchors moved by
  more than one delta. The emitter writes `component_data_shift = 0` and
  `component_data_shift_valid = false`; migrate TypeIds independently by exact
  symbol. Never choose the most common delta.
- `[WARN] ... EXPECTED-MANUAL — anonymous slot` — a global with no symbol (e.g.
  `global_switches_ptr`, an anonymous `__common` slot). It does NOT follow the
  uniform shift (it moved -0x24000 between 6995620 and 7209685 while its
  neighbors moved +0x8000): re-derive it by disassembly (ADRP+LDR reference
  scan; it is the hot slot written by `App::CreateGlobalSwitches`).
- `[WARN] ... ABI CHANGED` — the symbol resolved, but its signature no longer
  matches the C wrapper that calls it (e.g. the 7209685 Interrupt
  `ExecuteStatsFunctors` gained a leading `ecs::EntityWorld&`). The tool emits
  0 for it; write a new wrapper before enabling. **Address validity is not ABI
  validity — always diff the demangled signature against the wrapper.**
- `[WARN] ... AMBIGUOUS` — the symbol matched more than one address (e.g. const
  vs non-const overload). The tool takes the lowest; **make the manifest `symbol`
  more specific** (full signature) so it's unique, then re-run.
- `[ERROR] ... symbol not found` — the function was renamed/inlined, or the
  signature drifted. Open the binary's symbols (`nm BINARY | c++filt | grep Name`)
  and update the manifest `symbol`. If it's genuinely gone, that feature needs
  rework.
- `[ERROR] game function ... claimed address ... MOVED` — a version claim does
  not match the exact symbol. Update that version's address; do not preserve a
  cross-version “constant” assumption. BinkManager::LoadVideo is an example: it
  stayed at `0x10390b6cc` through 7209685 and moved to `0x103916380` in 7398727.
- `[WARN] exported_data ... shift != __DATA shift` — that global lives in a
  segment that shifted differently. Resolve it by its own symbol (it already is)
  and don't rely on the uniform shift for it.

Anything not flagged resolved cleanly.

## When you DO need Ghidra (struct offsets)

`struct_offsets` in the manifest are field offsets *inside* objects (e.g.
`EntityWorld -> StorageContainer` at `+0x2d0`). They're stable across minor
patches and the tool just carries them. They only change if Larian restructures
a class — symptoms are a crash *inside* a game function after the entry address
is already correct, or a structural read returning garbage. To re-find one:

- Each entry has a `verify_via` / `where` pointing at how it was originally found
  (often a one-line disassembly check, e.g. `EocServer::StartUp` does
  `ldr xN,[this,#0x288]`). `otool -tV` the named function and read the offset.
- Or probe at runtime with `Ext.Debug.ProbeStruct` / `ReadPtr` against a live
  object (see `agent_docs/development.md`).

Update the `value` in the manifest and the matching `#define` in the code.

## Adding a brand-new address to the recipe

If you wire a new game function/global into the extender, add it to the manifest
so future ports resolve it automatically:

- A called game function → `game_functions`. Add a stable `GAME_FN_*` ID, the
  exact demangled signature, and the audited preferred VA under `addresses` for
  the new version. Callers use `offset_table_game_fn(id)` and never pass a raw
  address.
- A function already represented by a dedicated `VersionOffsets.fn_*` field →
  `offset_table_functions`, with its exact demangled signature.
- A singleton pointer global → `data_singletons` with `method: "symbol"` and its
  demangled `symbol` (plain `nm` resolves local symbols too). Use
  `method: "got"` for `__DATA_CONST,__got` slots and `method: "disasm"` (with a
  `note` describing the derivation) only when there is truly no symbol.
- A field offset inside an object → `struct_offsets`.

Find the exact symbol string with:

```bash
nm "$BG3_BINARY" | c++filt | grep "YourFunctionName"
```

Use whatever `c++filt` prints, verbatim, as the manifest `symbol`. Whitespace is
normalized, but matching is otherwise exact; name substrings and overload guesses
are not accepted.

## How the typed function table works

`GameFunctionId` is a stable, typed identifier shared by callers and the
manifest. Every `VersionOffsets` row contains `game_functions[GAME_FN_COUNT]`,
whose values are offsets from the preferred image base. The caller asks
`offset_table_game_fn(GAME_FN_...)` for a runtime callable pointer.

Lookup is fail-closed: an unknown version, out-of-range ID, missing binary base,
or zero/unverified entry returns `NULL`, so the caller disables that feature
instead of jumping to a stale address. Symbol resolution proves the entry point,
not its ABI; independent subsystem gates such as
`FUNCTOR_ADDRS_VERIFIED_BUILD` remain mandatory.

For build 4.1.1.7398727, run against the preserved binary explicitly:

```bash
python3 tools/port_offsets.py verify \
  --binary "build/migration-binaries/4.1.1.7398727/Baldur's Gate 3" \
  --version 4.1.1.7398727
```

Until Wave 2A supplies disassembly-derived values, the 7398727 row deliberately
keeps `global_switches_ptr` and `osiris_interface_ptr` at zero. The audit reports
both by field name as expected-manual xfails.
