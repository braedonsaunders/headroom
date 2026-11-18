#pragma once
// Cabinet / mic-room / reverb convolution.
//
//  * PartitionedConvolver — zero-latency two-stage non-uniform partitioned convolution:
//      - Direct-form head of P1 = 128 taps (vDSP_dotpr),
//      - Stage 1 early reflections: up to 7 uniform partitions of P1 = 128 taps ([128, 1024)),
//      - Stage 2 late tail: partitions of P2 = 1024 taps ([1024, N)) via 2048-point vDSP FFT.
//    Cuts long-IR frequency-domain MAC operations by 6x–8x while preserving zero latency,
//    < -100 dB exactness against direct convolution, and click-free retarget() handoffs.
//  * CabinetIRLibrary     — provides deterministic physically modeled speaker cabinet/mic/room
//    and plate/spring/hall impulse responses (or loads user .wav files when present) and caches them
//    at the host sample rate.
//  * CabinetIR            — an immutable stereo IR (already partitioned + transformed) that the audio
//    thread swaps in atomically when the cabinet/mic selection changes.
#include "../preset/PresetLoader.hpp"
#include "../audio/WavIO.hpp"
#include "CabinetIds.hpp"
#include "../platform/Paths.hpp"
#include "../platform/VDsp.hpp" // Accelerate on macOS, portable vDSP subset elsewhere
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

namespace rig::dsp {

inline constexpr int kConvPart = 128;          // Stage 1 partition size P1 (FFT size 2*P1 = 256)
inline constexpr int kConvLog2N = 8;           // log2(2*P1)
inline constexpr int kStage1MaxParts = 7;      // Stage 1 covers taps [128, 1024) (7 * 128 = 896 taps)
inline constexpr int kTailPart = 1024;         // Stage 2 partition size P2 = 8 * P1 (FFT size 2*P2 = 2048)
inline constexpr int kTailLog2N = 11;          // log2(2*P2)
inline constexpr std::size_t kMaxIrTaps = 16384;

// Reference transforms on the platform vDSP (Accelerate on macOS) for tests (tests/test_dsp_quality.cpp's uniform
// reference convolver). The engine's convolver does not use them (see below); single-threaded use only.
inline FFTSetup sharedConvFFT() {
    static FFTSetup setup = vDSP_create_fftsetup(kConvLog2N, kFFTRadix2);
    return setup;
}
inline FFTSetup sharedTailConvFFT() {
    static FFTSetup setup = vDSP_create_fftsetup(kTailLog2N, kFFTRadix2);
    return setup;
}

// The convolver's arithmetic runs on rig::vdsp, the portable vDSP subset (platform/VDsp.hpp), on every platform,
// Apple's included. Accelerate's vDSP_fft_zrip is not deterministic with respect to buffer ADDRESS: from 512 points
// up it takes a different code path unless both split-complex buffers start on a 64-byte boundary (any other
// 16-byte-aligned address gives slightly different results; verified 64..16384 points, and 0 differences in 1600
// concurrent runs with the buffers at fixed addresses). The old convolver passed stack arrays, whose alignment moved
// with argv/environment size and with which thread ran it, so a cabinet's output changed by about -127 dB from
// sample 1024 on (first read as a concurrency effect). Two cabinets in parallel lanes, or a cabinet and a reverb in
// pipelined stages, must render the same samples as on one core. The portable code is plain C++ (same packing and
// scale as vDSP's real FFT), so its result does not depend on address, core or concurrent work. Any remaining
// Accelerate FFT call must use rig::vdsp::kFftAlign / FftVector buffers (platform/VDsp.hpp). Its cost is small at these sizes
// (a 256-point transform per 128 samples, a 2048-point one per 1024).
// One setup per user: the setups' twiddle tables are created here, on the control thread; the copy assignment keeps
// this object's (no allocation on the audio thread when render state is adopted).
class ConvFFT {
public:
    explicit ConvFFT(int log2n) : log2n_(log2n), s_(vdsp::vDSP_create_fftsetup(static_cast<vdsp::vDSP_Length>(log2n), vdsp::kFFTRadix2)) {}
    ConvFFT(const ConvFFT& o) : ConvFFT(o.log2n_) {}
    ConvFFT(ConvFFT&& o) noexcept : log2n_(o.log2n_), s_(o.s_) { o.s_ = nullptr; }
    ConvFFT& operator=(const ConvFFT&) noexcept { return *this; } // same size: keep this one
    ConvFFT& operator=(ConvFFT&& o) noexcept {
        if (this != &o) std::swap(s_, o.s_), std::swap(log2n_, o.log2n_);
        return *this;
    }
    ~ConvFFT() {
        if (s_) vdsp::vDSP_destroy_fftsetup(s_);
    }
    void forward(vdsp::DSPSplitComplex* io) const noexcept { vdsp::vDSP_fft_zrip(s_, io, 1, static_cast<vdsp::vDSP_Length>(log2n_), vdsp::kFFTDirection_Forward); }
    void inverse(vdsp::DSPSplitComplex* io) const noexcept { vdsp::vDSP_fft_zrip(s_, io, 1, static_cast<vdsp::vDSP_Length>(log2n_), vdsp::kFFTDirection_Inverse); }

private:
    int log2n_;
    vdsp::FFTSetup s_;
};

// One channel of a two-stage non-uniform partitioned, pre-transformed impulse response.
struct PartitionedIR {
    std::array<float, kConvPart> head{};             // taps [0, P1) for the direct-form head
    std::array<float, kConvPart> head_rev{};         // same, time-reversed for a contiguous dot product
    std::vector<std::array<float, kConvPart>> re;    // Stage 1 spectra: partitions 1..min(7, J-1) covering [128, 1024)
    std::vector<std::array<float, kConvPart>> im;
    int parts = 0;                                    // number of Stage 1 partitions (0..7)
    std::vector<std::array<float, kTailPart>> tail_re; // Stage 2 spectra: 1024-tap partitions covering [1024, N)
    std::vector<std::array<float, kTailPart>> tail_im;
    int tail_parts = 0;                               // number of Stage 2 partitions

