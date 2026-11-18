#include "PresetLoader.hpp"
#include "RigFormat.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rig::preset {

namespace {

struct XmlNode {
    std::string tag;
    std::unordered_map<std::string, std::string> attrs;
    std::string text;
    std::vector<XmlNode> children;

    std::string attr(const std::string& k, const std::string& def = "") const {
        if (auto it = attrs.find(k); it != attrs.end()) return it->second;
        return def;
    }

    const XmlNode* child(std::string_view t) const {
        for (const auto& c : children) {
            if (c.tag == t) return &c;
        }
        return nullptr;
    }
};

std::string unescapeXml(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '&') {
            if (sv.substr(i, 4) == "&lt;") { out.push_back('<'); i += 3; continue; }
            if (sv.substr(i, 4) == "&gt;") { out.push_back('>'); i += 3; continue; }
            if (sv.substr(i, 5) == "&amp;") { out.push_back('&'); i += 4; continue; }
            if (sv.substr(i, 6) == "&quot;") { out.push_back('"'); i += 5; continue; }
            if (sv.substr(i, 6) == "&apos;") { out.push_back('\''); i += 5; continue; }
        }
        out.push_back(sv[i]);
    }
    return out;
}

class MinimalXmlParser {
public:
    explicit MinimalXmlParser(std::string_view src) : src_(src) {}

    bool parseRoot(XmlNode& root) {
        skipWhitespaceAndDecl();
        return parseElement(root);
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;

    void skipSpaces() {
        while (pos_ < src_.size() && static_cast<unsigned char>(src_[pos_]) <= 32) {
            ++pos_;
        }
    }

    void skipWhitespaceAndDecl() {
        while (pos_ < src_.size()) {
            skipSpaces();
            if (pos_ + 4 <= src_.size() && src_.substr(pos_, 4) == "<!--") {
                auto end = src_.find("-->", pos_ + 4);
                pos_ = (end == std::string_view::npos) ? src_.size() : end + 3;
            } else if (pos_ + 2 <= src_.size() && (src_.substr(pos_, 2) == "<?" || src_.substr(pos_, 2) == "<!")) {
                auto end = src_.find('>', pos_ + 2);
                pos_ = (end == std::string_view::npos) ? src_.size() : end + 1;
            } else {
                break;
            }
        }
    }

    bool parseElement(XmlNode& node) {
        skipWhitespaceAndDecl();
        if (pos_ >= src_.size() || src_[pos_] != '<') return false;
        ++pos_; // skip '<'

        const std::size_t tag_start = pos_;
        while (pos_ < src_.size() && static_cast<unsigned char>(src_[pos_]) > 32 &&
               src_[pos_] != '>' && src_[pos_] != '/') {
            ++pos_;
        }
        node.tag = std::string(src_.substr(tag_start, pos_ - tag_start));
        if (node.tag.empty()) return false;

        while (pos_ < src_.size()) {
            skipSpaces();
            if (pos_ >= src_.size()) return false;
            if (src_[pos_] == '/') {
                ++pos_;
                if (pos_ < src_.size() && src_[pos_] == '>') ++pos_;
                return true;
            }
            if (src_[pos_] == '>') {
                ++pos_;
                break;
            }
            const std::size_t k_start = pos_;
            while (pos_ < src_.size() && static_cast<unsigned char>(src_[pos_]) > 32 &&
                   src_[pos_] != '=' && src_[pos_] != '>' && src_[pos_] != '/') {
                ++pos_;
            }
            std::string key(src_.substr(k_start, pos_ - k_start));
            skipSpaces();
            std::string val;
            if (pos_ < src_.size() && src_[pos_] == '=') {
                ++pos_;
                skipSpaces();
                if (pos_ < src_.size() && (src_[pos_] == '"' || src_[pos_] == '\'')) {
                    const char quote = src_[pos_++];
                    const std::size_t v_start = pos_;
                    while (pos_ < src_.size() && src_[pos_] != quote) ++pos_;
                    val = unescapeXml(src_.substr(v_start, pos_ - v_start));
                    if (pos_ < src_.size() && src_[pos_] == quote) ++pos_;
                }
            }
            if (!key.empty()) {
                node.attrs.emplace(std::move(key), std::move(val));
            }
        }

        while (pos_ < src_.size()) {
            const std::size_t lt = src_.find('<', pos_);
            if (lt == std::string_view::npos) {
                node.text += unescapeXml(src_.substr(pos_));
                pos_ = src_.size();
                break;
            }
            if (lt > pos_) {
                node.text += unescapeXml(src_.substr(pos_, lt - pos_));
                pos_ = lt;
            }
            if (pos_ + 2 <= src_.size() && src_.substr(pos_, 2) == "</") {
                const auto gt = src_.find('>', pos_ + 2);
                pos_ = (gt == std::string_view::npos) ? src_.size() : gt + 1;
                return true;
            }
            if (pos_ + 4 <= src_.size() && src_.substr(pos_, 4) == "<!--") {
                const auto end = src_.find("-->", pos_ + 4);
                pos_ = (end == std::string_view::npos) ? src_.size() : end + 3;
                continue;
            }
            XmlNode child;
            if (!parseElement(child)) break;
            node.children.push_back(std::move(child));
        }
        return true;
    }
};

int32_t parseIntOr(const std::string& s, int32_t fallback = 0) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return (end != s.c_str()) ? static_cast<int32_t>(v) : fallback;
}

