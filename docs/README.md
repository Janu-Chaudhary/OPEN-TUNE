# OpenTune — project catalogue

Where everything lives and what it is for. If a document is not listed here, it
either should be, or should not exist.

## Governing documents

| Document | Purpose | Changes when |
|---|---|---|
| `docs/PRD.md` | Why the product exists, for whom, success metrics, roadmap, risks | A product decision changes; owner-approved |
| `constitution.md` | Non-negotiable principles, with reasoning | Almost never; owner decision, recorded |
| `specs.md` | What v1 is: requirements, interfaces, acceptance criteria, open questions | A requirement or scope decision changes; flagged first |
| `tasks.md` | Ordered work with per-task done-criteria | Every task; living document |
| `CLAUDE.md` | How Claude works here: commands, layout, conventions, rituals | A convention proves wrong; kept under 100 lines |
| `engine/CLAUDE.md` | Real-time rules, loaded only inside `engine/` | A real-time rule is added or clarified |

## Records (this directory)

| Path | Purpose |
|---|---|
| `docs/decisions/` | Decision catalogue — reasoning, not just outcome. See its README for what earns a record |
| `docs/listening-log.md` | What human ears said at each checkpoint. Append-only. **Owner writes these; Claude cannot hear** |
| `docs/lessons.md` | Defects caught late or nearly missed, and the practice each one changed. Append-only |

### Decisions on record

| # | Decision |
|---|---|
| 0001 | doctest as the test framework |
| 0002 | dr_wav for WAV file I/O |
| 0003 | First-peak rather than global-max lag selection (inverts the octave bias — bears on AC7) |
| 0004 | Clamp the pitch ratio in the Engine, not the corrector (FR15 — the detector/corrector seam) |
| 0005 | Vendor Signalsmith Stretch and pull the real corrector into Stage 0 (resolves D5) |
| 0006 | AC4 is reachable — latency is a configuration knob; low-pitch resolution is the real limit |
| 0007 | What YIN's CMND actually buys — it does not break the octave tie; the threshold does |
| 0008 | AC7 fails and is fixable; AC2 is not measurable with algorithm-derived labels |
| 0009 | YIN's absolute threshold made relative — the fix for every octave error (D10) |
| 0010 | take04's AC7 excess is a real detector error; no step-3 rule fixes it without regressing synthetic (D12) |

## Guards

| Path | Purpose |
|---|---|
| `.githooks/pre-commit`, `pre-push` | Block wrong-identity commits and pushes at the git level |
| `.claude/settings.json` | Permission allow/deny lists and hook wiring for Claude Code |
| `.claude/hooks/guard-bash.py` | Denies `gh`, `--no-verify`, `github.com` remotes before they run |
| `.claude/hooks/guard-bash.test.sh` | Regression tests for the guard |
| `.claude/hooks/format-cpp.sh` | clang-format on files edited via Edit/Write |

## Agent tooling

| Path | Purpose |
|---|---|
| `skills-lock.json` | The tracked record of which agent skills this project uses — source and content hash each. Restore them with `npx skills install`. The skill bodies themselves live in `.agents/` and `.claude/skills/`, both gitignored: per-developer tooling, like `.venv` |

Currently pinned: **`find-skills`** (vercel-labs/skills, MIT) — searches the open skills
registry and recommends installable skills. Owner-requested 2026-09-04.
**Read any skill before use.** They run with full agent permissions, which makes them a
prompt-injection surface regardless of licence — a permissive licence is not a safety
property. `find-skills`' own Step 6 instructs installing further skills with `-g -y`
(global, no confirmation); that is not followed without asking the owner.

**Plugins** (installed at user level via `/plugin`, so nothing lands in this repo and there is
no redistribution question). Restore with:

```
/plugin marketplace add trailofbits/skills
/plugin install differential-review@trailofbits
/plugin install c-review@trailofbits
```

| Plugin | Use it for | Note |
|---|---|---|
| `differential-review` | Second opinion on a task diff: test coverage of *modified* code, blast radius by caller count, git-blame regression check | Its risk taxonomy is auth/crypto/value-transfer, none of which this codebase has — expect thin security findings and judge it on the coverage and regression phases |
| `c-review` | One-off memory-safety audit of `engine/` | 8–14 agents; needs `uv` on PATH. Worth running once YIN's buffer arithmetic exists, not before |

Both were installed as *skills* first, which silently produced a `c-review` that could not run:
`npx skills add` copies only `SKILL.md`, and the plugin's `workflows/` and `scripts/` never
arrived. If a skill's instructions reference files it does not ship, it is a plugin.
| `.claude/hooks/format-changed.sh` | clang-format on files changed by any Bash command — closes the gap the first hook leaves |

## Code (created as tasks complete)

| Path | Purpose | Stage |
|---|---|---|
| `engine/` | The product: pure DSP, platform-free | 0+ |
| `tests/` | All automated tests; `tests/support/Signals.h` generates known-answer input | 0+ |
| `tools/autotune-cli/` | WAV in → WAV out harness | 0 |
| `tools/analyze.py` | How Claude sees audio: pitch track + spectrogram PNG | 0 |
| `tools/d12_octave_evidence.py` | Adjudicates an octave dispute between a detector and the reference labels on spectral evidence independent of both, against a calibrated control (decision 0010) | 1 |
| `tools/autotune-live/` | Real-time microphone monitor | 3 |
| `apps/desktop/` | Minimal desktop GUI | 6 |
| `apps/android/` | Android app | 7 |
| `third_party/` | Vendored permissive dependencies, each with its LICENSE and VENDORED.md | as approved |

## Development environment

| Path | Purpose |
|---|---|
| `requirements-dev.txt` | Pinned Python deps for `tools/analyze.py`. Installed into `.venv/`, never system-wide |
| `.venv/` | Project virtualenv (gitignored). A user-local numpy 2.x shadows apt's, so a venv is required, not optional |
| `.clang-format` | LLVM base, 100 cols. Enforced by the `format-check` build target |

## Reading order for a newcomer

1. `docs/PRD.md` — why this exists and for whom
2. `constitution.md` — the rules and why
3. `specs.md` §1–§3, §7 — the shape of the answer
4. `tasks.md` — where we are
5. `docs/decisions/` — why things are the way they are

## Fresh clone setup

```bash
git config core.hooksPath .githooks   # REQUIRED - identity guards are inert without it
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt   # analysis tooling
npx skills install                    # agent skills pinned in skills-lock.json
```
