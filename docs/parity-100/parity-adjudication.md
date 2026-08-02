I’ll treat this as an audit of both the published arithmetic and the semantics behind it. I’m using the BG3SE macOS reverse-engineering skill because the task explicitly compares the port with the Windows reference; I’ll first read its full instructions, then inspect every named document and trace disputed APIs into both trees.
The skill’s documented project paths are stale, but the active repo and the user-supplied Windows reference are unambiguous, so I’m using those. Its macOS constraint and Windows-architecture references are directly relevant to deciding what is engineering versus evidence-gated RE, so I’m reading those before classifying gaps.
The published 97.3 is mechanically reproducible: 25 scored rows, with 68.3–68⅓ total row-percentage points missing, divided by 25. The next issue is whether those seven discounted rows—and several nominally “100%” rows—represent behaviorally honest units, so I’m tracing each row’s stated residual gap and its corresponding implementation/Windows contract.
A material inconsistency has emerged: the canonical deferral registry names only Noesis and DAP as scope exclusions, while the roadmap front matter also removes entity replication, virtual textures, and input injection from the denominator. Moreover, the automated parity scan checks only whether function names exist—not whether calls work—so it necessarily awards credit to warn-and-return stubs. I’m now pinning the evidence-gated items to the existing RE reports and separating real Windows contracts from macOS additions.
## Answer

The published **97.3% is arithmetically correct for the matrix as written**, but it is not a defensible behavioral-parity number. It is a presence/count score over 25 unevenly defined rows, and several “100%” rows contain admitted stubs or substitute macOS-only functions for missing Windows contracts.

For a release called “100% parity of the supported macOS surface,” I recommend:

- Exclude only explicitly declared, permanent scope exclusions: **Ext.UI/Noesis** and **Lua Debugger/DAP**.
- Require every other supported Windows contract to be behaviorally functional, not merely registered.
- Do not let macOS-only additions offset missing Windows APIs.
- Treat compatibility stubs as zero credit.
- Publish “100% supported-surface parity; Ext.UI and DAP excluded,” never unqualified “100% Windows parity.”

## Evidence

### 1. Exact reconstruction of 97.3%

The roadmap defines the number as the unweighted mean of scored rows ([ROADMAP.md:3](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:3), [ROADMAP.md:7](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:7)). There are 25 numeric rows, 18 at 100%, and seven below 100%; Ext.UI and Debugger are unscored ([ROADMAP.md:37](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:37)).

| Scored row below 100 | Matrix score | Row deficit | Contribution to overall missing percentage |
|---|---:|---:|---:|
| `Osi.*` | 95% | 5 | `5 / 25 = 0.200` |
| `Ext.Types` | `13/15 = 86.666…%` | `13.333…` | `13.333… / 25 = 0.5333` |
| `Ext.Net` | 95% | 5 | `0.200` |
| `Ext.Level` | 80% | 20 | `0.800` |
| Console/REPL | 95% | 5 | `0.200` |
| PersistentVars | 90% | 10 | `0.400` |
| Client Lua State | 90% | 10 | `0.400` |
| **Total** | | **68.333…** | **2.7333** |

Therefore:

```text
(18×100 + 95 + 86⅔ + 95 + 80 + 95 + 90 + 90) / 25
= 2431⅔ / 25
= 97.2666…%
≈ 97.3%
```

The displayed 86.7% produces 97.268%; the exact 13/15 fraction produces 97.2667%. Both round to 97.3%.

### 2. What the seven discounted rows actually mean

- `Osi.*`: the remaining stated work is filtered database queries and row deletion, currently “needs verification” ([ROADMAP.md:96](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:96)).
- `Ext.Types`: `Construct` and `GetHashSetValueAt` are the two published deferrals ([ROADMAP.md:49](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:49)).
- `Ext.Level`: five explicit deferrals account exactly for 20/25: three raycasts, tile debug info, and pathfinding creation ([docs/deferrals.md:16](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:16)).
- Console: the named missing behavior is client/server context switching ([ROADMAP.md:830](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:830)).
- PersistentVars: persistence is global JSON files, not save-specific extender data ([ROADMAP.md:417](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:417)). Windows serializes persistent, user, mod, and timer data through the savegame visitor ([SavegameSerializer.inl:31](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Extender/Shared/SavegameSerializer.inl:31), [SavegameSerializer.inl:51](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Extender/Shared/SavegameSerializer.inl:51)).
- Client Lua State: macOS intentionally uses one Lua VM with sequential context labels, not separate states ([ROADMAP.md:567](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:567)). Windows owns distinct client and server `unique_ptr` states ([ExtensionStateClient.h:23](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Extender/Client/ExtensionStateClient.h:23), [ExtensionStateServer.h:42](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Extender/Server/ExtensionStateServer.h:42)).
- `Ext.Net`: the matrix supplies no definition for its missing 5%. The roadmap simultaneously says full transport is complete while listing legacy networking and state synchronization as not started ([ROADMAP.md:883](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:883), [ROADMAP.md:998](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:998), [ROADMAP.md:1013](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:1013)). This row must first name its denominator.

## Details

### Three tiers

