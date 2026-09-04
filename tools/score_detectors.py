#!/usr/bin/env python3
"""Score OpenTune's pitch detectors against the T1.0 consensus labels.

This is the AC2 / AC7 measurement half of T1.8.  It takes two CSV tracks:

  - the reference labels from tools/label_f0.py (three independent Python
    estimators, 2-of-3 consensus -- see testdata/vocals/MANIFEST.md), and
  - a per-block dump of an engine detector from tools/f0-dump.

and reports, per detector and per take:

  - the cents-error distribution (median, mean, 95th percentile of |error|),
  - the fraction of scored frames inside +/-15 cents  -> AC2's bar,
  - the octave-error rate: estimate within 20 cents of 2x or 0.5x the label,
    which must be under 1%                             -> AC7's bar.

Nothing here feeds back into the labels.  The labels were produced before this
script ran and do not depend on any engine code.

TIME ALIGNMENT.  The label track timestamps each frame at the centre of its
2048-sample analysis window.  The detector dump timestamps each estimate at the
last sample it was handed, but the estimate describes a ~2200-sample span ending
there -- so it must be shifted back by about half that span (~23 ms) before the
two can be differenced.  Rather than assume a single number, this script scans a
range of offsets, reports the error at each, and states which one it scored at.
That turns a hidden assumption into a printed table.

Usage:
    .venv/bin/python tools/score_detectors.py <dumpdir> [--json out.json]
"""

from __future__ import annotations

import argparse
import json
import os

import numpy as np

# The six real takes of the T1.0 reference set.  `--cases` overrides this with
# any other set of case ids -- the synthetic ground-truth set in
# testdata/synthetic uses the identical CSV shape, so it scores unchanged.
TAKES = ["take01", "take02", "take03", "take04", "take05", "take06"]
DETECTORS = ["yin", "autocorr"]

# Offset applied to the detector's block-end timestamp, in seconds, to place its
# estimate at the centre of the audio it actually analysed.  At 48 kHz the
# detectors buffer 1478 comparison samples plus a 739-sample maximum lag reach =
# D11, corrected 2026-09-04.  The old value was 1108.5/48000 = 23.09 ms, half
# the 2217-sample BUFFER.  But the buffer is not the analysis window.  YIN sums
# d(tau) over buffer[0..W) with W = kAnalysisWindowLagMultiple * maxLag = 2*739
# = 1478; the further 740 samples exist only so the widest lag's comparison
# stays in bounds.  An estimate therefore describes the centre of that window,
# which sits (bufferSize-1) - W/2 = 2217 - 739 = 1478 samples behind the newest
# sample handed in -- 30.79 ms at 48 kHz, not 23.09.
#
# Half-the-buffer would be right for a detector with W = maxLag; that geometry
# gives 23.10 ms exactly, which is why the old constant looked plausible.  It
# was correct for a window this detector no longer uses.
#
# Confirmed from both directions: the geometry above, and an empirical scan
# against the synthetic set's exact labels, which put the optimum at
# 29.25-30.25 ms -- one constant across a tenfold range of contour velocity.
DEFAULT_OFFSET_S = 1478.0 / 48000.0

# Offsets scanned when reporting alignment sensitivity, in milliseconds.
SCAN_MS = [0.0, 5.0, 10.0, 15.0, 18.0, 20.0, 21.0, 22.0, 23.09, 24.0, 25.0, 26.0, 28.0, 30.0, 35.0, 40.0]


def cents(a, b):
    return 1200.0 * np.log2(np.asarray(a, float) / np.asarray(b, float))


def load_labels(path):
    return np.genfromtxt(path, delimiter=",", names=True, skip_header=3, dtype=None,
                         encoding=None)


def load_dump(path):
    return np.genfromtxt(path, delimiter=",", names=True, dtype=None, encoding=None)


def label_at(labels, times, require_three_way=False):
    """Interpolate the label track onto arbitrary times.

    Nearest-frame matching would quantise the comparison to the label hop
    (5.33 ms).  That is not harmless here: these takes move at a median ~500
    cents/second, so half a hop is already ~1.3 cents of manufactured error --
    a tenth of the +/-15 cent bar being tested.

    So interpolate instead, in the log-frequency domain, and ONLY between two
    adjacent label frames that are both voiced and within 100 cents of each
    other.  Interpolating across a note boundary or across an ambiguous gap
    would invent a label, which is the one thing this task forbids; those
    frames are returned as NaN and dropped from scoring.
    """
    lt = labels["time_s"]
    lf = labels["f0_hz"]
    lv = labels["voiced"].astype(bool)
    na = labels["n_agree"]

    ok = lv & np.isfinite(lf)
    if require_three_way:
        ok &= na >= 3

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
        interp = np.exp(
            (1.0 - frac) * np.log(lf[left]) + frac * np.log(lf[right])
        )
    return np.where(usable, interp, np.nan)


