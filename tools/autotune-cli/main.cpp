// opentune-cli: the Stage 0 offline pitch-correction test harness (T0.11).
//
//   opentune-cli in.wav out.wav [--key C:major] [--strength 0.8]
//
// This is HOST code (constitution IV): it owns file I/O, argument parsing,
// and error reporting to a human at a terminal -- none of which `engine/` is
// allowed to do. It is also this project's first real *host*: the shape of
// the loop below (construct the three collaborators, inject them into an
// Engine, then feed it fixed-size blocks) is deliberately the same call
// pattern a real-time host (Stage 3's `opentune-live`) will use. Going
// real-time later is meant to change only the caller -- swap this
// file-reading loop for a callback fed by an audio device -- and not the
// engine at all.
//
// This is also the one translation unit that provides dr_wav's
// implementation for the opentune-cli binary (DR_WAV_IMPLEMENTATION must
// appear in exactly one .cpp per binary that links dr_wav; tests/test_wav_file.cpp
// does the same for the separate opentune_tests binary -- two different
// executables, so there is no one-definition-rule conflict between them).
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "tools/autotune-cli/CliArgs.h"
#include "tools/autotune-cli/WavFile.h"

#include "opentune/AutocorrelationDetector.h"
#include "opentune/Engine.h"
#include "opentune/Params.h"
#include "opentune/ResampleCorrector.h"
#include "opentune/ScaleQuantizer.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using opentune::Engine;
    using opentune::Params;
    using opentune::host::CliOptions;

    // --- Argument parsing -------------------------------------------------
    // Malformed --key, out-of-range --strength, wrong argument count, etc.
    // all surface here as std::invalid_argument (see CliArgs.h) -- caught,
    // reported with the offending value named, and a non-zero exit. Never a
    // crash, never a silently-substituted default.
    CliOptions options;
    try {
        options = opentune::host::parseArgs(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << "opentune-cli: " << error.what() << "\n\n" << opentune::host::kUsageText;
        return EXIT_FAILURE;
    }

    if (options.showHelp) {
        std::cout << opentune::host::kUsageText;
        return EXIT_SUCCESS;
    }

    // --- Read input ---------------------------------------------------------
    // readMono() throws std::runtime_error, naming the path, for a missing
    // file or a file dr_wav cannot decode (WavFile.h). Caught here rather
    // than left to crash the process or print a raw exception message.
    opentune::host::WavData input;
    try {
        input = opentune::host::readMono(options.inputPath);
    } catch (const std::exception& error) {
        std::cerr << "opentune-cli: " << error.what() << "\n";
        return EXIT_FAILURE;
    }

    // --- Build the engine's parameters --------------------------------------
    Params params;
    // Parsed and stored, but NOT applied yet: Stage 0 corrects every voiced
    // frame at full strength regardless of this value. See CliArgs.h's
    // kUsageText and tasks.md T4.2, which is where this becomes real.
    params.strength = options.strength;
    // Parsed from --key, but Stage 0's ScaleQuantizer treats every
    // ScaleType identically to Chromatic regardless of key -- see
    // ScaleQuantizer.h. Real scale tables are Stage 5 (T5.1).
    params.scale = options.scale;

    // --- Wire the pipeline together (constitution V) ------------------------
    // Interfaces are chosen at construction and injected by reference, never
    // selected by #ifdef -- this is the entire point of Engine taking
    // PitchDetector&/ScaleQuantizer&/PitchCorrector& rather than owning
    // concrete types itself. Stage 0's choices:
    opentune::AutocorrelationDetector
        detector; // naive autocorrelation (Stage 1 replaces with YinDetector)
    opentune::ScaleQuantizer quantizer(params.scale);
    // Naive, deliberately-wrong corrector (see ResampleCorrector.h) -- it
    // resamples, which shifts pitch AND formants (the "chipmunk" effect) and
    // drifts duration. A concurrent task is landing SignalsmithCorrector
    // (Stage 2, formant-preserving); swapping it in is meant to be exactly
    // this one line changed, nothing else in this file:
    opentune::ResampleCorrector corrector;

    Engine engine(detector, quantizer, corrector, params);

    // Block size: the audio contract's nominal block (specs.md section 6)
    // and the size a real-time host would use at 48 kHz (5.33 ms/block).
    // Using it here, offline, is what makes "Stage 3 changes only the
    // caller" true -- the engine only ever sees 256-sample (or smaller,
    // for the final block -- see the loop below) calls, exactly as it will
    // from a live audio callback.
    constexpr int kBlockSize = 256;
    engine.prepare(input.sampleRate, kBlockSize);

    // --- Process ------------------------------------------------------------
    opentune::host::WavData output;
    output.sampleRate = input.sampleRate;
    output.samples.resize(input.samples.size());

    const std::size_t totalSamples = input.samples.size();
    std::size_t offset = 0;
    while (offset < totalSamples) {
        // Feed fixed 256-sample blocks. The final block is almost always
        // shorter than 256 samples (the file length is rarely an exact
        // multiple of the block size) -- Engine::process accepts any
        // n <= maxBlockSize (specs.md section 6), so that last call simply
        // passes the true remaining sample count rather than padding the
        // tail with silence. Padding would ask the engine to "process"
        // samples that were never in the file and would leave the output
        // buffer either too long or (if truncated back down) unaffected;
        // passing the exact remaining count keeps output.samples the same
        // length as input.samples with no invented or dropped samples.
        const std::size_t remaining = totalSamples - offset;
        const std::size_t blockLen = remaining < static_cast<std::size_t>(kBlockSize)
                                         ? remaining
                                         : static_cast<std::size_t>(kBlockSize);
        engine.process(input.samples.data() + offset, output.samples.data() + offset,
                       static_cast<int>(blockLen));
        offset += blockLen;
    }

    // --- Write output ---------------------------------------------------------
    try {
        opentune::host::writeMono(options.outputPath, output);
    } catch (const std::exception& error) {
        std::cerr << "opentune-cli: " << error.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