float parseFloatOr(const std::string& s, float fallback = 0.0f) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return (end != s.c_str()) ? v : fallback;
}

void parseComponentXml(const XmlNode& comp_xml, int32_t slot_idx, PresetComponentNode& out_node) {
    out_node.slot_index = slot_idx;
    out_node.table_id = parseIntOr(comp_xml.attr("id"), parseIntOr(comp_xml.attr("table_id"), 0));
    out_node.instance_index = slot_idx;

    const XmlNode* params_parent = comp_xml.child("parameters");
    if (!params_parent) {
        if (const auto* ca = comp_xml.child("component-audio")) {
            params_parent = ca->child("parameters");
        }
    }
    if (params_parent) {
        for (const auto& p : params_parent->children) {
            if (p.tag != "parameter") continue;
            PresetParam param{};
            param.id = p.attr("id");
            param.name = p.attr("name");
            const std::string val_str = p.attr("value");
            param.type = p.attr("type", "float");
            param.float_val = parseFloatOr(val_str, 0.0f);
            param.int_val = parseIntOr(val_str, 0);
            param.bool_val = (param.float_val >= 0.5f || val_str == "true");
            param.str_val = val_str;
            out_node.params.push_back(std::move(param));
        }
    }

    if (const auto* cc = comp_xml.child("component-container")) {
        int32_t sub_idx = 0;
        for (const auto& cd : cc->children) {
            if (const auto* sub_comp = cd.child("component")) {
                PresetComponentNode child_node{};
                parseComponentXml(*sub_comp, sub_idx++, child_node);
                out_node.children.push_back(std::move(child_node));
            } else if (cd.tag == "component") {
                PresetComponentNode child_node{};
                parseComponentXml(cd, sub_idx++, child_node);
                out_node.children.push_back(std::move(child_node));
            }
        }
    }
}

void parseModulationDb(const XmlNode& mod_db, std::vector<ModulationLink>& out_mods) {
    if (const auto* conns = mod_db.child("modulation-connections")) {
        for (const auto& mc : conns->children) {
            if (mc.tag != "modulation-connection") continue;
            ModulationLink link{};
            link.mod_table_id = parseIntOr(mc.attr("source-id"), 0);
            link.mod_index = parseIntOr(mc.attr("target-index"), 0);
            link.target_param_id = mc.attr("target-name");
            link.intensity = parseFloatOr(mc.attr("amount"), 0.0f);
            out_mods.push_back(std::move(link));
        }
    }
}

} // namespace

