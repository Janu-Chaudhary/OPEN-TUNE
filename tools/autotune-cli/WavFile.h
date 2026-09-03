// Mono float WAV I/O for host tools, built on dr_wav (third_party/dr_wav).
//
// This is HOST code, not engine code: it lives in `tools/`, is included from
// `namespace opentune::host` (not `opentune`), and is allowed to throw and do
// file I/O (constitution II binds only the real-time audio callback path;
// nothing here ever runs on that thread). `engine/` must never include this
// header or dr_wav.h — see constitution IV.
#pragma once

#include "dr_wav.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace opentune::host {

// Decoded (or about-to-be-encoded) mono audio: one float sample per frame,
// plus the sample rate it was captured/should be played at. The engine's
// audio contract (specs.md §6) is mono float at a caller-supplied sample
// rate, so this is the natural shape for handing audio to and from it.
struct WavData {
    std::vector<float> samples;
    double sampleRate = 0.0;
};

// Reads `path` and converts it to mono float, regardless of the file's
// original format.
//
// dr_wav's `drwav_open_file_and_read_pcm_frames_f32` does the format
// conversion for us: it accepts 8/16/24/32-bit integer PCM, 32-bit float, and
// several other encodings, and always hands back interleaved 32-bit float
// samples, so this function does not need to branch on bit depth itself.
//
// Downmixing: if the source file has more than one channel, we downmix to
// mono by averaging all channels of each frame. This is the simplest correct
// choice for a pitch-correction tool — it preserves overall level for
// centered (dual-mono) content and avoids the asymmetry of e.g. taking only
// the left channel, at the cost of some cancellation if left and right are
// out of phase. Good enough for a CLI harness reading vocal takes; a
// production tool might expose a channel-selection option instead.
//
// Throws std::runtime_error (with `path` in the message) if the file cannot
// be opened or decoded.
inline WavData readMono(const std::string& path) {
    unsigned int channels = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 totalFrameCount = 0;

    float* interleaved = drwav_open_file_and_read_pcm_frames_f32(
        path.c_str(), &channels, &sampleRate, &totalFrameCount, nullptr);

    if (interleaved == nullptr) {
        throw std::runtime_error("WavFile: failed to open or decode WAV file: " + path);
    }

    WavData result;
    result.sampleRate = static_cast<double>(sampleRate);
    result.samples.resize(static_cast<std::size_t>(totalFrameCount));

    if (channels == 1) {
        // Already mono: copy straight across.
        for (drwav_uint64 frame = 0; frame < totalFrameCount; ++frame) {
            result.samples[static_cast<std::size_t>(frame)] = interleaved[frame];
        }
    } else {
        // Downmix by averaging every channel in the frame. See the function
        // comment above for why averaging (rather than e.g. left-only).
        for (drwav_uint64 frame = 0; frame < totalFrameCount; ++frame) {
            float sum = 0.0f;
            const drwav_uint64 base = frame * channels;
            for (unsigned int ch = 0; ch < channels; ++ch) {
                sum += interleaved[base + ch];
            }
            result.samples[static_cast<std::size_t>(frame)] = sum / static_cast<float>(channels);
        }
    }

    // dr_wav allocated `interleaved` with its own allocator; it must be freed
    // with drwav_free, not delete/free, in case a custom allocator was used
    // elsewhere in the process (we pass nullptr above, so it is malloc/free
    // under the hood, but drwav_free is the documented, allocator-agnostic
    // way to release it).
    drwav_free(interleaved, nullptr);

    return result;
}

// Writes `data` to `path` as a 32-bit IEEE float, mono WAV file.
//
// 32-bit float is chosen (rather than 16-bit integer) so that a
// write-then-read round trip is lossless: every value representable as a
// `float` in `data.samples` is written and read back bit-for-bit identical,
// which is the done-criterion for T0.10. An integer format would quantize.
//
// Throws std::runtime_error (with `path` in the message) if the file cannot
// be created or the write fails.
inline void writeMono(const std::string& path, const WavData& data) {
    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = 1;
    format.sampleRate = static_cast<drwav_uint32>(data.sampleRate);
    format.bitsPerSample = 32;

    drwav wav;
    if (drwav_init_file_write(&wav, path.c_str(), &format, nullptr) != DRWAV_TRUE) {
        throw std::runtime_error("WavFile: failed to create WAV file for writing: " + path);
    }

    const drwav_uint64 frameCount = data.samples.size();
    const drwav_uint64 framesWritten =
        drwav_write_pcm_frames(&wav, frameCount, data.samples.data());
    drwav_uninit(&wav);

    if (framesWritten != frameCount) {
        throw std::runtime_error("WavFile: short write while writing WAV file: " + path);
    }
}

} // namespace opentune::host
