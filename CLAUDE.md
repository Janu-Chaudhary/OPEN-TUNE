# OpenTune

Real-time vocal pitch correction engine (C++17) plus thin app shells.
The engine is the product; apps are hosts around it.

**Read `constitution.md` before writing any code.** Those rules are non-negotiable.
`specs.md` = what we're building. `tasks.md` = what's next.

## Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug     # configure
cmake --build build -j                       # build
ctest --test-dir build --output-on-failure   # run tests
cmake --build build --target format-check   # fails on unformatted code; run before commit
./build/tools/autotune-cli/opentune-cli in.wav out.wav --key C:major --strength 0.8
./build/tools/autotune-live/opentune-live    # real-time monitor (Stage 3+)
```

## Layout

| Path | Contains | May depend on |
|---|---|---|
| `engine/` | The product. Pure DSP. | Nothing but the C++ stdlib + vendored DSP libs |
| `tools/autotune-cli/` | WAV in → WAV out test harness | engine, dr_wav |
| `tools/autotune-live/` | Real-time mic monitor | engine, miniaudio |
| `apps/desktop/` | Minimal desktop GUI | engine |
| `apps/android/` | Android app | engine via JNI |
| `tests/` | All automated tests | engine |
| `third_party/` | Vendored deps (committed) | — |

`engine/` must never include a file-format, UI, threading, or platform header.
If you need one, you are writing host code — put it in `tools/` or `apps/`.

## Conventions

- **C++17**, CMake. `namespace opentune`.
- Classes `PascalCase`, functions/variables `camelCase`, members `m_camelCase`,
  constants `kPascalCase`, files match their primary class name.
- Interfaces are abstract classes in `engine/include/opentune/`; implementations
  live in `engine/src/` and are chosen at construction. Never `#ifdef` between them.
- Comment heavily — this repo is meant to be learnable. DSP code explains the
  algorithm and why each magic number has its value, with a source link where one exists.
- Format with `clang-format` (LLVM style, 100 cols) before every commit.

## Session start

Read `tasks.md`. Name the first `[ ]` task, restate its **Done when**, and wait for a go.
Do not read the whole repo. One task per session; `/clear` after each commit.

## Workflow

1. Pick the next unchecked task in `tasks.md`. Work on one task at a time.
2. **Write the failing test first.** No implementation before a red test.
3. Implement until green. Run the full suite.
4. Show the diff and stop for review before committing.
5. On approval: commit with a conventional message, tick the task in `tasks.md`.

Commit format: `type(scope): summary` — e.g. `feat(detector): add YIN pitch detection`.
Types: `feat` `fix` `test` `refactor` `docs` `build` `chore`.

## Audio and verification

- **Never read audio files.** `.wav` is binary; it poisons context. Inspect audio only
  through `.venv/bin/python tools/analyze.py` — pitch track, cents error, and a spectrogram PNG,
  which you can view. You cannot hear; the owner does the listening.
- **Never claim a test passes without pasting the `ctest` summary line.** Never claim
  audio is correct without a number. Constitution VII.
- Cap output: `cmake --build build 2>&1 | tail -30`, `ctest --output-on-failure`. Never `-V`.
- Resolved open questions get a 20-line record in `docs/decisions/`. Listening
  checkpoints go in `docs/listening-log.md`.
- `engine/CLAUDE.md` holds the real-time rules and loads when you work there.

## Working with me (the human)

- I am new to DSP and to real-time audio. Explain the **audio concepts** — what an
  algorithm does and why it works. Do **not** explain ordinary C++ or CMake.
- Never add a third-party dependency without asking first.
- Never change `specs.md` scope silently. Flag it, get a decision, then edit.
- Keep `tasks.md` current: tick what's done, add what we discover.

## Non-negotiables (full text in `constitution.md`)

1. Permissive licenses only (MIT/BSD/Apache/public domain). No GPL or LGPL, ever.
2. The audio path never allocates, throws, locks, logs, or blocks.
3. The engine is causal and block-based — it never reads future samples.
4. Tests before implementation.
5. `engine/` stays platform-free.
6. **One identity only:** `Janu-Chaudhary <januchaudhary2004@gmail.com>`.
   Remotes use `github-januchaudhary:`, never `github.com:`.
   **Never run `gh`** — it is authenticated as a different account. A `PreToolUse`
   hook in `.claude/settings.json` denies it, `--no-verify`, and `github.com` remotes.

## Fresh clone setup

```bash
git config core.hooksPath .githooks   # REQUIRED - identity guards are inert without it
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt   # analysis tooling
```
