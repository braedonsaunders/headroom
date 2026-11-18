#pragma once
// Audio driver families for the portable AudioIO (AudioIOMiniaudio.cpp). macOS uses CoreAudio (AudioIO.mm)
// and ignores this. The preferred backend comes from HEADROOM_AUDIO_BACKEND (alsa | pulse | jack | wasapi |
// wasapi-exclusive | asio) or setPreferredBackend(); Auto tries the platform's low-latency order:
//   Linux:   JACK (also PipeWire's JACK shim) → PulseAudio (PipeWire's Pulse shim) → ALSA
//   Windows: WASAPI shared by default. ASIO and WASAPI exclusive are explicit choices.
//
// ASIO is a separate implementation, not a miniaudio backend: it needs the Steinberg ASIO SDK (fetched at
// build time, never vendored — see docs/PORTING.md) and hosts the selected in-process driver on its own COM apartment.
#include <string>
#include <vector>

namespace rig::audio {

enum class AudioBackend { Auto, Jack, Pulse, Alsa, Wasapi, WasapiExclusive, Asio };

const char* backendName(AudioBackend b);
AudioBackend backendFromName(const std::string& name); // unknown → Auto
std::vector<AudioBackend> availableBackends();          // compiled-in for this platform, in Auto order
void setPreferredBackend(AudioBackend b);               // takes effect on the next devices() / start()
AudioBackend preferredBackend();
AudioBackend activeBackend();                           // what the open device context actually uses

} // namespace rig::audio
