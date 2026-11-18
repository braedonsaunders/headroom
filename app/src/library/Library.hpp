#pragma once
// Preset & component library: indexes every bundled and user rack preset (.rig), infers
// categories from the actual signal chain, provides component presets, and writes
// user presets in Headroom's XML format.
#include "../preset/PresetLoader.hpp"
#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rig::library {

struct LibraryEntry {
    int idx = 0;
    std::string name, author, category, amp, comment, path;
    std::string based_on;      // Source Library preset name (empty if built from scratch)
    std::string based_on_path; // Source Library preset path or id
    int64_t mtime = 0;         // File modified time (Unix seconds)
    std::vector<std::string> tags;
    bool featured = false;
    bool user = false;
    bool showcase = false; // bundled Headroom rigs (presets/Showcase)
    bool library = false;  // Headroom's shipped library (presets/Library/<Category>/*.rig)
    int units = 0;
    // Shipped-library metadata (<headroom-library> block); empty for other sources.
    int order = 500;
    std::string lib_id, style, instrument;
    std::string inspired_artist, inspired_song, inspired_album, inspired_part, inspired_year;
    std::vector<std::pair<std::string, std::string>> sources; // (url, title)
};

struct FeaturedSpec {
    std::string file;      // file name within the showcase dir
    std::string title;     // display title
    std::string category;  // curated category label
};

struct ComponentPreset {
    std::string name;
    std::vector<std::pair<std::string, float>> params;
};

class PresetLibrary {
public:
    // `library_dir` = Headroom's shipped library; empty -> $HEADROOM_LIBRARY_DIR, else the "Library" folder next to
    // `showcase_dir`.
    void scan(const std::string& user_dir, const std::vector<FeaturedSpec>& featured,
              const std::string& showcase_dir = {}, const std::string& library_dir = {});
    // The library categories in browse order (Featured first, the MIDI categories last).
    static const std::vector<std::string>& categoryOrder();
    std::vector<LibraryEntry> entries() const;
    bool entry(int idx, LibraryEntry& out) const;
    std::string json() const;
    std::string userDir() const { return user_dir_; }

    // Saves (or overwrites by name) a user preset; returns its library index or -1.
    int saveUser(const preset::RackPreset& preset, std::string* err = nullptr);
    bool deleteUser(int idx);
    // Renames a user preset at `idx` to `new_name`; returns its new library index or -1.
    int renameUser(int idx, const std::string& new_name, std::string* err = nullptr);
    // Duplicates a user preset at `idx`; returns the duplicate's library index or -1.
    int duplicateUser(int idx, std::string* err = nullptr);
    // Copies an already-scanned index (plugin instances share one scan per process, then copy it).
    void assignFrom(const PresetLibrary& other);

    static std::string classify(const preset::RackPreset& p, std::string& amp_name, std::vector<std::string>& tags);

private:
    void rebuildUserEntries();
    mutable std::mutex mtx_;
    std::vector<LibraryEntry> entries_;
    std::string user_dir_;
};

class ComponentPresetBank {
public:
    void scan(); // seeds every catalog unit's Init preset from its schema
    const std::vector<ComponentPreset>* presetsFor(int32_t table_id) const;
    // INIT preset values (true defaults) if available.
    const ComponentPreset* initFor(int32_t table_id) const;

private:
    std::map<int32_t, std::vector<ComponentPreset>> by_table_;
};

bool writeUserPreset(const std::string& path, const preset::RackPreset& p, std::string* err = nullptr);
void writeUserPresetTo(std::ostream& o, const preset::RackPreset& p);
std::string userPresetXml(const preset::RackPreset& p); // the same .rig XML, in memory (plugin state)
std::string sanitizeFileName(const std::string& name);
std::string appSupportDir(); // ~/Library/Application Support/Headroom

} // namespace rig::library