    // The same impulse, bit for bit (a cabinet's two sides after IRMixer::finish(stereo = false)). Control thread.
    bool sameAs(const PartitionedIR& o) const noexcept {
        auto same = [](const auto& x, const auto& y) {
            return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), sizeof(x[0]) * x.size()) == 0);
        };
        return parts == o.parts && tail_parts == o.tail_parts && std::memcmp(head.data(), o.head.data(), sizeof(head)) == 0 &&
               same(re, o.re) && same(im, o.im) && same(tail_re, o.tail_re) && same(tail_im, o.tail_im);
    }

    void build(const std::vector<float>& ir, std::size_t max_taps = kMaxIrTaps) {
        ConvFFT conv_fft(kConvLog2N), tail_fft(kTailLog2N); // control thread
        head.fill(0.0f);
        const std::size_t n = std::min(ir.size(), max_taps);
        for (std::size_t i = 0; i < std::min<std::size_t>(n, kConvPart); ++i) head[i] = ir[i];
        for (std::size_t i = 0; i < static_cast<std::size_t>(kConvPart); ++i) head_rev[i] = head[static_cast<std::size_t>(kConvPart) - 1 - i];

        // Stage 1: up to kStage1MaxParts (7) partitions of size kConvPart (128), covering taps [128, 1024).
        const std::size_t stage1_end = std::min<std::size_t>(n, static_cast<std::size_t>(kTailPart));
        parts = (stage1_end > static_cast<std::size_t>(kConvPart))
                    ? static_cast<int>((stage1_end - kConvPart + kConvPart - 1) / kConvPart)
                    : 0;
        re.assign(static_cast<std::size_t>(parts), {});
        im.assign(static_cast<std::size_t>(parts), {});
        {
            std::array<float, 2 * kConvPart> buf{};
            std::array<float, kConvPart> r{}, m{};
            vdsp::DSPSplitComplex sc{r.data(), m.data()};
            const float scale = 1.0f / (4.0f * static_cast<float>(2 * kConvPart));
            for (int j = 0; j < parts; ++j) {
                buf.fill(0.0f);
                const std::size_t off = static_cast<std::size_t>(kConvPart) * static_cast<std::size_t>(j + 1);
                for (std::size_t k = 0; k < static_cast<std::size_t>(kConvPart) && off + k < stage1_end; ++k) buf[k] = ir[off + k];
                vdsp::vDSP_ctoz(reinterpret_cast<const vdsp::DSPComplex*>(buf.data()), 2, &sc, 1, kConvPart);
                conv_fft.forward(&sc);
                for (int k = 0; k < kConvPart; ++k) {
                    re[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] = r[static_cast<std::size_t>(k)] * scale;
                    im[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] = m[static_cast<std::size_t>(k)] * scale;
                }
            }
        }

        // Stage 2: partitions of size kTailPart (1024), covering taps [1024, n).
        tail_parts = (n > static_cast<std::size_t>(kTailPart))
                         ? static_cast<int>((n - kTailPart + kTailPart - 1) / kTailPart)
                         : 0;
        tail_re.assign(static_cast<std::size_t>(tail_parts), {});
        tail_im.assign(static_cast<std::size_t>(tail_parts), {});
        if (tail_parts > 0) {
            std::array<float, 2 * kTailPart> buf{};
            std::array<float, kTailPart> r{}, m{};
            vdsp::DSPSplitComplex sc{r.data(), m.data()};
            const float scale = 1.0f / (4.0f * static_cast<float>(2 * kTailPart));
            for (int j = 0; j < tail_parts; ++j) {
                buf.fill(0.0f);
                const std::size_t off = static_cast<std::size_t>(kTailPart) * static_cast<std::size_t>(j + 1);
                for (std::size_t k = 0; k < static_cast<std::size_t>(kTailPart) && off + k < n; ++k) buf[k] = ir[off + k];
                vdsp::vDSP_ctoz(reinterpret_cast<const vdsp::DSPComplex*>(buf.data()), 2, &sc, 1, kTailPart);
                tail_fft.forward(&sc);
                for (int k = 0; k < kTailPart; ++k) {
                    tail_re[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] = r[static_cast<std::size_t>(k)] * scale;
                    tail_im[static_cast<std::size_t>(j)][static_cast<std::size_t>(k)] = m[static_cast<std::size_t>(k)] * scale;
                }
            }
        }
    }
};

struct CabinetIR {
    PartitionedIR left, right;
    std::vector<float> preview; // decimated magnitude response for the UI (optional)
};

// Bounded LRU cache of pre-partitioned stereo cabinet IRs keyed by (selection_key, sample_rate).
class CabinetIRCache {
public:
    static constexpr std::size_t kMaxEntries = 48;

    static std::shared_ptr<const CabinetIR> getOrBuild(
        const std::string& key, double sr,
        const std::function<std::shared_ptr<const CabinetIR>()>& builder) {
        const auto full_key = std::make_pair(key, static_cast<int>(sr));
        {
            std::lock_guard<std::mutex> lk(mtx());
            auto& m = map();
            auto& l = lru();
            if (auto it = m.find(full_key); it != m.end()) {
                l.splice(l.begin(), l, it->second.second);
                return it->second.first;
            }
        }
        auto built = builder();
        if (!built) return nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx());
            auto& m = map();
            auto& l = lru();
            if (auto it = m.find(full_key); it != m.end()) {
                l.splice(l.begin(), l, it->second.second);
                return it->second.first;
            }
            l.push_front(full_key);
            m.emplace(full_key, std::make_pair(built, l.begin()));
            while (m.size() > kMaxEntries) {
                const auto& old_key = l.back();
                m.erase(old_key);
                l.pop_back();
            }
        }
        return built;
    }

private:
    using Key = std::pair<std::string, int>;
    using List = std::list<Key>;
    using Map = std::map<Key, std::pair<std::shared_ptr<const CabinetIR>, List::iterator>>;

    static std::mutex& mtx() { static std::mutex m; return m; }
    static Map& map() { static Map m; return m; }
    static List& lru() { static List l; return l; }
};

class PartitionedConvolver {
public:
    PartitionedConvolver(int max_parts = static_cast<int>(kMaxIrTaps / kConvPart)) { setCapacity(max_parts); }

    void setCapacity(int max_parts) {
        max_parts_ = std::max(1, max_parts);
        stage1_cap_ = std::min(max_parts_, kStage1MaxParts);
        fdl_re_.assign(static_cast<std::size_t>(stage1_cap_), {});
        fdl_im_.assign(static_cast<std::size_t>(stage1_cap_), {});
        const int total_taps = (max_parts_ + 1) * kConvPart;
        stage2_cap_ = (total_taps > kTailPart)
                          ? std::max(1, (total_taps - kTailPart + kTailPart - 1) / kTailPart)
                          : 1;
        tail_fdl_re_.assign(static_cast<std::size_t>(stage2_cap_), {});
        tail_fdl_im_.assign(static_cast<std::size_t>(stage2_cap_), {});
        reset();
    }

    void reset() noexcept {
        hist_.fill(0.0f);
        hpos_ = 0;
        cur_.fill(0.0f);
        prev_.fill(0.0f);
        tail_.fill(0.0f);
        for (auto& a : fdl_re_) a.fill(0.0f);
        for (auto& a : fdl_im_) a.fill(0.0f);
        cur_tail_.fill(0.0f);
        prev_tail_.fill(0.0f);
        tail2_out_.fill(0.0f);
        for (auto& a : tail_fdl_re_) a.fill(0.0f);
        for (auto& a : tail_fdl_im_) a.fill(0.0f);
        pos_ = 0;
        fdl_pos_ = 0;
        tail_pos_ = 0;
        tail_fdl_pos_ = 0;
    }

