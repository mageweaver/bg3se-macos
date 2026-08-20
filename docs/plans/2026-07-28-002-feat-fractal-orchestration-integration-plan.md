---
title: "feat: Weave plasma-ai/fractal into the parity/CLI/vetting wave campaign"
type: feat
status: active
date: 2026-07-28
origin: docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md
---

# Fractal Orchestration Integration — Campaign Amendment

Amendment to the approved 7-wave campaign (origin plan). Two research passes over `plasma-ai/fractal` (architecture survey + insertion-point analysis, both 2026-07-28, main branch) are synthesized here into per-insertion-point verdicts. **Fractal does not replace the wave structure, exit gates, or the assistant-as-gatekeeper protocol — it is adopted as an execution substrate for exactly one goal shape (pilot), with promotion contingent on pilot results.**

## What Fractal is (survey findings)

v1.0.0, Apache-2.0, PyPI `plasma-fractal`, Python ≥3.12, created 2026-07-01, active CI/tests/Sphinx docs. A **tree of autonomous agent nodes rooted in a git repo**: each node owns a git worktree on a dotted branch, a tmux session, and an iteration Loop walking numbered markdown step files (`00-PREPARE` → `04-COMMIT`), spawning an agent-CLI subprocess per step (the CLI / Codex / Grok / OpenCode / OMP). Shared SQLite DB tracks runs/iters/steps/costs/signals/radio messages. Nodes spawn children at runtime bounded by `max_depth`/`max_children`/`max_descendants`/`max_cost`/timeouts, with budget-reserve cascade, first-class pause/resume (survives reboots; paused time credited against deadlines), and pub/sub "radio" messaging (parents auto-subscribe to children). Observability: SQLite `activity` view, Textual TUI (`fractal open`), CLI.

Key mechanics that shaped the verdicts (integrator findings, file-cited in the research reports):
- **Action space is LLM-mediated**: fractal has no deterministic "run command, gate on exit code" primitive — every step is an agent invocation interpreting a markdown prompt (`fractal/core/loop.py:2055`, `agent.py:543`). The only shell hook is `scripts/setup.sh` (pre-iteration). Success criteria are natural-language Completion Requirements in `NODE.md`, LLM-interpreted.
- **Evaluation is LLM-driven**: parents check children via `fractal node list` + radio + merge; no programmatic JSON/exit-code gating exists.
- **Durability is genuinely strong**: pause/resume mid-step with frozen dirty worktree, run rows re-adopted, sessions resumed — far more durable than codex one-shots for multi-day work.
- **Cost control is real**: per-node/per-step model+effort overrides, subtree-shared `max_cost`, per-step `--max-budget-usd` hard-enforced for the the assistant backend, all caps live-retunable.
- **No sandbox**: worktree isolates the branch, not filesystem/network; node agents run with `bypassPermissions`/`danger-full-access`. Same trust posture as our codex-exec, but unattended for hours — mitigate with tight caps.

## Adoption gates — results

- **G1 Capability: PASS (qualified)** — nodes can run `bg3se_harness`/`cmake`/`curl` via their agent's Bash tool, but only LLM-mediated; deterministic gates belong in `setup.sh`, not steps.
- **G2 Anthropic backend: PASS** — native the CLI backend with enforced per-step budget caps; per-step model frontmatter (haiku for mechanical children).
- **G3 Serialization: PARTIAL** — no external-resource locks. **Design rule adopted: fractal nodes never build+deploy the dylib and never launch BG3.** Live-game work stays the assistant-only per the origin protocol. (Misfits: single BG3 instance + Accessibility perms + one `/tmp/bg3se.sock`; shared Steam-folder deploy target races under parallel `cmake --build`.)
- **G4 Net win: PASS for exactly one shape** — see verdicts.
- **G5 Blast radius: PASS with caps** — repo-scoped work, `max_cost`/`timeout`/`max_descendants` mandatory on every node, pinned `plasma-fractal==1.0.0`.

## Verdicts

