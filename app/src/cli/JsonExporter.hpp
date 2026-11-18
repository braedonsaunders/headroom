#pragma once
#include "../core/RackGraph.hpp"
#include "../dsp/DSPPrimitives.hpp"
#include "../generated/ComponentCatalog.hpp"
#include "../preset/PresetLoader.hpp"
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace rig::json {

inline std::string escJson(std::string_view s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    oss << static_cast<char>(c);
                }
        }
    }
    return oss.str();
}

inline std::string_view familyToString(catalog::EngineFamily fam) noexcept {
    switch (fam) {
        case catalog::EngineFamily::Routing: return "Routing";
        case catalog::EngineFamily::IO: return "IO";
        case catalog::EngineFamily::Tool: return "Tool";
        case catalog::EngineFamily::HardwareController: return "HardwareController";
        case catalog::EngineFamily::UIOnly: return "UIOnly";
        case catalog::EngineFamily::ClassicDSP: return "ClassicDSP";
        case catalog::EngineFamily::CircuitWaveshaper: return "CircuitWaveshaper";
        case catalog::EngineFamily::Infrastructure: return "Infrastructure";
        case catalog::EngineFamily::Modulator: return "Modulator";
        case catalog::EngineFamily::Convolution: return "Convolution";
        case catalog::EngineFamily::StudioHardware: return "StudioHardware";
        case catalog::EngineFamily::ModernDSP: return "ModernDSP";
        case catalog::EngineFamily::Neural: return "Neural";
        case catalog::EngineFamily::MasteringLimiter: return "MasteringLimiter";
    }
    return "Unknown";
}

inline std::string exportCatalogToJson() {
    std::ostringstream oss;
    oss << "{\n  \"component_count\": " << catalog::kAllComponents.size() << ",\n  \"components\": [\n";
    for (std::size_t i = 0; i < catalog::kAllComponents.size(); ++i) {
        const auto& c = catalog::kAllComponents[i];
        oss << "    {\n"
            << "      \"table_id\": " << c.table_id << ",\n"
            << "      \"display_name\": \"" << escJson(c.display_name) << "\",\n"
            << "      \"reference_name\": \"" << escJson(c.reference_name) << "\",\n"
            << "      \"dsp_class\": \"" << escJson(c.dsp_class) << "\",\n"
            << "      \"family\": \"" << escJson(familyToString(c.family)) << "\",\n"
            << "      \"target_sample_rate_k\": " << c.target_sample_rate_k << ",\n"
            << "      \"fixed_4x_oversampling\": " << (c.fixed_4x_oversampling ? "true" : "false") << ",\n"
            << "      \"param_count\": " << c.params.size() << "\n"
            << "    }" << (i + 1 < catalog::kAllComponents.size() ? "," : "") << "\n";
    }
    oss << "  ]\n}\n";
    return oss.str();
}

