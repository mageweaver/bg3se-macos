# ModifierValueLists registry — 4.1.1.7398727 re-derivation

## Result

`RPGStats` begins with its
`CNamedElementManager<CRPGStats_Modifier_ValueList, int, 0, -1>` base. The
manager is at `RPGStats + 0x00`, not `RPGStats + 0x08`. Its first qword is the
manager vtable. There is no separate outer `RPGStats` vtable before it.

The complete read chain is identical on builds 4.1.1.7209685 and
4.1.1.7398727:

```text
RPGStats::m_ptr slot --load pointer--> RPGStats / ModifierValueLists manager
manager + 0x14       --load u32-----> element count
manager + 0x08       --load pointer-> element pointer buffer
buffer + index*8     --load pointer-> CRPGStats_Modifier_ValueList
element + 0x00       --load u32-----> name FixedString index
element + 0x08       --load u32-----> value-map bucket count
element + 0x10       --load pointer-> MapItem **bucket array
element + 0x18       --load u32-----> value-map item count
```

`MapItem` remains `next +0x00`, FixedString key `+0x08`, and signed integer
value `+0x0c`.

## Versioned addresses and slice mapping

| Evidence | 4.1.1.7209685 | 4.1.1.7398727 |
|---|---:|---:|
| arm64 slice file offset | `0x0f558000` | `0x0f5c0000` |
| `RPGStats::m_ptr` | `0x1089cd730` | `0x1089fddd0` |
| ValueList manager `GetAmountOfEntries` | `0x102101ed8` | `0x1020ff5cc` |
| ValueList manager `GetEntry` | `0x102101ee0` | `0x1020ff5d4` |
| `CRPGStats_Modifier_ValueList::Insert` | `0x101c44920` | `0x101c42014` |
| Insert raw file offset | `0x1119c920` | `0x11202014` |

`nm -arch arm64` identifies both singleton slots and both Insert functions by
their exact local symbols. Reading 16 bytes at the two computed raw offsets
produces the same prologue in both binaries:

```text
ff 43 01 d1 f8 5f 01 a9 f6 57 02 a9 f4 4f 03 a9
```

## Registry-root evidence

In `RPGStats::Destroy`, `x19` holds the `RPGStats *`. Both binaries destroy the
first manager inline with no adjustment to `x19`:

```asm
; 4.1.1.7209685
102104020  ldr  w8, [x19, #0x14] ; first-manager count
102104068  ldr  x8, [x19, #0x8]  ; first-manager element buffer
10210406c  ldr  x21, [x8, x22]   ; element pointer
102104074  add  x0, x21, #0x8    ; destroy element FixedStringMap<int>
10210407c  ldr  w8, [x21]        ; release element name at +0x00

; 4.1.1.7398727
102101714  ldr  w8, [x19, #0x14]
10210175c  ldr  x8, [x19, #0x8]
102101760  ldr  x21, [x8, x22]
102101768  add  x0, x21, #0x8
102101770  ldr  w8, [x21]
```

After this inline teardown, each build begins the next manager at
`RPGStats + 0x60`. This fixes the first manager's base at `+0x00` and preserves
the previously observed `+0x60` manager stride.

The named manager accessors independently fix the array fields:

```asm
; GetAmountOfEntries, 7209685 / 7398727
102101ed8 / 1020ff5cc  ldr w0, [x0, #0x14]

; GetEntry, 7209685
102101ee0  ldr w8, [x0, #0x14]
102101eec  ldr x8, [x0, #0x8]
102101ef0  ldr x0, [x8, x1, lsl #3]

; GetEntry, 7398727
1020ff5d4  ldr w8, [x0, #0x14]
1020ff5e0  ldr x8, [x0, #0x8]
1020ff5e4  ldr x0, [x8, x1, lsl #3]
```

## Per-element map and Insert ABI

The two Insert bodies are instruction-for-instruction equivalent apart from
addresses. The decisive loads/stores are:

```asm
; 7209685 addresses shown first; 7398727 second
101c44944 / 101c42038  ldr  w8, [x1]        ; FixedString const&
101c44948 / 101c4203c  ldr  w9, [x0, #0x8] ; bucket count
101c44954 / 101c42048  ldr  x9, [x0, #0x10]; bucket array
101c449e8 / 101c420dc  ldr  w8, [x19, #0x18]
101c449f0 / 101c420e4  str  w8, [x19, #0x18]; item count + 1
```

The ABI remains:

```c
void Insert(void *value_list, uint32_t const *fixed_string, int32_t value);
// x0 = this, x1 = pointer to the 32-bit FixedString index, w2 = value
```

Production code resolves it through
`offset_table_game_fn(GAME_FN_VALUELIST_INSERT)`. No literal VA remains in
`stats_manager.c`.

## July regression diagnosis

The July singleton migration correctly moved `RPGStats::m_ptr` to
`0x1089cd730`, but the ValueList helper separately encoded
`RPGSTATS_OFFSET_MODIFIER_VALUE_LISTS = 0x08`. That assumption counted the
first manager's own vtable twice: once as a hypothetical outer `RPGStats` vtable
and once inside `CNamedElementManager`. Consequently the helper treated
`RPGStats + 0x08` as the manager base, read the count from actual
`RPGStats + 0x1c`, and read the element buffer from actual `RPGStats + 0x10`.
Every enum lookup therefore missed even though the singleton and other manager
reads remained healthy.

This file supersedes only the `RPGStats + 0x08` registry-root claim in
`VALUELIST_INSERT.md`. Its value-list map layout and Insert ABI remain valid.

## Safety and live gate

Registry reads now validate `count <= capacity <= 4096` and require a non-null
element buffer whenever `count > 0`. The read-only Lua diagnostic
`Ext.Stats.GetValueListRegistryDiagnostic()` returns validity, manager address,
count, and at most eight resolved names after scanning at most 32 elements.

Insertion is still gated by `BG3_KNOWN_VERSION`, which remains
`4.1.1.7209685`. Thus 7398727 can exercise the corrected read path and
diagnostic, while mutation remains fail-closed until the later live proof.

## Reproduction commands

```sh
nm -arch arm64 <binary> | rg \
  'RPGStats5m_ptr|CRPGStats_Modifier_ValueList6Insert|CNamedElementManagerI28CRPGStats_Modifier_ValueList'

otool -arch arm64 -tvV -p __ZN8RPGStats7DestroyEv <binary>
otool -arch arm64 -tvV -p \
  __ZNK20CNamedElementManagerI28CRPGStats_Modifier_ValueListiLi0ELin1EE18GetAmountOfEntriesEv \
  <binary>
otool -arch arm64 -tvV -p \
  __ZN28CRPGStats_Modifier_ValueList6InsertERKN2ls11FixedStringEi \
  <binary>
```