    float process(float x, const PartitionedIR& ir) noexcept {
        hist_[hpos_] = x;
        hist_[hpos_ + kConvPart] = x;
        float y = 0.0f;
        vdsp::vDSP_dotpr(ir.head_rev.data(), 1, hist_.data() + hpos_ + 1, 1, &y, kConvPart);
        hpos_ = (hpos_ + 1) % kConvPart;
        y += tail_[static_cast<std::size_t>(pos_)] + tail2_out_[static_cast<std::size_t>(tail_pos_)];
        cur_[static_cast<std::size_t>(pos_)] = x;
        cur_tail_[static_cast<std::size_t>(tail_pos_)] = x;
        if (++pos_ == kConvPart) {
            pos_ = 0;
            blockComplete(ir);
        }
        if (++tail_pos_ == kTailPart) {
            tail_pos_ = 0;
            tailBlockComplete(ir);
        }
        return y;
    }

    void retarget(const PartitionedIR& ir) noexcept {
        computeTail(ir);
        computeStage2Tail(ir);
    }
    int capacity() const noexcept { return max_parts_; }

private:
    void blockComplete(const PartitionedIR& ir) noexcept {
        std::array<float, 2 * kConvPart> buf{};
        std::copy(prev_.begin(), prev_.end(), buf.begin());
        std::copy(cur_.begin(), cur_.end(), buf.begin() + kConvPart);
        fdl_pos_ = (fdl_pos_ + stage1_cap_ - 1) % stage1_cap_;
        vdsp::DSPSplitComplex sc{fdl_re_[static_cast<std::size_t>(fdl_pos_)].data(), fdl_im_[static_cast<std::size_t>(fdl_pos_)].data()};
        vdsp::vDSP_ctoz(reinterpret_cast<const vdsp::DSPComplex*>(buf.data()), 2, &sc, 1, kConvPart);
        fft_.forward(&sc);
        prev_ = cur_;
        computeTail(ir);
    }

    void computeTail(const PartitionedIR& ir) noexcept {
        const int parts = std::min(ir.parts, stage1_cap_);
        if (parts == 0) {
            tail_.fill(0.0f);
            return;
        }
        std::array<float, kConvPart> ar{}, ai{};
        vdsp::DSPSplitComplex acc{ar.data(), ai.data()};
        float dc = 0.0f, nyq = 0.0f;
        for (int j = 0; j < parts; ++j) {
            auto& xr = fdl_re_[static_cast<std::size_t>((fdl_pos_ + j) % stage1_cap_)];
            auto& xi = fdl_im_[static_cast<std::size_t>((fdl_pos_ + j) % stage1_cap_)];
            const auto& hr = ir.re[static_cast<std::size_t>(j)];
            const auto& hi = ir.im[static_cast<std::size_t>(j)];
            dc += xr[0] * hr[0];
            nyq += xi[0] * hi[0];
            vdsp::DSPSplitComplex X{xr.data(), xi.data()};
            vdsp::DSPSplitComplex H{const_cast<float*>(hr.data()), const_cast<float*>(hi.data())};
            vdsp::vDSP_zvma(&X, 1, &H, 1, &acc, 1, &acc, 1, kConvPart);
        }
        ar[0] = dc;
        ai[0] = nyq;
        fft_.inverse(&acc);
        std::array<float, 2 * kConvPart> out{};
        vdsp::vDSP_ztoc(&acc, 1, reinterpret_cast<vdsp::DSPComplex*>(out.data()), 2, kConvPart);
        std::copy(out.begin() + kConvPart, out.end(), tail_.begin());
    }

    void tailBlockComplete(const PartitionedIR& ir) noexcept {
        std::array<float, 2 * kTailPart> buf{};
        std::copy(prev_tail_.begin(), prev_tail_.end(), buf.begin());
        std::copy(cur_tail_.begin(), cur_tail_.end(), buf.begin() + kTailPart);
        tail_fdl_pos_ = (tail_fdl_pos_ + stage2_cap_ - 1) % stage2_cap_;
        vdsp::DSPSplitComplex sc{tail_fdl_re_[static_cast<std::size_t>(tail_fdl_pos_)].data(),
                           tail_fdl_im_[static_cast<std::size_t>(tail_fdl_pos_)].data()};
        vdsp::vDSP_ctoz(reinterpret_cast<const vdsp::DSPComplex*>(buf.data()), 2, &sc, 1, kTailPart);
        tail_fft_.forward(&sc);
        prev_tail_ = cur_tail_;
        computeStage2Tail(ir);
    }

    void computeStage2Tail(const PartitionedIR& ir) noexcept {
        const int tparts = std::min(ir.tail_parts, stage2_cap_);
        if (tparts == 0) {
            tail2_out_.fill(0.0f);
            return;
        }
        std::array<float, kTailPart> ar{}, ai{};
        vdsp::DSPSplitComplex acc{ar.data(), ai.data()};
        float dc = 0.0f, nyq = 0.0f;
        for (int j = 0; j < tparts; ++j) {
            auto& xr = tail_fdl_re_[static_cast<std::size_t>((tail_fdl_pos_ + j) % stage2_cap_)];
            auto& xi = tail_fdl_im_[static_cast<std::size_t>((tail_fdl_pos_ + j) % stage2_cap_)];
            const auto& hr = ir.tail_re[static_cast<std::size_t>(j)];
            const auto& hi = ir.tail_im[static_cast<std::size_t>(j)];
            dc += xr[0] * hr[0];
            nyq += xi[0] * hi[0];
            vdsp::DSPSplitComplex X{xr.data(), xi.data()};
            vdsp::DSPSplitComplex H{const_cast<float*>(hr.data()), const_cast<float*>(hi.data())};
            vdsp::vDSP_zvma(&X, 1, &H, 1, &acc, 1, &acc, 1, kTailPart);
        }
        ar[0] = dc;
        ai[0] = nyq;
        tail_fft_.inverse(&acc);
        std::array<float, 2 * kTailPart> out{};
        vdsp::vDSP_ztoc(&acc, 1, reinterpret_cast<vdsp::DSPComplex*>(out.data()), 2, kTailPart);
        std::copy(out.begin() + kTailPart, out.end(), tail2_out_.begin());
    }

    ConvFFT fft_{kConvLog2N}, tail_fft_{kTailLog2N};
    std::array<float, 2 * kConvPart + 1> hist_{};
    std::size_t hpos_ = 0;
    std::array<float, kConvPart> cur_{}, prev_{}, tail_{};
    std::vector<std::array<float, kConvPart>> fdl_re_, fdl_im_;
    std::array<float, kTailPart> cur_tail_{}, prev_tail_{}, tail2_out_{};
    std::vector<std::array<float, kTailPart>> tail_fdl_re_, tail_fdl_im_;
    int max_parts_ = 1;
    int stage1_cap_ = 1;
    int stage2_cap_ = 1;
    int pos_ = 0;
    int fdl_pos_ = 0;
    int tail_pos_ = 0;
    int tail_fdl_pos_ = 0;
};

// ───────────────────────────── IR library ─────────────────────────────
class CabinetIRLibrary {
public:
    // Built-in cabinets: cab_ids::kMatched (26 amp-matched cabinets, 156000 order) and cab_ids::kStudio (29 studio
    // cabinets; 119000 index 0 = DI box, 1 = no cabinet, then these).
    static const char* matchedId(int idx) { return cab_ids::kMatched[static_cast<std::size_t>(std::clamp(idx, 0, 25))].id; }
    static const char* studioId(int idx) { return cab_ids::kStudio[static_cast<std::size_t>(std::clamp(idx, 0, 28))].id; }

