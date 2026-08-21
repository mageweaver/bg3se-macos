# Osiris hooks dropped return values, breaking multiplayer

Found and fixed 2026-08-20.

## Symptom

With BG3SE loaded, "Create Multiplayer Game" failed — both local and crossplay.
Vanilla worked. The game itself was fine; BG3SE was the cause.

## Bisection

The binary stays patched either way (the dylib is an `LC_LOAD_WEAK_DYLIB`, so
removing the file just makes the load fail silently), which ruled out the
injection itself immediately.

| Configuration | Multiplayer |
|---|---|
| vanilla (dylib moved aside) | works |
| BG3SE, `BG3SE_NO_NET=1` | fails |
| BG3SE, `BG3SE_NO_HOOKS=1` (all patches off) | works |
| BG3SE, `BG3SE_NO_STATICDATA_HOOKS=1` | fails |
| BG3SE, `BG3SE_NO_OSIRIS_HOOKS=1` | works |
| BG3SE, `BG3SE_NO_HOOK_INITGAME=1` only | works |
| BG3SE, all hooks, both fixes applied | works |

That narrowed 40+ code patches to one function.

## Root cause

Two hooked Osiris functions return values, but both hooks were declared `void`.
Each dropped the original's result, then ran its own post-call work — which
clobbers `x0` — and returned whatever was left. Callers testing those results
read noise.

`COsiris::InitGame()` — the one that broke multiplayer, since the game checks
its result when creating a session:

    mov  x0, x23        <- return value
    add  sp, sp, #0x230
    ldp  x29, x30, [sp, #0x50]
    ...
    ret

The other return path also loads `x0`.

`COsiris::Event(unsigned int, COsiArgumentDesc*)`:

    mov  w19, #0x1
    ...
    mov  x0, x19
    ret

with `cset w0, ne` on another path.

Both now capture the forwarded result before their post-call work and return it.
`InitGame` passes it through as a 64-bit word so it survives whether the value
is a bool, an int or a pointer.

## Note on method

`RemoveComponent` raised the same question earlier the same day and the answer
went the other way — `ImmediateWorldCache::RemoveComponent<T>` really is `void`,
proven from the mangled name, and an earlier "fix" that read a return value from
it was wrong and had to be retracted. A hooked function's return type cannot be
assumed in either direction; read the epilogue.

`Event` was found by reading signatures. `InitGame` looks correct until the
compiler output is checked, and was only findable because the bisection pointed
at it.

## Tooling added

Per-group and per-hook toggles, so this class of problem is bisectable without
rebuilding:

    BG3SE_NO_OSIRIS_HOOKS      BG3SE_NO_HOOK_INITGAME
    BG3SE_NO_STATICDATA_HOOKS  BG3SE_NO_HOOK_LOAD
    BG3SE_NO_VIDEOSKIP         BG3SE_NO_HOOK_EVENT
    BG3SE_NO_NET               BG3SE_NO_HOOK_REGDIV