def score(labels, dump, offset_s, require_three_way=False):
    times = dump["block_end_s"] - offset_s
    lf = label_at(labels, times, require_three_way)

    df = dump["f0_hz"]
    dv = dump["voiced"].astype(bool)

    label_ok = np.isfinite(lf)

    # AC2 is a statement about voiced frames: score where the reference says
    # voiced AND the detector committed to an estimate.  Frames the detector
    # declared unvoiced are excluded from the cents statistics (there is no
    # frequency to score) but counted separately as a recall figure, so a
    # detector cannot buy a good cents score by staying silent.
    scored = label_ok & dv & (df > 0.0)
    if scored.sum() == 0:
        return None

    err = cents(df[scored], lf[scored])
    aerr = np.abs(err)
    octave = np.abs(aerr - 1200.0) <= 20.0

    return {
        "label_voiced_frames": int(label_ok.sum()),
        "scored_frames": int(scored.sum()),
        "voiced_recall": round(float((label_ok & dv).sum() / max(1, int(label_ok.sum()))), 4),
        "median_signed_cents": round(float(np.median(err)), 2),
        "median_abs_cents": round(float(np.median(aerr)), 2),
        "mean_abs_cents": round(float(np.mean(aerr)), 2),
        "p95_abs_cents": round(float(np.percentile(aerr, 95)), 2),
        "within_15_cents": round(float((aerr <= 15.0).mean()), 4),
        "within_50_cents": round(float((aerr <= 50.0).mean()), 4),
        "octave_error_rate": round(float(octave.mean()), 4),
        "gross_error_rate": round(float((aerr > 337.0).mean()), 4),  # >20% of f0
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumpdir")
    ap.add_argument("--labeldir", default="testdata/vocals")
    ap.add_argument("--json", default=None)
    ap.add_argument("--cases", default=None,
                    help="comma-separated case ids to score instead of the six takes")
    args = ap.parse_args()

    takes = args.cases.split(",") if args.cases else TAKES

    results = []
    # Alignment sensitivity, pooled over all six takes.  This is reported, not
    # optimised over: the scored offset below is the one derived from the
    # detectors' buffer geometry, and this table exists so a reader can see how
    # much that choice is worth.  It is worth a lot here -- the takes move at a
    # median ~500 cents/second, so 2 ms of misalignment is ~1 cent of error all
    # by itself.
    print("=== alignment sensitivity, pooled over all takes ===")
    print("  offset_ms   yin:med|c| yin:<=15c   autocorr:med|c| autocorr:<=15c")
    labs = {t: load_labels(os.path.join(args.labeldir, "%s.f0.csv" % t)) for t in takes}
    dmps = {(t, d): load_dump(os.path.join(args.dumpdir, "%s.%s.csv" % (t, d)))
            for t in takes for d in DETECTORS}
    for ms in SCAN_MS:
        cells = []
        for det in DETECTORS:
            meds, w15, wts = [], [], []
            for t in takes:
                r = score(labs[t], dmps[(t, det)], ms / 1000.0)
                meds.append(r["median_abs_cents"] * r["scored_frames"])
                w15.append(r["within_15_cents"] * r["scored_frames"])
                wts.append(r["scored_frames"])
            tot = float(sum(wts))
            cells.append("%10.2f %10.4f" % (sum(meds) / tot, sum(w15) / tot))
        print("  %9.2f   %s   %s" % (ms, cells[0], cells[1]))
    print("  scored at offset %.2f ms (centre of the 1478-sample analysis window)\n"
          % (DEFAULT_OFFSET_S * 1000.0))

    for take in takes:
        lab = load_labels(os.path.join(args.labeldir, "%s.f0.csv" % take))
        for det in DETECTORS:
            dmp = load_dump(os.path.join(args.dumpdir, "%s.%s.csv" % (take, det)))
            for three in (False, True):
                s = score(lab, dmp, DEFAULT_OFFSET_S, require_three_way=three)
                if s is None:
                    continue
                s["take"] = take
                s["detector"] = det
                s["label_set"] = "3-way" if three else "2-of-3"
                results.append(s)

    cols = ["take", "detector", "label_set", "scored_frames", "median_abs_cents",
            "mean_abs_cents", "p95_abs_cents", "within_15_cents",
            "octave_error_rate", "gross_error_rate", "voiced_recall"]
    print("=== per-take scores ===")
    print("\t".join(c for c in cols))
    for r in results:
        print("\t".join(str(r[c]) for c in cols))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(results, fh, indent=2)


if __name__ == "__main__":
    main()
