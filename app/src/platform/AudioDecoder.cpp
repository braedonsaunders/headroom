#include "AudioDecoder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#else
#include "miniaudio.h" // implementation compiled once in the `miniaudio` target (third_party/miniaudio/miniaudio.c)
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

namespace rig::platform {

#ifdef __APPLE__
// ───────────────────────────── Core Audio (ExtAudioFile) ─────────────────────────────
namespace {

std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[1024];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

std::string osErr(OSStatus st) {
    // Four-char codes read better than numbers in a log ('typ?', 'fmt?', …).
    const uint32_t v = static_cast<uint32_t>(st);
    char c[5] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v), 0};
    bool printable = true;
    for (int i = 0; i < 4; ++i) printable = printable && c[i] >= 32 && c[i] < 127;
    return printable ? std::string("'") + c + "'" : std::to_string(st);
}

struct ExtFile {
    ExtAudioFileRef f = nullptr;
    ~ExtFile() { if (f) ExtAudioFileDispose(f); }
};

bool openExt(const std::string& path, ExtFile& ef, std::string* err) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()),
                                                          static_cast<CFIndex>(path.size()), false);
    if (!url) { if (err) *err = "bad path"; return false; }
    const OSStatus st = ExtAudioFileOpenURL(url, &ef.f);
    CFRelease(url);
    if (st != noErr || !ef.f) {
        ef.f = nullptr;
        if (err) *err = "This file can't be read as audio (" + osErr(st) + ")";
        return false;
    }
    return true;
}

const char* formatName(UInt32 id, UInt32 file_type) {
    switch (id) {
    case kAudioFormatMPEGLayer3: return "MP3";
    case kAudioFormatMPEG4AAC: case kAudioFormatMPEG4AAC_HE: case kAudioFormatMPEG4AAC_HE_V2: case kAudioFormatMPEG4AAC_LD: return "AAC";
    case kAudioFormatAppleLossless: return "ALAC";
    case kAudioFormatFLAC: return "FLAC";
    case kAudioFormatOpus: return "Opus";
    case kAudioFormatLinearPCM:
        switch (file_type) {
        case kAudioFileWAVEType: case kAudioFileRF64Type: case kAudioFileWave64Type: case kAudioFileBW64Type: return "WAV";
        case kAudioFileAIFFType: case kAudioFileAIFCType: return "AIFF";
        case kAudioFileCAFType: return "CAF";
        default: return "PCM";
        }
    default: return "Audio";
    }
}

class ExtReader final : public AudioFileReader {
public:
    ExtFile ef;
    UInt32 ch = 2;
    double src_rate = 0.0;
    int64_t src_frames = 0;
    int channels() const override { return static_cast<int>(ch); }
    double sourceRate() const override { return src_rate; }
    int64_t sourceFrames() const override { return src_frames; }
    int64_t read(float* dst, int64_t frames, std::string* err) override {
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = ch;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(frames) * ch * 4;
        abl.mBuffers[0].mData = dst;
        UInt32 n = static_cast<UInt32>(frames);
        const OSStatus st = ExtAudioFileRead(ef.f, &n, &abl);
        if (st != noErr) {
            if (err) *err = "Decoding failed (" + osErr(st) + ")";
            return -1;
        }
        return n;
    }
};

} // namespace

