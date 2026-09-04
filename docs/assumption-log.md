# Assumption log

Every claim in this project that turned out to be wrong, what caught it, and the check that
would have caught it sooner. Separate from `docs/lessons.md`: that file records **defects in the
code** and the practices they changed. This one records **assertions made without evidence** —
mostly mine — because they have a different failure shape and a different cure.

It exists because the same four patterns kept recurring across a single day of work. If you are
picking this project up, read §1 and §2; the catalogue in §3 is the evidence they rest on.

---

## 1. The pre-flight checklist

Before asserting anything that will be written down, acted on, or reported to the owner:

**Before stating a mechanism** ("X fails because Y")
- [ ] Have I *run* it, or am I reasoning from documentation? Reasoning is a hypothesis.
- [ ] Does the claim survive a case chosen to break it, not just the case that suggested it?
- [ ] Am I generalising from a context where I verified it to one where I did not?

**Before believing a number**
- [ ] What is the instrument's resolution, and is it finer than the threshold I am testing?
- [ ] Have I run the instrument on a **known-good control** — ideally one already in the repo?
- [ ] Would this metric produce the same answer for a completely different reason?
- [ ] Does the number have a sign, a direction, or a region I have collapsed away?

**Before writing a guard, filter, or rule**
- [ ] What is the narrowest thing that expresses the intent?
- [ ] Does it block the *documentation of itself*? (This has happened twice.)
- [ ] Does it block a legitimate use that merely mentions the forbidden thing?

**Before reporting a result**
- [ ] Is the artefact I measured actually complete? (Existence ≠ completion.)
- [ ] Did the number improve because the thing improved, or because the denominator changed?
- [ ] Am I reporting a measurement, or a conclusion drawn from one? Label which.

---

## 2. The four recurring patterns

### A. Asserting a mechanism from reasoning, then planning around it
The most expensive pattern here, because a wrong mechanism sends the *fix* to the wrong place —
which costs more than a wrong conclusion. Seven instances (§3: 1, 2, 6, 7, 17, 18, 19).

The tell: a claim of the form "X happens because Y" where nobody has run X. The cure is cheap —
a throwaway script driving the real objects, printing numbers, takes minutes. Every time that was
done, it either confirmed the mechanism in one command or destroyed it in one command.

**Corollary that cost real time:** when someone corrects a conclusion, check their *mechanism*
too. Twice a correction arrived right about the conclusion and wrong about the cause.

### B. Trusting a measurement without validating the instrument
Eight instances (§3: 4, 5, 8, 9, 10, 11, 14, 15). This is `lessons.md` L5, and it recurred four
times *after* being written down — including inside the decision record that first stated it.

Sub-shapes seen:
- The instrument's resolution was the size of the quantity (label spread ~10 ¢ vs a ±15 ¢ bar).
- The metric measured something else entirely (a "gap/loud ratio" that measured silence fraction).
- The reader was wrong, not the data (32-bit float read as 16-bit int).
- The query was wrong, not the system (`ps` on the wrong PID → "the process is gone").
- The aggregate collapsed a sign (octave errors summed across two opposite directions).
- The input was truncated and the guard checked existence, not completeness.

**One check catches most of these: run the instrument on something whose answer you already
know.** In every case a suitable control was already sitting in the repository.

### C. Guards written broader than their intent
Two instances (§3: 12, 13), both mine, both costing multiple rounds to diagnose.

A path glob says "this file is untouchable" when the intent was "do not read these bytes into
context". A substring match on `--no-verify` blocks *writing the document that explains
`--no-verify`*. Pattern matching over a whole command or path expresses intent badly; matching
over *command position with the target's role understood* expresses it well.

### D. Generalising a verified finding beyond where it was verified
Two instances (§3: 16, 19). "Every octave error is octave-down" was true of the synthetic breathy
cases and false on real recordings, where octave-**high** is twice as common. "The takes sing
accurately" was asserted from a glance and wrong by a factor that mattered.

The tell is a universal quantifier — *every*, *all*, *none* — attached to a finding measured on
one dataset, one file, or one code path.

---

## 3. The catalogue

Chronological. "Caught by" matters: most were caught by someone else re-measuring, which is why
briefs on this project explicitly ask implementers and reviewers to report disagreement.