    static constexpr std::array<const char*, 5> kMics = {{"dyn57", "dyn421", "con87", "rib121", "rib160"}};
    static constexpr std::array<const char*, 3> kPositions = {{"cap", "capedge", "cone"}};

    static std::string contentRoot() {
        static std::string cached;
        static std::once_flag once;
        std::call_once(once, [] {
            namespace fs = std::filesystem;
            if (const char* env = std::getenv("HEADROOM_IR_ROOT")) {
                std::error_code ec;
                if (fs::exists(env, ec)) {
                    cached = env;
                    return;
                }
            }
            const std::string d = platform::appDataDir() + "/IRs";
            std::error_code ec;
            fs::create_directories(d, ec);
            cached = d;
        });
        return cached;
    }

    // Returns a cached, host-rate IR: reads user .wav file if present on disk, otherwise synthesizes
    // a physically modeled speaker cabinet + microphone + position + room (or acoustic reverb) impulse response.
    static std::shared_ptr<const std::vector<float>> load(const std::string& rel_path, double sr, int channel = 0,
                                                          std::size_t max_taps = kMaxIrTaps) {
        static std::mutex mtx;
        static std::map<std::pair<std::string, int>, std::shared_ptr<const std::vector<float>>> cache;
        const auto key = std::make_pair(rel_path + "#" + std::to_string(channel) + "#" + std::to_string(max_taps), static_cast<int>(sr));
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (auto it = cache.find(key); it != cache.end()) return it->second;
        }
        std::shared_ptr<const std::vector<float>> result;
        // User .wav files; else synthesized.
        const std::string root = contentRoot();
        if (!result && !root.empty() && rel_path.size() > 4 && rel_path.substr(rel_path.size() - 4) == ".wav") {
            rig::audio::WavData w;
            std::string wav_path = root + "/" + rel_path;
            if (rel_path.rfind("user:", 0) == 0) {
                wav_path = root + "/" + rel_path.substr(5);
            }
            if (!rig::audio::readWavFile(wav_path, w) && rel_path.rfind("Cabinet IR Loader/", 0) == 0) {
                // Also check directly inside root for user IRs imported into ~/Library/Application Support/Headroom/IRs
                rig::audio::readWavFile(root + "/" + rel_path.substr(18), w);
            }
            if (!w.left.empty()) {
                const auto& ch_vec = (channel == 1 && !w.right.empty()) ? w.right : w.left;
                result = std::make_shared<const std::vector<float>>(resampleMono(ch_vec, w.sample_rate, sr, max_taps));
            }
        }
        if (!result) {
            result = std::make_shared<const std::vector<float>>(synthesizeImpulse(rel_path, sr, channel, max_taps));
        }
        std::lock_guard<std::mutex> lk(mtx);
        cache[key] = result;
        return result;
    }

    // Relative paths of every file under a sub-folder (or built-in acoustic spaces / cabinet captures).
    static const std::vector<std::string>& listFiles(const std::string& sub, const std::string& ext) {
        static std::mutex mtx;
        static std::map<std::string, std::vector<std::string>> cache;
        std::lock_guard<std::mutex> lk(mtx);
        auto& v = cache[sub + "|" + ext];
        if (v.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string base = contentRoot() + "/" + sub;
            for (const auto& it : fs::recursive_directory_iterator(base, ec)) {
                if (it.is_regular_file() && it.path().extension() == ext) v.push_back(fs::relative(it.path(), base, ec).string());
            }
            if (sub == "Cabinet IR Loader") {
                const std::vector<std::string> builtin_captures = {
                        "British Studio/MARSH 1960A G12M-25 SM57.wav",
                        "British Studio/MARSH 1960A G12M-25 R121.wav",
                        "British Studio/MARSH 1960B G12T-75 SM57.wav",
                        "British Studio/MARSH 1960TV Greenback MD421.wav",
                        "British Studio/VOX AC30 2x12 Alnico Blue SM57.wav",
                        "British Studio/VOX AC30 2x12 Alnico Blue R121.wav",
                        "British Studio/ORANGE PPC412 V30 SM57.wav",
                        "British Studio/ORANGE PPC412 V30 R121.wav",
                        "British Studio/HIWATT SE4123 Fane Crescendo SM57.wav",
                        "British Studio/HIWATT SE4123 Fane Crescendo U87.wav",
                        "American Vintage/FENDER Twin 2x12 Jensen C12K SM57.wav",
                        "American Vintage/FENDER Twin 2x12 Jensen C12K R121.wav",
                        "American Vintage/FENDER Deluxe 1x12 Oxford 12K5 SM57.wav",
                        "American Vintage/FENDER Bassman 4x10 Jensen P10R SM57.wav",
                        "American Vintage/FENDER Tweed 1x12 Jensen P12Q R121.wav",
                        "American Vintage/SUPRO 1x12 DT12 U67.wav",
                        "Modern High-Gain/MESA Recto 4x12 V30 SM57.wav",
                        "Modern High-Gain/MESA Recto 4x12 V30 R121.wav",
                        "Modern High-Gain/MESA Oversized 4x12 V30 MD421.wav",
                        "Modern High-Gain/PEAVEY 5150 4x12 Sheffield SM57.wav",
                        "Modern High-Gain/SOLDANO 4x12 Legend V12 SM57.wav",
                        "Modern High-Gain/BOGNER Uberkab 4x12 V30+T75 SM57.wav",
                        "Modern High-Gain/FRIEDMAN 4x12 Greenback+V30 SM57.wav",
                        "Modern High-Gain/DIEZEL 4x12 V30 Rear-Loaded SM57.wav",
                        "Bass Cabinets/AMPEG SVT 8x10 Heritage D112.wav",
                        "Bass Cabinets/AMPEG SVT 8x10 Heritage U47 FET.wav",
                        "Bass Cabinets/AMPEG B-15N 1x15 Portaflex RE20.wav",
                        "Bass Cabinets/HARTKE 410XL Aluminum Cone D6.wav",
                        "Bass Cabinets/AGUILAR DB 410 Classic MD421.wav",
                        "Boutique & Custom/MATCHLESS 2x12 G12H30+Blue SM57.wav",
                        "Boutique & Custom/TWO-ROCK 2x12 EV12L R121.wav",
                    };
                for (const auto& b : builtin_captures) {
                    if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
                }
                // Also include any user .wav IRs placed directly in contentRoot()
                for (const auto& it : fs::recursive_directory_iterator(contentRoot(), ec)) {
                    if (it.is_regular_file() && (it.path().extension() == ".wav" || it.path().extension() == ".WAV")) {
                        const std::string rel = fs::relative(it.path(), contentRoot(), ec).string();
                        if (!rel.empty() && rel.rfind("Cabinet IR Loader/", 0) != 0 &&
                            std::find(v.begin(), v.end(), rel) == v.end()) {
                            v.push_back(rel);
                        }
                    }
                }
            } else if (v.empty()) {
                if (sub == "Studio IR Reverb") {
                    v = {
                        "01 Concert Halls/2.4s Vienna Symphony Hall.ir",
                        "01 Concert Halls/3.1s Berlin Philharmonic Hall.ir",
                        "02 Studio Rooms/0.6s Wood Tracking Room A.ir",
                        "02 Studio Rooms/0.9s Live Drum Stone Room.ir",
                        "03 Vintage Plates/0.8s Short Vocal Plate.ir",
                        "03 Vintage Plates/1.8s EMT 140 Gold Foil Plate.ir",
                        "03 Vintage Plates/2.6s EMT 240 Dark Plate.ir",
                        "04 Spring Tanks/1.2s Accutronics 3-Spring Tank.ir",
                        "04 Spring Tanks/1.7s Vintage Surf Dual-Spring.ir",
                        "05 Cathedrals & Spaces/4.2s Gothic Stone Cathedral.ir",
                    };
                }
            }
            std::sort(v.begin(), v.end());
        }
        return v;
    }

    static std::string prettyIRName(const std::string& rel, bool strip_index = false) {
        std::string s = rel;
        if (auto dot = s.rfind('.'); dot != std::string::npos) s = s.substr(0, dot);
        if (auto sl = s.find('/'); sl != std::string::npos) {
            std::string folder = s.substr(0, sl);
            if (strip_index && folder.size() > 3 && std::isdigit(static_cast<unsigned char>(folder[0]))) folder = folder.substr(folder.find(' ') + 1);
            s = folder + " · " + s.substr(sl + 1);
        }
        return s;
    }

    // ───── studio (Studio Cab Room, 119000) cabinet definitions ─────
    struct RoomCabInfo {
        std::vector<std::string> mics;                              // mic tokens, ribbon -> condenser -> dynamic
        std::map<std::string, std::vector<std::string>> positions; // per mic: cap, capedge, cone, room
        bool room = true;                                           // room impulse available
        bool standard = false;                                      // exactly con87/dyn421/dyn57/rib121/rib160
    };

    static int micRank(const std::string& tok) {
        if (tok.size() < 4) return 9999;
        const std::string fam = tok.substr(0, 3);
        const int f = fam == "rib" ? 0 : fam == "con" ? 1 : fam == "dyn" ? 2 : 3;
        std::size_t i = 3;
        int num = 0;
        while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) num = num * 10 + (tok[i++] - '0');
        int variant = 0;
        const std::string rest = tok.substr(i);
        if (rest == "+") variant = 1;
        else if (rest.size() == 2 && (rest[0] == 'V' || rest[0] == 'v')) variant = 1 + (rest[1] - '0');
        return f * 100000 + num * 10 + variant;
    }
    static int positionRank(const std::string& p) { return p == "cap" ? 0 : p == "capedge" ? 1 : p == "cone" ? 2 : p == "room" ? 3 : 4; }

    static const RoomCabInfo& roomCabInfo(const std::string& folder) {
        static std::mutex mtx;
        static std::map<std::string, RoomCabInfo> cache;
        std::lock_guard<std::mutex> lk(mtx);
        if (auto it = cache.find(folder); it != cache.end()) return it->second;

        RoomCabInfo info;
        info.room = true;

        if (folder == "marshall-1960tv-4x12" || folder == "bogner-412st-4x12") { // 13-mic 4x12s
            const std::vector<std::string> m13 = {
                "rib121V1", "rib121V2", "rib160V1", "rib160V2",
                "con47V1", "con47V2", "con67", "con87V1", "con87V2",
                "dyn57V1", "dyn57V2", "dyn421V1", "dyn421V2"
            };
            for (const auto& m : m13) info.positions[m] = {"capedge"};
        } else if (folder == "marshall-1936-2x12" || folder == "mesa-lonestar-2x12") { // 8-mic 2x12s
            const std::vector<std::string> m8 = {
                "rib121V1", "rib121V2", "rib160", "con47", "con87", "dyn57V1", "dyn57V2", "dyn421"
            };
            for (const auto& m : m8) info.positions[m] = {"capedge"};
        } else if (folder == "mesa-oversized-4x12-a" || folder == "mesa-oversized-4x12-b") { // 6-mic oversized 4x12s
            const std::vector<std::string> m6 = {"rib121", "con47", "con87", "dyn57", "dyn421", "dyn441"};
            for (const auto& m : m6) info.positions[m] = {"cap", "capedge", "cone"};
        } else if (folder == "hartke-4x10-aluminum" || folder == "ampeg-svt-8x10" || folder == "ampeg-b15-1x15") { // bass cabinets
            info.positions["rib121"] = {"cap", "capedge", "cone"};
            info.positions["con30"] = {"cap", "capedge", "room"};
            info.positions["con47"] = {"cap", "capedge", "cone"};
            info.positions["con87"] = {"cap", "capedge", "cone"};
            info.positions["dyn6"] = {"cap", "capedge", "cone"};
            info.positions["dyn20"] = {"cap", "capedge", "cone"};
            info.positions["dyn112"] = {"cap", "capedge", "cone"};
            info.positions["dyn421"] = {"cap", "capedge", "cone"};
        } else {
            for (const char* m : kMics) {
                info.positions[m] = {"cap", "capedge", "cone"};
            }
        }

        for (auto& [mic, v] : info.positions) {
            info.mics.push_back(mic);
            std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return positionRank(a) < positionRank(b); });
        }
        std::sort(info.mics.begin(), info.mics.end(), [](const std::string& a, const std::string& b) { return micRank(a) < micRank(b); });
        if (info.mics.size() == kMics.size()) {
            info.standard = true;
            for (const char* m : kMics) info.standard &= info.positions.count(m) > 0;
        }
        return cache.emplace(folder, std::move(info)).first->second;
    }

    static std::string roomCabFile(const std::string& folder, const std::string& mic, const std::string& pos) {
        return "studio/" + folder + "/" + folder + "_" + mic + "_" + pos + ".ir";
    }

    static std::string roomCabMic(int cab, int mic, int pos) {
        const std::string name = studioId(cab);
        const RoomCabInfo& info = roomCabInfo(name);
        std::string tok = kMics[static_cast<std::size_t>(std::clamp(mic, 0, 4))];
        if (!info.standard && !info.mics.empty())
            tok = info.mics[static_cast<std::size_t>(std::clamp(mic, 0, static_cast<int>(info.mics.size()) - 1))];
        std::vector<std::string> ps;
        if (auto it = info.positions.find(tok); it != info.positions.end())
            for (const auto& p : it->second) if (p != "room") ps.push_back(p);
        const std::string p = ps.empty() ? std::string(kPositions[static_cast<std::size_t>(std::clamp(pos, 0, 2))])
                                         : ps[static_cast<std::size_t>(std::clamp(pos, 0, static_cast<int>(ps.size()) - 1))];
        return roomCabFile(name, tok, p);
    }
    static std::string roomCabRoom(int cab) {
        const std::string name = studioId(cab);
        return "studio/" + name + "/" + name + "_room.ir";
    }
    static std::string matched(int idx, char ab, bool room) {
        return std::string("matched/") + matchedId(idx) + (room ? "_room_" : "_") + ab + ".ir";
    }
    // One of the plate & spring reverb recordings ("Gold Plate Short", "Amp Spring", ...).
    static std::string plateSpring(std::string_view name) { return "plate-spring/" + std::string(name) + ".ir"; }