bool probeAudioFile(const std::string& path, AudioFileInfo& info, std::string* err) {
    ExtFile ef;
    if (!openExt(path, ef, err)) return false;
    AudioStreamBasicDescription ff{};
    UInt32 sz = sizeof ff;
    SInt64 len = 0;
    UInt32 lsz = sizeof len;
    if (ExtAudioFileGetProperty(ef.f, kExtAudioFileProperty_FileDataFormat, &sz, &ff) != noErr ||
        ExtAudioFileGetProperty(ef.f, kExtAudioFileProperty_FileLengthFrames, &lsz, &len) != noErr || ff.mSampleRate <= 0 || len <= 0) {
        if (err) *err = "This file has no audio in it";
        return false;
    }
    info.sample_rate = ff.mSampleRate;
    info.channels = static_cast<int>(ff.mChannelsPerFrame);
    info.seconds = double(len) / ff.mSampleRate;
    AudioFileID af = nullptr;
    UInt32 asz = sizeof af;
    UInt32 file_type = 0;
    if (ExtAudioFileGetProperty(ef.f, kExtAudioFileProperty_AudioFile, &asz, &af) == noErr && af) {
        UInt32 tsz = sizeof file_type;
        AudioFileGetProperty(af, kAudioFilePropertyFileFormat, &tsz, &file_type);
        CFDictionaryRef dict = nullptr;
        UInt32 dsz = sizeof dict;
        if (AudioFileGetProperty(af, kAudioFilePropertyInfoDictionary, &dsz, &dict) == noErr && dict) {
            auto get = [&](const char* k) {
                CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
                const void* v = CFDictionaryGetValue(dict, key);
                CFRelease(key);
                return v && CFGetTypeID(v) == CFStringGetTypeID() ? cfString(static_cast<CFStringRef>(v)) : std::string();
            };
            info.title = get(kAFInfoDictionary_Title);
            info.artist = get(kAFInfoDictionary_Artist);
            CFRelease(dict);
        }
    }
    info.format = formatName(ff.mFormatID, file_type);
    return true;
}

std::unique_ptr<AudioFileReader> openAudioFile(const std::string& path, double target_rate, std::string* err) {
    auto r = std::make_unique<ExtReader>();
    if (!openExt(path, r->ef, err)) return nullptr;
    AudioStreamBasicDescription ff{};
    UInt32 sz = sizeof ff;
    SInt64 len = 0;
    UInt32 lsz = sizeof len;
    if (ExtAudioFileGetProperty(r->ef.f, kExtAudioFileProperty_FileDataFormat, &sz, &ff) != noErr || ff.mSampleRate <= 0) {
        if (err) *err = "This file has no audio in it";
        return nullptr;
    }
    ExtAudioFileGetProperty(r->ef.f, kExtAudioFileProperty_FileLengthFrames, &lsz, &len);
    r->src_rate = ff.mSampleRate;
    r->src_frames = len;
    r->ch = std::clamp<UInt32>(ff.mChannelsPerFrame, 1, 2);
    AudioStreamBasicDescription cf{};
    cf.mSampleRate = target_rate;
    cf.mFormatID = kAudioFormatLinearPCM;
    cf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cf.mChannelsPerFrame = r->ch;
    cf.mBitsPerChannel = 32;
    cf.mFramesPerPacket = 1;
    cf.mBytesPerFrame = cf.mBytesPerPacket = 4 * r->ch;
    const OSStatus st = ExtAudioFileSetProperty(r->ef.f, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf);
    if (st != noErr) {
        if (err) *err = "Can't convert this file (" + osErr(st) + ")";
        return nullptr;
    }
    // Best sample-rate converter quality (decoding is offline, so it can afford it).
    AudioConverterRef conv = nullptr;
    UInt32 csz = sizeof conv;
    if (ExtAudioFileGetProperty(r->ef.f, kExtAudioFileProperty_AudioConverter, &csz, &conv) == noErr && conv) {
        UInt32 q = kAudioConverterQuality_Max;
        AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterQuality, sizeof q, &q);
        UInt32 cx = kAudioConverterSampleRateConverterComplexity_Mastering;
        AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterComplexity, sizeof cx, &cx);
        CFArrayRef none = nullptr;
        ExtAudioFileSetProperty(r->ef.f, kExtAudioFileProperty_ConverterConfig, sizeof none, &none);
    }
    return r;
}

#else
// ───────────────────────────── miniaudio decoders + sinc resampler ─────────────────────────────
namespace {

// ---------- container sniffing and tags (title / artist) ----------
std::vector<unsigned char> readHead(const std::string& path, std::size_t max, std::vector<unsigned char>* tail128 = nullptr) {
#ifdef _WIN32
    const int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wn > 0 ? wn - 1 : 0), L'\0');
    if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    std::ifstream f(w.c_str(), std::ios::binary);
#else
    std::ifstream f(path, std::ios::binary);