#### Tier 1 — closable by engineering without new instruction-level RE

- Finish and test `Osi.DB_*` filtered retrieval and deletion using the already recovered Osiris/database machinery.
- Generate the build-gated `TypeId → RemoveComponent<T>` dispatch table. The RE report already identifies this as one safe unlock route; the work remaining is table generation, version gating, and testing ([COMPONENT_OPS_AND_PROTO_INIT.md:307](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:307)).
- Implement entity tracing as a port-owned change log over the existing component event infrastructure. The Windows contract is only enable/disable, retrieve, and clear the log ([Entity.inl:243](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Entity.inl:243)).
- Implement `AddCustomFunction` and `AddCustomProperty` in the macOS proxy/property overlay. Windows implements these as Lua-side custom-property registration, not an engine call ([Types.inl:328](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Types.inl:328)).
- Build true dual Lua VM ownership, bootstrap isolation, and console switching. Windows’s console explicitly switches between client and server states ([Console.cpp:36](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Extender/Shared/Console.cpp:36)).
- Supply legacy networking aliases over the completed transport, if that is what the unexplained `Ext.Net` 5% represents.
- Input injection can likely be implemented with the existing macOS event infrastructure; merely providing capture and hotkeys is not semantic parity with `InjectKey*`.

#### Tier 2 — requires instruction-level RE evidence

- All five `Ext.Level` deferrals:
  - Raycasts require proof of by-value `ls::Function`/`ls::Optional` representation and lifecycle. The existing audit shows that correct slots alone are insufficient because the ABI shapes differ materially ([PHYSICS_VMT_AUDIT.md:234](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/PHYSICS_VMT_AUDIT.md:234)).
  - `GetTileDebugInfo` needs the open minimum-height load and flag conversions.
  - `BeginPathfinding` needs the full character-path population sequence ([AIGRID_PATHFINDING.md:400](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:400), [AIGRID_PATHFINDING.md:448](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:448)).
- Stats modifier-list mutation: `AddAttribute` lacks a proven allocator and modifier construction ABI; `AddEnumerationValue` lacks map allocation/insertion evidence ([lua_stats.c:782](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:782)).
- `ExecuteFunctors`: despite the “partial” label, the public binding currently has no usable functor/list object to invoke and returns without executing ([lua_stats.c:1091](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:1091)).
- Passive and interrupt prototype sync. Both loader population paths remain inlined and unmapped ([COMPONENT_OPS_AND_PROTO_INIT.md:537](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:537), [prototype_managers.c:797](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/prototype_managers.c:797)).
- Entity replication flags and `Replicate()`.
- Additional native component writes for unverified layouts and field types.
- `GetHashSetValueAt`, because the port lacks proven element size, hashing, and occupancy metadata ([lua_ext.c:1051](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ext.c:1051)).
- Savegame-integrated persistence; the roadmap itself says replacing file persistence with save hooks requires extensive RE ([ROADMAP.md:424](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:424)).
- Virtual Textures, unless a platform-independent interception route is found.

`Ext.Types.Construct` is special: the Windows reference itself validates the type and then ends with `// TODO; return 0` ([Types.inl:286](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Types.inl:286)). It should be documented as a nonfunctional reference contract and removed from the “missing Windows behavior” calculation. Implementing arbitrary construction would be a new capability requiring per-type allocator/constructor/destructor evidence.

#### Tier 3 — scope exclusions

The canonical registry lists exactly:

- `Ext.UI`/Noesis.
- Lua Debugger/DAP.

([docs/deferrals.md:65](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:65))

These should not count against “parity of the supported macOS surface,” provided that:

1. They are labeled **permanently excluded from this release profile**, not ambiguously “deferred.”
2. The release never calls itself unqualified “full Windows parity.”
3. The excluded contracts remain visible in a separate coverage/exclusions list.

The roadmap’s front matter also excludes replication, Virtual Textures, and input injection ([ROADMAP.md:11](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:11)), but the canonical registry does not. My recommendation is not to silently create three more exclusions: replication and input are behaviors inside rows scored as 100%, and Virtual Textures should receive its own row. Either implement them or make a new, explicit scope decision before release.

### Silent inconsistencies

1. **Entity is not behaviorally 100%.**  
   The matrix calls it 100%, but its own footnote admits three table stubs, a no-op `Replicate`, false-returning `RemoveComponent`, and partial writes ([ROADMAP.md:43](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:43), [ROADMAP.md:70](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:70)). The implementations confirm this ([main.c:1111](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c:1111), [entity_system.c:1642](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c:1642), [entity_system.c:1862](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c:1862)).

   More seriously, function-count substitution hides missing Windows functions. Windows registers `HandleToUuid`, `UuidToHandle`, entity creation/destruction, system-update subscriptions, trace retrieval/clearing, and component-type listing ([Entity.inl:291](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Entity.inl:291)). macOS instead registers many diagnostic/discovery additions ([entity_system.c:2809](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c:2809)). Thirty different functions do not equal parity with 26 reference contracts.

