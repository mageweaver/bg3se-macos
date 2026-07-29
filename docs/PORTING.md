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

# 3. Paste the generated struct entry into g_offset_table[]. The remap
#    table g_fn_remap is a fixed two-column {6995620, 7209685} table: a
#    third version needs a new column (and a matching branch in
#    offset_table_remap_fn), not new rows. Then:
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
`✓ all N fields + M remap entries match`. That's the proof the automation
reproduces hand-done work before you trust it on a new version.

## Reading the output / fixing flags

The resolver classifies every item:

- `[INFO] component_data_shift ...` — the signed delta from the 7209685-vintage
  compiled-in TypeId constants to your version, computed from the
  `data_shift_anchor`. It is applied ONLY to the TypeId-global family (proven
  uniform); every data singleton resolves by its own symbol. Sanity-check it
  looks like a small, plausible delta.
- `[WARN] ... MANUAL — anonymous slot` — a global with no symbol at all (e.g.
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
- `[ERROR] constant ... CHANGED` — an address we assumed was stable moved. Move
  that entry from `constant_functions` to `remap_functions`.
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

- A called game function → `remap_functions` (or `offset_table_functions` if it
  has a dedicated `VersionOffsets` field). Give the exact demangled `symbol` and
  its current-version `baseline` address.
- A singleton pointer global → `data_singletons` with `method: "symbol"` and its
  demangled `symbol` (plain `nm` resolves local symbols too). Use
  `method: "got"` for `__DATA_CONST,__got` slots and `method: "disasm"` (with a
  `note` describing the derivation) only when there is truly no symbol.
- A field offset inside an object → `struct_offsets`.

Find the exact symbol string with:

```bash
nm "$BG3_BINARY" | c++filt | grep "YourFunctionName"
```

Use whatever `c++filt` prints, verbatim, as the manifest `symbol` (whitespace is
normalized during matching).

## How the remap table works (background)

Several subsystems hold a hardcoded game-function address (mixed vintages:
some 6995620, some 7209685) and call it as a function pointer.
`offset_table_remap_fn(addr)` (in `offset_table.c`) matches the address against
EITHER column of the two-column `g_fn_remap` table and returns the running
version's column. It is fail-closed: an unknown version, or an address not in
the table, returns **0 — the caller skips that feature instead of jumping to a
stale address and crashing.** So even an un-ported function degrades gracefully.
The tool generates the column values for you; entries flagged `abi_changed` in
the manifest emit 0 until a matching wrapper exists.