#endif
    std::vector<unsigned char> b(max);
    if (!f) return {};
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(max));
    b.resize(static_cast<std::size_t>(f.gcount()));
    if (tail128) {
        f.clear();
        f.seekg(0, std::ios::end);
        const auto size = static_cast<long long>(f.tellg());
        if (size >= 128) {
            tail128->resize(128);
            f.seekg(size - 128);
            f.read(reinterpret_cast<char*>(tail128->data()), 128);
            if (f.gcount() != 128) tail128->clear();
        }
    }
    return b;
}

uint32_t be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t le32(const unsigned char* p) { return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0]; }
uint32_t syncsafe(const unsigned char* p) { return (uint32_t(p[0] & 127) << 21) | (uint32_t(p[1] & 127) << 14) | (uint32_t(p[2] & 127) << 7) | (p[3] & 127); }

void appendUtf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
}

std::string trimTag(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    std::size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    return s;
}

std::string latin1(const unsigned char* p, std::size_t n) {
    std::string o;
    for (std::size_t i = 0; i < n && p[i]; ++i) appendUtf8(o, p[i]);
    return o;
}

// ID3v2 text frame payload: encoding byte + text.
std::string id3Text(const unsigned char* p, std::size_t n) {
    if (n < 1) return {};
    const unsigned enc = p[0];
    ++p; --n;
    if (enc == 0) return trimTag(latin1(p, n));
    if (enc == 3) return trimTag(std::string(reinterpret_cast<const char*>(p), n));
    bool bigEndian = enc == 2;
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) { bigEndian = false; p += 2; n -= 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { bigEndian = true; p += 2; n -= 2; }
    }
    std::string o;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = bigEndian ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            const uint32_t lo = bigEndian ? (uint32_t(p[i + 2]) << 8 | p[i + 3]) : (uint32_t(p[i + 3]) << 8 | p[i + 2]);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        appendUtf8(o, c);
    }
    return trimTag(o);
}

void parseId3v2(const std::vector<unsigned char>& b, AudioFileInfo& info) {
    if (b.size() < 10) return;
    const unsigned ver = b[3];
    const std::size_t end = std::min<std::size_t>(b.size(), 10 + syncsafe(&b[6]));
    std::size_t p = 10;
    if (b[5] & 0x40 && ver >= 3 && p + 4 <= end) p += (ver == 4 ? syncsafe(&b[p]) : be32(&b[p]) + 4); // extended header
    const std::size_t idn = ver == 2 ? 3 : 4, hdr = ver == 2 ? 6 : 10;
    while (p + hdr <= end && b[p] != 0) {
        const std::string id(reinterpret_cast<const char*>(&b[p]), idn);
        std::size_t sz = ver == 2 ? (std::size_t(b[p + 3]) << 16 | std::size_t(b[p + 4]) << 8 | b[p + 5])
                                  : (ver == 4 ? syncsafe(&b[p + 4]) : be32(&b[p + 4]));
        p += hdr;
        if (p + sz > end) break;
        if (id == "TIT2" || id == "TT2") info.title = id3Text(&b[p], sz);
        else if (id == "TPE1" || id == "TP1") info.artist = id3Text(&b[p], sz);
        p += sz;
    }
}

void parseRiffInfo(const std::vector<unsigned char>& b, AudioFileInfo& info) {
    std::size_t p = 12;
    while (p + 8 <= b.size()) {
        const uint32_t sz = le32(&b[p + 4]);
        if (std::memcmp(&b[p], "LIST", 4) == 0 && p + 12 <= b.size() && std::memcmp(&b[p + 8], "INFO", 4) == 0) {
            std::size_t q = p + 12;
            const std::size_t e = std::min<std::size_t>(b.size(), p + 8 + sz);
            while (q + 8 <= e) {
                const uint32_t s2 = le32(&b[q + 4]);
                if (q + 8 + s2 > e) break;
                const std::string v = trimTag(std::string(reinterpret_cast<const char*>(&b[q + 8]), s2));
                if (std::memcmp(&b[q], "INAM", 4) == 0) info.title = v;
                else if (std::memcmp(&b[q], "IART", 4) == 0) info.artist = v;
                q += 8 + s2 + (s2 & 1);
            }
        }
        p += 8 + std::size_t(sz) + (sz & 1);
    }
}

