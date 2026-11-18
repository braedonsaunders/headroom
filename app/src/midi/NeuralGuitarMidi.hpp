#pragma once
// Streaming causal neural guitar → MIDI (Headroom). Reproduces tools/midi-bench/nn (features.py + model.py)
// frame by frame: 24 kHz input, one network step per 128-sample hop (5.33 ms), no look-ahead.
// Weights: .hgnn file from export_weights.py. All buffers are allocated in load(); process() never allocates.
#include "../platform/VDsp.hpp" // Accelerate on macOS, portable vDSP subset elsewhere
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "json.hpp"  // nlohmann (NeuralAmpModelerCore dependency include path)

namespace rig::midi {

class NeuralGuitarMidi {
public:
    struct Note { int pitch; bool on; float velocity; int frame; };

    struct Sparse { std::vector<int> start, idx; std::vector<float> w; };
    // Immutable model (weights, header, sparse filterbanks, transposed GRU weights): loaded once per file and
    // shared by every instance, so rebuilding a unit on a knob edit never re-reads or re-parses the weights.
    struct Shared {
        nlohmann::json H;
        std::map<std::string, std::vector<float>> T;
        Sparse fbl, fbs;
        std::vector<float> wihT, whhT;
    };
    static std::shared_ptr<const Shared> sharedFor(const std::string& path, std::string* err = nullptr) {
        static std::mutex m;
        static std::map<std::string, std::weak_ptr<const Shared>> cache;
        std::lock_guard<std::mutex> lk(m);
        if (auto sp = cache[path].lock()) return sp;
        auto sh = std::make_shared<Shared>();
        std::ifstream f(path, std::ios::binary);
        char magic[4]; uint32_t ver = 0, jl = 0;
        if (!f.read(magic, 4) || std::memcmp(magic, "HGNN", 4) || !f.read((char*)&ver, 4) || !f.read((char*)&jl, 4)) { if (err) *err = "bad header"; return nullptr; }
        std::string js(jl, '\0'); f.read(js.data(), jl);
        sh->H = nlohmann::json::parse(js, nullptr, false);
        if (sh->H.is_discarded()) { if (err) *err = "bad json"; return nullptr; }
        for (auto& t : sh->H["tensors"]) {
            size_t n = 1; for (auto d : t["shape"]) n *= (size_t)d.get<int>();
            std::vector<float> v(n); f.read((char*)v.data(), n * 4);
            sh->T[t["name"].get<std::string>()] = std::move(v);
        }
        if (!f) { if (err) *err = "truncated weights"; return nullptr; }
        const int nb = sh->H["n_bins"], nl = sh->H["n_long"], ns = sh->H["n_short"];
        const int pl = sh->H["pad"][std::to_string(nl)], ps = sh->H["pad"][std::to_string(ns)];
        sh->fbl = sparsify(sh->T.at("fb.long"), nb, pl / 2 + 1);
        sh->fbs = sparsify(sh->T.at("fb.short"), nb, ps / 2 + 1);
        const int G = sh->H["gru"];
        if (G > 0) {
            const auto& wi = sh->T.at("gru.w_ih"); const auto& wh = sh->T.at("gru.w_hh");
            const int C = (int)(wi.size() / (3 * (size_t)G));
            sh->wihT.assign((size_t)C * 3 * G, 0.0f); sh->whhT.assign((size_t)G * 3 * G, 0.0f);
            for (int r = 0; r < 3 * G; ++r) { for (int c = 0; c < C; ++c) sh->wihT[(size_t)c * 3 * G + r] = wi[(size_t)r * C + c];
                                              for (int c = 0; c < G; ++c) sh->whhT[(size_t)c * 3 * G + r] = wh[(size_t)r * G + c]; }
        }
        cache[path] = sh;
        return sh;
    }

