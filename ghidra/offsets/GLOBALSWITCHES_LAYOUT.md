# GlobalSwitches: Windows layout does not transfer to macOS

Investigated 2026-08-20 for `Ext.Utils.GetGlobalSwitches`.

## The deferral is real, and now has a proof

Windows implements this as a one-liner — `GetStaticSymbols().GetGlobalSwitches()`
(Utils.inl:149) — and exposes ~456 fields through the property map. The pointer
is not the problem: the port already resolves the singleton
(`VersionOffsets.global_switches_ptr = 0x08b25f40`) and already reads one field
from it. The problem is the struct layout.

## Calibration against a known-good anchor

`SkipSplashScreen` is the one field whose macOS offset was recovered by
disassembly rather than inference (`ldrb w8, [x8, #0x6ac]`, re-verified across
builds, including a toggle site):

    macOS   SkipSplashScreen @ 0x6AC     (global_switches.c:18)

BG3SE's `field_XXXX` placeholders encode hex offsets, so the Windows position is
readable directly from `GameDefinitions/Misc.h`:

    field_13E0  @ 0x13E0
    UIType      @ 0x13E4
    SkipLarianSignUp @ 0x13E5
    SkipSplashScreen @ 0x13E6      <- Windows
    field_13E7  @ 0x13E7

So the same field sits at **0x6AC on macOS and 0x13E6 on Windows**. Not a shift:
macOS reaches at 0x6AC what Windows reaches at 0x13E6, roughly a third of the
address. And Windows independently declares `float field_6AC` at 0x6AC — a float
where macOS keeps a bool.

## Consequence

Porting the Windows struct would read wrong data for essentially every field,
and it would do so **silently**: the struct is overwhelmingly bools, floats and
small ints, so a misaligned read yields plausible values rather than an error.
That is the same failure mode as the static-data strides and
`TransformComponent.Position`, on a 456-field surface.

Each field therefore needs individual recovery the way SkipSplashScreen did —
find the property registration or an access site and read the offset off the
instruction. That is tractable per field and intractable for 456.

## What would make this cheap

A property-name table. If the engine registers these switches by name (the
Windows comment for SkipSplashScreen mentions "property registration"), the
registration site would map every name to its offset in one pass, which is how
the 666-entry RemoveComponent table and the ReplicatedTypeContext globals were
recovered. Worth a scan before attempting anything field-by-field.

Until then `GetGlobalSwitches` stays absent rather than returning a table whose
values are wrong.
