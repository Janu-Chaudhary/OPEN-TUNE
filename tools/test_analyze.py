#!/usr/bin/env python3
"""Regression tests for tools/analyze.py's WAV loading (tasks.md D9).

No pytest here on purpose: requirements-dev.txt pins exactly the three
packages analyze.py itself needs (numpy, scipy, matplotlib) and adding a
test-only dependency is a call for the project owner, not this script (see
CLAUDE.md: "Never add a third-party dependency without asking first"). So
this is a plain script of `test_*` functions, run directly and asserting
loudly -- consistent with how load_mono() itself now fails.

Run with:
    .venv/bin/python tools/test_analyze.py
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402


def _sine(freq_hz: float, amplitude: float, duration_s: float, sample_rate: int) -> np.ndarray:
    n = int(round(duration_s * sample_rate))
    t = np.arange(n) / sample_rate
    return (amplitude * np.sin(2.0 * np.pi * freq_hz * t)).astype(np.float32)


def test_float32_roundtrip_duration_and_rms() -> None:
    """The D9 regression test: synthesize a known signal, write it as
    32-bit float (audioFormat=3, matching testdata/vocals/*.wav and dr_wav's
    own output), read it back through load_mono(), and check the numbers
    that D9 got wrong when a reader assumed 16-bit PCM instead:

      - duration: a 16-bit-assumption reader sees every float32 sample as
        two int16 samples, so it reports HALF the true duration.
      - RMS: reinterpreting float32 bit patterns as int16 audio produces
        near-white noise, flattening whatever RMS envelope the real signal
        had -- for a pure sine, RMS should be amplitude/sqrt(2), not some
        noise-floor-driven number.

    A sine wave's RMS has a closed form (amplitude / sqrt(2)) independent of
    frequency or phase, which is what makes this a strong, easy check.
    """
    sample_rate = 48000
    amplitude = 0.5
    duration_s = 2.0
    expected_rms = amplitude / np.sqrt(2.0)

    samples = _sine(freq_hz=220.0, amplitude=amplitude, duration_s=duration_s,
                     sample_rate=sample_rate)

    with tempfile.TemporaryDirectory() as tmp_dir:
        path = os.path.join(tmp_dir, "sine_f32.wav")
        wavfile.write(path, sample_rate, samples)  # float32 array -> audioFormat=3

        # Sanity-check our own fixture: confirm the file we just wrote
        # really is declared as 32-bit IEEE float, not something else.
        fmt = analyze.read_fmt_chunk(path)
        assert fmt.audio_format == analyze.WAVE_FORMAT_IEEE_FLOAT, (
            f"fixture bug: wrote audioFormat={fmt.audio_format}, expected "
            f"{analyze.WAVE_FORMAT_IEEE_FLOAT} (IEEE float)"
        )
        assert fmt.bits_per_sample == 32, (
            f"fixture bug: wrote bitsPerSample={fmt.bits_per_sample}, expected 32"
        )

        loaded, loaded_rate = analyze.load_mono(path)

    assert loaded_rate == sample_rate, f"sample rate {loaded_rate} != {sample_rate}"

    got_duration = len(loaded) / loaded_rate
    # A 16-bit-assumption bug halves this (2 int16s decoded per float32
    # sample halves the apparent sample count relative to the byte count,
    # and byte-for-byte the array length would come out ~half of true).
    assert abs(got_duration - duration_s) < 1.0 / sample_rate, (
        f"duration {got_duration:.6f}s != expected {duration_s:.6f}s -- "
        f"this is the D9 symptom (halved duration from a 16-bit-PCM "
        f"misread of 32-bit float data)"
    )

    got_rms = float(np.sqrt(np.mean(loaded.astype(np.float64) ** 2)))
    # Loose-ish tolerance: this checks against the exact bug (RMS collapsing
    # to a noise-driven ~0.54, per the D9 tell), not against floating-point
    # write/read precision.
    assert abs(got_rms - expected_rms) < 1e-3, (
        f"RMS {got_rms:.6f} != expected {expected_rms:.6f} -- this is the "
        f"D9 symptom (broadband noise manufactured from float32 samples "
        f"misread as int16)"
    )


def test_mismatched_fmt_chunk_raises() -> None:
    """load_mono() must ASSERT LOUDLY (tasks.md D9), not guess, when the fmt
    chunk it parses and the samples scipy decodes disagree.

    Simulates that disagreement directly: write an ordinary 16-bit PCM
    file, then hand-edit its fmt chunk to claim bitsPerSample=32 while
    leaving the actual sample data untouched. This is the same *shape* of
    failure as D9 (a reader trusting the wrong format description), just
    forced deterministically instead of depending on scipy behaving one way
    or another.
    """
    sample_rate = 48000
    samples_f32 = _sine(freq_hz=440.0, amplitude=0.4, duration_s=0.5, sample_rate=sample_rate)
    samples_i16 = (samples_f32 * 32767.0).astype(np.int16)

    with tempfile.TemporaryDirectory() as tmp_dir:
        path = os.path.join(tmp_dir, "corrupted_fmt.wav")
        wavfile.write(path, sample_rate, samples_i16)  # genuinely 16-bit PCM

        with open(path, "r+b") as f:
            raw = f.read()
        fmt_pos = raw.find(b"fmt ")
        assert fmt_pos != -1, "fixture bug: no fmt chunk in freshly-written WAV"
        bits_field_offset = fmt_pos + 8 + 14  # fmt body's bitsPerSample field
        raw = (raw[:bits_field_offset] + struct.pack("<H", 32)
               + raw[bits_field_offset + 2:])
        with open(path, "wb") as f:
            f.write(raw)

        fmt = analyze.read_fmt_chunk(path)
        assert fmt.bits_per_sample == 32, "fixture bug: fmt-chunk edit did not take"

        raised = False
        try:
            analyze.load_mono(path)
        except AssertionError:
            raised = True
        assert raised, (
            "load_mono() must raise AssertionError when the fmt chunk and "
            "the decoded sample dtype disagree, not silently proceed"
        )


def main() -> int:
    tests = [test_float32_roundtrip_duration_and_rms, test_mismatched_fmt_chunk_raises]
    failures = 0
    for test in tests:
        try:
            test()
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    print(f"{len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
