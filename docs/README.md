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
| `docs/decisions/` | Decision catalogue. One file per resolved open question; reasoning, not just outcome |
| `docs/listening-log.md` | What human ears said at each checkpoint. Append-only |

## Guards

| Path | Purpose |
|---|---|
| `.githooks/pre-commit`, `pre-push` | Block wrong-identity commits and pushes at the git level |
| `.claude/settings.json` | Permission allow/deny lists and hook wiring for Claude Code |
| `.claude/hooks/guard-bash.py` | Denies `gh`, `--no-verify`, `github.com` remotes before they run |
| `.claude/hooks/guard-bash.test.sh` | Regression tests for the guard |
| `.claude/hooks/format-cpp.sh` | clang-format on every edited C++ file |

## Code (created as tasks complete)

| Path | Purpose | Stage |
|---|---|---|
| `engine/` | The product: pure DSP, platform-free | 0+ |
| `tests/` | All automated tests; `tests/support/Signals.h` generates known-answer input | 0+ |
| `tools/autotune-cli/` | WAV in → WAV out harness | 0 |
| `tools/analyze.py` | How Claude sees audio: pitch track + spectrogram PNG | 0 |
| `tools/autotune-live/` | Real-time microphone monitor | 3 |
| `apps/desktop/` | Minimal desktop GUI | 6 |
| `apps/android/` | Android app | 7 |
| `third_party/` | Vendored permissive dependencies, each with its LICENSE | as approved |

## Reading order for a newcomer

1. `docs/PRD.md` — why this exists and for whom
2. `constitution.md` — the rules and why
3. `specs.md` §1–§3, §7 — the shape of the answer
4. `tasks.md` — where we are
5. `docs/decisions/` — why things are the way they are
