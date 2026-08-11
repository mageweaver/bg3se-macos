# Anonymous address migration: 4.1.1.7398727

Status: **resolved offline; report-only**

This note derives the two anonymous game-image slots needed for build
`4.1.1.7398727`. It does not authorize a version gate and no value was written
to `src/`. The derivation is implemented by
`scripts/re/migrate_anonymous_globals.py`; the lead owns source integration.

## Inputs and command

| Build | Frozen universal binary | arm64 slice offset |
|---|---|---:|
| `4.1.1.7209685` | `build/migration-binaries/4.1.1.7209685/Baldur's Gate 3` | `0xf558000` |
| `4.1.1.7398727` | `build/migration-binaries/4.1.1.7398727/Baldur's Gate 3` | `0xf5c0000` |

The offsets above are derived from each fat header by the script. The old
arm64 UUID is `9A647311-E263-3FF2-AF98-111CEDCB3034`; the new arm64 UUID is
`0C51CAED-6D60-3DCD-9299-8519C92631B0`.

Run from the repository root:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 scripts/re/migrate_anonymous_globals.py \
  --old "build/migration-binaries/4.1.1.7209685/Baldur's Gate 3" \
  --new "build/migration-binaries/4.1.1.7398727/Baldur's Gate 3" \
  --target global_switches_ptr \
  --target osiris_interface_ptr
```

The script uses exact local `nm -arch arm64 -n` symbols as anchors. It parses
the Mach-O mappings instead of assuming a universal-binary file offset, uses
the relocation masks already defined in `scripts/re/sig_scan_functors.py`, and
only reports a value when the role candidate is unique and independently
corroborated. It neither computes nor applies a data delta.

For every ADRP below, the target page is decoded as:

```text
imm21 = sign_extend((immhi << 2) | immlo, 21)
page  = (ADRP_PC & ~0xfff) + (imm21 << 12)
slot  = page + (unsigned_memory_imm12 * 8)
```

## `global_switches_ptr`

### Derivation recipe

1. Resolve the exact local symbol `_ZN3App20CreateGlobalSwitchesEv` with plain
   arm64 `nm`.
2. Within that function only, enumerate adjacent 64-bit `ADRP+STR` pairs. The
   global-switches role is the anonymous pointer slot receiving `x19`
   immediately after `eoc::EoCGlobalSwitches::EoCGlobalSwitches()` returns.
3. Require exactly one writer-role candidate.
4. Scan `__TEXT,__text` for adjacent 64-bit `ADRP+LDR/STR` references to the
   resulting slot. Require at least one load from a different named function.
5. Compare the old and new named-function instruction neighborhoods with
   address-bearing fields masked. Do not infer the result from the distance
   between the two data addresses.

### Old-build self-test (`4.1.1.7209685`)

`nm` places `App::CreateGlobalSwitches()` at `0x100c60208`. Its only adjacent
64-bit `ADRP+STR` role candidate is at `+0x98`:

```text
0x100c602a0  word 0x9003f4a8
             ADRP x8, immhi=0x1fa5, immlo=0
             signed imm21 = 32404
             (0x100c602a0 & ~0xfff) + (32404 << 12)
               = 0x108af4000

