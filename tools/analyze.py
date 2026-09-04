#!/usr/bin/env python3
"""analyze.py -- OpenTune's audio "eye" (T0.12).

Claude cannot hear. CLAUDE.md forbids reading a .wav file directly (it is
binary and poisons the context window with garbage), so this script is the
*only* way an AI agent working on this project perceives what happened to a
piece of audio. It is not a generic plotting utility -- every choice below
is made for a reader who can only look at one PNG and read some printed
numbers, never press play.

Usage:
    .venv/bin/python tools/analyze.py in.wav [--out report.png] [--ref target.wav]

Produces:
  - One PNG (default: <in>_report.png) with four aligned panels sharing a
    time axis: pitch track (Hz, with a MIDI axis alongside and semitone
    gridlines), RMS envelope with the voiced/unvoiced decision shaded in,
    and a spectrogram.
  - Printed numbers: median/mean pitch, voiced fraction, RMS range, and,
    with --ref, a cents-error summary (median/mean/max) comparing the two
    files' pitch tracks.

Dependencies: numpy, scipy, matplotlib only (see requirements-dev.txt).
This file is host-side dev tooling -- constitution V -- and must never be
imported by, or become a runtime dependency of, anything under engine/.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

import matplotlib

# Headless: this script runs from a terminal / an agent's tool call, never a
# GUI session, and must never try to pop up a window or block on one.
matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile
from scipy import signal as sp_signal

# --------------------------------------------------------------------------
# Constants that encode audio-domain knowledge, not arbitrary tuning.
# --------------------------------------------------------------------------

# Vocal fundamental frequency range this project cares about (specs.md
# targets sung/spoken vocals). Anything outside this band is almost never a
# real voiced pitch -- it is octave error, noise, or a tracker artifact --
# so it is excluded before the pitch track is even computed. Roughly two
# octaves below middle C to two above: a bass low note to a soprano high
# note, with margin.
MIN_F0_HZ = 65.0   # ~C2
MAX_F0_HZ = 1000.0  # ~B5

# A frame is judged "voiced" (a real, trackable pitch is present) when its
# normalized autocorrelation peak at the chosen lag exceeds this. Below it,
# the frame is either silence, noise, or too aperiodic to trust -- reporting
# a confident-looking pitch number for such a frame would be exactly the
# "confident wrong pitch on a DC/silent signal" failure this tool exists to
# avoid (see the task brief). 0.3 is a permissive-but-not-credulous
# placeholder threshold: it is a stand-in for the "real" voiced/unvoiced
# decision that Stage 1's YIN aperiodicity measure (T1.7) will replace.
VOICING_THRESHOLD = 0.3

# Above this fraction of a frame's samples being an exact repeat of the
# sample immediately before them, the frame is judged "stuck" -- a
# sample-and-hold artifact, not a real waveform -- regardless of what the
# autocorrelation score says. Real vocal/instrumental audio essentially
# never holds a sample value for many consecutive samples (that would be a
# literal DC segment), so any high repeat-fraction is tooling, not singing.
# This exists specifically because of tasks.md D5: ResampleCorrector
# degenerates into holding one sample per 256-sample block once it starves
# or runs off its history. That produces a *piecewise-constant* "staircase"
# signal -- not silent, not globally DC (each block holds a different
# level) -- whose sharp step edges create spurious short-lag periodicity
# that fools a naive autocorrelation pitch tracker into reporting a
# confident, high, WRONG pitch (observed: pinned near MAX_F0_HZ). Catching
# it here, independent of the periodicity score, is what keeps that failure
# mode from being reported as a real note -- see the task brief's explicit
# warning about this exact case.
STUCK_SAMPLE_FRACTION = 0.5

# A stuck frame must also be audible. Without this, digital silence -- every
# sample identical to the last -- is flagged as a sample-and-hold artifact,
# which is a false alarm on the most ordinary content there is. See
# is_stuck_frame() for the reasoning, and tasks.md D7 for the bug this fixes.
STUCK_MIN_AMPLITUDE = 0.01

# Frame size / hop for the short-time analysis (pitch + RMS). 2048 samples
# at typical vocal sample rates (44.1-48 kHz) is ~43-46 ms -- long enough to
# contain several periods of even a low male voice (~80 Hz => 12.5 ms
# period) so autocorrelation has something to lock onto, short enough to
# track a note that changes over a few hundred ms. Hop of 512 gives 4x
# overlap, smooth enough to see a corrector's block-rate artifacts (see
# tasks.md D5 -- ResampleCorrector's ~5.33ms block-rate staircase) without
# needing an enormous number of frames.
FRAME_SIZE = 2048
HOP_SIZE = 512

A4_HZ = 440.0
A4_MIDI = 69

# --------------------------------------------------------------------------
# WAV `fmt ` chunk handling (tasks.md D9).
#
# scipy.io.wavfile.read does honour the fmt chunk internally -- it will
# decode a genuinely 32-bit-float file as float32, not as int16 -- so this
# tool was never *forced* to reimplement WAV parsing. But it must not
# *trust* that silently either. D9 happened for real during triage of the
# owner's recordings: a WAV reader that assumed 16-bit PCM regardless of
# what the file declared reinterpreted 32-bit float samples as pairs of
# int16s. That doesn't crash -- it manufactures plausible-looking broadband
# noise, flattens the RMS envelope, and halves the reported duration, and it
# produced a confident, entirely wrong verdict about real audio before being
# caught. The tell was six independent recordings all reporting RMS
# 0.5403-0.5414 -- identical to four decimal places, which is impossible for
# real audio and is exactly what reinterpreting near-white bit patterns as
# samples looks like.
#
# So load_mono() below re-parses the fmt chunk itself, independent of
# scipy, and ASSERTS that (a) the format/bit-depth combination is one this
# tool actually knows how to interpret and (b) the dtype scipy handed back
# actually matches what the fmt chunk declares. Anything else fails loudly
# with a clear message instead of silently guessing -- a bug this cheap to
# detect (see test_analyze.py's regression test) should never recur
# silently.
WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE

# The last 14 bytes of every KSDATAFORMAT_SUBTYPE_* GUID used inside a
# WAVEFORMATEXTENSIBLE fmt chunk are this fixed suffix; the first two bytes
# vary and equal the plain-format code (1 = PCM, 3 = IEEE float) the
# extensible header is standing in for.
_SUBTYPE_GUID_SUFFIX = bytes.fromhex("0000000010008000" "00aa00389b71")

# (audioFormat, bitsPerSample) combinations this tool knows how to decode,
# and the numpy dtype scipy.io.wavfile.read must have produced for each one.
# Anything not in this table (24-bit PCM, 8-bit unsigned PCM, mu-law/A-law,
# 64-bit float, ...) is refused rather than guessed at -- extend this table
# deliberately if a new case shows up, don't remove the check.
_SUPPORTED_WAV_FORMATS: dict[tuple[int, int], np.dtype] = {
    (WAVE_FORMAT_PCM, 16): np.dtype(np.int16),
    (WAVE_FORMAT_PCM, 32): np.dtype(np.int32),
    (WAVE_FORMAT_IEEE_FLOAT, 32): np.dtype(np.float32),
}


@dataclass
class WavFmt:
    audio_format: int
    channels: int
    sample_rate: int
    bits_per_sample: int


def read_fmt_chunk(path: str) -> WavFmt:
    """Parse the WAV `fmt ` chunk directly, independent of scipy.

    This walks the RIFF chunk list by hand (id + little-endian u32 size,
    chunks word-aligned) looking for `fmt `, rather than assuming it is
    always the first chunk after the 12-byte RIFF/WAVE header -- some
    writers put a JUNK/LIST chunk first. Raises ValueError if the file is
    not RIFF/WAVE or has no fmt chunk at all; that is a malformed-file
    error, not a format-tag mismatch, so it is not folded into the D9
    assertion below.
    """
    with open(path, "rb") as f:
        header = f.read(12)
        if len(header) < 12 or header[0:4] != b"RIFF" or header[8:12] != b"WAVE":
            raise ValueError(f"'{path}': not a RIFF/WAVE file")

        while True:
            chunk_header = f.read(8)
            if len(chunk_header) < 8:
                raise ValueError(f"'{path}': no fmt chunk found")
            chunk_id = chunk_header[0:4]
            chunk_size = struct.unpack("<I", chunk_header[4:8])[0]

            if chunk_id == b"fmt ":
                body = f.read(chunk_size)
                if len(body) < 16:
                    raise ValueError(f"'{path}': fmt chunk is truncated")
                audio_format, channels, sample_rate, _byte_rate, _block_align, \
                    bits_per_sample = struct.unpack("<HHIIHH", body[:16])

                # WAVE_FORMAT_EXTENSIBLE (used by files with >2 channels or
                # >16-bit depths written by some tools) hides the real
                # format inside a sub-format GUID rather than the plain
                # audioFormat field -- unwrap it so the table lookup above
                # still works.
                if audio_format == WAVE_FORMAT_EXTENSIBLE and len(body) >= 40:
                    subtype_code = struct.unpack("<H", body[24:26])[0]
                    if body[26:40] == _SUBTYPE_GUID_SUFFIX:
                        audio_format = subtype_code

                return WavFmt(audio_format=audio_format, channels=channels,
                              sample_rate=sample_rate, bits_per_sample=bits_per_sample)

            # Chunks are word-aligned: an odd-sized chunk has one pad byte
            # after it that is not part of chunk_size.
            f.seek(chunk_size + (chunk_size & 1), 1)


def hz_to_midi(hz: np.ndarray) -> np.ndarray:
    """Convert Hz to (fractional) MIDI note number, NaN-safe.

    MIDI note number is a log-frequency scale: each integer step is one
    semitone, a constant frequency *ratio* (2**(1/12)) rather than a
    constant Hz difference. That is the scale a musician (and a pitch
    corrector snapping to a scale) actually thinks in, which is why the
    pitch-track panel below plots gridlines evenly spaced in MIDI/semitones,
    not evenly spaced in Hz.
    """
    with np.errstate(divide="ignore", invalid="ignore"):
        midi = A4_MIDI + 12.0 * np.log2(hz / A4_HZ)
    return midi


def midi_to_hz(midi: np.ndarray) -> np.ndarray:
    return A4_HZ * (2.0 ** ((midi - A4_MIDI) / 12.0))


@dataclass
class PitchTrack:
    times: np.ndarray       # frame center times, seconds
    f0_hz: np.ndarray       # NaN where unvoiced/invalid
    voiced: np.ndarray      # bool, one per frame
    rms: np.ndarray         # linear RMS, one per frame
    clarity: np.ndarray     # normalized autocorrelation peak, one per frame
    stuck: np.ndarray       # bool, one per frame -- sample-and-hold artifact (see D5)


def load_mono(path: str) -> tuple[np.ndarray, int]:
    """Load a WAV file as a mono float64 signal in [-1, 1].

    scipy.io.wavfile reads the file directly -- no dr_wav dependency needed
    on the Python side, and no new package (numpy/scipy/matplotlib are the
    only allowed deps, per T0.0 and this task's brief). Multi-channel files
    are averaged down to mono, matching what opentune-cli itself does to
    its input (tools/autotune-cli/WavFile.h) so a side-by-side comparison
    is apples to apples.
    """
    fmt = read_fmt_chunk(path)
    expected_dtype = _SUPPORTED_WAV_FORMATS.get((fmt.audio_format, fmt.bits_per_sample))
    if expected_dtype is None:
        raise AssertionError(
            f"'{path}': unhandled WAV format -- audioFormat={fmt.audio_format}, "
            f"bitsPerSample={fmt.bits_per_sample}. Refusing to guess how to decode "
            f"this (tasks.md D9); add it to _SUPPORTED_WAV_FORMATS deliberately "
            f"if it needs to be supported."
        )

    sample_rate, data = wavfile.read(path)

    # Defense in depth: scipy is trusted to decode the samples correctly,
    # but not trusted *silently*. If what it handed back doesn't match what
    # the fmt chunk we parsed ourselves declares, something is inconsistent
    # (a scipy behaviour change, a malformed/edited file, a mismatched
    # channel count) and guessing which one is right is exactly the D9
    # failure mode. Fail loudly instead.
    if sample_rate != fmt.sample_rate:
        raise AssertionError(
            f"'{path}': fmt chunk declares sampleRate={fmt.sample_rate} Hz but "
            f"scipy decoded {sample_rate} Hz -- refusing to guess which is right."
        )
    if data.dtype != expected_dtype:
        raise AssertionError(
            f"'{path}': fmt chunk declares audioFormat={fmt.audio_format}, "
            f"bitsPerSample={fmt.bits_per_sample} (expects {expected_dtype} samples) "
            f"but scipy decoded dtype={data.dtype} -- refusing to analyze a file "
            f"whose declared format and decoded samples disagree (tasks.md D9)."
        )

    if data.ndim > 1:
        data = data.mean(axis=1)

    # Normalize integer PCM formats to [-1, 1]; dr_wav/opentune-cli always
    # write float32, but a hand-supplied reference file might not be.
    if np.issubdtype(data.dtype, np.integer):
        max_val = float(np.iinfo(data.dtype).max)
        samples = data.astype(np.float64) / max_val
    else:
        samples = data.astype(np.float64)

    return samples, int(sample_rate)


def is_stuck_frame(frame: np.ndarray) -> bool:
    """True when `frame` looks like a sample-and-hold artifact rather than
    real audio -- see STUCK_SAMPLE_FRACTION above for why this exists.

    Two conditions, both required. The repeat fraction alone is not enough:
    digital silence is a sequence of identical zeros, so it satisfies any
    repeat test trivially. Silence and a sample-and-hold collapse are both
    "constant", but they are completely different problems -- silence is
    ordinary and expected (gaps between phrases, a file's lead-in), while a
    stuck frame is a corrector failure that the listener hears as a loud
    buzz. Reporting the first as the second sends the reader after a bug
    that is not there, which for a tool an agent uses *instead of hearing*
    is worse than reporting nothing at all.

    So a stuck frame must also be LOUD. The threshold is deliberately well
    above a noise floor and well below a real signal: the D5 collapse holds
    a sample from real audio, so its level sits near the signal's own
    amplitude (measured peak ~0.5 on a 0.7-peak input), nowhere near this.
    """
    if len(frame) < 2:
        return False
    repeat_fraction = float(np.mean(frame[1:] == frame[:-1]))
    if repeat_fraction < STUCK_SAMPLE_FRACTION:
        return False
    # Median, not peak: a frame straddling the boundary between silence and a
    # loud onset is mostly repeated zeros *and* contains a loud sample, so a
    # peak test still flags it. The median asks the right question -- is the
    # repeated value itself loud? -- which is true of a held staircase and
    # false of silence with an edge in it.
    return float(np.median(np.abs(frame))) >= STUCK_MIN_AMPLITUDE


def autocorrelation_pitch_frame(frame: np.ndarray, sample_rate: int) -> tuple[float, float]:
    """Estimate f0 and a voicing "clarity" score for one frame.

    This is a simple normalized-autocorrelation pitch estimator -- the same
    family of algorithm as engine/src/AutocorrelationDetector.cpp, but
    reimplemented independently in Python. That independence is
    deliberate: this tool exists to *check* the engine's own output, so it
    must not share a bug with the code it is checking.

    Method: correlate the frame with itself at every lag corresponding to
    MIN_F0_HZ..MAX_F0_HZ, normalize by zero-lag energy (so the score is
    comparable across frames of different loudness), and take the
    strongest peak. A flat/DC/silent frame has ~zero AC-normalized energy
    or a degenerate normalization -- both handled explicitly below so they
    read as "no pitch", not as a wrong pitch dressed up as a real one.
    """
    frame = frame - np.mean(frame)  # remove DC before correlating
    energy = np.dot(frame, frame)

    # A silent or constant (DC-removed-to-zero) frame has no periodicity to
    # find at all. Report it as unvoiced rather than let a near-zero
    # denominator produce a huge, meaningless "clarity" and a spurious f0 --
    # this is exactly the "confident wrong pitch on a DC signal" case the
    # task brief calls out by name.
    if energy < 1e-12:
        return float("nan"), 0.0

    min_lag = int(sample_rate / MAX_F0_HZ)
    max_lag = min(int(sample_rate / MIN_F0_HZ), len(frame) - 1)
    if max_lag <= min_lag:
        return float("nan"), 0.0

    # Full autocorrelation via FFT (much faster than a direct lag loop for
    # FRAME_SIZE=2048), then keep only the lag range we care about.
    n = 1
    while n < 2 * len(frame):
        n *= 2
    spectrum = np.fft.rfft(frame, n=n)
    autocorr_full = np.fft.irfft(spectrum * np.conj(spectrum), n=n)
    autocorr = autocorr_full[: max_lag + 1]

    lags = np.arange(min_lag, max_lag + 1)
    scores = autocorr[lags] / energy  # normalize: 1.0 = perfect periodicity

    best_index = int(np.argmax(scores))
    best_lag = lags[best_index]
    best_score = float(scores[best_index])

    if best_score <= 0.0:
        return float("nan"), 0.0

    f0 = sample_rate / float(best_lag)
    return f0, best_score


def analyze(samples: np.ndarray, sample_rate: int) -> PitchTrack:
    """Run the frame-by-frame pitch/RMS/voicing analysis over a whole file."""
    n_frames = max(0, 1 + (len(samples) - FRAME_SIZE) // HOP_SIZE)

    times = np.zeros(n_frames)
    f0_hz = np.full(n_frames, np.nan)
    voiced = np.zeros(n_frames, dtype=bool)
    rms = np.zeros(n_frames)
    clarity = np.zeros(n_frames)
    stuck = np.zeros(n_frames, dtype=bool)

    for i in range(n_frames):
        start = i * HOP_SIZE
        frame = samples[start : start + FRAME_SIZE]

        times[i] = (start + FRAME_SIZE / 2.0) / sample_rate
        rms[i] = float(np.sqrt(np.mean(frame.astype(np.float64) ** 2)))

        frame_is_stuck = is_stuck_frame(frame)
        stuck[i] = frame_is_stuck

        f0, score = autocorrelation_pitch_frame(frame, sample_rate)
        clarity[i] = score
        # A stuck (sample-and-hold) frame is never voiced, no matter how
        # confident the autocorrelation score looks -- see D5 and
        # STUCK_SAMPLE_FRACTION above. This check runs *after* the
        # autocorrelation call (so `clarity` still records what the naive
        # detector would have believed, for anyone debugging this tool
        # itself) but wins the final voiced/unvoiced decision.
        is_voiced = (not frame_is_stuck) and score >= VOICING_THRESHOLD and not np.isnan(f0)
        voiced[i] = is_voiced
        f0_hz[i] = f0 if is_voiced else float("nan")

    return PitchTrack(times=times, f0_hz=f0_hz, voiced=voiced, rms=rms, clarity=clarity,
                       stuck=stuck)


def print_summary(label: str, track: PitchTrack) -> None:
    """Print the numbers a reader needs without looking at the picture at
    all (constitution VII: quality is measured, not asserted)."""
    voiced_hz = track.f0_hz[track.voiced]
    voiced_fraction = float(np.mean(track.voiced)) if len(track.voiced) else 0.0

    print(f"--- {label} ---")
    if len(voiced_hz) > 0:
        print(f"  median pitch: {np.median(voiced_hz):.2f} Hz "
              f"(MIDI {hz_to_midi(np.median(voiced_hz)):.2f})")
        print(f"  mean pitch:   {np.mean(voiced_hz):.2f} Hz")
    else:
        print("  median pitch: n/a (no voiced frames)")
        print("  mean pitch:   n/a (no voiced frames)")
    print(f"  voiced fraction: {voiced_fraction:.1%} "
          f"({int(np.sum(track.voiced))}/{len(track.voiced)} frames)")
    stuck_fraction = float(np.mean(track.stuck)) if len(track.stuck) else 0.0
    if stuck_fraction > 0.0:
        # Flag this loudly rather than folding it silently into "unvoiced":
        # a stuck signal (D5's block-rate staircase) is a corrector bug
        # signature, not ordinary silence between phrases.
        print(f"  WARNING: {stuck_fraction:.1%} of frames are sample-and-hold "
              f"artifacts ({int(np.sum(track.stuck))}/{len(track.stuck)}) -- "
              f"see tasks.md D5")
    if len(track.rms):
        print(f"  RMS range: {np.min(track.rms):.4f} - {np.max(track.rms):.4f}")
    else:
        print("  RMS range: n/a (empty file)")


def cents_error_summary(track_a: PitchTrack, track_b: PitchTrack,
                         rate_a: int, rate_b: int) -> None:
    """Compare two pitch tracks (e.g. corrected output vs. a target) in
    cents -- the perceptually-linear unit for pitch error (100 cents = one
    semitone), matching how the rest of this project measures pitch error
    (see engine/CLAUDE.md, tests/test_engine.cpp's centsError helper).

    Frames are compared only where BOTH tracks call the frame voiced --
    comparing a real pitch against a NaN from an unvoiced frame is
    meaningless and would either crash or silently corrupt the summary.

    Comparison is by frame INDEX, not by the `times` value -- track_a[i]
    is compared against track_b[i]. times[i] = (i*HOP_SIZE + FRAME_SIZE/2)
    / sample_rate, so index i only lines up to the same instant in both
    files when both were analyzed at the same sample rate. If the rates
    differ this would silently compare frames from two different points in
    time and report a cents number that looks precise but means nothing --
    refuse instead.
    """
    if rate_a != rate_b:
        print("--- Cents error (vs. reference) ---")
        print(f"  input is {rate_a} Hz, reference is {rate_b} Hz -- frame index i means "
              f"a different point in time in each track at different sample rates, so "
              f"comparing by index would silently misalign them. Not computing a cents "
              f"error; resample one file to match the other first.")
        return

    n = min(len(track_a.times), len(track_b.times))
    both_voiced = track_a.voiced[:n] & track_b.voiced[:n]

    if not np.any(both_voiced):
        print("--- Cents error (vs. reference) ---")
        print("  no frames are voiced in both files -- cannot compare")
        return

    a = track_a.f0_hz[:n][both_voiced]
    b = track_b.f0_hz[:n][both_voiced]
    cents = 1200.0 * np.log2(a / b)

    print("--- Cents error (input vs. reference) ---")
    print(f"  compared frames: {int(np.sum(both_voiced))}/{n}")
    print(f"  median: {np.median(cents):+.1f} cents")
    print(f"  mean:   {np.mean(cents):+.1f} cents")
    print(f"  max:    {np.max(np.abs(cents)):.1f} cents (largest absolute error)")


def make_plot(path: str, samples: np.ndarray, sample_rate: int, track: PitchTrack,
              out_path: str) -> None:
    """Render the single self-explanatory PNG.

    Four panels, sharing a time axis (top to bottom):
      1. Pitch track: Hz on the left axis, MIDI note on the right axis,
         with horizontal gridlines at every semitone. This is the panel
         that answers the task's actual done-criterion -- does corrected
         pitch visibly SNAP to those lines, or wander/glitch between them?
      2. RMS envelope with voiced/unvoiced regions shaded, so silence and
         unpitched noise are visually distinct from a sung note, never
         plotted as if they were one.
      3. Spectrogram: shows energy across all frequencies over time, not
         just the tracked fundamental -- this is how a listener-substitute
         "sees" broadband artifacts (clicks, buzz, aliasing) that a pitch
         tracker alone would miss entirely.
    """
    duration = len(samples) / sample_rate
    time_axis_samples = np.arange(len(samples)) / sample_rate

    fig, (ax_pitch, ax_rms, ax_spec) = plt.subplots(
        3, 1, figsize=(14, 10), sharex=True,
        gridspec_kw={"height_ratios": [3, 1.4, 2.2]},
    )
    fig.suptitle(f"OpenTune audio analysis: {path}", fontsize=13, fontweight="bold")

    # --- Panel 1: pitch track -------------------------------------------
    voiced_hz = np.where(track.voiced, track.f0_hz, np.nan)

    # Semitone gridlines: pick the MIDI range actually present (padded by a
    # couple of semitones) so the lines are dense enough to be useful but
    # not so dense across the full vocal range that they turn into a solid
    # grey block. Falls back to a fixed one-octave window around A4 when
    # there is no voiced pitch at all (silence/DC), so the panel is still
    # legible rather than empty.
    if np.any(track.voiced):
        midi_present = hz_to_midi(voiced_hz[track.voiced])
        midi_low = int(np.floor(np.min(midi_present))) - 2
        midi_high = int(np.ceil(np.max(midi_present))) + 2
    else:
        midi_low, midi_high = A4_MIDI - 6, A4_MIDI + 6

    for midi_note in range(midi_low, midi_high + 1):
        ax_pitch.axhline(midi_to_hz(midi_note), color="0.85", linewidth=0.6, zorder=0)

    ax_pitch.plot(track.times, voiced_hz, color="#1f6fb4", linewidth=1.6,
                  label="voiced pitch", zorder=3)
    # Unvoiced/invalid frames are never drawn as a pitch line (that would
    # imply a note is present when it is not) -- instead mark their time
    # extent along the bottom of the panel so their presence is still
    # visible.
    # Ordinary unvoiced (silence/noise/aperiodic) frames get a grey tick at
    # the bottom of the panel. Stuck (sample-and-hold, D5) frames are a
    # DIFFERENT failure -- not "no pitch present" but "the corrector broke"
    # -- so they get their own red marker rather than being lumped in with
    # plain silence. Neither is ever drawn on the pitch line itself: that
    # line must only ever show a value this tool actually trusts.
    plain_unvoiced_times = track.times[(~track.voiced) & (~track.stuck)]
    if len(plain_unvoiced_times):
        ax_pitch.scatter(plain_unvoiced_times,
                          np.full_like(plain_unvoiced_times, midi_to_hz(midi_low)),
                          marker="|", color="0.5", s=40, label="unvoiced/invalid", zorder=2)
    stuck_times = track.times[track.stuck]
    if len(stuck_times):
        ax_pitch.scatter(stuck_times, np.full_like(stuck_times, midi_to_hz(midi_low)),
                          marker="x", color="#c0392b", s=40,
                          label="stuck/sample-and-hold (D5)", zorder=4)

    ax_pitch.set_ylim(midi_to_hz(midi_low), midi_to_hz(midi_high))
    ax_pitch.set_ylabel("Pitch (Hz)")
    ax_pitch.set_title("Pitch track (semitone gridlines -- corrected pitch should snap to them)")
    ax_pitch.legend(loc="upper right", fontsize=8)

    # Secondary right-hand axis in MIDI note numbers, sharing the same
    # (log-spaced-by-semitone) y positions as the Hz axis on the left.
    ax_pitch_midi = ax_pitch.secondary_yaxis(
        "right", functions=(hz_to_midi, midi_to_hz))
    ax_pitch_midi.set_ylabel("MIDI note")

    # --- Panel 2: RMS envelope + voiced/unvoiced shading -----------------
    ax_rms.plot(track.times, track.rms, color="#444444", linewidth=1.0)
    ax_rms.fill_between(track.times, 0, track.rms, color="#444444", alpha=0.15)
    # Shade voiced regions in translucent blue so it's visually obvious
    # which parts of the loudness envelope correspond to a tracked pitch.
    ax_rms.fill_between(track.times, 0, np.max(track.rms) if len(track.rms) else 1.0,
                         where=track.voiced, color="#1f6fb4", alpha=0.12, step="mid",
                         label="voiced region")
    ax_rms.set_ylabel("RMS amplitude")
    ax_rms.set_title("RMS envelope (blue shading = voiced)")
    ax_rms.set_ylim(bottom=0)
    ax_rms.legend(loc="upper right", fontsize=8)

    # --- Panel 3: spectrogram ---------------------------------------------
    # A spectrogram shows how energy is distributed across frequency *and*
    # time simultaneously -- unlike the pitch track above (which only shows
    # the single strongest periodicity per frame), it reveals broadband
    # artifacts a pitch tracker can miss entirely: clicks (a vertical
    # streak across all frequencies), buzz/aliasing from a corrector
    # collapsing to a block-rate staircase (extra harmonic combs), or noise
    # floor changes.
    #
    # D8 (OOM on real files): the raw sxx array itself was never the
    # problem -- even take04.wav's full ~96 s only produces a ~37 MB float64
    # grid. The actual cause was rendering it with
    # `pcolormesh(..., shading="gouraud")`: Agg builds gouraud-shaded quads
    # per-vertex, and for an ~9000-column mesh that pushed this process past
    # 4 GB RSS and into the OOM killer (measured; see docs/decisions/). Two
    # independent fixes, both kept: (1) `imshow` on a regular time/frequency
    # grid -- which a spectrogram always is -- rasterizes directly from the
    # array with no per-vertex blow-up, and (2) the column count is capped
    # so memory and render time stay bounded no matter how long the input
    # is, rather than growing without limit on some future longer take.
    # imshow also does not interpolate colors between STFT bins the way
    # gouraud shading did -- gouraud was quietly implying a smoothness
    # between adjacent time/frequency bins that was never actually measured,
    # which is exactly the kind of misleading-picture failure this tool
    # exists to avoid (CLAUDE.md: this PNG is the only way an agent
    # perceives the audio).
    MAX_SPEC_COLUMNS = 4000
    nperseg = min(1024, len(samples)) if len(samples) > 0 else 1
    if len(samples) >= 2:
        default_hop = max(1, nperseg // 2)
        hop = max(default_hop, len(samples) // MAX_SPEC_COLUMNS)
        noverlap = max(0, min(nperseg - 1, nperseg - hop))
        freqs, spec_times, sxx = sp_signal.spectrogram(
            samples, fs=sample_rate, nperseg=nperseg, noverlap=noverlap)
        # dB scale (log power) is what makes quiet harmonics visible
        # alongside a loud fundamental -- linear power would make everything
        # but the loudest bin look black. Floor at 1e-12 avoids log(0).
        sxx_db = 10.0 * np.log10(np.maximum(sxx, 1e-12))
        im = ax_spec.imshow(
            sxx_db, origin="lower", aspect="auto", cmap="magma",
            extent=(spec_times[0], spec_times[-1], freqs[0], freqs[-1]),
        )
        ax_spec.set_ylim(0, min(MAX_F0_HZ * 4, sample_rate / 2))
        fig.colorbar(im, ax=ax_spec, label="Power (dB)", pad=0.01)
    else:
        ax_spec.text(0.5, 0.5, "signal too short for a spectrogram",
                     ha="center", va="center", transform=ax_spec.transAxes)
    ax_spec.set_ylabel("Frequency (Hz)")
    ax_spec.set_xlabel("Time (s)")
    ax_spec.set_title("Spectrogram")

    ax_spec.set_xlim(0, duration)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Render a pitch/RMS/spectrogram report for a WAV file -- "
                     "the only way an AI agent on this project perceives audio.")
    parser.add_argument("input", help="Input WAV file to analyze.")
    parser.add_argument("--out", default=None,
                         help="Output PNG path (default: <input>_report.png).")
    parser.add_argument("--ref", default=None,
                         help="Optional reference/target WAV file for a cents-error "
                              "comparison against `input`.")
    args = parser.parse_args()

    out_path = args.out or (_strip_ext(args.input) + "_report.png")

    samples, sample_rate = load_mono(args.input)
    if len(samples) == 0:
        print(f"opentune-analyze: '{args.input}' is empty -- nothing to analyze.",
              file=sys.stderr)
        return 1

    track = analyze(samples, sample_rate)
    print_summary(args.input, track)

    if args.ref:
        ref_samples, ref_rate = load_mono(args.ref)
        ref_track = analyze(ref_samples, ref_rate)
        print_summary(args.ref, ref_track)
        cents_error_summary(track, ref_track, sample_rate, ref_rate)

    make_plot(args.input, samples, sample_rate, track, out_path)
    print(f"report written to {out_path}")
    return 0


def _strip_ext(path: str) -> str:
    if path.lower().endswith(".wav"):
        return path[: -len(".wav")]
    return path


if __name__ == "__main__":
    raise SystemExit(main())
