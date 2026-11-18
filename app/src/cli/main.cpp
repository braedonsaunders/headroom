#include "../audio/WavIO.hpp"
#include "../core/RackGraph.hpp"
#include "../generated/ComponentCatalog.hpp"
#include "../preset/PresetLoader.hpp"
#include "JsonExporter.hpp"
#include "../dsp/AmpModeling.hpp"
#include "../library/Library.hpp"
#include "../library/Showcase.hpp"
#include "../midi/MidiComponents.hpp"
#include "../core/IComponent.hpp"
#include "../midi/SynthLayer.hpp"
#include <map>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void printUsage() {
    std::cout << "Usage:\n"
              << "  rig_cli catalog                                        List all 166 component schemas\n"
              << "  rig_cli inspect <preset.rig>                           Inspect rack topology, parameters & macros\n"
              << "  rig_cli export-json <preset.rig>                       Export full rack state & live DSP telemetry as JSON\n"
              << "  rig_cli scan-presets <dir>                             Verify parsing of all .rig files in directory\n"
              << "  rig_cli render-test <preset.rig>                       Build RackGraph and render 1 second of 48kHz test audio\n"
              << "  rig_cli render-wav <preset.rig> <in.wav> <out.wav>     Process a real unprocessed DI guitar WAV through a preset\n"
              << "  rig_cli make-showcase <repo_root> [out_dir]            Regenerate and balance the bundled showcase rigs\n";
}

static void printNodeTree(const rig::preset::PresetComponentNode& node, int indent = 0) {
    const std::string pad(static_cast<std::size_t>(indent * 2), ' ');
    const auto* schema = rig::catalog::findByTableId(node.table_id);
    const std::string_view disp = schema ? schema->display_name : "Unknown";
    const std::string_view ref = schema ? schema->reference_name : "Unknown";
    std::cout << pad << "- [" << node.table_id << "] " << disp
              << " (ref: \"" << ref << "\", idx=" << node.instance_index
              << ", params=" << node.params.size()
              << ", mods=" << node.modulations.size() << ")\n";
    if (std::getenv("RIG_SHOW_PARAMS")) {
        std::cout << pad << "    ";
        for (const auto& p : node.params) {
            if (p.type == "string" || p.type == "filename") std::cout << p.id << "='" << p.str_val << "' ";
            else std::cout << p.id << "=" << p.float_val << " ";
        }
        std::cout << "\n";
    }
    for (const auto& ch : node.children) {
        printNodeTree(ch, indent + 1);
    }
}

