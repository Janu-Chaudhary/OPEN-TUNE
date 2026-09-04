#!/usr/bin/env python3
"""Synthesise voice-like signals whose f0 is exact by construction (T1.0).

WHY THIS EXISTS
---------------
`tools/label_f0.py` builds reference labels for the owner's real Hindi takes by
majority vote of three independent estimators.  Those labels are ~10 cents
precise (median inter-estimator spread 8.37 cents; on 25.7% of "agreed" frames
the estimators disagree by more than 15 cents).  AC2 asks whether the engine
lands inside +/-15 cents.  The ruler is the size of the quantity, so AC2 cannot
be read off that set at all -- see `docs/decisions/0008`.

A synthesised voice has no such problem.  We do not *estimate* its f0; we
*choose* it, sample by sample, and the waveform is built from that choice.  The
label carries no error bar beyond float64.  That is the only ground truth
available to this project today, and it is what AC2 has to be measured against.

WHY NOT A STACK OF SINES
------------------------
A bare harmonic stack flatters a pitch detector.  Its harmonics are exactly
equal-phase, its spectral envelope is a smooth roll-off, and there is nothing in
it that stresses the difference function.  Real voices are not like that, and a
detector that passes on sine stacks can still fail on a voice (this project has
seen exactly that: YIN measures 0.375 cents on synthetic sines and 83% inside
+/-15 cents on real singing).

So this tool uses the **source-filter model** of speech production, which is how
a voice actually works:

  * The **source** is the glottis.  The vocal folds open and close, chopping the
    airflow from the lungs into a train of puffs.  The *rate* of that chopping
    is the pitch -- f0 lives entirely in the source.  The pulse is not a sine
    and not an impulse: it is an asymmetric puff, rising slowly as the folds
    part and falling fast as they slam shut.  That asymmetry is why a voice has
    a rich harmonic series rolling off at roughly -12 dB/octave.
  * The **filter** is the vocal tract -- the tube from glottis to lips.  Like any
    tube it has resonances, and those resonances (the **formants**) boost some
    harmonics and suppress others.  The formants are what make /a/ sound
    different from /i/.  They do *not* change the pitch: moving your tongue
    changes the vowel, not the note.
  * Radiation from the lips acts as a differentiator, which is why the pressure
    waveform measured in front of a mouth has a sharp negative spike at the
    instant of glottal closure.

Filtering is a linear time-invariant operation, so it cannot change the period
of a periodic input.  That is the whole trick: we get realistic voice-shaped
spectra while f0 stays exactly what we asked for, provable and proved (see
`--verify`).

WHAT IS MODELLED, AND FROM WHERE
--------------------------------
Source: **Rosenberg (1971) model B**, the two-piece trigonometric glottal-flow
pulse -- A. E. Rosenberg, "Effect of glottal pulse shape on the quality of
natural vowels", JASA 49(2B):583-590, 1971.  Over one period of length T0:

    g(t) = 0.5 * (1 - cos(pi * t / T1))            0    <= t <= T1   (opening)
    g(t) = cos(pi * (t - T1) / (2 * T2))           T1   <  t <= T1+T2 (closing)
    g(t) = 0                                       T1+T2 <  t <  T0   (closed)

parameterised here by the open quotient OQ = (T1+T2)/T0 and the speed quotient
SQ = T1/T2.  Defaults OQ = 0.56, SQ = 2.5 sit inside the ranges Rosenberg used
(his T1/T0 ~ 0.4, T2/T0 ~ 0.16).  The Liljencrants-Fant model is the other
standard choice; Rosenberg is used here because it is closed-form in the phase
variable, which is exactly what a phase-accumulator synthesiser needs.

Filter: a **cascade of two-pole resonators**, one per formant, the standard
formant-synthesiser structure of D. H. Klatt, "Software for a cascade/parallel
formant synthesizer", JASA 67(3):971-995, 1980.  Each resonator is

    y[n] = A*x[n] + B*y[n-1] + C*y[n-2]
    C = -exp(-2*pi*BW/fs)
    B =  2*exp(-pi*BW/fs) * cos(2*pi*F/fs)
    A =  1 - B - C                       (unity gain at DC)

**Formant values -- and an honest note about Hindi.**  The target market is
Hindi (specs.md section 5), so Hindi vowel formants were wanted.  A search for a
citable table of measured Hindi vowel formant frequencies did not produce one
that could be quoted with confidence, and inventing numbers is worse than using
documented ones.  So the values below are the classic **Peterson & Barney
(1952)** adult-male means -- G. E. Peterson and H. L. Barney, "Control methods
used in a study of the vowels", JASA 24(2):175-184 -- for American English
vowels, mapped onto the nearest Hindi monophthong.  Hindi's five long vowels
/aa ii uu ee oo/ are close in the F1-F2 plane to the English vowels used here,
but they are not the same vowels and these are not Hindi measurements.

**This is a known limitation of the set, not a claim about Hindi.**  It affects
vowel *identity*, not f0: the pitch of every signal here is exact regardless of
which formant table shapes it, so no AC2/AC7 number depends on this choice.
Substituting a sourced Hindi table later changes the timbre and nothing else.

Formant bandwidths were not reported by Peterson & Barney.  They come from
Childers & Wu (1993) as tabulated by Xue et al.'s 2018 review of static formant
measurements; see the FORMANT_BW comment for why the wide end of the published
range was chosen and what it was checked against.

USAGE
-----
    .venv/bin/python tools/synth_voices.py --verify        # prove the generator
    .venv/bin/python tools/synth_voices.py --out testdata/synthetic

Writes, per case, `<case>.wav` (48 kHz mono float32, gitignored) and
`<case>.f0.csv` (the ground-truth label track, committed), in the same column
shape as `testdata/vocals/*.f0.csv` so `tools/score_detectors.py` consumes it
unchanged.

Dependencies: numpy and scipy only (both already in requirements-dev.txt).
"""