inline std::string exportPresetToJson(const std::string& path, double sample_rate = 48000.0) {
    preset::RackPreset preset;
    if (!preset::loadPreset(path, preset)) {
        return "{\"ok\":false,\"error\":\"Failed to parse preset file\"}\n";
    }

    RackGraph graph;
    graph.buildFromPreset(preset, sample_rate, 512);

    // Run 512 frames of a test chord signal (E2 + B2 + E3 guitar transient) to capture live telemetry
    StereoScratchBuffer in_buf{};
    StereoScratchBuffer out_buf{};
    constexpr uint32_t kFrames = 512;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float env = std::exp(-t * 18.0f);
        const float s = env * (0.45f * std::sin(2.0f * std::numbers::pi_v<float> * 82.41f * t) +
                               0.35f * std::sin(2.0f * std::numbers::pi_v<float> * 123.47f * t) +
                               0.25f * std::sin(2.0f * std::numbers::pi_v<float> * 164.81f * t) +
                               0.15f * std::sin(2.0f * std::numbers::pi_v<float> * 329.63f * t));
        in_buf.left[i] = s;
        in_buf.right[i] = s;
    }

    AudioBlock block{};
    block.in = in_buf.const_ptrs.data();
    block.out = out_buf.ptrs.data();
    block.num_channels = 2;
    block.num_frames = kFrames;
    block.sample_rate = sample_rate;
    graph.process(block);

    double in_rms = 0.0, out_rms_l = 0.0, out_rms_r = 0.0, peak_l = 0.0, peak_r = 0.0;
    for (uint32_t i = 0; i < kFrames; ++i) {
        in_rms += in_buf.left[i] * in_buf.left[i];
        out_rms_l += out_buf.left[i] * out_buf.left[i];
        out_rms_r += out_buf.right[i] * out_buf.right[i];
        peak_l = std::max(peak_l, static_cast<double>(std::fabs(out_buf.left[i])));
        peak_r = std::max(peak_r, static_cast<double>(std::fabs(out_buf.right[i])));
    }
    in_rms = std::sqrt(in_rms / kFrames);
    out_rms_l = std::sqrt(out_rms_l / kFrames);
    out_rms_r = std::sqrt(out_rms_r / kFrames);

    dsp::WaveshaperLUT512 demo_lut;
    demo_lut.initAsymmetricTriode(0.16f, 2.1f);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "{\n";
    oss << "  \"ok\": true,\n";
    oss << "  \"preset_name\": \"" << escJson(preset.info.name) << "\",\n";
    oss << "  \"author\": \"" << escJson(preset.info.author) << "\",\n";
    oss << "  \"comment\": \"" << escJson(preset.info.comment) << "\",\n";
    oss << "  \"sample_rate\": " << sample_rate << ",\n";
    oss << "  \"telemetry\": {\n";
    oss << "    \"in_rms\": " << in_rms << ",\n";
    oss << "    \"out_rms_l\": " << out_rms_l << ",\n";
    oss << "    \"out_rms_r\": " << out_rms_r << ",\n";
    oss << "    \"peak_l\": " << peak_l << ",\n";
    oss << "    \"peak_r\": " << peak_r << ",\n";
    oss << "    \"waveform_in\": [";
    for (uint32_t i = 0; i < 128; ++i) {
        if (i) oss << ",";
        oss << in_buf.left[i * 4];
    }
    oss << "],\n";
    oss << "    \"waveform_out\": [";
    for (uint32_t i = 0; i < 128; ++i) {
        if (i) oss << ",";
        oss << out_buf.left[i * 4];
    }
    oss << "],\n";
    oss << "    \"transfer_curve\": [";
    for (int i = 0; i < 64; ++i) {
        if (i) oss << ",";
        const float x = -1.5f + 3.0f * (static_cast<float>(i) / 63.0f);
        oss << demo_lut.process(x);
    }
    oss << "]\n  },\n";

    oss << "  \"macros\": [\n";
    for (std::size_t i = 0; i < preset.macros.size(); ++i) {
        const auto& m = preset.macros[i];
        oss << "    {\"slot\":" << m.column
            << ",\"target_table_id\":" << m.target_table_id
            << ",\"target_param\":\"" << escJson(m.target_param_id) << "\""
            << ",\"min\":" << m.min_val << ",\"max\":" << m.max_val << "}"
            << (i + 1 < preset.macros.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    oss << "  \"modulations\": [\n";
    for (std::size_t i = 0; i < preset.root_rack.modulations.size(); ++i) {
        const auto& md = preset.root_rack.modulations[i];
        oss << "    {\"source_id\":" << md.mod_table_id
            << ",\"target_param\":\"" << escJson(md.target_param_id) << "\""
            << ",\"amount\":" << md.intensity << "}"
            << (i + 1 < preset.root_rack.modulations.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";

    oss << "  \"slots\": [\n";
    for (std::size_t i = 0; i < preset.root_rack.children.size(); ++i) {
        const auto& node = preset.root_rack.children[i];
        const auto* schema = catalog::findByTableId(node.table_id);
        const std::string_view disp = schema ? schema->display_name : "Unknown Module";
        const std::string_view ref = schema ? schema->reference_name : "Unknown";
        const std::string_view dsp_cls = schema ? schema->dsp_class : "";
        const std::string_view fam = schema ? familyToString(schema->family) : "DSP";
        const int32_t k = schema ? schema->target_sample_rate_k : -1;
        const bool fixed_4x = schema ? schema->fixed_4x_oversampling : false;
        double internal_sr = sample_rate;
        uint32_t os_factor = 1;
        if (fixed_4x) {
            os_factor = 4;
            internal_sr = sample_rate * 4.0;
        } else if (k >= 0) {
            const auto cfg = dsp::InternalRateConfig::forModernDSP(sample_rate, k);
            os_factor = cfg.oversampling_factor;
            internal_sr = cfg.internal_sr;
        }

        oss << "    {\n";
        oss << "      \"slot_index\": " << node.slot_index << ",\n";
        oss << "      \"table_id\": " << node.table_id << ",\n";
        oss << "      \"display_name\": \"" << escJson(disp) << "\",\n";
        oss << "      \"reference_name\": \"" << escJson(ref) << "\",\n";
        oss << "      \"dsp_class\": \"" << escJson(dsp_cls) << "\",\n";
        oss << "      \"family\": \"" << escJson(fam) << "\",\n";
        oss << "      \"oversampling\": " << os_factor << ",\n";
        oss << "      \"internal_sr_khz\": " << (internal_sr / 1000.0) << ",\n";
        oss << "      \"params\": [\n";
        for (std::size_t p_idx = 0; p_idx < node.params.size(); ++p_idx) {
            const auto& p = node.params[p_idx];
            std::string_view unit = "";
            std::string_view role = "";
            if (schema) {
                for (const auto& sp : schema->params) {
                    if (sp.id == p.id) {
                        unit = sp.unit;
                        role = sp.role;
                        break;
                    }
                }
            }
            oss << "        {\"id\":\"" << escJson(p.id) << "\",\"name\":\"" << escJson(p.name)
                << "\",\"value\":" << p.float_val << ",\"unit\":\"" << escJson(unit)
                << "\",\"role\":\"" << escJson(role) << "\"}"
                << (p_idx + 1 < node.params.size() ? "," : "") << "\n";
        }
        oss << "      ]\n";
        oss << "    }" << (i + 1 < preset.root_rack.children.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

} // namespace rig::json
