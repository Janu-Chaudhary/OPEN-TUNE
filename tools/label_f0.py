#!/usr/bin/env python3
"""Derive reference f0 labels for the vocal test set (T1.0).

WHY THIS TOOL EXISTS
--------------------
To say "the detector is accurate to +/-15 cents" we need something to compare
its output against.  The obvious shortcut -- run OpenTune's own YinDetector and
call its output the label -- is circular: it would measure the detector's
agreement with itself and report the number as accuracy.  A detector that is
consistently an octave low would score 100%.

So the labels here come from THREE pitch estimators written from scratch in
this file, chosen because they fail in *different* ways:

  1. Cepstrum          - frequency domain, log-spectrum periodicity.  Sensitive
                         to spectral tilt and to formants; degrades on noisy or
                         inharmonic frames.
  2. Harmonic Product  - frequency domain, harmonic-comb matching.  Strong on
     Spectrum (HPS)      harmonic-rich tones; its classic failure is an
                         octave-DOWN slip when the true f0's odd harmonics are
                         weak.
  3. NCCF              - time domain, normalised cross-correlation with an
                         explicit sub-harmonic guard.  Its failures follow the
                         waveform (jitter, breathiness), not the spectrum.

A frame only gets a label where at least TWO of the three agree within
CONSENSUS_TOLERANCE_CENTS.  Frames where they do not agree are marked
`ambiguous`, get NO f0 value, and are excluded from scoring -- but they are
counted and reported, because the disagreement rate is itself a measurement of
how hard the material is.  A gap is never filled with a guess.

None of this is ground truth.  It is three algorithms cross-checking each
other.  Where all three share a bias (and they can: all three are ultimately
looking for periodicity in the same signal) the consensus is confidently wrong.
The MANIFEST states that limitation in the open.

FORMAT-TAG SAFETY
-----------------
These WAVs are audioFormat=3 (IEEE float), 32-bit.  A reader that assumes
16-bit PCM reinterprets each float as two ints: it manufactures broadband
noise and halves the duration.  That bug produced a confident, entirely wrong
verdict about this audio before it was caught (tasks.md D9).  So the fmt chunk
is parsed and honoured, and an unhandled format raises rather than guessing.

MEMORY
------
tools/analyze.py is OOM-killed on anything longer than a few seconds (D8).
These takes are 41-96 seconds.  This tool therefore never materialises a
whole-file 2-D frame array: it walks the file in overlapping segments, builds
the frame matrix for one segment at a time, reduces it to a handful of 1-D
per-frame results, and drops it.  Peak memory is set by CHUNK_FRAMES, not by
file length.

Usage:
    .venv/bin/python tools/label_f0.py testdata/vocals/take01.wav ...
    .venv/bin/python tools/label_f0.py --stats-json out.json testdata/vocals/*.wav
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

import numpy as np

# ---------------------------------------------------------------------------
# Analysis constants.  Every one of these is a decision; the reason is next to
# the number.
# ---------------------------------------------------------------------------

# Pitch search range, in Hz.  Matches specs.md section 6 (65-1100 Hz), which is
# the range the engine claims to detect.  Labels outside it would be untestable.
F0_MIN_HZ = 65.0
F0_MAX_HZ = 1100.0

# Analysis window, in samples at 48 kHz.  2048 samples = 42.7 ms = 2.8 periods
# of a 65 Hz voice.  Any shorter and the lowest notes have too few periods for
# either the autocorrelation or the cepstrum to see; any longer and a fast
# slide (meend) is smeared across the window.
WINDOW = 2048

# Hop, in samples.  256 = 5.33 ms = 187.5 frames/second.  Chosen to equal the
# engine's nominal audio block size so that one label frame corresponds to one
# detector process() call, and the two tracks line up without resampling.
HOP = 256

# Consensus tolerance, in cents.  Two estimators "agree" when they are within
# this much of each other.
#
# Why 30 and not 5 or 50:
#   - It must be far below the smallest *structural* error we need to catch.
#     The confusions that matter are the octave (1200 c), the twelfth (1902 c)
#     and the fifth (702 c).  30 cents cannot be mistaken for any of them.
#   - It must be above the intrinsic precision of the three estimators, or
#     honest agreement gets thrown away as disagreement.  The cepstrum's
#     quefrency peak is one sample wide (at 200 Hz, one quefrency sample is
#     ~8 cents even after parabolic interpolation), and HPS is limited by its
#     FFT bin grid before harmonic refinement.  Below ~20 cents the two
#     spectral methods start disagreeing on frames where nothing is wrong.
#   - It must be small enough that the label is still usable for a +/-15 cent
#     accuracy bar.  It is not comfortably so, and this is the honest weak
#     point of the whole method: a +/-30 c agreement window means a label can
#     itself be ~15 c off.  The tool therefore MEASURES the realised spread
#     (median pairwise disagreement among agreeing estimators) and reports it,
#     so the residual label error is a number rather than a hope.
CONSENSUS_TOLERANCE_CENTS = 30.0

# Frames per processing chunk.  Sets peak memory: CHUNK_FRAMES x FFT_LONG//2
# complex values dominate.  1024 frames keeps the HPS spectrum block near
# 30-60 MB, which is what stops this tool meeting analyze.py's fate.
CHUNK_FRAMES = 1024

# FFT sizes.  FFT_CEPS is the cepstrum transform length; FFT_LONG is the
# zero-padded length used for HPS, where a fine bin grid matters because the
# harmonic comb is evaluated by interpolation.
FFT_CEPS = 4096
FFT_LONG = 16384

# NCCF peak acceptance: a local correlation peak counts as "the period" if it
# reaches this fraction of the frame's best correlation.  Justified against this
# repo's own T1.2 measurement -- see estimate_nccf().
PEAK_ACCEPT_FRACTION = 0.88

# Number of harmonics summed by HPS.  Five is the usual choice: enough for the
# comb to be selective, few enough that the 5th harmonic of a 1100 Hz note
# (5.5 kHz) is still well inside a 24 kHz Nyquist and still has energy in a
# phone-mic vocal recording.
HPS_HARMONICS = 5

# Voicing energy gate.  A frame is a *candidate* for voiced only if its RMS
# clears both an absolute floor and a gate set relative to the take's own loud
# passages.
#
# The relative form matters.  An earlier version set the gate at
# (5th-percentile RMS + 10 dB), which assumes the quietest 5% of frames are
# silence.  On these recordings that is false: measured, take04's RMS runs from
# -35 dBFS at the 0.1st percentile to -10 dBFS at the 99th -- a 25 dB spread
# with no true silence anywhere (phone mic, room tone, no gate applied at
# capture).  That percentile rule put the gate at -17.7 dBFS and discarded
# every quiet sung note in the take.  Referencing the LOUD end instead is the
# standard noise-gate shape and does not assume silence exists.
VOICING_ABS_FLOOR_DB = -55.0  # below this it is room tone on any phone mic
VOICING_RANGE_DB = 30.0  # frames more than this far below the take's p95 RMS

EPS = 1e-12


# ---------------------------------------------------------------------------
# WAV reading -- fmt-chunk-honest, streaming.
# ---------------------------------------------------------------------------

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE


class WavInfo:
    """Everything needed to read sample data, taken from the fmt chunk."""

    def __init__(self, path, audio_format, channels, sample_rate, bits, data_off, data_len):
        self.path = path
        self.audio_format = audio_format
        self.channels = channels
        self.sample_rate = sample_rate
        self.bits = bits
        self.data_offset = data_off
        self.data_bytes = data_len
        self.bytes_per_frame = channels * bits // 8
        self.num_frames = data_len // self.bytes_per_frame
        self.duration_s = self.num_frames / float(sample_rate)

    @property
    def dtype(self):
        if self.audio_format == WAVE_FORMAT_IEEE_FLOAT and self.bits == 32:
            return np.dtype("<f4")
        if self.audio_format == WAVE_FORMAT_IEEE_FLOAT and self.bits == 64:
            return np.dtype("<f8")
        if self.audio_format == WAVE_FORMAT_PCM and self.bits == 16:
            return np.dtype("<i2")
        if self.audio_format == WAVE_FORMAT_PCM and self.bits == 32:
            return np.dtype("<i4")
        # Deliberately loud.  Guessing here is exactly the D9 bug.
        raise AssertionError(
            "Unhandled WAV format in %s: audioFormat=%d bits=%d. "
            "Refusing to guess -- misreading the format tag manufactures "
            "broadband noise and halves the duration (tasks.md D9)."
            % (self.path, self.audio_format, self.bits)
        )

    @property
    def scale(self):
        """Multiplier converting raw samples to float in [-1, 1]."""
        if self.audio_format == WAVE_FORMAT_IEEE_FLOAT:
            return 1.0
        return 1.0 / float(1 << (self.bits - 1))


def read_wav_header(path):
    """Parse RIFF/WAVE chunks and return a WavInfo.  Reads no sample data."""
    with open(path, "rb") as fh:
        riff = fh.read(12)
        if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise AssertionError("%s is not a RIFF/WAVE file" % path)

        fmt = None
        while True:
            head = fh.read(8)
            if len(head) < 8:
                break
            cid, csize = struct.unpack("<4sI", head)
            body_start = fh.tell()
            if cid == b"fmt ":
                body = fh.read(csize)
                audio_format, channels, sample_rate, _byte_rate, _align, bits = struct.unpack(
                    "<HHIIHH", body[:16]
                )
                if audio_format == WAVE_FORMAT_EXTENSIBLE and csize >= 40:
                    # The real format tag lives in the GUID's first two bytes.
                    audio_format = struct.unpack("<H", body[24:26])[0]
                fmt = (audio_format, channels, sample_rate, bits)
            elif cid == b"data":
                if fmt is None:
                    raise AssertionError("%s: data chunk before fmt chunk" % path)
                avail = os.path.getsize(path) - body_start
                return WavInfo(path, fmt[0], fmt[1], fmt[2], fmt[3], body_start,
                               min(csize, avail))
            fh.seek(body_start + csize + (csize & 1))
    raise AssertionError("%s: no data chunk found" % path)


def read_samples(info, start_frame, count):
    """Read `count` frames from `start_frame`, downmixed to mono float64.

    Reads only the bytes asked for -- this is what keeps a 96-second file from
    ever being resident in full.
    """
    count = max(0, min(count, info.num_frames - start_frame))
    if count == 0:
        return np.zeros(0, dtype=np.float64)
    dt = info.dtype
    offset = info.data_offset + start_frame * info.bytes_per_frame
    raw = np.fromfile(info.path, dtype=dt, count=count * info.channels, offset=offset)
    x = raw.astype(np.float64) * info.scale
    if info.channels > 1:
        x = x.reshape(-1, info.channels).mean(axis=1)
    return x


# ---------------------------------------------------------------------------
# Small shared helpers.
# ---------------------------------------------------------------------------


def cents(a, b):
    """Interval from b to a, in cents.  NaN-safe (NaN in -> NaN out)."""
    with np.errstate(divide="ignore", invalid="ignore"):
        return 1200.0 * np.log2(np.asarray(a, dtype=float) / np.asarray(b, dtype=float))


def parabolic_peak(y_left, y_mid, y_right):
    """Sub-sample offset of a parabola's vertex through three samples.

    Standard three-point fit.  Returned offset is in [-0.5, 0.5] for a genuine
    local maximum; clipped so a flat or degenerate triple cannot fling the
    estimate into a neighbouring bin.
    """
    denom = y_left - 2.0 * y_mid + y_right
    with np.errstate(divide="ignore", invalid="ignore"):
        delta = 0.5 * (y_left - y_right) / denom
    delta = np.where(np.isfinite(delta), delta, 0.0)
    return np.clip(delta, -0.5, 0.5)


def frame_signal(x, num_frames, hop, window):
    """Non-copying view of `x` as (num_frames, window) with stride `hop`."""
    return np.lib.stride_tricks.as_strided(
        x,
        shape=(num_frames, window),
        strides=(x.strides[0] * hop, x.strides[0]),
        writeable=False,
    )


# ---------------------------------------------------------------------------
# Estimator 1: cepstrum.
# ---------------------------------------------------------------------------


def estimate_cepstrum(frames_win, sr):
    """Real-cepstrum f0, one value per frame (NaN where no peak is credible).

    A voiced spectrum is a harmonic comb: energy at f0, 2*f0, 3*f0, ...  Take
    the log magnitude spectrum and that comb becomes an additive ripple with
    period f0 Hz *along the frequency axis*.  An inverse FFT of the log
    spectrum ("cepstrum") therefore shows a peak at "quefrency" 1/f0 seconds --
    i.e. at lag = sr/f0 samples.  The log is what makes this work: it turns the
    source-filter product (glottal comb x vocal-tract envelope) into a sum, so
    the slow formant envelope lands at low quefrency and the fast harmonic
    ripple lands at the period, separating the two.

    This is a frequency-domain measurement of a frequency-domain structure, so
    it fails on frames the time-domain method handles fine (and vice versa) --
    which is the entire point of running it.
    """
    spec = np.fft.rfft(frames_win, n=FFT_CEPS, axis=1)
    log_mag = np.log(np.abs(spec) + EPS)
    ceps = np.fft.irfft(log_mag, n=FFT_CEPS, axis=1)

    q_min = int(np.floor(sr / F0_MAX_HZ))
    q_max = int(np.ceil(sr / F0_MIN_HZ))
    band = ceps[:, q_min:q_max + 1]
    n_frames, n_q = band.shape

    # RAHMONIC SUPPORT.  Taking the tallest cepstral peak outright is not safe:
    # the vocal-tract envelope leaves its own ripple in the log spectrum, and on
    # a low note that ripple can out-peak the true period.  (Measured: a
    # synthetic 80 Hz harmonic tone was read as 889 Hz by the bare-peak rule.)
    #
    # But a genuine period P leaves peaks at P, 2P, 3P ... in the cepstrum
    # ("rahmonics"), because the harmonic ripple is not a pure sinusoid along
    # the frequency axis.  An envelope artefact has no such family.  So score a
    # quefrency by its own height PLUS half the height at 2q -- a cheap test for
    # "is there a period family here?" that costs one extra gather.
    score = band.copy()
    q_abs = q_min + np.arange(n_q)
    two_q = 2 * q_abs
    inside = two_q <= q_max
    score[:, inside] += 0.5 * ceps[:, two_q[inside]]

    idx = np.argmax(score, axis=1)
    rows = np.arange(n_frames)
    peak = band[rows, idx]

    # Parabolic refinement is done on the RAW cepstrum, not the score: the
    # score's +2q term is there to choose the right peak, not to locate it.
    safe = np.clip(idx, 1, n_q - 2)
    delta = parabolic_peak(band[rows, safe - 1], band[rows, safe], band[rows, safe + 1])
    delta = np.where(idx == safe, delta, 0.0)

    quefrency = (q_min + idx).astype(float) + delta
    f0 = sr / quefrency

    # Reject frames with no real peak: the cepstral peak must stand above the
    # local cepstral background, or we are reading noise.  The threshold is a
    # relative one (peak vs the band's own std) so it adapts to level.
    background = band.std(axis=1) + EPS
    credible = peak > 2.5 * background
    return np.where(credible, f0, np.nan)


# ---------------------------------------------------------------------------
# Estimator 2: Harmonic Product Spectrum.
# ---------------------------------------------------------------------------


def estimate_hps(frames_win, sr):
    """HPS f0 with a harmonic-sum refinement, one value per frame.

    HPS asks: at which f does the spectrum have energy at f AND 2f AND 3f AND
    ...?  Multiplying the spectrum by decimated copies of itself (equivalently,
    summing log magnitudes at h*f) makes every harmonic vote for the true f0,
    while a spurious peak at 2*f0 gets no vote from the odd harmonics.

    Its characteristic failure is the mirror image: when the true f0's own
    partials are weak or the recording is bass-rolled-off (a phone mic is), the
    comb at f0/2 can score well because every harmonic of f0 is also a harmonic
    of f0/2 -- an octave-DOWN slip.  That is a different failure mode from the
    cepstrum's and from the correlator's, which is why it earns its place.

    Two stages: a coarse comb search on the FFT bin grid, then a fine search on
    a continuous f0 grid (+/-6%) using linear interpolation into the magnitude
    spectrum, because the bin grid alone (2.93 Hz at FFT_LONG) is ~25 cents at
    200 Hz -- too coarse to be a label.
    """
    n_frames = frames_win.shape[0]
    mag = np.abs(np.fft.rfft(frames_win, n=FFT_LONG, axis=1))
    bin_hz = sr / FFT_LONG
    log_mag = np.log(mag + EPS)

    # --- coarse: decimate-and-add on the bin grid ---
    k_max = log_mag.shape[1] // HPS_HARMONICS
    comb = np.zeros((n_frames, k_max))
    for h in range(1, HPS_HARMONICS + 1):
        comb += log_mag[:, ::h][:, :k_max]

    k_lo = max(1, int(np.floor(F0_MIN_HZ / bin_hz)))
    k_hi = min(k_max - 1, int(np.ceil(F0_MAX_HZ / bin_hz)))
    band = comb[:, k_lo:k_hi + 1]
    k_coarse = k_lo + np.argmax(band, axis=1)
    f0_coarse = k_coarse.astype(float) * bin_hz

    # --- fine: continuous harmonic-sum search around the coarse estimate ---
    # 121 steps across +/-6% is a 0.1% grid = 1.7 cents, comfortably finer than
    # the 30-cent consensus tolerance.
    ratios = np.linspace(0.94, 1.06, 121)
    cand = f0_coarse[:, None] * ratios[None, :]  # (n_frames, 121)
    score = np.zeros_like(cand)
    rows = np.arange(n_frames)[:, None]
    n_bins = mag.shape[1]
    for h in range(1, HPS_HARMONICS + 1):
        pos = cand * h / bin_hz
        lo = np.clip(np.floor(pos).astype(np.int64), 0, n_bins - 2)
        frac = np.clip(pos - lo, 0.0, 1.0)
        interp = mag[rows, lo] * (1.0 - frac) + mag[rows, lo + 1] * frac
        score += np.log(interp + EPS)
    best = np.argmax(score, axis=1)
    f0 = cand[np.arange(n_frames), best]

    # Reject frames where the comb has no preference -- a flat score surface
    # means there is no harmonic structure to lock onto.
    contrast = band.max(axis=1) - band.mean(axis=1)
    credible = contrast > 0.5 * HPS_HARMONICS
    f0 = np.where(credible & (f0 >= F0_MIN_HZ) & (f0 <= F0_MAX_HZ), f0, np.nan)
    return f0


# ---------------------------------------------------------------------------
# Estimator 3: NCCF (time domain).
# ---------------------------------------------------------------------------


def estimate_nccf(frames_raw, sr):
    """Normalised cross-correlation f0, shortest-accepted-peak selection.

    r(tau) = sum_j x[j] x[j+tau] / sqrt( sum_j x[j]^2 * sum_j x[j+tau]^2 )

    over a fixed comparison length M, so r is a true correlation coefficient in
    [-1, 1] and is not biased downward at long lags the way a raw
    autocorrelation is.  A periodic signal peaks at its period -- and equally at
    every multiple of it, which is the ambiguity every time-domain method has to
    resolve somehow.

    The rule here is deliberately NOT YIN's (this must not be a re-derivation of
    the detector under test): among the local maxima of r, take the shortest lag
    that reaches PEAK_ACCEPT_FRACTION of the frame's best correlation.  See the
    comment on the selection block below for why that threshold, and for how it
    differs from both YIN and AutocorrelationDetector.
    """
    n_frames, w = frames_raw.shape
    lag_min = int(np.floor(sr / F0_MAX_HZ))
    lag_max = int(np.ceil(sr / F0_MIN_HZ))
    m = w - lag_max  # comparison length, constant across lags

    x = frames_raw - frames_raw.mean(axis=1, keepdims=True)

    # Cross-correlation of the leading M samples against the whole frame, via
    # FFT (O(w log w) instead of O(M * lags)).
    nfft = 1 << int(np.ceil(np.log2(w + m)))
    a = np.zeros((n_frames, nfft))
    a[:, :m] = x[:, :m]
    fa = np.fft.rfft(a, axis=1)
    fb = np.fft.rfft(x, n=nfft, axis=1)
    corr = np.fft.irfft(np.conj(fa) * fb, n=nfft, axis=1)[:, :lag_max + 1]

    # Energies: e0 for the leading segment, e_tau for each shifted segment.
    sq = np.concatenate([np.zeros((n_frames, 1)), np.cumsum(x * x, axis=1)], axis=1)
    e0 = sq[:, m] - sq[:, 0]
    lags = np.arange(lag_max + 1)
    e_tau = sq[:, lags + m] - sq[:, lags]

    with np.errstate(divide="ignore", invalid="ignore"):
        r = corr / np.sqrt(np.maximum(e0[:, None] * e_tau, EPS))
    r = np.nan_to_num(r, nan=-1.0, posinf=-1.0, neginf=-1.0)

    search = r[:, lag_min:lag_max + 1]
    rmax = search.max(axis=1)

    # PEAK SELECTION.  A perfectly periodic signal gives r = 1.0 at P, 2P, 3P,
    # 4P ... alike; argmax then lands on whichever multiple wins by floating
    # point noise.  (Measured: a synthetic 300 Hz tone gave argmax at lag 640 =
    # 4P.)  So do not take the global maximum at all.
    #
    # Rule: among the local maxima of r, take the SHORTEST lag whose correlation
    # reaches PEAK_ACCEPT_FRACTION of the best.  The shortest lag that still
    # correlates almost perfectly is the actual period; every longer one is a
    # multiple of it.
    #
    # The risk of a short-lag rule is the opposite error -- locking onto a
    # HARMONIC when the fundamental is weak, which is exactly the failure this
    # repo measured for first-peak autocorrelation (docs/decisions/0003).  The
    # same measurement sets the threshold safely: T1.2 found r(P/2) = 0.20 for a
    # removed fundamental and 0.59 for an H2-dominant signal, both far under
    # 0.88, while a true multiple sits at essentially r(P).  So 0.88 separates
    # "genuinely also periodic here" from "merely harmonic-rich".
    #
    # Note this is deliberately a different mechanism from YIN's (CMND plus a
    # fixed absolute threshold) and from AutocorrelationDetector's (first peak
    # of the RAW, unnormalised autocorrelation, no threshold relative to the
    # best).  Normalisation plus a relative accept threshold is the RAPT/Praat
    # lineage, and it fails on different frames than either.
    accept = PEAK_ACCEPT_FRACTION * rmax

    # Local maxima within the search band.
    is_peak = np.zeros_like(search, dtype=bool)
    is_peak[:, 1:-1] = (search[:, 1:-1] >= search[:, :-2]) & (
        search[:, 1:-1] > search[:, 2:]
    )
    good = is_peak & (search >= accept[:, None])

    # Shortest accepted lag; fall back to the global max where nothing qualifies
    # (can happen when the only peak is at a band edge).
    any_good = good.any(axis=1)
    first = np.argmax(good, axis=1)
    fallback = np.argmax(search, axis=1)
    idx = np.where(any_good, first, fallback)
    tau = lag_min + idx
    rmax = search[np.arange(n_frames), idx]

    safe = np.clip(tau, 1, lag_max - 1)
    rows = np.arange(n_frames)
    delta = parabolic_peak(r[rows, safe - 1], r[rows, safe], r[rows, safe + 1])
    tau_ref = safe.astype(float) + delta
    f0 = sr / np.maximum(tau_ref, 1.0)

    # A correlation below 0.4 is not a periodic frame in any useful sense.
    credible = (rmax > 0.4) & (f0 >= F0_MIN_HZ) & (f0 <= F0_MAX_HZ)
    return np.where(credible, f0, np.nan)


# ---------------------------------------------------------------------------
# Consensus.
# ---------------------------------------------------------------------------


def consensus(f_ceps, f_hps, f_nccf, tol_cents=CONSENSUS_TOLERANCE_CENTS):
    """Combine three estimator tracks into labels + an agreement count.

    Returns (label_hz, n_agree, spread_cents).  label_hz is NaN wherever fewer
    than two estimators agree -- never a filled-in guess.  spread_cents is the
    realised disagreement inside the agreeing group, which is the honest
    measure of how precise the label actually is.
    """
    est = np.stack([f_ceps, f_hps, f_nccf], axis=1)  # (n, 3)
    n = est.shape[0]
    label = np.full(n, np.nan)
    n_agree = np.zeros(n, dtype=np.int8)
    spread = np.full(n, np.nan)

    pairs = ((0, 1), (0, 2), (1, 2))
    d = {}
    for i, j in pairs:
        d[(i, j)] = np.abs(cents(est[:, i], est[:, j]))

    all_three = (
        (d[(0, 1)] <= tol_cents) & (d[(0, 2)] <= tol_cents) & (d[(1, 2)] <= tol_cents)
    )

    # 3-way agreement: label is the geometric mean of all three.
    with np.errstate(invalid="ignore"):
        geo3 = np.exp(np.nanmean(np.log(est), axis=1))
    label = np.where(all_three, geo3, label)
    n_agree = np.where(all_three, 3, n_agree).astype(np.int8)
    spread = np.where(
        all_three, np.maximum.reduce([d[(0, 1)], d[(0, 2)], d[(1, 2)]]), spread
    )

    # 2-way: the first pair that agrees, checked in a fixed order so the result
    # is deterministic.  Pairs are ordered cepstrum+nccf first (one spectral,
    # one temporal -- the most independent pair, so their agreement is the
    # strongest evidence), then the two remaining pairs.
    for i, j in ((0, 2), (1, 2), (0, 1)):
        ok = (d[(i, j)] <= tol_cents) & (n_agree == 0)
        geo2 = np.exp(0.5 * (np.log(est[:, i]) + np.log(est[:, j])))
        label = np.where(ok, geo2, label)
        spread = np.where(ok, d[(i, j)], spread)
        n_agree = np.where(ok, 2, n_agree).astype(np.int8)

    return label, n_agree, spread


# ---------------------------------------------------------------------------
# Per-file driver.
# ---------------------------------------------------------------------------


def analyse_file(path, verbose=True):
    info = read_wav_header(path)
    sr = float(info.sample_rate)
    if verbose:
        print(
            "[%s] audioFormat=%d channels=%d rate=%d bits=%d frames=%d duration=%.2f s"
            % (
                os.path.basename(path),
                info.audio_format,
                info.channels,
                info.sample_rate,
                info.bits,
                info.num_frames,
                info.duration_s,
            ),
            file=sys.stderr,
        )

    if info.num_frames < WINDOW:
        raise AssertionError("%s: shorter than one analysis window" % path)

    total_frames = 1 + (info.num_frames - WINDOW) // HOP
    win = np.hanning(WINDOW)

    out_ceps = np.empty(total_frames)
    out_hps = np.empty(total_frames)
    out_nccf = np.empty(total_frames)
    out_rms = np.empty(total_frames)

    done = 0
    while done < total_frames:
        n_this = min(CHUNK_FRAMES, total_frames - done)
        start = done * HOP
        need = (n_this - 1) * HOP + WINDOW
        seg = read_samples(info, start, need)
        if seg.size < need:  # final partial read: pad, frames stay well-defined
            seg = np.concatenate([seg, np.zeros(need - seg.size)])
        frames = frame_signal(seg, n_this, HOP, WINDOW)

        out_rms[done:done + n_this] = np.sqrt(np.mean(frames * frames, axis=1))
        frames_win = frames * win[None, :]
        out_ceps[done:done + n_this] = estimate_cepstrum(frames_win, sr)
        out_hps[done:done + n_this] = estimate_hps(frames_win, sr)
        out_nccf[done:done + n_this] = estimate_nccf(np.ascontiguousarray(frames), sr)

        done += n_this
        del seg, frames, frames_win

    label, n_agree, spread = consensus(out_ceps, out_hps, out_nccf)

    # Voicing: energy gate AND a consensus.  Both are required -- loud noise is
    # not voiced, and a quiet frame that three estimators happen to agree on is
    # more likely to be a numerical accident than a sung note.
    rms_db = 20.0 * np.log10(out_rms + EPS)
    noise_floor_db = float(np.percentile(rms_db, 5))
    loud_db = float(np.percentile(rms_db, 95))
    gate_db = max(VOICING_ABS_FLOOR_DB, loud_db - VOICING_RANGE_DB)
    energy_ok = rms_db > gate_db
    voiced = energy_ok & (n_agree >= 2)

    times = (np.arange(total_frames) * HOP + WINDOW / 2.0) / sr

    return {
        "path": path,
        "info": info,
        "times": times,
        "f0": np.where(voiced, label, np.nan),
        "label_raw": label,
        "voiced": voiced,
        "energy_ok": energy_ok,
        "n_agree": n_agree,
        "spread": spread,
        "ceps": out_ceps,
        "hps": out_hps,
        "nccf": out_nccf,
        "rms_db": rms_db,
        "gate_db": gate_db,
        "noise_floor_db": noise_floor_db,
        "loud_db": loud_db,
    }


def write_csv(res, out_path):
    """One row per analysis frame.  Per-estimator columns are included so a
    human can audit any disagreement without re-running anything."""
    n = len(res["times"])

    def fmt(v):
        return "" if not np.isfinite(v) else "%.4f" % v

    with open(out_path, "w") as fh:
        fh.write(
            "# OpenTune reference f0 labels (T1.0). Algorithm-derived, NOT ground truth.\n"
            "# source=%s sample_rate=%d window=%d hop=%d tolerance_cents=%.1f\n"
            "# consensus of cepstrum + HPS + NCCF; f0_hz is blank where <2 agree.\n"
            % (os.path.basename(res["path"]), res["info"].sample_rate, WINDOW, HOP,
               CONSENSUS_TOLERANCE_CENTS)
        )
        fh.write(
            "time_s,f0_hz,voiced,consensus,n_agree,f0_cepstrum_hz,f0_hps_hz,"
            "f0_nccf_hz,spread_cents,rms_db\n"
        )
        for i in range(n):
            fh.write(
                "%.6f,%s,%d,%s,%d,%s,%s,%s,%s,%.2f\n"
                % (
                    res["times"][i],
                    fmt(res["f0"][i]),
                    1 if res["voiced"][i] else 0,
                    "agreed" if res["n_agree"][i] >= 2 else "ambiguous",
                    res["n_agree"][i],
                    fmt(res["ceps"][i]),
                    fmt(res["hps"][i]),
                    fmt(res["nccf"][i]),
                    fmt(res["spread"][i]),
                    res["rms_db"][i],
                )
            )


def characterise(res):
    """Reduce a track to the numbers the MANIFEST quotes.

    Everything here is measured, not assumed -- the owner has not said which
    takes are sung and which are spoken, so the description has to come out of
    the data.
    """
    f0 = res["f0"]
    voiced = res["voiced"]
    n = len(f0)
    vf = f0[voiced]

    stats = {
        "file": os.path.basename(res["path"]),
        "duration_s": round(res["info"].duration_s, 2),
        "sample_rate": res["info"].sample_rate,
        "audio_format": res["info"].audio_format,
        "bits": res["info"].bits,
        "frames": n,
        "voiced_frames": int(voiced.sum()),
        "voiced_fraction": round(float(voiced.mean()), 4),
        "energy_ok_frames": int(res["energy_ok"].sum()),
        "consensus_rate_in_energy": round(
            float(((res["n_agree"] >= 2) & res["energy_ok"]).sum())
            / max(1, int(res["energy_ok"].sum())),
            4,
        ),
        "ambiguous_frames_in_energy": int(
            ((res["n_agree"] < 2) & res["energy_ok"]).sum()
        ),
        "three_way_fraction_of_agreed": round(
            float((res["n_agree"] == 3).sum()) / max(1, int((res["n_agree"] >= 2).sum())), 4
        ),
        "noise_floor_db": round(res["noise_floor_db"], 1),
        "loud_p95_db": round(res["loud_db"], 1),
        "gate_db": round(res["gate_db"], 1),
    }

    if vf.size == 0:
        return stats

    stats.update(
        {
            "f0_median_hz": round(float(np.median(vf)), 2),
            "f0_p05_hz": round(float(np.percentile(vf, 5)), 2),
            "f0_p95_hz": round(float(np.percentile(vf, 95)), 2),
            "f0_min_hz": round(float(vf.min()), 2),
            "f0_max_hz": round(float(vf.max()), 2),
            "f0_range_semitones": round(
                float(
                    12.0
                    * np.log2(np.percentile(vf, 95) / np.percentile(vf, 5))
                ),
                2,
            ),
            "label_spread_median_cents": round(
                float(np.nanmedian(res["spread"][voiced])), 2
            ),
            "label_spread_p95_cents": round(
                float(np.nanpercentile(res["spread"][voiced], 95)), 2
            ),
        }
    )

    # --- contour shape: how fast does pitch move? ---
    # Measured over a 48 ms baseline (9 hops), NOT frame to frame.  At a 5.33 ms
    # hop a 3-cent label wobble is already 560 cents/second, so a per-frame
    # slope measures label noise, not the singer.  48 ms is short enough to
    # resolve a real slide and long enough that estimator jitter averages out.
    lag = 9
    dt = HOP / float(res["info"].sample_rate)
    base = lag * dt
    if n > lag:
        both = voiced[:-lag] & voiced[lag:]
        slope = np.abs(cents(f0[lag:], f0[:-lag])[both]) / base
    else:
        slope = np.zeros(0)
    if slope.size:
        stats["slope_median_cents_per_s"] = round(float(np.median(slope)), 1)
        stats["slope_p90_cents_per_s"] = round(float(np.percentile(slope, 90)), 1)
        # A "slide" (meend) is sustained motion: >= 500 cents/s held for
        # >= 150 ms.  The threshold is set from the measured label noise, not
        # picked for looks: the median label spread is ~9 cents, and 9 cents
        # across the 48 ms baseline is already 187 cents/s, so a 200 cents/s
        # threshold would mostly count jitter.  500 cents/s means 24 cents of
        # real movement across the baseline -- comfortably above the noise.
        # Vibrato does not qualify either: it reverses every ~100 ms, so it
        # cannot hold one direction for 150 ms.
        fast = slope >= 500.0
        run, events = 0, 0
        for v in fast:
            run = run + 1 if v else 0
            if run == 28:  # 150 ms of consecutive fast frames
                events += 1
        stats["sustained_slide_events"] = events
        stats["fast_motion_fraction"] = round(float(fast.mean()), 4)

    # --- steadiness: fraction of voiced frames sitting inside a note ---
    # A frame is "steady" if the 100 ms window centred on it stays within
    # +/-50 cents (a quarter tone).  High steadiness = sustained singing;
    # low = speech, where pitch never settles.
    half = 9
    steady = np.zeros(n, dtype=bool)
    for i in range(half, n - half):
        if not voiced[i]:
            continue
        seg = f0[i - half:i + half + 1]
        if np.isnan(seg).any():
            continue
        c = np.abs(cents(seg, f0[i]))
        steady[i] = c.max() <= 50.0
    stats["steady_fraction_of_voiced"] = round(
        float(steady.sum()) / max(1, int(voiced.sum())), 4
    )

    # --- residual label error: internal octave inconsistency ---
    # The consensus rule accepts a label on a 2-of-3 vote, so two estimators
    # that slip the same octave produce a confident wrong label.  There is no
    # external truth to check that against -- but a real vocal contour is
    # continuous, so a label sitting an octave away from its own immediate
    # neighbourhood is almost certainly an error.  Measuring that gives a
    # LOWER BOUND on the label track's error rate, and it is the number that
    # bounds how small a detector octave-error rate this set can resolve.
    vals = f0[voiced]
    k = 47  # +/- 0.125 s of voiced frames
    if vals.size > k:
        sw = np.lib.stride_tricks.sliding_window_view(vals, k)
        local_med = np.median(sw, axis=1)
        centre = vals[k // 2:k // 2 + local_med.size]
        off = np.abs(cents(centre, local_med))
        stats["label_octave_inconsistency"] = round(
            float((np.abs(off - 1200.0) < 100.0).mean()), 4
        )
    pair_adj = voiced[:-1] & voiced[1:]
    if pair_adj.any():
        jump = np.abs(cents(f0[1:], f0[:-1])[pair_adj])
        stats["adjacent_jump_gt_600c"] = round(float((jump > 600.0).mean()), 4)

    # --- longest continuously voiced run, in seconds (phrase-length proxy) ---
    run = best = 0
    for v in voiced:
        run = run + 1 if v else 0
        best = max(best, run)
    stats["longest_voiced_run_s"] = round(best * dt, 2)

    return stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("wavs", nargs="+")
    ap.add_argument("--outdir", default=None, help="where to write .f0.csv (default: alongside)")
    ap.add_argument("--stats-json", default=None)
    args = ap.parse_args()

    all_stats = []
    for path in args.wavs:
        res = analyse_file(path)
        base = os.path.basename(path)
        stem = base[:-4] if base.lower().endswith(".wav") else base
        outdir = args.outdir or os.path.dirname(path)
        csv_path = os.path.join(outdir, stem + ".f0.csv")
        write_csv(res, csv_path)
        st = characterise(res)
        st["csv"] = os.path.basename(csv_path)
        all_stats.append(st)
        print(json.dumps(st, indent=2))

    if args.stats_json:
        with open(args.stats_json, "w") as fh:
            json.dump(all_stats, fh, indent=2)


if __name__ == "__main__":
    main()
