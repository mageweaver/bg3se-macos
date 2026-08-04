# CRPGStats_Modifier_ValueList::Insert (ARM64 macOS)

**Date:** 2026-08-03  
**Game build:** 4.1.1.7209685  
**Verdict:** **GO**

`CRPGStats_Modifier_ValueList::Insert(ls::FixedString const&, int)` allocates
and links missing map nodes itself. It is safe to call outside the stats loader
provided the caller supplies the verified object pointer and ABI, rejects an
existing label, and verifies both lookup directions after the call.

## Binary and address verification

The installed binary was resolved through `scripts/find_bg3.sh` (sourced by
`scripts/launch_bg3.sh` and `scripts/deploy.sh`):

```text
/Users/tomdimino/Library/Application Support/Steam/steamapps/common/
Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

`otool -f` reports this fat-architecture entry (the `offset` field is decimal):

```text
architecture 1
    cputype 16777228
    cpusubtype 0
    capabilities 0x0
    offset 257261568
    size 243843216
    align 2^14 (16384)
```

`257261568 == 0xf558000`. Therefore:

```text
slice file offset = 0xf558000
vaddr             = 0x101c44920
raw file offset   = 0xf558000 + (0x101c44920 - 0x100000000)
                  = 0x1119c920
```

The older `0xf534000` constant in `scripts/re/README.md` and its disassembler
scripts is stale for this installed binary. A direct `dd` extraction at
`0x1119c920` starts with bytes `ff 43 01 d1`, decoded as
`sub sp, sp, #0x50`, so the requested address is instruction-aligned and has a
plausible prologue.

The local symbol table independently identifies the address:

```text
0000000101c44920 t
__ZN28CRPGStats_Modifier_ValueList6InsertERKN2ls11FixedStringEi
```

Demangled:

```text
CRPGStats_Modifier_ValueList::Insert(ls::FixedString const&, int)
```

There is no address discrepancy. The Ghidra HTTP bridge was attempted first at
`http://127.0.0.1:8080/decompile_function?address=0x101c44920` and was down
(`curl: (7) Failed to connect`), so the evidence below comes from `nm`,
`otool -arch arm64 -tvV`, and raw-byte extraction.

## Insert disassembly and ABI

Relevant complete normal path (the instruction after `ret` is the exception
termination landing pad):

```asm
101c44920  sub   sp, sp, #0x50
101c44924  stp   x24, x23, [sp, #0x10]
101c44928  stp   x22, x21, [sp, #0x20]
101c4492c  stp   x20, x19, [sp, #0x30]
101c44930  stp   x29, x30, [sp, #0x40]
101c44934  add   x29, sp, #0x40
101c44938  mov   x20, x2
101c4493c  mov   x22, x1
101c44940  mov   x19, x0
101c44944  ldr   w8, [x1]
101c44948  ldr   w9, [x0, #0x8]
101c4494c  udiv  w10, w8, w9
101c44950  msub  w23, w10, w9, w8
101c44954  ldr   x9, [x0, #0x10]
101c44958  add   x9, x9, w23, uxtw #3
101c4495c  ldr   x9, [x9]
101c44960  cbz   x9, 0x101c44978
101c44964  ldr   w10, [x9, #0x8]
101c44968  cmp   w8, w10
101c4496c  b.ne  0x101c4495c
101c44970  str   w20, [x9, #0xc]
101c44974  b     0x101c449f4
101c44978  add   x0, x19, #0x18
101c4497c  bl    ls::ObjectAllocator<
                      ls::MapItem<ls::FixedString, int, ...>, ...
                  >::Allocate()
101c44980  mov   x21, x0
101c44984  ldr   w8, [x0, #0x8]
101c44988  ldr   w22, [x22]
101c4498c  cmp   w8, w22
101c44990  b.eq  0x101c449d0
101c44994  cmn   w22, #0x1
101c44998  b.eq  0x101c449a8
101c4499c  mov   x0, x22
101c449a0  bl    ls::gst::Acquire(unsigned int)
101c449a4  ldr   w8, [x21, #0x8]
101c449a8  cmn   w8, #0x1
101c449ac  b.eq  0x101c449cc
101c449b0  str   w8, [sp, #0x8]
101c449b4  adrp  x8, 0x108af4000
101c449b8  ldr   x8, [x8, #0xcd8]
101c449bc  mov   w9, #0xc600
101c449c0  add   x0, x8, x9
101c449c4  add   x1, sp, #0x8
101c449c8  bl    ls::gst::Map::Release(NodePoolData const&)
101c449cc  str   w22, [x21, #0x8]
101c449d0  str   w20, [x21, #0xc]
101c449d4  ldr   x8, [x19, #0x10]
101c449d8  lsl   x9, x23, #3
101c449dc  ldr   x10, [x8, x9]
101c449e0  str   x10, [x21]
101c449e4  str   x21, [x8, x9]
101c449e8  ldr   w8, [x19, #0x18]
101c449ec  add   w8, w8, #0x1
101c449f0  str   w8, [x19, #0x18]
101c449f4  ldp   x29, x30, [sp, #0x40]
101c449f8  ldp   x20, x19, [sp, #0x30]
101c449fc  ldp   x22, x21, [sp, #0x20]
101c44a00  ldp   x24, x23, [sp, #0x10]
101c44a04  add   sp, sp, #0x50
101c44a08  ret
```