    bool load(const std::string& path, std::string* err = nullptr) {
        S_ = sharedFor(path, err);
        if (!S_) return false;
        const auto& H = S_->H;
        nb_ = H["n_bins"]; np_ = H["n_pitch"]; minp_ = H["min_pitch"]; hop_ = H["hop"];
        nl_ = H["n_long"]; ns_ = H["n_short"]; padl_ = H["pad"][std::to_string(nl_)]; pads_ = H["pad"][std::to_string(ns_)];
        offs_ = H["harm_offsets"].get<std::vector<int>>();
        gru_ = H["gru"]; pdim_ = H["pdim"];
        th_on_ = th_base_ = H["decoder"]["th_on"]; th_off_ = H["decoder"]["th_off"]; refr_ = H["decoder"]["refractory"];
        legato_ = H["decoder"].contains("legato") ? H["decoder"]["legato"].get<float>() : 0.65f;
        legato_run_ = H["decoder"].contains("legato_run") ? H["decoder"]["legato_run"].get<int>() : 2;
        const auto& D = H["decoder"];  // ghost / blip suppression: code defaults, header may override
        confirm_ = D.value("confirm", kConfirm); confirm_hold_ = D.value("confirm_hold", kConfirmHold);
        ghost_confirm_ = D.value("ghost_confirm", 0.0f);
        ghost_attack_ = D.value("ghost_attack", kGhostAttack); ghost_creep_ = D.value("ghost_creep", kGhostCreep);
        agc_d_ = H.contains("agc") ? H["agc"]["decay"].get<float>() : 0.0f;
        agc_floor_ = H.contains("agc") ? H["agc"]["floor"].get<float>() : -5.0f;
        layers_.clear();
        buildFrontEnd();
        buildLayers();
        reset();
        return true;
    }
    bool loaded() const noexcept { return S_ != nullptr; }
    void setThreshold(float th_on) noexcept { th_on_ = th_on; }
    void setLegato(float l) noexcept { legato_ = l; }  // <= 0 disables the legato rule
    void setLegatoRun(int n) noexcept { legato_run_ = std::max(1, n); }
    // Onset confirmation: a fresh onset whose sustain prob is still below `conf` is held up to `hold` frames for the
    // sustain head to confirm it, else dropped (<= 0 disables).
    void setConfirm(float conf, int hold) noexcept { confirm_ = conf; confirm_hold_ = std::clamp(hold, 1, 8); }
    // Ghost suppression under a sounding octave/fifth parent (see decode): `confirm` = stricter onset confirmation
    // (0 = same as the general one), `attack`/`creep` gate legato triggers (attack <= 0 and creep >= 1 disable).
    void setGhost(float confirm, float attack, float creep) noexcept { ghost_confirm_ = confirm; ghost_attack_ = attack; ghost_creep_ = creep; }
    // Feed one frame of (onset, sustain) probabilities [n_pitch each] straight into the decoder (tests, decoder
    // experiments on dumped probabilities). Same state and frame counter as process().
    void decodeFrame(const float* on, const float* fr, std::vector<Note>& out) {
        std::copy(on, on + np_, pon_.begin()); std::copy(fr, fr + np_, pfr_.begin());
        decode(out); ++frame_;
    }
    int minPitch() const noexcept { return minp_; }
    int numPitches() const noexcept { return np_; }
    // Sensitivity 0..1 around the model's tuned operating point (0.5 = the threshold found best on the benchmark).
    void setSensitivity(float s) noexcept { th_on_ = std::clamp(th_base_ + (0.5f - s) * 0.4f, 0.15f, 0.95f); }