void parseAiff(const std::vector<unsigned char>& b, AudioFileInfo& info) {
    std::size_t p = 12;
    while (p + 8 <= b.size()) {
        const uint32_t sz = be32(&b[p + 4]);
        if (p + 8 + sz <= b.size()) {
            const std::string v = trimTag(std::string(reinterpret_cast<const char*>(&b[p + 8]), sz));
            if (std::memcmp(&b[p], "NAME", 4) == 0) info.title = v;
            else if (std::memcmp(&b[p], "AUTH", 4) == 0) info.artist = v;
        }
        p += 8 + std::size_t(sz) + (sz & 1);
    }
}

void parseFlac(const std::vector<unsigned char>& b, AudioFileInfo& info) {
    std::size_t p = 4;
    while (p + 4 <= b.size()) {
        const bool last = b[p] & 0x80;
        const unsigned type = b[p] & 0x7F;
        const std::size_t len = std::size_t(b[p + 1]) << 16 | std::size_t(b[p + 2]) << 8 | b[p + 3];
        p += 4;
        if (type == 4 && p + len <= b.size()) { // VORBIS_COMMENT (little-endian lengths)
            std::size_t q = p;
            if (q + 4 > p + len) break;
            q += 4 + le32(&b[q]);
            if (q + 4 > p + len) break;
            uint32_t count = le32(&b[q]);
            q += 4;
            while (count-- && q + 4 <= p + len) {
                const uint32_t n = le32(&b[q]);
                q += 4;
                if (q + n > p + len) break;
                const std::string kv(reinterpret_cast<const char*>(&b[q]), n);
                q += n;
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string k = kv.substr(0, eq);
                for (auto& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (k == "TITLE") info.title = kv.substr(eq + 1);
                else if (k == "ARTIST") info.artist = kv.substr(eq + 1);
            }
        }
        p += len;
        if (last) break;
    }
}

// Container from the first bytes; tags filled where the container has them.
std::string sniff(const std::string& path, AudioFileInfo* tags) {
    std::vector<unsigned char> tail;
    const auto b = readHead(path, 1 << 20, tags ? &tail : nullptr);
    if (b.size() < 12) return {};
    auto is = [&](std::size_t at, const char* s) { return b.size() >= at + 4 && std::memcmp(&b[at], s, 4) == 0; };
    std::string fmt;
    if ((is(0, "RIFF") || is(0, "RF64") || is(0, "BW64")) && is(8, "WAVE")) { fmt = "WAV"; if (tags) parseRiffInfo(b, *tags); }
    else if (is(0, "riff")) fmt = "WAV"; // Wave64
    else if (is(0, "FORM") && (is(8, "AIFF") || is(8, "AIFC"))) { fmt = "AIFF"; if (tags) parseAiff(b, *tags); }
    else if (is(0, "fLaC")) { fmt = "FLAC"; if (tags) parseFlac(b, *tags); }
    else if (std::memcmp(b.data(), "ID3", 3) == 0) { fmt = "MP3"; if (tags) parseId3v2(b, *tags); }
    else if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) fmt = "MP3";
    else if (is(0, "OggS")) fmt = "OGG";
    if (fmt == "MP3" && tags && tail.size() == 128 && std::memcmp(tail.data(), "TAG", 3) == 0) {
        if (tags->title.empty()) tags->title = trimTag(latin1(&tail[3], 30));
        if (tags->artist.empty()) tags->artist = trimTag(latin1(&tail[33], 30));
    }
    return fmt;
}

// ---------- decoder ----------
struct MaDecoder {
    ma_decoder dec{};
    bool open = false;
    ~MaDecoder() { if (open) ma_decoder_uninit(&dec); }
};

bool openMa(const std::string& path, MaDecoder& d, std::string* err) {
    const ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0); // native channels and rate
#ifdef _WIN32
    const int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wn > 0 ? wn - 1 : 0), L'\0');
    if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    const ma_result r = ma_decoder_init_file_w(w.c_str(), &cfg, &d.dec);
