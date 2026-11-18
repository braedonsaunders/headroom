#pragma once
// Backing tracks for the Practice view: the library folder (~/Library/Application Support/Headroom/Backing
// Tracks, or $HEADROOM_BACKING_DIR), background decoding, the /api/backing routes and the small http page that
// hosts the YouTube IFrame player (a file:// page has no origin YouTube will accept, so the UI embeds
// http://127.0.0.1:<port>/practice/youtube.html instead). Contract: app/docs/ENGINE_API.md → "Backing tracks".
//
// Audio goes through BackingTrackPlayer, which the engine mixes in after the rig (RealtimeAudioServer
// renderChunk → player().process()).

#include "BackingTrackPlayer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rig::audio {

class BackingTracks {
public:
    struct Entry {
        std::string id, title, artist, file, format, original;
        double duration = 0.0;
        int64_t added = 0, last_used = 0; // unix milliseconds
        uint64_t size = 0;
    };
    using Query = std::function<std::string(const char*)>;

    BackingTracks() = default;
    ~BackingTracks();
    BackingTracks(const BackingTracks&) = delete;
    BackingTracks& operator=(const BackingTracks&) = delete;

    static std::string defaultDir();
    // Prepares the player and reads the library. Call before audio starts.
    void start(double sample_rate, const std::string& dir);
    void stop();
    // Device rate changed (audio stopped): re-prepares and re-decodes the loaded track at the new rate.
    void setSampleRate(double sample_rate);

    BackingTrackPlayer& player() { return player_; }
    const std::string& dir() const { return dir_; }

    static bool handlesPath(const std::string& path) { return path.rfind("/api/backing", 0) == 0; }
    std::string handleApi(const std::string& path, const Query& q, const std::string& body);
    std::string telemetryJson();

    // GET /practice/youtube.html (served with text/html by the engine's HTTP loop).
    static bool isYoutubeHostPath(const std::string& path) { return path == "/practice/youtube.html"; }
    static const std::string& youtubeHostHtml();
    static void sendHtml(int fd, const std::string& html);

    // Library operations (also used by tests).
    bool importFile(const std::string& path, Entry& out, std::string& err);
    bool rename(const std::string& id, const std::string& title);
    bool remove(const std::string& id);
    std::vector<Entry> list();
    // Starts decoding `id` in the background; it's handed to the player when done (paused unless `play`).
    bool load(const std::string& id, bool play, double start_sec, std::string& err);
    bool waitLoaded(int timeout_ms); // tests: true once no load is in flight
    std::string stateJson();

private:
    void readIndex();
    void writeIndexLocked();
    void scanFolderLocked();
    void startDecode(const Entry& e, bool play, double start_sec);
    void joinLoader();
    std::string entryJson(const Entry& e) const;
    std::string pathOf(const Entry& e) const { return dir_ + "/" + e.file; }

    BackingTrackPlayer player_;
    std::string dir_;
    bool started_ = false;

    std::mutex mtx_;                  // library + current-track metadata + player control calls
    std::vector<Entry> entries_;
    Entry current_;                   // loaded track (id empty = none)
    std::vector<float> peaks_, rms_;  // its overview
    float norm_gain_ = 1.0f;

    std::thread loader_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> loading_{false};
    std::string loading_id_, error_;
    uint64_t gen_ = 0;
};

} // namespace rig::audio
