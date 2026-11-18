// Tube amplifier heads and drive pedals built on the analog models in AmpModeling.hpp.
//
// Every amp is a voicing (stage count, per-stage gain/coupling/cathode/bias, tone-stack circuit and
// position, power-section character) plus an explicit mapping from the preset parameter ids to
// physical controls, so knob meaning is exact per amp rather than guessed from names.
#include <limits>
#include <iomanip>
#include "AmpModeling.hpp"
#include "AmpUtilities.hpp"
#include "circuit/CircuitAmp.hpp"
#include "ComponentModel.hpp"
#include "ConvolutionEngine.hpp"
#include "TubeAmpComponents.hpp"
#include "../core/EngineFingerprint.hpp" // amp-trims cache key
#include "json.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>

namespace rig {

namespace {

using amp::dbToLin;
using amp::FmvToneStack;
using amp::OnePole;
using amp::Oversampler;
using amp::PowerAmp;
using amp::PowerSpec;
using amp::StageSpec;
using amp::Svf;
using amp::ToneStackValues;
using amp::TriodeStage;
namespace stacks = amp::stacks;

// ───────────────────────────── Voicings ─────────────────────────────
constexpr int kMaxStages = 5;

struct ChannelVoice {
    int n = 2;
    std::array<StageSpec, kMaxStages> st{};
    int pot_after = 1;       // gain pot sits after this many stages (0 = at the input)
    int stack_after = 99;    // tone stack sits after this many stages (clamped to n)
    float bright_db = 0.0f;  // bright cap boost (max, at low gain settings)
    float bright_hz = 2200.0f;
    float input_hp = 25.0f;
    float pot_floor = 0.004f;
};

struct AmpVoice {
    std::array<ChannelVoice, 4> ch{};
    ToneStackValues stack = stacks::kMarshall;
    PowerSpec power{};
    float stack_in = 1.0f;   // level into the tone stack
    float power_in = 1.0f;   // level from tone stack into the phase inverter
    bool has_master = true;
    float fixed_master = 0.62f;
    bool bass_amp = false;
    bool solid_state = false;
};

StageSpec S(float gain, float hp, float lp, float bias = 0.0f, float cath_hz = 0.0f, float cath_db = 0.0f,
            float lim_pos = 1.0f, float lim_neg = 1.6f, float blocking = 0.35f) {
    StageSpec s;
    s.gain = gain; s.hp_hz = hp; s.lp_hz = lp; s.bias = bias;
    s.cath_hz = cath_hz; s.cath_db = cath_db;
    s.lim_pos = lim_pos; s.lim_neg = lim_neg; s.blocking = blocking;
    return s;
}
// Cathode follower: unity-ish gain, clips on the positive swing.
StageSpec CF(float drive = 1.0f, float lp = 16000.0f) { return S(drive, 8.0f, lp, 0.0f, 0, 0, 0.85f, 2.6f, 0.15f); }

PowerSpec P(float drive, float sag, float knee, float asym, float ot_hp, float ot_lp,
            float pres_hz = 3500.0f, float pres_db = 7.0f, float depth_hz = 95.0f, float depth_db = 6.0f,
            bool class_a = false) {
    PowerSpec p;
    p.drive = drive; p.sag = sag; p.knee = knee; p.asym = asym;
    p.ot_hp_hz = ot_hp; p.ot_lp_hz = ot_lp;
    p.presence_hz = pres_hz; p.presence_db = pres_db;
    p.depth_hz = depth_hz; p.depth_db = depth_db;
    p.class_a = class_a;
    return p;
}

ChannelVoice C(int pot_after, int stack_after, float bright_db, float input_hp, std::initializer_list<StageSpec> st) {
    ChannelVoice c;
    c.n = static_cast<int>(std::min<std::size_t>(st.size(), kMaxStages));
    int i = 0;
    for (const auto& s : st) {
        if (i >= kMaxStages) break;
        c.st[static_cast<std::size_t>(i++)] = s;
    }
    c.pot_after = pot_after;
    c.stack_after = stack_after;
    c.bright_db = bright_db;
    c.input_hp = input_hp;
    return c;
}

// Builds the high-gain "cascaded" lead channel used by the modern amps.
ChannelVoice leadChannel(float s2, float s3, float s4, float tight_hz, float cath2_hz, float lp, float bright = 3.0f) {
    return C(1, 99, bright, tight_hz,
             {S(1.5f, 20, 15000, -0.05f, 300, -5.0f),
              S(s2, 110, 11000, -0.20f, cath2_hz, -9.0f),
              S(s3, 70, lp, -0.10f, 250, -4.0f),
              S(s4, 40, lp * 0.8f, -0.50f, 0, 0, 1.0f, 1.3f, 0.45f),
              CF(1.1f, 14000)});
}

AmpVoice makeVoice(int32_t tid) {
    AmpVoice v;
    switch (tid) {
        case 34000: // 1959 Plexi (non-master)
        case 92000: { // Variac brown-sound Plexi
            const float s2 = (tid == 92000) ? 9.0f : 7.0f;
            v.ch[0] = C(1, 99, 6.0f, 22.0f, {S(1.4f, 20, 14000, -0.10f), S(s2, 30, 11000, -0.15f), CF(1.2f)});
            v.stack = stacks::kMarshall;
            v.power = P((tid == 92000) ? 2.6f : 2.2f, (tid == 92000) ? 0.45f : 0.32f, 1.0f, 0.06f, 45, 10500);
            v.has_master = false;
            v.fixed_master = 0.66f;
            break;
        }
        case 91000: // Hot-rodded Plexi
            v.ch[0] = C(1, 99, 4.0f, 30.0f,
                        {S(1.4f, 20, 14000, -0.05f), S(9.0f, 45, 10000, -0.30f, 400, -6.0f), S(4.0f, 30, 8500, -0.10f), CF(1.1f)});
            v.stack = stacks::kMarshall;
            v.power = P(1.6f, 0.30f, 1.0f, 0.06f, 45, 10500);
            break;
        case 56000: // JCM 800 2203
            v.ch[0] = C(1, 99, 4.0f, 30.0f,
                        {S(1.3f, 20, 16000, -0.05f, 120, -6.0f), S(11.0f, 60, 12000, -0.45f, 0, 0, 1.0f, 1.3f, 0.45f),
                         S(4.0f, 30, 9000, -0.10f), CF(1.0f)});
            v.stack = stacks::kMarshall;
            v.power = P(1.25f, 0.25f, 1.0f, 0.06f, 45, 10500);
            break;
        case 101000: // Jumped JCM Mk II — channel 1 = gain-mode (extra cascaded stage)
            v.ch[0] = C(1, 99, 4.0f, 28.0f,
                        {S(1.4f, 20, 16000, -0.05f, 120, -6.0f), S(12.0f, 60, 12000, -0.45f, 0, 0, 1.0f, 1.3f, 0.45f),
                         S(4.0f, 30, 9000, -0.10f), CF(1.0f)});
            v.ch[1] = C(1, 99, 4.0f, 45.0f,
                        {S(1.5f, 20, 16000, -0.05f, 200, -6.0f), S(13.0f, 80, 11500, -0.40f, 500, -6.0f),
                         S(7.0f, 50, 9000, -0.15f), S(3.5f, 35, 7500, -0.45f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.stack = stacks::kMarshall;
            v.power = P(1.3f, 0.25f, 1.0f, 0.06f, 45, 10500);
            break;
        case 134000: // 5150 — ch0 rhythm, ch1 rhythm+crunch, ch2 lead
            v.ch[0] = C(1, 99, 6.0f, 30.0f, {S(1.4f, 20, 15000, -0.05f, 300, -4.0f), S(5.0f, 60, 12000, -0.15f), CF(1.0f)});
            v.ch[1] = C(1, 99, 6.0f, 35.0f,
                        {S(1.5f, 20, 15000, -0.05f, 300, -4.0f), S(9.0f, 80, 11000, -0.20f, 600, -6.0f),
                         S(4.0f, 40, 8500, -0.40f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[2] = leadChannel(15.0f, 10.0f, 5.0f, 45.0f, 700.0f, 9000.0f, 2.0f);
            v.stack = stacks::kModern;
            v.power = P(1.35f, 0.22f, 1.0f, 0.05f, 45, 9500, 3800, 8, 85, 8);
            break;
        case 122000: // Soldano SLO-100 — ch0 normal, ch1 overdrive
        case 163000: // Soldano SLO-100 (Neural)
            v.ch[0] = C(1, 99, 5.0f, 25.0f, {S(1.3f, 20, 15000, -0.05f), S(4.0f, 30, 12000, -0.10f), CF(1.0f)});
            v.ch[1] = C(1, 99, 5.0f, 30.0f, {S(1.4f, 20, 15000, -0.05f), S(8.0f, 50, 11000, -0.25f, 400, -5.0f),
                                               S(3.0f, 30, 9000, -0.40f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[2] = C(1, 99, 3.0f, 40.0f,
                        {S(1.5f, 20, 15000, -0.05f, 300, -4.0f), S(13.0f, 90, 11500, -0.15f, 500, -7.0f),
                         S(8.0f, 60, 9500, -0.10f, 200, -3.0f), S(4.0f, 35, 7800, -0.55f, 0, 0, 1.0f, 1.3f, 0.45f), CF(1.1f)});
            v.stack = stacks::kModern;
            v.power = P(1.3f, 0.22f, 1.0f, 0.05f, 45, 10000, 3600, 7, 90, 7);
            break;
        case 81000: // Bogner Uberschall — ch0 crunch, ch1 ultra
            v.ch[0] = C(1, 99, 3.0f, 35.0f, {S(1.4f, 20, 14000, -0.05f, 250, -4.0f), S(9.0f, 70, 11000, -0.25f, 500, -6.0f),
                                               S(4.0f, 40, 8500, -0.40f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[1] = leadChannel(17.0f, 11.0f, 6.0f, 60.0f, 850.0f, 8000.0f, 1.0f);
            v.stack = stacks::kModern;
            v.power = P(1.4f, 0.25f, 1.0f, 0.05f, 45, 9000, 3500, 7, 80, 8);
            break;
        case 16000: // Dual Rectifier — modes: 0 clean, 1 raw, 2 vintage, 3 modern
        case 96000: { // Mesa/Boogie Dual Rectifier Rev F (tighter & hotter)
            const bool rev_c = (tid == 96000);
            v.ch[0] = C(1, 99, 6.0f, 25.0f, {S(1.2f, 20, 15000, -0.05f), S(3.5f, 25, 12000, -0.10f), CF(1.0f)});
            v.ch[1] = C(1, 99, 2.0f, 35.0f, {S(1.4f, 20, 14000, -0.05f, 250, -4.0f), S(10.0f, 60, 11000, -0.20f, 500, -5.0f),
                                               S(5.0f, 40, 8500, -0.35f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[2] = leadChannel(13.0f, 9.0f, 4.5f, 35.0f, 500.0f, 8500.0f, 1.5f);
            v.ch[3] = leadChannel(rev_c ? 18.0f : 16.0f, rev_c ? 11.0f : 10.0f, rev_c ? 6.0f : 5.0f, rev_c ? 55.0f : 32.0f,
                                  rev_c ? 800.0f : 450.0f, rev_c ? 7800.0f : 8200.0f, 1.0f);
            v.stack = stacks::kModern;
            v.power = P(1.45f, rev_c ? 0.30f : 0.45f, 1.0f, 0.05f, rev_c ? 50.0f : 38.0f, 9000, 3300, 7, 80, 8);
            break;
        }
        case 80000: // Orange OR120
            v.ch[0] = C(1, 99, 0.0f, 30.0f, {S(1.4f, 20, 13000, -0.05f), S(8.0f, 60, 10000, -0.20f, 300, -4.0f),
                                               S(4.0f, 40, 8000, -0.30f), CF(1.0f)});
            v.stack = stacks::kFender;
            v.power = P(1.7f, 0.35f, 1.0f, 0.07f, 45, 9000, 3000, 6, 100, 5);
            break;
        case 87000: // Hiwatt DR103 Custom 100
            v.ch[0] = C(1, 99, 4.0f, 22.0f, {S(1.1f, 20, 16000, -0.05f), S(3.0f, 22, 15000, -0.08f), CF(0.9f)});
            v.stack = stacks::kHiwatt;
            v.power = P(0.95f, 0.10f, 1.3f, 0.04f, 40, 11500);
            break;
        case 30000: // Fender '65 Twin Reverb (Fender stack before the volume pot)
            v.ch[0] = C(1, 1, 7.0f, 20.0f, {S(1.2f, 20, 14000, -0.10f), S(3.0f, 20, 12000, -0.10f)});
            v.stack = stacks::kFender;
            v.power = P(0.85f, 0.15f, 1.25f, 0.04f, 40, 11000, 3500, 0, 95, 0);
            v.has_master = false;
            v.fixed_master = 0.55f;
            v.power_in = 1.6f;
            break;
        case 172000: // Fender '65 Deluxe Reverb
            v.ch[0] = C(1, 1, 6.0f, 20.0f, {S(1.3f, 20, 13000, -0.10f), S(3.6f, 20, 11000, -0.12f)});
            v.stack = stacks::kFender;
            v.power = P(1.35f, 0.30f, 1.0f, 0.05f, 45, 10000, 3500, 0, 95, 0);
            v.has_master = false;
            v.fixed_master = 0.6f;
            v.power_in = 1.6f;
            break;
        case 62000: // Fender '59 Bassman 5F6-A
            v.ch[0] = C(1, 99, 5.0f, 20.0f, {S(1.3f, 20, 12000, -0.05f), S(4.0f, 20, 9000, -0.20f), CF(1.0f)});
            v.stack = stacks::kFender;
            v.power = P(1.5f, 0.38f, 1.0f, 0.06f, 45, 9500, 3000, 6, 95, 0);
            v.has_master = false;
            v.fixed_master = 0.62f;
            break;
        case 79000: // 5E3 Tweed Deluxe — single tone control
            v.ch[0] = C(1, 99, 4.0f, 20.0f, {S(1.6f, 20, 10000, -0.10f), S(5.0f, 20, 8000, -0.25f)});
            v.stack = stacks::kFender;
            v.power = P(2.0f, 0.50f, 0.95f, 0.10f, 50, 8500, 3000, 0, 95, 0, true);
            v.has_master = false;
            v.fixed_master = 0.62f;
            break;
        case 152000: // Supro 1624T Dual-Tone
            v.ch[0] = C(1, 99, 5.0f, 30.0f, {S(2.0f, 30, 9000, -0.10f), S(5.0f, 30, 7000, -0.30f)});
            v.stack = stacks::kFender;
            v.power = P(2.2f, 0.50f, 0.95f, 0.10f, 60, 7500, 3000, 0, 95, 0, true);
            v.has_master = false;
            v.fixed_master = 0.6f;
            break;
        case 38000:  // Vox AC30 Top Boost
        case 176000: // Vox AC15 Custom
            v.ch[0] = C(1, 99, 5.0f, 25.0f, {S(1.5f, 20, 16000, -0.05f), S(3.2f, 25, 14000, -0.12f), CF(1.0f)});
            v.stack = stacks::kVox;
            v.power = P(tid == 176000 ? 2.2f : 1.9f, 0.32f, 0.9f, 0.09f, 55, 12000, 3500, 0, 95, 0, true);
            v.has_master = false;
            v.fixed_master = 0.62f;
            break;
        case 151000: // Friedman BE-100 Deluxe — gain modes 0..2
            v.ch[0] = C(1, 99, 3.0f, 35.0f, {S(1.4f, 20, 15000, -0.05f, 250, -4.0f), S(9.0f, 70, 11000, -0.25f, 500, -6.0f),
                                               S(4.5f, 40, 8800, -0.40f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[1] = leadChannel(12.0f, 8.0f, 4.0f, 45.0f, 600.0f, 9000.0f, 2.0f);
            v.ch[2] = leadChannel(16.0f, 10.0f, 5.5f, 50.0f, 700.0f, 8500.0f, 1.5f);
            v.stack = stacks::kMarshall;
            v.power = P(1.4f, 0.25f, 1.0f, 0.06f, 45, 9800, 3500, 6, 90, 6);
            break;
        case 162000: // Bogner Ecstasy 101B boutique overdrive channel — gain modes 0..2
        case 300000:
            v.ch[0] = C(1, 99, 4.0f, 28.0f, {S(1.4f, 20, 14000, -0.05f), S(6.0f, 50, 11000, -0.20f, 300, -4.0f), CF(1.0f)});
            v.ch[1] = C(1, 99, 3.0f, 35.0f, {S(1.4f, 20, 14000, -0.05f, 250, -4.0f), S(10.0f, 60, 10500, -0.20f, 400, -5.0f),
                                               S(5.0f, 40, 8500, -0.35f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.0f)});
            v.ch[2] = C(1, 99, 2.0f, 40.0f,
                        {S(1.5f, 20, 14000, -0.05f, 300, -4.0f), S(12.0f, 80, 10000, -0.15f, 450, -6.0f),
                         S(7.0f, 55, 8500, -0.10f, 200, -3.0f), S(3.5f, 35, 7000, -0.45f, 0, 0, 1.0f, 1.3f, 0.4f), CF(1.1f)});
            v.stack = stacks::kModern;
            v.power = P(1.35f, 0.25f, 1.0f, 0.06f, 45, 9800, 3500, 6, 90, 6);
            break;
        case 49000: // Roland JC-120 solid-state combo
            v.ch[0] = C(1, 99, 4.0f, 25.0f, {S(0.5f, 20, 18000, 0.0f, 0, 0, 3.0f, 3.0f, 0.0f)});
            v.stack = stacks::kFender;
            v.power = P(0.6f, 0.0f, 1.5f, 0.0f, 45, 14000, 3500, 0, 95, 0);
            v.solid_state = true;
            v.power_in = 1.4f;
            break;
        case 54000:  // Ampeg SVT 1969 Blue Line
        case 177000: // Ampeg SVT-VR
            v.ch[0] = C(1, 99, 3.0f, 18.0f, {S(1.1f, 15, 14000, -0.05f), S(2.6f, 15, 11000, -0.12f), CF(1.0f)});
            v.stack = stacks::kBass;
            v.power = P(1.0f, 0.22f, 1.1f, 0.05f, 28, 8500, 3000, 4, 60, 6);
            v.bass_amp = true;
            break;
        case 150000: // Solid-state bass head
            v.ch[0] = C(1, 99, 3.0f, 18.0f, {S(0.6f, 15, 16000, 0.0f, 0, 0, 2.5f, 2.5f, 0.0f), S(1.2f, 15, 14000, 0.0f, 0, 0, 1.6f, 1.8f, 0.0f)});
            v.stack = stacks::kBass;
            v.power = P(0.7f, 0.05f, 1.4f, 0.0f, 28, 10000, 3000, 4, 60, 6);
            v.bass_amp = true;
            v.solid_state = true;
            break;
        default: // generic British crunch
            v.ch[0] = C(1, 99, 4.0f, 30.0f, {S(1.3f, 20, 15000, -0.05f), S(8.0f, 50, 11000, -0.30f), S(3.5f, 30, 9000), CF(1.0f)});
            v.stack = stacks::kMarshall;
            v.power = P(1.3f, 0.25f, 1.0f, 0.06f, 45, 10000);
            break;
    }
    // Output damping (negative feedback) and the speaker resonance of the cab each amp normally drives.
    switch (tid) {
        case 38000: case 176000: case 79000: case 152000: v.power.damping = 0.15f; v.power.spk_fs = 105.0f; break; // no NFB
        case 62000: v.power.damping = 0.40f; v.power.spk_fs = 95.0f; break;
        case 30000: case 172000: v.power.damping = 0.60f; v.power.spk_fs = 110.0f; break;
        case 80000: v.power.damping = 0.45f; v.power.spk_fs = 85.0f; break;
        case 87000: v.power.damping = 0.70f; v.power.spk_fs = 85.0f; break;
        case 134000: case 122000: case 163000: case 81000: case 16000: case 96000: case 151000: case 162000: case 300000:
            v.power.damping = 0.62f; v.power.spk_fs = 82.0f; break;
        case 54000: case 177000: v.power.damping = 0.72f; v.power.spk_fs = 62.0f; break;
        case 49000: case 150000: v.power.damping = 0.95f; v.power.spk_fs = 70.0f; break; // solid state
        default: v.power.damping = 0.52f; v.power.spk_fs = 88.0f; break; // Marshall family
    }
    for (auto& c : v.ch) {
        if (c.n <= 0) c = v.ch[0];
    }
    return v;
}

// ───────────────────────────── Model export & user overrides ─────────────────────────────
using nlohmann::json;

// Strict variant: false when the text is not a JSON object (callers keep their last valid model).
bool parseModelChecked(std::string_view s, json& out) {
    if (s.empty()) { out = json::object(); return true; }
    try {
        out = json::parse(s.begin(), s.end());
        return out.is_object();
    } catch (...) {
        return false;
    }
}
bool jset(const json& o, const char* k, float& v, float lo, float hi) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_number()) return false;
    v = std::clamp(it->get<float>(), lo, hi);
    return true;
}
bool jflag(const json& o, const char* k, bool def = false) {
    const auto it = o.find(k);
    if (it == o.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() >= 0.5;
    return def;
}
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string fmtNum(double v, int prec) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}
std::string fmtHz(double hz) {
    if (hz >= 1000.0) return fmtNum(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz";
    return fmtNum(hz, hz < 10.0 ? 1 : 0) + " Hz";
}
std::string fmtOhm(double r) {
    if (r >= 1e6) return fmtNum(r / 1e6, r >= 1e7 ? 0 : 1) + " MΩ";
    if (r >= 1e3) return fmtNum(r / 1e3, r >= 1e5 ? 0 : 1) + " kΩ";
    return fmtNum(r, 0) + " Ω";
}
std::string fmtFarad(double c) {
    if (c >= 1e-6) return fmtNum(c * 1e6, c >= 1e-5 ? 0 : 1) + " µF";
    if (c >= 1e-9) return fmtNum(c * 1e9, c >= 1e-8 ? 0 : 1) + " nF";
    return fmtNum(c * 1e12, 0) + " pF";
}
std::string fmtDb(double db, int prec = 1) { return (db > 0.05 ? "+" : "") + fmtNum(db, prec) + " dB"; }
double linToDb(double g) { return 20.0 * std::log10(std::max(1e-9, std::abs(g))); }

// One displayed value of a schematic part. `note` says what the number means in the model.
json mval(const std::string& k, const std::string& v, double raw, const std::string& note = {}, json edit = nullptr) {
    json o = {{"k", k}, {"v", v}, {"raw", raw}};
    if (!note.empty()) o["note"] = note;
    if (!edit.is_null()) o["edit"] = std::move(edit);
    return o;
}
json editNum(const std::string& path, double mn, double mx, double step, bool log = false) {
    json e = {{"path", path}, {"min", mn}, {"max", mx}, {"step", step}};
    if (log) e["scale"] = "log";
    return e;
}
json editOpts(const std::string& path, json opts) { return {{"path", path}, {"options", std::move(opts)}}; }
json block(const std::string& id, const std::string& type, const std::string& label, const std::string& sub, json values,
           const std::string& section) {
    return {{"id", id}, {"type", type}, {"label", label}, {"sub", sub}, {"values", std::move(values)}, {"section", section}};
}
// 96 log-spaced analysis frequencies, 20 Hz – 20 kHz.
std::vector<double> logFreqs(int n = 96, double lo = 20.0, double hi = 20000.0) {
    std::vector<double> f(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) f[static_cast<std::size_t>(i)] = lo * std::pow(hi / lo, static_cast<double>(i) / (n - 1));
    return f;
}
// Magnitude (dB) of an impulse response at the given frequencies (direct DFT, Hann-tapered tail).
std::vector<double> irMagnitudeDb(const std::vector<float>& h, double sr, const std::vector<double>& freqs) {
    std::vector<double> out;
    out.reserve(freqs.size());
    const std::size_t n = h.size();
    for (double f : freqs) {
        const double w = 2.0 * std::numbers::pi * f / sr;
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = (i > n * 3 / 4) ? 0.5 + 0.5 * std::cos(std::numbers::pi * static_cast<double>(i - n * 3 / 4) / static_cast<double>(n / 4)) : 1.0;
            re += h[i] * t * std::cos(w * static_cast<double>(i));
            im -= h[i] * t * std::sin(w * static_cast<double>(i));
        }
        out.push_back(linToDb(std::sqrt(re * re + im * im)));
    }
    return out;
}

struct StackPreset { const char* id; const char* label; ToneStackValues v; };
const std::array<StackPreset, 6>& stackPresets() {
    static const std::array<StackPreset, 6> p = {{
        {"fender", "Fender values (Bassman / blackface)", stacks::kFender},
        {"marshall", "Marshall values (JTM / JCM)", stacks::kMarshall},
        {"modern", "Modern high-gain values (Soldano / 5150 / Mesa)", stacks::kModern},
        {"hiwatt", "Mid-forward values (Hiwatt)", stacks::kHiwatt},
        {"vox", "Vox-style values", stacks::kVox},
        {"bass", "Bass values (SVT)", stacks::kBass},
    }};
    return p;
}
const char* stackPresetId(const ToneStackValues& v) {
    for (const auto& p : stackPresets())
        if (p.v.C1 == v.C1 && p.v.C2 == v.C2 && p.v.C3 == v.C3 && p.v.R1 == v.R1 && p.v.R2 == v.R2 && p.v.R3 == v.R3 && p.v.R4 == v.R4) return p.id;
    return "custom";
}

bool isFollowerSpec(const StageSpec& s) { return s.lim_neg >= 2.4f || s.lim_pos >= 2.0f; }

void applyStageOverride(StageSpec& s, const json& o) {
    if (const auto t = o.find("type"); t != o.end() && t->is_string()) {
        const std::string ty = t->get<std::string>();
        if (ty == "follower" && !isFollowerSpec(s)) { s.lim_pos = 0.85f; s.lim_neg = 2.6f; s.blocking = 0.15f; s.bias = 0.0f; }
        else if (ty == "triode" && isFollowerSpec(s)) { s.lim_pos = 1.0f; s.lim_neg = 1.6f; s.blocking = 0.35f; s.bias = -0.05f; }
    }
    jset(o, "gain", s.gain, 0.05f, 40.0f);
    jset(o, "hp_hz", s.hp_hz, 1.0f, 2000.0f);
    jset(o, "lp_hz", s.lp_hz, 500.0f, 22000.0f);
    jset(o, "bias", s.bias, -1.0f, 0.3f);
    jset(o, "cath_hz", s.cath_hz, 0.0f, 3000.0f);
    jset(o, "cath_db", s.cath_db, -24.0f, 0.0f);
    jset(o, "lim_pos", s.lim_pos, 0.1f, 4.0f);
    jset(o, "lim_neg", s.lim_neg, 0.1f, 4.0f);
    jset(o, "blocking", s.blocking, 0.0f, 1.0f);
}

// Original (stock) index of every stage after path edits, per channel.
using StageOrigin = std::array<std::array<int, kMaxStages>, 4>;
StageOrigin identityOrigin() {
    StageOrigin o{};
    for (auto& c : o)
        for (int i = 0; i < kMaxStages; ++i) c[static_cast<std::size_t>(i)] = i;
    return o;
}

// Applies {"amp":{...}} overrides to a voicing. Returns the canonical DSP part of the overrides ("" if none).
// Stage keys always address the stock stage index, so edits stay attached to a stage when another is removed.
std::string applyAmpOverrides(AmpVoice& v, const json& root, int real_channels, StageOrigin* origin = nullptr) {
    if (origin) *origin = identityOrigin();
    const auto a = root.find("amp");
    if (a == root.end() || !a->is_object() || a->empty()) return {};
    if (const auto chs = a->find("channels"); chs != a->end() && chs->is_object()) {
        for (const auto& [key, co] : chs->items()) {
            if (!co.is_object()) continue;
            int k = -1;
            try { k = std::stoi(key); } catch (...) { continue; }
            if (k < 0 || k >= 4) continue;
            ChannelVoice& c = v.ch[static_cast<std::size_t>(k)];
            std::array<bool, kMaxStages> off{};
            if (const auto st = co.find("stages"); st != co.end() && st->is_object()) {
                for (const auto& [ik, so] : st->items()) {
                    int i = -1;
                    try { i = std::stoi(ik); } catch (...) { continue; }
                    if (i < 0 || i >= c.n || !so.is_object()) continue;
                    applyStageOverride(c.st[static_cast<std::size_t>(i)], so);
                    off[static_cast<std::size_t>(i)] = jflag(so, "off");
                }
            }
            jset(co, "bright_db", c.bright_db, 0.0f, 12.0f);
            jset(co, "bright_hz", c.bright_hz, 200.0f, 10000.0f);
            jset(co, "input_hp", c.input_hp, 5.0f, 400.0f);
            // Path edits: drop stages (positions of the gain pot and tone stack move with them).
            int w = 0;
            int pot = c.pot_after, stack = std::min(c.stack_after, c.n + 1);
            const int n0 = c.n;
            for (int i = 0; i < n0; ++i) {
                if (off[static_cast<std::size_t>(i)] && c.n - (i - w) > 1) {
                    if (i < pot) --pot;
                    if (i < stack) --stack;
                    continue;
                }
                if (origin) (*origin)[static_cast<std::size_t>(k)][static_cast<std::size_t>(w)] = i;
                c.st[static_cast<std::size_t>(w++)] = c.st[static_cast<std::size_t>(i)];
            }
            for (int i = w; i < kMaxStages; ++i) c.st[static_cast<std::size_t>(i)] = StageSpec{};
            c.n = std::max(1, w);
            c.pot_after = std::clamp(pot, 0, c.n);
            c.stack_after = std::clamp(stack, 1, c.n + 1) > c.n ? 99 : std::clamp(stack, 1, c.n + 1);
            // Gain-pot / tone-stack positions count stages of the edited path (after removals).
            float f = 0.0f;
            if (jset(co, "pot_after", f, 0.0f, 5.0f)) c.pot_after = std::clamp(static_cast<int>(std::lround(f)), 0, c.n);
            if (jset(co, "stack_after", f, 1.0f, 6.0f)) {
                const int sa = std::clamp(static_cast<int>(std::lround(f)), 1, c.n + 1);
                c.stack_after = sa > c.n ? 99 : sa;
            }
        }
    }
    if (const auto st = a->find("stack"); st != a->end() && st->is_object()) {
        if (const auto pr = st->find("preset"); pr != st->end() && pr->is_string()) {
            for (const auto& p : stackPresets())
                if (pr->get<std::string>() == p.id) v.stack = p.v;
        }
        float x = 0.0f;
        if (jset(*st, "C1", x, 1e-12f, 1e-5f)) v.stack.C1 = x;
        if (jset(*st, "C2", x, 1e-12f, 1e-5f)) v.stack.C2 = x;
        if (jset(*st, "C3", x, 1e-12f, 1e-5f)) v.stack.C3 = x;
        if (jset(*st, "R1", x, 100.0f, 1e7f)) v.stack.R1 = x;
        if (jset(*st, "R2", x, 100.0f, 1e7f)) v.stack.R2 = x;
        if (jset(*st, "R3", x, 100.0f, 1e7f)) v.stack.R3 = x;
        if (jset(*st, "R4", x, 100.0f, 1e7f)) v.stack.R4 = x;
    }
    if (const auto pw = a->find("power"); pw != a->end() && pw->is_object()) {
        PowerSpec& p = v.power;
        jset(*pw, "drive", p.drive, 0.05f, 6.0f);
        jset(*pw, "sag", p.sag, 0.0f, 1.0f);
        jset(*pw, "asym", p.asym, 0.0f, 0.4f);
        jset(*pw, "knee", p.knee, 0.3f, 3.0f);
        jset(*pw, "presence_hz", p.presence_hz, 500.0f, 12000.0f);
        jset(*pw, "presence_db", p.presence_db, 0.0f, 15.0f);
        jset(*pw, "depth_hz", p.depth_hz, 30.0f, 400.0f);
        jset(*pw, "depth_db", p.depth_db, 0.0f, 15.0f);
        jset(*pw, "ot_hp_hz", p.ot_hp_hz, 10.0f, 300.0f);
        jset(*pw, "ot_lp_hz", p.ot_lp_hz, 2000.0f, 20000.0f);
        jset(*pw, "damping", p.damping, 0.0f, 1.0f);
        jset(*pw, "spk_fs", p.spk_fs, 40.0f, 200.0f);
        if (pw->contains("class_a")) p.class_a = jflag(*pw, "class_a");
    }
    jset(*a, "stack_in", v.stack_in, 0.05f, 4.0f);
    jset(*a, "power_in", v.power_in, 0.05f, 4.0f);
    // Channels the amp doesn't really have mirror channel 0 (as makeVoice does).
    for (int k = std::max(1, real_channels); k < 4; ++k) {
        v.ch[static_cast<std::size_t>(k)] = v.ch[0];
        if (origin) (*origin)[static_cast<std::size_t>(k)] = (*origin)[0];
    }
    return a->dump();
}

int realChannelCount(const AmpVoice& v) {
    int n = 1;
    for (int k = 1; k < 4; ++k)
        if (std::memcmp(&v.ch[static_cast<std::size_t>(k)], &v.ch[0], sizeof(ChannelVoice)) != 0) n = k + 1;
    return n;
}

// Channels a player can actually select on each amp (index into AmpVoice::ch, front-panel name).
std::vector<std::pair<int, std::string>> ampChannels(int32_t tid) {
    switch (tid) {
        case 101000: return {{0, "Normal"}, {1, "High gain"}};
        case 134000: return {{0, "Rhythm"}, {1, "Rhythm · Crunch"}, {2, "Lead"}};
        case 122000: return {{0, "Normal"}, {2, "Overdrive"}};
        case 163000: return {{0, "Normal"}, {1, "Normal · Crunch"}, {2, "Overdrive"}};
        case 81000: return {{0, "Crunch"}, {1, "Ultra"}};
        case 16000: case 96000: return {{0, "Clean"}, {1, "Raw"}, {2, "Vintage"}, {3, "Modern"}};
        case 151000: case 162000: case 300000: return {{0, "Gain mode 1"}, {1, "Gain mode 2"}, {2, "Gain mode 3"}};
        default: return {{0, "Main"}};
    }
}

// Power sections as voiced for factory amps with these output tubes — offered as editing starting points.
json powerProfiles() {
    struct P { int32_t tid; const char* id; const char* label; };
    static const P kP[] = {
        {56000, "el34_jcm", "EL34 pair · as voiced for Marshall JCM800 2203"},
        {34000, "el34_plexi", "EL34 quad · as voiced for Marshall 1959SLP Super Lead"},
        {87000, "el34_hiwatt", "EL34 quad · as voiced for Hiwatt DR103 (stiff)"},
        {30000, "6l6_blackface", "6L6 quad · as voiced for Fender '65 Twin Reverb"},
        {16000, "6l6_recto", "6L6 quad · as voiced for Mesa/Boogie Dual Rectifier"},
        {134000, "6l6_5150", "6L6 quad · as voiced for Peavey 5150"},
        {79000, "6v6_tweed", "6V6 pair, no NFB · as voiced for Fender '57 Custom Deluxe"},
        {38000, "el84_ac30", "EL84 quad, no NFB · as voiced for Vox AC30 Top Boost"},
        {54000, "6550_svt", "6550 sextet · as voiced for Ampeg SVT-VR"},
        {49000, "solid_state", "Solid-state output · as voiced for Roland JC-120"},
    };
    json out = json::array();
    for (const auto& p : kP) {
        const PowerSpec s = makeVoice(p.tid).power;
        out.push_back({{"id", p.id}, {"label", p.label}, {"from", p.tid},
                       {"spec", {{"drive", s.drive}, {"sag", s.sag}, {"asym", s.asym}, {"knee", s.knee}, {"presence_hz", s.presence_hz},
                                 {"presence_db", s.presence_db}, {"depth_hz", s.depth_hz}, {"depth_db", s.depth_db}, {"ot_hp_hz", s.ot_hp_hz},
                                 {"ot_lp_hz", s.ot_lp_hz}, {"class_a", s.class_a}, {"damping", s.damping}, {"spk_fs", s.spk_fs}}}});
    }
    return out;
}

// ───────────────────────────── Control mapping ─────────────────────────────
struct AmpControls {
    int channel = 0;
    float gain = 0.5f;
    float volume = 1.0f; // channel volume after the preamp (e.g. Bogner Uberschall "Volume")
    float bass = 0.5f, mid = 0.5f, treble = 0.5f;
    float presence = 0.35f, depth = 0.35f, master = 0.6f;
    float sag = 0.5f, vac = 0.5f, bias = 0.5f;
    float bright = 0.0f;   // 0..1 amount of bright cap
    float input_boost_db = 0.0f;
    float cut = 0.0f;      // Vox tone cut
    float lo_cut = 0.0f;   // Orange OR120 low cut / Bass amp low cut
    float tight = 0.0f;
    std::array<float, 9> geq{};   // graphic EQ band gains (dB), Ampeg SVT-VR
    bool geq_on = false;
    bool circuit_core = false;
    bool input_gate = false;
    bool level_matching = true;
    int input_scale = 2; // explicitly assumed volts per full scale: .25, .5, 1, 2, 4
    // Built-in effects (spring reverb, tremolo / vibrato / chorus)
    bool rev_on = false;
    float rev_level = 0.0f, rev_decay = 1.0f;
    int mod_mode = 0; // 0 off, 1 tremolo, 2 vibrato, 3 chorus
    float mod_rate = 0.5f, mod_depth = 0.0f;
    bool operator==(const AmpControls&) const = default;
};

class TubeAmpComponent final : public SchemaComponentBase, public model::Introspectable {
public:
    explicit TubeAmpComponent(const catalog::ComponentSchema& schema)
        : SchemaComponentBase(schema), base_voice_(makeVoice(schema.table_id)), voice_(base_voice_) {
        real_channels_ = realChannelCount(base_voice_);
        // Pre-create the override slot so later writes never rehash the parameter map.
        params_[model::kModelParam].kind = catalog::ParamKind::String;
        params_["amp_core"].num_val = 0; // preserve existing rigs until the reference circuit is selected
        params_["input_gate"].num_val = 0;
        params_["level_matching"].num_val = 1;
        params_["input_voltage_scale"].num_val = 2;
    }

    std::vector<ExportedParam> exportParams() const override {
        auto v = SchemaComponentBase::exportParams();
        v.erase(std::remove_if(v.begin(), v.end(), [](const ExportedParam& e) {
                    return e.id == model::kAnalysisParam || (e.id == model::kModelParam && e.str.empty());
                }), v.end());
        return v;
    }

    void adoptRuntimeStateFrom(IComponent& component) noexcept override {
        auto* previous = dynamic_cast<TubeAmpComponent*>(&component);
        if (!previous || previous->tableId() != tableId() || previous->isr_ != isr_ ||
            previous->last_.channel != last_.channel) return;
        os_ = previous->os_;
        for (std::size_t i = 0; i < chain_.size(); ++i) {
            chain_[i].adoptRuntimeStateFrom(previous->chain_[i]);
            gate_[i].adoptRuntimeStateFrom(previous->gate_[i]);
        }
        if (vib_.size() == previous->vib_.size()) {
            vib_.swap(previous->vib_);
            vpos_ = previous->vpos_; lfo_ = previous->lfo_;
        }
        if (has_spring_ == previous->has_spring_ && spring_decay_ == previous->spring_decay_)
            std::swap(spring_, previous->spring_);
        load_ = previous->load_; // PowerAmp::adoptRuntimeStateFrom carried the matching overrides
    }

    // Face meters (UnitMeters): the built-in tremolo / vibrato / chorus LFO's phase, or "not running" (-2) while the
    // effect is switched off, so the face's LFO lamp follows the amp's own oscillator.
    void liveReadout(UnitLiveReadout& r) const noexcept override {
        r.lfo_phase = fx_.mod_mode != 0 && fx_.mod_depth > 0.001f ? lfo_ : -2.0f;
    }
    // Series pipelining (RackGraph): both channels run the same chain on the same state. The built-in chorus (dry left,
    // wet right) and the spring tank (its left and right impulses) make two sides.
    bool monoPreserving() const noexcept override {
        const bool chorus = fx_.mod_mode == 3 && fx_.mod_depth > 0.001f;
        const bool spring = has_spring_ && fx_.rev_on && spring_ir_;
        return !chorus && !spring;
    }

    // Cabinet → power-amp coupling: the rack hands each amp the impedance profile of the speaker
    // cabinet it drives (invalid = none, so the voicing's own speaker is used). Audio thread, once per
    // block; the PowerAmp filters are only redesigned when the profile actually changes (cab swap).
    void setSpeakerLoadProfile(const SpeakerImpedanceProfile& p) noexcept override {
        const bool same = p.valid == load_.valid &&
                          (!p.valid || (p.fs_hz == load_.fs_hz && p.q_res == load_.q_res && p.le_scale == load_.le_scale));
        if (same) return;
        load_ = p.valid ? p : SpeakerImpedanceProfile{};
        for (auto& c : chain_)
            c.power.setSpeakerLoadProfile(load_.valid ? load_.fs_hz : 0.0f, load_.valid ? load_.q_res : 1.4f,
                                          load_.valid ? load_.le_scale : 1.0f);
    }

    std::vector<ExtraParam> extraParams() const override {
        auto extras = std::vector<ExtraParam>{{"input_gate", "Input gate", {"Off", "On"}, 0, {}},
                {"level_matching", "Output level matching", {"Off", "On"}, 1, {}},
                {"input_voltage_scale", "Input reference (assumed)",
                 {"0.25 V / FS", "0.5 V / FS", "1 V / FS", "2 V / FS", "4 V / FS"}, 2, {}}};
        if (supportsCircuitCore()) extras.push_back({"amp_core", "Amp core", {"Voicing model", "Electrical reference"}, 0, {}});
        return extras;
    }
    bool supportsCircuitCore() const noexcept { return schema_.table_id == 56000 || schema_.table_id == 34000; }

    std::string modelJson() const override;
    std::string curvesJson() const override;

protected:
    void onStringParameterChanged(std::string_view id, std::string_view value) override {
        if (id == model::kAnalysisParam) { analysis_ = (value == "1"); return; }
        if (id != model::kModelParam) return;
        // Callers hold the graph lock (or own a unit that isn't playing yet), so the voicing can be
        // swapped directly; the next control-rate pass rebuilds the chains.
        json j;
        if (!parseModelChecked(value, j)) return; // malformed: keep the last valid voicing
        std::optional<circuit::CircuitDefinition> definition;
        if (j.contains("circuit")) {
            circuit::CircuitDefinition next;
            std::string error;
            const auto& spec = j["circuit"];
            if (spec.contains("schema")) {
                throw std::invalid_argument("Use circuit.values physical overrides; performance controls are amp parameters");
            } else {
                if (!spec.is_object()) throw std::invalid_argument("Circuit overrides must be an object");
                for (auto it = spec.begin(); it != spec.end(); ++it)
                    if (it.key() != "values" && it.key() != "topology") throw std::invalid_argument("Unknown circuit override field");
                if (spec.contains("topology")) {
                    const auto topology = spec.at("topology").get<std::string>();
                    if (topology == circuit::CircuitDefinition::topologyId(circuit::CircuitDefinition::Topology::BassmanStackPair))
                        next = circuit::CircuitDefinition(circuit::CircuitDefinition::Topology::BassmanStackPair);
                    else if (topology != circuit::CircuitDefinition::topologyId(circuit::CircuitDefinition::Topology::MarshallPair))
                        throw std::invalid_argument("Unsupported executable circuit topology");
                }
                auto apply = [&](auto&& self, const json& values, const std::string& prefix) -> void {
                    if (!values.is_object()) throw std::invalid_argument("Circuit values must be an object");
                    for (auto it = values.begin(); it != values.end(); ++it) {
                        const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
                        if (it.value().is_object()) self(self, it.value(), path);
                        else if (path.rfind("control.", 0) == 0 && path != "control.feedback")
                            throw std::invalid_argument("Performance controls belong to amp parameters, not circuit overrides");
                        else if (!it.value().is_number() || !next.setScalar(path, it.value().get<double>(), error))
                            throw std::invalid_argument(error.empty() ? "Circuit value must be numeric" : error);
                    }
                };
                if (spec.contains("values")) apply(apply, spec["values"], "");
            }
            if (!supportsCircuitCore()) throw std::invalid_argument("This amp topology does not support the electrical reference section");
            definition = std::move(next);
        }
        circuit_definition_ = std::move(definition);
        AmpVoice nv = base_voice_;
        model_key_ = applyAmpOverrides(nv, j, real_channels_, &origin_);
        voice_ = nv;
        if (prepared_) {
            trims_ = trimsFor(voice_, model_key_);
            dirty_ = true;
        }
    }

    void onPrepare(double sr, uint32_t /*max_block*/) override {
        os_.prepare(sr >= 88000.0 ? 4 : 8); // 8x at 44.1/48 kHz (≈ 384 kHz internal)
        isr_ = sr * os_.factor();
        prepared_ = true;
        has_spring_ = (schema_.table_id == 30000 || schema_.table_id == 172000);
        if (has_spring_) {
            spring_taps_ = static_cast<std::size_t>(sr * 2.5);
            for (auto& c : spring_) c.setCapacity(static_cast<int>(spring_taps_ / dsp::kConvPart) + 1);
            spring_decay_ = -1.0f;
        }
        vib_.assign(4096, 0.0f);
        trims_ = trimsFor(voice_, model_key_);
        dirty_ = true;
        last_ = AmpControls{};
        last_.channel = -1;
        onProcessControlRate();
        onReset();
    }

    void onReset() override {
        os_.reset();
        // Pre-settle every chain to its quiescent operating point (tube bias offsets through the coupling
        // networks) so a freshly loaded rig starts silently instead of with a DC thump.
        const int settle = static_cast<int>(isr_ * 0.15);
        for (auto& c : chain_) {
            c.reset();
            for (int i = 0; i < settle; ++i) (void)c.process(0.0f);
        }
        for (auto& g : gate_) g.prepare(host_sr_);
        for (auto& c : spring_) c.reset();
        std::fill(vib_.begin(), vib_.end(), 0.0f);
    }

    void onProcessControlRate() override {
        AmpControls ctl = readControls();
        if (ctl == last_ && !dirty_) return;
        const bool rebuild = dirty_ || ctl.channel != last_.channel || ctl.circuit_core != last_.circuit_core;
        last_ = ctl;
        dirty_ = false;
        for (auto& c : chain_) applyControls(c, ctl, rebuild);
        out_gain_ = ctl.level_matching ? (ctl.circuit_core ? outputCompensation(ctl) : trimFor(ctl.channel) * outputCompensation(ctl)) : 1.0f;
        gate_on_ = ctl.input_gate;
        constexpr double scales[] = {0.25, 0.5, 1.0, 2.0, 4.0};
        for (auto& gate : gate_) {
            gate.setGateEnabled(gate_on_);
            gate.setVoltageScale(circuit::VoltageCalibration::assumed(scales[ctl.input_scale]));
        }
        if (has_spring_ && ctl.rev_on && std::abs(ctl.rev_decay - spring_decay_) > 0.01f) {
            spring_decay_ = ctl.rev_decay;
            spring_ir_ = buildSpring(ctl.rev_decay);
        }
        fx_ = ctl;
        if (std::getenv("RIG_AMP_DEBUG")) {
            std::fprintf(stderr, "[amp %d %s] ch=%d gain=%.2f vol=%.2f B=%.2f M=%.2f T=%.2f pres=%.2f depth=%.2f master=%.2f bright=%.2f boost=%.1fdB trim=%.2fdB gate=%d\n",
                         schema_.table_id, std::string(schema_.reference_name).c_str(), ctl.channel, ctl.gain, ctl.volume,
                         ctl.bass, ctl.mid, ctl.treble, ctl.presence, ctl.depth, ctl.master, ctl.bright, ctl.input_boost_db,
                         20.0f * std::log10(trimFor(ctl.channel)), gate_on_ ? 1 : 0);
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        const uint32_t nf = block.num_frames;
        // Equal inputs can follow different stereo histories. Both channels
        // must advance their capacitor, filter, gate and supply states even
        // during a mono/silent segment; copying the left output loses tails.
        const int nch = block.num_channels > 1 ? 2 : 1;
        const int fac = os_.factor();
        float up[8];
        for (int ch = 0; ch < nch; ++ch) {
            const float* in = block.in[ch] ? block.in[ch] : block.in[0];
            float* out = block.out[ch];
            if (!in || !out) continue;
            auto& c = chain_[static_cast<std::size_t>(ch)];
            auto& g = gate_[static_cast<std::size_t>(ch)];
            for (uint32_t i = 0; i < nf; ++i) {
                float x = in[i];
                x = static_cast<float>(g.processVolts(x));
                os_.up(ch, x, up);
                if (c.electrical) {
                    // Keep upstream nonlinear voicings at their existing rate;
                    // solve the coupled section at 88.2/96 kHz, with proper
                    // filtering at both rate boundaries.
                    for (int k = 0; k < fac; ++k) up[k] = c.processFront(up[k]);
                    const int reduction = c.circuit_rate_bridge.factor();
                    for (int k = 0; k < fac; k += reduction) {
                        const float front = c.circuit_rate_bridge.down(0, up + k);
                        const float speaker = static_cast<float>(c.electrical->processPower(c.electrical->processPreamp(front) * c.vol) / 32.0);
                        c.circuit_rate_bridge.up(0, speaker, up + k);
                    }
                } else {
                    for (int k = 0; k < fac; ++k) up[k] = c.process(up[k]);
                }
                out[i] = os_.down(ch, up) * out_gain_;
            }
        }
        if (nch == 1) {
            for (uint32_t c = 1; c < block.num_channels; ++c) {
                if (block.out[c] && block.out[c] != block.out[0]) std::copy(block.out[0], block.out[0] + nf, block.out[c]);
            }
        }
        if (block.num_channels > 1 && block.out[1]) processBuiltInFx(block.out[0], block.out[1], nf);
    }

private:
    // Tremolo / vibrato / chorus, then the spring tank (as on the real amps, the effects follow the preamp).
    void processBuiltInFx(float* L, float* R, uint32_t nf) noexcept {
        const float sr = static_cast<float>(host_sr_);
        if (fx_.mod_mode != 0 && fx_.mod_depth > 0.001f) {
            const float rate = 0.8f + 9.0f * std::pow(std::clamp(fx_.mod_rate, 0.0f, 1.0f), 1.3f);
            const float inc = rate / sr;
            const float depth = std::clamp(fx_.mod_depth, 0.0f, 1.0f);
            const std::size_t vn = vib_.size();
            for (uint32_t i = 0; i < nf; ++i) {
                lfo_ += inc;
                if (lfo_ >= 1.0f) lfo_ -= 1.0f;
                const float s = std::sin(2.0f * amp::kPi * lfo_);
                if (fx_.mod_mode == 1) {
                    // Opto-style tremolo: smoothed, slightly asymmetric throb.
                    const float u = 0.5f + 0.5f * s;
                    const float g = 1.0f - depth * 0.9f * u * std::sqrt(u);
                    L[i] *= g;
                    R[i] *= g;
                } else {
                    const float m = L[i];
                    vib_[vpos_] = m;
                    const float dms = (fx_.mod_mode == 3 ? 7.0f : 2.0f) + depth * (fx_.mod_mode == 3 ? 4.0f : 3.0f) * (0.5f + 0.5f * s);
                    float rp = static_cast<float>(vpos_) - dms * 0.001f * sr;
                    while (rp < 0.0f) rp += static_cast<float>(vn);
                    const auto i0 = static_cast<std::size_t>(rp) % vn;
                    const float fr = rp - std::floor(rp);
                    const float d = vib_[i0] + fr * (vib_[(i0 + 1) % vn] - vib_[i0]);
                    vpos_ = (vpos_ + 1) % vn;
                    if (fx_.mod_mode == 2) {
                        L[i] = d;
                        R[i] = d;
                    } else { // JC-style stereo chorus: dry left, wet right
                        L[i] = m;
                        R[i] = 0.5f * (m + d) * 1.3f;
                    }
                }
            }
        }
        const std::shared_ptr<const dsp::CabinetIR> ir = spring_ir_;
        if (has_spring_ && fx_.rev_on && ir) {
            const float wet = 1.4f * std::clamp(fx_.rev_level, 0.0f, 1.0f);
            for (uint32_t i = 0; i < nf; ++i) {
                const float m = 0.5f * (L[i] + R[i]);
                const float wl = spring_[0].process(m, ir->left);
                const float wr = spring_[1].process(m, ir->right);
                L[i] += wet * wl;
                R[i] += wet * wr;
            }
        }
    }

    std::shared_ptr<const dsp::CabinetIR> buildSpring(float decay) const {
        auto l = dsp::CabinetIRLibrary::load(dsp::CabinetIRLibrary::plateSpring("Amp Spring"), host_sr_, 0, spring_taps_);
        auto r = dsp::CabinetIRLibrary::load(dsp::CabinetIRLibrary::plateSpring("Amp Spring"), host_sr_, 1, spring_taps_);
        if (!l) return nullptr;
        std::vector<float> L = *l, R = r ? *r : *l;
        const std::size_t n = std::min(L.size(), R.size());
        const float tau = static_cast<float>(host_sr_) * (0.2f + 1.6f * decay * decay);
        double e = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const float w = std::exp(-static_cast<float>(i) / tau);
            L[i] *= w;
            R[i] *= w;
            e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
        }
        L.resize(n);
        R.resize(n);
        const float g = (e > 1e-12) ? static_cast<float>(0.55 / std::sqrt(e)) : 1.0f;
        for (auto& v : L) v *= g;
        for (auto& v : R) v *= g;
        auto out = std::make_shared<dsp::CabinetIR>();
        out->left.build(L, spring_taps_);
        out->right.build(R, spring_taps_);
        return out;
    }

    struct Chain {
        int n = 2, pot_after = 1, stack_after = 2;
        std::array<TriodeStage, kMaxStages> st{};
        OnePole in_hp{}, bright{};
        float bright_k = 1.0f;
        float pot = 0.5f, pot_t = 0.5f;
        float vol = 1.0f, vol_t = 1.0f;
        float boost = 1.0f, boost_t = 1.0f;
        float gk = 0.0005f; // per-sample glide coefficient (≈ 10 ms)
        FmvToneStack stack{};
        PowerAmp power{};
        Svf cut_lp{}, lo_cut{};
        std::array<Svf, 9> geq{};
        int geq_n = 0;
        bool use_cut = false, use_locut = false;
        float stack_in = 1.0f, power_in = 1.0f;
        std::unique_ptr<circuit::CircuitAmp> electrical;
        Oversampler circuit_rate_bridge;
        float circuit_sag = -1, circuit_vac = -1, circuit_bias = -1;

        void adoptRuntimeStateFrom(const Chain& previous) noexcept {
            if (n != previous.n || pot_after != previous.pot_after || stack_after != previous.stack_after ||
                static_cast<bool>(electrical) != static_cast<bool>(previous.electrical)) return;
            if (electrical) {
                electrical->adoptAudioStateFrom(*previous.electrical);
                circuit_rate_bridge = previous.circuit_rate_bridge;
            }
            for (int i = 0; i < n; ++i) st[static_cast<std::size_t>(i)].adoptRuntimeStateFrom(previous.st[static_cast<std::size_t>(i)]);
            in_hp.adoptRuntimeStateFrom(previous.in_hp); bright.adoptRuntimeStateFrom(previous.bright);
            stack.adoptRuntimeStateFrom(previous.stack); power.adoptRuntimeStateFrom(previous.power);
            cut_lp.adoptRuntimeStateFrom(previous.cut_lp); lo_cut.adoptRuntimeStateFrom(previous.lo_cut);
            for (int i = 0; i < std::min(geq_n, previous.geq_n); ++i)
                geq[static_cast<std::size_t>(i)].adoptRuntimeStateFrom(previous.geq[static_cast<std::size_t>(i)]);
            pot = previous.pot; vol = previous.vol; boost = previous.boost;
        }
        void reset() {
            if (electrical) electrical->reset();
            circuit_rate_bridge.reset();
            for (auto& s : st) s.reset();
            in_hp.reset();
            bright.reset();
            stack.reset();
            power.reset();
            cut_lp.reset();
            lo_cut.reset();
        }
        float processFront(float x) noexcept {
            pot += (pot_t - pot) * gk;
            vol += (vol_t - vol) * gk;
            boost += (boost_t - boost) * gk;
            x = in_hp.hp(x) * boost;
            if (use_locut) x = lo_cut.process(x);
            if (pot_after == 0) x = applyPot(x);
            for (int i = 0; i < n - 2; ++i) {
                x = st[static_cast<std::size_t>(i)].process(x);
                if (i + 1 == pot_after) x = applyPot(x);
            }
            return x;
        }
        float process(float x) noexcept {
            pot += (pot_t - pot) * gk;
            vol += (vol_t - vol) * gk;
            boost += (boost_t - boost) * gk;
            x = in_hp.hp(x) * boost;
            if (use_locut) x = lo_cut.process(x);
            if (pot_after == 0) x = applyPot(x);
            for (int i = 0; i < n; ++i) {
                x = st[static_cast<std::size_t>(i)].process(x);
                if (i + 1 == pot_after) x = applyPot(x);
                if (i + 1 == stack_after) x = stack.process(x * stack_in);
            }
            if (stack_after > n) x = stack.process(x * stack_in);
            // Shared supply: when the power section sags, the preamp's B+ dips a little too.
            x *= vol * power_in * (1.0f - 0.35f * power.sagEnvelope());
            if (use_cut) x = cut_lp.process(x);
            x = power.process(x);
            for (int i = 0; i < geq_n; ++i) x = geq[static_cast<std::size_t>(i)].process(x);
            return x;
        }
        float applyPot(float x) noexcept {
            const float b = bright.lp(x);
            return (x + (bright_k - 1.0f) * (x - b)) * pot;
        }
    };

    AmpControls readControls() const {
        AmpControls c;
        auto P = [this](std::string_view id, float def = 0.5f) { return getParamOr(id, def); };
        auto on = [&](std::string_view id) { return P(id, 0.0f) >= 0.5f; };
        const int32_t tid = schema_.table_id;
        switch (tid) {
            case 34000: case 92000: {
                const float v1 = P("V1"), v2 = P("V2", 0.0f);
                c.gain = std::max(v1, v2 * 0.92f);
                c.bright = std::clamp(v1 / std::max(0.05f, v1 + v2), 0.0f, 1.0f);
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T"); c.presence = P("Pr");
                break;
            }
            case 91000:
                c.gain = P("V1"); c.master = std::max(0.08f, P("V2", 0.4f) * 1.4f);
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T"); c.presence = P("Pr");
                c.bright = 0.4f;
                break;
            case 56000:
                c.gain = P("V1"); c.master = P("V2");
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T"); c.presence = P("Pr");
                c.input_boost_db = on("Hg") ? 6.0f : 0.0f;
                c.bright = 1.0f;
                break;
            case 101000:
                c.gain = P("V1"); c.master = P("V2");
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T"); c.presence = P("Pr");
                c.channel = on("Hg") ? 1 : 0;
                c.bright = 1.0f;
                break;
            case 134000: {
                const bool lead = on("L13");
                c.channel = lead ? 2 : (on("C16") ? 1 : 0);
                c.gain = lead ? P("L1") : P("R0");
                c.bright = (!lead && on("B15")) ? 1.0f : 0.0f;
                c.input_boost_db = (!lead && on("H14")) ? 5.0f : 0.0f;
                c.bass = P("B2"); c.mid = P("M3"); c.treble = P("T4");
                c.master = P("P5"); c.depth = P("R6"); c.presence = P("P7");
                break;
            }
            case 122000: {
                const bool od = on("O2");
                c.channel = od ? 2 : 0;
                c.gain = od ? P("P1") : P("P0");
                c.bass = P("B3"); c.mid = P("M4"); c.treble = P("T5");
                c.master = P("M6"); c.presence = P("P7"); c.depth = P("D8");
                c.bright = od ? 0.3f : 1.0f;
                break;
            }
            case 163000: {
                const bool od = on("channel");
                c.channel = od ? 2 : (on("normalMode") ? 1 : 0);
                c.gain = od ? P("overdrivePreamp") : P("normalPreamp");
                c.bright = on("bright") ? 1.0f : 0.0f;
                c.bass = P("bass"); c.mid = P("middle"); c.treble = P("treble");
                c.master = P("master"); c.presence = P("presence"); c.depth = P("depth");
                break;
            }
            case 81000:
                c.channel = on("c") ? 1 : 0;
                c.gain = P("g"); c.volume = 0.35f + 1.3f * P("v");
                c.bass = P("b"); c.mid = P("m"); c.treble = P("t"); c.presence = P("p");
                c.master = P("Mst");
                break;
            case 16000: case 96000: {
                const float mraw = P("M", 3.0f);
                c.channel = std::clamp(static_cast<int>(std::lround(mraw)), 0, 3);
                c.gain = P("Gn"); c.master = P("Mast");
                c.bass = P("Bs"); c.mid = P("Mid"); c.treble = P("Trb"); c.presence = P("Prs");
                break;
            }
            case 80000:
                c.gain = P("g"); c.master = P("m");
                c.bass = P("b"); c.mid = 0.62f; c.treble = P("t"); c.presence = P("p");
                c.lo_cut = P("f", 0.0f);
                break;
            case 87000: {
                const float nv = P("nv", 0.0f), bv = P("bv", 0.0f);
                c.gain = std::max(nv, bv);
                c.bright = std::clamp(bv / std::max(0.05f, nv + bv), 0.0f, 1.0f);
                c.bass = P("bs"); c.mid = P("md"); c.treble = P("tr"); c.presence = P("pr");
                c.master = P("Mst");
                break;
            }
            case 30000:
                c.gain = P("Vol"); c.bright = on("Br") ? 1.0f : 0.0f;
                c.bass = P("Bs"); c.mid = P("Md"); c.treble = P("Tr");
                c.rev_on = on("RvOn"); c.rev_level = P("RvL", 0.3f); c.rev_decay = 0.3f + 0.7f * P("RvTm", 0.5f);
                c.mod_mode = on("ViOn") ? 1 : 0; c.mod_rate = P("ViSpd"); c.mod_depth = P("ViInt");
                break;
            case 172000:
                c.gain = P("volume"); c.bright = on("bright") ? 1.0f : 0.0f;
                c.bass = P("bass"); c.mid = 0.5f; c.treble = P("treble");
                c.rev_on = on("reverb on"); c.rev_level = P("reverb intensity", 0.3f);
                c.rev_decay = on("reverb mode") ? 1.0f : 0.7f;
                c.mod_mode = on("tremolo on") ? 1 : 0; c.mod_rate = P("speed"); c.mod_depth = P("intensity");
                break;
            case 62000: {
                const float vb = P("VB", 0.0f), vn = P("VN", 0.0f);
                c.gain = std::max(vb, vn);
                c.bright = std::clamp(vb / std::max(0.05f, vb + vn), 0.0f, 1.0f);
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T"); c.presence = P("Pr");
                break;
            }
            case 79000: {
                const float vb = P("vb", 0.0f), vn = P("vn", 0.0f);
                c.gain = std::max(vb, vn);
                c.bright = std::clamp(vb / std::max(0.05f, vb + vn), 0.0f, 1.0f);
                c.treble = P("t"); c.bass = 0.6f; c.mid = 0.8f;
                break;
            }
            case 152000:
                c.gain = P("gain"); c.treble = P("tone"); c.bass = 0.6f; c.mid = 0.7f;
                c.bright = on("bright") ? 1.0f : 0.0f;
                c.mod_depth = P("mod", 0.0f); c.mod_mode = c.mod_depth > 0.01f ? 1 : 0; c.mod_rate = 0.45f;
                break;
            case 38000:
                c.gain = std::max(P("Vol", 0.0f), P("Br", 0.0f));
                c.bright = std::clamp(P("Br", 0.0f) / std::max(0.05f, P("Vol", 0.0f) + P("Br", 0.0f)), 0.0f, 1.0f);
                c.treble = P("Tb"); c.bass = P("Bs"); c.mid = 0.5f;
                c.cut = P("Tc", 0.0f);
                c.mod_depth = P("TDt", 0.0f); c.mod_mode = c.mod_depth > 0.01f ? 1 : 0; c.mod_rate = P("TSp");
                break;
            case 176000:
                c.gain = P("volume"); c.cut = P("cut", 0.0f);
                c.bass = 0.5f; c.mid = 0.5f;
                c.treble = 0.3f + 0.6f * P("brilliance");
                c.bright = P("brilliance");
                {
                    const int fx = static_cast<int>(std::lround(P("effectsSelector", 0.0f)));
                    c.mod_mode = (fx == 1) ? 1 : (fx == 2 ? 2 : 0);
                    c.mod_rate = (fx == 2) ? P("vibratoSpeed") : P("tremoloSpeed");
                    c.mod_depth = P("depth", 0.0f);
                }
                break;
            case 151000: case 162000: case 300000: {
                const float gm = P("gainMode", 1.0f);
                c.channel = std::clamp(static_cast<int>(std::lround(gm)), 0, 2);
                c.gain = P("gain"); c.master = P("master");
                c.bass = P("low"); c.mid = P("mid"); c.treble = P("treble");
                c.presence = 0.45f; c.depth = 0.45f;
                break;
            }
            case 49000:
                c.gain = P("V"); c.bass = P("a"); c.mid = P("M"); c.treble = P("T");
                c.bright = on("B") ? 1.0f : 0.0f;
                {
                    const int fx = static_cast<int>(std::lround(P("em", 0.0f)));
                    c.mod_mode = (fx == 1) ? 3 : (fx == 2 ? 2 : 0);
                    c.mod_rate = P("R"); c.mod_depth = P("d");
                }
                break;
            case 54000:
                c.gain = P("G"); c.master = P("V");
                c.bass = P("B"); c.mid = P("M"); c.treble = P("T");
                c.input_boost_db = 6.0f * P("D", 0.0f);
                c.bright = on("br") ? 1.0f : 0.0f;
                c.depth = 0.35f + 0.5f * (on("uL") ? 1.0f : 0.0f);
                c.presence = 0.35f + 0.5f * (on("uH") ? 1.0f : 0.0f);
                c.geq_on = on("gE");
                if (c.geq_on) {
                    static constexpr const char* kIds[9] = {"40", "90", "180", "300", "500", "1", "2", "4", "10"};
                    for (int i = 0; i < 9; ++i) c.geq[static_cast<std::size_t>(i)] = (P(kIds[i]) - 0.5f) * 24.0f;
                }
                break;
            case 177000:
                c.gain = P("gain"); c.master = P("volume");
                c.bass = P("bass"); c.mid = P("midrange"); c.treble = P("treble");
                c.depth = 0.35f + 0.5f * (on("ultra low") ? 1.0f : 0.0f);
                c.presence = 0.35f + 0.5f * (on("ultra high") ? 1.0f : 0.0f);
                break;
            case 150000:
                c.gain = P("volume"); c.master = P("master");
                c.bass = P("bass"); c.mid = 0.5f * (P("loMid") + P("hiMid")); c.treble = P("treble");
                c.input_boost_db = on("boost") ? 6.0f : 0.0f;
                c.lo_cut = on("loCut") ? 0.5f : 0.0f;
                c.presence = 0.35f + 0.5f * (on("hiBoost") ? 1.0f : 0.0f);
                break;
            default:
                c.gain = getNormalizedParam({"Gain", "Preamp", "Drive", "Volume", "gain"}, 0.5f);
                c.bass = getNormalizedParam({"Bass", "Low", "bass"}, 0.5f);
                c.mid = getNormalizedParam({"Mid", "Middle", "mid"}, 0.5f);
                c.treble = getNormalizedParam({"Treble", "treble"}, 0.5f);
                c.presence = getNormalizedParam({"Presence", "presence"}, 0.35f);
                c.master = getNormalizedParam({"Master", "master"}, 0.5f);
                break;
        }
        // Shared power-section controls present on most legacy amps.
        c.sag = getParamOr(tid == 134000 ? "S10" : (tid == 122000 ? "S11" : "Sg"), 0.5f);
        c.vac = getParamOr(tid == 134000 ? "V9" : (tid == 122000 ? "V10" : (tid == 30000 || tid == 38000 || tid == 92000 ? "Vr" : "Var")), 0.5f);
        if (tid == 92000) c.vac = getParamOr("Var", 0.5f);
        c.bias = getParamOr(tid == 134000 ? "B12" : (tid == 122000 ? "B13" : (tid == 30000 || tid == 38000 ? "Bia" : (tid == 16000 || tid == 96000 ? "Bias" : "Bi"))), 0.5f);
        if (!voice_.has_master) c.master = voice_.fixed_master;
        c.channel = std::clamp(c.channel, 0, 3);
        const auto& cv = voice_.ch[static_cast<std::size_t>(c.channel)];
        c.circuit_core = supportsCircuitCore() && (getParamOr("amp_core", 0) >= 0.5f || circuit_definition_.has_value());
        if (c.circuit_core && !(cv.n >= 2 && cv.st[static_cast<std::size_t>(cv.n-1)].lim_neg >= 2.4f &&
                                cv.pot_after <= cv.n - 2 && cv.stack_after >= cv.n))
            throw std::invalid_argument("Electrical reference requires the supported terminal triode/follower and tone-stack topology");
        c.input_gate = getParamOr("input_gate", 0) >= 0.5f;
        c.level_matching = getParamOr("level_matching", 1) >= 0.5f;
        const float scale = getParamOr("input_voltage_scale", 2);
        c.input_scale = std::isfinite(scale) ? static_cast<int>(std::clamp(std::round(scale), 0.0f, 4.0f)) : 2;
        return c;
    }

    void applyControls(Chain& c, const AmpControls& ctl, bool rebuild, const AmpVoice* vp = nullptr) const {
        const AmpVoice& voice = vp ? *vp : voice_;
        const ChannelVoice& cv = voice.ch[static_cast<std::size_t>(ctl.channel)];
        if (rebuild) {
            c.n = cv.n;
            c.pot_after = std::min(cv.pot_after, cv.n);
            c.stack_after = std::min(cv.stack_after, cv.n + 1);
            for (int i = 0; i < cv.n; ++i) c.st[static_cast<std::size_t>(i)].prepare(isr_, cv.st[static_cast<std::size_t>(i)]);
            c.in_hp.set(isr_, cv.input_hp);
            c.bright.set(isr_, cv.bright_hz);
            c.stack.prepare(isr_, voice.stack);
            c.power.prepare(isr_, voice.power);
            c.stack_in = voice.stack_in;
            c.power_in = voice.power_in;
            c.electrical.reset();
            if (ctl.circuit_core) {
                auto next = std::make_unique<circuit::CircuitAmp>();
                const int reduction = host_sr_ < 88000 ? 4 : os_.factor();
                c.circuit_rate_bridge.prepare(reduction);
                std::string error;
                if (!next->prepare(isr_ / reduction, error)) throw std::runtime_error(error);
                if (circuit_definition_ && !next->configure(*circuit_definition_, error)) throw std::runtime_error(error);
                c.electrical = std::move(next);
                c.circuit_sag = c.circuit_vac = c.circuit_bias = -1;
            }
        }
        c.pot_t = std::max(cv.pot_floor, amp::audioTaper(ctl.gain));
        c.gk = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(isr_)));
        // Bright cap: strongest at low gain settings, fades out as the pot opens up.
        c.bright_k = dbToLin(cv.bright_db * ctl.bright * std::pow(1.0f - std::clamp(ctl.gain, 0.0f, 1.0f), 1.2f) +
                             0.25f * cv.bright_db * ctl.bright);
        c.boost_t = dbToLin(ctl.input_boost_db);
        c.vol_t = ctl.volume;
        if (rebuild) {
            c.pot = c.pot_t;
            c.boost = c.boost_t;
            c.vol = c.vol_t;
            c.stack.update(ctl.bass, ctl.mid, ctl.treble);
        } else {
            c.stack.setTarget(ctl.bass, ctl.mid, ctl.treble);
        }
        c.power.setControls(ctl.master, ctl.presence, ctl.depth, ctl.sag, ctl.vac, ctl.bias);
        if (c.electrical) {
            if (c.circuit_sag != ctl.sag || c.circuit_vac != ctl.vac || c.circuit_bias != ctl.bias) {
                auto definition = circuit_definition_.value_or(circuit::CircuitDefinition{});
                const auto base = definition.applied();
                std::string error;
                // Native knobs alter actual electrical values; midpoint retains
                // the editable component definition. Bounds remain validated.
                const bool valid = definition.setScalar("power.sourceR", base.power.reservoirResistanceOhms * (0.5 + ctl.sag), error) &&
                                   definition.setScalar("power.sourceV", base.power.reservoirSourceVolts * (0.8 + 0.4 * ctl.vac), error) &&
                                   definition.setScalar("power.biasV", base.power.outputBiasVolts * (1.5 - ctl.bias), error);
                if (!valid) throw std::invalid_argument(error);
                const auto previous = *c.electrical;
                if (!c.electrical->configure(definition, error)) throw std::invalid_argument(error);
                c.electrical->adoptAudioStateFrom(previous);
                c.circuit_sag = ctl.sag; c.circuit_vac = ctl.vac; c.circuit_bias = ctl.bias;
            }
            c.electrical->setPerformanceControls(amp::audioTaper(ctl.master), ctl.bass, ctl.mid, ctl.treble,
                                                  c.electrical->definition().applied().feedback, ctl.presence, ctl.depth);
        }
        c.use_cut = ctl.cut > 0.01f;
        if (c.use_cut) c.cut_lp.set(Svf::Type::LowPass, isr_, 16000.0f * std::pow(0.12f, ctl.cut), 0.6f);
        c.geq_n = 0;
        if (ctl.geq_on) {
            static constexpr float kHz[9] = {40, 90, 180, 300, 500, 1000, 2000, 4000, 10000};
            for (int i = 0; i < 9; ++i) c.geq[static_cast<std::size_t>(i)].set(Svf::Type::Bell, isr_, kHz[i], 1.1f, ctl.geq[static_cast<std::size_t>(i)]);
            c.geq_n = 9;
        }
        c.use_locut = ctl.lo_cut > 0.01f;
        if (c.use_locut) c.lo_cut.set(Svf::Type::HighPass, isr_, 40.0f + 260.0f * ctl.lo_cut, 0.6f);
    }

    // Keep master moves musical: turning master up gets louder, but not by the full drive change.
    // High-headroom amps (Fender cleans, Hiwatt, JC, bass heads) barely saturate, so their volume/gain
    // knob would otherwise swing the level by 30 dB; partially compensate so cranking them is usable.
    float outputCompensation(const AmpControls& ctl) const {
        float db = 0.0f;
        if (voice_.has_master) db -= (ctl.master - 0.5f) * 6.0f;
        switch (schema_.table_id) {
            case 30000: case 172000: case 49000: case 54000: case 177000: case 150000: case 87000:
                db -= (ctl.gain - 0.6f) * 16.0f;
                break;
            default: break;
        }
        return dbToLin(db);
    }

    // Static per-amp output trims: render the calibration DI at reference settings for every channel
    // and normalise to ≈ -18 dBFS RMS so amps swap at comparable levels.
    // Output trims depend only on (amp, sample rate, oversampling, the engine's level version): computed once and
    // persisted, so loading a rig never re-renders calibration audio for its amps. A rebuild alone never invalidates
    // them (core/EngineFingerprint.hpp: kLevelVersion is bumped by hand when amp levels change on purpose).
    static std::string trimCachePath(bool custom = false) {
        // "-exact": written at max_digits10. "-lvl": every line starts with the level tag (readTrimCache); a new file, so
        // builds that keyed the older "-fp" file by the build fingerprint never see (or compact away) these lines.
        return platform::appDataDir() + "/amp-trims-" + (custom ? "custom-" : "") + kEngineDspVersion + "-exact-lvl.txt";
    }
    static const std::string& trimLevelTag() {
        static const std::string tag = std::string(kLevelVersion) + "/" + kEngineDspVersion;
        return tag;
    }
    // Each line starts with the level tag: a trim measured under another level version is dead. Lines of `fields`
    // numbers after it; the file is compacted once dead lines pile up. Returns the live lines' fields.
    static std::vector<std::vector<std::string>> readTrimCache(bool custom, std::size_t fields) {
        const std::string& fp = trimLevelTag();
        std::vector<std::vector<std::string>> live;
        std::vector<std::string> keep;
        std::size_t dead = 0;
        {
            std::ifstream f(trimCachePath(custom));
            std::string line;
            while (std::getline(f, line)) {
                std::istringstream ls(line);
                std::string tag, tok;
                std::vector<std::string> v;
                ls >> tag;
                while (ls >> tok) v.push_back(tok);
                if (tag != fp || v.size() != fields) { ++dead; continue; }
                live.push_back(std::move(v));
                keep.push_back(line);
            }
        }
        if (dead >= 256) {
            std::ofstream f(trimCachePath(custom), std::ios::trunc);
            for (const auto& l : keep) f << l << "\n";
        }
        return live;
    }
    // Level trims for a voicing. Stock voicings use the persistent per-amp cache; user-modified ones are
    // keyed by a hash of their overrides (own cache file), so a custom amp levels like a factory one.
    std::array<float, 4> trimsFor(const AmpVoice& v, const std::string& model_key) const {
        if (analysis_) return {1, 1, 1, 1};
        if (!model_key.empty()) return customTrims(v, model_key);
        static std::mutex mtx;
        static std::map<std::tuple<int32_t, int, int>, std::array<float, 4>> cache;
        static bool loaded = false;
        const auto key = std::make_tuple(schema_.table_id, static_cast<int>(host_sr_), os_.factor());
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!loaded) {
                loaded = true;
                for (const auto& v : readTrimCache(false, 7)) {
                    try {
                        cache[std::make_tuple(static_cast<int32_t>(std::stol(v[0])), std::stoi(v[1]), std::stoi(v[2]))] =
                            {std::stof(v[3]), std::stof(v[4]), std::stof(v[5]), std::stof(v[6])};
                    } catch (...) {}
                }
            }
            if (auto it = cache.find(key); it != cache.end()) return it->second;
        }
        const auto trims = renderTrims(v);
        std::lock_guard<std::mutex> lk(mtx);
        cache[key] = trims;
        std::ofstream f(trimCachePath(), std::ios::app);
        f << trimLevelTag() << " " << std::setprecision(std::numeric_limits<float>::max_digits10) << schema_.table_id << " " << static_cast<int>(host_sr_) << " " << os_.factor() << " " << trims[0] << " " << trims[1] << " " << trims[2] << " " << trims[3] << "\n";
        return trims;
    }
    std::array<float, 4> customTrims(const AmpVoice& v, const std::string& model_key) const {
        static std::mutex mtx;
        static std::map<std::tuple<uint64_t, int32_t, int, int>, std::array<float, 4>> cache;
        static bool loaded = false;
        const uint64_t h = fnv1a(model_key);
        const auto key = std::make_tuple(h, schema_.table_id, static_cast<int>(host_sr_), os_.factor());
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!loaded) {
                loaded = true;
                for (const auto& v : readTrimCache(true, 8)) {
                    try {
                        cache[std::make_tuple(static_cast<uint64_t>(std::stoull(v[0])), static_cast<int32_t>(std::stol(v[1])), std::stoi(v[2]), std::stoi(v[3]))] =
                            {std::stof(v[4]), std::stof(v[5]), std::stof(v[6]), std::stof(v[7])};
                    } catch (...) {}
                }
            }
            if (auto it = cache.find(key); it != cache.end()) return it->second;
        }
        const auto trims = renderTrims(v);
        std::lock_guard<std::mutex> lk(mtx);
        cache[key] = trims;
        std::ofstream f(trimCachePath(true), std::ios::app);
        f << trimLevelTag() << " " << std::setprecision(std::numeric_limits<float>::max_digits10) << h << " " << schema_.table_id << " " << static_cast<int>(host_sr_) << " " << os_.factor() << " " << trims[0] << " " << trims[1] << " " << trims[2] << " " << trims[3] << "\n";
        return trims;
    }
    std::array<float, 4> renderTrims(const AmpVoice& voice) const {
        const auto sig = amp::makeCalibrationDI(host_sr_, 3.0f);
        std::array<float, 4> trims{1, 1, 1, 1};
        for (int ch = 0; ch < 4; ++ch) {
            bool dup = false;
            for (int e = 0; e < ch && !dup; ++e) {
                if (std::memcmp(&voice.ch[static_cast<std::size_t>(e)], &voice.ch[static_cast<std::size_t>(ch)], sizeof(ChannelVoice)) == 0) {
                    trims[static_cast<std::size_t>(ch)] = trims[static_cast<std::size_t>(e)];
                    dup = true;
                }
            }
            if (dup) continue;
            AmpControls ref;
            ref.channel = ch;
            ref.gain = 0.6f;
            ref.master = voice.has_master ? 0.5f : voice.fixed_master;
            Chain c;
            applyControls(c, ref, true, &voice);
            c.reset();
            Oversampler os;
            os.prepare(os_.factor());
            float buf[8];
            double e = 0.0;
            std::size_t cnt = 0;
            for (std::size_t i = 0; i < sig.size(); ++i) {
                os.up(0, sig[i], buf);
                for (int k = 0; k < os.factor(); ++k) buf[k] = c.process(buf[k]);
                const float y = os.down(0, buf);
                if (i > sig.size() / 10) {
                    e += static_cast<double>(y) * y;
                    ++cnt;
                }
            }
            const float rms = static_cast<float>(std::sqrt(e / std::max<std::size_t>(1, cnt)));
            trims[static_cast<std::size_t>(ch)] = std::clamp(0.125f / std::max(1e-5f, rms), 0.02f, 50.0f);
        }
        return trims;
    }

    float trimFor(int ch) const { return trims_[static_cast<std::size_t>(std::clamp(ch, 0, 3))]; }

    // Soft downward expander ahead of the high-gain preamp: keeps the DI's hiss floor from being
    // amplified by 60+ dB between phrases, while leaving note decays above ~-62 dBFS untouched.
    AmpVoice base_voice_;
    AmpVoice voice_;
    StageOrigin origin_ = identityOrigin();
    int real_channels_ = 1;
    std::string model_key_;
    std::optional<circuit::CircuitDefinition> circuit_definition_;
    bool analysis_ = false;
    bool prepared_ = false;
    AmpControls fx_{};
    bool has_spring_ = false;
    std::size_t spring_taps_ = 0;
    float spring_decay_ = -1.0f;
    std::array<dsp::PartitionedConvolver, 2> spring_{dsp::PartitionedConvolver(1), dsp::PartitionedConvolver(1)};
    std::shared_ptr<const dsp::CabinetIR> spring_ir_{};
    std::vector<float> vib_{};
    std::size_t vpos_ = 0;
    float lfo_ = 0.0f;
    std::array<amp::AmpInputUtilities, 2> gate_{};
    bool gate_on_ = false;
    Oversampler os_{};
    double isr_ = 192000.0;
    std::array<Chain, 2> chain_{};
    SpeakerImpedanceProfile load_{}; // cabinet currently loading the power amp (see setSpeakerLoadProfile)
    AmpControls last_{};
    bool dirty_ = true;
    float out_gain_ = 1.0f;
    std::array<float, 4> trims_{1, 1, 1, 1};
};

// ───────────────────────────── Amp model export ─────────────────────────────
std::string stageLabel(int i) { return "V" + std::to_string(i / 2 + 1) + (i % 2 ? "B" : "A"); }

std::string TubeAmpComponent::modelJson() const {
    const AmpControls ctl = readControls();
    const int32_t tid = schema_.table_id;
    const AmpVoice& V = voice_;
    const std::string chp = "amp.channels.";
    json m = json::object();
    m["kind"] = "amp";
    m["solid_state"] = V.solid_state;
    m["bass_amp"] = V.bass_amp;
    m["has_master"] = V.has_master;
    m["fixed_master"] = V.fixed_master;
    m["oversampling"] = os_.factor();
    m["internal_rate"] = isr_;
    m["active_channel"] = ctl.channel;
    m["speaker_load"] = {{"from_cabinet", load_.valid}, {"fs_hz", chain_[0].power.effectiveSpeakerFs()},
                         {"q", load_.valid ? load_.q_res : 1.4f}, {"le_scale", load_.valid ? load_.le_scale : 1.0f}};
    m["custom"] = !model_key_.empty() || circuit_definition_.has_value();
    m["controls"] = {{"channel", ctl.channel}, {"gain", ctl.gain}, {"volume", ctl.volume}, {"bass", ctl.bass}, {"mid", ctl.mid}, {"treble", ctl.treble},
                     {"presence", ctl.presence}, {"depth", ctl.depth}, {"master", ctl.master}, {"sag", ctl.sag}, {"vac", ctl.vac}, {"bias", ctl.bias},
                     {"bright", ctl.bright}, {"input_boost_db", ctl.input_boost_db}, {"cut", ctl.cut}, {"lo_cut", ctl.lo_cut},
                     {"reverb_on", ctl.rev_on}, {"reverb_level", ctl.rev_level}, {"mod_mode", ctl.mod_mode}, {"mod_rate", ctl.mod_rate}, {"mod_depth", ctl.mod_depth}};
    m["trims_db"] = {linToDb(trims_[0]), linToDb(trims_[1]), linToDb(trims_[2]), linToDb(trims_[3])};
    m["output_comp_db"] = ctl.level_matching ? linToDb(outputCompensation(ctl)) : 0.0;
    constexpr double input_scales[] = {0.25, 0.5, 1, 2, 4};
    m["utilities"] = {{"input_gate", ctl.input_gate}, {"level_matching", ctl.level_matching},
                       {"volts_per_full_scale", input_scales[ctl.input_scale]}, {"calibration", "assumed"}};

    if (ctl.circuit_core && chain_[0].electrical) {
        const auto& engine = *chain_[0].electrical;
        const auto& definition = engine.definition();
        const auto editableDefinition = circuit_definition_.value_or(circuit::CircuitDefinition{});
        m["backend"] = "hybrid-circuit";
        m["verification"] = "circuit-reference";
        m["verification_scope"] = "Subcircuit equation checks and sample-rate convergence; no hardware verification";
        m["netlist"] = definition.connectivity();
        m["circuit"] = definition.editorSchema();
        // These are component values before the native Sag/Variac/Bias scaling.
        // Exposing effective values as edit inputs would compound a knob's
        // multiplier every time the user reapplied the displayed value.
        m["circuit"]["editable_components"] = editableDefinition.toJson()["components"];
        for (auto& parameter : m["circuit"]["parameters"]) {
            const auto id = parameter["id"].get<std::string>();
            if (id.rfind("control.", 0) == 0 && id != "control.feedback") {
                parameter["editable"] = false;
                parameter["controlled_by"] = "amp parameter";
            }
        }
        m["capabilities"] = definition.editorSchema()["capabilities"];
        m["circuit_sample_rate"] = isr_ / chain_[0].circuit_rate_bridge.factor();
        m["output_reference"] = "32 speaker volts per digital full scale; assumed output reference";
        m["description"] = "Upstream amp voicing stages feed a solved paired-triode/follower/tone circuit and reduced push-pull power circuit. Shared supply currents and voltages are modeled. This is a hybrid reference topology, not an exact recreation of the named hardware.";
        json blocks = json::array();
        const int front = std::max(0, chain_[0].n - 2);
        for (int i = 0; i < front; ++i) {
            auto b = block("front" + std::to_string(i), "triode", "Upstream stage " + std::to_string(i + 1),
                           "behavioral triode stage", json::array(), "Preamp");
            b["fidelity"] = "behavioural";
            blocks.push_back(std::move(b));
        }
        for (const char* section : {"preamp", "power", "master", "interface", "load"}) {
            json values = json::array();
            for (const auto& parameter : editableDefinition.scalars()) {
                if (parameter.id.rfind(std::string(section) + ".", 0) != 0 &&
                    !(std::string(section) == "power" && parameter.id == "control.feedback")) continue;
                double multiplier = 1;
                if (parameter.id == "power.sourceR") multiplier = 0.5 + ctl.sag;
                if (parameter.id == "power.sourceV") multiplier = 0.8 + 0.4 * ctl.vac;
                if (parameter.id == "power.biasV") multiplier = 1.5 - ctl.bias;
                // Base edits must remain valid both before and after native
                // control scaling. Keep the editor's bounds consistent with
                // the same validation that candidate preparation applies.
                const double editMin = std::max(parameter.minimum, parameter.minimum / multiplier);
                const double editMax = std::min(parameter.maximum, parameter.maximum / multiplier);
                auto edit = editNum("circuit.values." + parameter.id, editMin, editMax,
                                    std::max(1e-12, std::abs(parameter.value) * 0.01), editMin > 0);
                edit["unit"] = parameter.unit;
                edit["source"] = "circuit-definition";
                std::string note = parameter.provenance;
                if (parameter.id == "power.sourceR" || parameter.id == "power.sourceV" || parameter.id == "power.biasV") {
                    const double effective = m["circuit"]["components"][parameter.id].get<double>();
                    note += "; base component value; effective with amp controls: " + fmtNum(effective, 8) + " " + parameter.unit;
                }
                values.push_back(mval(parameter.label, fmtNum(parameter.value, 8) + " " + parameter.unit,
                                      parameter.value, note, edit));
            }
            auto b = block(std::string("circuit-") + section, "circuit", section, "electrical reference", values, "Circuit");
            b["fidelity"] = "circuit-solved";
            blocks.push_back(std::move(b));
        }
        blocks.push_back(block("out", "out", "Output reference", "32 V / FS", json::array(), "Output"));
        m["blocks"] = std::move(blocks);
        return m.dump();
    }

    // Channels: each reachable channel's preamp, with the operating point every stage settles to.
    json chans = json::array();
    for (const auto& [k, name] : ampChannels(tid)) {
        const ChannelVoice& cv = V.ch[static_cast<std::size_t>(k)];
        AmpControls c2 = ctl;
        c2.channel = k;
        Chain c;
        applyControls(c, c2, true);
        json stages = json::array();
        const auto& org = origin_[static_cast<std::size_t>(k)];
        for (int i = 0; i < cv.n; ++i) {
            const auto& ts = c.st[static_cast<std::size_t>(i)];
            const StageSpec& sp = ts.spec();
            const int oi = org[static_cast<std::size_t>(i)];
            const std::string role = ts.isAnalytic() ? (V.solid_state ? "solid_state" : "cathode_follower") : (ts.isDynamic() ? "triode" : "cold_clipper");
            json st = {{"index", i}, {"orig", oi}, {"label", stageLabel(oi)}, {"role", role},
                       {"spec", {{"gain", sp.gain}, {"hp_hz", sp.hp_hz}, {"lp_hz", sp.lp_hz}, {"bias", sp.bias}, {"cath_hz", sp.cath_hz}, {"cath_db", sp.cath_db},
                                 {"lim_pos", sp.lim_pos}, {"lim_neg", sp.lim_neg}, {"blocking", sp.blocking}}}};
            if (ts.isDynamic()) {
                st["curve"] = (ts.curveVariant() == 0) ? "12AX7 · Koren · hot bias (Rk 1.5 kΩ)" : "12AX7 · Koren · warm bias (Rk 2.7 kΩ)";
                st["rk"] = ts.cathodeR();
                st["ck"] = ts.cathodeC();
                st["cath_corner_hz"] = 1.0 / (2.0 * std::numbers::pi * ts.cathodeR() * ts.cathodeC());
                st["vk0"] = ts.quiescentVk();
                st["ip0_ma"] = ts.quiescentIp() * 1000.0;
            } else if (!ts.isAnalytic()) {
                st["curve"] = "12AX7 · Koren · cold clipper (Rk 10 kΩ unbypassed)";
                st["rk"] = 10000.0;
            } else {
                st["curve"] = V.solid_state ? "Analytic soft clipper (solid-state stage)" : "Analytic follower clipper (clips on the positive swing)";
            }
            stages.push_back(std::move(st));
        }
        json removed = json::array();
        const ChannelVoice& bv = base_voice_.ch[static_cast<std::size_t>(k)];
        for (int oi = 0; oi < bv.n; ++oi) {
            bool present = false;
            for (int i = 0; i < cv.n; ++i) present |= org[static_cast<std::size_t>(i)] == oi;
            if (!present) removed.push_back({{"orig", oi}, {"label", stageLabel(oi)}});
        }
        chans.push_back({{"index", k}, {"name", name}, {"n", cv.n}, {"removed", std::move(removed)}, {"pot_after", std::min(cv.pot_after, cv.n)},
                         {"stack_after", std::min(cv.stack_after, cv.n + 1)}, {"bright_db", cv.bright_db}, {"bright_hz", cv.bright_hz},
                         {"input_hp", cv.input_hp}, {"pot_floor", cv.pot_floor}, {"gate", ctl.input_gate}, {"stages", std::move(stages)}});
    }
    m["channels"] = std::move(chans);

    const auto& tv = V.stack;
    m["tone_stack"] = {{"topology", "FMV passive (Yeh & Smith analytic 3rd-order)"}, {"preset", stackPresetId(tv)},
                       {"values", {{"C1", tv.C1}, {"C2", tv.C2}, {"C3", tv.C3}, {"R1", tv.R1}, {"R2", tv.R2}, {"R3", tv.R3}, {"R4", tv.R4}}},
                       {"stack_in", V.stack_in}, {"tapers", {{"bass", "audio"}, {"mid", "linear"}, {"treble", "60 % linear + 40 % audio"}}}};
    json presets = json::array();
    for (const auto& p : stackPresets()) presets.push_back({{"id", p.id}, {"label", p.label}});
    m["tone_stack_presets"] = std::move(presets);

    const PowerSpec& ps = V.power;
    const auto d = PowerAmp::derive(ps, ctl.master, ctl.presence, ctl.depth, ctl.sag, ctl.vac, ctl.bias);
    const float beta = PowerAmp::feedbackBeta(ps);
    const float und = 1.0f - std::clamp(ps.damping, 0.0f, 1.0f);
    m["power"] = {{"spec", {{"drive", ps.drive}, {"sag", ps.sag}, {"asym", ps.asym}, {"knee", ps.knee}, {"presence_hz", ps.presence_hz}, {"presence_db", ps.presence_db},
                            {"depth_hz", ps.depth_hz}, {"depth_db", ps.depth_db}, {"ot_hp_hz", ps.ot_hp_hz}, {"ot_lp_hz", ps.ot_lp_hz}, {"class_a", ps.class_a},
                            {"damping", ps.damping}, {"spk_fs", ps.spk_fs}}},
                  {"nfb_beta", beta}, {"power_in", V.power_in},
                  {"derived", {{"drive", d.drive}, {"sag", d.sag}, {"headroom", d.headroom}, {"asym", d.asym}, {"presence_db", d.presence_db}, {"depth_db", d.depth_db}}},
                  {"speaker", {{"fs_hz", ps.spk_fs}, {"resonance_db", 1.0f + 7.0f * und}, {"hf_rise_db", 0.5f + 4.5f * und}}}};
    m["power_profiles"] = powerProfiles();
    m["builtin"] = {{"spring", has_spring_}, {"tremolo", tid == 30000 || tid == 172000 || tid == 152000 || tid == 38000 || tid == 176000},
                    {"vibrato", tid == 176000 || tid == 49000}, {"chorus", tid == 49000}};

    // ── Signal chain of the active channel, as the Chain processes it ──
    const int ch = ctl.channel;
    const ChannelVoice& cv = V.ch[static_cast<std::size_t>(ch)];
    Chain c;
    applyControls(c, ctl, true);
    const std::string cpath = chp + std::to_string(ch);
    const auto& org = origin_[static_cast<std::size_t>(ch)];
    json blocks = json::array();
    {
        json v = json::array();
        v.push_back(mval("Input coupling HP", fmtHz(cv.input_hp), cv.input_hp, "1-pole high-pass ahead of the first grid", editNum(cpath + ".input_hp", 5, 400, 1, true)));
        if (ctl.input_boost_db > 0.0f) v.push_back(mval("Input boost", fmtDb(ctl.input_boost_db), ctl.input_boost_db, "front-panel switch"));
        if (cv.n >= 4) v.push_back(mval("Input expander", "-61 dBFS · 1:4", -61, "keeps DI hiss out of the high-gain cascade (channels with 4+ stages)"));
        blocks.push_back(block("in", "jack", "Input", "Guitar in", std::move(v), "Input"));
    }
    if (ctl.lo_cut > 0.01f) {
        const float hz = 40.0f + 260.0f * ctl.lo_cut;
        blocks.push_back(block("locut", "filter_hp", "Low cut", fmtHz(hz), json::array({mval("Corner", fmtHz(hz), hz, "2-pole SVF high-pass, Q 0.6")}), "Input"));
    }
    auto potBlock = [&]() {
        json v = json::array();
        v.push_back(mval("Wiper", fmtNum(100.0 * c.pot_t, 1) + " % (audio taper)", c.pot_t, "gain knob → audio-taper pot; minimum " + fmtNum(cv.pot_floor * 100.0, 1) + " %"));
        v.push_back(mval("Bright cap", fmtDb(linToDb(c.bright_k)) + " above " + fmtHz(cv.bright_hz), linToDb(c.bright_k),
                         "treble bleed around the pot; strongest at low gain (max " + fmtDb(cv.bright_db) + ")", editNum(cpath + ".bright_db", 0, 12, 0.5)));
        v.push_back(mval("Position", cv.pot_after == 0 ? "at the input" : "after " + stageLabel(org[static_cast<std::size_t>(cv.pot_after - 1)]), cv.pot_after, "",
                         editNum(cpath + ".pot_after", 0, cv.n, 1)));
        blocks.push_back(block("pot", "pot", "Gain pot", fmtNum(100.0 * ctl.gain, 0) + " %", std::move(v), "Preamp"));
    };
    auto stackBlock = [&]() {
        json v = json::array();
        const std::string sp = "amp.stack.";
        v.push_back(mval("Topology", "FMV passive network", 0, "Yeh & Smith closed-form 3rd-order transfer function, bilinear at " + fmtNum(isr_ / 1000.0, 0) + " kHz"));
        v.push_back(mval("Values", stackPresetId(tv), 0, "", editOpts(sp + "preset", [&] {
                              json o = json::array();
                              for (const auto& p : stackPresets()) o.push_back({{"v", p.id}, {"label", p.label}});
                              return o;
                          }())));
        v.push_back(mval("C1 (treble)", fmtFarad(tv.C1), tv.C1, "", editNum(sp + "C1", 22e-12, 2.2e-9, 1e-12, true)));
        v.push_back(mval("C2 (bass)", fmtFarad(tv.C2), tv.C2, "", editNum(sp + "C2", 2.2e-9, 220e-9, 1e-10, true)));
        v.push_back(mval("C3 (mid)", fmtFarad(tv.C3), tv.C3, "", editNum(sp + "C3", 2.2e-9, 220e-9, 1e-10, true)));
        v.push_back(mval("R1 (slope)", fmtOhm(tv.R1), tv.R1, "", editNum(sp + "R1", 10e3, 2.2e6, 100, true)));
        v.push_back(mval("R2 (bass pot)", fmtOhm(tv.R2), tv.R2, "", editNum(sp + "R2", 100e3, 2.2e6, 1000, true)));
        v.push_back(mval("R3 (mid pot)", fmtOhm(tv.R3), tv.R3, "", editNum(sp + "R3", 2.2e3, 220e3, 100, true)));
        v.push_back(mval("R4 (treble pot)", fmtOhm(tv.R4), tv.R4, "", editNum(sp + "R4", 4.7e3, 470e3, 100, true)));
        v.push_back(mval("Knobs", "B " + fmtNum(ctl.bass * 10, 1) + " · M " + fmtNum(ctl.mid * 10, 1) + " · T " + fmtNum(ctl.treble * 10, 1), 0,
                         "bass & treble pots audio taper, mid linear"));
        v.push_back(mval("Position", cv.stack_after > cv.n ? "after the last stage" : "after " + stageLabel(org[static_cast<std::size_t>(cv.stack_after - 1)]), cv.stack_after, "",
                         editNum(cpath + ".stack_after", 1, cv.n + 1, 1)));
        blocks.push_back(block("stack", "tonestack", "Tone stack", std::string(stackPresetId(tv)) + " · FMV", std::move(v), "Tone"));
    };
    if (cv.pot_after == 0) potBlock();
    for (int i = 0; i < cv.n; ++i) {
        const auto& ts = c.st[static_cast<std::size_t>(i)];
        const StageSpec& sp = ts.spec();
        const int oi = org[static_cast<std::size_t>(i)];
        const std::string spth = cpath + ".stages." + std::to_string(oi) + ".";
        const bool follower = ts.isAnalytic();
        json v = json::array();
        std::string type, sub;
        if (follower) {
            type = V.solid_state ? "transistor" : "follower";
            sub = V.solid_state ? "solid-state stage" : "cathode follower";
            v.push_back(mval("Model", V.solid_state ? "analytic soft clipper" : "analytic follower: clips on the positive swing", 0,
                             "knees: +" + fmtNum(sp.lim_pos, 2) + " / -" + fmtNum(sp.lim_neg, 2) + " (normalised)"));
        } else {
            type = "triode";
            v.push_back(mval("Tube", "12AX7 (Koren: µ 100, ex 1.4, kG1 1060, kP 600, kVB 300)", 0, "B+ 250 V, 100 kΩ plate load, 68 kΩ grid source"));
            const int var = ts.curveVariant();
            json opts = json::array({{{"v", -0.05}, {"label", "Hot · Rk 1.5 kΩ, dynamic cathode"}},
                                     {{"v", -0.20}, {"label", "Warm · Rk 2.7 kΩ, dynamic cathode"}},
                                     {{"v", -0.50}, {"label", "Cold clipper · Rk 10 kΩ unbypassed"}}});
            v.push_back(mval("Operating point", var == 0 ? "hot (Rk 1.5 kΩ)" : (var == 1 ? "warm (Rk 2.7 kΩ)" : "cold clipper (Rk 10 kΩ, unbypassed)"), sp.bias,
                             "bias selects which solved Koren operating point the stage runs", editOpts(spth + "bias", std::move(opts))));
            if (ts.isDynamic()) {
                sub = "Rk " + fmtOhm(ts.cathodeR()) + " ∥ " + fmtFarad(ts.cathodeC());
                v.push_back(mval("Cathode Ck", fmtFarad(ts.cathodeC()), ts.cathodeC(),
                                 sp.cath_hz > 0.0f && std::abs(sp.cath_db) > 0.01f ? "partial bypass: Ck from the " + fmtHz(sp.cath_hz) + " corner" : "fully bypassed (4 Hz corner)"));
                v.push_back(mval("Bypass corner", sp.cath_hz > 0.0f && std::abs(sp.cath_db) > 0.01f ? fmtHz(sp.cath_hz) : "full bypass", sp.cath_hz,
                                 "the Rk ∥ Ck state integrates the plate current every sample", editNum(spth + "cath_hz", 0, 2000, 1, true)));
                v.push_back(mval("Quiescent Vk", fmtNum(ts.quiescentVk(), 2) + " V", ts.quiescentVk(), "cathode bias the stage settles to"));
                v.push_back(mval("Quiescent Ip", fmtNum(ts.quiescentIp() * 1000.0, 3) + " mA", ts.quiescentIp() * 1000.0, ""));
            } else {
                sub = "cold clipper · 10 kΩ";
                v.push_back(mval("Cathode", "10 kΩ unbypassed", 10000, "static asymmetric curve + first-order ADAA; cathode \"bloom\" 35 ms"));
            }
        }
        v.push_back(mval("Grid drive", fmtNum(sp.gain, 2) + "× (" + fmtDb(linToDb(sp.gain)) + ")", sp.gain, "drive into this grid, normalised so u = 1 is the conduction knee",
                         editNum(spth + "gain", 0.1, 40, 0.05, true)));
        v.push_back(mval("Coupling HP", fmtHz(sp.hp_hz), sp.hp_hz, "coupling capacitor into the grid", editNum(spth + "hp_hz", 1, 2000, 1, true)));
        v.push_back(mval("Miller LP", fmtHz(sp.lp_hz), sp.lp_hz, "plate load / Miller capacitance roll-off", editNum(spth + "lp_hz", 500, 22000, 10, true)));
        v.push_back(mval("Blocking", fmtNum(sp.blocking, 2), sp.blocking, "grid current charging the coupling cap (0.35 = physical Child-Langmuir); recovers through Rg·Cc",
                         editNum(spth + "blocking", 0, 1, 0.01)));
        v.push_back(mval("Role", follower ? "follower" : "gain stage", follower ? 1 : 0, "",
                         editOpts(spth + "type", json::array({{{"v", "triode"}, {"label", "Triode gain stage"}}, {{"v", "follower"}, {"label", "Cathode follower"}}}))));
        if (cv.n > 1) v.push_back(mval("In signal path", "yes", 1, "remove this stage from the path", editOpts(spth + "off", json::array({{{"v", false}, {"label", "In path"}}, {{"v", true}, {"label", "Removed"}}}))));
        json b = block("s" + std::to_string(oi), type, stageLabel(oi), sub.empty() ? fmtNum(sp.gain, 1) + "×" : sub, std::move(v), "Preamp");
        b["stage"] = i;
        b["orig"] = oi;
        b["role"] = follower ? (V.solid_state ? "solid_state" : "cathode_follower") : (ts.isDynamic() ? "triode" : "cold_clipper");
        b["gain"] = sp.gain;
        blocks.push_back(std::move(b));
        if (i + 1 == cv.pot_after) potBlock();
        if (i + 1 == c.stack_after) stackBlock();
    }
    if (c.stack_after > cv.n) stackBlock();
    if (std::abs(ctl.volume - 1.0f) > 0.01f)
        blocks.push_back(block("vol", "pot", "Channel volume", fmtDb(linToDb(ctl.volume)), json::array({mval("Level", fmtDb(linToDb(ctl.volume)), ctl.volume, "post-preamp channel volume")}), "Preamp"));
    if (c.use_cut) {
        const float hz = 16000.0f * std::pow(0.12f, ctl.cut);
        blocks.push_back(block("cut", "filter_lp", "Tone cut", fmtHz(hz), json::array({mval("Corner", fmtHz(hz), hz, "2-pole low-pass ahead of the phase inverter (Vox-style cut)")}), "Power"));
    }
    {
        json v = json::array();
        v.push_back(mval("Topology", "long-tailed pair", 0, "soft, nearly symmetric clip at ±2.2 (normalised)"));
        v.push_back(mval("Level in", fmtNum(V.power_in, 2) + "×", V.power_in, "preamp → PI level", editNum("amp.power_in", 0.05, 4, 0.01, true)));
        v.push_back(mval("PI drive", fmtNum(d.drive, 2) + "×", d.drive, V.has_master ? "master volume (audio taper) × voicing drive" : "non-master amp: fixed at " + fmtNum(V.fixed_master * 10, 1)));
        v.push_back(mval("Coupling HP", "18 Hz", 18, ""));
        if (beta > 0.0f) v.push_back(mval("NFB return", "β " + fmtNum(beta, 2), beta, "closed loop around PI, output stage and transformer"));
        v.push_back(mval("Preamp B+ dip", "35 % of sag", 0.35, "shared supply: the preamp sags with the power section"));
        blocks.push_back(block("pi", "pi", "Phase inverter", V.has_master ? "Master " + fmtNum(ctl.master * 10, 1) : "non-master", std::move(v), "Power"));
    }
    {
        json v = json::array();
        const std::string pp = "amp.power.";
        v.push_back(mval("Output stage", V.solid_state ? "solid-state (modelled with the same push-pull clipper)" : (ps.class_a ? "push-pull, class-A character blend" : "push-pull pentode pair"), 0,
                         "no tube-specific curves: one tanh pair with imbalance; tube type is not a separate model parameter"));
        v.push_back(mval("Class A blend", ps.class_a ? "on (30 % 1.6× soft path)" : "off", ps.class_a ? 1 : 0, "",
                         editOpts(pp + "class_a", json::array({{{"v", false}, {"label", "Off · push-pull"}}, {{"v", true}, {"label", "On · class-A character"}}}))));
        v.push_back(mval("Voicing drive", fmtNum(ps.drive, 2), ps.drive, "PI drive at master = 1", editNum(pp + "drive", 0.05, 6, 0.01, true)));
        v.push_back(mval("Headroom", fmtNum(d.headroom, 2), d.headroom, "knee × (0.8 + 0.4 × variac)", editNum(pp + "knee", 0.3, 3, 0.01)));
        v.push_back(mval("Imbalance", fmtNum(d.asym, 3), d.asym, "even harmonics; scaled by the bias control", editNum(pp + "asym", 0, 0.4, 0.005)));
        v.push_back(mval("Sag", fmtNum(d.sag, 3), d.sag, "headroom loss under load (4 ms attack, 140 ms release)", editNum(pp + "sag", 0, 1, 0.01)));
        v.push_back(mval("Negative feedback", beta > 0.0f ? "β " + fmtNum(beta, 2) : "none", beta, "β = 0.75 × damping (≤ 0.6); class-A voicings run open loop",
                         editNum(pp + "damping", 0, 1, 0.01)));
        v.push_back(mval("Presence", fmtDb(d.presence_db) + " @ " + fmtHz(ps.presence_hz), d.presence_db, beta > 0.0f ? "shelf removes feedback above the corner (boost at the output)" : "pre-PI high shelf (open loop)",
                         editNum(pp + "presence_hz", 500, 12000, 10, true)));
        v.push_back(mval("Depth / resonance", fmtDb(d.depth_db) + " @ " + fmtHz(ps.depth_hz), d.depth_db, beta > 0.0f ? "bell removes feedback around the corner" : "pre-PI bell (open loop)",
                         editNum(pp + "depth_hz", 30, 400, 1, true)));
        blocks.push_back(block("power", "power", V.solid_state ? "Output stage" : "Power tubes", ps.class_a ? "class A" : "push-pull", std::move(v), "Power"));
    }
    {
        json v = json::array();
        const std::string pp = "amp.power.";
        v.push_back(mval("Low corner", fmtHz(ps.ot_hp_hz), ps.ot_hp_hz, "2-pole high-pass, Q 0.6", editNum(pp + "ot_hp_hz", 10, 300, 1, true)));
        v.push_back(mval("High corner", fmtHz(ps.ot_lp_hz), ps.ot_lp_hz, "2-pole low-pass, Q 0.6", editNum(pp + "ot_lp_hz", 2000, 20000, 50, true)));
        blocks.push_back(block("ot", "transformer", V.solid_state ? "Output" : "Output transformer", fmtHz(ps.ot_hp_hz) + " – " + fmtHz(ps.ot_lp_hz), std::move(v), "Power"));
    }
    {
        json v = json::array();
        v.push_back(mval("Resonance", fmtDb(1.0f + 7.0f * und) + " @ " + fmtHz(ps.spk_fs), ps.spk_fs, "voltage follows the speaker impedance peak (Q 1.4): low damping = bigger bump",
                         editNum("amp.power.spk_fs", 40, 200, 1, true)));
        v.push_back(mval("Voice-coil rise", fmtDb(0.5f + 4.5f * und) + " above 2.8 kHz", 0.5f + 4.5f * und, "inductive impedance rise into a tube output"));
        v.push_back(mval("Damping", fmtNum(ps.damping, 2), ps.damping, "1 = stiff (solid state), 0 = no feedback"));
        v.push_back(mval("Cone excursion", "soft limit below 160 Hz", 160, "1.6 · tanh on the low band"));
        v.push_back(mval("Power compression", "τ 0.9 s · 1/√(1 + 0.35·heat)", 0.9, "voice-coil heating under sustained loud playing"));
        blocks.push_back(block("spk", "speaker", "Speaker load", fmtHz(ps.spk_fs) + " resonance", std::move(v), "Speaker"));
    }
    if (c.geq_n > 0) {
        json v = json::array();
        static constexpr float kHz[9] = {40, 90, 180, 300, 500, 1000, 2000, 4000, 10000};
        for (int i = 0; i < 9; ++i) v.push_back(mval(fmtHz(kHz[i]), fmtDb(ctl.geq[static_cast<std::size_t>(i)]), ctl.geq[static_cast<std::size_t>(i)], "bell, Q 1.1"));
        blocks.push_back(block("geq", "eq", "Graphic EQ", "9 bands", std::move(v), "Speaker"));
    }
    if (ctl.mod_mode != 0 && ctl.mod_depth > 0.001f) {
        static const char* kM[] = {"", "Tremolo (opto-style)", "Vibrato (2–5 ms delay)", "Chorus (JC-style, dry L / wet R)"};
        const float rate = 0.8f + 9.0f * std::pow(std::clamp(ctl.mod_rate, 0.0f, 1.0f), 1.3f);
        blocks.push_back(block("mod", "lfo", kM[std::clamp(ctl.mod_mode, 0, 3)], fmtNum(rate, 1) + " Hz",
                               json::array({mval("Rate", fmtNum(rate, 2) + " Hz", rate, "0.8 + 9·rate^1.3"), mval("Depth", fmtNum(ctl.mod_depth * 100, 0) + " %", ctl.mod_depth, "")}), "Effects"));
    }
    if (has_spring_)
        blocks.push_back(block("spring", "spring", "Spring reverb", ctl.rev_on ? "on" : "off",
                               json::array({mval("Tank", "Accutronics 3-Spring Tank IR", 0, "spring tank impulse response, partitioned convolution"),
                                            mval("Decay", "τ " + fmtNum(0.2f + 1.6f * ctl.rev_decay * ctl.rev_decay, 2) + " s", ctl.rev_decay, "exponential envelope on the IR"),
                                            mval("Level", fmtNum(1.4f * ctl.rev_level, 2) + "×", ctl.rev_level, "")}), "Effects"));
    blocks.push_back(block("out", "out", "Output", fmtDb(linToDb(ctl.level_matching ? trims_[static_cast<std::size_t>(ch)] * outputCompensation(ctl) : 1.0f)),
                           json::array({mval("Level trim", fmtDb(linToDb(trims_[static_cast<std::size_t>(ch)])), trims_[static_cast<std::size_t>(ch)],
                                             analysis_ ? "not computed on analysis instances" : "calibrated: the reference DI renders at ≈ -18 dBFS"),
                                        mval("Master compensation", ctl.level_matching ? fmtDb(linToDb(outputCompensation(ctl))) : "off", ctl.level_matching ? outputCompensation(ctl) : 1.0f, "explicit output utility; not part of the circuit")}), "Output"));
    m["blocks"] = std::move(blocks);
    if (beta > 0.0f) m["loops"] = json::array({{{"from", "ot"}, {"to", "pi"}, {"label", "NFB β " + fmtNum(beta, 2) + " · presence & depth in the loop"}}});
    return m.dump();
}

std::string TubeAmpComponent::curvesJson() const {
    if (readControls().circuit_core) return R"({"backend":"hybrid-circuit","static_unavailable":"Use rendered harmonic/impulse analysis for the coupled circuit"})";
    const AmpControls ctl = readControls();
    json out = json::object();
    // Tone stack (analog prototype; the engine runs its bilinear transform at the oversampled rate).
    {
        FmvToneStack ts;
        ts.prepare(isr_, voice_.stack);
        const auto fr = logFreqs();
        json f = json::array(), db = json::array(), flat = json::array();
        for (double x : fr) {
            f.push_back(std::round(x * 10.0) / 10.0);
            db.push_back(std::round(linToDb(ts.analysisMagnitude(x, ctl.bass, ctl.mid, ctl.treble)) * 100.0) / 100.0);
            flat.push_back(std::round(linToDb(ts.analysisMagnitude(x, 0.5f, 0.5f, 0.5f)) * 100.0) / 100.0);
        }
        out["tonestack"] = {{"f", f}, {"db", db}, {"noon_db", flat}, {"bass", ctl.bass}, {"mid", ctl.mid}, {"treble", ctl.treble}};
    }
    // Static transfer curve of every stage in the active channel (u = 1 ↔ grid at the conduction knee).
    {
        Chain c;
        applyControls(c, ctl, true);
        json u = json::array(), stages = json::array();
        for (int k = 0; k <= 160; ++k) u.push_back(-8.0 + 0.1 * k);
        for (int i = 0; i < c.n; ++i) {
            const auto& ts = c.st[static_cast<std::size_t>(i)];
            json y = json::array();
            for (int k = 0; k <= 160; ++k) y.push_back(std::round(ts.staticTransfer(static_cast<float>(-8.0 + 0.1 * k)) * 1000.0) / 1000.0);
            stages.push_back({{"label", stageLabel(origin_[static_cast<std::size_t>(ctl.channel)][static_cast<std::size_t>(i)])}, {"role", ts.isAnalytic() ? "follower" : (ts.isDynamic() ? "triode" : "cold_clipper")}, {"gain", ts.spec().gain}, {"y", y}});
        }
        out["stages"] = {{"u", u}, {"curves", stages}};
    }
    // Power stage: PI + push-pull pair at the current master / variac / bias (open loop, sag at rest).
    {
        const PowerSpec& ps = voice_.power;
        const auto d = PowerAmp::derive(ps, ctl.master, ctl.presence, ctl.depth, ctl.sag, ctl.vac, ctl.bias);
        json x = json::array(), y = json::array();
        for (int k = 0; k <= 120; ++k) {
            const float xi = -1.5f + 0.025f * static_cast<float>(k);
            const float v = PowerAmp::phaseInverter(xi * d.drive) / d.headroom;
            x.push_back(xi);
            y.push_back(std::round(PowerAmp::outputStage(v, d.asym, ps.class_a) * d.headroom * 1000.0) / 1000.0);
        }
        out["power"] = {{"x", x}, {"y", y}, {"drive", d.drive}, {"headroom", d.headroom}, {"asym", d.asym}, {"nfb_beta", PowerAmp::feedbackBeta(ps)}};
    }
    return out.dump();
}

// ───────────────────────────── Drive pedals ─────────────────────────────
enum class PedalKind { Screamer, TrebleBoost, CleanBoost, HardClip, Rat, Fuzz, Muff, Percolator, Preamp, HM2, Metal, Saturator };

inline float diodeSoft(float x, float vd) noexcept { return vd * amp::fastTanh(x / vd); }
inline float diodeHard(float x, float vd) noexcept {
    const float r = x / vd;
    return vd * r / std::pow(1.0f + std::pow(std::abs(r), 2.5f), 0.4f);
}
inline float germanium(float x) noexcept {
    return (x >= 0.0f) ? amp::fastTanh(x * 0.9f) / 0.9f * 0.95f : 0.75f * amp::fastTanh(x / 0.75f);
}

const char* const kPedalKindIds[] = {"screamer", "treble_boost", "clean_boost", "hard_clip", "rat", "fuzz", "muff", "percolator", "preamp", "hm2", "metal", "saturator"};
const char* const kPedalKindLabels[] = {
    "Op-amp, diodes in the feedback loop (soft clip + clean sum)",
    "Germanium treble booster",
    "Clean FET boost",
    "Op-amp + diodes to ground (hard clip)",
    "LM308 op-amp, GBW-limited, hard diodes",
    "Two-transistor fuzz with bias starve",
    "Cascaded soft-clip stages (Muff-style)",
    "Germanium transistor + hard diodes (percolator)",
    "Two asymmetric tube-style stages + EQ",
    "Dual-band boost into hard diodes (HM-2 style)",
    "High-gain op-amp, pre-EQ, hard diodes, parametric mid",
    "Analog character stages + EQ",
};

// {"drive":{...}} user overrides, applied after the knob mapping on every (re)configure.
struct PedalOverrides {
    int kind = -1, clip = -1;
    float vd = 0.0f, drive_db = 0.0f, gain2_db = 0.0f, level_db = 0.0f;
    float pre_hp_hz = 0.0f, hump_hz = 0.0f, fb_lp_hz = 0.0f, post_lp_hz = 0.0f, bias = -99.0f;
    std::string key;
};
PedalOverrides parsePedalOverrides(const json& root) {
    PedalOverrides o;
    const auto d = root.find("drive");
    if (d == root.end() || !d->is_object() || d->empty()) return o;
    if (const auto k = d->find("kind"); k != d->end() && k->is_string())
        for (int i = 0; i < 12; ++i)
            if (k->get<std::string>() == kPedalKindIds[i]) o.kind = i;
    if (const auto c = d->find("clip"); c != d->end() && c->is_string()) o.clip = c->get<std::string>() == "soft" ? 0 : (c->get<std::string>() == "hard" ? 1 : -1);
    jset(*d, "vd", o.vd, 0.05f, 2.0f);
    jset(*d, "drive_db", o.drive_db, -24.0f, 24.0f);
    jset(*d, "gain2_db", o.gain2_db, -24.0f, 24.0f);
    jset(*d, "level_db", o.level_db, -24.0f, 24.0f);
    jset(*d, "pre_hp_hz", o.pre_hp_hz, 10.0f, 2000.0f);
    jset(*d, "hump_hz", o.hump_hz, 100.0f, 3000.0f);
    jset(*d, "fb_lp_hz", o.fb_lp_hz, 500.0f, 20000.0f);
    jset(*d, "post_lp_hz", o.post_lp_hz, 300.0f, 20000.0f);
    jset(*d, "bias", o.bias, -0.5f, 0.5f);
    o.key = d->dump();
    return o;
}

class DrivePedalComponent final : public SchemaComponentBase, public model::Introspectable {
public:
    DrivePedalComponent(const catalog::ComponentSchema& schema, PedalKind kind)
        : SchemaComponentBase(schema), base_kind_(kind), kind_(kind) {
        params_[model::kModelParam].kind = catalog::ParamKind::String;
    }

    std::vector<ExportedParam> exportParams() const override {
        auto v = SchemaComponentBase::exportParams();
        v.erase(std::remove_if(v.begin(), v.end(), [](const ExportedParam& e) {
                    return e.id == model::kAnalysisParam || (e.id == model::kModelParam && e.str.empty());
                }), v.end());
        return v;
    }
    std::string modelJson() const override;
    std::string curvesJson() const override;

    // Knob edits arrive as a prepared copy of this unit: keep the oversampler / filter histories and glide the
    // knob-derived gains from the values the old copy was playing (a fresh copy would restart every filter and
    // step every gain on each knob tick).
    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<DrivePedalComponent*>(&previous);
        if (!old || old->tableId() != tableId() || old->isr_ != isr_ || old->os_.factor() != os_.factor()) return;
        os_ = old->os_;
        dry_os_ = old->dry_os_;
        for (std::size_t i = 0; i < ch_.size(); ++i) ch_[i].adoptRuntimeStateFrom(old->ch_[i]);
    }
    // Series pipelining (RackGraph): mono or stereo is the rack's per-block AudioBlock::mono_input, else chosen per call
    // (onProcessAudio); the mono path copies the left output to the right.
    int subBlockSafety() const noexcept override { return 1; }
    bool monoPreserving() const noexcept override { return true; }

protected:
    void onStringParameterChanged(std::string_view id, std::string_view value) override {
        if (id != model::kModelParam) return;
        json j;
        if (!parseModelChecked(value, j)) return; // malformed: keep the last valid overrides
        ov_ = parsePedalOverrides(j);
        kind_ = ov_.kind >= 0 ? static_cast<PedalKind>(ov_.kind) : base_kind_;
        dirty_ = true;
    }

    void onPrepare(double sr, uint32_t) override {
        os_.prepare(sr >= 88000.0 ? 2 : 4);
        dry_os_.prepare(os_.factor());
        isr_ = sr * os_.factor();
        glide_ = 1.0f - std::exp(-1.0f / (0.012f * static_cast<float>(sr))); // ~12 ms knob glide
        has_mix_ = std::any_of(schema_.params.begin(), schema_.params.end(), [](const auto& p) { return p.id == "mix"; });
        dirty_ = true;
        onProcessControlRate();
        onReset();
    }
    void onReset() override {
        os_.reset();
        dry_os_.reset();
        for (auto& c : ch_) c.reset();
    }

    void onProcessControlRate() override {
        Controls k = readControls();
        if (k == last_ && !dirty_) return;
        last_ = k;
        dirty_ = false;
        for (auto& c : ch_) {
            const Ch::Gains playing = c.gains();
            configure(c, k);
            c.target = c.gains();
            if (c.running) c.setGains(playing); // already sounding: glide (see Ch::glide)
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        const uint32_t nf = block.num_frames;
        const bool stereo_in = block.num_channels > 1 && block.in[1] && block.in[0] != block.in[1] &&
                               (block.mono_input >= 0 ? block.mono_input == 0 : !std::equal(block.in[0], block.in[0] + nf, block.in[1]));
        const int nch = stereo_in ? 2 : 1;
        const int fac = os_.factor();
        float up[8], dry_up[8];
        for (int ch = 0; ch < nch; ++ch) {
            const float* in = block.in[ch] ? block.in[ch] : block.in[0];
            float* out = block.out[ch];
            if (!in || !out) continue;
            auto& c = ch_[static_cast<std::size_t>(ch)];
            c.running = true;
            for (uint32_t i = 0; i < nf; ++i) {
                c.glide(glide_);
                os_.up(ch, in[i], up);
                for (int k = 0; k < fac; ++k) up[k] = c.process(up[k], kind_);
                const float wet = c.post(os_.down(ch, up));
                if (!has_mix_) {
                    out[i] = wet;
                    continue;
                }
                // Dry path through an identical (unprocessed) up/down half-band chain: the polyphase IIR
                // half-bands are not linear phase, so this reproduces the wet path's exact oversampling
                // delay and phase. Blending against the raw input would comb-filter (≈20 dB notches).
                // Kept running at Mix = 100 % so turning the knob down never starts from cold filters.
                dry_os_.up(ch, in[i], dry_up);
                const float dry = dry_os_.down(ch, dry_up);
                out[i] = (c.mix >= 0.999f) ? wet : (1.0f - c.mix) * dry + c.mix * wet;
            }
        }
        if (nch == 1) {
            for (uint32_t c = 1; c < block.num_channels; ++c) {
                if (block.out[c] && block.out[c] != block.out[0]) std::copy(block.out[0], block.out[0] + nf, block.out[c]);
            }
        }
    }

private:
    struct Controls {
        float drive = 0.5f, tone = 0.5f, level = 0.5f, bass = 0.5f, mid = 0.5f, treble = 0.5f;
        float mix = 1.0f, bright = 0.0f, extra = 0.0f;
        int mode = 0;
        bool cab = false;
        bool operator==(const Controls&) const = default;
    };

    struct Ch {
        OnePole pre_hp{}, fb_hp{}, fb_lp{}, tone_lp{}, tone_hp{}, inter_hp{}, inter_lp{}, clean_lp{};
        Svf pre_eq1{}, pre_eq2{}, post_lp{}, eq_bass{}, eq_mid{}, eq_treb{}, cab_lp{}, cab_bell{}, cab_hp{};
        float gain = 10.0f, gain2 = 1.0f, vd = 0.5f, tone_mix = 0.5f, out = 1.0f, clean = 0.0f, mix = 1.0f;
        float bias = 0.0f, env = 0.0f;
        int clip_shape = -1; // user override: -1 = as designed, 0 = soft diodes, 1 = hard diodes
        bool cab = false, eq = false;

        // Knob-derived scalars glide per sample from what is playing to what configure() designed.
        struct Gains { float gain, gain2, vd, tone_mix, out, mix, bias; };
        Gains target{10.0f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f, 0.0f};
        bool running = false, gliding = false;
        Gains gains() const noexcept { return {gain, gain2, vd, tone_mix, out, mix, bias}; }
        void setGains(const Gains& g) noexcept {
            gain = g.gain; gain2 = g.gain2; vd = g.vd; tone_mix = g.tone_mix; out = g.out; mix = g.mix; bias = g.bias;
            gliding = true;
        }
        void glide(float a) noexcept {
            if (!gliding) return;
            auto step = [a](float& v, float t) noexcept { v += (t - v) * a; return std::abs(t - v) <= 1e-5f * (1.0f + std::abs(t)); };
            bool done = step(gain, target.gain);
            done &= step(gain2, target.gain2);
            done &= step(vd, target.vd);
            done &= step(out, target.out);
            done &= step(mix, target.mix);
            done &= step(bias, target.bias);
            if (tone_mix < 0.0f || target.tone_mix < 0.0f) tone_mix = target.tone_mix; // < 0 = no tone blend (a switch)
            else done &= step(tone_mix, target.tone_mix);
            if (done) { setGains(target); gliding = false; }
        }
        void adoptRuntimeStateFrom(const Ch& p) noexcept {
            using P1 = std::pair<OnePole*, const OnePole*>;
            using P2 = std::pair<Svf*, const Svf*>;
            for (auto [f, o] : std::initializer_list<P1>{{&pre_hp, &p.pre_hp}, {&fb_hp, &p.fb_hp}, {&fb_lp, &p.fb_lp}, {&tone_lp, &p.tone_lp},
                                                         {&tone_hp, &p.tone_hp}, {&inter_hp, &p.inter_hp}, {&inter_lp, &p.inter_lp}, {&clean_lp, &p.clean_lp}})
                f->adoptRuntimeStateFrom(*o);
            for (auto [f, o] : std::initializer_list<P2>{{&pre_eq1, &p.pre_eq1}, {&pre_eq2, &p.pre_eq2}, {&post_lp, &p.post_lp}, {&eq_bass, &p.eq_bass},
                                                         {&eq_mid, &p.eq_mid}, {&eq_treb, &p.eq_treb}, {&cab_lp, &p.cab_lp}, {&cab_bell, &p.cab_bell}, {&cab_hp, &p.cab_hp}})
                f->adoptRuntimeStateFrom(*o);
            env = p.env;
            if (p.running) {
                const Gains t = target;
                setGains(p.gains());
                target = t;
                running = true;
            }
        }

        float clipD(float u, float v, bool soft_by_design) const noexcept {
            const bool soft = clip_shape < 0 ? soft_by_design : clip_shape == 0;
            return soft ? diodeSoft(u, v) : diodeHard(u, v);
        }
        // process() with every filter at unity (their passband): the static curve of the clipping path.
        float staticCurve(float x, PedalKind k) const noexcept {
            switch (k) {
                case PedalKind::Screamer: return x + clipD(x * gain, vd, true);
                case PedalKind::TrebleBoost: { const float u = 1.12f * x * gain + bias; return germanium(u) - germanium(bias); }
                case PedalKind::CleanBoost: return diodeSoft(x * gain, 1.8f);
                case PedalKind::HardClip: case PedalKind::Metal: case PedalKind::HM2: return clipD(x * gain, vd, false);
                case PedalKind::Rat: return clipD(std::clamp(x * gain, -4.0f, 4.0f), vd, false);
                case PedalKind::Fuzz: { const float u = germanium(x * gain + bias) - germanium(bias); return diodeSoft(u * 1.5f, 0.9f); }
                case PedalKind::Muff: return clipD(clipD(x * gain, vd, true) * gain2, vd, true);
                case PedalKind::Percolator: { const float u = (germanium(x * gain + bias) - germanium(bias)) * gain2; return clipD(u, vd, false); }
                case PedalKind::Preamp: case PedalKind::Saturator: {
                    float u = x * gain;
                    u = (u >= 0.0f) ? u / std::sqrt(1.0f + u * u) : 1.4f * amp::fastTanh(u / 1.4f);
                    u = -u * gain2;
                    u = (u >= 0.0f) ? u / std::sqrt(1.0f + u * u) : 1.4f * amp::fastTanh(u / 1.4f);
                    return -u;
                }
            }
            return x;
        }
        // The linear filters ahead of / between the clippers, in process() order (clippers as unity).
        float preFilter(float x, PedalKind k) noexcept {
            switch (k) {
                case PedalKind::Screamer: return fb_lp.lp(fb_hp.hp(x));
                case PedalKind::TrebleBoost: return pre_hp.hp(x) + 0.12f * x;
                case PedalKind::CleanBoost: return x;
                case PedalKind::HardClip: case PedalKind::Metal: case PedalKind::HM2: return fb_lp.lp(pre_eq2.process(pre_eq1.process(pre_hp.hp(x))));
                case PedalKind::Rat: return fb_lp.lp(pre_hp.hp(x));
                case PedalKind::Fuzz: case PedalKind::Preamp: case PedalKind::Saturator: return pre_hp.hp(x);
                case PedalKind::Muff: return inter_lp.lp(inter_hp.hp(pre_hp.hp(x)));
                case PedalKind::Percolator: return inter_hp.hp(pre_hp.hp(x));
            }
            return x;
        }

        void reset() {
            for (auto* f : {&pre_hp, &fb_hp, &fb_lp, &tone_lp, &tone_hp, &inter_hp, &inter_lp, &clean_lp}) f->reset();
            for (auto* f : {&pre_eq1, &pre_eq2, &post_lp, &eq_bass, &eq_mid, &eq_treb, &cab_lp, &cab_bell, &cab_hp}) f->reset();
            env = 0.0f;
        }

        float process(float x, PedalKind k) noexcept {
            switch (k) {
                case PedalKind::Screamer: {
                    // Op-amp non-inverting stage with diodes in the feedback: clean signal + clipped HPF'd gain.
                    const float h = fb_lp.lp(fb_hp.hp(x)) * gain;
                    return x + clipD(h, vd, true);
                }
                case PedalKind::TrebleBoost: {
                    const float h = pre_hp.hp(x) + 0.12f * x;
                    float u = h * gain + bias;
                    return germanium(u) - germanium(bias);
                }
                case PedalKind::CleanBoost:
                    return diodeSoft(x * gain, 1.8f);
                case PedalKind::HardClip:
                case PedalKind::Metal:
                case PedalKind::HM2: {
                    float u = pre_hp.hp(x);
                    u = pre_eq1.process(u);
                    u = pre_eq2.process(u);
                    u = fb_lp.lp(u * gain);
                    return clipD(u, vd, false);
                }
                case PedalKind::Rat: {
                    float u = pre_hp.hp(x) * gain;
                    u = fb_lp.lp(u); // op-amp gain-bandwidth limit
                    u = std::clamp(u, -4.0f, 4.0f);
                    return clipD(u, vd, false);
                }
                case PedalKind::Fuzz: {
                    // Two-transistor fuzz: input-level dependent bias starves the clipper ("sputter" / cleanup).
                    const float a = std::abs(x);
                    env += (a - env) * ((a > env) ? 0.002f : 0.00005f);
                    float u = pre_hp.hp(x) * gain + bias - 0.6f * env;
                    u = germanium(u) - germanium(bias);
                    return diodeSoft(u * 1.5f, 0.9f);
                }
                case PedalKind::Muff: {
                    float u = pre_hp.hp(x) * gain;
                    u = clipD(u, vd, true);
                    u = inter_lp.lp(inter_hp.hp(u)) * gain2;
                    u = clipD(u, vd, true);
                    return u;
                }
                case PedalKind::Percolator: {
                    float u = pre_hp.hp(x) * gain + bias;
                    u = germanium(u) - germanium(bias);
                    u = inter_hp.hp(u) * gain2;
                    return clipD(u, vd, false);
                }
                case PedalKind::Preamp:
                case PedalKind::Saturator: {
                    float u = pre_hp.hp(x) * gain;
                    // Two soft tube-like stages with alternating asymmetry.
                    u = (u >= 0.0f) ? u / std::sqrt(1.0f + u * u) : 1.4f * amp::fastTanh(u / 1.4f);
                    u = -u * gain2;
                    u = (u >= 0.0f) ? u / std::sqrt(1.0f + u * u) : 1.4f * amp::fastTanh(u / 1.4f);
                    return -u;
                }
            }
            return x;
        }

        float post(float y) noexcept {
            float t = tone_lp.lp(y);
            if (tone_mix >= 0.0f) {
                const float hp = y - t;
                t = (1.0f - tone_mix) * t + tone_mix * (hp * 0.9f + t * 0.35f);
            } else {
                t = y;
            }
            t = post_lp.process(t);
            if (eq) {
                t = eq_bass.process(t);
                t = eq_mid.process(t);
                t = eq_treb.process(t);
            }
            if (cab) t = cab_hp.process(cab_bell.process(cab_lp.process(t)));
            return t * out;
        }
    };

    Controls readControls() const {
        Controls k;
        auto P = [this](std::string_view id, float def = 0.5f) { return getParamOr(id, def); };
        auto on = [&](std::string_view id) { return P(id, 0.0f) >= 0.5f; };
        switch (schema_.table_id) {
            case 15000: // Ibanez TS808 Tube Screamer
                k.drive = P("Dr"); k.tone = P("Tn"); k.level = P("Vol"); k.bass = P("Bs"); k.mix = 1.0f;
                k.extra = P("Cl", 0.0f); k.bright = P("Br", 0.5f);
                break;
            case 167000: // Ibanez TS9DX Turbo Tube Screamer
                k.drive = P("overdrive"); k.tone = P("tone"); k.level = P("level");
                k.bass = P("bass"); k.mid = P("mid"); k.treble = P("treble");
                k.mode = static_cast<int>(std::lround(P("mode", 0.0f)));
                break;
            case 40000: k.drive = P("Bs"); k.bright = on("Br") ? 1.0f : 0.0f; break;
            case 65000: k.drive = P("B"); break;
            case 17000: k.drive = P("Dst"); k.tone = P("tn"); k.level = P("vol"); k.bass = P("Bs"); k.mid = P("M"); k.treble = P("T"); break;
            case 42000: k.drive = P("D"); k.tone = P("F"); k.level = P("V"); k.bass = P("B"); k.treble = P("T"); k.extra = P("Bl", 0.0f); break;
            case 41000: k.drive = P("F"); k.level = P("V"); k.bass = P("B"); k.treble = P("T"); break;
            case 166000: case 169000: k.drive = P("fuzz"); k.level = P("volume"); k.mode = static_cast<int>(std::lround(P("mode", 0.0f))); break;
            case 43000: k.drive = P("S"); k.tone = P("T"); k.level = P("V"); k.bass = P("B", 0.25f); k.treble = P("R"); break;
            case 174000: k.drive = P("harmonics"); k.level = P("volume"); k.extra = P("diodes", 0.0f); break;
            case 44000: k.drive = P("G"); k.level = P("V"); k.bass = P("B"); k.mid = P("M"); k.treble = P("T"); k.extra = P("S", 0.0f); break;
            case 46000: k.drive = P("D"); k.level = P("V"); k.bass = P("B"); k.mid = P("M"); k.treble = P("T"); k.tone = P("MF"); break;
            case 45000:
                k.drive = P("D"); k.level = P("V"); k.bass = P("B"); k.treble = P("T");
                k.cab = on("CM"); k.extra = P("H", 0.0f); k.mode = on("A") ? 1 : 0;
                break;
            case 170000: k.drive = P("gain"); k.level = P("volume"); k.bass = P("bass"); k.treble = P("treble"); break;
            case 84000: k.drive = P("g"); k.level = P("v"); k.bass = P("b"); k.treble = P("t"); k.mid = 1.0f - P("c"); k.tone = P("f"); break;
            case 111000: k.drive = P("D0"); k.tone = P("T2"); k.mix = 1.0f; k.level = 0.5f; break;
            case 165000: k.drive = P("distortion"); k.bass = P("low"); k.treble = P("high"); k.level = P("level"); break;
            case 173000:
                k.drive = P("drive", 0.2f); k.level = P("out"); k.mix = P("mix", 1.0f);
                k.bass = P("bass"); k.mid = P("mid"); k.treble = P("treble");
                break;
            default:
                k.drive = getNormalizedParam({"Drive", "Gain", "Distortion", "Fuzz", "drive", "gain"}, 0.5f);
                k.level = getNormalizedParam({"Volume", "Level", "Output", "level"}, 0.5f);
                k.tone = getNormalizedParam({"Tone", "tone"}, 0.5f);
                break;
        }
        return k;
    }

    void configure(Ch& c, const Controls& k) const {
        const double sr = isr_;
        c.eq = false;
        c.cab = false;
        c.tone_mix = -1.0f;
        c.post_lp.set(Svf::Type::LowPass, sr, 16000.0f, 0.6f);
        auto setEq = [&](float b, float m, float t, float bhz, float mhz, float thz, float range_db) {
            c.eq = true;
            c.eq_bass.set(Svf::Type::LowShelf, sr, bhz, 0.7f, (b - 0.5f) * 2.0f * range_db);
            c.eq_mid.set(Svf::Type::Bell, sr, mhz, 0.8f, (m - 0.5f) * 2.0f * range_db);
            c.eq_treb.set(Svf::Type::HighShelf, sr, thz, 0.7f, (t - 0.5f) * 2.0f * range_db);
        };
        switch (kind_) {
            case PedalKind::Screamer: {
                const bool deluxe = schema_.table_id == 167000;
                const float hp = deluxe ? 720.0f * (1.4f - 0.8f * k.bass) : 720.0f * (1.5f - k.bass);
                c.fb_hp.set(sr, hp);
                const float r = 51e3f + 500e3f * amp::audioTaper(k.drive);
                c.fb_lp.set(sr, 1.0f / (2.0f * amp::kPi * r * 51e-12f) * (deluxe && k.mode == 2 ? 0.7f : 1.0f));
                c.gain = r / 4700.0f;
                c.vd = (deluxe && k.mode == 1) ? 0.62f : 0.5f;
                c.tone_lp.set(sr, 700.0f + 5200.0f * k.tone * k.tone);
                c.tone_mix = -1.0f;
                c.post_lp.set(Svf::Type::LowPass, sr, 900.0f + 7000.0f * k.tone, 0.55f);
                if (deluxe) setEq(k.bass, k.mid, k.treble, 120.0f, 800.0f, 3200.0f, 9.0f);
                c.out = dbToLin(-26.0f + 38.0f * k.level) * 0.75f;
                break;
            }
            case PedalKind::TrebleBoost:
                c.pre_hp.set(sr, k.bright > 0.5f ? 1900.0f : 1100.0f);
                c.gain = dbToLin(6.0f + 24.0f * k.drive);
                c.bias = 0.08f;
                c.out = 0.85f;
                break;
            case PedalKind::CleanBoost:
                c.gain = dbToLin(20.0f * k.drive);
                c.out = 1.0f;
                break;
            case PedalKind::HardClip:
                c.pre_hp.set(sr, 90.0f);
                c.pre_eq1.set(Svf::Type::Bell, sr, 800.0f, 0.7f, 3.0f);
                c.pre_eq2.set(Svf::Type::HighShelf, sr, 3000.0f, 0.7f, 0.0f);
                c.fb_lp.set(sr, 9000.0f);
                c.gain = dbToLin(12.0f + 38.0f * k.drive);
                c.vd = 0.45f;
                c.tone_lp.set(sr, 900.0f);
                c.tone_mix = std::clamp(k.tone, 0.0f, 1.0f);
                setEq(k.bass, k.mid, k.treble, 120.0f, 900.0f, 3000.0f, 9.0f);
                c.post_lp.set(Svf::Type::LowPass, sr, 7500.0f, 0.6f);
                c.out = dbToLin(-22.0f + 30.0f * k.level);
                break;
            case PedalKind::Rat: {
                const float g = 1.0f + 2000.0f * amp::audioTaper(k.drive);
                c.gain = g;
                c.pre_hp.set(sr, 60.0f);
                c.fb_lp.set(sr, std::clamp(1.0e6f / g, 600.0f, 20000.0f)); // LM308 GBW
                c.vd = 0.55f;
                c.post_lp.set(Svf::Type::LowPass, sr, 475.0f + 31000.0f * std::pow(1.0f - k.tone, 2.2f), 0.5f);
                c.out = dbToLin(-24.0f + 30.0f * k.level);
                break;
            }
            case PedalKind::Fuzz: {
                const bool red = schema_.table_id == 166000 || schema_.table_id == 169000;
                c.pre_hp.set(sr, red ? 60.0f : 90.0f);
                c.gain = dbToLin(10.0f + 34.0f * k.drive);
                c.bias = red ? 0.15f : 0.08f;
                c.post_lp.set(Svf::Type::LowPass, sr, red ? 5200.0f : 6500.0f, 0.6f);
                c.out = dbToLin(-24.0f + 26.0f * k.level);
                break;
            }
            case PedalKind::Muff:
                c.pre_hp.set(sr, 100.0f);
                c.gain = dbToLin(8.0f + 26.0f * k.drive);
                c.gain2 = dbToLin(14.0f + 10.0f * k.drive);
                c.inter_hp.set(sr, 120.0f);
                c.inter_lp.set(sr, 5000.0f);
                c.vd = 0.5f;
                c.tone_lp.set(sr, 1000.0f);
                c.tone_mix = std::clamp(k.tone, 0.0f, 1.0f);
                c.out = dbToLin(-20.0f + 28.0f * k.level);
                break;
            case PedalKind::Percolator:
                c.pre_hp.set(sr, 120.0f);
                c.gain = dbToLin(10.0f + 26.0f * k.drive);
                c.gain2 = 4.0f;
                c.inter_hp.set(sr, 300.0f);
                c.bias = 0.12f;
                c.vd = k.extra > 0.5f ? 0.35f : 0.55f;
                c.post_lp.set(Svf::Type::LowPass, sr, 4500.0f, 0.6f);
                c.out = dbToLin(-18.0f + 24.0f * k.level);
                break;
            case PedalKind::Preamp: {
                const bool trans = schema_.table_id == 45000;
                c.pre_hp.set(sr, 70.0f);
                c.gain = dbToLin((trans ? 4.0f : 2.0f) + 30.0f * k.drive + (trans ? 8.0f * k.extra : 0.0f));
                c.gain2 = 2.0f + 4.0f * k.drive;
                setEq(k.bass, 0.5f, k.treble, 120.0f, 800.0f, 3000.0f, 12.0f);
                c.cab = trans && k.cab;
                if (c.cab) {
                    c.cab_lp.set(Svf::Type::LowPass, sr, 4800.0f, 0.75f);
                    c.cab_bell.set(Svf::Type::Bell, sr, 2400.0f, 1.0f, -3.0f);
                    c.cab_hp.set(Svf::Type::HighPass, sr, 75.0f, 0.7f);
                }
                c.out = dbToLin(-24.0f + 24.0f * k.level) * 0.8f;
                break;
            }
            case PedalKind::Metal: {
                const bool zone = schema_.table_id == 46000;
                c.pre_hp.set(sr, 120.0f);
                c.pre_eq1.set(Svf::Type::Bell, sr, 1000.0f, 0.8f, 6.0f);
                c.pre_eq2.set(Svf::Type::HighShelf, sr, 2500.0f, 0.7f, 4.0f);
                c.fb_lp.set(sr, 8000.0f);
                c.gain = dbToLin(24.0f + 34.0f * k.drive);
                c.vd = 0.45f;
                const float mhz = zone ? 200.0f * std::pow(25.0f, k.tone) : 750.0f;
                setEq(k.bass, (schema_.table_id == 44000) ? k.mid - 0.4f * k.extra : k.mid, k.treble, 110.0f, mhz, 3500.0f, 12.0f);
                c.post_lp.set(Svf::Type::LowPass, sr, 7000.0f, 0.6f);
                c.out = dbToLin(-22.0f + 26.0f * k.level);
                break;
            }
            case PedalKind::HM2:
                c.pre_hp.set(sr, 70.0f);
                c.pre_eq1.set(Svf::Type::Bell, sr, 95.0f, 0.9f, 4.0f + 14.0f * k.bass);
                c.pre_eq2.set(Svf::Type::Bell, sr, 1500.0f, 0.9f, 4.0f + 14.0f * k.treble);
                c.fb_lp.set(sr, 10000.0f);
                c.gain = dbToLin(18.0f + 30.0f * k.drive);
                c.vd = 0.42f;
                c.post_lp.set(Svf::Type::LowPass, sr, 6500.0f, 0.6f);
                c.out = dbToLin(-22.0f + 28.0f * k.level);
                break;
            case PedalKind::Saturator:
                c.pre_hp.set(sr, 20.0f);
                c.gain = dbToLin(-6.0f + 26.0f * k.drive);
                c.gain2 = 1.0f + 2.0f * k.drive;
                setEq(k.bass, k.mid, k.treble, 110.0f, 900.0f, 4000.0f, 9.0f);
                c.out = dbToLin(-12.0f + 22.0f * k.level) / std::max(0.4f, c.gain * 0.6f);
                break;
        }
        c.mix = std::clamp(k.mix, 0.0f, 1.0f);
        // User overrides (My Components): replace the designed values after the knob mapping.
        if (ov_.vd > 0.0f) c.vd = ov_.vd;
        if (ov_.drive_db != 0.0f) c.gain *= dbToLin(ov_.drive_db);
        if (ov_.gain2_db != 0.0f) c.gain2 *= dbToLin(ov_.gain2_db);
        if (ov_.pre_hp_hz > 0.0f) c.pre_hp.set(sr, ov_.pre_hp_hz);
        if (ov_.hump_hz > 0.0f) c.fb_hp.set(sr, ov_.hump_hz);
        if (ov_.fb_lp_hz > 0.0f) c.fb_lp.set(sr, ov_.fb_lp_hz);
        if (ov_.post_lp_hz > 0.0f) c.post_lp.set(Svf::Type::LowPass, sr, ov_.post_lp_hz, c.post_lp.q());
        if (ov_.bias > -9.0f) c.bias = ov_.bias;
        if (ov_.level_db != 0.0f) c.out *= dbToLin(ov_.level_db);
        c.clip_shape = ov_.clip;
    }

    PedalKind base_kind_;
    PedalOverrides ov_{};
    PedalKind kind_;
    Oversampler os_{}, dry_os_{}; // dry_os_: phase/delay-matched dry path for the Mix control
    bool has_mix_ = false;
    double isr_ = 192000.0;
    std::array<Ch, 2> ch_{};
    Controls last_{};
    bool dirty_ = true;
    float glide_ = 0.0017f;
};

// ───────────────────────────── Pedal model export ─────────────────────────────
std::string DrivePedalComponent::modelJson() const {
    const Controls k = readControls();
    Ch c;
    configure(c, k);
    const int kind = static_cast<int>(kind_);
    const bool custom = !ov_.key.empty();
    json m = json::object();
    m["kind"] = "drive";
    m["topology"] = kPedalKindIds[kind];
    m["topology_label"] = kPedalKindLabels[kind];
    m["designed_topology"] = kPedalKindIds[static_cast<int>(base_kind_)];
    m["oversampling"] = os_.factor();
    m["internal_rate"] = isr_;
    m["custom"] = custom;
    m["controls"] = {{"drive", k.drive}, {"tone", k.tone}, {"level", k.level}, {"bass", k.bass}, {"mid", k.mid}, {"treble", k.treble},
                     {"mix", k.mix}, {"bright", k.bright}, {"extra", k.extra}, {"mode", k.mode}, {"cab", k.cab}};
    json topo = json::array();
    for (int i = 0; i < 12; ++i) topo.push_back({{"v", kPedalKindIds[i]}, {"label", kPedalKindLabels[i]}});
    m["topologies"] = topo;
    json diodes = json::array({{{"v", 0.35}, {"label", "Germanium · knee 0.35"}}, {{"v", 0.42}, {"label", "Silicon, tight · 0.42"}},
                               {{"v", 0.45}, {"label", "Silicon, hard · 0.45"}}, {{"v", 0.50}, {"label", "Silicon 1N914-style · 0.50"}},
                               {{"v", 0.55}, {"label", "Silicon, LM308 voicing · 0.55"}}, {{"v", 0.62}, {"label", "Silicon, high knee · 0.62"}},
                               {{"v", 0.90}, {"label", "LED · 0.90 (no factory pedal uses this)"}}});
    m["diode_options"] = diodes;
    const double sr = isr_;
    (void)sr;
    const PedalKind K = kind_;
    const bool soft_design = (K == PedalKind::Screamer || K == PedalKind::Muff);
    const bool uses_vd = !(K == PedalKind::TrebleBoost || K == PedalKind::CleanBoost || K == PedalKind::Fuzz || K == PedalKind::Preamp || K == PedalKind::Saturator);
    json blocks = json::array();
    blocks.push_back(block("in", "jack", "Input", "Guitar in", json::array({mval("Oversampling", std::to_string(os_.factor()) + "×", os_.factor(),
                                                                                 "the whole circuit runs at " + fmtNum(isr_ / 1000.0, 0) + " kHz")}), "Input"));
    auto hp = [&](const std::string& id, const std::string& label, const OnePole& f, const std::string& note, const std::string& path, const std::string& section) {
        blocks.push_back(block(id, "filter_hp", label, fmtHz(f.hz()), json::array({mval("Corner", fmtHz(f.hz()), f.hz(), note, path.empty() ? json() : editNum(path, 10, 2000, 1, true))}), section));
    };
    auto svfBlock = [&](const std::string& id, const std::string& label, const Svf& f, const std::string& note, const std::string& section) {
        static const char* kT[] = {"low-pass", "high-pass", "band-pass", "bell", "low shelf", "high shelf"};
        const int t = static_cast<int>(f.type());
        json v = json::array({mval("Type", kT[t], t, "Cytomic trapezoidal SVF"), mval("Frequency", fmtHz(f.hz()), f.hz(), note), mval("Q", fmtNum(f.q(), 2), f.q())});
        if (t >= 3) v.push_back(mval("Gain", fmtDb(f.gainDb()), f.gainDb()));
        blocks.push_back(block(id, t == 0 ? "filter_lp" : (t == 1 ? "filter_hp" : "eq"), label, fmtHz(f.hz()) + (t >= 3 ? " " + fmtDb(f.gainDb()) : ""), std::move(v), section));
    };
    auto diodeBlock = [&](const std::string& label, bool soft, const std::string& where) {
        const bool s = c.clip_shape < 0 ? soft : c.clip_shape == 0;
        json v = json::array();
        v.push_back(mval("Pair", "antiparallel, " + where, 0, ""));
        v.push_back(mval("Knee", fmtNum(c.vd, 2), c.vd, "normalised clipping voltage (1.0 ≈ 1 V at the jack)", editOpts("drive.vd", diodes)));
        v.push_back(mval("Shape", s ? "soft · vd·tanh(x/vd)" : "hard · x/(1+|x|^2.5)^0.4", s ? 0 : 1, c.clip_shape < 0 ? "as designed" : "user override",
                         editOpts("drive.clip", json::array({{{"v", "design"}, {"label", std::string("As designed (") + (soft ? "soft" : "hard") + ")"}},
                                                             {{"v", "soft"}, {"label", "Soft"}}, {{"v", "hard"}, {"label", "Hard"}}}))));
        blocks.push_back(block("diodes", "diodes", label, (s ? "soft · " : "hard · ") + fmtNum(c.vd, 2), std::move(v), "Clipper"));
    };
    auto gainVals = [&](const std::string& note) {
        return json::array({mval("Gain", fmtNum(c.gain, 1) + "× (" + fmtDb(linToDb(c.gain)) + ")", c.gain, note),
                            mval("Drive trim", fmtDb(ov_.drive_db), ov_.drive_db, "user offset on top of the knob mapping", editNum("drive.drive_db", -24, 24, 0.5))});
    };
    switch (K) {
        case PedalKind::Screamer: {
            const float r = c.gain * 4700.0f;
            json v = gainVals("1 + Rfb / 4.7 kΩ in the passband (here Rfb/4.7 kΩ: the clean path is summed separately)");
            v.push_back(mval("Feedback R", fmtOhm(r), r, "51 kΩ + 500 kΩ drive pot (audio taper)"));
            v.push_back(mval("Mid hump HP", fmtHz(c.fb_hp.hz()), c.fb_hp.hz(), "4.7 kΩ / 47 nF leg: only frequencies above it get the gain", editNum("drive.hump_hz", 100, 3000, 5, true)));
            v.push_back(mval("Feedback LP", fmtHz(c.fb_lp.hz()), c.fb_lp.hz(), "51 pF across Rfb: gain rolls off as drive rises", editNum("drive.fb_lp_hz", 500, 20000, 10, true)));
            blocks.push_back(block("opamp", "opamp", "Op-amp gain stage", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            diodeBlock("Feedback diodes", true, "in the op-amp feedback loop");
            blocks.push_back(block("sum", "mix", "Clean + clipped", "x + clip(h)", json::array({mval("Sum", "unity clean path + clipped gain branch", 1, "why a TS keeps its note definition")}), "Clipper"));
            break;
        }
        case PedalKind::TrebleBoost: {
            hp("prehp", "Treble cap", c.pre_hp, "input cap into the base (+12 % full-range bleed)", "drive.pre_hp_hz", "Input");
            json v = gainVals("germanium transistor, common emitter");
            v.push_back(mval("Bias", fmtNum(c.bias, 3), c.bias, "operating point offset (asymmetry)", editNum("drive.bias", -0.5, 0.5, 0.005)));
            v.push_back(mval("Curve", "germanium: soft positive, 0.75·tanh(x/0.75) negative", 0, ""));
            blocks.push_back(block("q1", "transistor", "Germanium transistor", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            break;
        }
        case PedalKind::CleanBoost:
            blocks.push_back(block("fet", "transistor", "FET boost", fmtDb(linToDb(c.gain)), [&] {
                json v = gainVals("0 – 20 dB clean gain");
                v.push_back(mval("Rail limit", "1.8 · tanh(x/1.8)", 1.8, "soft supply-rail limiting"));
                return v;
            }(), "Clipper"));
            break;
        case PedalKind::HardClip: case PedalKind::Metal: case PedalKind::HM2: {
            hp("prehp", "Input HP", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            svfBlock("preeq1", K == PedalKind::HM2 ? "Low boost" : "Pre-EQ", c.pre_eq1, "shapes what the clipper sees", "Input");
            svfBlock("preeq2", K == PedalKind::HM2 ? "High boost" : "Pre-EQ shelf", c.pre_eq2, "", "Input");
            json v = gainVals("op-amp gain into the diodes");
            v.push_back(mval("Bandwidth", fmtHz(c.fb_lp.hz()), c.fb_lp.hz(), "1-pole low-pass on the gain stage", editNum("drive.fb_lp_hz", 500, 20000, 10, true)));
            blocks.push_back(block("opamp", "opamp", "Gain stage", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            diodeBlock("Clipping diodes", false, "to ground");
            break;
        }
        case PedalKind::Rat: {
            hp("prehp", "Input HP", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            json v = gainVals("1 + 2000 · taper(drive)");
            v.push_back(mval("GBW limit", fmtHz(c.fb_lp.hz()), c.fb_lp.hz(), "LM308 gain-bandwidth: 1 MHz / gain, clamped 600 Hz – 20 kHz", editNum("drive.fb_lp_hz", 500, 20000, 10, true)));
            v.push_back(mval("Rails", "±4", 4, "op-amp output swing clamp"));
            blocks.push_back(block("opamp", "opamp", "LM308 op-amp", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            diodeBlock("Clipping diodes", false, "to ground");
            break;
        }
        case PedalKind::Fuzz: {
            hp("prehp", "Input cap", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            json v = gainVals("two-transistor feedback pair");
            v.push_back(mval("Bias", fmtNum(c.bias, 3), c.bias, "germanium operating point", editNum("drive.bias", -0.5, 0.5, 0.005)));
            v.push_back(mval("Bias starve", "0.6 × input envelope", 0.6, "hard playing pulls the bias → sputter and clean-up"));
            blocks.push_back(block("q1", "transistor", "Q1 + Q2 fuzz pair", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            blocks.push_back(block("sat", "clip", "Output saturation", "1.5× · 0.9", json::array({mval("Curve", "0.9 · tanh(1.5x / 0.9)", 0.9, "collector swing limit")}), "Clipper"));
            break;
        }
        case PedalKind::Muff: {
            hp("prehp", "Input HP", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            blocks.push_back(block("st1", "transistor", "Clipping stage 1", fmtDb(linToDb(c.gain)), gainVals("first transistor stage"), "Clipper"));
            diodeBlock("Stage 1 diodes", true, "in the collector feedback");
            blocks.push_back(block("inter", "filter_bp", "Interstage", fmtHz(c.inter_hp.hz()) + " – " + fmtHz(c.inter_lp.hz()),
                                   json::array({mval("High-pass", fmtHz(c.inter_hp.hz()), c.inter_hp.hz()), mval("Low-pass", fmtHz(c.inter_lp.hz()), c.inter_lp.hz())}), "Clipper"));
            blocks.push_back(block("st2", "transistor", "Clipping stage 2", fmtDb(linToDb(c.gain2)),
                                   json::array({mval("Gain", fmtNum(c.gain2, 1) + "×", c.gain2, ""), mval("Trim", fmtDb(ov_.gain2_db), ov_.gain2_db, "", editNum("drive.gain2_db", -24, 24, 0.5))}), "Clipper"));
            diodeBlock("Stage 2 diodes", true, "in the collector feedback");
            break;
        }
        case PedalKind::Percolator: {
            hp("prehp", "Input HP", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            json v = gainVals("germanium transistor");
            v.push_back(mval("Bias", fmtNum(c.bias, 3), c.bias, "", editNum("drive.bias", -0.5, 0.5, 0.005)));
            blocks.push_back(block("q1", "transistor", "Germanium transistor", fmtDb(linToDb(c.gain)), std::move(v), "Clipper"));
            blocks.push_back(block("inter", "filter_hp", "Interstage", fmtHz(c.inter_hp.hz()), json::array({mval("High-pass", fmtHz(c.inter_hp.hz()), c.inter_hp.hz()),
                                                                                                            mval("Gain", fmtNum(c.gain2, 1) + "×", c.gain2)}), "Clipper"));
            diodeBlock("Clipping diodes", false, "to ground (Ge/Si switch sets the knee)");
            break;
        }
        case PedalKind::Preamp: case PedalKind::Saturator: {
            hp("prehp", "Input HP", c.pre_hp, "1-pole coupling", "drive.pre_hp_hz", "Input");
            blocks.push_back(block("st1", "triode", "Stage 1", fmtDb(linToDb(c.gain)), [&] {
                json v = gainVals("asymmetric tube-style stage: x/√(1+x²) up, 1.4·tanh(x/1.4) down");
                return v;
            }(), "Clipper"));
            blocks.push_back(block("st2", "triode", "Stage 2 (inverting)", fmtNum(c.gain2, 1) + "×",
                                   json::array({mval("Gain", fmtNum(c.gain2, 2) + "×", c.gain2, "inverting: the asymmetry alternates"),
                                                mval("Trim", fmtDb(ov_.gain2_db), ov_.gain2_db, "", editNum("drive.gain2_db", -24, 24, 0.5))}), "Clipper"));
            break;
        }
    }
    // Post section (process order of post()).
    if (c.tone_mix >= 0.0f)
        blocks.push_back(block("tone", "tone", "Tone blend", fmtNum(c.tone_mix * 100.0, 0) + " %",
                               json::array({mval("Split", fmtHz(c.tone_lp.hz()), c.tone_lp.hz(), "1-pole LP / HP split, crossfaded by the tone knob"),
                                            mval("Blend", fmtNum(c.tone_mix * 100.0, 0) + " % treble", c.tone_mix)}), "Tone"));
    {
        json v = json::array({mval("Corner", fmtHz(c.post_lp.hz()), c.post_lp.hz(), K == PedalKind::Screamer ? "the tone control" : (K == PedalKind::Rat ? "the filter knob" : "output low-pass"),
                                   editNum("drive.post_lp_hz", 300, 20000, 10, true)),
                              mval("Q", fmtNum(c.post_lp.q(), 2), c.post_lp.q())});
        blocks.push_back(block("postlp", "filter_lp", K == PedalKind::Screamer ? "Tone" : (K == PedalKind::Rat ? "Filter" : "Output LP"), fmtHz(c.post_lp.hz()), std::move(v), "Tone"));
    }
    if (c.eq) {
        json v = json::array({mval("Bass", fmtDb(c.eq_bass.gainDb()) + " @ " + fmtHz(c.eq_bass.hz()), c.eq_bass.gainDb(), "low shelf"),
                              mval("Mid", fmtDb(c.eq_mid.gainDb()) + " @ " + fmtHz(c.eq_mid.hz()), c.eq_mid.gainDb(), "bell, Q 0.8"),
                              mval("Treble", fmtDb(c.eq_treb.gainDb()) + " @ " + fmtHz(c.eq_treb.hz()), c.eq_treb.gainDb(), "high shelf")});
        blocks.push_back(block("eq", "eq", "EQ", "3-band", std::move(v), "Tone"));
    }
    if (c.cab)
        blocks.push_back(block("cab", "speaker", "Speaker sim", "LP 4.8 kHz", json::array({mval("Filters", "LP 4.8 kHz · bell 2.4 kHz -3 dB · HP 75 Hz", 0, "SansAmp-style speaker emulation switch")}), "Tone"));
    {
        json v = json::array({mval("Output", fmtDb(linToDb(c.out)), c.out, "level knob"), mval("Level trim", fmtDb(ov_.level_db), ov_.level_db, "", editNum("drive.level_db", -24, 24, 0.5))});
        if (c.mix < 0.999f) v.push_back(mval("Mix", fmtNum(c.mix * 100.0, 0) + " %", c.mix, "dry/wet at the host rate"));
        blocks.push_back(block("out", "out", "Level", fmtDb(linToDb(c.out)), std::move(v), "Output"));
    }
    (void)uses_vd;
    (void)soft_design;
    m["values"] = {{"gain", c.gain}, {"gain2", c.gain2}, {"vd", c.vd}, {"bias", c.bias}, {"out", c.out}, {"mix", c.mix}, {"clip_shape", c.clip_shape},
                   {"pre_hp_hz", c.pre_hp.hz()}, {"fb_hp_hz", c.fb_hp.hz()}, {"fb_lp_hz", c.fb_lp.hz()}, {"post_lp_hz", c.post_lp.hz()}};
    m["blocks"] = std::move(blocks);
    return m.dump();
}

std::string DrivePedalComponent::curvesJson() const {
    const Controls k = readControls();
    Ch c;
    configure(c, k);
    json out = json::object();
    json x = json::array(), y = json::array();
    for (int i = 0; i <= 160; ++i) {
        const float xi = -1.0f + static_cast<float>(i) / 80.0f;
        x.push_back(xi);
        y.push_back(std::round(c.staticCurve(xi, kind_) * 1000.0) / 1000.0);
    }
    out["unit"] = {{"x", x}, {"y", y}, {"note", "input (≈ V at the jack) → clipper output, filters at unity"}};
    const auto fr = logFreqs();
    json f = json::array();
    for (double v : fr) f.push_back(std::round(v * 10.0) / 10.0);
    auto response = [&](auto&& fn) {
        Ch t = c;
        t.reset();
        std::vector<float> h(16384);
        for (std::size_t i = 0; i < h.size(); ++i) h[i] = fn(t, i == 0 ? 1.0f : 0.0f);
        auto db = irMagnitudeDb(h, isr_, fr);
        double mx = -300.0;
        for (double v : db) mx = std::max(mx, v);
        json a = json::array();
        for (double v : db) a.push_back(std::round((v - mx) * 100.0) / 100.0);
        return std::make_pair(a, mx);
    };
    const PedalKind K = kind_;
    auto pre = response([&](Ch& t, float v) { return t.preFilter(v, K); });
    auto post = response([](Ch& t, float v) { return t.post(v); });
    out["pre"] = {{"f", f}, {"db", pre.first}, {"peak_db", pre.second}};
    out["tone"] = {{"f", f}, {"db", post.first}, {"peak_db", post.second}};
    return out.dump();
}

} // namespace

bool isTubeAmpTable(int32_t t) noexcept {
    switch (t) {
        case 16000: case 30000: case 34000: case 38000: case 49000: case 54000: case 56000: case 62000:
        case 79000: case 80000: case 81000: case 87000: case 91000: case 92000: case 96000: case 101000:
        case 122000: case 134000: case 150000: case 151000: case 152000: case 162000: case 163000:
        case 172000: case 176000: case 177000: case 300000:
            return true;
        default:
            return false;
    }
}

bool isDrivePedalTable(int32_t t) noexcept {
    switch (t) {
        case 15000: case 167000: case 40000: case 65000: case 17000: case 42000: case 41000: case 166000:
        case 169000: case 43000: case 174000: case 44000: case 46000: case 45000: case 170000: case 84000:
        case 111000: case 165000: case 173000:
            return true;
        default:
            return false;
    }
}

std::string engineDescription(int32_t t) {
    if (isTubeAmpTable(t)) {
        const AmpVoice v = makeVoice(t);
        int max_n = 0;
        for (const auto& c : v.ch) max_n = std::max(max_n, c.n);
        const char* stack = "FMV tone stack";
        if (&v.stack == &v.stack) {
            if (v.stack.R4 == stacks::kFender.R4 && v.stack.C1 == stacks::kFender.C1) stack = "Fender tone stack";
            else if (v.stack.C1 == stacks::kMarshall.C1 && v.stack.R4 == stacks::kMarshall.R4) stack = "Marshall tone stack";
            else if (v.stack.C1 == stacks::kModern.C1) stack = "modern high-gain tone stack";
            else if (v.stack.C1 == stacks::kVox.C1) stack = "top-boost tone stack";
            else if (v.stack.R4 == stacks::kHiwatt.R4) stack = "mid-forward tone stack";
            else if (v.stack.C3 == stacks::kBass.C3) stack = "bass tone stack";
        }
        std::string d = v.solid_state ? "Solid-state preamp" : (std::to_string(max_n) + "-stage 12AX7 preamp (Koren tube model, dynamic cathode bias, 8x)");
        d += " · ";
        d += stack;
        d += v.solid_state ? " · solid-state power amp" : (v.power.class_a ? " · class-A power amp w/ sag" : " · push-pull power amp w/ sag");
        if (!v.solid_state) d += " · speaker load & power compression";
        return d;
    }
    switch (t) {
        case 15000: case 167000: return "Op-amp soft clipper · 720 Hz mid hump · tone filter";
        case 40000: return "Germanium treble booster";
        case 65000: return "Clean FET boost";
        case 17000: case 84000: return "Transistor + hard diode clipper · tone blend";
        case 42000: return "LM308 high-gain op-amp · hard diode clipper · filter";
        case 41000: case 166000: case 169000: return "Two-transistor fuzz with bias starve";
        case 43000: return "Dual cascaded clipping stages · scooped tone stack";
        case 174000: return "Germanium/silicon percolator fuzz";
        case 44000: case 46000: return "High-gain op-amp · pre-EQ · hard clipper · parametric EQ";
        case 45000: case 170000: case 111000: return "Tube-style preamp stages · Baxandall EQ";
        case 165000: return "Dual-band boosted HM-style distortion";
        case 173000: return "Analog character saturation · EQ";
        default: return "";
    }
}

int engineOversampling(int32_t t, double host_sr) {
    if (isTubeAmpTable(t)) return host_sr >= 88000.0 ? 4 : 8;
    if (isDrivePedalTable(t)) return host_sr >= 88000.0 ? 2 : 4;
    return 0;
}

void prewarmTubeAmpModel(int32_t table_id, const std::string& model_json, double sample_rate) {
    const auto* s = catalog::findByTableId(table_id);
    if (!s || !isTubeAmpTable(table_id) || model_json.empty()) return;
    TubeAmpComponent c(*s);
    c.setStringParameter(model::kModelParam, model_json);
    c.prepare(sample_rate, 512); // computes (and caches) the trims for this voicing
}

std::unique_ptr<IComponent> makeTubeAmp(const catalog::ComponentSchema& schema) {
    return std::make_unique<TubeAmpComponent>(schema);
}

std::unique_ptr<IComponent> makeDrivePedal(const catalog::ComponentSchema& schema) {
    PedalKind k = PedalKind::HardClip;
    switch (schema.table_id) {
        case 15000: case 167000: k = PedalKind::Screamer; break;
        case 40000: k = PedalKind::TrebleBoost; break;
        case 65000: k = PedalKind::CleanBoost; break;
        case 17000: case 84000: k = PedalKind::HardClip; break;
        case 42000: k = PedalKind::Rat; break;
        case 41000: case 166000: case 169000: k = PedalKind::Fuzz; break;
        case 43000: k = PedalKind::Muff; break;
        case 174000: k = PedalKind::Percolator; break;
        case 44000: case 46000: k = PedalKind::Metal; break;
        case 45000: case 170000: case 111000: k = PedalKind::Preamp; break;
        case 165000: k = PedalKind::HM2; break;
        case 173000: k = PedalKind::Saturator; break;
        default: break;
    }
    return std::make_unique<DrivePedalComponent>(schema, k);
}

} // namespace rig
