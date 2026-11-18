// Cabinet rack unit (900100). Every impulse it convolves is a cabinet/mic/position impulse from
// the cabinet library (CabinetCatalog.hpp). The only non-impulse path is the "no content" fallback,
// a fixed speaker-voicing filter that the unit labels as an approximation in /api/rack and the UI.
#include "CabinetUnit.hpp"
#include "CabinetCatalog.hpp"
#include "ConvolutionEngine.hpp"
#include "DSPPrimitives.hpp"
#include "LooperUnit.hpp"
#include "PitchDoublerUnits.hpp"
#include "ToneMatchAndCalibration.hpp"
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>

namespace rig {
namespace {

using catalog::ParamKind;
using catalog::ParamSchema;
using dsp::cab::CabInfo;
using dsp::cab::Catalog;
using dsp::cab::MicInfo;

constexpr ParamSchema kCabParams[] = {
    {"blend", "Mic Blend", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""},
    {"room", "Room", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"alignB", "Mic B Distance", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"phaseB", "Mic B Invert", ParamKind::Bool, 0.0f, 0.0f, 1.0f, "", ""},
    {"width", "Width", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"lowcut", "Low Cut", ParamKind::Float, 0.0f, 0.0f, 1.0f, "", ""},
    {"highcut", "High Cut", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
    {"level", "Level", ParamKind::Float, 0.5f, 0.0f, 1.0f, "", ""}, // 0.5 = 0 dB, ±12 dB
    {"doubler", "Doubler", ParamKind::Float, 0.0f, 0.0f, 1.0f, "Room & Stereo", ""},
    {"doublerDelay", "Doubler Tightness", ParamKind::Float, 0.45f, 0.0f, 1.0f, "Room & Stereo", ""},
    {"air", "Amp in Room", ParamKind::Float, 0.0f, 0.0f, 1.0f, "Room & Stereo", ""},
    {"demic", "De-Mic Body", ParamKind::Float, 0.0f, 0.0f, 1.0f, "Room & Stereo", ""},
    {"Pwr", "On/Off", ParamKind::Float, 1.0f, 0.0f, 1.0f, "", ""},
};

const std::vector<catalog::ComponentSchema>& studioSchemas() {
    static const std::vector<catalog::ComponentSchema> v = {
        {kCabinetUnitTable, "Cabinet", "Speaker Cabinet & Microphones", "HeadroomCabinet", catalog::EngineFamily::Convolution, -1, false,
         std::span<const ParamSchema>(kCabParams)},
        looperUnitSchema(),
        transposeUnitSchema(),
    };
    return v;
}

// Amp-Matched Cab's (156000) level at Volume 0.5 (ampMatchedCabVolume(0.5)), so a Cabinet set to a matched
// cabinet at Level 0 dB is sample-identical to the 156000 it replaces as the auto-cab.
constexpr float kMatchedParityVolume = 1.15f;
constexpr float kMaxAlignMs = 1.5f; // mic B time offset: up to ~50 cm further from the speaker

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

struct Resolved {
    const CabInfo* cab = nullptr;
    const MicInfo* micA = nullptr;
    std::string posA;
    const MicInfo* micB = nullptr; // null = off
    std::string posB;
};

class CabinetUnit final : public SchemaComponentBase {
public:
    using SchemaComponentBase::SchemaComponentBase;

    // ───── selection (control thread) ─────
    static bool isSelector(std::string_view id) noexcept { return id == "cab" || id == "micA" || id == "posA" || id == "micB" || id == "posB"; }
    static bool isSelectionString(std::string_view id) noexcept {
        return id == "cab_id" || id == "micA_id" || id == "posA_id" || id == "micB_id" || id == "posB_id";
    }

    Resolved resolve() const {
        Resolved r;
        const auto& cat = Catalog::get();
        r.cab = cat.find(cab_);
        if (!r.cab && cab_.empty() && !cat.cabs().empty()) r.cab = defaultCab();
        if (!r.cab) return r;
        r.micA = micA_.empty() ? Catalog::defaultMic(*r.cab) : Catalog::closestMic(*r.cab, micA_);
        if (r.micA) r.posA = hasPos(*r.micA, posA_) ? posA_ : Catalog::defaultPosition(*r.micA);
        if (!micB_.empty() && r.cab->mics.size() > 1) {
            r.micB = Catalog::closestMic(*r.cab, micB_);
            if (r.micB) r.posB = hasPos(*r.micB, posB_) ? posB_ : Catalog::defaultPosition(*r.micB);
        }
        return r;
    }
    bool missing() const { return !cab_.empty() && !Catalog::get().find(cab_); }

    void choose(const std::string& cab_in, const std::string& micA, const std::string& posA, const std::string& micB, const std::string& posB) {
        const std::string cab = cab_in.empty() ? cab_in : dsp::cab::canonicalCabId(cab_in);
        // Omitted (empty) fields keep the current choice, so a partial update (e.g. only Mic A) never
        // resets the cabinet or the other mic. Mics invalid for a newly chosen cab fall back in resolve().
        if (!cab.empty() && cab != cab_) {  // a different cabinet: start from its defaults unless mics are given
            cab_ = cab; micA_ = micA; posA_ = posA; micB_ = micB; posB_ = posB;
            store();
            return;
        }
        if (!micA.empty()) micA_ = micA;
        if (!posA.empty()) posA_ = posA;
        if (!micB.empty()) micB_ = micB;
        if (!posB.empty()) posB_ = posB;
        store();
    }

    std::vector<ExtraParam> extraParams() const override {
        const auto& cat = Catalog::get();
        const Resolved r = resolve();
        std::vector<ExtraParam> out;
        ExtraParam cab{"cab", "Cabinet", {}, 0.0f, {}};
        for (const auto& c : cat.cabs()) {
            cab.labels.push_back(c.group + " · " + c.label);
            cab.groups.push_back(c.group);
        }
        if (missing() || cat.cabs().empty()) {
            cab.labels.push_back("Missing · " + (cab_.empty() ? std::string("no cabinet content installed") : cab_));
            cab.groups.push_back("Missing");
        }
        out.push_back(std::move(cab));
        ExtraParam ma{"micA", "Mic A", {}, 0.0f, {}};
        ExtraParam mb{"micB", "Mic B", {"Off"}, 0.0f, {}};
        if (r.cab) {
            for (const auto& m : r.cab->mics) {
                ma.labels.push_back(m.label);
                ma.groups.push_back(m.family);
                mb.labels.push_back(m.label);
            }
        }
        if (ma.labels.empty()) ma.labels.push_back("—");
        out.push_back(std::move(ma));
        out.push_back(posParam("posA", "Position A", r.micA));
        out.push_back(std::move(mb));
        out.push_back(posParam("posB", "Position B", r.micB));
        return out;
    }

    void setParameter(std::string_view id, float value) override {
        if (!isSelector(id)) { SchemaComponentBase::setParameter(id, value); return; }
        const int idx = static_cast<int>(std::lround(value));
        const auto& cat = Catalog::get();
        Resolved r = resolve();
        if (id == "cab") {
            if (idx < 0 || idx >= static_cast<int>(cat.cabs().size())) return; // "Missing" placeholder: keep
            const CabInfo& c = cat.cabs()[static_cast<std::size_t>(idx)];
            if (r.cab && c.id == r.cab->id && !missing()) return;
            const bool was_matched = r.cab && r.cab->kind == "matched";
            cab_ = c.id;
            if (c.kind == "matched") {
                // A and B are alternative captures of the matched cabinet (the 156000 Amp-Matched Cab's "Cab A/B"), not a mic
                // pair: several B files are polarity-inverted or 1.6 ms late against A. Start on A alone.
                micA_ = "A"; posA_ = "fixed";
                micB_.clear(); posB_ = "fixed";
            } else {
                const MicInfo* a = (was_matched || micA_.empty()) ? Catalog::defaultMic(c) : Catalog::closestMic(c, micA_);
                micA_ = a ? a->id : "";
                if (a && !hasPos(*a, posA_)) posA_ = Catalog::defaultPosition(*a);
                const MicInfo* b = (!was_matched && !micB_.empty() && c.mics.size() > 1) ? Catalog::closestMic(c, micB_) : nullptr;
                micB_ = b ? b->id : "";
                if (b && !hasPos(*b, posB_)) posB_ = Catalog::defaultPosition(*b);
            }
        } else if (!r.cab) {
            return;
        } else if (id == "micA" || id == "micB") {
            const bool isB = id == "micB";
            const int mi = isB ? idx - 1 : idx;
            if (isB && mi < 0) { micB_.clear(); store(); return; }
            if (mi < 0 || mi >= static_cast<int>(r.cab->mics.size())) return;
            const MicInfo& m = r.cab->mics[static_cast<std::size_t>(mi)];
            std::string& pos = isB ? posB_ : posA_;
            const std::string keep = isB ? r.posB : r.posA;
            (isB ? micB_ : micA_) = m.id;
            pos = hasPos(m, keep) ? keep : Catalog::defaultPosition(m);
            if (!isB && micA_.empty()) micA_ = m.id;
            if (isB && micA_.empty() && r.micA) micA_ = r.micA->id;
        } else { // posA / posB
            const MicInfo* m = id == "posA" ? r.micA : r.micB;
            if (!m || idx < 0 || idx >= static_cast<int>(m->positions.size())) return;
            (id == "posA" ? posA_ : posB_) = m->positions[static_cast<std::size_t>(idx)];
            if (id == "posA" && micA_.empty()) micA_ = m->id;
        }
        if (cab_.empty() && r.cab) cab_ = r.cab->id;
        store();
    }

    float getParameter(std::string_view id) const override {
        if (!isSelector(id)) return SchemaComponentBase::getParameter(id);
        const auto& cat = Catalog::get();
        const Resolved r = resolve();
        if (id == "cab") {
            if (missing() || !r.cab) return static_cast<float>(cat.cabs().size());
            return static_cast<float>(std::max(0, cat.indexOf(r.cab->id)));
        }
        auto micIndex = [&](const MicInfo* m) {
            if (!r.cab || !m) return -1;
            for (std::size_t i = 0; i < r.cab->mics.size(); ++i) if (&r.cab->mics[i] == m) return static_cast<int>(i);
            return -1;
        };
        auto posIndex = [](const MicInfo* m, const std::string& p) {
            if (!m) return 0;
            for (std::size_t i = 0; i < m->positions.size(); ++i) if (m->positions[i] == p) return static_cast<int>(i);
            return 0;
        };
        if (id == "micA") return static_cast<float>(std::max(0, micIndex(r.micA)));
        if (id == "micB") return static_cast<float>(r.micB ? micIndex(r.micB) + 1 : 0);
        if (id == "posA") return static_cast<float>(posIndex(r.micA, r.posA));
        return static_cast<float>(posIndex(r.micB, r.posB));
    }

    void setStringParameter(std::string_view id, std::string_view value) override {
        if (id == "cab_id") { // older rigs may carry a legacy cabinet id: store and use the current one
            cab_ = dsp::cab::canonicalCabId(std::string(value));
            SchemaComponentBase::setStringParameter(id, cab_);
            return;
        }
        SchemaComponentBase::setStringParameter(id, value);
        const std::string v(value);
        if (id == "micA_id") micA_ = v;
        else if (id == "posA_id") posA_ = v;
        else if (id == "micB_id") micB_ = v;
        else if (id == "posB_id") posB_ = v;
    }

    float levelDb() const { return (std::clamp(getParamOr("level", 0.5f), 0.0f, 1.0f) - 0.5f) * 24.0f; }

    bool paramDisplay(std::string_view id, std::string& out) const override {
        char b[48];
        const float v = std::clamp(getParamOr(id, 0.0f), 0.0f, 1.0f);
        if (id == "level") std::snprintf(b, sizeof(b), "%+.1f dB", static_cast<double>(levelDb()));
        else if (id == "blend") {
            const int a = static_cast<int>(std::lround(100.0 * std::pow(std::cos(v * std::numbers::pi * 0.5), 2.0)));
            std::snprintf(b, sizeof(b), "A %d · B %d", a, 100 - a);
        } else if (id == "room" || id == "width") std::snprintf(b, sizeof(b), "%d%%", static_cast<int>(std::lround(v * 100.0f)));
        else if (id == "doubler" || id == "air" || id == "demic") {
            if (v <= 0.005f) std::snprintf(b, sizeof(b), "Off");
            else std::snprintf(b, sizeof(b), "%d%%", static_cast<int>(std::lround(v * 100.0f)));
        } else if (id == "doublerDelay") {
            std::snprintf(b, sizeof(b), "%.1f ms", 6.0 + 24.0 * static_cast<double>(v));
        } else if (id == "alignB") std::snprintf(b, sizeof(b), "%.2f ms · %.0f cm", static_cast<double>(v * kMaxAlignMs), static_cast<double>(v * kMaxAlignMs * 34.3f));
        else if (id == "lowcut") {
            if (v <= 0.005f) std::snprintf(b, sizeof(b), "Off");
            else std::snprintf(b, sizeof(b), "%.0f Hz", 20.0 * std::pow(15.0, static_cast<double>(v)));
        } else if (id == "highcut") {
            if (v >= 0.995f) std::snprintf(b, sizeof(b), "Off");
            else std::snprintf(b, sizeof(b), "%.1f kHz", 2.0 * std::pow(10.0, static_cast<double>(v)));
        } else return false;
        out = b;
        return true;
    }

    std::string status() const {
        if (Catalog::get().cabs().empty()) return "no_content";
        if (missing()) return "missing";
        if (!ir_) return "approximation";
        return "ok";
    }

    std::string json() const {
        const Resolved r = resolve();
        std::ostringstream o;
        o << "\"cabinet\":{\"status\":\"" << status() << "\",\"cab_id\":\"" << esc(r.cab ? r.cab->id : cab_) << "\",\"stored_cab_id\":\"" << esc(cab_)
          << "\",\"kind\":\"" << (r.cab ? r.cab->kind : std::string()) << "\",\"label\":\"" << esc(r.cab ? r.cab->label : cab_) << "\",\"group\":\""
          << esc(r.cab ? r.cab->group : std::string()) << "\",\"speakers\":\"" << esc(r.cab ? r.cab->speakers : std::string("4x12"))
          << "\",\"pair\":\"" << esc(r.cab ? r.cab->pair : std::string()) << "\",\"room_available\":" << (r.cab && r.cab->room ? "true" : "false");
        auto mic = [&](const char* key, const MicInfo* m, const std::string& pos) {
            o << ",\"" << key << "\":";
            if (!m) { o << "null"; return; }
            o << "{\"id\":\"" << esc(m->id) << "\",\"label\":\"" << esc(m->label) << "\",\"family\":\"" << esc(m->family) << "\",\"note\":\"" << esc(m->note)
              << "\",\"position\":\"" << esc(pos) << "\",\"position_label\":\"" << esc(dsp::cab::positionLabel(pos)) << "\"}";
        };
        mic("mic_a", r.micA, r.posA);
        mic("mic_b", r.micB, r.posB);
        o << ",\"room_stereo\":{\"doubler\":" << std::clamp(getParamOr("doubler", 0.0f), 0.0f, 1.0f)
          << ",\"doubler_delay\":" << std::clamp(getParamOr("doublerDelay", 0.45f), 0.0f, 1.0f)
          << ",\"air\":" << std::clamp(getParamOr("air", 0.0f), 0.0f, 1.0f)
          << ",\"demic\":" << std::clamp(getParamOr("demic", 0.0f), 0.0f, 1.0f) << "}";
        o << ",\"approximation\":" << (ir_ ? "false" : "true");
        if (!ir_) o << ",\"approximation_note\":\"No cabinet impulse found for this selection: a fixed speaker-voicing filter (high-pass 80 Hz, low-pass 5 kHz) stands in. It is not a microphone model.\"";
        o << "}";
        return o.str();
    }

    SpeakerImpedanceProfile speakerImpedanceProfile() const noexcept override {
        return isBypassed() ? SpeakerImpedanceProfile{} : spk_profile_;
    }
    // Series pipelining (RackGraph): an IR that is the same on both sides (Width 0) convolves both channels alike; the
    // doubler and the room (the spatial stage) make two sides.
    bool monoPreserving() const noexcept override { return ir_mono_ && !stereo_room_on_; }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* old = dynamic_cast<CabinetUnit*>(&previous);
        if (!old || host_sr_ != old->host_sr_) return;
        // Input history is IR-independent: keep it, and re-aim the pending tail at the new IR so a cab,
        // mic or position change continues the cabinet/room tail instead of restarting it.
        std::swap(conv_, old->conv_);
        if (ir_ && ir_ != old->ir_) {
            conv_[0].retarget(ir_->left);
            conv_[1].retarget(ir_->right);
        }
        for (int c = 0; c < 2; ++c) {
            lowcut_[c].adoptRuntimeStateFrom(old->lowcut_[c]);
            highcut_[c].adoptRuntimeStateFrom(old->highcut_[c]);
            for (int k = 0; k < 3; ++k) voice_[c][k].adoptRuntimeStateFrom(old->voice_[c][k]);
        }
        spatial_.adoptFrom(old->spatial_);
        level_cur_ = old->level_cur_;
        level_primed_ = old->level_primed_;
    }

protected:
    void onPrepare(double sr, uint32_t) override {
        spatial_.prepare(sr);
        for (int c = 0; c < 2; ++c) {
            voice_[c][0].setHighPass(sr, 80.0f, 0.7071f);
            voice_[c][1].setLowPass(sr, 5000.0f, 0.7071f);
            voice_[c][2].setLowPass(sr, 5000.0f, 0.7071f);
        }
        // Re-preparing a live unit (sample-rate change) rebuilds its IR and filters; a fresh unit waits
        // for processControlRate() so it never builds the default selection first.
        if (controlled_) { ir_key_.clear(); onProcessControlRate(); }
        onReset();
    }

    void onReset() override {
        for (auto& c : conv_) c.reset();
        for (int c = 0; c < 2; ++c) {
            lowcut_[c].reset();
            highcut_[c].reset();
            for (auto& v : voice_[c]) v.reset();
        }
        spatial_.reset();
        if (controlled_) syncSpatialParams();
        level_primed_ = false; // a fresh unit starts at its level instead of gliding to it
    }

    void onProcessControlRate() override {
        controlled_ = true;
        const Resolved r = resolve();
        spk_profile_ = r.cab ? dsp::cab::speakerProfileForCab(r.cab->id, r.cab->speakers)
                             : dsp::cab::speakerProfileForCab(cab_);
        const std::string key = irKey(r);
        if (key != ir_key_) {
            ir_key_ = key;
            ir_ = r.cab ? dsp::CabinetIRCache::getOrBuild("cab900100|" + key, host_sr_, [&] { return buildIR(r); }) : nullptr;
            ir_mono_ = !ir_ || ir_->left.sameAs(ir_->right);
        }
        const float lc = std::clamp(getParamOr("lowcut", 0.0f), 0.0f, 1.0f);
        const float hc = std::clamp(getParamOr("highcut", 1.0f), 0.0f, 1.0f);
        lowcut_on_ = lc > 0.005f;
        highcut_on_ = hc < 0.995f;
        for (int c = 0; c < 2; ++c) {
            lowcut_[c].setHighPass(host_sr_, 20.0f * std::pow(15.0f, lc), 0.7071f);   // 20 → 300 Hz
            highcut_[c].setLowPass(host_sr_, 2000.0f * std::pow(10.0f, hc), 0.7071f);  // 2 → 20 kHz
        }
        syncSpatialParams();
        level_target_ = std::pow(10.0f, levelDb() / 20.0f);
    }

    void onProcessAudio(AudioBlock& block) override {
        const float* inL = block.in[0];
        const float* inR = (block.num_channels > 1 && block.in[1]) ? block.in[1] : inL;
        float* outL = block.out[0];
        float* outR = (block.num_channels > 1 && block.out[1]) ? block.out[1] : nullptr;
        const dsp::CabinetIR* ir = ir_.get();
        const float k = 1.0f - std::exp(-1.0f / (0.02f * static_cast<float>(host_sr_)));
        if (!level_primed_) { level_cur_ = level_target_; level_primed_ = true; }
        for (uint32_t i = 0; i < block.num_frames; ++i) {
            float l, r;
            if (ir) {
                l = conv_[0].process(inL[i], ir->left);
                r = conv_[1].process(inR[i], ir->right);
            } else { // labelled approximation: no impulse available
                l = voice_[0][2].process(voice_[0][1].process(voice_[0][0].process(inL[i]))) * 0.9f;
                r = voice_[1][2].process(voice_[1][1].process(voice_[1][0].process(inR[i]))) * 0.9f;
            }
            if (lowcut_on_) { l = lowcut_[0].process(l); r = lowcut_[1].process(r); }
            if (highcut_on_) { l = highcut_[0].process(l); r = highcut_[1].process(r); }
            level_cur_ += k * (level_target_ - level_cur_);
            outL[i] = l * level_cur_;
            if (outR) outR[i] = r * level_cur_;
        }
        if (stereo_room_on_) {
            if (outR) {
                spatial_.processStereo(outL, outR, block.num_frames);
            } else {
                std::array<float, 64> tmpR{};
                for (uint32_t off = 0; off < block.num_frames; off += 64) {
                    const uint32_t n = std::min<uint32_t>(64, block.num_frames - off);
                    for (uint32_t i = 0; i < n; ++i) tmpR[i] = outL[off + i];
                    spatial_.processStereo(outL + off, tmpR.data(), n);
                }
            }
        }
    }

private:
    void syncSpatialParams() noexcept {
        const float dbl = std::clamp(getParamOr("doubler", 0.0f), 0.0f, 1.0f);
        const float dbl_dly = std::clamp(getParamOr("doublerDelay", 0.45f), 0.0f, 1.0f);
        const float air = std::clamp(getParamOr("air", 0.0f), 0.0f, 1.0f);
        const float demic = std::clamp(getParamOr("demic", 0.0f), 0.0f, 1.0f);
        const bool dbl_on = dbl > 0.001f;
        const bool rm_on = air > 0.001f || demic > 0.001f;
        stereo_room_on_ = dbl_on || rm_on;
        spatial_.setDoublerEnabled(dbl_on);
        spatial_.setDoublerWidth(dbl);
        spatial_.setDoublerDelayMs(6.0f + 24.0f * dbl_dly);
        spatial_.setDoublerDetune(2.0f + 8.0f * dbl_dly);
        spatial_.setRoomEnabled(rm_on);
        spatial_.setRoomAmount(air);
        spatial_.setDeMicAmount(demic);
    }

    static bool hasPos(const MicInfo& m, const std::string& p) { return std::find(m.positions.begin(), m.positions.end(), p) != m.positions.end(); }
    static const CabInfo* defaultCab() {
        const auto& cat = Catalog::get();
        for (const auto& c : cat.cabs()) if (c.kind == "studio") return &c;
        return cat.cabs().empty() ? nullptr : &cat.cabs().front();
    }
    ExtraParam posParam(const char* id, const char* name, const MicInfo* m) const {
        ExtraParam p{id, name, {}, 0.0f, {}};
        if (m) for (const auto& s : m->positions) p.labels.push_back(dsp::cab::positionLabel(s));
        if (p.labels.empty()) p.labels.push_back("—");
        return p;
    }
    void store() {
        SchemaComponentBase::setStringParameter("cab_id", cab_);
        SchemaComponentBase::setStringParameter("micA_id", micA_);
        SchemaComponentBase::setStringParameter("posA_id", posA_);
        SchemaComponentBase::setStringParameter("micB_id", micB_);
        SchemaComponentBase::setStringParameter("posB_id", posB_);
    }

    std::string irKey(const Resolved& r) const {
        if (!r.cab) return {};
        auto q = [&](const char* id, float def) { return std::to_string(static_cast<int>(std::lround(getParamOr(id, def) * 1000.0f))); };
        std::string k = r.cab->id + "|" + (r.micA ? r.micA->id : "") + "|" + r.posA + "|" + (r.micB ? r.micB->id : "-") + "|" + r.posB;
        k += "|" + q("blend", 0.5f) + "|" + q("room", 0.0f) + "|" + q("alignB", 0.0f) + "|" + q("phaseB", 0.0f) + "|" + q("width", 0.0f);
        return k;
    }

    std::shared_ptr<const dsp::CabinetIR> buildIR(const Resolved& r) const {
        const double sr = host_sr_;
        dsp::IRMixer mix(sr);
        bool any = false;
        const float width = std::clamp(getParamOr("width", 0.0f), 0.0f, 1.0f);
        const float blend = std::clamp(getParamOr("blend", 0.5f), 0.0f, 1.0f);
        const bool two = r.micB != nullptr;
        const float gA = two ? std::cos(blend * std::numbers::pi_v<float> * 0.5f) : 1.0f;
        const float gB = two ? std::sin(blend * std::numbers::pi_v<float> * 0.5f) : 0.0f;
        auto add = [&](const std::string& rel, float gain, float pan, float delay_ms) {
            if (rel.empty() || std::abs(gain) < 1e-4f) return;
            if (auto ir = dsp::CabinetIRLibrary::load(rel, sr)) { mix.add(*ir, gain, pan, delay_ms); any = true; }
        };
        if (r.micA) add(Catalog::micFile(*r.cab, *r.micA, r.posA), gA, two ? 0.5f - 0.5f * width : 0.5f, 0.0f);
        if (two) {
            const float pol = getParamOr("phaseB", 0.0f) >= 0.5f ? -1.0f : 1.0f;
            add(Catalog::micFile(*r.cab, *r.micB, r.posB), gB * pol, 0.5f + 0.5f * width,
                kMaxAlignMs * std::clamp(getParamOr("alignB", 0.0f), 0.0f, 1.0f));
        }
        const float room = std::clamp(getParamOr("room", 0.0f), 0.0f, 1.0f);
        if (room > 0.001f && any) {
            const auto files = Catalog::roomFiles(*r.cab);
            const float g = 0.9f * room * room / std::sqrt(static_cast<float>(std::max<std::size_t>(1, files.size())));
            for (std::size_t i = 0; i < files.size(); ++i) {
                const float pan = files.size() > 1 ? (i == 0 ? 0.5f - 0.3f * width : 0.5f + 0.3f * width) : 0.5f;
                add(files[i], g, pan, 0.0f);
            }
        }
        if (!any) return nullptr;
        return mix.finish(width > 0.005f, kMatchedParityVolume);
    }

    std::string cab_, micA_, posA_, micB_, posB_;
    std::array<dsp::PartitionedConvolver, 2> conv_{};
    std::shared_ptr<const dsp::CabinetIR> ir_{};
    std::string ir_key_{};
    std::array<dsp::BiquadFilter, 2> lowcut_{}, highcut_{};
    std::array<std::array<dsp::BiquadFilter, 3>, 2> voice_{};
    StereoDoublerRoomProcessor spatial_{};
    bool lowcut_on_ = false, highcut_on_ = false, stereo_room_on_ = false, controlled_ = false, level_primed_ = false;
    bool ir_mono_ = true; // ir_ is the same impulse on both sides
    float level_target_ = 1.0f, level_cur_ = 1.0f;
    SpeakerImpedanceProfile spk_profile_{};
};

} // namespace

std::unique_ptr<IComponent> makeCabinetUnit(const catalog::ComponentSchema& schema) { return std::make_unique<CabinetUnit>(schema); }

namespace cabinet {
bool isCabinetUnit(const IComponent& c) noexcept { return dynamic_cast<const CabinetUnit*>(&c) != nullptr; }

bool select(IComponent& unit, const std::string& cab_id, const std::string& mic_a, const std::string& pos_a, const std::string& mic_b,
            const std::string& pos_b) {
    auto* u = dynamic_cast<CabinetUnit*>(&unit);
    if (!u) return false;
    u->choose(cab_id, mic_a, pos_a, mic_b, pos_b);
    return true;
}

bool selectMatched(IComponent& unit, int matched_index) {
    if (matched_index < 0 || matched_index >= static_cast<int>(dsp::cab_ids::kMatched.size())) return false;
    const std::string id = std::string("matched:") + dsp::cab_ids::kMatched[static_cast<std::size_t>(matched_index)].id;
    return select(unit, id, "A", "fixed", "", ""); // == Amp-Matched Cab (156000) at its defaults (capture A, no room)
}

bool apply(IComponent& unit, const Equivalent& eq) {
    if (!select(unit, eq.cab, eq.mic_a, eq.pos_a, eq.mic_b, eq.pos_b)) return false;
    for (const auto& [id, v] : eq.params) unit.setParameter(id, v);
    return true;
}

bool equivalentOf(const IComponent& unit, Equivalent& out) {
    using Lib = dsp::CabinetIRLibrary;
    auto P = [&](const std::string& id, float def) {
        float v = unit.getParameter(id);
        return std::isfinite(v) ? v : def;
    };
    auto levelFor = [](float volume) { // Cabinet level param (0.5 = 0 dB) matching a cabinet unit's volume
        const float db = 20.0f * std::log10(std::max(1e-3f, volume / kMatchedParityVolume));
        return std::clamp(0.5f + db / 24.0f, 0.0f, 1.0f);
    };
    out = Equivalent{};
    switch (unit.tableId()) {
        case 156000: {
            const int idx = std::clamp(static_cast<int>(std::lround(P("Cab", 9.0f))), 0, static_cast<int>(dsp::cab_ids::kMatched.size()) - 1);
            out.cab = std::string("matched:") + Lib::matchedId(idx);
            // Unit semantics (see ConvolutionCabinetComponent): V picks the A or B capture, X-Fade (c) blends the
            // energy-matched room in at constant power. The Cabinet unit plays its capture at full level plus both room
            // files at 0.9 * room^2 / sqrt(2) each, so: Level carries the close gain cos(c * pi/2), and Room is solved
            // for the same room-to-close energy (exact without room; approximate with it, as the room captures differ).
            const char ab = P("V", 0.0f) > 0.5f ? 'B' : 'A';
            out.mic_a = std::string(1, ab); out.pos_a = "fixed";
            const float c = std::clamp(P("c", 0.0f), 0.0f, 1.0f);
            const float g_close = std::cos(c * 1.5707963f), g_room = std::sin(c * 1.5707963f);
            float room = 0.0f;
            if (g_room > 1e-4f) {
                auto energy = [](const std::shared_ptr<const std::vector<float>>& v) { double e = 0.0; if (v) for (float x : *v) e += static_cast<double>(x) * x; return e; };
                const double e_close = energy(Lib::load(Lib::matched(idx, ab, false), 48000.0));
                const auto ra = Lib::load(Lib::matched(idx, 'A', true), 48000.0), rb = Lib::load(Lib::matched(idx, 'B', true), 48000.0);
                double e_cu = 0.0; // the Cabinet unit's two room files, summed
                const std::size_t n = std::max(ra ? ra->size() : 0, rb ? rb->size() : 0);
                for (std::size_t i = 0; i < n; ++i) {
                    const double v = ((ra && i < ra->size()) ? (*ra)[i] : 0.0f) + ((rb && i < rb->size()) ? (*rb)[i] : 0.0f);
                    e_cu += v * v;
                }
                const double g_cu = (e_cu > 1e-12) ? g_room / std::max(1e-3f, g_close) * std::sqrt(e_close / e_cu) : 0.0;
                room = std::clamp(static_cast<float>(std::sqrt(g_cu * std::numbers::sqrt2 / 0.9)), 0.0f, 1.0f);
            }
            out.params = {{"room", room}, {"level", levelFor(dsp::ampMatchedCabVolume(P("MV", 0.5f)) * g_close)}};
            return true;
        }
        case 88000: {
            const int cab = std::clamp(static_cast<int>(std::lround(P("Cab", 24.0f))), 0, 28);
            static constexpr int kGeneric[11] = {17, 3, 8, 6, 5, 25, 26, 24, 23, 15, 13};
            static constexpr int kMatchedFrom11[18] = {1, 6, 16, 4, 10, 11, 2, 5, 14, 0, 13, 15, 8, 9, 3, 12, 17, 7};
            if (cab < 11) {
                const std::string folder = Lib::studioId(kGeneric[cab]);
                out.cab = "studio:" + folder;
            } else {
                out.cab = std::string("matched:") + Lib::matchedId(kMatchedFrom11[cab - 11]);
                out.mic_a = "A"; out.pos_a = "fixed"; out.mic_b = "B"; out.pos_b = "fixed";
            }
            out.params = {{"blend", std::clamp(P("c", 0.3f), 0.0f, 1.0f)}, {"room", std::sqrt(std::clamp(P("a", 0.0f), 0.0f, 1.0f) * 0.66f)},
                          {"level", levelFor(0.7f + 0.9f * P("MV", 0.5f))}};
            return true;
        }
        case 119000: {
            struct Ch { std::string folder, mic, pos; float gain; };
            std::vector<Ch> chans;
            for (int i = 1; i <= 8 && chans.size() < 2; ++i) {
                const std::string n = std::to_string(i);
                const int cab = static_cast<int>(std::lround(P("Cab" + n, i <= 2 ? 10.0f : 1.0f)));
                if (cab < 2 || P("m" + n, 0.0f) >= 0.5f) continue;
                // "studio/<id>/<id>_<mic>_<pos>.ir" → tokens
                const std::string rel = Lib::roomCabMic(cab - 2, static_cast<int>(std::lround(P("Mic" + n, 0.0f))),
                                                        static_cast<int>(std::lround(P("MPos" + n, 0.0f))));
                const std::string folder = Lib::studioId(cab - 2);
                std::string stem = rel.substr(rel.rfind('/') + 1);
                if (auto dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
                if (stem.size() <= folder.size() + 1) continue;
                stem = stem.substr(folder.size() + 1);
                const auto us = stem.rfind('_');
                if (us == std::string::npos) continue;
                chans.push_back({folder, stem.substr(0, us), stem.substr(us + 1), std::clamp(P("g" + n, 0.5f), 0.0f, 1.0f)});
            }
            if (chans.empty()) return false;
            out.cab = "studio:" + chans[0].folder;
            out.mic_a = chans[0].mic; out.pos_a = chans[0].pos;
            if (chans.size() > 1 && chans[1].folder == chans[0].folder) {
                out.mic_b = chans[1].mic; out.pos_b = chans[1].pos;
                const float ga = chans[0].gain, gb = chans[1].gain;
                out.params.push_back({"blend", static_cast<float>(std::atan2(gb, std::max(1e-4f, ga)) / (std::numbers::pi * 0.5))});
            }
            out.params.push_back({"room", std::clamp(P("a", 0.0f), 0.0f, 1.0f) * 0.7f});
            out.params.push_back({"level", levelFor(0.6f + 0.8f * P("v", 0.5f))});
            return true;
        }
        case 168000: {
            const auto& files = Lib::listFiles("Cabinet IR Loader", ".wav");
            const int sel = static_cast<int>(std::lround(P("IR1", 1.0f)));
            if (sel <= 0 || sel > static_cast<int>(files.size())) return false;
            out.cab = "capture:" + files[static_cast<std::size_t>(sel - 1)];
            out.mic_a = "ir"; out.pos_a = "fixed";
            out.params = {{"level", levelFor(0.6f + 0.8f * P("v", 0.5f))}};
            return true;
        }
        default: return false;
    }
}

std::string slotJson(const IComponent& unit) {
    const auto* u = dynamic_cast<const CabinetUnit*>(&unit);
    return u ? u->json() : std::string();
}

std::string catalogJson() {
    const auto& cat = Catalog::get();
    std::ostringstream o;
    o << "{\"content_root\":\"" << esc(dsp::CabinetIRLibrary::contentRoot()) << "\",\"available\":" << (cat.cabs().empty() ? "false" : "true")
      << ",\"cabinets\":[";
    for (std::size_t i = 0; i < cat.cabs().size(); ++i) {
        const auto& c = cat.cabs()[i];
        o << (i ? "," : "") << "{\"index\":" << i << ",\"id\":\"" << esc(c.id) << "\",\"kind\":\"" << c.kind << "\",\"label\":\"" << esc(c.label)
          << "\",\"group\":\"" << esc(c.group) << "\",\"speakers\":\"" << esc(c.speakers) << "\",\"room\":" << (c.room ? "true" : "false")
          << ",\"pair\":\"" << esc(c.pair) << "\",\"mics\":[";
        for (std::size_t m = 0; m < c.mics.size(); ++m) {
            const auto& mi = c.mics[m];
            o << (m ? "," : "") << "{\"id\":\"" << esc(mi.id) << "\",\"label\":\"" << esc(mi.label) << "\",\"family\":\"" << esc(mi.family)
              << "\",\"note\":\"" << esc(mi.note) << "\",\"positions\":[";
            for (std::size_t p = 0; p < mi.positions.size(); ++p)
                o << (p ? "," : "") << "{\"id\":\"" << esc(mi.positions[p]) << "\",\"label\":\"" << esc(dsp::cab::positionLabel(mi.positions[p])) << "\"}";
            o << "]}";
        }
        o << "]}";
    }
    o << "],\"counts\":{\"matched\":";
    std::size_t nm = 0, ns = 0, nc = 0;
    for (const auto& c : cat.cabs()) (c.kind == "matched" ? nm : c.kind == "studio" ? ns : nc)++;
    o << nm << ",\"studio\":" << ns << ",\"capture\":" << nc << ",\"studio_mic_recordings\":" << cat.micCount() << "}}";
    return o.str();
}
} // namespace cabinet
} // namespace rig

namespace rig::catalog {
const ComponentSchema* findStudioComponent(int32_t table_id) noexcept {
    for (const auto& c : studioSchemas())
        if (c.table_id == table_id) return &c;
    if (table_id == kDoublerRoomUnitTable) return &doublerRoomUnitSchema();
    if (table_id == kToneMatchUnitTable) return &toneMatchUnitSchema();
    return nullptr;
}
std::span<const ComponentSchema> studioComponents() noexcept { return {studioSchemas().data(), studioSchemas().size()}; }
} // namespace rig::catalog
