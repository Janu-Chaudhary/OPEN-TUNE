# Lessons

Defects and near-misses that changed how we work. Append-only, newest last.

A record earns a place here when something was **caught late, or nearly missed** — and the
answer is a change of practice, not just a fix. The fix belongs in the code; the reason
belongs here. Decisions go in `docs/decisions/` instead; this file is for how we were wrong.

Format: what happened · why it was missed · what changed.

---

## L1 — A worked example in the plan was arithmetically false
**Found:** T0.5 (`ScaleQuantizer`), by the implementer, before writing any code.

`tasks.md` specified "452 Hz → 466.16 Hz (A#4)" as a done-criterion. It is wrong. The
A4/A#4 decision boundary is MIDI 69.5 = 452.89 Hz, so 452 Hz snaps *down* to 440 Hz. The
implementer refused to code against it and escalated; the arithmetic was verified
independently and the plan corrected to 455 Hz before implementation began.

**Why it was missed:** the number was written by hand, read plausibly, and was never
checked. A spec is not self-verifying just because it is precise.

**What changed:** hand-written worked examples in `tasks.md` are treated as fallible.
Implementers are briefed to verify a criterion's arithmetic before coding against it and to
stop rather than work around it. Had this one been implemented as written, the test would
have encoded the error and every later test would have inherited it.

---

## L2 — A confident wrong answer inside the declared range
**Found:** T0.4 (`AutocorrelationDetector`), by an independent reviewer, after the task had
been implemented and self-tested green.

`minLag` mapped to scratch index 0, but the peak search started at index 1 — so the shortest
lag in the declared range could never be selected. A 1090 Hz tone, comfortably inside the
declared 65–1100 Hz range, was reported as 545 Hz: exactly one octave low, with full
confidence. Separately, `lround` had left the range ceiling 9.1 Hz short of its own
declaration.

**Why it was missed:** every test sat at 110/220/440/880 Hz. Nothing tested the *endpoints*
of the range the component itself claimed to cover, so an entire band above 880 Hz was
unexercised. The failure mode was not a crash or an obvious wrong number — it was a
plausible pitch, one octave out.

**What changed:** a component that declares a working range must be tested **at both
endpoints of that range**, not only in its comfortable middle. Regression tests now sit at
70 Hz and 1090 Hz. More generally: in DSP the dangerous failure is not the crash, it is the
answer that looks reasonable.

---

## L3 — The defect that per-task review cannot see
**Found:** during T0.7, by reasoning across two already-approved tasks — not by any review of
either one.

`ResampleCorrector` misbehaves above pitch ratio 1.0. `AutocorrelationDetector`'s octave
errors lean high. Each was reviewed independently and each was approved, correctly. The
interaction between them was invisible to both reviews.

**Why it was missed:** it was not missed by anyone at fault. The defect lives at the **seam**
between two components and belongs to neither task that created them. Per-task review is
blind to it by construction — a reviewer reading the corrector has no reason to read the
detector's failure modes, and vice versa.

**What changed:** two things.
1. Recorded as **FR15** in `specs.md` and made the centre of T0.9, the first task that joins
   the components. See `docs/decisions/0004`.
2. **Whenever a task wires previously-independent components together, its brief must state
   each component's known failure modes** and ask explicitly how they interact. Composition
   tasks get an integration question, not just a functional one: *what does each side do
   wrong, and does the other side care?*

The general shape to watch for: component A's characteristic error lands in component B's
weakest region. Both are individually correct. The system is not.

---

## L4 — A cross-component risk, asserted from reasoning, was wrong in its specifics
**Found:** T0.9, by the implementer, before writing the code it was told to write. Confirmed
numerically by the controller, then probed on the assembled pipeline.

L3 produced FR15 with a stated justification: a detector octave error would request pitch
ratio ≈ 2.0 and produce garbage. **The mechanism was wrong.** The ratio is `snap(f) / f`, and
nearest-note rounding is bounded to half a semitone by construction — the pipeline's ratio
range is exactly 0.9715–1.0293 (±50.00 cents), verified over 65–1100 Hz. A detector octave
error does not inflate the ratio; it corrects toward the wrong note at an ordinary ratio.

Probing the assembled pipeline then found the real defect, which is worse. `ResampleCorrector`
cannot sustain **any** ratio ≠ 1.0: above 1.0 it starves, below 1.0 it exhausts its bounded
history, and either way the output becomes one constant value per block. A flat 435 Hz input —
a twenty-cent correction — collapses in 0.68 s; a sharp 445 Hz input in 4.84 s. Only an
exactly-in-tune input survives.

**Why it was missed:** L3 was derived by reasoning across two components' documented
behaviours without running them together. The reasoning correctly identified *that* the seam
was dangerous and *which* two components were involved — and was wrong about the mechanism,
which sent the fix (a clamp) at a problem that could not occur.

**What changed:** a cross-component risk is **probed on the assembled pipeline before it
becomes a requirement**, not argued from each component's documentation. Writing a throwaway
program that drives the real objects and prints numbers took minutes and produced a table
that settled it. Reasoning produced a plausible requirement that was inert.

**And the correction itself was corrected.** The controller's first probe reported "decays to
near-silence" and "the sharp direction is stable". Both were wrong, and the reviewer caught
both: the output after collapse is a block-rate staircase at *full scale* — the near-zero RMS
readings were samples taken at nulls of the aliased envelope — and the sharp direction fails
too, at 4.84 s, once the bounded history runs out. Probing beat reasoning; probing *carefully*
beat probing. RMS alone hid a full-scale signal; min/max spread per block exposed it in one
line.

Corollary, and the sharpest part of this lesson: **test both directions, and test longer than
you think you need to.** Every T0.9 test corrected downward and none ran past 0.25 s. Half the
input domain was broken, the other half was broken more slowly, and the suite was green. That
is L2 in a new costume twice over — the untested region was not a frequency band this time but
a *sign*, and then a *duration*.

**Second instance, 2026-09-04 (T1.2).** The same shape again, in a different place. Decision
0003 predicted that a weak or missing fundamental would make `AutocorrelationDetector` report an
octave high. An implementer wrote the test set, found all five predicted cases detected
*correctly*, worked out why, and constructed a sixth case that does reproduce it. Measured, a
missing fundamental scores 0.20 at half-period, nowhere near an ambiguity; only an
even-harmonics-only signal reaches 1.0000, and that signal genuinely has the shorter period.

The reasoning that produced 0003 was sound about the mechanism and wrong about the conditions —
exactly L4's failure mode. **Predicted failure modes get reproduced before they are planned
around.** Three of this project's records have now been corrected by someone who went and
measured: a worked example, a seam risk, and this one.