// The app always loads the neural guitar->MIDI model (RealtimeAudioServer::start), so a Pitch -> MIDI unit in Poly
// mode tracks with it there. Offline renders must do the same or they hear the classic tracker instead.
// HEADROOM_P2M_MODEL=<file> picks the weights, HEADROOM_P2M_MODEL=- keeps the classic tracker (A/B), and by default
// app/resources/models/guitar_midi.hgnn is found next to the executable's build folder or the working directory.
static void useAppGuitarMidiModel(const char* argv0) {
    if (const char* e = std::getenv("HEADROOM_P2M_MODEL")) {
        if (std::string(e) != "-") rig::setNeuralGuitarModelPath(e);
        return;
    }
    std::error_code ec;
    std::vector<fs::path> starts{fs::current_path(ec)};
    const fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
    if (!ec) starts.push_back(exe.parent_path());
    for (fs::path d : starts) {
        for (; !d.empty(); d = d.parent_path()) {
            for (const fs::path& c : {d / "resources" / "models" / "guitar_midi.hgnn", d / "app" / "resources" / "models" / "guitar_midi.hgnn"})
                if (fs::is_regular_file(c, ec)) { rig::setNeuralGuitarModelPath(c.string()); return; }
            if (d == d.root_path()) break;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "make-showcase" && argc >= 3) {
        // rig_cli make-showcase <repo_root> [out_dir]
        const std::string root = argv[2];
        const std::string out_dir = argc >= 4 ? argv[3] : root + "/app/presets/Showcase";
        if (!std::getenv("HEADROOM_P2M_MODEL") && fs::exists(root + "/app/resources/models/guitar_midi.hgnn"))
            rig::setNeuralGuitarModelPath(root + "/app/resources/models/guitar_midi.hgnn"); // lanes balanced as the app tracks
        else useAppGuitarMidiModel(argv[0]);
        rig::library::ComponentPresetBank bank;
        bank.scan();
        const auto rigs = rig::library::buildShowcase(&bank, root + "/app/samples/di", &std::cerr);
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        for (const auto& e : std::filesystem::directory_iterator(out_dir, ec))
            if (e.path().extension() == ".rig") std::filesystem::remove(e.path(), ec);
        for (const auto& r : rigs) {
            std::string err;
            if (!rig::library::writeUserPreset(out_dir + "/" + r.file, r.preset, &err)) std::cerr << "write failed: " << r.file << " " << err << "\n";
            else std::cout << r.file << "\n";
        }
        return 0;
    }
    if (cmd == "library" && argc >= 3) {
        const std::string root = argv[2];
        const std::string showcase = root + "/app/presets/Showcase";
        rig::library::PresetLibrary lib;
        lib.scan("/tmp/headroom/userpresets", {{"00 JCM800 British Crunch.rig", "JCM800 British Crunch", ""}}, showcase);
        std::map<std::string, int> cats;
        for (const auto& e : lib.entries()) cats[e.category]++;
        for (const auto& kv : cats) std::cout << kv.first << ": " << kv.second << "\n";
        int shown = 0;
        for (const auto& e : lib.entries()) {
            if (shown++ % 5 != 0) continue;
            std::cout << "  " << e.name << " | " << e.category << " | " << e.amp << " | ";
            for (auto& t : e.tags) std::cout << t << ",";
            std::cout << "\n";
        }
        rig::library::ComponentPresetBank bank;
        bank.scan();
        int comps = 0, presets = 0, inits = 0;
        for (const auto& c : rig::catalog::kAllComponents) {
            if (const auto* v = bank.presetsFor(c.table_id)) {
                ++comps;
                presets += static_cast<int>(v->size());
                if (bank.initFor(c.table_id)) ++inits;
            }
        }
        std::cout << "component banks: " << comps << " components, " << presets << " presets, " << inits << " with INIT\n";
        return 0;
    }
    if (cmd == "dump-tags" && argc >= 3) {
        for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
            if (!entry.is_regular_file() || entry.path().extension() != ".rig") continue;
            rig::preset::RackPreset preset;
            if (!rig::preset::loadPreset(entry.path().string(), preset, nullptr)) continue;
            std::cout << preset.info.name << " | " << preset.info.author << " | ";
            for (const auto& t : preset.info.tags) std::cout << t.mode << ":" << t.id << "=" << t.value << "; ";
            std::cout << " | macros=" << preset.macros.size() << " | comment=" << preset.info.comment.substr(0, 80) << "\n";
        }
        return 0;
    }
    if (cmd == "render-synth" && argc >= 4) {
        // rig_cli render-synth <in.wav> <out.wav> [poly|mono] [preset] [harmony]
        rig::audio::WavData w;
        std::string err;
        if (!rig::audio::readWavFile(argv[2], w, &err)) { std::cerr << err << "\n"; return 1; }
        rig::midi::SynthLayer layer;
        layer.prepare(w.sample_rate);
        std::map<std::string, std::string> q{{"enabled", "1"}, {"mode", argc > 4 ? argv[4] : "poly"}};
        if (argc > 5) q["preset"] = argv[5];
        if (argc > 6) q["harmony"] = argv[6];
        layer.set(q);
        std::vector<float> L(w.left.size()), R(w.left.size());
        int ons = 0;
        for (std::size_t p = 0; p + 256 <= w.left.size(); p += 256) {
            layer.process(&w.left[p], &w.right[p], &L[p], &R[p], 256, [&](const rig::midi::MidiEvent& e) {
                if (e.type == rig::midi::MidiEvent::NoteOn) { ++ons; if (std::getenv("V")) std::cerr << (p + e.offset) / double(w.sample_rate) << " on " << e.note << "\n"; }
            });
        }
        rig::audio::writeWavFile16(argv[3], w.sample_rate, L, R);
        std::cout << "note-ons: " << ons << "\n";
        return 0;
    }
    if (cmd == "calib-di" && argc >= 3) {
        const auto sig = rig::amp::makeCalibrationDI(44100.0, 4.0f);
        if (!rig::audio::writeWavFile16(argv[2], 44100, sig, sig)) return 1;
        return 0;
    }
    if (cmd == "catalog") {
        std::cout << "Total registered components: " << rig::catalog::kAllComponents.size() << "\n";
        for (const auto& c : rig::catalog::kAllComponents) {
            std::cout << "  " << c.table_id << " | " << c.display_name
                      << " | ref=\"" << c.reference_name << "\""
                      << " | dsp=" << c.dsp_class
                      << " | params=" << c.params.size() << "\n";
        }
        return 0;
    }

    if (cmd == "inspect" && argc >= 3) {
        rig::preset::RackPreset preset;
        std::string err;
        if (!rig::preset::loadPreset(argv[2], preset, &err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        std::cout << "Preset: " << preset.info.name << " (author: " << preset.info.author << ")\n";
        std::cout << "UUID:   " << preset.info.uuid << "\n";
        std::cout << "Tags:   " << preset.info.tags.size() << "\n";
        std::cout << "Macros: " << preset.macros.size() << "\n";
        std::cout << "Signal Chain:\n";
        printNodeTree(preset.root_rack, 1);
        return 0;
    }

    if (cmd == "export-json" && argc >= 3) {
        std::cout << rig::json::exportPresetToJson(argv[2], 48000.0);
        return 0;
    }

    if (cmd == "scan-presets" && argc >= 3) {
        std::size_t total = 0, ok = 0, total_nodes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
            if (!entry.is_regular_file() || entry.path().extension() != ".rig") continue;
            ++total;
            rig::preset::RackPreset preset;
            std::string err;
            if (rig::preset::loadPreset(entry.path().string(), preset, &err)) {
                ++ok;
                rig::RackGraph graph;
                graph.buildFromPreset(preset, 48000.0, 512);
                total_nodes += graph.totalComponentCount();
            } else {
                std::cerr << "FAIL: " << entry.path().string() << " -> " << err << "\n";
            }
        }
        std::cout << "Scanned .rig presets: " << ok << " / " << total
                  << " succeeded (" << total_nodes << " instantiated component nodes).\n";
        return (ok == total && total > 0) ? 0 : 1;
    }

    if (cmd == "render-test" && argc >= 3) {
        rig::preset::RackPreset preset;
        std::string err;
        if (!rig::preset::loadPreset(argv[2], preset, &err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        rig::RackGraph graph;
        graph.buildFromPreset(preset, 48000.0, 512);

        rig::StereoScratchBuffer in_buf{};
        rig::StereoScratchBuffer out_buf{};
        double rms_accum = 0.0;
        double peak = 0.0;
        constexpr uint32_t kBlock = 512;
        constexpr uint32_t kNumBlocks = 48000 / kBlock;

        for (uint32_t b = 0; b < kNumBlocks; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double t = static_cast<double>(b * kBlock + i) / 48000.0;
                const float sample = 0.25f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 110.0 * t));
                in_buf.left[i] = sample;
                in_buf.right[i] = sample;
            }
            rig::AudioBlock block{};
            block.in = in_buf.const_ptrs.data();
            block.out = out_buf.ptrs.data();
            block.num_channels = 2;
            block.num_frames = kBlock;
            block.sample_rate = 48000.0;
            graph.process(block);

            for (uint32_t i = 0; i < kBlock; ++i) {
                const double v = out_buf.left[i];
                rms_accum += v * v;
                peak = std::max(peak, std::abs(v));
            }
        }
        const double rms = std::sqrt(rms_accum / static_cast<double>(kNumBlocks * kBlock));
        std::cout << "Rendered \"" << preset.info.name << "\" (" << graph.totalComponentCount()
                  << " nodes): RMS=" << rms << ", Peak=" << peak << "\n";
        return 0;
    }

    if (cmd == "render-wav" && argc >= 5) {
        useAppGuitarMidiModel(argv[0]);
        rig::preset::RackPreset preset;
        std::string err;
        if (!rig::preset::loadPreset(argv[2], preset, &err)) {
            std::cerr << "Preset error: " << err << "\n";
            return 1;
        }
        rig::audio::WavData wav_in;
        if (!rig::audio::readWavFile(argv[3], wav_in, &err)) {
            std::cerr << "WAV error: " << err << "\n";
            return 1;
        }

        const double sr = static_cast<double>(wav_in.sample_rate);
        constexpr uint32_t kBlock = 512;
        rig::RackGraph graph;
        graph.buildFromPreset(preset, sr, kBlock);
        if (std::getenv("RIG_TAPS")) graph.setDebugTaps(true);

        const std::size_t total_frames = wav_in.left.size();
        std::vector<float> out_l(total_frames, 0.0f);
        std::vector<float> out_r(total_frames, 0.0f);

        rig::StereoScratchBuffer in_buf{};
        rig::StereoScratchBuffer out_buf{};

        for (std::size_t offset = 0; offset < total_frames; offset += kBlock) {
            const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(kBlock, total_frames - offset));
            for (uint32_t i = 0; i < n; ++i) {
                in_buf.left[i] = wav_in.left[offset + i];
                in_buf.right[i] = wav_in.right[offset + i];
            }
            rig::AudioBlock block{};
            block.in = in_buf.const_ptrs.data();
            block.out = out_buf.ptrs.data();
            block.num_channels = 2;
            block.num_frames = n;
            block.sample_rate = sr;
            graph.process(block);

            for (uint32_t i = 0; i < n; ++i) {
                out_l[offset + i] = out_buf.left[i];
                out_r[offset + i] = out_buf.right[i];
            }
        }

        if (std::getenv("RIG_TAPS")) graph.dumpDebugTaps();
        if (!rig::audio::writeWavFile16(argv[4], wav_in.sample_rate, out_l, out_r)) {
            std::cerr << "Failed to write output WAV: " << argv[4] << "\n";
            return 1;
        }
        std::cout << "Rendered \"" << preset.info.name << "\" on " << fs::path(argv[3]).filename().string()
                  << " -> " << argv[4] << " (" << total_frames << " frames @ " << wav_in.sample_rate << " Hz)\n";
        return 0;
    }

    printUsage();
    return 1;
}