The Darwin ARM64 ABI is:

```c
void Insert(void *value_list, uint32_t const *fixed_string, int32_t value);
// x0 = CRPGStats_Modifier_ValueList *this
// x1 = ls::FixedString const* (pointer to a 32-bit GST index)
// w2 = int value
```

This is **not** an LTO-promoted by-value FixedString call. The decisive
instruction is `ldr w8, [x1]` at `0x101c44944`; the function dereferences
`x1`. It also dereferences the reference again at `0x101c44988` before storing
the node key.

The observed value-list/map layout is:

```text
+0x00  uint32_t Name (FixedString index)
+0x08  uint32_t bucket_count
+0x10  MapItem **buckets
+0x18  uint32_t item_count / stateless ObjectAllocator base

MapItem:
+0x00  MapItem *next
+0x08  uint32_t key (FixedString index)
+0x0c  int32_t value
```

This layout also explains the manager destructor/replacement path, which calls
`ls::FixedStringMap<int>::~FixedStringMap()` on `value_list + 0x08` and releases
the value-list name stored at `value_list + 0x00`.

## Storage-growth gate

The allocation target at `0x101254708` is also present as a matching local
symbol:

```text
ls::ObjectAllocator<ls::MapItem<ls::FixedString, int, ...>, ...>::Allocate()
```

Its material allocation path is:

```asm
101254708  stp   x20, x19, [sp, #-0x20]!
10125470c  stp   x29, x30, [sp, #0x10]
101254710  add   x29, sp, #0x10
             ; one-time engine allocator bookkeeping omitted
101254768  mov   w0, #0x10
10125476c  bl    0x1077a03c4       ; _malloc symbol stub
101254770  mov   x19, x0
101254774  cbz   x0, 0x101254788
101254778  str   xzr, [x19]        ; next = NULL
10125477c  mov   w8, #-0x1
101254780  str   w8, [x19, #0x8]   ; key = null FixedString
1012547a8  mov   x0, x19
1012547ac  ldp   x29, x30, [sp, #0x10]
1012547b0  ldp   x20, x19, [sp], #0x20
1012547b4  ret
```

Therefore a missing key gets a new 16-byte map node from the engine allocation
path. `Insert` then acquires the FixedString reference, writes key/value, links
the node at the head of the selected bucket, and increments `item_count`.
The bucket array is fixed for this operation, but collisions are a linked list;
bucket reallocation is not required for correctness or storage growth.

**Gate conclusion: GO.** The function does not depend on spare loader-owned
array capacity and does not require the caller to allocate or grow backing
storage.