2. **Stats is “100% function-count parity,” not 100% functionality.**  
   The footnotes concede `AddAttribute`, `AddEnumerationValue`, `ExecuteFunctors`, passive sync, and interrupt sync ([CLAUDE.md:177](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/CLAUDE.md:177)). The Windows versions perform actual allocation/map insertion ([Stats.inl:707](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Stats.inl:707)); macOS returns false/nil ([lua_stats.c:782](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:782)).

3. **Types uses a manufactured denominator.**  
   Windows registers 14 functions, including functional `AddCustomFunction` and `AddCustomProperty` ([Types.inl:369](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Types.inl:369)). The matrix calls the baseline 15, adds three macOS-specific helpers, and excludes the two real Windows custom-property functions. Both are registered but false-returning on macOS ([lua_ext.c:1089](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ext.c:1089)).

4. **The parity scanner awards stubs full credit.**  
   It enumerates functions and compares names only ([parity.py:81](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/tools/bg3se_harness/parity.py:81), [parity.py:120](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/tools/bg3se_harness/parity.py:120)). It cannot distinguish a working API from warn-and-return-false.

5. **Scope doctrine conflicts internally.**  
   The registry’s exclusion section names only UI and DAP, while the roadmap also removes replication, Virtual Textures, and input injection. Entity replication nevertheless appears inside the deferral registry rather than the exclusion section ([docs/deferrals.md:31](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:31)).

6. **Other nominally completed rows contain residual gaps.**
   - `Ext.Input` is 100% while injection is excluded.
   - `Ext.Vars` is 100% while client/server sync is listed as missing ([ROADMAP.md:564](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:564)).
   - `Ext.IMGUI` is 100% while fonts, textures, and style enums remain ([ROADMAP.md:1231](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:1231)).
   - The detailed StaticData section still says ~85% and calls multi-type verification incomplete while the matrix says 100% ([ROADMAP.md:1317](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:1317)). This may be stale documentation, but a 100% release must resolve it.

### Would honest accounting move 97.3%?

Yes—downward.

There is no single exact replacement until the project publishes a matched contract inventory. As a conservative illustration using only admitted stubs:

- Entity: five explicitly nonfunctional items among the claimed 30 gives at most `25/30 = 83.3%`.
- Stats: counting only the three directly registered stub/partial entry points gives at most `49/52 = 94.2%`; treating passive and interrupt sync as additional claimed functional units lowers it further.
- Types: adding the two real Windows custom-property contracts to the current 15-unit denominator gives `13/17 = 76.5%`.

Substituting just those three illustrative corrections gives:

```text
97.2667
- (100 - 83.3333)/25
- (100 - 94.2308)/25
- (86.6667 - 76.4706)/25
= approximately 95.96%
```

Thus **~96.0% is a plausible admitted-gap upper bound**, not a new authoritative score. A strict matched-contract audit would probably be lower because Entity function substitution, input injection, variable sync, and IMGUI residuals remain unpriced.

## Related

### Definition of done for “100% supported-surface parity”

A release is done when:

1. A versioned manifest lists each Windows reference contract, context, inputs, outputs, side effects, failure behavior, and macOS disposition.
2. Ext.UI and DAP are marked permanent exclusions with no percentage.
3. Every other deferral converts:
   - Five `Ext.Level` APIs.
   - Entity removal, tracing, replication, and supported property writes.
   - Stats mutation, real functor execution, and passive/interrupt sync.
   - `GetHashSetValueAt` and both custom-property APIs.
4. `Construct` is documented as nonfunctional in the Windows reference, rather than charged as a macOS parity failure.
5. Persistent variables, user/mod variables, and persistent timers follow save-slot semantics.
6. Client and server Lua state have real isolation, lifecycle, and console selection.
7. `Ext.Net` defines and closes its actual remaining 5%.
8. Input injection and Virtual Textures are either implemented or subjected to explicit new scope decisions; they cannot remain silently outside the denominator.
9. Conformance tests invoke behavior. Presence-only tests do not award parity credit.
10. The final matrix shows every scored row at 100%, while excluded rows show `— Permanent scope exclusion`.

### Concrete path from the published 97.3 to 100

Under the existing matrix arithmetic, the nominal path is:

1. Complete `Osi.*` database semantics: **+0.200**
2. Complete `Ext.Types` 15-unit row: **+0.533**
3. Define and close the `Ext.Net` residual: **+0.200**
4. Convert all five `Ext.Level` deferrals: **+0.800**
5. Add console client/server context switching: **+0.200**
6. Replace file-global PersistentVars with savegame semantics: **+0.400**
7. Implement true client/server Lua-state parity: **+0.400**

Total: **+2.733 points**, taking 97.2667% to 100%.

For an honest release, precede that ladder with the known-stub rebaseline to approximately 95.96%, then additionally recover at least:

8. Entity admitted stubs: **+0.667**
9. Stats admitted public stubs: **+0.231**
10. The two improperly excluded Windows Types contracts: an additional **+0.408** beyond item 2

That corrected minimum ladder totals approximately **+4.039 points from 95.96 to 100**. Input injection, Vars synchronization, IMGUI residuals, and any strict Entity contract mismatches are zero-delta release gates only because the present matrix wrongly awards or omits their points; they still must be resolved before the claim is honest.