#else
    const ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &d.dec);
#endif
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("This file can't be read as audio (") + ma_result_description(r) + ")";
        return false;
    }
    d.open = true;
    return true;
}

double besselI0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 64; ++k) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

class MaReader final : public AudioFileReader {
public:
    MaDecoder d;
    int ch = 2;          // delivered channels
    int src_ch = 2;      // decoder channels
    double src_rate = 0.0, dst_rate = 0.0;
    int64_t src_frames = 0;

    int channels() const override { return ch; }
    double sourceRate() const override { return src_rate; }
    int64_t sourceFrames() const override { return src_frames; }

    void setup() {
        pass_ = std::fabs(src_rate - dst_rate) < 1e-6;
        tmp_.assign(static_cast<std::size_t>(kChunk) * static_cast<std::size_t>(src_ch), 0.0f);
        if (pass_) return;
        // Kaiser-windowed sinc, 32 zero crossings each side at 1:1 (more when downsampling), β = 9 (≈ −90 dB).
        const double ratio = dst_rate / src_rate;
        fc_ = 0.5 * std::min(1.0, ratio) * 0.94;               // cycles per source sample
        half_ = static_cast<int>(std::ceil(32.0 / (2.0 * fc_))); // half-width in source samples
        const double beta = 9.0, i0b = besselI0(beta);
        table_.resize(static_cast<std::size_t>(2 * half_ * kPhases + 2));
        for (std::size_t q = 0; q < table_.size(); ++q) {
            const double x = double(q) / kPhases - half_;
            const double u = x / half_;
            const double win = std::fabs(u) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / i0b;
            const double a = 2.0 * fc_ * x;
            const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(3.14159265358979323846 * a) / (3.14159265358979323846 * a);
            table_[q] = static_cast<float>(2.0 * fc_ * sinc * win);
        }
        weights_.resize(static_cast<std::size_t>(2 * half_));
    }

    int64_t read(float* dst, int64_t frames, std::string* err) override {
        if (pass_) return decodeInto(dst, frames, err);
        const double step = src_rate / dst_rate;
        int64_t made = 0;
        while (made < frames) {
            const double t = double(out_n_) * step;  // output frame position on the source time line
            const int64_t i0 = static_cast<int64_t>(std::floor(t));
            const int64_t first = i0 - half_ + 1, last = i0 + half_;
            if (!fill(last + 1, err)) return -1;
            if (eof_ && out_n_ >= static_cast<int64_t>(std::ceil(double(total_) / step - 1e-9))) break;
            // weights for taps first..last
            const double frac = t - double(i0);
            for (int k = 0; k < 2 * half_; ++k) {
                const double x = frac + double(half_ - 1 - k);      // t − j for j = first + k
                const double pos = (x + half_) * kPhases;
                const auto qi = static_cast<std::size_t>(pos);
                const float f = static_cast<float>(pos - double(qi));
                weights_[static_cast<std::size_t>(k)] = qi + 1 < table_.size() ? table_[qi] + f * (table_[qi + 1] - table_[qi]) : 0.0f;
            }
            for (int c = 0; c < ch; ++c) {
                double acc = 0.0;
                for (int k = 0; k < 2 * half_; ++k) {
                    const int64_t j = first + k;
                    if (j < base_ || j >= base_ + static_cast<int64_t>(buf_.size() / static_cast<std::size_t>(ch))) continue; // before start / past end: zeros
                    acc += double(weights_[static_cast<std::size_t>(k)]) * buf_[static_cast<std::size_t>((j - base_) * ch + c)];
                }
                dst[made * ch + c] = static_cast<float>(acc);
            }
            ++made;
            ++out_n_;
            // drop history no tap can reach any more
            const int64_t keep_from = static_cast<int64_t>(std::floor(double(out_n_) * step)) - half_;
            if (keep_from - base_ > 65536) {
                const auto drop = static_cast<std::size_t>(keep_from - base_);
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * static_cast<std::size_t>(ch)));
                base_ += static_cast<int64_t>(drop);
            }
        }
        return made;
    }

