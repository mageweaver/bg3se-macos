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

Following +0x28 gives a region of repeating 16-byte entries:

    +0x00  u32 = 1
    +0x04  u32 = 2
    +0x08  pointer               (successive entries 32 bytes apart)

Not yet interpreted — do not build on this until the entry shape is confirmed and
its keys enumerate to something matching the LSX (BG3SX's set must yield one subset
keyed by "").

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
