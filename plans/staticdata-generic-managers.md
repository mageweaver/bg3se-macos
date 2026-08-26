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
