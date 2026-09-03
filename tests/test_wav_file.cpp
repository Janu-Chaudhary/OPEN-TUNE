// T0.10 done-criterion: a written-then-read WAV round-trips bit-identically.
//
// This is also the one translation unit that provides dr_wav's implementation
// (DR_WAV_IMPLEMENTATION must appear in exactly one .cpp). T0.11 will add
// tools/autotune-cli's own .cpp for the CLI binary and move this macro there;
// until then, the test binary is the only host consumer of dr_wav, so it is
// the natural home for it.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "doctest.h"
#include "support/Signals.h"
#include "tools/autotune-cli/WavFile.h"

#include <chrono>
#include <filesystem>
#include <vector>

using opentune::host::WavData;

namespace {

// A path under the system temp directory, unique to this process/run so
// concurrent test runs (and repeated local runs) never collide. Removed by
// the caller once the test is done with it. Built from a steady-clock tick
// count rather than a process id to stay platform-free (no <unistd.h>).
std::filesystem::path tempWavPath(const char* name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("opentune_test_") + name + "_" + std::to_string(tick) + ".wav");
}

} // namespace

// A 32-bit float WAV round trips bit-identically: every sample written comes
// back exactly, and the sample rate is preserved. This is the primary
// done-criterion for T0.10, and it is *exact* equality on purpose (not a
// tolerance) — float32 samples written as float32 and read back as float32
// undergo no lossy conversion, so any difference at all is a bug.
TEST_CASE("WavFile: mono float round trip is bit-identical") {
    const double sampleRate = 48000.0;
    const std::vector<float> original = opentune::test::sine(440.0f, sampleRate, 2000);

    const std::filesystem::path path = tempWavPath("roundtrip");

    WavData written;
    written.samples = original;
    written.sampleRate = sampleRate;
    opentune::host::writeMono(path.string(), written);

    WavData readBack = opentune::host::readMono(path.string());

    std::filesystem::remove(path);

    REQUIRE(readBack.samples.size() == original.size());
    CHECK(readBack.sampleRate == doctest::Approx(sampleRate));
    for (std::size_t i = 0; i < original.size(); ++i) {
        // Bit-identical: compare with ==, not a tolerance. See comment above.
        CHECK(readBack.samples[i] == original[i]);
    }
}

// readMono() must fail loudly and usefully rather than crash or return
// garbage when the file does not exist.
TEST_CASE("WavFile: reading a missing file throws a useful error") {
    const std::filesystem::path path = tempWavPath("does_not_exist");
    std::filesystem::remove(path); // just in case a previous run left it behind

    bool threw = false;
    try {
        opentune::host::readMono(path.string());
    } catch (const std::runtime_error& error) {
        threw = true;
        // A "useful" message names the offending path, not just "failed".
        CHECK(doctest::String(error.what()) == doctest::Contains(path.string().c_str()));
    }
    CHECK(threw);
}

// readMono() must convert any input format to mono float: this test writes a
// stereo 16-bit PCM WAV by hand (bypassing WavFile.h, which only ever writes
// float mono) and checks that reading it back downmixes channels by
// averaging and converts int16 -> float correctly.
//
// int16 -> float32 conversion is not the identity (it is a division), so this
// assertion uses a small tolerance rather than exact equality — unlike the
// float round trip above, which is genuinely lossless.
TEST_CASE("WavFile: readMono converts 16-bit stereo PCM to mono float") {
    const std::filesystem::path path = tempWavPath("stereo16");

    const drwav_uint32 sampleRate = 44100;
    const std::vector<drwav_int16> interleavedStereo = {
        // frame 0: left=+16384 (~0.5), right=-16384 (~-0.5) -> average ~0.0
        16384,
        -16384,
        // frame 1: left=+32767 (max), right=+32767 (max) -> average ~1.0
        32767,
        32767,
        // frame 2: left=0, right=-32768 (min) -> average ~-0.5
        0,
        -32768,
    };

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 2;
    format.sampleRate = sampleRate;
    format.bitsPerSample = 16;

    drwav wav;
    REQUIRE(drwav_init_file_write(&wav, path.string().c_str(), &format, nullptr) == DRWAV_TRUE);
    const drwav_uint64 frameCount = interleavedStereo.size() / format.channels;
    const drwav_uint64 framesWritten =
        drwav_write_pcm_frames(&wav, frameCount, interleavedStereo.data());
    drwav_uninit(&wav);
    REQUIRE(framesWritten == frameCount);

    const WavData readBack = opentune::host::readMono(path.string());
    std::filesystem::remove(path);

    REQUIRE(readBack.samples.size() == frameCount);
    CHECK(readBack.sampleRate == doctest::Approx(static_cast<double>(sampleRate)));

    const float tolerance = 1.0f / 32768.0f; // one int16 quantization step
    CHECK(readBack.samples[0] == doctest::Approx(0.0f).epsilon(tolerance));
    CHECK(readBack.samples[1] == doctest::Approx(1.0f).epsilon(tolerance));
    CHECK(readBack.samples[2] == doctest::Approx(-0.5f).epsilon(tolerance));
}
