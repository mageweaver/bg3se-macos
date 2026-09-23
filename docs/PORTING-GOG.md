# Porting BG3SE-macOS to a GOG build

Companion to [PORTING.md](PORTING.md), which covers re-porting to a new game
version. Read that first — the mechanics are the same and are not repeated here.

## Why GOG needs its own addresses

Steam and GOG ship the same game version as different binaries. Both report
`CFBundleShortVersionString = 4.1.1.7398727`, and every address differs:

| symbol | Steam | GOG | delta |
|---|---|---|---|
| `esv::EocServer::m_ptr` | `0x1089c6f58` | `0x1089bf6d8` | −0x7880 |
| `ls::GlobalTemplateManager::m_ptr` | `0x108ac0d98` | `0x108ab94e8` | −0x78b0 |
| `GlobalTemplateManager::GetTemplateRaw` | `0x105f9cda4` | `0x105f94654` | −0x8750 |
| `esv::GameStateMachine::Update` | `0x104a20ad4` | `0x104a189f4` | −0x80e0 |

Four deltas, so no single shift applies; every address is resolved
independently. The existing tooling already does that, which is why GOG support
needed no new reverse engineering.

About 4,700 of those addresses are generated constants in
`generated_typeids.h` and friends. Both stores' tables are compiled into one
dylib and selected at runtime, so there is one build and one release artifact.
See [One dylib, both stores](#one-dylib-both-stores) below.

## GOG bundle layout

```
Baldur's Gate 3.app/Contents/MacOS/
├── Baldur's Gate 3        ← 200 KB arch-selector stub
└── Baldur's Gate 3 GOG    ← 501 MB game — point every tool at this
```

The stub picks a CPU slice and hands off with `posix_spawn` +
`POSIX_SPAWN_SETEXEC`: exec without fork, so PID and environment survive into
the game. `nm` on the stub returns nothing useful, and injecting into it makes
the extender claim the PID and then suppress itself as a duplicate image when
the game loads. The extender now skips the stub, and `scripts/bg3g.sh` execs the
game binary directly.

Anything keyed on `CFBundleShortVersionString` alone will match the wrong
store's addresses. Non-Steam rows in `offset_table.c` are keyed
`<version>-<store>`, and `src/gen/build_identity.c` carries each binary's
`LC_UUID` so an unrecognised game disables addresses instead of corrupting
memory.

## One dylib, both stores

Every store's generated tables are compiled in, and the dylib picks between
them at runtime from the store detected in the loaded image's path — the same
source `offset_table.c` already used to pick its row. A build is therefore
never tied to one store, and cannot be pointed at the wrong game.

| Layer | Selected by |
|---|---|
| `offset_table.c` rows | store, at runtime (pre-existing) |
| Build identity and `LC_UUID` | `src/gen/build_identity.c`, lookup by store |
| Component TypeId tables | `src/gen/generated_registry.c`, dispatched |
| `g_system_names`, `k_replicated_type_globals` | `src/gen/store_tables.c`, dispatched |

The component tables needed no change to `tools/extract_typeids.py`. Each
store's generated file is compiled with its own `src/gen/<store>` on the include
path and its public functions renamed, using per-source `COMPILE_OPTIONS`.

Adding a store means adding a `src/gen/<store>/` directory and naming it in
`BG3_STORES`. CMake fails the configure if a named store has no generated
tables, so a half-added store cannot build.

The cost is about 186KB of dylib for the second component table.

An unrecognised store, or a binary whose `LC_UUID` is not in the table,
disables addresses rather than guessing. That is the same fail-closed path a
version mismatch already took.

## Re-porting to a new GOG build

```bash
BIN="$HOME/Applications/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3 GOG"
VER="4.1.1.NNNNNNN-gog"        # installed CFBundleShortVersionString + "-gog"

# 1. The two anonymous slots. They have no symbol, and step 2 needs them.
PYTHONDONTWRITEBYTECODE=1 python3 scripts/re/migrate_anonymous_globals.py \
    --old "$BIN" --new "$BIN" \
    --target global_switches_ptr --target osiris_interface_ptr
#    Both targets must report status: RESOLVED_UNIQUE. Take the two values from
#    the SUMMARY. SELF_TEST=FAIL is expected: the self-test compares --old
#    against the 4.1.1.7209685 values, and you passed the new binary as --old.

# 2. Record this build's address claims so the audits have something to check.
python3 tools/port_offsets.py record --binary "$BIN" --version "$VER" \
    --set global_switches_ptr=0x... --set osiris_interface_ptr=0x...
#    Paste the step 1 values as-is: they are full VAs, the manifest stores
#    offsets, and record converts.

# 3. Generate the offset_table.c row and paste it into g_offset_table[].
#    Fill the two anonymous slots in by hand; the resolver emits 0 for them.
python3 tools/port_offsets.py resolve --binary "$BIN" --version "$VER" --emit

# 4. Regenerate this store's address tables.
python3 tools/extract_typeids.py "$BIN" --build-id "$VER" --registry \
    --header-out   src/gen/gog/generated_typeids.h \
    --registry-out src/gen/gog/generated_component_registry.c
python3 tools/generate_remove_component.py --binary "$BIN" --build-id "$VER" \
    --out src/gen/gog/generated_remove_component.h

# 5. Update the build identity so the runtime guard matches this binary.
dwarfdump --uuid "$BIN" | grep arm64      # → this store's arm64 LC_UUID
$EDITOR tools/gen_build_identity.py       # edit SUPPORTED_BUILDS, then:
python3 tools/gen_build_identity.py       # regenerates src/gen/build_identity.*
#    --check reports drift without writing, for CI and the audits.

# 6. Build and check. One build serves every store.
cmake -B build && cmake --build build
python3 tools/port_offsets.py verify --binary "$BIN" --version "$VER"
PYTHONPATH=tools pytest tests/harness/ -q
./build/bin/bg3se_test_tier0
```

`verify` must end with `✓ all N fields + M game functions match`.

Skipping step 2 leaves the two anonymous slots reported as `CARRIED` rather than
compared: `verify` can only check a field the manifest has a claim for, and
those two have no symbol to resolve. `CARRIED` means "not derivable here, so the
table is trusted" — not an error, but getting them compared is the point of
step 2.

## Contributing a GOG port back

The maintainer may not own a GOG copy, so a PR has to carry its own evidence:

1. Full `port_offsets.py verify` output from step 6.
2. `port_offsets.py resolve` output, showing `resolved N addresses` and no
   `[ERROR]` lines.
3. `migrate_anonymous_globals.py` output from step 1, showing
   `status: RESOLVED_UNIQUE` for both targets.
4. Harness and tier0 results. Some suites fail on a GOG-only machine for
   unrelated reasons — check them against upstream `main` and say which.
5. Game version, macOS version, and chip.
6. `!test` and `!test_ingame` pass counts from the SE console.

That lets a maintainer confirm the addresses are reproducible from a binary
rather than hand-edited, which is the one thing they cannot check locally.

### Sanity check before submitting

Every address should move by one of a few deltas from the previous GOG build.
Mixed deltas within a segment mean something moved rather than shifted. For a
new store or build, compare against the same version's Steam row instead: on
4.1.1.7398727 every `__DATA` address moved by exactly −0x7880 or −0x78b0,
including both anonymous slots, which is how those two were corroborated.