    // Feed 24 kHz mono samples; emits note events (frame-accurate) into `out`.
    void process(const float* x, int n, std::vector<Note>& out) {
        for (int i = 0; i < n; ++i) {
            ring_[rpos_] = x[i]; rpos_ = (rpos_ + 1) % nl_;
            // frame t ends at (and includes) sample t*hop — same convention as features.py
            if (hopc_ == 0) { step(out); ++frame_; }
            if (++hopc_ == hop_) hopc_ = 0;
        }
    }
    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f); rpos_ = 0; hopc_ = 0; frame_ = 0;
        for (auto& L : layers_) for (auto& h : L.hist) std::fill(h.begin(), h.end(), 0.0f);
        std::fill(h_.begin(), h_.end(), 0.0f);
        agc_l_ = -1e9f;
        std::fill(active_.begin(), active_.end(), 0); std::fill(armed_.begin(), armed_.end(), 1);
        std::fill(last_.begin(), last_.end(), -999); std::fill(low_.begin(), low_.end(), 0); std::fill(run_.begin(), run_.end(), 0);
        std::fill(pend_.begin(), pend_.end(), 0); std::fill(pend_o_.begin(), pend_o_.end(), 0.0f); std::fill(pend_c_.begin(), pend_c_.end(), 0.0f);
        std::fill(prev_on_.begin(), prev_on_.end(), 0.0f); std::fill(frh_.begin(), frh_.end(), 0.0f);
    }
    const std::vector<float>& onsetProb() const { return pon_; }
    const std::vector<float>& frameProb() const { return pfr_; }
    int sampleRate() const { return S_ ? S_->H["sr"].get<int>() : 24000; }