| Insertion point | Verdict | Rationale |
|---|---|---|
| **Ghidra batch component-size extraction** (922 missing sizes; ecl:: 351, esv:: 298, ls:: 118, eoc:: 126) | **ADOPT — pilot** | Fractal's sweet spot: no game, no GPU, no Accessibility; disjoint per-namespace staging files; wiki/plan memory + pause/resume fit a multi-day grind; replaces the ad-hoc parallel-agent workflow in `agent_docs/development.md`. Caveat: the Ghidra HTTP bridge is single-threaded, so curls serialize at the bridge — parallelism value is in decompile-output parsing and error recovery, not raw fan-out; cap children accordingly (≤4). |
| Code-only wave orchestration (Waves 2/3/6 goals replacing codex-exec) | **DEFER until pilot verdict** | Integrator argued ADOPT-with-caveats (iteration structure + durability + cost caps beat one-shots). Gate G4 says the incumbent codex-exec pattern already works and is approved; running two orchestrators doubles failure surface. If the pilot lands ≥800 verified sizes within budget with clean merges, promote fractal to Wave 3's small-gap sweep (Goal 3.2) as the second adoption. |
| Mod-vetting matrix fan-out (Wave 5) | **DEFER** | Vet analysis could fan out, but the core value — autonomous vetting — breaks on the single-game-instance constraint; every child would block on the one the assistant-mediated live game. Revisit only if an "external executor" pattern emerges. |
| Crash bisection (Goal 1.1) | **DEFER (effectively reject)** | Sequential search over live-game launches; fractal adds durability but zero parallelism; the assistant driving the harness directly is simpler and equally capable at n=14 (~4 probes). |
| Whole-campaign / live-game orchestration | **REJECT** | Confirmed structural misfit: tmux nodes have no GUI/Accessibility access; origin plan's "live validation is the assistant-only" rule stands. |

## Pilot: Goal 3.4 — Fractal Ghidra size-extraction swarm (added to Wave 3)

Prereqs: Ghidra running with the Ghidra HTTP bridge on :8080 (BG3 ARM64 binary analyzed); `uv tool install plasma-fractal` (pin 1.0.0); `fractal init` on a dedicated branch point.

Run definition (parent node; integrator's sketch, adopted):

```bash
cd /Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos
fractal init
fractal node init ghidra_sizes \
  --agent claude --model claude-sonnet-5 \
  --max-iters 5 --max-children 4 --max-descendants 8 \
  --max-cost 20 --timeout 4h
fractal node start ghidra_sizes && fractal open   # TUI monitor
```

Parent `NODE.md`: spawn one child per namespace batch (`ecl_batch` 351, `esv_batch` 298, `ls_batch` 118, `eoc_gaps` 126), children on `claude-haiku-4-5` with `--max-cost 4 --max-iters 10 --timeout 1h`; each decompiles `AddComponent<T>` via `curl http://127.0.0.1:8080/decompile_function`, parses the second arg of `ComponentFrameStorageAllocRaw`, writes `ghidra/offsets/staging/<namespace>.md`; parent merges branches and consolidates into `COMPONENT_SIZES_{ECL,ESV,LS,EOC_CORE}.md` with a FAILURES section per doc. Completion requirement: ≥800 of 922 extracted, staging merged, failures listed.

**Constraints stamped into NODE.md (non-negotiable):** never run `cmake --build` or any deploy script; never launch or signal BG3; never touch `src/` — output is documentation tables only. the assistant (this session) verifies the merged tables (spot-check ≥20 sizes against direct bridge queries) and only then feeds them into the generated-registry pipeline in a separate, the assistant-gated step.

**Pilot success/failure criteria:** ≥800 sizes, ≤$20, ≤4h wall-clock, zero commits outside `ghidra/offsets/`, spot-check accuracy ≥95%. Failure on any → fractal stays single-shape or is dropped; verdict recorded in the progress doc either way.

## Acceptance criteria

- [x] Both research reports synthesized with verdicts (this document).
- [ ] Pilot Goal 3.4 executed during Wave 3; verdict recorded in `docs/bugs/wave-campaign-progress.md`.
- [ ] If pilot passes: promotion decision for Goal 3.2 recorded; origin plan's Orchestration Protocol gains one bullet ("fractal = subordinate executor for offline fan-out shapes; never builds, deploys, or touches the live game").
- [ ] If pilot fails: verdict + rationale recorded; question closed for this campaign.

## Risks

- Stealth-startup optics: fractal is public Apache-2.0; Tom joins Plasma AI August 2026 — adoption doubles as employer-stack familiarization; legitimate side benefit, but G4 ruled, not sentiment.
- Young project (v1.0.0, 2026-07-01): pin `plasma-fractal==1.0.0`; TUI/CLI churn expected.
- LLM-overhead-per-work-unit: 5 agent steps per iteration; acceptable for extraction (error recovery has value), unacceptable for deterministic pipelines — keep offline gates in `setup.sh`/the assistant.
- Unattended `bypassPermissions` nodes: mandatory caps on every node; work product confined to `ghidra/offsets/`.

## Sources

- **Origin:** `docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md` — wave structure, exit gates, the assistant-only live-game rule (all unchanged).
- `https://github.com/plasma-ai/fractal` @ main 2026-07-28 — architecture survey (core/{node,loop,agent}.py, impl/claude.py, schema.sql, _node/NODE.md, _scripts/) + insertion analysis (wiki/features/{loop,cost,spawning}, wiki/design/durability.md); full reports in session record 2026-07-28.
- `agent_docs/development.md` — incumbent Ghidra parallel-agent workflow (replaced by pilot if it passes).
- `docs/plans/2026-05-02-001-feat-systematic-top5-mod-vetting-plan.md` — vetting shapes (deferred insertion).
