# Wave 2 Functor Hook Crash — Evidence Bundle (2026-07-29)

## Timeline
| Time | Session | Dylib | Outcome |
|------|---------|-------|---------|
| 13:39 | bg3se_2026-07-29_13-39-12.log | pre-Wave-2 runtime state: "Functor hooks SKIPPED (addresses verified for 6995620, detected 7209685)" | -continueGame engaged at +20s, Init→LoadSession→Running, tier tests ran. Last known-good full run. |
| 18:14 | bg3se_2026-07-29_18-14-20.log | Wave-2 build (commits a0bc648, 4bec7ae, a0f3ee6, 2fcd6a2): "Functor hooks: 10/10 installed" | Sat at main menu ~2.5 min. NO state transition out of Init, no InitGame/Load calls, no errors. User quit. |
| 18:22:30 | (session log rotated; crash 39s in) | Same Wave-2 build, 10/10 hooks installed | Save load progressed (server ticking statuses) then SIGSEGV at 18:23:09. |

## Crash (pid 25497, `~/Library/Logs/DiagnosticReports/Baldur's Gate 3-2026-07-29-182309.ips`)
- EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x6d0133ed6db63ebf ("possible pointer authentication failure") — garbage upper bits, classic corrupted-register deref.
- Faulting thread 25 (ServerWorker). Main binary base 0x10203c000 (slide 0x203c000).

Frames (slid → unslid):
```
0  ecs::EntityWorld::GetComponent<ls::TransformComponent const,true>(ls::ID<ecs::EntityHandleTraits>, ...)+64   0x1030f60a0 → 0x1010ba0a0
1  esv::functor::ExecuteStatsFunctors(eoc::StatsFunctorList const*, esv::functor::TargetContextData&)+140       0x1077b6908 → 0x10577a908
2  esv::Status::OnStatusEvent(eoc::EStatusEvent, eoc::HitDesc const&, eoc::AttackDesc const&, FixedString const&, ERemoveCause)+2768
3  esv::StatusBoost::OnStatusEvent(...)+36
4  esv::Status::OnStatusEvent(eoc::EStatusEvent)+232
5  esv::Status::Tick(ls::Guid const&, float, eoc::EStatusTickType)+992
6  esv::StatusBoost::Tick(...)+24
7  esv::StatusMachine::TickStartTurn(ls::Guid const&, float)+100
8  esv::status::ExecutionSystem::UpdateStatuses(...)+28700
9  ecs SystemUpdate<esv::status::ExecutionSystem> → SystemDependencyExecutor → EntityWorld::Update → ServerWorker::DoWork
```

**Unslid frame 1 = 0x10577a908 = ADDR_EXECUTE_FUNCTORS_TARGET (0x10577a87c) + 0x8c** — the crash PC is 35 instructions inside the function we inline-hooked (Target variant), i.e. executing the original body reached via the Dobby/arm64_hook trampoline, dying while fetching a component with a bad entity handle sourced from the context/registers.

## What changed since the last good run (13:39 → 18:14)
Only the dylib. Same game build 4.1.1.7209685, identical modsettings.lsx (mtime 13:22, 8 identical mods), same save.
Wave 2 commits in the new dylib:
- a0bc648 component property writes (passive unless called)
- 4bec7ae Ext.Stats honest implementations (passive unless called)
- **a0f3ee6 functor hooks re-enabled on 7209685** — 10 inline hooks NOW INSTALL (previously version-gated off): 8 ExecuteStatsFunctors wrappers + ExecuteStatsFunctor dispatcher? (verify which 10) + ProcessDealDamageFunctors
- 2fcd6a2 test registrations (Lua strings only)

## Suspects (unranked)
1. **ABI mismatch in a wrapper**: hook_ExecuteFunctors_Target assumes 2 params (StatsFunctorList const*, TargetContextData*). If the real Target variant takes a 3rd param (cf. Interrupt taking leading ecs::EntityWorld&), the wrapper→orig call drops/repositions args. Ground truth: scratchpad nm dump (13 demangled local symbols) — recheck the Target signature specifically.
2. **Dobby/arm64_hook prologue relocation bug**: if the first N instructions of the Target function include PC-relative instructions (ADRP/LDR-literal/B.cond) mis-relocated into the trampoline, registers go garbage exactly when orig executes. Crash at +0x8c using a handle loaded near function entry fits.
3. **Wrapper-side corruption before calling orig**: pre-event work (subscriber fast-path check, lua_gate, event fire) clobbering callee-saved or argument registers in a way the asm shim doesn't preserve.
4. Not the Lua event path itself firing wrong — no Lua frames in the crash stack.

## Open questions
- Why did the 18:14 run never leave Init (menu stall, -continueGame not engaging) while the 18:22 run reached status ticking? Same binary + args. Is the menu stall an independent flake, or did the first run's server thread die silently earlier (a swallowed crash on another thread would NOT stall the menu though)?
- Which of the 10 hooks were actually hit before the crash? (No BG3SE log entries for functor events observed — check the rotated session log for the 18:22 boot.)

## Addendum (the assistant, 18:4x): prologue byte analysis
Raw prologue decode of all 10 hooked functions (file offsets via fat 0xf558000):
- All 10 begin with plain STP saves (FP/SIMD `0x6d......` and GPR `0xa9......`) — **no PC-relative instructions in the first 6**, so trampoline relocation of the copied prologue is not inherently unsafe for these targets.
- **The fault address 0x6d0133ed6db63ebf is instruction bytes**: high word 0x6d0133ed == Interrupt variant's prologue instruction #2 exactly; low word 0x6db63ebf is an FP-STP encoding (Interrupt instr #1 is 0x6db63bef — 2 nibbles differ, so possibly a *different* function's FP-STP prologue with different registers). The "entity handle" the game dereferenced is the first 8 bytes of some function entry — i.e., a function pointer (hook target? trampoline? table entry?) was loaded as data along the Target execution path.
- Discriminating next step: disassemble ExecuteStatsFunctors(Target) +0x80..+0x90 to see which register/field supplies the handle passed to GetComponent, then trace where that value could alias a code pointer (our g_functor_hooks table? trampoline literal pool layout in the shared MAP_JIT page? off-by-one in per-hook trampoline slot allocation?).

## Addendum 2 (the assistant): faulting-thread registers — the decisive fact
- pc = GetComponent+64, lr = Target+0x8c (its caller).
- **x0 = 0x6d0133ed6db63bef = the EXACT first 8 bytes of the Interrupt variant's entry (0x105786548): instr0 0x6db63bef | instr1 0x6d0133ed.**
- x0 is the `this` (EntityWorld*) argument to GetComponent. Therefore the code loaded EntityWorld* by dereferencing a pointer whose VALUE was the (slid) Interrupt function address (0x1077c2548). I.e., an `EntityWorld**`-shaped slot somewhere contains the ADDRESS OF THE HOOKED INTERRUPT FUNCTION.
- x7 = ls::v_Unassigned<ls::ID<EntityHandleTraits>> (unassigned-handle sentinel); x1 = 1.
- Prime suspects: (a) our Interrupt wrapper's EntityWorld capture logic storing/aliasing the function address instead of the runtime x0 argument into a world-pointer global the game (or our entity system) publishes; (b) g_functor_hooks table layout confusion where a target-address field sits where consumer code expects a captured-world field; (c) install-time code writing ADDR_EXECUTE_FUNCTORS_INTERRUPT into the wrong global slot (e.g., an EntityWorld capture global). Grep functor_hooks.c/entity_system.c for any write of an ADDR_* or hook-target pointer into globals consumed as EntityWorld.