private:
    static uint32_t hashStr(std::string_view s) noexcept {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return h ? h : 1u;
    }

    // Simple 2-pole biquad helper for offline impulse shaping
    struct OfflineBiquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        static OfflineBiquad highPass(double sr, double fc, double q = 0.7071) {
            OfflineBiquad f;
            const double w0 = 2.0 * std::numbers::pi * std::clamp(fc, 15.0, sr * 0.45) / sr;
            const double sn = std::sin(w0), cs = std::cos(w0), alpha = sn / (2.0 * q);
            const double a0 = 1.0 + alpha;
            f.b0 = ((1.0 + cs) * 0.5) / a0;
            f.b1 = -(1.0 + cs) / a0;
            f.b2 = ((1.0 + cs) * 0.5) / a0;
            f.a1 = (-2.0 * cs) / a0;
            f.a2 = (1.0 - alpha) / a0;
            return f;
        }
        static OfflineBiquad lowPass(double sr, double fc, double q = 0.7071) {
            OfflineBiquad f;
            const double w0 = 2.0 * std::numbers::pi * std::clamp(fc, 40.0, sr * 0.47) / sr;
            const double sn = std::sin(w0), cs = std::cos(w0), alpha = sn / (2.0 * q);
            const double a0 = 1.0 + alpha;
            f.b0 = ((1.0 - cs) * 0.5) / a0;
            f.b1 = (1.0 - cs) / a0;
            f.b2 = ((1.0 - cs) * 0.5) / a0;
            f.a1 = (-2.0 * cs) / a0;
            f.a2 = (1.0 - alpha) / a0;
            return f;
        }
        static OfflineBiquad peak(double sr, double fc, double q, double db) {
            OfflineBiquad f;
            const double A = std::pow(10.0, db / 40.0);
            const double w0 = 2.0 * std::numbers::pi * std::clamp(fc, 20.0, sr * 0.46) / sr;
            const double sn = std::sin(w0), cs = std::cos(w0), alpha = sn / (2.0 * q);
            const double a0 = 1.0 + alpha / A;
            f.b0 = (1.0 + alpha * A) / a0;
            f.b1 = (-2.0 * cs) / a0;
            f.b2 = (1.0 - alpha * A) / a0;
            f.a1 = (-2.0 * cs) / a0;
            f.a2 = (1.0 - alpha / A) / a0;
            return f;
        }
        void apply(std::vector<float>& buf) {
            for (float& x : buf) {
                const double in = x;
                const double y = b0 * in + z1;
                z1 = b1 * in - a1 * y + z2;
                z2 = b2 * in - a2 * y;
                x = static_cast<float>(y);
            }
        }
    };

    // The built-in cabinet an IR path belongs to ("matched/<id>_…", "studio/<id>/…"), or nullptr.
    static const cab_ids::CabDef* builtinCabFor(std::string_view rel) {
        if (rel.rfind("matched/", 0) == 0) {
            rel.remove_prefix(8);
            for (const auto& c : cab_ids::kMatched) {
                const std::string_view id = c.id;
                if (rel.substr(0, id.size()) == id && rel.size() > id.size() && rel[id.size()] == '_') return &c;
            }
        } else if (rel.rfind("studio/", 0) == 0) {
            rel.remove_prefix(7);
            const int i = cab_ids::studioIndex(rel.substr(0, rel.find('/')));
            if (i >= 0) return &cab_ids::kStudio[static_cast<std::size_t>(i)];
        }
        return nullptr;
    }

    // FNV-1a continued from `h` over `s` (hashStr's raw state).
    static uint32_t hashMore(uint32_t h, std::string_view s) noexcept {
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }
    // Noise seed of a synthesized IR: built-in cabinets and plate/spring recordings continue from their table seed over
    // the rest of the path (so renaming an id never changes the sound), anything else hashes its path.
    static uint32_t synthSeed(std::string_view rel) noexcept {
        uint32_t h = 0;
        if (const cab_ids::CabDef* def = builtinCabFor(rel)) {
            const std::string_view id = def->id;
            const std::size_t skip = rel.rfind("matched/", 0) == 0 ? 8 + id.size() : 7 + id.size() + 1 + id.size();
            h = hashMore(def->seed, rel.substr(skip));
        } else if (rel.rfind("plate-spring/", 0) == 0) {
            h = hashMore(cab_ids::kPlateSpringSeed, rel.substr(13));
        } else {
            return hashStr(rel);
        }
        return h ? h : 1u;
    }

    // Synthesizes a physically modeled speaker cabinet / mic / position / room or reverb impulse response.
    static std::vector<float> synthesizeImpulse(const std::string& rel_path, double sr, int channel, std::size_t max_taps) {
        sr = (sr > 1000.0) ? sr : 48000.0;
        const bool is_reverb = rel_path.find("Verb") != std::string::npos ||
                               rel_path.find("Plate") != std::string::npos ||
                               rel_path.find("Spring") != std::string::npos ||
                               rel_path.find("Hall") != std::string::npos ||
                               rel_path.find("Cathedral") != std::string::npos;

        if (is_reverb) {
            const bool is_spring = rel_path.find("Spring") != std::string::npos;
            const bool is_short = rel_path.find("Short") != std::string::npos || rel_path.find("0.") != std::string::npos;
            const double rt60 = is_spring ? 1.35 : (is_short ? 0.95 : 2.15);
            const std::size_t len = std::min<std::size_t>(max_taps, static_cast<std::size_t>(sr * std::min(3.0, rt60 * 1.3)));
            std::vector<float> ir(std::max<std::size_t>(512, len), 0.0f);
            uint32_t rng = synthSeed(rel_path) ^ (static_cast<uint32_t>(channel) * 0x9E3779B9u);

            // Early reflections + dispersive spring chirp or dense plate diffusion
            ir[0] = 0.35f;
            const double tau = (rt60 / 6.907755) * sr;
            for (std::size_t i = 1; i < ir.size(); ++i) {
                rng = rng * 1664525u + 1013904223u;
                const float wn = (static_cast<float>(rng >> 8) / 8388608.0f) - 1.0f;
                const double env = std::exp(-static_cast<double>(i) / tau);
                float s = static_cast<float>(wn * env * 0.18);
                if (is_spring) {
                    // Dispersive metallic spring bounce resonances every ~38 ms
                    const double t = static_cast<double>(i) / sr;
                    s += static_cast<float>(0.08 * env * std::sin(2.0 * std::numbers::pi * (180.0 + 950.0 * std::exp(-t * 6.0)) * t));
                }
                ir[i] = s;
            }
            OfflineBiquad::highPass(sr, is_spring ? 110.0 : 80.0, 0.707).apply(ir);
            OfflineBiquad::lowPass(sr, is_spring ? 5200.0 : 7800.0, 0.707).apply(ir);
            return ir;
        }

        // Speaker cabinet + microphone + position + room impulse response
        const bool is_room = rel_path.find("_room") != std::string::npos;
        const std::size_t len = std::min<std::size_t>(max_taps, static_cast<std::size_t>(sr * (is_room ? 0.16 : 0.065)));
        std::vector<float> ir(std::max<std::size_t>(256, len), 0.0f);
        const uint32_t h = synthSeed(rel_path);

        // Identify speaker format & character
        double f_res = 92.0 + static_cast<double>(h % 19) - 9.0;
        double f_lp = 5200.0;
        double cone_cry_hz = 2450.0 + static_cast<double>((h >> 5) % 450);
        using cab_ids::SynthVoice;
        SynthVoice voice = SynthVoice::Generic;
        if (const cab_ids::CabDef* def = builtinCabFor(rel_path)) voice = def->voice;
        else if (rel_path.find("8x10") != std::string::npos || rel_path.find("Bass") != std::string::npos) voice = SynthVoice::Bass;
        else if (rel_path.find("2x12") != std::string::npos || rel_path.find("AC") != std::string::npos) voice = SynthVoice::TwoByTwelve;
        else if (rel_path.find("1x12") != std::string::npos || rel_path.find("Tweed") != std::string::npos) voice = SynthVoice::OneByTwelve;
        if (voice == SynthVoice::Bass) {
            f_res = 62.0; f_lp = 4200.0; cone_cry_hz = 1650.0;
        } else if (voice == SynthVoice::TwoByTwelve) {
            f_res = 104.0; f_lp = 5800.0; cone_cry_hz = 2750.0;
        } else if (voice == SynthVoice::OneByTwelve) {
            f_res = 112.0; f_lp = 5400.0; cone_cry_hz = 2300.0;
        }

        // Identify mic & position
        const bool is_cap = rel_path.find("_cap.") != std::string::npos;
        const bool is_cone = rel_path.find("_cone.") != std::string::npos;
        const bool is_capedge = rel_path.find("_capedge.") != std::string::npos;
        const bool is_matched_b = rel_path.find("_B.") != std::string::npos;

        const bool is_rib121 = rel_path.find("rib121") != std::string::npos || is_matched_b;
        const bool is_rib160 = rel_path.find("rib160") != std::string::npos;
        const bool is_con87  = rel_path.find("con87") != std::string::npos;
        const bool is_con47  = rel_path.find("con47") != std::string::npos || rel_path.find("con67") != std::string::npos;
        const bool is_dyn421 = rel_path.find("dyn421") != std::string::npos;

        // Position alters high-frequency roll-off and presence peak:
        // cap (on-axis) is brightest, capedge is balanced, cone (off-axis) is darkest.
        double pos_lp_mult = 1.0;
        double pos_pres_db = 3.0;
        if (is_cap) {
            pos_lp_mult = 1.32;
            pos_pres_db = 5.5;
        } else if (is_capedge) {
            pos_lp_mult = 1.02;
            pos_pres_db = 2.8;
        } else if (is_cone) {
            pos_lp_mult = 0.68;
            pos_pres_db = -1.5;
        }

        // Minimum-phase impulse seed + cabinet internal wood reflections + speaker cone breakup modes
        const std::size_t pre_delay = is_room ? static_cast<std::size_t>(sr * 0.0045) : 0;
        if (pre_delay < ir.size()) ir[pre_delay] = 1.0f;

        uint32_t rng = h ^ 0xA5A5A5A5u;
        const double cab_tau = sr * (is_room ? 0.032 : 0.0042);
        for (std::size_t i = pre_delay + 1; i < ir.size(); ++i) {
            const double t = static_cast<double>(i - pre_delay) / sr;
            const double env = std::exp(-static_cast<double>(i - pre_delay) / cab_tau);
            rng = rng * 1664525u + 1013904223u;
            const double wn = (static_cast<double>(rng >> 8) / 8388608.0) - 1.0;
            // Modal speaker cone resonances + cabinet box reflections
            const double modes = 0.14 * std::sin(2.0 * std::numbers::pi * f_res * t) * std::exp(-t * 45.0)
                               + 0.09 * std::sin(2.0 * std::numbers::pi * (cone_cry_hz * 0.55) * t) * std::exp(-t * 140.0)
                               + 0.06 * std::sin(2.0 * std::numbers::pi * cone_cry_hz * t) * std::exp(-t * 210.0);
            ir[i] = static_cast<float>(modes + (is_room ? 0.16 : 0.035) * wn * env);
        }

        // Filter cascade: speaker bass resonance, mid dip, cone cry peak, and steep high-frequency rolloff
        OfflineBiquad::highPass(sr, f_res * 0.85, 1.35).apply(ir);
        OfflineBiquad::peak(sr, f_res * 1.18, 1.8, is_rib121 ? 6.5 : 3.2).apply(ir);
        OfflineBiquad::peak(sr, 420.0, 1.2, -2.8).apply(ir);
        OfflineBiquad::peak(sr, cone_cry_hz, 2.1, pos_pres_db).apply(ir);

        // Microphone frequency response shaping
        if (is_rib121) {
            // Royer R-121 figure-8 ribbon: warm proximity low-end boost, smooth dark top
            OfflineBiquad::peak(sr, 130.0, 0.9, 5.2).apply(ir);
            OfflineBiquad::lowPass(sr, 3900.0 * pos_lp_mult, 0.75).apply(ir);
        } else if (is_rib160) {
            // Beyerdynamic M 160 hypercardioid ribbon: focused mids, natural top
            OfflineBiquad::peak(sr, 160.0, 1.0, 2.2).apply(ir);
            OfflineBiquad::lowPass(sr, 4600.0 * pos_lp_mult, 0.80).apply(ir);
        } else if (is_con87) {
            // Neumann U87 Ai condenser: tight low end, airy upper-presence peak
            OfflineBiquad::highPass(sr, 115.0, 0.85).apply(ir);
            OfflineBiquad::peak(sr, 140.0, 1.0, -2.5).apply(ir);
            OfflineBiquad::peak(sr, 4800.0, 1.6, 3.8).apply(ir);
            OfflineBiquad::lowPass(sr, 6400.0 * pos_lp_mult, 0.78).apply(ir);
        } else if (is_con47) {
            // Neumann U47 FET condenser: full low-mids, authoritative presence
            OfflineBiquad::peak(sr, 180.0, 1.1, 2.8).apply(ir);
            OfflineBiquad::peak(sr, 3600.0, 1.4, 2.5).apply(ir);
            OfflineBiquad::lowPass(sr, 5600.0 * pos_lp_mult, 0.75).apply(ir);
        } else if (is_dyn421) {
            // Sennheiser MD 421-II: scooped low-mid, punchy upper-mid attack
            OfflineBiquad::peak(sr, 115.0, 1.2, 2.4).apply(ir);
            OfflineBiquad::peak(sr, 3400.0, 1.8, 3.6).apply(ir);
            OfflineBiquad::lowPass(sr, 5400.0 * pos_lp_mult, 0.82).apply(ir);
        } else {
            // Shure SM57 cardioid dynamic: tight lows, classic 3.1 kHz presence bark
            OfflineBiquad::highPass(sr, 95.0, 0.8).apply(ir);
            OfflineBiquad::peak(sr, 3150.0, 1.7, 4.2).apply(ir);
            OfflineBiquad::lowPass(sr, f_lp * pos_lp_mult, 0.85).apply(ir);
        }
        // Final speaker cone roll-off
        OfflineBiquad::lowPass(sr, (f_lp + 900.0) * pos_lp_mult, 0.707).apply(ir);

        // Normalize energy so every close-mic IR has consistent L2 norm (~0.32)
        double e = 0.0;
        for (float v : ir) e += static_cast<double>(v) * v;
        if (e > 1e-12) {
            const float target = is_room ? 0.22f : 0.32f;
            const float g = static_cast<float>(target / std::sqrt(e));
            for (float& v : ir) v *= g;
        }
        return ir;
    }

    static std::vector<float> resampleMono(const std::vector<float>& mono, uint32_t src_sr, double sr, std::size_t max_taps) {
        const std::size_t frames = std::min(mono.size(), static_cast<std::size_t>(static_cast<double>(max_taps) * src_sr / sr) + 64);
        if (std::abs(static_cast<double>(src_sr) - sr) < 1.0) {
            std::vector<float> out(mono.begin(), mono.begin() + static_cast<long>( std::min(frames, max_taps)));
            return out;
        }
        const double ratio = static_cast<double>(src_sr) / sr;
        const std::size_t out_len = std::min<std::size_t>(max_taps, static_cast<std::size_t>(static_cast<double>(frames) / ratio));
        std::vector<float> out(out_len, 0.0f);
        const double cutoff = std::min(1.0, 1.0 / ratio);
        constexpr int kHalf = 16;
        for (std::size_t i = 0; i < out_len; ++i) {
            const double pos = static_cast<double>(i) * ratio;
            const auto c = static_cast<long>(std::floor(pos));
            double acc = 0.0;
            for (long k = c - kHalf + 1; k <= c + kHalf; ++k) {
                if (k < 0 || k >= static_cast<long>(frames)) continue;
                const double t = pos - static_cast<double>(k);
                const double x = t * cutoff;
                const double sinc = (std::abs(x) < 1e-9) ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * t / kHalf);
                acc += mono[static_cast<std::size_t>(k)] * sinc * w * cutoff;
            }
            out[i] = static_cast<float>(acc);
        }
        return out;
    }
};

