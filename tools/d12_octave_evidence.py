#!/usr/bin/env python3
"""D12: decide, per frame, whether YIN or the consensus LABEL has the octave right.

The problem this exists to solve.  `tools/score_detectors.py` reports YIN's AC7
octave-error rate against the reference labels in `testdata/vocals/*.f0.csv`.
Those labels are a 2-of-3 consensus of three window-based estimators (cepstrum,
HPS, NCCF), and period-doubling ambiguity is precisely the failure all three
share -- HPS has a documented octave-DOWN bias of its own (MANIFEST section 2).
The manifest already measures the label track's own octave inconsistency at
0.36%-3.43% per take and says outright that `take02`, `take04` and `take06`
cannot resolve a sub-1% octave-error rate.  So "YIN disagrees with the label by
an octave" does not by itself say who is wrong.

This script answers that with evidence independent of BOTH sides.

THE TEST.  Take a disputed frame where the two candidates are fLow and
fHigh ~= 2*fLow.  If fHigh is the true fundamental, the spectrum carries energy
at fHigh, 2*fHigh, 3*fHigh ... -- i.e. at the EVEN multiples of fLow -- and
nothing at all at fLow, 3*fLow, 5*fLow.  If fLow is the true fundamental, the
ODD multiples are present too.  So the question "which of the two is the
fundamental?" reduces to "is there a partial at 3*fLow and 5*fLow, or is that
part of the spectrum at the noise floor?"  That is a statement about the
magnitude spectrum only.  It uses no autocorrelation, no cepstrum, no HPS and
no NCCF, so it is independent of YIN and of all three labelling estimators.

CALIBRATION, BEFORE THE METRIC IS USED TO JUDGE ANYTHING (docs/lessons.md L5,
third instance).  A new metric invented on the spot is worthless until it is
run against a known-good control.  The control here is free and in-domain: the
frames of the SAME take where YIN and the labels AGREE.  On those frames the
agreed f0 is the fundamental by two independent votes, so

  * scoring the agreed f0 as `fLow` must come out POSITIVE (odd partials
    present -- they are the real harmonics), and
  * scoring agreed_f0 / 2 as `fLow` must come out NEGATIVE (there is nothing
    an octave below a correctly-identified fundamental).

Those two distributions, measured on the same voice, same microphone and the
same window length, give the metric's separation and its error rate.  The
disputed frames are only classified afterwards, with that error rate attached.

Usage:
    .venv/bin/python tools/d12_octave_evidence.py take04 \
        --dump /tmp/d12/dump/take04.yin.csv
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import label_f0  # noqa: E402  (reader only -- this script never writes labels)

# Same alignment constant score_detectors.py uses (D11): a YIN estimate
# describes the centre of its 1478-sample comparison window, which sits 1478
# samples behind the newest sample handed in.
OFFSET_S = 1478.0 / 48000.0

# Analysis window for the spectral test.  4096 at 48 kHz = 85.3 ms, giving a
# Hann mainlobe of ~46.9 Hz -- narrow enough to resolve partials spaced by
# fLow >= 100 Hz, which is the whole requirement.  Longer would resolve better
# and smear more during a slide; 4096 is the compromise, and the calibration
# below is what says whether it was good enough.
NFFT_WINDOW = 4096
NFFT_PAD = 16384

# Partial indices, as multiples of the LOW candidate.  Odd multiples exist only
# if fLow is the fundamental; even multiples are harmonics of fHigh either way
# and serve as the control that says "there is a voice here at all".
ODD_K = (1, 3, 5)
EVEN_K = (2, 4, 6)

# Second, sharper test.  "Is there energy at fLow's odd harmonics?" answers
# whether the fLow periodicity EXISTS.  It does not distinguish a genuinely
# weak fundamental (a real voice at fLow whose first harmonic the phone mic
# rolled off) from a SUBHARMONIC (a voice at fHigh whose cycles alternate
# slightly -- diplophonia -- which sprinkles weak partials at every odd
# multiple of fHigh/2).  Both put energy at fLow, 3*fLow, 5*fLow.
#
# What separates them is the spectral ENVELOPE.  A vocal tract filters a
# harmonic series smoothly, so on a real fundamental the odd partials sit ON
# the envelope traced by their even neighbours.  Subharmonics do not: they are
# a small alternation riding on the fHigh series, so they sit uniformly BELOW
# that envelope, typically by 10-20 dB.  ENVELOPE_K are the odd multiples whose
# two even neighbours both exist, so the envelope can be interpolated.
ENVELOPE_K = (3, 5)


def cents(a, b):
    with np.errstate(divide="ignore", invalid="ignore"):
        return 1200.0 * np.log2(np.asarray(a, float) / np.asarray(b, float))


def load_labels(path):
    return np.genfromtxt(path, delimiter=",", names=True, skip_header=3, dtype=None,
                         encoding=None)


def load_dump(path):
    return np.genfromtxt(path, delimiter=",", names=True, dtype=None, encoding=None)


def label_at(labels, times):
    """score_detectors.py's interpolation, reproduced so the frame set matches."""
    lt = labels["time_s"]
    lf = labels["f0_hz"]
    lv = labels["voiced"].astype(bool)
    ok = lv & np.isfinite(lf)
    right = np.clip(np.searchsorted(lt, times), 1, len(lt) - 1)
    left = right - 1
    both = ok[left] & ok[right]
    with np.errstate(invalid="ignore", divide="ignore"):
        step = np.abs(cents(lf[right], lf[left]))
    usable = both & (step <= 100.0)
    span = lt[right] - lt[left]
    frac = np.where(span > 0, (times - lt[left]) / np.where(span > 0, span, 1.0), 0.0)
    frac = np.clip(frac, 0.0, 1.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        interp = np.exp((1.0 - frac) * np.log(lf[left]) + frac * np.log(lf[right]))
    return np.where(usable, interp, np.nan)


class Spectra:
    """Magnitude spectra of `take.wav`, one per requested centre time."""

    def __init__(self, wav_path):
        self.info = label_f0.read_wav_header(wav_path)
        self.x = label_f0.read_samples(self.info, 0, self.info.num_frames)
        self.sr = float(self.info.sample_rate)
        self.win = np.hanning(NFFT_WINDOW)
        self.freqs = np.fft.rfftfreq(NFFT_PAD, 1.0 / self.sr)

    def magnitude(self, centre_s):
        start = int(round(centre_s * self.sr)) - NFFT_WINDOW // 2
        if start < 0 or start + NFFT_WINDOW > len(self.x):
            return None
        seg = self.x[start:start + NFFT_WINDOW] * self.win
        if not np.any(seg):
            return None
        return np.abs(np.fft.rfft(seg, NFFT_PAD))


def band_peak(mag, freqs, centre_hz, halfwidth_hz):
    lo = np.searchsorted(freqs, centre_hz - halfwidth_hz)
    hi = np.searchsorted(freqs, centre_hz + halfwidth_hz)
    if hi <= lo or hi > len(mag):
        return np.nan
    return float(mag[lo:hi].max())


def band_median(mag, freqs, centre_hz, halfwidth_hz):
    lo = np.searchsorted(freqs, centre_hz - halfwidth_hz)
    hi = np.searchsorted(freqs, centre_hz + halfwidth_hz)
    if hi <= lo or hi > len(mag):
        return np.nan
    return float(np.median(mag[lo:hi]))


def odd_even_scores(mag, freqs, f_low, nyquist):
    """dB by which the odd / even multiples of f_low stand above the local floor.

    The floor for multiple k is measured in the two gaps either side, at
    (k -/+ 0.5)*f_low, taking the smaller of the two medians: a gap that happens
    to contain a neighbouring formant peak would inflate the floor and hide a
    real partial, so the quieter gap is the honest estimate.
    """
    half = 0.35 * f_low          # search band; gaps sit at +/-0.5*f_low, so no overlap
    gap_half = 0.15 * f_low

    def score(ks):
        out = []
        for k in ks:
            f = k * f_low
            if f + half >= nyquist:
                continue
            peak = band_peak(mag, freqs, f, half)
            g1 = band_median(mag, freqs, f - 0.5 * f_low, gap_half)
            g2 = band_median(mag, freqs, f + 0.5 * f_low, gap_half)
            floor = np.nanmin([g1, g2])
            if not np.isfinite(peak) or not np.isfinite(floor) or floor <= 0.0:
                continue
            out.append(20.0 * np.log10(peak / floor))
        return float(np.median(out)) if len(out) >= 2 else np.nan

    return score(ODD_K), score(EVEN_K)


def envelope_deficit(mag, freqs, f_low, nyquist):
    """dB by which f_low's odd partials sit BELOW the envelope of the even ones.

    For odd k the two neighbours k-1 and k+1 are even multiples of f_low, i.e.
    harmonics of f_high.  Interpolating their levels in dB gives the spectral
    envelope's expected level at k*f_low.  Near 0 means the odd partial is a
    full member of a smooth harmonic series -> f_low is the fundamental.  A
    large positive deficit means it is a subharmonic riding on the f_high
    series -> f_high is the fundamental.
    """
    half = 0.35 * f_low
    out = []
    for k in ENVELOPE_K:
        if (k + 1) * f_low + half >= nyquist:
            continue
        levels = []
        for kk in (k - 1, k, k + 1):
            pk = band_peak(mag, freqs, kk * f_low, half)
            if not np.isfinite(pk) or pk <= 0.0:
                levels = None
                break
            levels.append(20.0 * np.log10(pk))
        if levels is None:
            continue
        out.append(0.5 * (levels[0] + levels[2]) - levels[1])
    return float(np.median(out)) if out else np.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take")
    ap.add_argument("--wavdir", default="testdata/vocals")
    ap.add_argument("--labeldir", default="testdata/vocals")
    ap.add_argument("--dump", required=True)
    ap.add_argument("--max-control", type=int, default=1500)
    ap.add_argument("--csv", default=None, help="write the per-frame table here")
    args = ap.parse_args()

    labels = load_labels(os.path.join(args.labeldir, "%s.f0.csv" % args.take))
    dump = load_dump(args.dump)

    times = dump["block_end_s"] - OFFSET_S
    lf = label_at(labels, times)
    df = dump["f0_hz"]
    dv = dump["voiced"].astype(bool)

    scored = np.isfinite(lf) & dv & (df > 0.0)
    err = np.full(len(df), np.nan)
    err[scored] = cents(df[scored], lf[scored])
    aerr = np.abs(err)

    disputed = scored & (np.abs(aerr - 1200.0) <= 20.0)
    agreed = scored & (aerr <= 50.0)

    print("=== %s: frame accounting ===" % args.take)
    print("  scored frames                : %d" % int(scored.sum()))
    print("  agreed within 50 c (control) : %d" % int(agreed.sum()))
    print("  octave-disputed frames       : %d  (%.2f%% of scored)"
          % (int(disputed.sum()), 100.0 * disputed.sum() / max(1, scored.sum())))
    yin_low = disputed & (err < 0)
    print("     YIN below label (YIN low) : %d" % int(yin_low.sum()))
    print("     YIN above label (YIN high): %d" % int((disputed & (err > 0)).sum()))

    spec = Spectra(os.path.join(args.wavdir, "%s.wav" % args.take))
    nyq = spec.sr / 2.0

    # ---- calibration on the agreed frames --------------------------------
    idx_agreed = np.flatnonzero(agreed)
    if len(idx_agreed) > args.max_control:
        idx_agreed = idx_agreed[np.linspace(0, len(idx_agreed) - 1,
                                            args.max_control).astype(int)]

    pos, neg, pos_f0 = [], [], []
    pos_def, neg_def = [], []
    for i in idx_agreed:
        mag = spec.magnitude(times[i])
        if mag is None:
            continue
        f0 = float(np.sqrt(df[i] * lf[i]))       # the agreed fundamental
        # POSITIVE control: f0 really is the fundamental, so its odd multiples
        # (f0, 3f0, 5f0) are real partials and must stand above the floor.
        o, e = odd_even_scores(mag, spec.freqs, f0, nyq)
        if np.isfinite(o) and np.isfinite(e):
            pos.append(o)
            pos_f0.append(f0)
            d_ = envelope_deficit(mag, spec.freqs, f0, nyq)
            if np.isfinite(d_):
                pos_def.append(d_)
        # NEGATIVE control: pretend f0/2 were the fundamental.  Its odd
        # multiples (f0/2, 3f0/2, 5f0/2) are half-harmonics that do not exist.
        o2, e2 = odd_even_scores(mag, spec.freqs, 0.5 * f0, nyq)
        if np.isfinite(o2) and np.isfinite(e2):
            neg.append(o2)
            d2_ = envelope_deficit(mag, spec.freqs, 0.5 * f0, nyq)
            if np.isfinite(d2_):
                neg_def.append(d2_)

    pos = np.array(pos)
    neg = np.array(neg)
    print("\n=== calibration on frames where YIN and the labels AGREE ===")
    print("  positive control (odd multiples of the agreed f0, which are real):")
    print("    n=%d  median %.1f dB  p10 %.1f  p25 %.1f" %
          (len(pos), np.median(pos), np.percentile(pos, 10), np.percentile(pos, 25)))
    print("  negative control (odd multiples of agreed f0/2, which do not exist):")
    print("    n=%d  median %.1f dB  p75 %.1f  p90 %.1f" %
          (len(neg), np.median(neg), np.percentile(neg, 75), np.percentile(neg, 90)))

    # Threshold: the point minimising total misclassification of the two
    # controls.  Reported with its error rates, not asserted.
    cands = np.linspace(0.0, 30.0, 601)
    best, best_t = None, None
    for t in cands:
        e = (pos < t).mean() + (neg >= t).mean()
        if best is None or e < best:
            best, best_t = e, t
    pd_ = np.array(pos_def)
    nd_ = np.array(neg_def)
    print("  envelope deficit, positive control (odd partials of a real f0):")
    print("    n=%d  median %.1f dB  p75 %.1f  p90 %.1f" %
          (len(pd_), np.median(pd_), np.percentile(pd_, 75), np.percentile(pd_, 90)))
    print("  envelope deficit, negative control (odd partials of f0/2):")
    print("    n=%d  median %.1f dB  p10 %.1f  p25 %.1f" %
          (len(nd_), np.median(nd_), np.percentile(nd_, 10), np.percentile(nd_, 25)))
    dcands = np.linspace(-5.0, 40.0, 901)
    dbest, dbest_t = None, None
    for t in dcands:
        e_ = (pd_ >= t).mean() + (nd_ < t).mean()
        if dbest is None or e_ < dbest:
            dbest, dbest_t = e_, t
    print("  deficit threshold %.1f dB -> positive-control miss %.2f%%, "
          "negative-control false-alarm %.2f%%"
          % (dbest_t, 100 * (pd_ >= dbest_t).mean(), 100 * (nd_ < dbest_t).mean()))

    fp = float((neg >= best_t).mean())
    fn = float((pos < best_t).mean())
    print("  threshold %.1f dB  ->  positive-control miss %.2f%%, "
          "negative-control false-alarm %.2f%%" % (best_t, 100 * fn, 100 * fp))

    # ---- classify the disputed frames ------------------------------------
    rows = []
    for i in np.flatnonzero(disputed):
        mag = spec.magnitude(times[i])
        if mag is None:
            continue
        f_low = min(float(df[i]), float(lf[i]))
        f_high = max(float(df[i]), float(lf[i]))
        o, e = odd_even_scores(mag, spec.freqs, f_low, nyq)
        dfc = envelope_deficit(mag, spec.freqs, f_low, nyq)
        # Two tests, and they are combined conservatively: a frame is only
        # called for f_low when the odd partials are BOTH present (above the
        # floor) and sitting ON the envelope.  Present-but-far-below-envelope
        # is the subharmonic signature and is called for f_high; present but
        # in between is called ambiguous rather than guessed.
        if not np.isfinite(o) or not np.isfinite(e) or e < 6.0:
            verdict = "unusable"
        elif o < best_t:
            verdict = "high_is_f0"         # nothing at the odd multiples at all
        elif not np.isfinite(dfc):
            verdict = "ambiguous"
        elif dfc < dbest_t - 4.0:
            verdict = "low_is_f0"          # odd partials on the envelope
        elif dfc > dbest_t + 4.0:
            verdict = "high_is_f0"         # odd partials are subharmonics
        else:
            verdict = "ambiguous"
        rows.append((times[i], float(df[i]), float(lf[i]), f_low, f_high, o, e,
                     dfc, verdict, "yin" if df[i] < lf[i] else "label"))

    print("\n=== disputed frames, judged on the spectrum ===")
    def tally(sel):
        fy = fl = am = 0
        for r in sel:
            who_low = r[9]
            if r[8] in ("unusable", "ambiguous"):
                am += 1
            elif r[8] == "low_is_f0":
                fy += (who_low == "yin")
                fl += (who_low == "label")
            else:
                fy += (who_low == "label")
                fl += (who_low == "yin")
        return fy, fl, am

    for name, sel in [("ALL disputed", rows),
                      ("YIN reads LOW  (yin = f_low)",
                       [r for r in rows if r[9] == "yin"]),
                      ("YIN reads HIGH (label = f_low)",
                       [r for r in rows if r[9] == "label"])]:
        fy, fl, am = tally(sel)
        n = max(1, len(sel))
        print("  %-32s n=%4d   YIN %4d (%.1f%%)  LABEL %4d (%.1f%%)  "
              "ambiguous %4d (%.1f%%)"
              % (name, len(sel), fy, 100.0 * fy / n, fl, 100.0 * fl / n,
                 am, 100.0 * am / n))

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("time_s,yin_hz,label_hz,f_low,f_high,odd_db,even_db,"
                     "env_deficit_db,verdict,who_low\n")
            for r in rows:
                fh.write("%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%s\n" % r)
        print("  per-frame table -> %s" % args.csv)


if __name__ == "__main__":
    main()
