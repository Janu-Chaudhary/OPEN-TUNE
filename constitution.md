# OpenTune Constitution

Non-negotiable principles. Conventions in `CLAUDE.md` may evolve; these do not.
Changing anything here is a deliberate decision by the project owner, recorded
with its reasoning.

---

## I. Permissive dependencies only

Every third-party dependency must be **MIT, BSD, Apache-2.0, ISC, or public domain**.

**GPL and LGPL are forbidden without exception.** This includes Rubber Band
(GPL/commercial), JUCE (GPL/commercial), and aubio.

**Why:** OpenTune's final license is deliberately undecided — it may end up
permissive, copyleft, or proprietary. A single GPL dependency permanently removes
the permissive and proprietary options, and blocks iOS distribution entirely.
Cost of obeying this rule today: near zero. Cost of discovering a violation in
year two: a rewrite.

Dependencies are **vendored** into `third_party/` and committed, so builds are
reproducible and the licence set is auditable at a glance. Each dependency's
folder carries its unmodified LICENSE file.

**No dependency is added without the owner's explicit approval.**

## II. The audio path is sacred

Code that runs on the audio callback thread must never:

- allocate or free memory (no `new`, `malloc`, `std::vector::push_back`, `std::string`)
- throw or catch exceptions
- take a lock, mutex, or any blocking primitive
- perform I/O of any kind, including logging
- call anything whose worst-case execution time is unbounded

The audio callback has a hard deadline. At 48 kHz with 256-sample blocks that is
**5.33 milliseconds**. Missing it produces an audible click, and the causes are
notoriously hard to trace after the fact.

Real-time functions are marked `noexcept`. Setup happens in `prepare()`, which runs
off the audio thread and may allocate freely. Parameter changes reach the audio
thread through lock-free atomics, never through locks.

## III. The engine is causal

The engine processes **fixed-size blocks in order and never reads future samples.**
This holds even in the offline CLI, where lookahead would be technically possible.

**Why:** it is what makes real-time operation possible. An engine written with
lookahead cannot be made real-time later without a rewrite. Obeying the constraint
from the first commit means "going real-time" is only a change of host.

*Analysis lag is not output latency.* The detector may examine a long trailing
window of already-received audio to estimate pitch; that costs accuracy in the
first moments of a note, not delay in the output.

## IV. The engine is pure

`engine/` depends only on the C++ standard library and vendored DSP code. No file
formats, no UI, no OS calls, no threads, no platform headers.

**Why:** it is what lets the same engine serve a CLI, a desktop app, an Android
app, and one day a plugin, without forking. It is also what lets a contributor
improve pitch correction without understanding anything else in the repository.

## V. Interfaces before implementations

Every replaceable part is an abstract interface with one clear job. Implementations
are chosen at construction, never by preprocessor branching.

The three core interfaces are `PitchDetector`, `ScaleQuantizer`, and `PitchCorrector`.
A naive implementation and a serious implementation of each must be able to coexist
and be swapped without touching any other file.

## VI. Tests come first

The failing test is written before the implementation, every time.

Audio makes this unusually practical: a synthetic 440 Hz sine is a known input with
a known correct answer. Quality regressions in DSP are otherwise close to invisible —
the ear adapts, and nobody notices a slow drift across forty commits.

Every merged change keeps the full suite green.

## VII. Quality is measured, not asserted

"Sounds good" is not a criterion. Every quality claim maps to a number that a test
can check — cents of error, milliseconds of latency, dropouts per run.

Measurements catch regressions. Listening catches what measurement misses. Both are
required; neither substitutes for the other.

## VIII. Prove the hard part first

The pitch correction engine is the only part of OpenTune that is hard and the only
part that is differentiating. Effects, harmony, beat libraries, and video export are
all worthless if the engine does not sound good, and all straightforward once it does.

Work that makes the product look finished is not permitted to precede work that
determines whether it can exist.

## IX. One identity

This repository belongs to exactly one GitHub identity:

**`Janu-Chaudhary <januchaudhary2004@gmail.com>`**, reached over SSH as
**`github-januchaudhary`**.

No other account may author, commit to, or push this repository.

**Why this needs a rule rather than good intentions:** a second GitHub account
(`janu-droid`, SSH key comment `hello@chardi.ai`) exists on this machine and is
the **default** — plain `github.com` resolves to it. Without enforcement, the
natural commands produce a split identity: commits authored by one account,
pushed by another.

Enforcement, in order of reliability:

1. **`.githooks/pre-commit`** blocks any commit whose author name or email is
   wrong, and any commit made while a remote points at plain `github.com`.
2. **`.githooks/pre-push`** blocks any push to a remote that is not the
   `github-januchaudhary` alias, and any push containing a commit with a foreign
   author email. This covers the case `pre-commit` cannot see: commits that
   already exist being pushed to a newly added remote.
   Both hooks verified blocking on 2026-09-03.
3. **Repo-local `user.name` / `user.email`** are pinned, so a change to the
   global config cannot leak in.
4. **Remotes must use `github-januchaudhary:owner/repo.git`.** Never
   `git@github.com:` and never an HTTPS URL.
5. **The `gh` CLI is forbidden in this repository.** It is authenticated as
   `janu-droid` and holds `repo` scope, so every `gh` command would act as the
   wrong account. Repository creation, pull requests, and releases are done by
   the owner in a browser, or after a second `gh` account is configured.

The hooks are committed to the repository rather than left in an untracked
`.git/hooks`, so they travel with the code. They are **not** automatically active
in a fresh clone: `core.hooksPath` lives in `.git/config`, which is never cloned.
Every clone must run:

```bash
git config core.hooksPath .githooks
```

Until that is run, the hooks exist but do nothing.

**These hooks are a guard against mistakes, not a security control.**
`git commit --no-verify` bypasses them at the shell. They exist to make the wrong
thing require deliberate effort.

6. **Claude Code is guarded separately.** `.claude/settings.json` denies `gh` as a
   permission rule, and a `PreToolUse` hook (`.claude/hooks/guard-bash.py`) rejects
   any command that invokes `gh`, passes `--no-verify` to git, adds a `github.com`
   remote, or sets a foreign `user.email` — before it runs. This binds the agent, not
   a human at a terminal. Regression tests: `.claude/hooks/guard-bash.test.sh`.

Commit signing is not yet configured; the author field is therefore assertable
but not cryptographically provable. Adding it is tracked as Q6 in `specs.md`.