private:
    static constexpr int64_t kChunk = 8192;
    static constexpr int kPhases = 1024;
    bool pass_ = true;
    bool eof_ = false;
    double fc_ = 0.5;
    int half_ = 0;
    std::vector<float> table_, weights_, tmp_;
    std::vector<float> buf_;  // decoded source, `ch` interleaved, starting at absolute frame base_
    int64_t base_ = 0, total_ = 0, out_n_ = 0;

    // Decodes up to `frames` frames, keeping the first `ch` channels.
    int64_t decodeInto(float* dst, int64_t frames, std::string* err) {
        int64_t done = 0;
        while (done < frames) {
            const ma_uint64 want = static_cast<ma_uint64>(std::min<int64_t>(kChunk, frames - done));
            ma_uint64 got = 0;
            const ma_result r = ma_decoder_read_pcm_frames(&d.dec, tmp_.data(), want, &got);
            if (r != MA_SUCCESS && r != MA_AT_END) {
                if (err) *err = std::string("Decoding failed (") + ma_result_description(r) + ")";
                return -1;
            }
            for (ma_uint64 i = 0; i < got; ++i)
                for (int c = 0; c < ch; ++c) dst[(done + static_cast<int64_t>(i)) * ch + c] = tmp_[static_cast<std::size_t>(i) * static_cast<std::size_t>(src_ch) + static_cast<std::size_t>(c)];
            done += static_cast<int64_t>(got);
            if (got < want) break;
        }
        return done;
    }

    // Makes source frames up to (absolute) `upto` available, or reaches the end of the file.
    bool fill(int64_t upto, std::string* err) {
        while (!eof_ && base_ + static_cast<int64_t>(buf_.size() / static_cast<std::size_t>(ch)) < upto) {
            const std::size_t old = buf_.size();
            buf_.resize(old + static_cast<std::size_t>(kChunk * ch));
            const int64_t got = decodeInto(buf_.data() + old, kChunk, err);
            if (got < 0) return false;
            buf_.resize(old + static_cast<std::size_t>(got * ch));
            total_ += got;
            if (got < kChunk) eof_ = true;
        }
        return true;
    }
};

} // namespace

bool probeAudioFile(const std::string& path, AudioFileInfo& info, std::string* err) {
    MaDecoder d;
    if (!openMa(path, d, err)) return false;
    ma_format f;
    ma_uint32 ch = 0, sr = 0;
    ma_uint64 len = 0;
    if (ma_decoder_get_data_format(&d.dec, &f, &ch, &sr, nullptr, 0) != MA_SUCCESS || sr == 0 ||
        ma_decoder_get_length_in_pcm_frames(&d.dec, &len) != MA_SUCCESS || len == 0) {
        if (err) *err = "This file has no audio in it";
        return false;
    }
    info.sample_rate = sr;
    info.channels = static_cast<int>(ch);
    info.seconds = double(len) / sr;
    AudioFileInfo tags;
    const std::string fmt = sniff(path, &tags);
    info.title = tags.title;
    info.artist = tags.artist;
    info.format = fmt.empty() ? "Audio" : fmt;
    return true;
}

std::unique_ptr<AudioFileReader> openAudioFile(const std::string& path, double target_rate, std::string* err) {
    auto r = std::make_unique<MaReader>();
    if (!openMa(path, r->d, err)) return nullptr;
    ma_format f;
    ma_uint32 ch = 0, sr = 0;
    if (ma_decoder_get_data_format(&r->d.dec, &f, &ch, &sr, nullptr, 0) != MA_SUCCESS || sr == 0 || ch == 0) {
        if (err) *err = "This file has no audio in it";
        return nullptr;
    }
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&r->d.dec, &len) != MA_SUCCESS) len = 0;
    r->src_rate = sr;
    r->src_ch = static_cast<int>(ch);
    r->ch = static_cast<int>(std::clamp<ma_uint32>(ch, 1, 2));
    r->src_frames = static_cast<int64_t>(len);
    r->dst_rate = target_rate > 0 ? target_rate : sr;
    r->setup();
    return r;
}

#endif

} // namespace rig::platform
