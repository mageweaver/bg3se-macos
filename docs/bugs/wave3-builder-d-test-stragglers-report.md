# Wave 3 Builder D — Test-Honesty Stragglers

Date: 2026-07-30  
Scope: the three remaining test-coverage gaps from the Wave 2 review

## Damage-event subscription lifecycle

The two tier-2 tests had already been strengthened after the review: each
created a real subscription, required a numeric handle, and required
`Unsubscribe` to return exactly `true`. Their names now say precisely what
they verify:

- `Parity.Events.BeforeDealDamage.SubscriptionLifecycle`
- `Parity.Events.DealDamage.SubscriptionLifecycle`

Each test asserts that its event object exists, `Subscribe` returns a numeric
handle, and that handle can be unsubscribed successfully. Neither test claims
that a damage event fired. Real firing remains the responsibility of the
existing `Stats.DamageEvents.PairedFiring` tier-2 test, which requires a
positive before count and an identical after count.

## ExecuteStatsFunctors hidden-result ABI guard

Added the binary-independent pytest
`tests/harness/test_functor_abi_audit.py`. It structurally parses
`src/stats/functor_hooks.c` and:

1. Requires the discovered `hook_ExecuteFunctors_*` set to equal the expected
   nine wrappers.
2. Requires every wrapper to declare `void *result_out` as its first
   parameter.
3. Finds every call to that wrapper's saved original function and requires
   `result_out` to be the first forwarded argument.

This catches both signature drift and forwarding omissions, including either
original-function call path in a wrapper. The existing offset/signature test
in `tests/harness/test_offset_audit.py` was not changed or weakened.

## Visible HandleRoundtrip fixture absence

`Parity.Entity.HandleRoundtrip` still performs its real roundtrip when the
entity exposes `GetHandle`: it requires a non-nil handle and requires
`Ext.Entity.GetByHandle(handle)` to return a non-nil entity.

When `GetHandle` is absent, the test now prints:

```text
    (fixture absent; not exercised) entity.GetHandle unavailable
```

The fixture-dependent pass is therefore visible and cannot be mistaken for an
exercised handle roundtrip.

## Offline verification

- `cd build && cmake --build .`
  - Succeeded; produced the arm64/x86_64 universal `libbg3se.dylib`.
  - The sandbox denied the existing post-build copy into the installed game
    app, but the build target completed successfully.
- `./build/bin/bg3se_test_tier0`
  - **55/55 passed**.
- `PYTHONPATH=tools python3 -m pytest tests/harness/ -q`
  - **195 passed** (previously 194; one offline ABI-forwarding test added).
- No Baldur's Gate 3 launch, live harness command, or
  `/tmp/bg3se.sock` access was performed.

## Live filters for the orchestrator

With a loaded save, run the renamed lifecycle filters and the handle test:

```text
!test_ingame Parity.Events.BeforeDealDamage.SubscriptionLifecycle
!test_ingame Parity.Events.DealDamage.SubscriptionLifecycle
!test_ingame Parity.Entity.HandleRoundtrip
```

For actual event firing, separately arm the existing probe, cause a real
damage-functor invocation such as a `BURNING` status tick, and then run:

```text
!test_ingame Stats.DamageEvents.PairedFiring
```

The first two filters prove only subscription lifecycle mechanics. The paired
firing filter is the live behavioral assertion.