from __future__ import annotations

import argparse
import os

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

SAMPLE_RATE = 48000

# Label-track geometry, copied from tools/label_f0.py so the two sets are
# interchangeable inputs to the scorer.  time_s is the CENTRE of a 2048-sample
# window; the hop is 256 samples (5.33 ms), the engine's nominal block size.
LABEL_WINDOW = 2048
LABEL_HOP = 256

# Peterson & Barney (1952) adult-male vowel means, Hz.  See the module docstring
# for why these stand in for Hindi values and what that does and does not cost.
# F4/F5 are not from Peterson & Barney -- they are fixed generic high formants,
# present only so the synthetic spectrum does not run out of resonances above
# 3 kHz.
#
# BANDWIDTHS.  B1-B3 are Childers & Wu (1993) as tabulated in Table 5 of
# S. A. Xue et al.'s review, "Static measurements of vowel formant frequencies
# and bandwidths: a review", J. Commun. Disord. 74:74-97, 2018 (PMC6002811),
# which reports the range across studies as B1 50-140, B2 62-149, B3 67-223 Hz
# for English-speaking adults.  Childers & Wu sit at the WIDE end of that range,
# and that end was chosen on a measurement, not a preference: with narrow
# bandwidths (Fant 1962's 48/50/98) the F1 resonance is so sharp that a single
# harmonic landing on it dominates everything else by 20+ dB, which no real
# recording does.  Checked against the owner's own takes -- the median harmonic
# profile H1..H12 of voiced 150-260 Hz frames in take01/03/05 -- the narrow set
# is 9.6 dB RMS away from real, Childers & Wu 7.3 dB.  B4/B5 are extrapolated,
# not cited.
#
# This choice was made against RECORDED VOICES and never against detector
# output.  Tuning a ground-truth generator until the thing under test passes
# would destroy the whole point of having ground truth.
FORMANT_BW = [140.0, 149.0, 223.0, 260.0, 300.0]

VOWELS = {
    # key            F1     F2      F3     F4      F5     P&B vowel   ~Hindi
    "aa": [730.0, 1090.0, 2440.0, 3400.0, 4500.0],  # /ɑ/ "hod"      आ
    "ii": [270.0, 2290.0, 3010.0, 3700.0, 4700.0],  # /i/ "heed"     ई
    "uu": [300.0, 870.0, 2240.0, 3400.0, 4500.0],  # /u/ "who'd"    ऊ
    "ee": [390.0, 1990.0, 2550.0, 3500.0, 4600.0],  # /ɪ/ "hid"      ए
    "oo": [440.0, 1020.0, 2240.0, 3400.0, 4500.0],  # /ʊ/ "hood"    ओ
}

# Unvoiced fricative-ish shaping: a high-frequency-dominated noise, produced by
# a turbulent constriction rather than by the folds.  Rough /s/-like resonances.
FRICATIVE_FORMANTS = [1400.0, 4000.0, 6500.0]
FRICATIVE_BW = [300.0, 600.0, 900.0]


# ---------------------------------------------------------------------------
# Source: glottal pulse train
# ---------------------------------------------------------------------------


def rosenberg_pulse(phase, oq=0.56, sq=2.5):
    """Rosenberg (1971) model B glottal flow, evaluated on normalised phase.

    `phase` is the position within the cycle in [0, 1).  Working in phase rather
    than in time is what lets f0 vary continuously (vibrato, meend) while the
    pulse shape stays constant: the pulse simply stretches with the period.

    oq  -- open quotient, the fraction of the cycle the folds are apart.
    sq  -- speed quotient, opening time / closing time.  Greater than 1 because
           the folds close faster than they open; that asymmetry is what puts
           energy into the upper harmonics.
    """
    t1 = oq * sq / (1.0 + sq)  # opening phase width
    t2 = oq / (1.0 + sq)  # closing phase width

    p = np.mod(phase, 1.0)
    g = np.zeros_like(p)

    opening = p <= t1
    g[opening] = 0.5 * (1.0 - np.cos(np.pi * p[opening] / t1))

    closing = (p > t1) & (p <= t1 + t2)
    g[closing] = np.cos(np.pi * (p[closing] - t1) / (2.0 * t2))

    return g


def harmonic_source(phase, n_harmonics, amps):
    """Explicit harmonic stack, used ONLY for the missing-fundamental family.

    Weak/missing-fundamental cases need individual harmonics turned down or off,
    which the Rosenberg pulse does not expose as a knob.  Harmonic k rides on
    k * phase, so a continuously varying f0 stays exactly coherent across the
    whole series.
    """
    out = np.zeros_like(phase)
    for k in range(1, n_harmonics + 1):
        if amps[k - 1] == 0.0:
            continue
        out += amps[k - 1] * np.sin(2.0 * np.pi * k * phase)
    return out


def glottal_rolloff(n_harmonics, db_per_octave=-12.0):
    """Amplitude of harmonic k under a -12 dB/octave source spectrum.

    -12 dB/octave is the textbook slope of glottal *flow*; lip radiation
    differentiates, restoring +6, for the ~-6 dB/octave seen at the mouth.
    """
    k = np.arange(1, n_harmonics + 1, dtype=float)
    return 10.0 ** (db_per_octave * np.log2(k) / 20.0)


# ---------------------------------------------------------------------------
# Phase accumulation, jitter and shimmer
# ---------------------------------------------------------------------------