private:
    static bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }
    const std::vector<float>& W(const std::string& n) const { return S_->T.at(n); }

    // ---------------- front-end ----------------
    static Sparse sparsify(const std::vector<float>& dense, int rows, int cols) {
        Sparse s; s.start.push_back(0);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) { float v = dense[(size_t)r * cols + c]; if (v != 0.0f) { s.idx.push_back(c); s.w.push_back(v); } }
            s.start.push_back((int)s.idx.size());
        }
        return s;
    }
    void buildFrontEnd() {
        ring_.assign(nl_, 0.0f);
        lg_l_ = (int)std::lround(std::log2((double)padl_)); lg_s_ = (int)std::lround(std::log2((double)pads_));
        fft_.reset(vDSP_create_fftsetup(lg_l_, kFFTRadix2));
        buf_.assign(padl_, 0.0f); re_.assign(padl_ / 2, 0.0f); im_.assign(padl_ / 2, 0.0f); pw_.assign(padl_ / 2 + 1, 0.0f);
        feat_.assign(2 * (size_t)nb_, 0.0f);
    }
    // log10 CQ power of the last `n` samples, window w, zero-padded to `pad`, into dst[nb_]
    void spectrum(int n, int pad, int lg, const std::vector<float>& w, const Sparse& fb, float* dst) {
        std::fill(buf_.begin(), buf_.begin() + pad, 0.0f);
        for (int i = 0; i < n; ++i) buf_[i] = ring_[(rpos_ + nl_ - n + i) % nl_] * w[i];
        DSPSplitComplex sc{re_.data(), im_.data()};
        vDSP_ctoz((const DSPComplex*)buf_.data(), 2, &sc, 1, pad / 2);
        vDSP_fft_zrip(fft_.get(), &sc, 1, lg, kFFTDirection_Forward);
        // zrip output is 2x the DFT: |X|^2 = (re^2 + im^2) / 4; DC in re[0], Nyquist in im[0]
        pw_[0] = re_[0] * re_[0] / 4.0f; pw_[pad / 2] = im_[0] * im_[0] / 4.0f;
        for (int k = 1; k < pad / 2; ++k) pw_[k] = (re_[k] * re_[k] + im_[k] * im_[k]) / 4.0f;
        for (int b = 0; b < nb_; ++b) {
            double acc = 0; for (int j = fb.start[b]; j < fb.start[b + 1]; ++j) acc += fb.w[j] * pw_[fb.idx[j]];
            dst[b] = (float)std::log10(acc + 1e-7);
        }
    }

    // ---------------- network ----------------
    struct Conv { int cin, cout, kf, kt, dil, stride, pad_f, fin, fout; const float* w; const float* b; };
    struct Layer {
        std::string type, norm;
        Conv c1{}, c2{};                 // stem/pool use c1 only
        std::vector<Conv> mix;           // interval-mix convs
        const float* scale = nullptr; const float* shift = nullptr;
        std::vector<std::vector<float>> hist;  // ring of past conv inputs (kt-1)*dil+1 frames
        int hpos = 0;
    };
    Conv mkconv(const nlohmann::json& j, const std::string& p, int fin) {
        Conv c{j["cin"], j["cout"], j["kf"], j["kt"], j["dil_t"], j["stride_f"], j["pad_f"], fin, 0, W(p + ".w").data(), W(p + ".b").data()};
        c.fout = (fin + 2 * c.pad_f - c.kf) / c.stride + 1;
        return c;
    }
    void buildLayers() {
        int F = nb_, cin = 2 + (int)offs_.size();
        for (auto& j : S_->H["layers"]) {
            Layer L; L.type = j["type"];
            std::string nm = j.contains("name") ? j["name"].get<std::string>() : L.type;
            if (L.type == "stem" || L.type == "pool") {
                L.c1 = mkconv(j, L.type, F);
                L.hist.assign(std::max(1, (L.c1.kt - 1) * L.c1.dil + 1), std::vector<float>((size_t)L.c1.cin * F));
                F = L.c1.fout; cin = L.c1.cout;
            } else if (L.type == "block") {
                L.norm = j["norm"];
                if (L.norm == "affine") { L.scale = W(nm + ".n.scale").data(); L.shift = W(nm + ".n.shift").data(); }
                L.c1 = mkconv(j["c1"], nm + ".c1", F); L.c2 = mkconv(j["c2"], nm + ".c2", F);
                L.hist.assign((L.c1.kt - 1) * L.c1.dil + 1, std::vector<float>((size_t)cin * F));
            } else {  // intervalmix
                L.norm = j["norm"];
                if (L.norm == "affine") { L.scale = W(nm + ".n.scale").data(); L.shift = W(nm + ".n.shift").data(); }
                auto ivs = j["intervals"].get<std::vector<int>>();
                for (size_t k = 0; k < ivs.size(); ++k) {
                    Conv c{cin, cin, 3, 1, 1, 1, ivs[k], F, F, W(nm + ".k" + std::to_string(k) + ".w").data(), W(nm + ".k" + std::to_string(k) + ".b").data()};
                    L.mix.push_back(c);
                }
                L.hist.assign(1, std::vector<float>((size_t)cin * F));
            }
            layers_.push_back(std::move(L));
        }
        Fsem_ = F; C_ = cin;
        size_t mx = 0;
        for (auto& L : layers_) for (const Conv* c : {&L.c1, &L.c2}) if (c->cin) mx = std::max(mx, (size_t)c->cin * c->kf * c->kt * c->fout);
        for (auto& L : layers_) for (auto& c : L.mix) mx = std::max(mx, (size_t)c.cin * c.kf * c.fout);
        col_.assign(mx, 0.0f);
        size_t act = (size_t)std::max(C_, 64) * nb_ * 2;
        a_.assign(act, 0.0f); b_.assign(act, 0.0f); t_.assign(act, 0.0f); u_.assign(act, 0.0f);
        h_.assign((size_t)np_ * gru_, 0.0f); gx_.assign((size_t)np_ * 3 * gru_, 0.0f); gh_.assign((size_t)np_ * 3 * gru_, 0.0f);
        pon_.assign(np_, 0.0f); pfr_.assign(np_, 0.0f);
        active_.assign(np_, 0); armed_.assign(np_, 1); last_.assign(np_, -999); low_.assign(np_, 0); onset_frame_.assign(np_, 0); run_.assign(np_, 0);
        pend_.assign(np_, 0); pend_o_.assign(np_, 0.0f); pend_c_.assign(np_, 0.0f); prev_on_.assign(np_, 0.0f); frh_.assign((size_t)kFrHist * np_, 0.0f);
    }
    static inline float erf_fast(float x) {  // Abramowitz & Stegun 7.1.26, |error| < 1.5e-7
        const float s = x < 0.0f ? -1.0f : 1.0f; x = std::fabs(x);
        const float t = 1.0f / (1.0f + 0.3275911f * x);
        const float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t * std::exp(-x * x);
        return s * y;
    }
    static inline float gelu(float x) { return 0.5f * x * (1.0f + erf_fast(x * 0.70710678f)); }
    static void geluInPlace(float* v, size_t n) { for (size_t i = 0; i < n; ++i) v[i] = gelu(v[i]); }
    // y (M x N) = A (M x K) * B (K x N) + bias[M]; vDSP_mmul never allocates (cblas_sgemm can on this path)
    static void matmulBias(const float* A, const float* B, float* y, int M, int N, int K, const float* bias) {
        vDSP_mmul(A, 1, B, 1, y, 1, (vDSP_Length)M, (vDSP_Length)N, (vDSP_Length)K);
        if (bias) for (int m = 0; m < M; ++m) vDSP_vsadd(y + (size_t)m * N, 1, bias + m, y + (size_t)m * N, 1, (vDSP_Length)N);
    }

    // y (cout x fout) = conv over the ring `hist` (newest frame at hpos-1) — im2col + one SGEMM
    void runConv(const Conv& c, const std::vector<std::vector<float>>& hist, int hpos, float* y) {
        const int K = c.cin * c.kf * c.kt, H = (int)hist.size();
        float* col = col_.data();
        for (int ci = 0; ci < c.cin; ++ci)
            for (int kf = 0; kf < c.kf; ++kf)
                for (int kt = 0; kt < c.kt; ++kt) {
                    const int back = (c.kt - 1 - kt) * c.dil;                 // frames into the past
                    const float* src = hist[(hpos - 1 - back + 2 * H) % H].data() + (size_t)ci * c.fin;
                    float* row = col + (size_t)((ci * c.kf + kf) * c.kt + kt) * c.fout;
                    const int off = kf - c.pad_f;
                    if (c.stride == 1) {  // shifted contiguous copy, zero edges
                        const int lo = std::max(0, -off), hi = std::min(c.fout, c.fin - off);
                        if (lo > 0) std::memset(row, 0, lo * 4);
                        if (hi > lo) std::memcpy(row + lo, src + lo + off, (hi - lo) * 4);
                        if (hi < c.fout) std::memset(row + std::max(hi, lo), 0, (c.fout - std::max(hi, lo)) * 4);
                    } else {
                        for (int fo = 0; fo < c.fout; ++fo) { const int fi = fo * c.stride + off; row[fo] = (fi >= 0 && fi < c.fin) ? src[fi] : 0.0f; }
                    }
                }
        matmulBias(c.w, col, y, c.cout, c.fout, K, c.b);
    }
    void push(Layer& L, const float* x, size_t n) { std::memcpy(L.hist[L.hpos].data(), x, n * 4); L.hpos = (L.hpos + 1) % (int)L.hist.size(); }

    void step(std::vector<Note>& out) {
        // features: long + short spectra, then harmonic-shifted copies of the long one, normalised
        float* fl = feat_.data(); float* fs = feat_.data() + nb_;
        spectrum(nl_, padl_, lg_l_, W("win.long"), S_->fbl, fl);
        spectrum(ns_, pads_, lg_s_, W("win.short"), S_->fbs, fs);
        // causal input AGC (model.py agc_level): L = max(E, L_prev - decay), floored; E = log10 mean bin power
        float fillv = -7.0f;
        if (agc_d_ > 0.0f) {
            double acc = 0; for (int b = 0; b < nb_; ++b) acc += std::pow(10.0, (double)fl[b]);
            const float E = (float)std::log10(acc / nb_);
            agc_l_ = std::max(E, agc_l_ - agc_d_);
            const float L = std::max(agc_l_, agc_floor_);
            for (int b = 0; b < nb_; ++b) { fl[b] -= L; fs[b] -= L; }
            fillv = -7.0f - agc_floor_;
        }
        const float* mu = W("in.mu").data(); const float* sd = W("in.sd").data();
        const int cin0 = 2 + (int)offs_.size();
        float* x = a_.data();
        for (int b = 0; b < nb_; ++b) { x[b] = (fl[b] - mu[0]) / sd[0]; x[nb_ + b] = (fs[b] - mu[1]) / sd[1]; }
        for (size_t k = 0; k < offs_.size(); ++k)
            for (int b = 0; b < nb_; ++b) { int s = b + offs_[k]; float v = (s >= 0 && s < nb_) ? fl[s] : fillv; x[(2 + k) * nb_ + b] = (v - mu[2 + k]) / sd[2 + k]; }
        (void)cin0;
        int F = nb_;
        float* cur = a_.data(); float* nxt = b_.data();
        for (auto& L : layers_) {
            if (L.type == "stem" || L.type == "pool") {
                push(L, cur, (size_t)L.c1.cin * F);
                runConv(L.c1, L.hist, L.hpos, nxt);
                geluInPlace(nxt, (size_t)L.c1.cout * L.c1.fout);
                F = L.c1.fout; std::swap(cur, nxt);
            } else if (L.type == "block") {
                const int C = L.c1.cin; const size_t n = (size_t)C * F;
                float* nrm = t_.data();
                for (int c = 0; c < C; ++c) for (int f = 0; f < F; ++f)
                    nrm[(size_t)c * F + f] = L.scale ? cur[(size_t)c * F + f] * L.scale[c] + L.shift[c] : cur[(size_t)c * F + f];
                push(L, nrm, n);
                runConv(L.c1, L.hist, L.hpos, u_.data());
                geluInPlace(u_.data(), n);
                matmulBias(L.c2.w, u_.data(), nxt, C, F, C, L.c2.b);  // 1x1 conv
                vDSP_vadd(nxt, 1, cur, 1, nxt, 1, (vDSP_Length)n);
                std::swap(cur, nxt);
            } else {  // interval mix: x + mean_k gelu(conv_k(norm(x)))
                const int C = C_; const size_t n = (size_t)C * F;
                float* nrm = t_.data();
                for (int c = 0; c < C; ++c) for (int f = 0; f < F; ++f)
                    nrm[(size_t)c * F + f] = L.scale ? cur[(size_t)c * F + f] * L.scale[c] + L.shift[c] : cur[(size_t)c * F + f];
                push(L, nrm, n);
                std::memcpy(nxt, cur, n * 4);
                const float inv = 1.0f / (float)L.mix.size();
                for (auto& c : L.mix) {
                    // dilated (3,1) conv along frequency: taps at f - d, f, f + d
                    Conv cc = c; cc.kf = 3; cc.pad_f = c.pad_f;
                    float* y = u_.data();
                    const int K = C * 3;
                    float* col = col_.data();
                    const float* src = L.hist[(L.hpos - 1 + (int)L.hist.size()) % (int)L.hist.size()].data();
                    const int d = c.pad_f;  // dilation == padding
                    for (int ci = 0; ci < C; ++ci) for (int k = 0; k < 3; ++k) {
                        float* row = col + (size_t)(ci * 3 + k) * F;
                        const int off = (k - 1) * d, lo = std::max(0, -off), hi = std::min(F, F - off);
                        if (lo > 0) std::memset(row, 0, lo * 4);
                        if (hi > lo) std::memcpy(row + lo, src + (size_t)ci * F + lo + off, (hi - lo) * 4);
                        if (hi < F) std::memset(row + std::max(hi, lo), 0, (F - std::max(hi, lo)) * 4);
                    }
                    matmulBias(c.w, col, y, C, F, K, c.b);
                    geluInPlace(y, n);
                    vDSP_vsma(y, 1, &inv, nxt, 1, nxt, 1, (vDSP_Length)n);
                }
                std::swap(cur, nxt);
            }
        }
        // per-pitch-row GRU (rows 0..np-1 of the semitone grid) + linear head
        const int P = np_, C = C_, G = gru_;
        float* seq = t_.data();                      // (P x C): row-major per pitch
        for (int p = 0; p < P; ++p) for (int c = 0; c < C; ++c) seq[(size_t)p * C + c] = cur[(size_t)c * Fsem_ + p];
        const float* hw = W("head.w").data(); const float* hb = W("head.b").data();
        if (G > 0) {
            const float* wih = W("gru.w_ih").data(); const float* whh = W("gru.w_hh").data();
            const float* bih = W("gru.b_ih").data(); const float* bhh = W("gru.b_hh").data();
            (void)wih; (void)whh;
            matmulBias(seq, S_->wihT.data(), gx_.data(), P, 3 * G, C, nullptr);
            matmulBias(h_.data(), S_->whhT.data(), gh_.data(), P, 3 * G, G, nullptr);
            for (int p = 0; p < P; ++p) {
                float* gx = gx_.data() + (size_t)p * 3 * G; float* gh = gh_.data() + (size_t)p * 3 * G; float* h = h_.data() + (size_t)p * G;
                for (int k = 0; k < G; ++k) {
                    const float r = 1.0f / (1.0f + std::exp(-(gx[k] + bih[k] + gh[k] + bhh[k])));
                    const float z = 1.0f / (1.0f + std::exp(-(gx[G + k] + bih[G + k] + gh[G + k] + bhh[G + k])));
                    const float nn = std::tanh(gx[2 * G + k] + bih[2 * G + k] + r * (gh[2 * G + k] + bhh[2 * G + k]));
                    h[k] = (1.0f - z) * nn + z * h[k];
                }
            }
        }
        for (int p = 0; p < P; ++p) {
            float o = hb[0], fr = hb[1];
            const float* h = h_.data() + (size_t)p * G; const float* s = seq + (size_t)p * C;
            for (int k = 0; k < G; ++k) { o += hw[k] * h[k]; fr += hw[(G + C) + k] * h[k]; }
            for (int c = 0; c < C; ++c) { o += hw[G + c] * s[c]; fr += hw[(G + C) + G + c] * s[c]; }
            pon_[p] = 1.0f / (1.0f + std::exp(-o)); pfr_[p] = 1.0f / (1.0f + std::exp(-fr));
        }
        decode(out);
    }

    // Causal decoder: onset threshold crossing with re-arm + refractory; note-off after 2 frames of low sustain
    // probability or at a re-trigger. On top of train.py decode_fast:
    //  - Legato trigger: the sustain head alone starts a note (hammer-ons, and strings the onset head misses in
    //    dense strums).
    //  - Onset confirmation: most false positives are onset-head blips the sustain head never backs; an onset on a
    //    silent pitch whose sustain prob is < confirm_ is held up to confirm_hold_ frames and emitted as soon as the
    //    sustain prob reaches confirm_, else dropped. Most real notes already pass in the onset frame.
    //  - Octave/fifth ghosts: a sounding note at p-12/-19/-24/-7 (the "parent") puts its harmonics on p. A legato
    //    trigger under a parent needs a trace of attack on p (onset prob, this or last frame, >= ghost_attack_) and
    //    must not be a slow creep (sustain prob kGhostLag frames ago <= ghost_creep_); ghost_confirm_ optionally
    //    raises the onset confirmation under a parent. Parents sit below p, so this frame's are already decided.
    // Fixed cost per frame, no allocation, no added latency for confirmed notes.
    void decode(std::vector<Note>& out) {
        for (int p = 0; p < np_; ++p) {
            const float o = pon_[p], o_prev = prev_on_[p];
            prev_on_[p] = o;
            const float fr_lag = frh_[(size_t)((frame_ - kGhostLag) & (kFrHist - 1)) * np_ + p];
            frh_[(size_t)(frame_ & (kFrHist - 1)) * np_ + p] = pfr_[p];
            if (o < th_on_ * 0.6f) armed_[p] = 1;
            if (pend_[p] > 0) {  // held onset: start it once the sustain head confirms, drop it when the hold runs out
                if (pfr_[p] >= pend_c_[p]) { pend_[p] = 0; startNote(p, pend_o_[p], out); }
                else --pend_[p];
                continue;
            }
            const bool parent = hasGhostParent(p);
            bool trig = o >= th_on_ && armed_[p] && frame_ - last_[p] >= refr_, held = false;
            if (trig && !active_[p]) {
                const float conf = parent ? std::max(confirm_, ghost_confirm_) : confirm_;
                if (conf > 0.0f && pfr_[p] < conf) {
                    trig = false; held = true; armed_[p] = 0; last_[p] = frame_;
                    pend_[p] = confirm_hold_; pend_o_[p] = o; pend_c_[p] = conf;
                }
            }
            if (!trig && !held && legato_ > 0.0f && !active_[p] && frame_ - last_[p] >= refr_) {
                run_[p] = pfr_[p] >= legato_ ? run_[p] + 1 : 0;
                if (run_[p] >= legato_run_ && !(parent && (std::max(o, o_prev) < ghost_attack_ || fr_lag > ghost_creep_))) trig = true;
            } else if (active_[p]) {
                run_[p] = 0;
            }
            if (trig) startNote(p, o, out);
            else if (active_[p] && frame_ > onset_frame_[p]) {
                low_[p] = pfr_[p] < th_off_ ? low_[p] + 1 : 0;
                if (low_[p] >= 2) { out.push_back({minp_ + p, false, 0.0f, frame_}); active_[p] = 0; }
            }
        }
    }
    void startNote(int p, float o, std::vector<Note>& out) {
        run_[p] = 0;
        if (active_[p]) out.push_back({minp_ + p, false, 0.0f, frame_});
        out.push_back({minp_ + p, true, std::clamp(0.35f + 0.65f * o, 0.0f, 1.0f), frame_});
        active_[p] = 1; armed_[p] = 0; last_[p] = frame_; low_[p] = 0; onset_frame_[p] = frame_;
    }
    bool hasGhostParent(int p) const noexcept {
        for (int iv : {12, 19, 24, 7}) if (p - iv >= 0 && active_[p - iv]) return true;
        return false;
    }

    struct FftDel { void operator()(OpaqueFFTSetup* p) const noexcept { if (p) vDSP_destroy_fftsetup(p); } };
    std::shared_ptr<const Shared> S_;
    int nb_ = 0, np_ = 0, minp_ = 0, hop_ = 128, nl_ = 2048, ns_ = 512, padl_ = 8192, pads_ = 1024, lg_l_ = 13, lg_s_ = 10;
    int gru_ = 0, pdim_ = 0, Fsem_ = 0, C_ = 0, refr_ = 4;
    float th_on_ = 0.5f, th_base_ = 0.5f, th_off_ = 0.35f, legato_ = 0.65f;
    int legato_run_ = 2;
    // Onset confirmation + ghost suppression defaults (GuitarMix-tuned on a 30-file subset; RESULTS.md)
    static constexpr float kConfirm = 0.35f, kGhostAttack = 0.06f, kGhostCreep = 0.4f;
    static constexpr int kConfirmHold = 2, kGhostLag = 4, kFrHist = 8;  // kFrHist: power of two > kGhostLag
    float confirm_ = kConfirm, ghost_confirm_ = 0.0f, ghost_attack_ = kGhostAttack, ghost_creep_ = kGhostCreep;
    int confirm_hold_ = kConfirmHold;
    std::vector<int> offs_;
    std::unique_ptr<OpaqueFFTSetup, FftDel> fft_;
    std::vector<float> ring_, buf_, pw_, feat_, col_, a_, b_, t_, u_, h_, gx_, gh_, pon_, pfr_;
    vdsp::FftVector<float> re_, im_; // vDSP_fft_zrip's arrays: 64-byte aligned, else its result depends on the address
    std::vector<Layer> layers_;
    std::vector<int> active_, armed_, last_, low_, onset_frame_, run_, pend_;
    std::vector<float> pend_o_, pend_c_, prev_on_, frh_;  // held onset prob / its confirm level, last onset prob, sustain history
    int rpos_ = 0, hopc_ = 0, frame_ = 0;
    float agc_d_ = 0.0f, agc_floor_ = -5.0f, agc_l_ = -1e9f;
};

} // namespace rig::midi
