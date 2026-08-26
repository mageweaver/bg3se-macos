# STDString is 16 bytes, not a std::string

## What was wrong

The resource field generator sized `STDString` as a `std::basic_string`, which
upstream's header says it is: 24 bytes on libc++, 32 on MSVC. It is neither.
Every field after the first string in a struct was therefore at the wrong
offset, in **35 of the 106** resource types.

This hid well. A wrong string size makes the size byte land on a zero, so the
string reads as *empty* -- and an empty string passes any "does this look like
text" check. `Tag` reported a clean layout for exactly this reason.

## The real layout

Read off a live `Progression` (a Level 1 Barbarian), not assumed:

```
+0x28  42 61 72 62 61 72 69 61 6e 00 00 00 00 00 00 09   |Barbarian......|
```

Nine characters inline, with the length `0x09` in **byte 15**.

```
+0x58  e0 dc ee d3 0a 00 00 00  51 00 00 00 60 00 00 80
       pointer                  size = 81   cap = 0x60, bit31 set
```

So:

| form  | layout |
|-------|--------|
| short | characters inline at +0, length in `byte[15] & 0x7f` (max 15) |
| long  | pointer at +0, `uint32` length at +8, `uint32` capacity at +12 with bit 31 set |

The discriminator is the top bit of the last byte in both forms, which is the
one thing it shares with libc++.

Every other field of that sample then decodes coherently -- `PassivePrototypesAdded`
with 4 entries, `PassivesAdded` an 81-character string, `BoostPrototypes` with 8,
`Boosts` 231 characters, `ProgressionType` 0, `Level` 1 -- which is what
confirms the size rather than any one field looking plausible.

## The false lead

`Progression` has a field upstream names `field_D0`, and with 24-byte strings
the computed offset landed on exactly `0xd0`. Four strings' worth of arithmetic
hitting the named offset is a strong-looking coincidence, and it argued for 24
right up until the live bytes said otherwise.

These `field_XX` names come from Windows reverse engineering and can also go
stale as a struct gains fields: `ClassDescription::field_71` computes to `0xd9`,
and `ActionResourceGroup`'s `field_38`/`field_48` sit 8 bytes above what we
compute. They are not an oracle for this build. Live bytes are.

## How it was caught

The layout self-check (`check_layout` in `src/lua/lua_resource_object.c`) runs
the first time a proxy is built for a type and validates a real object: strings
must look like text, arrays must not claim more elements than their capacity.
It flagged `Progression` while nine other types passed.

Its one blind spot was the vacuous pass above, now closed: a string that reads
as empty but whose first byte is printable is reported, because that is the
signature of a wrong string size.
