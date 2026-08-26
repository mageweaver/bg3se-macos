# Generic static data managers (Ext.StaticData)

Status: **scoped, not built.** Four types are currently missing; the generic path
would cover all 114.

## What is already fixed

- `ClassDescription` accepted as an alias for our `Class` (same manager,
  `eoc::ClassDescriptions`). Cleared DoubleSubclass and
  SubclassCompatibilityFramework.
- `Ext.StaticData.Get` argument order corrected to `(guid, type)` per
  upstream/BG3Extender/IdeHelpers/ExtIdeHelpers.lua. Cleared 5eSpells and
  CommunityLibrary — their "unknown type" was a GUID being read as a type name.

## What is still missing

Only four distinct types, with their engine classes from
upstream/BG3Extender/GameDefinitions/GuidResources.h:

    SpellList                          eoc::SpellListManager
    Tag                                ls::TagManager
    CharacterCreationAppearanceVisual  eoc::CharacterCreationAppearanceVisualManager
    PhotoModeEmoteCollection           eoc::photo_mode::EmoteCollections

Affecting: EasyCheat (Tag), BG3SX (CharacterCreationAppearanceVisual,
PhotoModeEmoteCollection), and two SpellList callers.

## Why the current mechanism does not extend cheaply

We capture managers by hooking a per-type `Get<T>()` accessor. Of these four, only
`ls::TagManager` has one as a symbol; the rest are inlined. So three of the four
cannot be added the existing way at all.

## The generic path

All of them live under `ls::ImmutableDataHeadmaster`, reached by type index.
Disassembling `ls::ImmutableDataHeadmaster::Get<ls::TagManager>()` @ 0x10118616c
shows the shape:

    typeIndex = *(ls::TypeId<T, ls::ImmutableDataHeadmaster>::m_TypeIndex)   // a global
    slot      = typeIndex % hashSize
    ... open-addressed HashMap probe using arrays at headmaster +0x00, +0x10,
        +0x20, with the count at +0x2c

So the work is:

1. Generate a name -> `m_TypeIndex` symbol-address registry, exactly as
   `src/entity/generated_component_registry.c` already does for components. The
   binary carries 656 `ImmutableData` symbols of the form
   `ls::TypeId<eoc::ClassDescriptions, ls::ImmutableDataHeadmaster>::m_TypeIndex`.
2. Locate the ImmutableDataHeadmaster instance.
3. Implement the HashMap probe (Norbyte's HashMap, not LegacyRefMap — a different
   container from the one in the AnimationSet work).
4. Route `Ext.StaticData.Get/GetAll` through it, keeping the nine hand-hooked
   managers working.

That replaces nine bespoke managers with all 114 types and removes the Get<T> hook
requirement entirely.

## Note

Check `upstream/` first for anything structural. Every answer in this document came
from there — the type list, the engine classes, and the argument order — and the
AnimationSet work lost a long stretch to disassembly before the same realisation.


## DONE: Get and GetAll routed generically (2026-08-26)

All `Unknown static data type` errors are gone. EasyCheat and BG3SX both cleared.

    registry            106 types, generated from upstream + binary symbols
    manager resolution  via ls::ImmutableDataHeadmaster, verified for all types
    Get                 the bank's own virtual GetObjectByKey (vtable slot 6)
    GetAll              the bank's Resources.Keys array (Guid[])

Counts verified live against what the game ships: Background 28, Race 203,
Tag 1298, CharacterCreationAppearanceVisual 9044.

## Remaining: Ext.StaticData.Create

BG3AF's PhotoModeManager/EmoteCollection.lua:166 calls

    Ext.StaticData.Create("PhotoModeEmoteCollection", uuid)

Our Get fix is what let it reach that line — it now correctly returns nil for a
resource that does not exist yet, so BG3AF proceeds to create one.

Upstream implements this as `gGuidResourceHelpers.Get(type)->Create(...)`, a C++
template that knows `sizeof(T)` for each of the 114 types at compile time. That is
the one thing the generic path cannot get for free: allocating a resource requires
knowing how big it is.

### If this is picked up

`sizeof(T)` can be derived at runtime rather than tabulated: two GUIDs from the same
bank, looked up through GetObjectByKey, return pointers into the same values array,
so `|ptr_j - ptr_i| / |j - i|` is the element size. Adding the object would then go
through `AddLoadedObject`, vtable slot 7.

The risk is what a zero-filled resource means to the game. That needs checking
before anything is written, not after.

### Priority note

This only blocks BG3AF's photo-mode emote registration. BG3SX's animation links are
added earlier in the same file and already work — 91 of them were confirmed present
in a live session. So the cost of leaving this unimplemented is cosmetic, not
structural.