| # | The claim | What was true | Caught by | Check that would have caught it |
|---|---|---|---|---|
| 1 | `tasks.md`: "452 Hz → 466.16 Hz (A#4)" | The A4/A#4 boundary is 452.89 Hz, so 452 snaps *down* | T0.5 implementer, before coding | Evaluate the arithmetic once |
| 2 | FR15: a detector octave error requests ratio ≈ 2.0 | `snap(f)/f` is bounded to ±50 ¢ by construction; range 0.9715–1.0293 | T0.9 implementer | Compute the ratio's range |
| 3 | "The repository has been deleted" | The folder had been renamed | Owner | `ls` the parent directory |
| 4 | D6 output "decays to near-silence" | Full-scale block-rate staircase; the low RMS readings were nulls of an aliased envelope | T0.9 reviewer | Look at min/max per block, not just RMS |
| 5 | D6: "the sharp direction is stable" | It fails too, at 4.84 s, when the bounded history runs out | T0.9 reviewer | Run it longer than the test did |
| 6 | Decision 0003: weak/missing fundamental causes octave-high errors | It does not — surviving odd harmonics break periodicity at P/2. Needs *even-harmonics-only* | T1.2 implementer | Measure r(P/2) on the actual signals |
| 7 | `tasks.md` T1.4: "CMND is the step that kills octave errors" | It is not; the running mean plateaus so P and 2P tie exactly. T1.5's threshold breaks the tie | T1.3–T1.7 implementer | Assert the claim as a test |
| 8 | Decision 0008: velocity wrecks YIN's accuracy, 43% → 11% | The *labels* degrade with velocity; YIN measures 1.49 ¢ at 3500 ¢/s on exact labels | Synthetic set (conclusion), controller (mechanism) | Bin label spread by velocity |
| 9 | "Your recordings look like a dense mix, not solo vocals" | 32-bit float read as 16-bit int, manufacturing noise and halving duration | Controller, on the tell that six files reported identical RMS | Honour the fmt chunk; distrust identical numbers |
| 10 | "The labelling process is gone — that's the failure I warned about" | Alive at 96.6% CPU; the `ps` query used the wrong PID | Controller, on re-check | Query by the right handle before concluding |
| 11 | ref08 flagged as carrying accompaniment (gap/loud 0.092) | The metric measures silence fraction. The owner's own clean take scores 0.2887 by it | Fold-in agent, re-measuring | Run the metric on a known-good control |
| 12 | `Read(**/*.wav)` deny expresses "don't read audio into context" | It blocked `ls`, and every program taking a `.wav` argument | Repeated tool denials | Ask what the narrowest expression is |
| 13 | A `--no-verify` substring guard is safe | It blocked editing the documentation describing `--no-verify` | The guard firing during its own install | Test the guard against its own docs |
| 14 | AC7 = 3.83% after the D10 fix | 0.000%. `even_harmonics_only` is a behaviour pin — 2·f0 is *correct* there | Controller, on inspecting per-case rates | Know which cases are tests and which are pins |
| 15 | 12 dump files present, therefore complete | One was truncated mid-write by a timeout; `[ -f ] \|\|` skipped regenerating it | numpy refusing a malformed row | Compare row counts against a known-good pair |
| 16 | "Your takes sing accurately — a deliberately off-pitch take is the one thing nothing can substitute for" | Held notes are 15.7 ¢ off at the median, 23.5% beyond 30 ¢. The material was already correctable | Owner asking "will the 6 songs not work?" | Measure deviation before asserting it |
| 17 | D12 lead: octave-**low**, concentrated above 420 Hz | Octave-**high** dominates 6.97% vs 3.35%, concentrated at 260–320 Hz | D12 agent | Split the error rate by sign |
| 18 | D10: the threshold *fallback* is the sole mechanism | Half. At HNR 10 dB step 3 proper fires at the octave and never reaches the fallback | D10 implementer | Instrument which branch actually ran |
| 19 | "Every octave error is octave-down" | True of synthetic breathy cases; false on real recordings | D12 agent | Re-measure before generalising across datasets |

---

## 4. What actually works

Recorded because it is repeatable, not as consolation:

- **A throwaway probe driving the real objects.** Minutes to write, and it settled #2, #4, #5, #6,
  #8, #16 and #17 outright. Reasoning about DSP behaviour has a poor record here; running it has
  a perfect one.
- **Asking implementers and reviewers to report disagreement explicitly.** One sentence in a
  brief. It caught #1, #7, #11, #17 and #18 — five of nineteen — from people who could have
  quietly deferred to the brief instead.
- **Mutation testing every test.** Five test suites on this project passed on the null hypothesis
  and would have shipped as evidence of correctness.
- **Independent verification of a subagent's headline claim** before acting on it. Cheap, and it
  has changed the conclusion more than once.
- **Writing the correction down where the wrong claim lives**, not only in the commit message. A
  decision record that still asserts a refuted mechanism sends the next reader to the wrong place —
  see #18, where the wrong mechanism was already ruling out the right fix.
