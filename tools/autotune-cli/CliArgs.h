// Command-line argument parsing for opentune-cli.
//
// This is HOST code (constitution IV): it lives in `tools/`, throws on bad
// input, and does no real-time work at all -- parsing argv happens once,
// long before any audio block is processed. `engine/` must never include
// this header.
//
// Split out of main.cpp (rather than left inline there) so it can be unit
// tested directly: argument parsing, key parsing, and strength validation
// are the genuinely unit-testable parts of this CLI (constitution VI/VII --
// "does it process real audio and sound corrected" is an owner-verified,
// by-ear judgment call, not something a unit test can assert).
#pragma once

#include "opentune/ScaleQuantizer.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace opentune::host {

// Everything main() needs, once parsing succeeds.
struct CliOptions {
    // Set when `--help`/`-h` was requested. When true, every other field is
    // unspecified/default -- main() should print kUsageText and exit before
    // looking at them, exactly as it would refuse to open `inputPath` as a
    // WAV file if it tried.
    bool showHelp = false;

    std::string inputPath;
    std::string outputPath;

    // Parsed from `--key <root>:<mode>` (default: chromatic, no root).
    // IMPORTANT (see kUsageText and Engine.h): Stage 0's ScaleQuantizer
    // treats every ScaleType identically to Chromatic regardless of what is
    // parsed here -- see ScaleQuantizer.h's class comment. Real scale
    // tables are Stage 5 (T5.1). We still parse and validate `--key`
    // strictly, both so the flag's surface is stable once Stage 5 lands and
    // so a typo'd key is caught here rather than silently ignored.
    Scale scale = {0, ScaleType::Chromatic};

    // Parsed from `--strength <0.0-1.0>` (default: Params' own default,
    // 0.8f -- see main.cpp, which copies this into a fresh Params rather
    // than duplicating the default here).
    // IMPORTANT (see kUsageText): Stage 0 parses and validates this value
    // and stores it on Params, but the engine does not yet act on it --
    // every voiced frame is corrected at full strength regardless. Applying
    // it is T4.2.
    float strength = 0.8f;
};

// The full --help text. Deliberately explicit about what Stage 0 does and
// does NOT do (task brief: "do not imply working features") -- printed both
// for `--help`/`-h` and, prefixed with an error, whenever argument parsing
// fails.
inline const char* kUsageText =
    "opentune-cli -- Stage 0 offline pitch-correction test harness\n"
    "\n"
    "Usage:\n"
    "  opentune-cli <in.wav> <out.wav> [--key <root>:<mode>] [--strength <0.0-1.0>]\n"
    "\n"
    "Arguments:\n"
    "  <in.wav>   Input WAV file (any dr_wav-supported format; downmixed to mono).\n"
    "  <out.wav>  Output path; written as 32-bit float mono WAV.\n"
    "\n"
    "Options:\n"
    "  --key <root>:<mode>   e.g. C:major, F#:minor, Bb:harmonic_minor. Parsed and\n"
    "                        validated, but Stage 0's quantizer is chromatic-only:\n"
    "                        it snaps to the nearest semitone regardless of key.\n"
    "                        Real scale tables land in Stage 5 (T5.1). Default: no\n"
    "                        key (chromatic).\n"
    "  --strength <0.0-1.0>  Correction strength. Parsed and stored, but NOT yet\n"
    "                        applied -- Stage 0 corrects every voiced frame at full\n"
    "                        strength regardless of this value. Applying it is\n"
    "                        T4.2. Default: 0.8.\n"
    "  -h, --help            Show this text and exit.\n"
    "\n"
    "Stage 0 honesty note: this tool wires the real pipeline together (detect ->\n"
    "quantize -> correct) using the naive Stage 0 implementations. It is meant to\n"
    "make the *problem* audible (chipmunk effect, warbling), not to sound good yet\n"
    "-- see tasks.md's Stage 0 checkpoint.\n";

namespace detail {

// C..B natural pitch classes, semitones above C, per the twelve-tone
// equal-tempered grid (specs.md section 6 -- A4 = 440 Hz = MIDI 69, and MIDI
// note numbers increase by 1 per semitone, so this table is really just
// "how far is this natural note from C").
inline int naturalPitchClass(char letter) {
    switch (letter) {
    case 'C':
        return 0;
    case 'D':
        return 2;
    case 'E':
        return 4;
    case 'F':
        return 5;
    case 'G':
        return 7;
    case 'A':
        return 9;
    case 'B':
        return 11;
    default:
        throw std::invalid_argument(std::string("--key: unrecognised root note letter '") + letter +
                                    "' (expected A-G)");
    }
}

} // namespace detail

