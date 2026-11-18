#pragma once
// Per-user locations, per platform. Nothing here creates directories; callers do that where they always have.
//
//   appDataDir()   macOS   ~/Library/Application Support/Headroom
//                  Windows %APPDATA%\Headroom                     (Roaming; e.g. C:\Users\me\AppData\Roaming\Headroom)
//                  Linux   $XDG_DATA_HOME/Headroom, else ~/.local/share/Headroom
//                  HEADROOM_DATA_DIR overrides it everywhere (tests, portable installs).
//   homeDir()      $HOME (macOS/Linux), %USERPROFILE% (Windows)
//   documentsDir() ~/Documents (macOS/Linux, XDG_DOCUMENTS_DIR when set), the Known Folder on Windows
//   vst3Dirs()     the platform's standard VST3 folders (system first, then per-user)
#include <string>
#include <vector>

namespace rig::platform {

std::string homeDir();
std::string appDataDir();
std::string documentsDir();
std::vector<std::string> vst3Dirs();

// Root of the shipped content tree, laid out as <root>/app/{ui,presets,resources,third_party/...}: the repository
// root in dev builds, the staged bundle/install root in packages. Set once by the engine at start (the same
// `repo_root` RealtimeAudioServer::start/startHosted receive); empty until then.
void setAppRoot(const std::string& root);
std::string appRoot();

} // namespace rig::platform