## Value-list resolution and runtime label creation

The Windows reference resolves the list with:

```cpp
GetStaticSymbols().GetStats()->ModifierValueLists.GetByName(typeName)
```

The macOS port already exposes the same data without a new global lookup:

- `RPGStats::m_ptr` is resolved by `stats_manager.c`.
- `ModifierValueLists` starts at `RPGStats + 0x08`.
- The existing `get_manager_count()`, `get_manager_element()`, and
  `find_rpgenumeration_by_name()` helpers traverse its `Values` array and read
  each value-list name from `+0x00`.
- `fixed_string_intern()` is an existing, version-gated wrapper around
  `ls::FixedString::Create(char const*, int)`, so runtime labels can be created
  without inventing a second interning path.

The old enum read helpers treated the value list itself as a
`CNamedElementManager`; the target disassembly disproves that assumption. Enum
readback must instead traverse the verified bucket/node layout above.

## Implementation contract

The call is enabled only for the exact audited game build. Before calling
`Insert`, the implementation must resolve the value list, intern the label,
and reject a node whose key already exists. The inserted integer is the
pre-call `item_count`, matching the Windows implementation's `Values.size()`.
The Windows `GetPropertyType()` rules are also applied: primitive types, flag
types, and empty/unknown value lists are rejected rather than mutated.

Success requires all of the following post-call reads:

1. `item_count == old_item_count + 1`;
2. key lookup maps the new label's FixedString index to the inserted integer;
3. reverse traversal maps that integer back to the same FixedString index and
   resolved label text.

Any failed guard or readback returns `false`; no unverified success is exposed
to Lua.

## Proposed Tier-2 tests

### Grows exactly once and verifies both lookup directions

```lua
local enumName = 'DamageType'
local label = 'BG3SE_Wave7_B1_GrowOnce'

local function contiguousCount(name)
    local i = 0
    while Ext.Stats.EnumIndexToLabel(name, i) ~= nil do
        i = i + 1
    end
    return i
end

assert(Ext.Stats.EnumLabelToIndex(enumName, label) == nil)
local before = contiguousCount(enumName)
assert(Ext.Stats.AddEnumerationValue(enumName, label) == true)
local index = Ext.Stats.EnumLabelToIndex(enumName, label)
assert(index == before)
assert(Ext.Stats.EnumIndexToLabel(enumName, index) == label)
assert(contiguousCount(enumName) == before + 1)
```

### Duplicate is rejected without a second growth

```lua
local enumName = 'DamageType'
local label = 'BG3SE_Wave7_B1_Duplicate'

assert(Ext.Stats.AddEnumerationValue(enumName, label) == true)
local index = assert(Ext.Stats.EnumLabelToIndex(enumName, label))
assert(Ext.Stats.AddEnumerationValue(enumName, label) == false)
assert(Ext.Stats.EnumLabelToIndex(enumName, label) == index)
assert(Ext.Stats.EnumIndexToLabel(enumName, index) == label)
```

### Survives a stats reload

Paste this into startup Lua before triggering the Tier-2 harness's normal
stats reload. The first `StatsStructureLoaded` installs the value; later
`StatsLoaded` deliveries only verify it, so a rebuild that discards the map
node will fail instead of silently re-adding it.

```lua
local enumName = 'DamageType'
local label = 'BG3SE_Wave7_B1_Reload'
local installedIndex = nil

Ext.Events.StatsStructureLoaded:Subscribe(function()
    if installedIndex == nil then
        assert(Ext.Stats.AddEnumerationValue(enumName, label) == true)
        installedIndex = assert(Ext.Stats.EnumLabelToIndex(enumName, label))
    end
end)

Ext.Events.StatsLoaded:Subscribe(function()
    if installedIndex ~= nil then
        assert(Ext.Stats.EnumLabelToIndex(enumName, label) == installedIndex)
        assert(Ext.Stats.EnumIndexToLabel(enumName, installedIndex) == label)
    end
end)
```
