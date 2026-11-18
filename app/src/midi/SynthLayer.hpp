#pragma once
// The guitar → MIDI → synth layer that runs in parallel with the rig.
#include "PitchToMidi.hpp"
#include "Synth.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace rig::midi {

struct LayerSettings {
    bool enabled = false;
    std::string route = "parallel"; // parallel | into_rig | synth_only
    float blend = 0.5f;
    float synth_level = 0.8f;
    bool midi_out = true;
};

class SynthLayer {
public:
    using MidiSink = std::function<void(const MidiEvent&)>;

    void prepare(double sr) {
        std::lock_guard<std::mutex> lk(mtx_);
        sr_ = sr;
        det_.prepare(sr);
        fx_.prepare(sr);
        synth_.prepare(sr);
        ev_.reserve(256);
        fxev_.reserve(256);
        mono_.assign(4096, 0.0f);
    }

    bool enabled() const { return set_.enabled; }
    const LayerSettings& settings() const { return set_; }

    // Audio thread. Writes synth output; returns false if the layer is busy/disabled.
    bool process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n, const MidiSink& sink) {
        std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
        if (!lk.owns_lock() || !set_.enabled || n > mono_.size()) return false;
        for (uint32_t i = 0; i < n; ++i) mono_[i] = 0.5f * (inL[i] + inR[i]);
        ev_.clear();
        fxev_.clear();
        det_.process(mono_.data(), n, ev_);
        fx_.process(ev_, n, fxev_);
        std::stable_sort(fxev_.begin(), fxev_.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
        synth_.setFollowLevel(det_.level());
        synth_.render(fxev_, outL, outR, n);
        if (set_.midi_out && sink) {
            for (auto e : fxev_) {
                if (e.type == MidiEvent::PitchBend) e.bend = e.bend * 2.0f / static_cast<float>(std::max(1, det_.settings().bend_range));
                sink(e);
            }
        }
        // Snapshot for telemetry.
        if (snap_mtx_.try_lock()) {
            fx_.sounding(snap_notes_);
            snap_pitch_ = det_.pitchHz();
            snap_conf_ = det_.confidence();
            snap_level_ = det_.level();
            snap_mtx_.unlock();
        }
        return true;
    }

    // ───────────── control thread ─────────────
    void set(const std::map<std::string, std::string>& q) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto has = [&](const char* k) { return q.count(k) && !q.at(k).empty(); };
        auto f = [&](const char* k, float d) { try { return has(k) ? std::stof(q.at(k)) : d; } catch (...) { return d; } };
        auto i = [&](const char* k, int d) { try { return has(k) ? std::stoi(q.at(k)) : d; } catch (...) { return d; } };
        auto s = [&](const char* k, const std::string& d) { return has(k) ? q.at(k) : d; };
        const bool was = set_.enabled;
        if (has("enabled")) set_.enabled = q.at("enabled") == "1" || q.at("enabled") == "true";
        set_.route = s("route", set_.route);
        set_.blend = std::clamp(f("blend", set_.blend), 0.0f, 1.0f);
        set_.synth_level = std::clamp(f("synth_level", set_.synth_level), 0.0f, 1.5f);
        if (has("midi_out")) set_.midi_out = q.at("midi_out") == "1" || q.at("midi_out") == "true";
        auto d = det_.settings();
        const bool was_poly = d.poly;
        if (has("mode")) d.poly = q.at("mode") != "mono";
        d.sensitivity = std::clamp(f("sensitivity", d.sensitivity), 0.0f, 1.0f);
        d.bend_range = std::clamp(i("bend_range", d.bend_range), 1, 24);
        d.low_note = std::clamp(i("low_note", d.low_note), 20, 100);
        d.high_note = std::clamp(i("high_note", d.high_note), d.low_note + 1, 110);
        d.velocity_curve = std::clamp(f("velocity_curve", d.velocity_curve), 0.0f, 1.0f);
        if (d.poly != was_poly) {
            std::vector<MidiEvent> dummy;
            det_.allNotesOff(dummy);
            synth_.allNotesOff();
        }
        det_.setSettings(d);
        auto m = fx_.settings();
        m.octave = std::clamp(i("octave", m.octave), -3, 3);
        m.transpose = std::clamp(i("transpose", m.transpose), -12, 12);
        m.harmony = s("harmony", m.harmony);
        m.scale_root = std::clamp(i("scale_root", m.scale_root), 0, 11);
        m.scale = s("scale", m.scale);
        m.arp = s("arp", m.arp);
        m.arp_rate = s("arp_rate", m.arp_rate);
        if (has("hold")) m.hold = q.at("hold") == "1" || q.at("hold") == "true";
        m.bpm = std::clamp(f("bpm", m.bpm), 30.0f, 300.0f);
        fx_.setSettings(m);
        if (has("preset")) synth_.applyPreset(q.at("preset"));
        if (was && !set_.enabled) synth_.allNotesOff();
    }

    void setParam(const std::string& id, float norm) {
        std::lock_guard<std::mutex> lk(mtx_);
        synth_.setParam(id, norm);
    }

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            o += c;
        }
        return o;
    }

    std::string json() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(4);
        const auto& d = det_.settings();
        const auto& m = fx_.settings();
        o << "{\"enabled\":" << (set_.enabled ? "true" : "false") << ",\"route\":\"" << set_.route << "\",\"blend\":" << set_.blend
          << ",\"synth_level\":" << set_.synth_level << ",\"midi_out\":" << (set_.midi_out ? "true" : "false");
        o << ",\"detect\":{\"mode\":\"" << (d.poly ? "poly" : "mono") << "\",\"sensitivity\":" << d.sensitivity << ",\"bend_range\":" << d.bend_range
          << ",\"low_note\":" << d.low_note << ",\"high_note\":" << d.high_note << ",\"velocity_curve\":" << d.velocity_curve << "}";
        o << ",\"midi_fx\":{\"octave\":" << m.octave << ",\"transpose\":" << m.transpose << ",\"harmony\":\"" << m.harmony << "\",\"scale_root\":"
          << m.scale_root << ",\"scale\":\"" << m.scale << "\",\"arp\":\"" << m.arp << "\",\"arp_rate\":\"" << m.arp_rate << "\",\"hold\":"
          << (m.hold ? "true" : "false") << ",\"bpm\":" << m.bpm << "}";
        o << ",\"synth\":{\"preset\":\"" << esc(synth_.presetName()) << "\",\"params\":[";
        const auto& defs = synthParamDefs();
        for (std::size_t k = 0; k < defs.size(); ++k) {
            const auto& pd = defs[k];
            const float n = synth_.params()[k];
            o << (k ? "," : "") << "{\"id\":\"" << pd.id << "\",\"name\":\"" << pd.name << "\",\"kind\":\"" << (pd.steps ? "stepped" : "continuous")
              << "\",\"norm\":" << n << ",\"default\":" << pd.def << ",\"fmt\":\"" << esc(Synth::formatParam(pd, n)) << "\"";
            if (pd.steps) {
                o << ",\"labels\":[";
                std::string l = pd.labels;
                std::size_t st = 0;
                for (int j = 0; j < pd.steps; ++j) {
                    const std::size_t e = l.find('|', st);
                    o << (j ? "," : "") << "\"" << l.substr(st, e == std::string::npos ? std::string::npos : e - st) << "\"";
                    st = e + 1;
                }
                o << "]";
            }
            o << "}";
        }
        o << "]},\"presets\":[";
        const auto& ps = synthPresets();
        for (std::size_t k = 0; k < ps.size(); ++k) o << (k ? "," : "") << "\"" << esc(ps[k].name) << "\"";
        o << "],\"notes\":[";
        std::lock_guard<std::mutex> lk2(snap_mtx_);
        for (std::size_t k = 0; k < snap_notes_.size(); ++k)
            o << (k ? "," : "") << "{\"note\":" << snap_notes_[k].first << ",\"vel\":" << static_cast<int>(snap_notes_[k].second * 127.0f) << "}";
        o << "]}";
        return o.str();
    }

    // Compact live view for the 80 ms telemetry poll (no lock: benign snapshot races).
    std::string telemetryJson() const {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(3);
        std::lock_guard<std::mutex> lk(snap_mtx_);
        o << "{\"enabled\":" << (set_.enabled ? "true" : "false") << ",\"notes\":[";
        const auto notes = snap_notes_;
        for (std::size_t k = 0; k < notes.size(); ++k) o << (k ? "," : "") << "[" << notes[k].first << "," << static_cast<int>(notes[k].second * 127.0f) << "]";
        o << "],\"pitch\":" << snap_pitch_ << ",\"confidence\":" << snap_conf_ << ",\"level\":" << snap_level_ << "}";
        return o.str();
    }

    // Persistence inside user presets.
    std::string toXml() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ostringstream o;
        const auto& d = det_.settings();
        const auto& m = fx_.settings();
        o << "<headroom-synth enabled=\"" << set_.enabled << "\" route=\"" << set_.route << "\" blend=\"" << set_.blend << "\" synth_level=\""
          << set_.synth_level << "\" midi_out=\"" << set_.midi_out << "\" mode=\"" << (d.poly ? "poly" : "mono") << "\" sensitivity=\"" << d.sensitivity
          << "\" bend_range=\"" << d.bend_range << "\" low_note=\"" << d.low_note << "\" high_note=\"" << d.high_note << "\" octave=\"" << m.octave
          << "\" transpose=\"" << m.transpose << "\" harmony=\"" << m.harmony << "\" scale_root=\"" << m.scale_root << "\" scale=\"" << m.scale
          << "\" arp=\"" << m.arp << "\" arp_rate=\"" << m.arp_rate << "\" hold=\"" << m.hold << "\" bpm=\"" << m.bpm << "\" preset=\""
          << synth_.presetName() << "\">";
        const auto& defs = synthParamDefs();
        for (std::size_t k = 0; k < defs.size(); ++k) o << "<p id=\"" << defs[k].id << "\" v=\"" << synth_.params()[k] << "\"/>";
        o << "</headroom-synth>";
        return o.str();
    }

    void fromXml(const std::string& xml) {
        auto attr = [&](const std::string& src, const std::string& key) -> std::string {
            const std::string k = " " + key + "=\"";
            const auto p = src.find(k);
            if (p == std::string::npos) return {};
            const auto e = src.find('"', p + k.size());
            return src.substr(p + k.size(), e - p - k.size());
        };
        const auto head_end = xml.find('>');
        const std::string head = xml.substr(0, head_end);
        std::map<std::string, std::string> q;
        for (const char* k : {"enabled", "route", "blend", "synth_level", "midi_out", "mode", "sensitivity", "bend_range", "low_note", "high_note",
                              "octave", "transpose", "harmony", "scale_root", "scale", "arp", "arp_rate", "hold", "bpm", "preset"}) {
            q[k] = attr(head, k);
        }
        set(q);
        std::size_t pos = head_end;
        while ((pos = xml.find("<p ", pos)) != std::string::npos) {
            const auto e = xml.find("/>", pos);
            const std::string tag = xml.substr(pos, e - pos);
            try { setParam(attr(tag, "id"), std::stof(attr(tag, "v"))); } catch (...) {}
            pos = e;
        }
    }

private:
    mutable std::mutex mtx_;
    mutable std::mutex snap_mtx_;
    double sr_ = 48000.0;
    LayerSettings set_{};
    PitchToMidi det_;
    MidiFx fx_;
    Synth synth_;
    std::vector<MidiEvent> ev_, fxev_;
    std::vector<float> mono_;
    std::vector<std::pair<int, float>> snap_notes_;
    float snap_pitch_ = 0.0f, snap_conf_ = 0.0f, snap_level_ = 0.0f;
};

} // namespace rig::midi