def accumulate(f0_base, jitter=0.0, shimmer=0.0, rng=None):
    """Integrate an f0 trajectory into phase, with per-cycle perturbation.

    Returns (phase, f0_actual, amplitude).

    Real vocal folds are not a metronome.  Consecutive cycles differ slightly in
    length (**jitter**) and in the strength of the puff (**shimmer**); a voice
    with zero jitter sounds synthetic, and a detector that assumes exact
    periodicity is not being tested by one.  Healthy sustained phonation runs
    around 0.2-1% jitter and 2-6% shimmer; pathological or untrained voices go
    considerably higher, which is why the set includes both.

    Jitter is applied as a perturbation of the PERIOD, resampled at each glottal
    closure, so `f0_actual` is piecewise constant within a cycle.  That array is
    the ground truth -- the perturbation is not noise added on top of a label,
    it *is* the label.  Nothing here is estimated.
    """
    n = len(f0_base)
    phase = np.empty(n)
    f0_actual = np.empty(n)
    amp = np.empty(n)

    if rng is None:
        rng = np.random.default_rng(0)

    ph = 0.0
    period_factor = 1.0
    amp_factor = 1.0
    if jitter > 0.0:
        period_factor = 1.0 + jitter * rng.standard_normal()
    if shimmer > 0.0:
        amp_factor = 1.0 + shimmer * rng.standard_normal()

    for i in range(n):
        f = f0_base[i] / period_factor
        f0_actual[i] = f
        amp[i] = amp_factor
        phase[i] = ph
        ph += f / SAMPLE_RATE
        if ph >= 1.0:
            ph -= 1.0
            if jitter > 0.0:
                period_factor = 1.0 + jitter * rng.standard_normal()
            if shimmer > 0.0:
                amp_factor = 1.0 + shimmer * rng.standard_normal()

    return phase, f0_actual, amp


# ---------------------------------------------------------------------------
# Filter: cascade of formant resonators
# ---------------------------------------------------------------------------


def resonator(x, freq, bw, fs=SAMPLE_RATE):
    """One two-pole resonator (Klatt 1980), normalised to unity gain at DC."""
    c = -np.exp(-2.0 * np.pi * bw / fs)
    b = 2.0 * np.exp(-np.pi * bw / fs) * np.cos(2.0 * np.pi * freq / fs)
    a = 1.0 - b - c
    return lfilter([a], [1.0, -b, -c], x)


def vocal_tract(x, formants, bandwidths=None, fs=SAMPLE_RATE):
    """Cascade the resonators.  LTI, so it cannot change the input's period."""
    if bandwidths is None:
        bandwidths = FORMANT_BW
    y = x
    for f, bw in zip(formants, bandwidths):
        y = resonator(y, f, bw, fs)
    return y


def lip_radiation(x):
    """Radiation from the lips ~ a first difference (a +6 dB/octave tilt).

    A first difference is also LTI, so this too leaves the period alone.
    """
    return np.diff(x, prepend=x[0])


# ---------------------------------------------------------------------------
# f0 trajectory builders
# ---------------------------------------------------------------------------


def t_axis(duration):
    return np.arange(int(round(duration * SAMPLE_RATE))) / float(SAMPLE_RATE)


def traj_steady(duration, f0):
    return np.full(len(t_axis(duration)), float(f0))


def traj_vibrato(duration, f0, rate_hz, depth_cents):
    """Sinusoidal modulation in the CENTS domain, which is how vibrato is heard.

    Singing vibrato is typically 5-7 Hz at +/-50 to +/-150 cents; the extent is
    what listeners hear as 'depth', and it is symmetric in log-frequency, not in
    Hz.  A detector must track this: it is a real pitch movement, not noise.
    """
    t = t_axis(duration)
    return f0 * 2.0 ** ((depth_cents / 1200.0) * np.sin(2.0 * np.pi * rate_hz * t))


def traj_meend(duration, f0_low, interval_cents, velocity_cents_per_s, hold_s=0.12):
    """Repeated glides up and down at a fixed cents/second rate -- meend.

    The continuous slide between notes is the defining gesture of Hindi singing
    (specs.md section 5), and it is where a fixed-window detector suffers: the
    pitch moves *inside* the analysis window, so the window contains no single
    period.  Measured on the owner's real takes, contour velocity runs a median
    345-741 cents/second with excursions past 2000, so the cases sweep that band
    and beyond.

    Built by integrating a piecewise-constant cents-velocity, which makes the
    velocity exact rather than approximate.
    """
    n = len(t_axis(duration))
    hold_n = int(round(hold_s * SAMPLE_RATE))
    glide_n = max(1, int(round((interval_cents / velocity_cents_per_s) * SAMPLE_RATE)))

    cents = np.empty(n)
    i = 0
    direction = 1
    level = 0.0
    while i < n:
        end = min(n, i + hold_n)
        cents[i:end] = level
        i = end
        if i >= n:
            break
        end = min(n, i + glide_n)
        step = np.arange(1, end - i + 1) * (interval_cents / glide_n) * direction
        cents[i:end] = level + step
        level = cents[end - 1]
        i = end
        direction *= -1
    return f0_low * 2.0 ** (cents / 1200.0)


# ---------------------------------------------------------------------------
# Case rendering
# ---------------------------------------------------------------------------


def render_voiced(
    f0_traj,
    vowel="aa",
    jitter=0.0,
    shimmer=0.0,
    hnr_db=None,
    harmonic_amps=None,
    n_harmonics=None,
    seed=0,
):
    """Render one voiced stretch.  Returns (audio, f0_per_sample, source).

    `source` is the pre-filter glottal signal, handed back so `--verify` can
    measure the period of the thing that actually carries the pitch.
    """
    rng = np.random.default_rng(seed)
    phase, f0_actual, amp = accumulate(f0_traj, jitter=jitter, shimmer=shimmer, rng=rng)

    if harmonic_amps is not None:
        src = harmonic_source(phase, n_harmonics, harmonic_amps)
    else:
        src = rosenberg_pulse(phase)
    src = src * amp

    if hnr_db is not None:
        # Breathy phonation: the folds do not close completely, so air rushes
        # through continuously and adds turbulence on top of the pulse train.
        # HNR is set on the source, before the tract, because both components
        # then pass through the same filter -- which is what physically happens.
        noise = rng.standard_normal(len(src))
        h_rms = float(np.sqrt(np.mean((src - src.mean()) ** 2)))
        n_rms = float(np.sqrt(np.mean(noise**2)))
        target = h_rms / (10.0 ** (hnr_db / 20.0))
        src = src + noise * (target / n_rms)

    audio = vocal_tract(lip_radiation(src), VOWELS[vowel])
    return audio, f0_actual, src