// Parses the `<root>:<mode>` half of `--key` (e.g. "C:major", "F#:minor",
// "Bb:harmonic_minor") into a Scale.
//
// The root is one letter A-G, optionally followed by a single accidental:
// '#' (sharp, +1 semitone) or 'b' (flat, -1 semitone) -- lowercase 'b' only,
// so it can never be confused with the natural note B (always uppercase).
// The resulting root is expressed as MIDI note 60 (C4) plus that offset, per
// Scale::rootMidiNote's contract (ScaleQuantizer.h) -- any octave would do
// since only the pitch class matters to quantization, and C4 matches the
// Params.h/specs.md convention of octave 4 as the nominal reference.
//
// Throws std::invalid_argument, naming the problem, on: no ':' separator,
// an empty root, an unrecognised root letter, an accidental that is neither
// '#' nor 'b', a root longer than two characters, or an unrecognised mode
// name. Mode names are matched case-sensitively against the lowercase forms
// below -- "Major" or "MAJOR" are rejected rather than silently accepted,
// so a typo is caught rather than guessed at.
inline Scale parseKey(const std::string& keyArg) {
    const std::size_t colon = keyArg.find(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument(
            "--key must be in the form <root>:<mode> (e.g. C:major), got: " + keyArg);
    }

    const std::string rootStr = keyArg.substr(0, colon);
    const std::string modeStr = keyArg.substr(colon + 1);

    if (rootStr.empty()) {
        throw std::invalid_argument("--key is missing a root note before ':': " + keyArg);
    }
    if (rootStr.size() > 2) {
        throw std::invalid_argument(
            "--key root note must be a letter with an optional # or b, got: " + rootStr);
    }

    int pitchClass = detail::naturalPitchClass(rootStr[0]);
    if (rootStr.size() == 2) {
        if (rootStr[1] == '#') {
            pitchClass += 1;
        } else if (rootStr[1] == 'b') {
            pitchClass -= 1;
        } else {
            throw std::invalid_argument("--key accidental must be '#' or 'b', got: " + rootStr);
        }
    }
    // Wrap into [0, 11]: Cb and B# are legal spellings that land outside the
    // natural-note table's raw range.
    pitchClass = ((pitchClass % 12) + 12) % 12;

    ScaleType type;
    if (modeStr == "chromatic") {
        type = ScaleType::Chromatic;
    } else if (modeStr == "major") {
        type = ScaleType::Major;
    } else if (modeStr == "minor" || modeStr == "natural_minor") {
        type = ScaleType::NaturalMinor;
    } else if (modeStr == "harmonic_minor") {
        type = ScaleType::HarmonicMinor;
    } else if (modeStr == "pentatonic") {
        type = ScaleType::Pentatonic;
    } else {
        throw std::invalid_argument(
            "--key mode must be one of chromatic, major, minor, harmonic_minor, "
            "pentatonic, got: " +
            modeStr);
    }

    constexpr int kMidiC4 = 60;
    return Scale{kMidiC4 + pitchClass, type};
}

// Parses `--strength`'s argument into a validated [0.0, 1.0] value (specs.md
// section 7.1 -- Params::strength's documented range). Throws
// std::invalid_argument, naming the problem, if the text is not a plain
// number, has trailing garbage after the number, or falls outside
// [0.0, 1.0] -- checked at both endpoints (docs/lessons.md: "test a
// declared range at both endpoints"), so e.g. "-0.01" and "1.01" are both
// rejected while "0.0" and "1.0" are both accepted.
inline float parseStrength(const std::string& strengthArg) {
    std::size_t parsedChars = 0;
    float value = 0.0f;
    try {
        value = std::stof(strengthArg, &parsedChars);
    } catch (const std::exception&) {
        throw std::invalid_argument("--strength must be a number between 0.0 and 1.0, got: " +
                                    strengthArg);
    }
    if (parsedChars != strengthArg.size()) {
        throw std::invalid_argument("--strength must be a number between 0.0 and 1.0, got: " +
                                    strengthArg);
    }
    if (value < 0.0f || value > 1.0f) {
        throw std::invalid_argument("--strength must be between 0.0 and 1.0, got: " + strengthArg);
    }
    return value;
}

// Parses the full argv (argv[0] is the program name, per convention, and is
// ignored). Throws std::invalid_argument, naming the problem, on: an
// unrecognised flag, `--key`/`--strength` given without a following value,
// a malformed `--key` or out-of-range `--strength` (see parseKey/
// parseStrength above), or a positional-argument count other than exactly
// two (missing input/output path, or extra trailing arguments) -- unless
// `-h`/`--help` was given, in which case positionals are not required at
// all and every other field is left at its default (see CliOptions).
inline CliOptions parseArgs(int argc, char** argv) {
    CliOptions options;
    std::array<std::string, 2> positionals;
    int positionalCount = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
            continue;
        }

        if (arg == "--key") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--key requires a value (e.g. --key C:major)");
            }
            options.scale = parseKey(argv[++i]);
            continue;
        }

        if (arg == "--strength") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--strength requires a value (e.g. --strength 0.8)");
            }
            options.strength = parseStrength(argv[++i]);
            continue;
        }

        if (!arg.empty() && arg[0] == '-' && arg != "-") {
            throw std::invalid_argument("unrecognised option: " + arg);
        }

        if (positionalCount >= 2) {
            throw std::invalid_argument("too many arguments (expected <in.wav> <out.wav>), got: " +
                                        arg);
        }
        positionals[static_cast<std::size_t>(positionalCount)] = arg;
        ++positionalCount;
    }

    if (options.showHelp) {
        return options;
    }

    if (positionalCount != 2) {
        throw std::invalid_argument(
            "expected exactly two arguments: <in.wav> <out.wav> (see --help)");
    }

    options.inputPath = positionals[0];
    options.outputPath = positionals[1];
    return options;
}

} // namespace opentune::host