bool loadPreset(const std::string& path, RackPreset& out_preset, std::string* err_msg) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err_msg) *err_msg = "Failed to open file: " + path;
        return false;
    }
    const auto sz = f.tellg();
    if (sz <= 32) {
        if (err_msg) *err_msg = "File too small to be a preset: " + path;
        return false;
    }
    std::string raw(static_cast<std::size_t>(sz), '\0');
    f.seekg(0);
    f.read(raw.data(), sz);
    return parsePresetData(raw, path, out_preset, err_msg);
}

bool parsePresetData(std::string_view raw_sv, const std::string& path, RackPreset& out_preset, std::string* err_msg) {
    out_preset = RackPreset{};
    out_preset.source_path = path;
    out_preset.root_rack.table_id = 100;

    // 1. Parse optional <headroom-info>...</headroom-info>
    const std::string_view db_open = format::kInfoOpen;
    const std::string_view db_close = format::kInfoClose;
    const auto db_start = raw_sv.find(db_open);
    if (db_start != std::string_view::npos) {
        const auto db_end = raw_sv.find(db_close, db_start);
        if (db_end != std::string_view::npos) {
            const auto slice = raw_sv.substr(db_start, (db_end + db_close.size()) - db_start);
            MinimalXmlParser db_parser(slice);
            XmlNode db_root;
            if (db_parser.parseRoot(db_root)) {
                if (const auto* si = db_root.child("soundinfo")) {
                    if (const auto* props = si->child("properties")) {
                        if (const auto* nm = props->child("name")) out_preset.info.name = nm->text;
                        if (const auto* au = props->child("author")) out_preset.info.author = au->text;
                        if (const auto* bo = props->child("based-on")) out_preset.info.based_on = bo->text;
                        if (const auto* bp = props->child("based-on-path")) out_preset.info.based_on_path = bp->text;
                    }
                }
            }
        }
    }

    // 2. Parse <headroom-rig>...</headroom-rig>
    const std::string_view rig_open = format::kRigOpen;
    const std::string_view rig_close = format::kRigClose;
    const auto rig_start = raw_sv.find(rig_open);
    if (rig_start == std::string_view::npos) {
        if (err_msg) *err_msg = "Missing <headroom-rig> in: " + path;
        return false;
    }
    const auto rig_end = raw_sv.find(rig_close, rig_start);
    if (rig_end == std::string_view::npos) {
        if (err_msg) *err_msg = "Missing </headroom-rig> in: " + path;
        return false;
    }

    const auto rig_slice = raw_sv.substr(rig_start, (rig_end + rig_close.size()) - rig_start);
    MinimalXmlParser rig_parser(rig_slice);
    XmlNode rig_root;
    if (!rig_parser.parseRoot(rig_root) || rig_root.tag != format::kRigTag) {
        if (err_msg) *err_msg = "Failed to parse <headroom-rig> XML in: " + path;
        return false;
    }

    if (const auto* meta = rig_root.child("metadata")) {
        if (out_preset.info.name.empty()) out_preset.info.name = meta->attr("preset-name");
        if (out_preset.info.author.empty()) out_preset.info.author = meta->attr("preset-author");
        out_preset.info.comment = meta->attr("preset-comment");
        if (out_preset.info.based_on.empty()) out_preset.info.based_on = meta->attr("based-on");
        if (out_preset.info.based_on_path.empty()) out_preset.info.based_on_path = meta->attr("based-on-path");
        if (const auto* tc = meta->child("tag-collection")) {
            for (const auto& t : tc->children) {
                if (t.tag == "tag") {
                    out_preset.info.tags.push_back({"tag", "", t.attr("name")});
                }
            }
        }
    }

    const XmlNode* rack_node = rig_root.child(std::string(format::kRackTag));
    if (rack_node) {
        if (const auto* non_fix = rack_node->child("non-fix-components")) {
            int32_t slot = 0;
            for (const auto& comp_xml : non_fix->children) {
                if (comp_xml.tag != "component") continue;
                PresetComponentNode node{};
                parseComponentXml(comp_xml, slot++, node);
                out_preset.root_rack.children.push_back(std::move(node));
            }
        }
    }

    if (const auto* rcdb = rig_root.child("realtimecontroldb")) {
        if (const auto* mod_db = rcdb->child("modulationdb")) {
            parseModulationDb(*mod_db, out_preset.root_rack.modulations);
        }
        if (const auto* rem_db = rcdb->child("remotecontroldb")) {
            if (const auto* listeners = rem_db->child("component-event-listeners")) {
                for (const auto& cd : listeners->children) {
                    if (cd.tag != "connection-data") continue;
                    if (const auto* t = cd.child("target")) {
                        MacroTarget mt{};
                        mt.page = 0;
                        mt.column = parseIntOr(t->attr("controller-slot-id"), 0);
                        mt.target_table_id = parseIntOr(t->attr("component-id"), 0);
                        mt.target_index = parseIntOr(t->attr("target-number"), 1);
                        mt.target_param_id = t->attr("target-name");
                        mt.min_val = parseFloatOr(t->attr("min-value"), 0.0f);
                        mt.max_val = parseFloatOr(t->attr("max-value"), 1.0f);
                        mt.control_name = t->attr("controller-name");
                        out_preset.macros.push_back(std::move(mt));
                    }
                }
            }
        }
    }

    auto attrNum = [&](std::size_t from, std::string_view key) -> double {
        const auto k = raw_sv.find(key, from);
        if (k == std::string_view::npos) return 0.0;
        const auto q = raw_sv.find('"', k + key.size());
        const auto e = q == std::string_view::npos ? q : raw_sv.find('"', q + 1);
        if (e == std::string_view::npos) return 0.0;
        try { return std::stod(std::string(raw_sv.substr(q + 1, e - q - 1))); } catch (...) { return 0.0; }
    };
    if (const auto ht = raw_sv.find("<headroom-tempo"); ht != std::string_view::npos) {
        out_preset.tempo_bpm = attrNum(ht, "bpm=");
    } else if (const auto mf = raw_sv.find("<fix-component id=\"12000\""); mf != std::string_view::npos) {
        const auto me = raw_sv.find("</fix-component>", mf);
        const auto tp = raw_sv.find("id=\"Tmp\"", mf);
        if (tp != std::string_view::npos && (me == std::string_view::npos || tp < me)) out_preset.tempo_bpm = attrNum(tp, "value=");
    }
    if (!(out_preset.tempo_bpm >= 20.0 && out_preset.tempo_bpm <= 300.0)) out_preset.tempo_bpm = 0.0;

    if (const auto hl = raw_sv.find("<headroom-lineage"); hl != std::string_view::npos) {
        const auto he = raw_sv.find('>', hl);
        if (he != std::string_view::npos) {
            MinimalXmlParser lp(raw_sv.substr(hl, he - hl + 1));
            XmlNode ln;
            if (lp.parseRoot(ln)) {
                if (out_preset.info.based_on.empty()) out_preset.info.based_on = ln.attr("based-on");
                if (out_preset.info.based_on_path.empty()) out_preset.info.based_on_path = ln.attr("based-on-path");
            }
        }
    }

    if (const auto hl = raw_sv.find("<headroom-library"); hl != std::string_view::npos) {
        const auto he = raw_sv.find('>', hl);
        if (he != std::string_view::npos) {
            std::string tag(raw_sv.substr(hl, he - hl));
            if (!tag.empty() && tag.back() == '/') tag.pop_back();
            tag += "/>"; // parse the opening tag alone (its children aren't needed here)
            MinimalXmlParser lp(tag);
            XmlNode ln;
            if (lp.parseRoot(ln)) out_preset.info.instrument = ln.attr("instrument");
        }
    }

    if (const auto hs = raw_sv.find("<headroom-synth"); hs != std::string_view::npos) {
        const std::string_view close = "</headroom-synth>";
        if (const auto he = raw_sv.find(close, hs); he != std::string_view::npos) out_preset.extra_xml = std::string(raw_sv.substr(hs, he + close.size() - hs));
    }
    return true;
}

} // namespace rig::preset