0x100c602a4  word 0xf9079913
             STR x19, [x8, #0xf30]
             imm12=0x1e6, scale=8, 0x1e6 * 8 = 0xf30
             0x108af4000 + 0xf30 = 0x108af4f30
```

This reproduces the required old ground truth exactly:
`global_switches_ptr=0x108af4f30`.

The immediately following named consumer, `App::SaveAnalyticsConfig()`, gives
an explicit ADRP+LDR cross-check of the same slot:

```text
0x100c602d4  word 0x9003f4aa
             ADRP x10, immhi=0x1fa5, immlo=0
             signed imm21 = 32404 => page 0x108af4000
0x100c602d8  word 0xf947994a
             LDR x10, [x10, #0xf30]
             imm12=0x1e6, scale=8 => slot 0x108af4f30
```

### New-build result (`4.1.1.7398727`)

`nm` places `App::CreateGlobalSwitches()` at `0x100c60890`. The corresponding
unique writer is again at `+0x98`:

```text
0x100c60928  word 0xb003f628
             ADRP x8, immhi=0x1fb1, immlo=1
             signed imm21 = 32453
             (0x100c60928 & ~0xfff) + (32453 << 12)
               = 0x108b25000

0x100c6092c  word 0xf907a113
             STR x19, [x8, #0xf40]
             imm12=0x1e8, scale=8, 0x1e8 * 8 = 0xf40
             0x108b25000 + 0xf40 = 0x108b25f40
```

The new `App::SaveAnalyticsConfig()` repeats the ADRP+LDR cross-check:

```text
0x100c6095c  word 0xb003f62a
             ADRP x10, immhi=0x1fb1, immlo=1
             signed imm21 = 32453 => page 0x108b25000
0x100c60960  word 0xf947a14a
             LDR x10, [x10, #0xf40]
             imm12=0x1e8, scale=8 => slot 0x108b25f40
```

### Uniqueness and corroboration

- Exactly one adjacent 64-bit `ADRP+STR` candidate exists in the named
  `App::CreateGlobalSwitches()` function in each build.
- The constructor call and register flow are preserved: malloc result in
  `x19`, `EoCGlobalSwitches::EoCGlobalSwitches(x19)`, then the candidate stores
  `x19` into the anonymous slot.
- The old and new candidate pairs match `2/2` after the repository's
  address-bearing instruction masks are applied.
- The named anchor windows match `42/43` instructions. The sole unmatched
  instruction is the unrelated SIMD constant-pool `LDR q0` at anchor `+0x1c`;
  it does not participate in the slot write.
- Both builds have exactly 851 adjacent direct references to the derived slot:
  849 loads and 2 stores. The old references span 575 distinct named load
  consumers other than the writer; the new references span 576.
- Named load corroboration includes `App::SaveAnalyticsConfig()`,
  `App::LoadAnalyticsConfig()`, `App::InitAppConfig()`,
  `App::CopyValuesToConfig()`, and `App::CopyValuesFromConfig()` in both
  builds.

Result: **unique candidate `global_switches_ptr=0x108b25f40`**.

## `osiris_interface_ptr`

### Derivation recipe

1. Resolve exact local symbol
   `_ZN3osi15OsirisInterface11OsirisQueryEjP16COsiArgumentDesc` with plain
   arm64 `nm`.
2. Enumerate adjacent 64-bit `ADRP+LDR` pairs in the first `0x100` bytes of the
   named prologue.
3. Select only a pair whose loaded pointer register is immediately checked by
   `CBZ`. This identifies the Osiris interface null/failure guard. It excludes
   the earlier stack-canary GOT load and the later generic allocator load,
   whose guard is `CBNZ`.
4. Require that this role occur exactly once.
5. Corroborate the slot with independent named loads and named lifecycle
   writers elsewhere in the binary, then compare the masked old/new prologue.

### Old-build self-test (`4.1.1.7209685`)

`nm` places
`osi::OsirisInterface::OsirisQuery(unsigned int, COsiArgumentDesc*)` at
`0x105c093b0`. The sole null-guarded ADRP+LDR role occurs at prologue `+0x30`:

```text
0x105c093e0  word 0xb00173e8
             ADRP x8, immhi=0xb9f, immlo=1
             signed imm21 = 11901
             (0x105c093e0 & ~0xfff) + (11901 << 12)
               = 0x108a86000

0x105c093e4  word 0xf9409519
             LDR x25, [x8, #0x128]
             imm12=0x25, scale=8, 0x25 * 8 = 0x128
             0x108a86000 + 0x128 = 0x108a86128

0x105c093e8  CBZ x25, failure path
```

This reproduces the required old ground truth exactly:
`osiris_interface_ptr=0x108a86128`.

### New-build result (`4.1.1.7398727`)

`nm` places the same exact function at `0x105c1439c`. The sole null-guarded
ADRP+LDR role remains at prologue `+0x30`:

```text
0x105c143cc  word 0xd0017508
             ADRP x8, immhi=0xba8, immlo=2
             signed imm21 = 11938
             (0x105c143cc & ~0xfff) + (11938 << 12)
               = 0x108ab6000

0x105c143d0  word 0xf9447d19
             LDR x25, [x8, #0x8f8]
             imm12=0x11f, scale=8, 0x11f * 8 = 0x8f8
             0x108ab6000 + 0x8f8 = 0x108ab68f8

0x105c143d4  CBZ x25, failure path
```

### Uniqueness and corroboration

- Exactly one adjacent 64-bit ADRP+LDR in each named OsirisQuery prologue has
  the required immediate `CBZ` null-guard role.
- The first 23 instructions of the old and new named prologues match `23/23`
  after address-bearing fields are masked; the selected pairs match `2/2`.
- Both builds contain exactly five adjacent direct references to the slot:
  three loads and two stores, owned by the same five named functions.
- Old named references are:
  `OsirisCall` load at `0x105c09178`, `OsirisQuery` load at `0x105c093e0`,
  `ErrorMessage` load at `0x105c0989c`, `InitStory` store at `0x105c0bd98`,
  and `ShutdownStory` store at `0x105c0c76c`.
- New named references are:
  `OsirisCall` load at `0x105c14164`, `OsirisQuery` load at `0x105c143cc`,
  `ErrorMessage` load at `0x105c14888`, `InitStory` store at `0x105c16d84`,
  and `ShutdownStory` store at `0x105c17758`.
- There are no direct `BL` edges to OsirisQuery in either build. This is not
  used as negative evidence because the independent peer loads and lifecycle
  writers establish the same slot role without assuming a direct-call ABI.

Result: **unique candidate `osiris_interface_ptr=0x108ab68f8`**.

## Summary

The migration is based on named function roles and decoded instructions, not
on a data-segment delta. Both new candidates are unique under their respective
recipes and preserve the old-build roles with independent named corroboration.
No source file or version gate was changed.

```text
global_switches_ptr=0x108b25f40
osiris_interface_ptr=0x108ab68f8
self_test global_switches_ptr: PASS expected=0x108af4f30 actual=0x108af4f30
self_test osiris_interface_ptr: PASS expected=0x108a86128 actual=0x108a86128
SELF_TEST=PASS
NEW_RESOLUTION=PASS
```
