# Wave Campaign Progress — Parity Closure + CLI Automation + Top-10 Vetting

Plan: `docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md`
Append-only. Each entry: date, wave/goal, action, evidence, verdict.

---

## 2026-07-28 — Wave 0: Truth & Baseline

**Housekeeping (Claude)** — DONE
- Committed campaign artifacts (wave plan, e2e review, CLI goal spec, May plans, progress.json) as `f701dc1`.
- `.gitignore` extended: `.screenshots/`, `.subdaimon-output/`, `__pycache__/`, `*.pyc`. Stale tracked `.pyc` removed.
- `gold.*.log` / `network.*.log` already covered by existing `*.log` rule.

**Goal 0.2 — osiris_call_by_id fail-closed (from Codex review 2026-04-02)** — ALREADY RESOLVED
- Verified `src/injector/main.c:2659-2670`: the unsafe `pfn_InternalCall` fallback is gone; function fails closed with explanatory comment ("InternalCall takes COsiParameterList*, NOT OsiArgumentDesc*"). Fixed between 2026-04-02 review and the May 16 commits. No work needed.

**Goal 0.1 — documentation reconciliation (codex docs profile)** — DONE (2026-07-28, ~126k tokens)
- ROADMAP.md, CLAUDE.md, README.md, agent_docs/api-status.md now tell one story: **v0.37.1**, overall **~94.7%** (unweighted mean of supported-surface matrix rows, source cited), honest per-namespace numbers (Types 9/15=60%, Math 57/59=96.6%, Level 15/21=71%, Audio 13/17=76%), Stats "function-count parity" + stub footnote, Entity stub footnote (incl. stubbed component writes), Ext.UI/Debugger moved outside the denominator with the scope-corrected exclusion statement (Ext.UI excluded; Debugger, replication, Virtual Textures, Input Injection deferred).
- Known residual for Wave 3: Ext.Localization still listed 100% (GetLanguage/CreateHandle) while the Windows baseline expects GetTranslatedString/UpdateTranslatedString — reconcile when Goal 3.2 implements them.

**Wave 0 exit gate** — ✅ PASSED (2026-07-28)
- Offline suites green (55+41+23+23), docs agree on numerator/denominator/exclusions, progress doc live, Goal 0.2 verified already-fixed. Wave 1 unblocked.

**Offline baseline gate** — GREEN (2026-07-28)
- `src/core/version.h` = **0.37.1** (ground truth for doc reconciliation).
- pytest tests/harness: **55 passed** (2.67s). tier0: **41/41**. tests_nexus: **OK**. tests_wiki: **OK**.
- No C/ObjC changes yet this wave, so no rebuild required for the baseline.

**Parallel research track: plasma-ai/fractal integration (user-requested 2026-07-28)** — COMPLETE
- Two subagent reports (architecture survey + insertion-point analysis) synthesized into `docs/plans/2026-07-28-002-feat-fractal-orchestration-integration-plan.md`.
- Verdicts: **ADOPT** Ghidra component-size extraction swarm as Wave 3 pilot Goal 3.4 (≥800/922 sizes, ≤$20, ≤4h, output confined to `ghidra/offsets/`, never builds/deploys/launches game); **DEFER** code-wave orchestration (until pilot verdict), vetting fan-out, crash bisection; **REJECT** live-game orchestration (tmux nodes lack GUI/Accessibility — origin plan's Claude-only rule confirmed structurally).
- Key findings: Claude Code native backend with hard budget caps (G2 pass); action space is LLM-mediated only — no deterministic exit-code gating (offline gates stay in setup.sh/Claude); branch-only worktree isolation leaves the shared dylib deploy target and single BG3 instance unprotected (design rule: fractal nodes never build or touch the game); Ghidra HTTP bridge is single-threaded, so swarm value is parse/recovery, not parallel queries.
