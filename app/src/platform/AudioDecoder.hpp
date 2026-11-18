#pragma once
// Compressed / PCM audio file decoding for backing tracks (off the audio thread only).
//
//   macOS    ExtAudioFile (Core Audio): WAV, AIFF, CAF, MP3, AAC/M4A, ALAC, FLAC, Opus … with Apple's mastering-
//            quality sample-rate converter.
//   others   miniaudio's built-in decoders (dr_wav / dr_flac / dr_mp3; app/third_party/miniaudio): WAV (incl.
//            RF64/W64), AIFF, FLAC, MP3, resampled by a Kaiser-windowed sinc converter (≈ −90 dB stop band).
//            AAC/M4A/ALAC need a platform codec: Windows Media Foundation (IMFSourceReader) is the planned path on
//            Windows; on Linux they stay unsupported unless a system codec (e.g. FFmpeg/GStreamer) is added.
//            OGG Vorbis can be enabled by compiling stb_vorbis into the miniaudio translation unit.
#include <cstdint>
#include <memory>
#include <string>

namespace rig::platform {

struct AudioFileInfo {
    double seconds = 0.0;
    double sample_rate = 0.0;
    int channels = 0;
    std::string title, artist; // from the file's tags when it has them
    std::string format;        // "MP3", "AAC", "WAV" …
};

// Opens the file and reads its length / tags without decoding it. False (and `err`) when it can't be read.
bool probeAudioFile(const std::string& path, AudioFileInfo& info, std::string* err = nullptr);

// Sequential reader delivering interleaved float frames (1 or 2 channels: mono stays mono, more than two keep the
// first pair) at the rate given to openAudioFile.
class AudioFileReader {
public:
    virtual ~AudioFileReader() = default;
    virtual int channels() const = 0;
    virtual double sourceRate() const = 0;
    virtual int64_t sourceFrames() const = 0; // length at the source rate (0 when unknown)
    // Up to `frames` frames into dst (frames × channels() floats). 0 at the end, −1 on a decode error (err set).
    virtual int64_t read(float* dst, int64_t frames, std::string* err) = 0;
};

std::unique_ptr<AudioFileReader> openAudioFile(const std::string& path, double target_rate, std::string* err = nullptr);

} // namespace rig::platform
