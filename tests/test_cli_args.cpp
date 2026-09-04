// T0.11: unit tests for opentune-cli's argument parsing.
//
// Per the T0.11 brief, the CLI's genuinely unit-testable surface is argument
// parsing, key parsing, and error paths -- NOT "the output is audibly
// pitch-shifted," which is an owner-verified, by-ear judgment call
// (constitution VII) and is not asserted anywhere in this file.
#include "doctest.h"
#include "tools/autotune-cli/CliArgs.h"

#include <cstring>
#include <vector>

using opentune::Scale;
using opentune::ScaleType;
using opentune::host::CliOptions;
using opentune::host::parseArgs;
using opentune::host::parseKey;
using opentune::host::parseStrength;

namespace {

// argv/argc are notoriously fiddly to build by hand in a test: char** must
// point at non-const, NUL-terminated buffers that outlive the call. This
// helper owns the storage (as std::vector<std::string>, which will not
// reallocate mid-call because argv is built after every string exists) and
// hands back a char* argv array built from it.
std::vector<char*> makeArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        // const_cast is safe here: parseArgs never writes through argv[i].
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    return argv;
}

} // namespace

TEST_CASE("parseKey: root with no accidental and major mode") {
    const Scale scale = parseKey("C:major");
    CHECK(scale.rootMidiNote == 60); // C4
    CHECK(scale.type == ScaleType::Major);
}

TEST_CASE("parseKey: sharp root") {
    const Scale scale = parseKey("F#:minor");
    CHECK(scale.rootMidiNote == 66); // F4 (65) + 1
    CHECK(scale.type == ScaleType::NaturalMinor);
}

TEST_CASE("parseKey: flat root wraps below C correctly") {
    const Scale scale = parseKey("Cb:chromatic");
    // pitchClass = C(0) - 1 = -1, wrapped into [0,11] -> 11 (i.e. Cb == B),
    // then re-added to the MIDI-C4 base: 60 + 11 = 71 (B4).
    CHECK(scale.rootMidiNote == 71);
    CHECK(scale.type == ScaleType::Chromatic);
}

TEST_CASE("parseKey: all documented mode aliases are accepted") {
    CHECK(parseKey("A:major").type == ScaleType::Major);
    CHECK(parseKey("A:minor").type == ScaleType::NaturalMinor);
    CHECK(parseKey("A:natural_minor").type == ScaleType::NaturalMinor);
    CHECK(parseKey("A:harmonic_minor").type == ScaleType::HarmonicMinor);
    CHECK(parseKey("A:pentatonic").type == ScaleType::Pentatonic);
    CHECK(parseKey("A:chromatic").type == ScaleType::Chromatic);
}

TEST_CASE("parseKey: missing ':' separator throws") {
    CHECK_THROWS_AS(parseKey("Cmajor"), std::invalid_argument);
}

TEST_CASE("parseKey: empty root throws") {
    CHECK_THROWS_AS(parseKey(":major"), std::invalid_argument);
}

TEST_CASE("parseKey: unrecognised root letter throws") {
    CHECK_THROWS_AS(parseKey("H:major"), std::invalid_argument);
}

TEST_CASE("parseKey: bad accidental throws") {
    CHECK_THROWS_AS(parseKey("Cx:major"), std::invalid_argument);
}

TEST_CASE("parseKey: unrecognised mode throws") {
    CHECK_THROWS_AS(parseKey("C:doowop"), std::invalid_argument);
}

TEST_CASE("parseKey: mode name is case-sensitive (typo'd casing is rejected, not guessed)") {
    CHECK_THROWS_AS(parseKey("C:Major"), std::invalid_argument);
}

// Range checked at both endpoints (docs/lessons.md), not just "somewhere in
// the middle is fine."
TEST_CASE("parseStrength: both endpoints of the valid range are accepted") {
    CHECK(parseStrength("0.0") == doctest::Approx(0.0f));
    CHECK(parseStrength("1.0") == doctest::Approx(1.0f));
}

TEST_CASE("parseStrength: just outside either endpoint throws") {
    CHECK_THROWS_AS(parseStrength("-0.01"), std::invalid_argument);
    CHECK_THROWS_AS(parseStrength("1.01"), std::invalid_argument);
}

TEST_CASE("parseStrength: non-numeric text throws") {
    CHECK_THROWS_AS(parseStrength("loud"), std::invalid_argument);
}

TEST_CASE("parseStrength: trailing garbage after a valid number throws") {
    CHECK_THROWS_AS(parseStrength("0.8abc"), std::invalid_argument);
}

TEST_CASE("parseArgs: minimal positional-only invocation") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav"};
    std::vector<char*> argv = makeArgv(args);
    const CliOptions options = parseArgs(static_cast<int>(argv.size()), argv.data());

    CHECK_FALSE(options.showHelp);
    CHECK(options.inputPath == "in.wav");
    CHECK(options.outputPath == "out.wav");
    CHECK(options.scale.type == ScaleType::Chromatic); // default: no --key given
    CHECK(options.strength == doctest::Approx(0.8f));  // default
}

TEST_CASE("parseArgs: --key and --strength are both honoured") {
    std::vector<std::string> args = {"opentune-cli", "in.wav",     "out.wav", "--key",
                                     "D:major",      "--strength", "0.5"};
    std::vector<char*> argv = makeArgv(args);
    const CliOptions options = parseArgs(static_cast<int>(argv.size()), argv.data());

    CHECK(options.inputPath == "in.wav");
    CHECK(options.outputPath == "out.wav");
    CHECK(options.scale.rootMidiNote == 62); // D4
    CHECK(options.scale.type == ScaleType::Major);
    CHECK(options.strength == doctest::Approx(0.5f));
}

TEST_CASE("parseArgs: --help short-circuits the positional-argument requirement") {
    std::vector<std::string> args = {"opentune-cli", "--help"};
    std::vector<char*> argv = makeArgv(args);
    const CliOptions options = parseArgs(static_cast<int>(argv.size()), argv.data());
    CHECK(options.showHelp);
}

TEST_CASE("parseArgs: missing positional arguments throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}

TEST_CASE("parseArgs: too many positional arguments throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav", "extra.wav"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}

TEST_CASE("parseArgs: unrecognised flag throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav", "--bogus"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}

TEST_CASE("parseArgs: --key without a following value throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav", "--key"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}

TEST_CASE("parseArgs: --strength without a following value throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav", "--strength"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}

TEST_CASE("parseArgs: an out-of-range --strength reaching through parseArgs throws") {
    std::vector<std::string> args = {"opentune-cli", "in.wav", "out.wav", "--strength", "2.0"};
    std::vector<char*> argv = makeArgv(args);
    CHECK_THROWS_AS(parseArgs(static_cast<int>(argv.size()), argv.data()), std::invalid_argument);
}
