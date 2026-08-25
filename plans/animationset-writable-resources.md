# Writable AnimationSet resources (BG3AF / BG3SX / WickedAnims / GrazztRing)

Status: **derivation in progress**. Read this before touching `src/lua/lua_resource.c`
or adding resource property support.

## What the mods need

BG3AF's `AnimationSet:AddLink` (Shared/WaterfallManager/AnimationSet.lua:128) is the
API every dependent mod goes through:

```lua
local subSets = self[1].AnimationBank.AnimationSubSets
for SubSetMapKey, AnimationSubSet in pairs(subSets) do
    AnimationSubSet.Animation[mapKey] = { ID = animID, flags = flags }   -- WRITE
end
```

So this is not a read-only exposure. The mod mutates the game's own containers, and
the game must honour the additions afterwards. A read-only version would let the
loop run and silently drop every assignment — the same silent-success failure mode
that cost the mod team ~15 builds on the Osiris dispatch (see 769a83c-era work).
Do not ship that.

## Structure (confirmed)

From `ls::AnimationSetResource::Visit(ls::ObjectVisitor*)` @ 0x105cef668, whose
field-name FixedStrings come from the `ls::FFFS` table:

    AnimationSet -> AnimationBank -> AnimationSubSets -> MapKey / Object

Confirmed independently against BG3SX's own definition,
`Public/BG3SX/Content/Assets/[PAK]_AnimationSets/_merged.lsx`:

    AnimationSetBank
      Resource            ID = bfa9dad2-2a5b-45cc-b770-9537badf9152
        AnimationSet
          AnimationBank
            AnimationSubSets
              Object      MapKey = ""        <- the DFLT subset
                Animation                    <- MapKey -> ID entries

BG3SX's sets use an empty-string MapKey, matching BG3AF's comment that
`SubSetMapKey of "" hosts DFLT links`. That gives a ground-truth target when
searching live memory: resource `bfa9dad2-...` must expose one subset keyed by "".

Relevant classes: `ls::AnimationSetResource`, `ls::AnimationSubSet`,
`ls::AnimationResource`.

## Layout confirmed live (build 4.1.1.7398727)

`AnimationSetResource` records are **48 bytes** and sit contiguously — the same
vtable (0x10D742BE8) repeats at +0, +48, +96, +144 from any record base.

    +0x00  vtable
    +0x08..0x14  zero
    +0x18  GUID FixedString        <- resource identity (see f90e2e2)
    +0x28  heap pointer            <- only pointer in the record; the bank

The 48-byte stride is what made +0x48 look like a valid GUID offset: it lands on
the NEXT record's GUID, which is a real UUID and looks up fine. Verify identity by
asking for a known UUID and reading the field back, never by round-tripping.

`+0x28` is per-resource, not shared: two different sets give different pointers
while sharing the AnimationSetResource vtable. It leads to an array of 32-byte
records:

    +0x00  u32 = 1, u32 = 2
    +0x08  pointer to this record + 0x10   (self-referential)
    +0x10  0, 0
    +0x18  pointer into the resource pool

Dereferencing that last pointer and resolving FixedStrings finds the GUIDs
bfa9dad2, 0c914b3f, 284eea6d and 71d2f8cc — precisely the sibling Resource nodes
in BG3SX's `_merged.lsx`, whose root node is `AnimationSetBank`. So **+0x28 is the
bank listing sibling sets, not this resource's AnimationBank.**

### gst::Map is a packed handle, not a pointer

`ls::gst::Map::Release(NodePoolData const&)` @ 0x1064d11dc decodes its argument:

    ldr w9, [x1] ; and x9, #0xf     -> low 4 bits  = pool selector
    madd x21 = x9*0x1200 + poolBase -> pools are 0x1200 bytes apart
    ubfx x9, w22, #4, #16           -> bits 4..19  = index into a table

So a NodePoolData is a single u32 handle: `pool = h & 0xF`, `index = (h >> 4) & 0xFFFF`.
That is why no plain pointer to the subsets could be found in the record.

### AnimationSetResource record (48 bytes, vtable identity confirmed)

The vptr resolves to `vtable for ls::AnimationSetResource` + 0x10 (past the RTTI
header), so the 48-byte object is genuinely the resource.

    +0x00  vptr
    +0x18  GUID FixedString              (verified by identity check)
    +0x1C  u32   NodePoolData handle     (differs per resource)
    +0x20  u32   count                   (0 for BG3SX's empty sets, 2 for vanilla)
    +0x28  pointer                       (heap; differs per resource)

Differential across three sets — BG3SX's `bfa9dad2` ships ONE subset whose Animation
map is EMPTY, vanilla `HUM_M_Base` / `HUM_F_Base` have populated ones:

    BG3SX     +0x1C = 1          +0x20 = 0
    HUM_M     +0x1C = 393217     +0x20 = 2      (0x60001 -> pool 1, index 0x6000)
    HUM_F     +0x1C = 131072     +0x20 = 2

### Why read-only is not an option here

BG3SX's `_merged.lsx` defines its subsets with an EMPTY `Animation` node — the
container ships empty and BG3AF's AddLink is what fills it. So there is nothing to
read; the write path is the only path that does anything.

### Open question

Confirm what +0x1C indexes and what +0x20 counts. The handle decode is taken from
Release; the pool base it is relative to (`[x0, #0x1080]`, where x0 is the Map)
still has to be located, and the node layout read out of it.

Next experiment: resolve +0x1C for BG3SX's set through the pool and check that it
yields exactly one subset whose MapKey FixedString is the empty string. That is the
ground truth the whole model has to reproduce before any of it is trusted.

### Next probe

Take the pointer at +0x28, walk the 32-byte records, and for each one dump the
pointed-to block far enough to find a FixedString that is NOT a set GUID — the
subset MapKey ("" for BG3SX) or an animation MapKey. That distinguishes "this is
just the sibling list" from "the subsets hang off here too".

## The hard part

The containers are `ls::gst::Map` — the game's hash map with a node pool. Only one
method is exported:

    ls::gst::Map::Release(ls::gst::NodePoolData const&)

`Insert` and `Find` are inlined templates, so there is nothing to call. Writing
therefore means replicating node allocation against the game's own pool, derived by
disassembly. Getting it wrong corrupts the game heap rather than failing cleanly.
The port has no existing map-walking code to build on — checked, nothing.

## Plan

1. **Layout, live.** With the game running, take the resource pointer for
   `bfa9dad2-...` (now reachable: `Ext.Resource.Get(uuid, "AnimationSet")` works
   since 2783854 fixed the type index to 4) and walk the struct with
   `Ext.Debug.ReadU32` / `ReadPtr` / `ReadFixedString`, looking for the subset map
   whose single key resolves to "". That pins AnimationBank and AnimationSubSets.
2. **gst::Map read path.** Derive bucket array, node layout, and the hash from the
   located map. Verify by enumerating keys and matching them against the LSX.
3. **Read-only exposure**, gated so writes raise rather than silently drop.
4. **Insert path.** Derive node allocation from inlined call sites. Verify by adding
   one link and confirming the game plays the animation.

Steps 1-3 are safe. Step 4 is the one that can corrupt memory; do it last, behind a
flag, and verify each write by reading the map back before trusting it.

## Notes

- `Ext.Resource.Get` returns opaque stubs today (`Type`, `ResourceId`, `_ptr`,
  `_ptrHex`). Typed fields are what this plan adds.
- The GUID offset is +0x48 for some resource classes but not all (169abc1);
  subclasses differ. Do not assume it for AnimationSetResource without checking.
