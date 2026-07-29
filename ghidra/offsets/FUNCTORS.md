# Stats Functor System Offsets

**Game version:** Baldur's Gate 3 v4.1.1.7209685

**Platform:** macOS ARM64

**Verified:** 2026-07-29

**Status:** nm-verified local symbols; wrapper ABIs verified

## Ground truth

The macOS binary retains these functions as LOCAL (`t`) symbols. Use plain
`nm` without `-g`/`-gU` when auditing them. The addresses and signatures below
were also corroborated by a masked ARM64 signature scan; each scan result was
unique.

| Target | Address | Verified signature |
|---|---:|---|
| `ExecuteStatsFunctor` | `0x10577399c` | `(StatsFunctorBase const*, unsigned long, AttackTargetContextData&)` |
| `ExecuteStatsFunctors<AttackTarget>` | `0x10577787c` | `(StatsFunctorList const*, AttackTargetContextData&)` |
| `ExecuteStatsFunctors<AttackPosition>` | `0x105777bd0` | `(StatsFunctorList const*, AttackPositionContextData&)` |
| `ExecuteStatsFunctors<Move>` | `0x1057796c0` | `(StatsFunctorList const*, MoveContextData&)` |
| `ExecuteStatsFunctors<Target>` | `0x10577a87c` | `(StatsFunctorList const*, TargetContextData&)` |
| `ExecuteStatsFunctors<NearbyAttacked>` | `0x10577e43c` | `(StatsFunctorList const*, NearbyAttackedContextData&)` |
| `ExecuteStatsFunctors<NearbyAttacking>` | `0x10577fb0c` | `(StatsFunctorList const*, NearbyAttackingContextData&)` |
| `ExecuteStatsFunctors<Equip>` | `0x10578098c` | `(StatsFunctorList const*, EquipContextData&)` |
| `ExecuteStatsFunctors<Source>` | `0x1057829f4` | `(StatsFunctorList const*, SourceContextData&)` |
| `ExecuteStatsFunctors<Interrupt>` | `0x105786548` | `(ecs::EntityWorld&, StatsFunctorList const*, InterruptContextData&)` |
| `(anonymous namespace)::ProcessDealDamageFunctors` | `0x10537e8b4` | 12 parameters; expanded below |

### Wrapper ABI

The eight non-Interrupt `ExecuteStatsFunctors` overloads are free functions,
not methods:

```c
void ExecuteStatsFunctors(
    StatsFunctorList const* functors,
    ContextData& context);
```

At the C ABI boundary the reference is a pointer, so the wrappers have exactly
two pointer-sized parameters. There is no `self` parameter and no leading
`HitResult*`.

The Interrupt overload has exactly three parameters:

```c
void ExecuteStatsFunctors(
    ecs::EntityWorld& world,
    StatsFunctorList const* functors,
    InterruptContextData& context);
```

The main dispatcher is:

```c
void ExecuteStatsFunctor(
    StatsFunctorBase const* functor,
    unsigned long functorId,
    AttackTargetContextData& context);
```

It is not a general replacement for the context-specific list overloads: it
accepts one functor and only `AttackTargetContextData`.

## Damage processing

The exact nm symbol at `0x10537e8b4` is:

```c
void ProcessDealDamageFunctors(
    ecs::WorldView<
        eoc::repose::StateComponent const,
        ls::PhysicsComponent const,
        ls::TransformComponent const,
        ls::uuid::Component const>& worldView,
    eoc::StatsFunctorBase const& functor,
    ls::ID<ecs::EntityHandleTraits> const& entity,
    ls::Optional<Vector3f> const& position,
    eoc::spell_cast::StateComponent const& spellState,
    ls::EnumFlags<eoc::EDamageEffectFlag> const& damageEffectFlags,
    EAbility const& ability,
    ESpellAttackType const& spellAttackType,
    eoc::interrupt::Dependency const& dependency1,
    eoc::interrupt::Dependency const& dependency2,
    int eventIndex,
    ls::DynamicArray<
        eoc::interrupt::InterruptEvent,
        ls::TaggedAllocator<int>>& interruptEvents);
```

`functor_hooks.c` fires `BeforeDealDamage` before this function and
`DealDamage` after it returns. The event payload safely copies the entity ID and
the by-value `eventIndex`, and exposes opaque pointer fields for all reference
arguments. `Hit`, `Attack`, `Position`, `DamageEffectFlags`, `Ability`,
`SpellAttackType`, and `Result` remain explicitly nil because this signature
does not provide the Windows `ApplyDamage` payload and the opaque layouts have
not been independently verified.

### Corrected old attribution

The old documentation labeled `0x10538e8fc` as
`DealDamageFunctor::ApplyDamage`. On 4.1.1.7209685 its masked signature maps to
`0x10537de3c`, which lies inside:

```text
(anonymous namespace)::ProcessSpellFunctors @ 0x10537de24
```

That old label was incorrect. `ProcessSpellFunctors` is documented for
correction only and is not a hook target. The damage hook target is the exact
nm match `ProcessDealDamageFunctors @ 0x10537e8b4`.

## Recovery and verification method

1. Dumped headless Ghidra bytes/instructions for the baseline targets with
   `ghidra/scripts/dump_functor_bytes.py`.
2. Masked relocation-sensitive ARM64 operands and scanned the 7209685 binary
   with `scripts/re/sig_scan_functors.py`.
3. Required one unique candidate for every target.
4. Corroborated every candidate and full ABI with demangled LOCAL symbols from
   plain `nm | c++filt`.
5. Added each address to `tests/harness/test_offset_audit.py`, whose local-symbol
   lookup intentionally runs `nm` without `-g`.

Address uniqueness proves the code location; the exact
`FUNCTOR_ADDRS_VERIFIED_BUILD` install gate separately protects the verified
wrapper ABIs and keeps unknown builds fail-closed.
