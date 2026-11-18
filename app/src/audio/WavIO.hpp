#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace rig::audio {

struct WavData {
    uint32_t sample_rate = 44100;
    uint16_t num_channels = 2;
    std::vector<float> left;
    std::vector<float> right;
};

inline bool readWavFile(const std::string& path, WavData& out, std::string* err = nullptr) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err) *err = "Cannot open WAV file: " + path;
        return false;
    }
    const auto sz = f.tellg();
    if (sz < 44) {
        if (err) *err = "WAV file too small: " + path;
        return false;
    }
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);

    if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        if (err) *err = "Not a valid RIFF WAVE file: " + path;
        return false;
    }

    uint16_t format_tag = 1;
    uint16_t channels = 1;
    uint32_t sr = 44100;
    uint16_t bits = 16;
    const uint8_t* data_ptr = nullptr;
    uint32_t data_size = 0;

    std::size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        uint32_t chunk_sz = 0;
        std::memcpy(&chunk_sz, buf.data() + pos + 4, 4);
        const uint32_t avail = static_cast<uint32_t>(buf.size() - (pos + 8));
        const uint32_t safe_sz = std::min(chunk_sz, avail);

        if (std::memcmp(buf.data() + pos, "fmt ", 4) == 0 && safe_sz >= 16) {
            std::memcpy(&format_tag, buf.data() + pos + 8, 2);
            std::memcpy(&channels, buf.data() + pos + 10, 2);
            std::memcpy(&sr, buf.data() + pos + 12, 4);
            std::memcpy(&bits, buf.data() + pos + 22, 2);
        } else if (std::memcmp(buf.data() + pos, "data", 4) == 0) {
            data_ptr = buf.data() + pos + 8;
            data_size = safe_sz;
            break;
        }
        pos += 8 + chunk_sz + (chunk_sz & 1);
    }

    if (!data_ptr || data_size == 0 || channels == 0) {
        if (err) *err = "Missing data chunk in WAV: " + path;
        return false;
    }

    out.sample_rate = sr;
    out.num_channels = channels;
    const std::size_t bytes_per_sample = bits / 8;
    const std::size_t frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0) return false;
    const std::size_t num_frames = data_size / frame_bytes;

    out.left.resize(num_frames);
    out.right.resize(num_frames);

    for (std::size_t i = 0; i < num_frames; ++i) {
        float ch_vals[2] = {0.0f, 0.0f};
        for (uint16_t c = 0; c < std::min<uint16_t>(channels, 2); ++c) {
            const uint8_t* p = data_ptr + i * frame_bytes + c * bytes_per_sample;
            if (bits == 16) {
                int16_t v = 0;
                std::memcpy(&v, p, 2);
                ch_vals[c] = static_cast<float>(v) / 32768.0f;
            } else if (bits == 24) {
                int32_t v = (static_cast<int32_t>(p[2]) << 24) | (static_cast<int32_t>(p[1]) << 16) | (static_cast<int32_t>(p[0]) << 8);
                v >>= 8;
                ch_vals[c] = static_cast<float>(v) / 8388608.0f;
            } else if (bits == 32 && format_tag == 3) {
                float v = 0.0f;
                std::memcpy(&v, p, 4);
                ch_vals[c] = v;
            }
        }
        out.left[i] = ch_vals[0];
        out.right[i] = (channels >= 2) ? ch_vals[1] : ch_vals[0];
    }

    return true;
}

inline bool writeWavFile16(const std::string& path, uint32_t sample_rate, const std::vector<float>& left, const std::vector<float>& right) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const uint32_t num_frames = static_cast<uint32_t>(std::min(left.size(), right.size()));
    const uint16_t channels = 2;
    const uint16_t bits = 16;
    const uint16_t block_align = channels * (bits / 8);
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_bytes = num_frames * block_align;
    const uint32_t riff_size = 36 + data_bytes;
    const uint32_t fmt_size = 16;
    const uint16_t audio_format = 1;

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riff_size), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmt_size), 4);
    f.write(reinterpret_cast<const char*>(&audio_format), 2);
    f.write(reinterpret_cast<const char*>(&channels), 2);
    f.write(reinterpret_cast<const char*>(&sample_rate), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bits), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);

    std::vector<int16_t> pcm(num_frames * 2);
    for (uint32_t i = 0; i < num_frames; ++i) {
        // Straight conversion (the rack's master limiter already guarantees headroom).
        const float l = std::clamp(left[i], -1.0f, 1.0f);
        const float r = std::clamp(right[i], -1.0f, 1.0f);
        pcm[2 * i + 0] = static_cast<int16_t>(std::clamp(l * 32767.0f, -32768.0f, 32767.0f));
        pcm[2 * i + 1] = static_cast<int16_t>(std::clamp(r * 32767.0f, -32768.0f, 32767.0f));
    }
    f.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return true;
}

} // namespace rig::audio
