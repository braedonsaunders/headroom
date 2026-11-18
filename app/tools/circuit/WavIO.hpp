#pragma once
// Minimal WAV reader/writer for the circuit tools (PCM 16/24/32, float 32; mono mixdown on read).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ce_tools {

inline bool readWav(const std::string& path, std::vector<double>& out, double& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24)); };
    size_t p = 12;
    int fmt = 0, ch = 0, bits = 0;
    size_t dataOff = 0, dataLen = 0;
    while (p + 8 <= d.size()) {
        const uint32_t len = u32(p + 4);
        if (!std::memcmp(d.data() + p, "fmt ", 4)) {
            fmt = static_cast<int>(u16(p + 8));
            ch = static_cast<int>(u16(p + 10));
            rate = u32(p + 12);
            bits = static_cast<int>(u16(p + 22));
            if (fmt == 0xFFFE && len >= 26) fmt = static_cast<int>(u16(p + 32));
        } else if (!std::memcmp(d.data() + p, "data", 4)) {
            dataOff = p + 8;
            dataLen = std::min<size_t>(len, d.size() - dataOff);
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!dataOff || ch <= 0) return false;
    const int bps = bits / 8;
    const size_t frames = dataLen / static_cast<size_t>(bps * ch);
    out.assign(frames, 0.0);
    for (size_t i = 0; i < frames; ++i) {
        double s = 0;
        for (int c = 0; c < ch; ++c) {
            const unsigned char* q = d.data() + dataOff + (i * static_cast<size_t>(ch) + static_cast<size_t>(c)) * static_cast<size_t>(bps);
            double v = 0;
            if (fmt == 3 && bits == 32) {
                float x;
                std::memcpy(&x, q, 4);
                v = x;
            } else if (bits == 16) {
                v = static_cast<int16_t>(q[0] | (q[1] << 8)) / 32768.0;
            } else if (bits == 24) {
                int32_t x = q[0] | (q[1] << 8) | (q[2] << 16);
                if (x & 0x800000) x |= ~0xFFFFFF;
                v = x / 8388608.0;
            } else if (bits == 32) {
                int32_t x;
                std::memcpy(&x, q, 4);
                v = x / 2147483648.0;
            }
            s += v;
        }
        out[i] = s / ch;
    }
    return true;
}

inline bool writeWav(const std::string& path, const std::vector<double>& x, double rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t n = static_cast<uint32_t>(x.size()), dataLen = n * 4, r = static_cast<uint32_t>(rate);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataLen);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(3);
    w16(1);
    w32(r);
    w32(r * 4);
    w16(4);
    w16(32);
    std::fwrite("data", 1, 4, f);
    w32(dataLen);
    for (double v : x) {
        const float s = static_cast<float>(v);
        std::fwrite(&s, 4, 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace ce_tools