// Amp-Matched Cab (156000) Volume: -12..+12 dB around the level it has always had at 0.5 (gain 1.15), so
// the control can stage a rig instead of trimming it (the old 0.7 + 0.9 * MV law spanned only -3.1..+4.1 dB).
// dB = 6.8 u + 68.8 u^3 (u = MV - 0.5): the same slope as the old law through the middle, so settings in the
// commonly used 0.3-0.7 move by under 0.6 dB, while the ends now reach +/-12 dB. Same span as the Cabinet
// unit's Level (dB-linear), so conversions between the two stay exact.
inline float ampMatchedCabVolumeDb(float mv) noexcept {
    const float u = std::clamp(mv, 0.0f, 1.0f) - 0.5f;
    return 6.8f * u + 68.8f * u * u * u;
}
inline float ampMatchedCabVolume(float mv) noexcept {
    return 1.15f * std::pow(10.0f, ampMatchedCabVolumeDb(mv) / 20.0f);
}

// Accumulates weighted, delayed, panned impulses into a stereo IR.
class IRMixer {
public:
    explicit IRMixer(double sr) : sr_(sr) {}

    void add(const std::vector<float>& ir, float gain, float pan01, float delay_ms) {
        const float a = std::clamp(pan01, 0.0f, 1.0f) * std::numbers::pi_v<float> * 0.5f;
        const float gl = std::cos(a) * std::numbers::sqrt2_v<float> * gain;
        const float gr = std::sin(a) * std::numbers::sqrt2_v<float> * gain;
        const auto d = static_cast<std::size_t>(std::max(0.0f, delay_ms) * 0.001f * static_cast<float>(sr_));
        const std::size_t n = std::min(kMaxIrTaps, ir.size() + d);
        if (l_.size() < n) {
            l_.resize(n, 0.0f);
            r_.resize(n, 0.0f);
        }
        for (std::size_t i = 0; i + d < n; ++i) {
            l_[i + d] += ir[i] * gl;
            r_[i + d] += ir[i] * gr;
        }
        any_ = true;
    }
    void addImpulse(float gain, float pan01) { add(std::vector<float>{1.0f}, gain, pan01, 0.0f); }

