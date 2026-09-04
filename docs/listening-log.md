# Listening log

The only record of what human ears said. Claude cannot hear; every entry here is yours.
Append, never edit history. One entry per listening checkpoint.

Format:

```
## YYYY-MM-DD — Stage N checkpoint — commit abc1234
**Input:** which recording · **Settings:** strength / retune / key
- what you heard, plainly (warble on held notes, chipmunk on the high phrase, click at 0:12…)
- what sounded *right*
- verdict: pass / fail the listening checklist in specs.md §9
```

---

## No entries yet — and this is now the biggest gap in the project

Nothing below this line has been written, because nothing has been listened to. Every acceptance
criterion met so far is number-shaped: AC1, AC2, AC6 and AC7 all measure a *detector*. None of them
says whether the engine **sounds** good, and no amount of further measurement will.

**What is ready to listen to, as of 2026-09-04 (commit `b01d23a`):**

```bash
cmake --build build -j
./build/tools/autotune-cli/opentune-cli in.wav out.wav --key C:major --strength 0.8
.venv/bin/python tools/analyze.py out.wav --out report.png --ref in.wav
```

The pipeline is `AutocorrelationDetector` → `ScaleQuantizer` → `SignalsmithCorrector`. Correction is
sustained and latency-compensated: a 12-second flat 435 Hz input holds 440.4 Hz (+1.4 cents) from
0.5 s to 11.5 s, output length matches input exactly, and there is no startup hole. That was not
true earlier in Stage 0 — the naive corrector collapsed after 0.68 s — so a listen now tells you
something a listen then could not.

**What to listen for, and why each one is here rather than a guess:**

- **Chipmunk effect on upward correction.** Formant preservation is not yet enabled (that is T2.4).
  Expect it; the question is how bad.
- **Warble or flutter on held notes.** `retuneMs` is stored but not applied (T4.2), so correction is
  instantaneous — the most likely source of an unnatural sound.
- **Meend and ornaments.** FR16 says continuous slides must survive correction. The detector handles
  3500 cents/second fine on synthetic material; whether the *corrector* preserves the gesture is a
  different question and only ears answer it.
- **Consonants and breaths.** FR2 routes unvoiced audio through the corrector at unity ratio, so it
  is no longer bit-identical. Does that colour the consonants?
- **Anything at phrase boundaries.** D6 was a 123 ms hole at every voiced/unvoiced transition,
  fixed 2026-09-04. If you hear a click or dropout there, it is back.

**One recording is worth more than any other: a take sung deliberately flat, then deliberately
sharp.** Every voice in the reference set — all nine — sings accurately, so the corrector currently
has almost nothing to do. Stage 2's A/B pass between the naive and Signalsmith correctors cannot
mean anything without input that needs correcting, and that comparison is what justifies the whole
Stage 2 decision.

Delete this section when the first real entry lands.

