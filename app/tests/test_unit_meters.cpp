// Per-unit face meters (src/core/UnitMeters.hpp): levels, gain reduction, LFO phase, spectrum, pitch, waveform,
// arming (nothing runs unless the UI polls), bypass rest, and no allocation on the audio thread.
#undef NDEBUG
#include "../src/core/RackGraph.hpp"
#include "../src/core/UnitMeters.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace { thread_local bool inAudio = false; }
void* operator new(std::size_t size) {
    assert(!inAudio);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { assert(!inAudio); std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using rig::UnitMeterBank;

static double field(const std::string& j, const std::string& unit_prefix, const char* key) {
    const auto at = j.find(unit_prefix);
    assert(at != std::string::npos);
    const auto k = j.find(std::string("\"") + key + "\":", at);
    const auto end = j.find('}', at);
    if (k == std::string::npos || k > end) return -999.0;
    return std::atof(j.c_str() + k + std::strlen(key) + 3);
}

static std::vector<int> spectrumOf(const std::string& j) {
    std::vector<int> out;
    auto p = j.find("\"sp\":[");
    assert(p != std::string::npos);
    p += 6;
    while (j[p] != ']') {
        out.push_back(std::atoi(j.c_str() + p));
        while (j[p] != ',' && j[p] != ']') ++p;
        if (j[p] == ',') ++p;
    }
    return out;
}

int main() {
    constexpr double sr = 48000.0;
    constexpr uint32_t B = 256;
    // ---- bank alone ----
    {
        auto bank = std::make_shared<UnitMeterBank>();
        std::vector<float> in(B), out(B);
        rig::UnitLiveReadout ro;
        // Not armed: no metering.
        assert(!bank->beginBlock(B, sr));
        std::string j = bank->json({2}, {2}, {2}, {2}); // arms, assigns a ring to slot 2
        assert(j.find("\"idle\":1") != std::string::npos);
        double ph = 0.0;
        for (int blk = 0; blk < 200; ++blk) { // ~1 s of a 110 Hz sine at -6 dBFS out, -12 dBFS in
            for (uint32_t i = 0; i < B; ++i) {
                const float v = static_cast<float>(std::sin(ph));
                ph += 2.0 * std::numbers::pi * 110.0 / sr;
                in[i] = 0.25f * v;
                out[i] = 0.5f * v;
            }
            inAudio = true;
            const bool on = bank->beginBlock(B, sr);
            if (blk == 0) assert(on);
            if (on) {
                ro.gain_reduction_db = 3.5f;
                ro.lfo_phase = 0.25f;
                bank->record(2, in.data(), in.data(), out.data(), out.data(), B, ro);
            }
            inAudio = false;
            if (blk % 20 == 0) bank->json({2}, {2}, {2}, {2}); // keep polling (re-arms; the UI asks for the same analysis every poll)
        }
        j = bank->json({2, 5}, {2}, {2}, {2});
        std::printf("bank: %s\n", j.substr(0, 220).c_str());
        assert(std::abs(field(j, "{\"s\":2", "or") - (-9.0)) < 0.6);   // 0.5 sine → -9 dBFS RMS
        assert(std::abs(field(j, "{\"s\":2", "op") - (-6.0)) < 0.6);   // peak -6 dBFS
        assert(std::abs(field(j, "{\"s\":2", "ir") - (-15.0)) < 0.6);
        assert(std::abs(field(j, "{\"s\":2", "gr") - 3.5) < 0.05);
        assert(std::abs(field(j, "{\"s\":2", "lfo") - 0.25) < 0.01);
        assert(std::abs(field(j, "{\"s\":2", "hz") - 110.0) < 1.0);
        assert(j.find("{\"s\":5,\"idle\":1}") != std::string::npos);   // never written → idle
        const auto sp = spectrumOf(j);
        assert(static_cast<int>(sp.size()) == UnitMeterBank::kBands);
        int best = 0;
        for (int k = 1; k < UnitMeterBank::kBands; ++k) if (sp[k] > sp[best]) best = k;
        std::printf("spectrum peak band %d (%.0f Hz) = %d dB\n", best, UnitMeterBank::bandHz(best), sp[best]);
        assert(std::abs(std::log2(UnitMeterBank::bandHz(best) / 110.0)) < 0.35);
        assert(std::abs(sp[best] - (-6)) <= 3);
        assert(j.find("\"wv\":[") != std::string::npos);
        // Idle: a bypassed unit rests at the floor.
        inAudio = true;
        bank->beginBlock(B, sr);
        bank->recordIdle(2);
        inAudio = false;
        j = bank->json({2}, {}, {}, {});
        assert(field(j, "{\"s\":2", "or") <= -119.0);
        // Stops metering ~1.5 s after the last poll.
        bool on = true;
        for (int blk = 0; blk < 400; ++blk) on = bank->beginBlock(B, sr);
        assert(!on);
        assert(UnitMeterBank::parseList("0,3,7") == (std::vector<int>{0, 3, 7}));
    }
    // ---- through a rack: compressors report gain reduction (a circuit unit from its own meter node), a phaser its LFO phase ----
    {
        rig::RackGraph g;
        g.prepare(sr, 512);
        assert(g.addComponentSlot(130000)); // VCA bus compressor (DynamicsComponent)
        assert(g.addComponentSlot(75000));  // 4-stage phaser (StereoModulationComponent)
        assert(g.addComponentSlot(48000));  // stomp compressor, bypassed below
        assert(g.addComponentSlot(124000)); // opto leveling amp (CircuitUnit: GR from its T4B meter-bridge cell)
        g.setSlotBypassed(2, true);
        auto bank = std::make_shared<UnitMeterBank>();
        g.setMeterBank(bank);
        auto diag = std::make_shared<rig::EngineDiagBank>(); // /api/diag counters: also allocation-free on the audio thread
        g.setDiagBank(diag);
        auto plan = g.renderSnapshot();
        bank->json({0, 1, 2, 3}, {}, {}, {});
        std::vector<float> l(B), r(B), ol(B), orr(B);
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        double ph = 0.0;
        for (int blk = 0; blk < 600; ++blk) { // ~3 s: the LFO phase is reported once two full cycles have been seen
            if (blk % 100 == 99) bank->json({0, 1, 2, 3}, {}, {}, {}); // keep polling (metering disarms ~1.5 s after a poll)
            for (uint32_t i = 0; i < B; ++i) {
                l[i] = r[i] = 0.9f * static_cast<float>(std::sin(ph));
                ph += 2.0 * std::numbers::pi * 220.0 / sr;
            }
            rig::AudioBlock blkc{};
            blkc.in = ins;
            blkc.out = outs;
            blkc.num_channels = 2;
            blkc.num_frames = B;
            blkc.bpm = 120.0;
            const uint64_t t0 = rig::diagNowNs();
            inAudio = true;
            diag->beginRender(t0);
            plan->process(blkc);
            diag->endRender(rig::diagNowNs(), rig::diagNowNs() - t0, static_cast<uint64_t>(1e9 * B / sr), B);
            inAudio = false;
        }
        {
            const auto tot = diag->renderTotal();
            assert(tot.blocks == 600);
            assert(diag->slotTotal(0).blocks == 600 && diag->slotTotal(0).out_peak > 0.0f); // compressor
            assert(diag->slotTotal(2).blocks == 0);                                         // bypassed: not rendered
            const auto circ = diag->slotInfo(3);
            assert(circ.circuit && circ.internal_rate >= sr && diag->slotTotal(3).c_samples > 0); // circuit unit's solver counters
            assert(diag->masterTotal().samples == 600ull * B);
        }
        const std::string j = bank->json({0, 1, 2, 3}, {}, {}, {});
        std::printf("rack: %s\n", j.c_str());
        assert(field(j, "{\"s\":0", "gr") > 1.0);                 // hot sine into a compressor
        assert(field(j, "{\"s\":0", "ir") > -10.0);
        const double lfo = field(j, "{\"s\":1", "lfo");
        assert(lfo >= 0.0 && lfo < 1.0);
        assert(field(j, "{\"s\":2", "or") <= -119.0);               // bypassed → rest
        assert(field(j, "{\"s\":2", "gr") == -999.0);               // and no GR reported
        assert(field(j, "{\"s\":3", "gr") > 3.0);                 // the circuit's own meter cell, on a hot sine
    }
    {
        // Leslie 122 (31000): the circuit amplifier feeding the rotor block reports the horn rotor's angle as its LFO phase
        // (the face's rotor), and renders allocation-free like every circuit unit.
        rig::RackGraph g;
        g.prepare(sr, 512);
        assert(g.addComponentSlot(31000));
        auto bank = std::make_shared<UnitMeterBank>();
        g.setMeterBank(bank);
        auto plan = g.renderSnapshot();
        bank->json({0}, {}, {}, {});
        std::vector<float> l(B), r(B), ol(B), orr(B);
        const float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        double ph = 0.0;
        for (int blk = 0; blk < 200; ++blk) {
            if (blk % 100 == 99) bank->json({0}, {}, {}, {});
            for (uint32_t i = 0; i < B; ++i) {
                l[i] = r[i] = 0.3f * static_cast<float>(std::sin(ph));
                ph += 2.0 * std::numbers::pi * 440.0 / sr;
            }
            rig::AudioBlock blkc{};
            blkc.in = ins;
            blkc.out = outs;
            blkc.num_channels = 2;
            blkc.num_frames = B;
            blkc.bpm = 120.0;
            inAudio = true;
            plan->process(blkc);
            inAudio = false;
        }
        const std::string j = bank->json({0}, {}, {}, {});
        std::printf("leslie: %s\n", j.substr(0, 160).c_str());
        const double lfo = field(j, "{\"s\":0", "lfo");
        assert(lfo >= 0.0 && lfo < 1.0);
        const double lfo2 = field(j, "{\"s\":0", "lfo2"); // the drum rotor's angle (the face's drum)
        assert(lfo2 >= 0.0 && lfo2 < 1.0);
        assert(std::abs(lfo2 - lfo) > 1e-3);
        assert(field(j, "{\"s\":0", "or") > -40.0);
    }
    {
        // Tube saturation compressors (159000 STC-159, 160000 STC-160 GT) at their catalogue defaults, on a plucked
        // guitar-level note train (peaks ≈ -12 dBFS): they compress audibly (the output is not the dry input), report
        // that gain reduction steadily (not a one-block flick), and Compress sets the amount. 159000's Mix default 0
        // is fully wet (the modelled hardware's Mix runs wet → dry), which used to leave the unit fully dry.
        for (int32_t tid : {159000, 160000}) {
            auto run = [&](float compress, double* in_db, double* out_db, std::vector<double>* grs) {
                rig::RackGraph g;
                g.prepare(sr, 512);
                assert(g.addComponentSlot(tid));
                if (compress >= 0.0f) g.setComponentParameter(0, tid == 159000 ? "M5" : "C6", compress);
                auto bank = std::make_shared<UnitMeterBank>();
                g.setMeterBank(bank);
                auto plan = g.renderSnapshot();
                bank->json({0}, {}, {}, {});
                std::vector<float> l(B), r(B), ol(B), orr(B);
                const float* ins[2] = {l.data(), r.data()};
                float* outs[2] = {ol.data(), orr.data()};
                double ei = 0.0, eo = 0.0;
                uint64_t n = 0;
                for (int blk = 0; blk < 480; ++blk) { // ~5 s: a note every 0.5 s, decaying like a string
                    if (blk % 10 == 9) {
                        const std::string j = bank->json({0}, {}, {}, {});
                        if (grs && blk > 100) grs->push_back(field(j, "{\"s\":0", "gr"));
                    }
                    for (uint32_t i = 0; i < B; ++i) {
                        const uint64_t k = n++;
                        const double t = static_cast<double>(k % 24000) / sr;
                        const double v = 0.16 * std::exp(-t * 3.0) * (std::sin(2.0 * std::numbers::pi * 110.0 * k / sr) +
                                                                      0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * k / sr));
                        l[i] = r[i] = static_cast<float>(v);
                    }
                    rig::AudioBlock blkc{};
                    blkc.in = ins;
                    blkc.out = outs;
                    blkc.num_channels = 2;
                    blkc.num_frames = B;
                    inAudio = true;
                    plan->process(blkc);
                    inAudio = false;
                    if (blk > 100)
                        for (uint32_t i = 0; i < B; ++i) { ei += double(l[i]) * l[i]; eo += double(ol[i]) * ol[i]; }
                }
                *in_db = 10.0 * std::log10(ei + 1e-30);
                *out_db = 10.0 * std::log10(eo + 1e-30);
            };
            double i0, o0, i1, o1, i2, o2;
            std::vector<double> grs;
            run(-1.0f, &i0, &o0, &grs);
            run(0.0f, &i1, &o1, nullptr);
            std::vector<double> grs_hi;
            run(1.0f, &i2, &o2, &grs_hi);
            double mean = 0.0, lo = 1e9, hi = -1e9, mean_hi = 0.0;
            for (double v : grs) { mean += v; lo = std::min(lo, v); hi = std::max(hi, v); }
            for (double v : grs_hi) mean_hi += v;
            mean /= grs.size();
            mean_hi /= grs_hi.size();
            std::printf("tube comp %d: default GR mean %.1f dB (min %.1f max %.1f), Compress 1: %.1f dB; level in→out %.1f dB\n",
                        tid, mean, lo, hi, mean_hi, o0 - i0);
            assert(mean > 2.5 && mean < 12.0);   // several dB at the default
            assert(lo > 0.5);                     // the needle stays down between notes (no flick back to 0)
            assert(mean_hi > mean + 4.0);         // Compress is the amount
            assert(std::abs(o0 - i0) < 6.0);      // automatic make-up: the level stays in the same place
            (void)i1; (void)o1; (void)i2; (void)o2;
        }
    }
    std::printf("unit meters: ok\n");
    return 0;
}