    std::shared_ptr<CabinetIR> finish(bool stereo, float volume) {
        auto out = std::make_shared<CabinetIR>();
        if (!any_ || l_.empty()) {
            l_ = {1.0f};
            r_ = {1.0f};
        }
        if (!stereo) {
            for (std::size_t i = 0; i < l_.size(); ++i) {
                const float m = 0.5f * (l_[i] + r_[i]);
                l_[i] = m;
                r_[i] = m;
            }
        }
        const float ref = refGain(sr_);
        const float g = ref * volume;
        for (auto& v : l_) v *= g;
        for (auto& v : r_) v *= g;
        out->left.build(l_);
        out->right.build(r_);
        return out;
    }

    std::shared_ptr<CabinetIR> finishRaw(bool stereo, float volume) {
        auto out = std::make_shared<CabinetIR>();
        if (!any_ || l_.empty()) {
            l_ = {1.0f};
            r_ = {1.0f};
        }
        if (!stereo) {
            for (std::size_t i = 0; i < l_.size(); ++i) {
                const float m = 0.5f * (l_[i] + r_[i]);
                l_[i] = m;
                r_[i] = m;
            }
        }
        double e = 0.0;
        for (std::size_t i = 0; i < l_.size(); ++i) e += 0.5 * (static_cast<double>(l_[i]) * l_[i] + static_cast<double>(r_[i]) * r_[i]);
        const float g = (e > 1e-9) ? static_cast<float>(0.32 / std::sqrt(e)) * volume : volume;
        for (auto& v : l_) v *= g;
        for (auto& v : r_) v *= g;
        out->left.build(l_);
        out->right.build(r_);
        return out;
    }

private:
    static float refGain(double sr) {
        static std::mutex mtx;
        static std::map<int, float> cache;
        std::lock_guard<std::mutex> lk(mtx);
        const int key = static_cast<int>(sr);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        auto ir = CabinetIRLibrary::load(CabinetIRLibrary::matched(10, 'A', false), sr, 0);
        double e = 0.0;
        if (ir) {
            for (float v : *ir) e += static_cast<double>(v) * v;
        }
        const float g = (e > 1e-9) ? static_cast<float>(0.32 / std::sqrt(e)) : 1.0f;
        cache[key] = g;
        return g;
    }

    double sr_ = 48000.0;
    std::vector<float> l_, r_;
    bool any_ = false;
};

} // namespace rig::dsp
