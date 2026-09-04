// opentune-f0dump: run a PitchDetector over a WAV file and print its per-block
// estimate as CSV.  Host code (constitution IV): it does file I/O, allocates
// freely, and never runs on an audio thread.  It exists so that T1.0's Python
// scoring script can compare the engine's detectors against reference labels
// that were produced INDEPENDENTLY of the engine.
//
// The point of the separation: the labels in testdata/vocals/*.f0.csv come from
// three pitch estimators written from scratch in tools/label_f0.py.  Nothing in
// this file influences them.  This tool only reports what the engine says, so
// that the two tracks can be differenced.
//
// Usage:
//   opentune-f0dump <in.wav> <yin|autocorr> [blockSize]
//
// Output (stdout), one row per processed block:
//   block_end_s,f0_hz,confidence,voiced
// block_end_s is the timestamp of the LAST sample handed to the detector in
// that call.  It is deliberately raw: this tool applies no time correction of
// its own.
//
// A correction is nevertheless needed downstream, and it is worth stating why.
// The detector is handed 256 new samples per call, but analyses a window that
// spans roughly 2200 samples ending at that block's last sample (at 48 kHz:
// a 1478-sample comparison window plus a 739-sample maximum lag reach).  Its
// estimate therefore describes audio centred about 23 ms EARLIER than the
// block boundary.  Comparing it against a centre-timestamped label track
// without that shift manufactures error that is really just misalignment.
// The correction is applied in the scoring script, where the sensitivity of
// the result to the exact offset can be measured and reported instead of
// silently assumed.

// This is the one translation unit providing dr_wav's implementation for the
// opentune-f0dump binary (DR_WAV_IMPLEMENTATION must appear in exactly one .cpp
// per binary that links dr_wav).
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "opentune/AutocorrelationDetector.h"
#include "opentune/PitchDetector.h"
#include "opentune/YinDetector.h"
#include "tools/autotune-cli/WavFile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <in.wav> <yin|autocorr> [blockSize]\n", argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    const std::string which = argv[2];
    const int blockSize = (argc > 3) ? std::atoi(argv[3]) : 256;
    if (blockSize <= 0) {
        std::fprintf(stderr, "blockSize must be positive\n");
        return 2;
    }

    opentune::host::WavData wav;
    try {
        wav = opentune::host::readMono(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    std::unique_ptr<opentune::PitchDetector> detector;
    if (which == "yin") {
        detector = std::make_unique<opentune::YinDetector>();
    } else if (which == "autocorr") {
        detector = std::make_unique<opentune::AutocorrelationDetector>();
    } else {
        std::fprintf(stderr, "unknown detector '%s' (want yin or autocorr)\n", which.c_str());
        return 2;
    }

    detector->prepare(wav.sampleRate, blockSize);

    std::fprintf(stderr, "detector=%s sampleRate=%.0f blockSize=%d samples=%zu\n", which.c_str(),
                 wav.sampleRate, blockSize, wav.samples.size());

    std::printf("block_end_s,f0_hz,confidence,voiced\n");

    const std::size_t n = wav.samples.size();
    for (std::size_t start = 0; start + static_cast<std::size_t>(blockSize) <= n;
         start += static_cast<std::size_t>(blockSize)) {
        const opentune::PitchEstimate est =
            detector->process(wav.samples.data() + start, blockSize);

        const double blockEnd = static_cast<double>(start + static_cast<std::size_t>(blockSize));
        std::printf("%.6f,%.4f,%.4f,%d\n", blockEnd / wav.sampleRate,
                    static_cast<double>(est.frequencyHz), static_cast<double>(est.confidence),
                    est.voiced ? 1 : 0);
    }

    return 0;
}