def render_unvoiced(duration, seed=0):
    """Fricative-like turbulence: no periodicity, so no f0 exists at all."""
    rng = np.random.default_rng(seed)
    n = len(t_axis(duration))
    noise = rng.standard_normal(n)
    return vocal_tract(noise, FRICATIVE_FORMANTS, FRICATIVE_BW)


def fade(x, ms=8.0):
    """Raised-cosine edges, so a segment boundary is not a broadband click.

    A click is a transient with energy at every frequency; leaving them in would
    hand the detector an artefact that no voice produces, and any error near the
    boundary would be an artefact of the generator rather than of the detector.
    """
    n = min(int(round(ms * SAMPLE_RATE / 1000.0)), len(x) // 2)
    if n < 2:
        return x
    w = 0.5 * (1.0 - np.cos(np.pi * np.arange(n) / n))
    y = x.copy()
    y[:n] *= w
    y[-n:] *= w[::-1]
    return y


class Segment:
    """One stretch of a case: voiced (with an f0 track), unvoiced, or silent."""

    def __init__(self, audio, f0, voiced):
        self.audio = audio
        self.f0 = f0
        self.voiced = voiced


def seg_voiced(f0_traj, gain=1.0, **kw):
    audio, f0, _ = render_voiced(f0_traj, **kw)
    audio = fade(audio)
    peak = float(np.max(np.abs(audio))) or 1.0
    return Segment(audio * (gain / peak), f0, np.ones(len(audio), bool))


def seg_unvoiced(duration, gain=0.2, seed=0):
    a = fade(render_unvoiced(duration, seed))
    peak = float(np.max(np.abs(a))) or 1.0
    a = a * (gain / peak)
    return Segment(a, np.full(len(a), np.nan), np.zeros(len(a), bool))


def seg_silence(duration, noise_dbfs=None, seed=0):
    n = len(t_axis(duration))
    if noise_dbfs is None:
        a = np.zeros(n)
    else:
        rng = np.random.default_rng(seed)
        a = rng.standard_normal(n) * (10.0 ** (noise_dbfs / 20.0))
    return Segment(a, np.full(n, np.nan), np.zeros(n, bool))


def concat(segments):
    audio = np.concatenate([s.audio for s in segments])
    f0 = np.concatenate([s.f0 for s in segments])
    voiced = np.concatenate([s.voiced for s in segments])
    return audio, f0, voiced


# ---------------------------------------------------------------------------
# The case set
# ---------------------------------------------------------------------------

# Steady notes spanning the whole declared detectable range, 65-1100 Hz
# (specs.md section 6), INCLUDING both endpoints -- the T1.0 real set covers
# only about 93-591 Hz at the percentile level, so the extremes were untested.
# Vowels are rotated so the set does not measure one formant pattern only.
STEADY_NOTES = [
    (65.0, "aa"),
    (82.41, "uu"),
    (110.0, "aa"),
    (146.83, "oo"),
    (196.0, "ee"),
    (261.63, "aa"),
    (329.63, "ii"),
    (440.0, "aa"),
    (587.33, "ee"),
    (783.99, "uu"),
    (987.77, "aa"),
    (1100.0, "ii"),
]

STEADY_S = 1.5
CASE_S = 2.5


def build_cases():
    """Return {case_id: (audio, f0_per_sample, voiced_mask, description)}.

    One variable moves per case.  The point is isolating *why* a detector fails,
    not recording that it did: if the jitter cases pass and the meend cases do
    not, that is a window-length problem, and it says so without further work.
    """
    cases = {}

    # --- steady notes across the full range ------------------------------
    for f0, vowel in STEADY_NOTES:
        cid = "steady_%04dhz" % round(f0)
        cases[cid] = (
            *concat([seg_voiced(traj_steady(STEADY_S, f0), vowel=vowel, seed=1)]),
            "steady %.2f Hz, vowel /%s/, no modulation" % (f0, vowel),
        )

    # --- vibrato ----------------------------------------------------------
    for cid, f0, rate, depth in [
        ("vibrato_5p5hz_50c", 220.0, 5.5, 50.0),
        ("vibrato_6hz_100c", 440.0, 6.0, 100.0),
    ]:
        cases[cid] = (
            *concat([seg_voiced(traj_vibrato(CASE_S, f0, rate, depth), vowel="aa", seed=2)]),
            "vibrato %.1f Hz at +/-%.0f cents around %.1f Hz" % (rate, depth, f0),
        )

    # --- jitter (cycle-to-cycle PERIOD variation) -------------------------
    for cid, j in [("jitter_0p5pct", 0.005), ("jitter_2pct", 0.02)]:
        cases[cid] = (
            *concat([seg_voiced(traj_steady(CASE_S, 220.0), vowel="aa", jitter=j, seed=3)]),
            "220 Hz with %.1f%% cycle-to-cycle period jitter" % (j * 100.0),
        )

    # --- shimmer (cycle-to-cycle AMPLITUDE variation) ---------------------
    for cid, s in [("shimmer_5pct", 0.05), ("shimmer_15pct", 0.15)]:
        cases[cid] = (
            *concat([seg_voiced(traj_steady(CASE_S, 220.0), vowel="aa", shimmer=s, seed=4)]),
            "220 Hz with %.0f%% cycle-to-cycle amplitude shimmer" % (s * 100.0),
        )

    # --- jitter and shimmer together, an untrained-voice figure -----------
    cases["jitter_shimmer_rough"] = (
        *concat(
            [seg_voiced(traj_steady(CASE_S, 196.0), vowel="aa", jitter=0.02, shimmer=0.15, seed=5)]
        ),
        "196 Hz with 2% jitter and 15% shimmer together (rough phonation)",
    )

    # --- meend: continuous glides at several speeds -----------------------
    for cid, vel in [
        ("meend_350cps", 350.0),
        ("meend_700cps", 700.0),
        ("meend_1500cps", 1500.0),
        ("meend_2400cps", 2400.0),
        ("meend_3500cps", 3500.0),
    ]:
        cases[cid] = (
            *concat(
                [seg_voiced(traj_meend(CASE_S, 196.0, 700.0, vel), vowel="aa", seed=6)]
            ),
            "meend: 700-cent glides up and down at %.0f cents/second from 196 Hz" % vel,
        )

    # --- weak and missing fundamentals ------------------------------------
    # docs/decisions/0003, as CORRECTED by T1.2: removing H1 does NOT by itself
    # create an octave ambiguity, because the surviving odd harmonics are not
    # periodic at P/2 and cancel there.  Only an even-harmonics-only signal is
    # genuinely periodic at the half period -- and reporting 2*f0 for that one
    # is arguably right, since the waveform really does have that period.  These
    # three cases pin all three points of that finding against exact labels.
    nh = 24
    base = glottal_rolloff(nh)

    weak = base.copy()
    weak[0] *= 10.0 ** (-20.0 / 20.0)
    missing = base.copy()
    missing[0] = 0.0
    even = base.copy()
    even[0::2] = 0.0  # zero H1, H3, H5 ... leaving only even harmonics

    for cid, amps, note in [
        ("weak_fundamental", weak, "H1 attenuated 20 dB"),
        ("missing_fundamental", missing, "H1 removed entirely"),
        ("even_harmonics_only", even, "odd harmonics removed; period really is P/2"),
    ]:
        cases[cid] = (
            *concat(
                [
                    seg_voiced(
                        traj_steady(CASE_S, 220.0),
                        vowel="aa",
                        harmonic_amps=amps,
                        n_harmonics=nh,
                        seed=7,
                    )
                ]
            ),
            "220 Hz harmonic source, %s" % note,
        )

    # Telephone band: the realistic way a fundamental goes missing.  A 196 Hz
    # voice through a 300 Hz high-pass loses H1 outright and most of H2's level,
    # which is exactly what a phone handset does to the primary user's voice.
    audio, f0, voiced = concat([seg_voiced(traj_steady(CASE_S, 196.0), vowel="aa", seed=8)])
    from scipy.signal import butter, sosfilt

    sos = butter(4, [300.0, 3400.0], btype="bandpass", fs=SAMPLE_RATE, output="sos")
    audio = sosfilt(sos, audio)
    audio = audio / (float(np.max(np.abs(audio))) or 1.0)
    cases["telephone_band"] = (audio, f0, voiced, "196 Hz through a 300-3400 Hz telephone band")

    # --- breathy / noisy phonation ----------------------------------------
    for cid, hnr in [("breathy_hnr20db", 20.0), ("breathy_hnr10db", 10.0), ("breathy_hnr5db", 5.0)]:
        cases[cid] = (
            *concat(
                [seg_voiced(traj_steady(CASE_S, 220.0), vowel="aa", hnr_db=hnr, seed=9)]
            ),
            "220 Hz with aspiration noise at %.0f dB harmonics-to-noise ratio" % hnr,
        )

    # --- silence and unvoiced segments (AC6, FR2) --------------------------
    # The real set is entirely sung and contains no silence at all, so AC6 and
    # the unvoiced bypass were measured on the easy half of the problem
    # (testdata/vocals/MANIFEST.md section 6c).  These two cases supply the hard
    # half: true digital silence, a near-silent noise floor, and turbulent
    # unvoiced stretches that no periodicity model should call voiced.
    cases["voicing_alternating"] = (
        *concat(
            [
                seg_silence(0.4),
                seg_voiced(traj_steady(0.6, 196.0), vowel="aa", seed=10),
                seg_unvoiced(0.3, seed=11),
                seg_voiced(traj_steady(0.5, 261.63), vowel="ee", seed=12),
                seg_silence(0.3),
                seg_voiced(traj_steady(0.4, 146.83), vowel="oo", seed=13),
                seg_unvoiced(0.25, seed=14),
                seg_silence(0.25),
            ]
        ),
        "silence / voiced / unvoiced-fricative alternation, digital silence",
    )
    cases["voicing_noisefloor"] = (
        *concat(
            [
                seg_silence(0.4, noise_dbfs=-55.0, seed=15),
                seg_voiced(traj_steady(0.6, 196.0), vowel="aa", seed=16),
                seg_unvoiced(0.3, seed=17),
                seg_silence(0.4, noise_dbfs=-55.0, seed=18),
                seg_voiced(traj_steady(0.5, 329.63), vowel="ii", seed=19),
                seg_silence(0.3, noise_dbfs=-55.0, seed=20),
            ]
        ),
        "same alternation over a -55 dBFS noise floor instead of true silence",
    )

    return cases


# ---------------------------------------------------------------------------
# Verification: does the audio actually carry the f0 we claim?
# ---------------------------------------------------------------------------


def exact_lag_for_cycles(f0_per_sample, n_cycles):
    """How many samples the LABEL says it takes to advance `n_cycles` cycles.

    Integrating the label track gives the exact expected interval, whatever
    shape the trajectory has.  This is the reference every measurement below is
    differenced against.
    """
    cum = np.concatenate([[0.0], np.cumsum(f0_per_sample) / SAMPLE_RATE])
    if cum[-1] < n_cycles:
        return None
    i = int(np.searchsorted(cum, n_cycles))
    lo, hi = cum[i - 1], cum[i]
    return (i - 1) + (n_cycles - lo) / (hi - lo)


# --- Method A: period counting on the glottal source -----------------------


def method_a_cycle_count(source, f0_per_sample):
    """Count glottal cycles in the SOURCE and compare against the label.

    The Rosenberg pulse crosses its own mean exactly once on the way up per
    cycle, so upward mean-crossings are cycle markers.  The crossing instant is
    interpolated linearly between the two straddling samples.

    Two numbers come out:

      * the CUMULATIVE error -- the counted number of cycles between the first
        and last crossing, against the exact number the label track says should
        fit in that span.  Per-crossing timing wobble telescopes away over a
        long span, so this is the sensitive test: a synthesiser whose waveform
        drifts from its label by even a tiny rate error shows up here, growing
        with duration.
      * the per-cycle median, a diagnostic on how steady the individual cycles
        are.  Amplitude modulation moves the crossing point within a cycle, so
        this number is inflated by shimmer without the pitch being wrong.

    Nothing here consults the phase accumulator.  It reads the samples.
    """
    x = np.asarray(source, float)
    x = x - float(np.mean(x))
    sign = x >= 0.0
    idx = np.nonzero((~sign[:-1]) & sign[1:])[0]
    if len(idx) < 4:
        return None

    frac = -x[idx] / (x[idx + 1] - x[idx])
    t_cross = idx + frac  # in samples

    n_cycles = len(t_cross) - 1
    span = t_cross[-1] - t_cross[0]

    # Exact expected span: integrate the label over the same interval.
    a = int(np.floor(t_cross[0]))
    cum = np.cumsum(f0_per_sample[a:]) / SAMPLE_RATE
    expected = exact_lag_for_cycles(f0_per_sample[a:], n_cycles)
    del cum
    if expected is None:
        return None
    cumulative_cents = 1200.0 * np.log2(expected / span)

    per_cycle = []
    for k in range(n_cycles):
        lo = max(0, int(np.floor(t_cross[k])))
        hi = min(len(f0_per_sample), max(lo + 1, int(np.ceil(t_cross[k + 1]))))
        f_true = 1.0 / np.mean(1.0 / f0_per_sample[lo:hi])
        f_meas = SAMPLE_RATE / (t_cross[k + 1] - t_cross[k])
        per_cycle.append(1200.0 * np.log2(f_meas / f_true))
    per_cycle = np.abs(np.asarray(per_cycle))

    return {
        "cycles": n_cycles,
        "cumulative_cents": cumulative_cents,
        "median_cents": float(np.median(per_cycle)),
        "p95_cents": float(np.percentile(per_cycle, 95)),
    }


# --- Method B: long-lag normalised cross-correlation on the OUTPUT ---------


def _nccf(x, window, lags):
    seg = x[:window]
    e0 = float(np.dot(seg, seg))
    out = np.empty(len(lags))
    for i, l in enumerate(lags):
        y = x[l : l + window]
        out[i] = np.dot(seg, y) / np.sqrt(e0 * float(np.dot(y, y)) + 1e-300)
    return out


def _refine(x, window, lag, half):
    lags = np.arange(int(lag - half), int(lag + half) + 1)
    lags = lags[(lags > 0) & (lags + window < len(x))]
    r = _nccf(x, window, lags)
    k = int(np.argmax(r))
    d = 0.0
    if 0 < k < len(r) - 1:
        a, b, c = r[k - 1], r[k], r[k + 1]
        den = a - 2.0 * b + c
        if den != 0.0:
            d = float(np.clip(0.5 * (a - c) / den, -1.0, 1.0))
    return lags[k] + d, float(r[k])


def method_b_long_lag(audio, fmin=55.0, fmax=1400.0):
    """Measure the period of the FINAL audio, after the vocal tract.

    Method A reads the source.  This one reads what actually goes in the WAV,
    which is the thing a detector will see, and it has to survive formant
    ringing, aspiration noise and a missing fundamental -- all of which put
    extra mean-crossings inside a single cycle, so counting will not do.

    Instead, normalised cross-correlation, with no prior on the answer:

      1. Coarse: correlate the first half of the signal against itself at every
         lag from 1400 Hz down to 55 Hz, and take the SHORTEST local maximum
         reaching 95% of the best.  Shortest, not highest, because a periodic
         signal correlates just as well at 2P and 3P; taking the highest is how
         you report an octave too low (docs/decisions/0003).
      2. Ladder: re-measure at 2, 4, 8 ... N cycles of lag, each step searching
         only +/-0.4 of a period around the previous estimate scaled up, so it
         cannot slip a whole cycle.  A lag of N periods divides the residual
         timing error by N, which is where the sub-millicent precision comes
         from.
      3. Parabolic interpolation on the correlation peak for the sub-sample
         part.

    Returns the cycle count, the measured lag, and the peak correlation.  The
    caller differences the lag against `exact_lag_for_cycles`.

    LIMIT, stated because it matters: this assumes the signal really is
    periodic.  Jitter makes it not, so the correlation peak drifts by a fraction
    of a cent and Method A is the authority for the jitter cases.
    """
    x = np.asarray(audio, float)
    x = x - float(np.mean(x))
    n = len(x)
    lo = max(2, int(SAMPLE_RATE / fmax))
    hi = int(SAMPLE_RATE / fmin)
    window = n // 2
    lags = np.arange(lo, min(hi, window) + 1)
    r = _nccf(x, window, lags)
    rmax = r.max()
    t0 = None
    for i in range(1, len(r) - 1):
        if r[i] >= 0.95 * rmax and r[i] >= r[i - 1] and r[i] >= r[i + 1]:
            t0 = float(lags[i])
            break
    if t0 is None:
        t0 = float(lags[int(np.argmax(r))])

    n_max = int(0.45 * n / t0)
    n_cyc, period, lag, corr = 1, t0, t0, 1.0
    while True:
        nxt = min(n_max, n_cyc * 2)
        if nxt <= n_cyc:
            break
        w2 = n - int(nxt * period * 1.02) - 2
        if w2 < 4 * t0:
            break
        lag, corr = _refine(x, w2, nxt * period, 0.4 * t0)
        period = lag / nxt
        n_cyc = nxt
        if n_cyc >= n_max:
            break
    return n_cyc, lag, corr


def verify():
    """Prove the generator before trusting a single number it produces.

    A synthesiser with a bug produces confident wrong ground truth, which is
    strictly worse than having none: it would silently redefine what "correct"
    means for every measurement downstream.  So the audio is measured back, by
    two methods that share no code with the synthesis path, and the results are
    printed rather than asserted quietly.
    """
    ok = True
    print("=== generator self-verification ===")
    print("Bar: measured f0 must match the LABEL to well under one cent.\n")

    # ---- Method A: cycle counting on the glottal source -------------------
    print("--- Method A: mean-crossing cycle counting on the glottal SOURCE ---")
    print("(covers every trajectory shape; the phase accumulator is never read)")
    print("  %-24s %7s %12s %10s %10s" % ("case", "cycles", "cumul |c|", "med |c|", "p95 |c|"))

    checks = [("steady %.2f Hz" % f0, traj_steady(1.5, f0), {}) for f0, _ in STEADY_NOTES]
    checks += [
        ("vibrato 5.5Hz 50c", traj_vibrato(2.5, 220.0, 5.5, 50.0), {}),
        ("vibrato 6Hz 100c", traj_vibrato(2.5, 440.0, 6.0, 100.0), {}),
        ("meend 350 c/s", traj_meend(2.5, 196.0, 700.0, 350.0), {}),
        ("meend 700 c/s", traj_meend(2.5, 196.0, 700.0, 700.0), {}),
        ("meend 1500 c/s", traj_meend(2.5, 196.0, 700.0, 1500.0), {}),
        ("meend 2400 c/s", traj_meend(2.5, 196.0, 700.0, 2400.0), {}),
        ("meend 3500 c/s", traj_meend(2.5, 196.0, 700.0, 3500.0), {}),
        ("jitter 0.5%", traj_steady(2.5, 220.0), dict(jitter=0.005, seed=3)),
        ("jitter 2%", traj_steady(2.5, 220.0), dict(jitter=0.02, seed=3)),
        ("shimmer 5%", traj_steady(2.5, 220.0), dict(shimmer=0.05, seed=4)),
        ("shimmer 15%", traj_steady(2.5, 220.0), dict(shimmer=0.15, seed=4)),
        ("jitter+shimmer", traj_steady(2.5, 196.0), dict(jitter=0.02, shimmer=0.15, seed=5)),
    ]
    worst_a = 0.0
    for name, traj, kw in checks:
        _, f0_actual, src = render_voiced(traj, vowel="aa", **kw)
        m = method_a_cycle_count(src, f0_actual)
        if m is None:
            print("  %-24s   NO CYCLES FOUND" % name)
            ok = False
            continue
        c = abs(m["cumulative_cents"])
        worst_a = max(worst_a, c)
        if c >= 0.1:
            ok = False
        print("  %-24s %7d %12.6f %10.5f %10.5f%s"
              % (name, m["cycles"], c, m["median_cents"], m["p95_cents"],
                 "" if c < 0.1 else "   <-- FAIL"))

    # ---- Method B: long-lag correlation on the emitted audio --------------
    print("\n--- Method B: long-lag NCCF on the FINAL audio, after the tract ---")
    print("(no prior on the answer; proves the filter and the noise did not move f0)")
    print("  %-24s %7s %12s %8s" % ("case", "cycles", "err cents", "peak r"))

    nh = 24
    base = glottal_rolloff(nh)
    weak = base.copy()
    weak[0] *= 10.0 ** (-20.0 / 20.0)
    missing = base.copy()
    missing[0] = 0.0
    even = base.copy()
    even[0::2] = 0.0

    b_checks = [("steady %.2f /%s/" % (f0, v), traj_steady(1.5, f0), dict(vowel=v, seed=1))
                for f0, v in STEADY_NOTES]
    b_checks += [
        ("shimmer 15%", traj_steady(2.5, 220.0), dict(vowel="aa", shimmer=0.15, seed=4)),
        ("breathy HNR 20 dB", traj_steady(2.5, 220.0), dict(vowel="aa", hnr_db=20.0, seed=9)),
        ("breathy HNR 10 dB", traj_steady(2.5, 220.0), dict(vowel="aa", hnr_db=10.0, seed=9)),
        ("breathy HNR 5 dB", traj_steady(2.5, 220.0), dict(vowel="aa", hnr_db=5.0, seed=9)),
        ("weak fundamental", traj_steady(2.5, 220.0),
         dict(vowel="aa", harmonic_amps=weak, n_harmonics=nh, seed=7)),
        ("missing fundamental", traj_steady(2.5, 220.0),
         dict(vowel="aa", harmonic_amps=missing, n_harmonics=nh, seed=7)),
    ]
    worst_b = 0.0
    for name, traj, kw in b_checks:
        audio, f0_actual, _ = render_voiced(traj, **kw)
        n_cyc, lag, corr = method_b_long_lag(audio)
        expected = exact_lag_for_cycles(f0_actual, n_cyc)
        err = 1200.0 * np.log2(expected / lag)
        worst_b = max(worst_b, abs(err))
        if abs(err) >= 0.1:
            ok = False
        print("  %-24s %7d %12.6f %8.4f%s"
              % (name, n_cyc, err, corr, "" if abs(err) < 0.1 else "   <-- FAIL"))

    # The one case that must NOT read back as f0, and why.
    audio, f0_actual, _ = render_voiced(
        traj_steady(2.5, 220.0), vowel="aa", harmonic_amps=even, n_harmonics=nh, seed=7)
    n_cyc, lag, corr = method_b_long_lag(audio)
    err = 1200.0 * np.log2(exact_lag_for_cycles(f0_actual, n_cyc) / lag)
    print("  %-24s %7d %12.6f %8.4f   <-- EXPECTED +1200: an even-harmonics-only"
          % ("even harmonics only", n_cyc, err, corr))
    print("  %55s waveform genuinely repeats at P/2 (T1.2, docs/decisions/0003)." % "")
    if abs(err - 1200.0) > 1.0:
        ok = False

    print("\n  Method A worst cumulative error: %.6f cents" % worst_a)
    print("  Method B worst error:             %.6f cents" % worst_b)
    print("  Jitter cases are Method A's to judge -- a jittered signal is not")
    print("  periodic, so Method B's correlation peak is not defined to a cent.")
    print("\n  verdict: %s" % ("PASS" if ok else "FAIL"))
    return ok


# ---------------------------------------------------------------------------
# Writing the set
# ---------------------------------------------------------------------------


def write_labels(path, case_id, f0, voiced, audio, description):
    """Write the ground-truth label track in tools/label_f0.py's column shape.

    The scorer reads `time_s, f0_hz, voiced, n_agree`; the remaining columns
    exist so the two label sets are literally the same file format and no
    special case is needed anywhere downstream.  For a synthetic case the three
    "estimator" columns all carry the same exact value and spread_cents is 0 --
    there is nothing to cross-check, because nothing was estimated.

    A frame is labelled voiced only if the ENTIRE 2048-sample analysis window it
    describes is voiced.  Windows that straddle a voicing boundary carry no
    label and are marked `transition`, the same treatment the real set gives its
    `ambiguous` frames.  Inventing a label for a half-voiced window would be
    exactly the "no gap is ever filled with a guess" rule being broken.
    """
    n = len(audio)
    rows = []
    for start in range(0, n - LABEL_WINDOW + 1, LABEL_HOP):
        end = start + LABEL_WINDOW
        centre = start + LABEL_WINDOW // 2
        t = centre / float(SAMPLE_RATE)
        win = audio[start:end]
        rms = float(np.sqrt(np.mean(win**2)))
        rms_db = 20.0 * np.log10(rms) if rms > 0 else -200.0

        vfrac = float(np.mean(voiced[start:end]))
        if vfrac >= 1.0:
            f = float(f0[centre])
            rows.append(
                "%.6f,%.6f,1,exact,3,%.6f,%.6f,%.6f,0.000,%.2f"
                % (t, f, f, f, f, rms_db)
            )
        elif vfrac <= 0.0:
            rows.append("%.6f,,0,unvoiced,0,,,,,%.2f" % (t, rms_db))
        else:
            rows.append("%.6f,,0,transition,0,,,,,%.2f" % (t, rms_db))

    with open(path, "w") as fh:
        fh.write("# OpenTune SYNTHETIC ground-truth f0 (T1.0). Exact by construction.\n")
        fh.write(
            "# source=%s.wav sample_rate=%d window=%d hop=%d tolerance_cents=0.0\n"
            % (case_id, SAMPLE_RATE, LABEL_WINDOW, LABEL_HOP)
        )
        fh.write("# %s; f0_hz is the synthesiser's own per-sample value at the window centre.\n"
                 % description)
        fh.write(
            "time_s,f0_hz,voiced,consensus,n_agree,f0_cepstrum_hz,f0_hps_hz,"
            "f0_nccf_hz,spread_cents,rms_db\n"
        )
        fh.write("\n".join(rows))
        fh.write("\n")
    return len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="testdata/synthetic")
    ap.add_argument("--verify", action="store_true", help="run generator self-check and exit")
    args = ap.parse_args()

    if args.verify:
        raise SystemExit(0 if verify() else 1)

    if not verify():
        raise SystemExit("generator self-verification FAILED; refusing to write a set")

    os.makedirs(args.out, exist_ok=True)
    cases = build_cases()

    print("\n=== writing %d cases to %s ===" % (len(cases), args.out))
    print("  %-22s %8s %8s %8s %8s  %s" % ("case", "dur_s", "frames", "voiced", "f0_lo-hi", "note"))
    for cid in sorted(cases):
        audio, f0, voiced, desc = cases[cid]
        audio = np.asarray(audio, dtype=np.float64)
        peak = float(np.max(np.abs(audio)))
        if peak > 0:
            audio = audio * (0.7 / peak)
        wavfile.write(
            os.path.join(args.out, cid + ".wav"), SAMPLE_RATE, audio.astype(np.float32)
        )
        nrows = write_labels(
            os.path.join(args.out, cid + ".f0.csv"), cid, f0, voiced, audio, desc
        )
        vf = f0[voiced]
        rng_s = "%.0f-%.0f" % (vf.min(), vf.max()) if len(vf) else "-"
        print(
            "  %-22s %8.2f %8d %8.1f%% %8s  %s"
            % (cid, len(audio) / SAMPLE_RATE, nrows, 100.0 * voiced.mean(), rng_s, desc)
        )


if __name__ == "__main__":
    main()
