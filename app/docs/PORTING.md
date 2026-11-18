# Porting Headroom to Windows and Linux

Goal: ship Headroom as an installed desktop app on **macOS, Windows and Linux** from one source tree, with the
macOS build continuing to pass at every step. This document is the audit, the target architecture, the
packaging and CI plan, and the phased schedule. The "Status" section at the end records what builds today.

---

## 1. Inventory: what is macOS-specific today

### 1.1 App shell (`src/gui/StudioApp.mm`, 654 lines, Objective-C++)

| Feature | API | Notes for the port |
|---|---|---|
| Window, menus, app lifecycle | Cocoa `NSApplication`, `NSWindow`, `NSMenu` | per-platform shell |
| Web UI host | `WKWebView`, `loadFileURL:allowingReadAccessToURL:` (UI from `file://`) | WebView2 (Win), WebKitGTK (Linux) |
| JS → native bridge | `window.webkit.messageHandlers.headroom.postMessage({action,…})` (14 call sites in `ui/`) | `window.HRNative.post` + polyfill (§2.2) |
| Native → JS | `evaluateJavaScript:` calling `window.headroomSecretReply`, `headroomAiFetch`, `headroomBackingImport`, `headroomBackingDrag`, import callbacks | same callbacks via `webview_eval` / `ExecuteScript` |
| Injected scripts | `WKUserScript` at document start: `window.RIG_PORT`, error/console forwarding (`jslog`) | `webview_init` |
| API keys (Guitar Tech) | Keychain `SecItemAdd/Update/CopyMatching/Delete`, service `com.headroom.studio.ai` | Credential Manager / libsecret (§2.7) |
| AI fetch proxy (CORS-free streaming fetch from a `file://` page) | `NSURLSession` data delegate → base64 chunks into JS | WinHTTP or libcurl (§2.2) |
| File pickers (IRs, NAM, rig presets, backing tracks) | `NSOpenPanel`, `UTType` (`UniformTypeIdentifiers`) | IFileOpenDialog / GTK file chooser (portals) |
| Drag and drop of files (rig presets, audio) | `NSDraggingDestination` on a `WKWebView` subclass | WebView2 `AllowExternalDrop` + JS `drop` / GTK DnD |
| Reveal in Finder / open URL | `NSWorkspace openURL:` / `activateFileViewerSelectingURLs:` | `ShellExecuteW`, `SHOpenFolderAndSelectItems` / `xdg-open`, D-Bus `org.freedesktop.FileManager1` |
| Single instance | `NSRunningApplication terminate` on prior instances | named mutex (Win) / lock file + socket (Linux) |
| Screenshot mode (UI checks) | `WKWebView takeSnapshot` | optional per shell |
| Paths | `NSBundle mainBundle` resource path, `RIG_APP_SOURCE_DIR` | exe-relative + `platform::Paths` |

### 1.2 Engine

| File | Apple API | Purpose | Port |
|---|---|---|---|
| `src/audio/AudioIO.mm` (369) | CoreAudio HAL (`AudioObject*`), AUHAL output/input units, `AVCaptureDevice` mic permission | device enumeration, independent in/out devices with a drift-controlled ring, SR/buffer control, latency, underruns | `AudioIO.hpp` is already a pimpl interface → miniaudio backend + ASIO backend (§2.1) |
| `src/audio/RealtimeAudioServer.hpp` (3113) | `<AudioToolbox>` include, `pthread_set_qos_class_self_np`, POSIX sockets, `SO_NOSIGPIPE`, `signal(SIGPIPE)`, `$HOME` | the engine: HTTP API on 127.0.0.1, render loop, telemetry | `platform/Socket.hpp` (Winsock), thread priority shim, `platform::homeDir()` |
| `src/audio/RealtimeAudioServerHosted.inl` | same (plugin-hosted mode) | per-plugin-instance engine + HTTP | same |
| `src/audio/BackingTrackPlayer.{hpp,cpp}` | `ExtAudioFile` (decode WAV/AIFF/CAF/MP3/AAC/ALAC/FLAC + tags), `AudioConverter` resampling, **AUNewTimePitch** (speed/pitch) | Practice backing tracks | decoder + time-stretch interfaces (§2.4) |
| `src/audio/BackingTracks.cpp` | `<sys/socket.h>` (`send`), `<sys/stat.h>` | backing-track library + YouTube host page | `platform/Socket.hpp` |
| `src/dsp/ConvolutionEngine.hpp` | vDSP `fft_zrip`, `ctoz/ztoc`, `zvma`, `dotpr` | partitioned cabinet/IR convolution (audio thread) | `platform/VDsp.hpp` (§2.3) ✅ |
| `src/midi/NeuralGuitarMidi.hpp` | vDSP FFT, `vDSP_mmul`, `vadd/vsadd/vsma` | neural guitar → MIDI (CQT front end + matmuls) | `platform/VDsp.hpp` ✅ |
| `src/midi/PitchToMidi.hpp` | vDSP FFT, `mmul`, `mtrans`, `distancesq`, `svesq`, `vsmul` | YIN + NMF pitch detection | `platform/VDsp.hpp` ✅ |
| `src/analysis/ListenAnalyzer.hpp` | vDSP FFT (already `#ifdef __APPLE__` with a naive fallback that allocates) | Listen/key analysis (not audio thread) | can switch to `VDsp.hpp` unconditionally |
| `src/midi/MidiComponents.cpp` | AudioUnit `MusicDevice` hosting: Apple **DLSMusicDevice** (128 GM sounds) + any installed AU instrument | Instrument unit's GM/AU sounds | ✅ stubbed off Apple (silent, indices kept); SoundFont player later (§2.8) |
| `src/midi/MidiOut.mm` | CoreMIDI virtual source "Headroom MIDI Out", `mach` thread policy | guitar → MIDI to DAWs | ALSA sequencer (Linux), Windows MIDI Services (§2.5) |
| `src/plugins/PluginHost.cpp` (1378) | AudioUnit v2/v3 hosting (`AudioComponent*`, `AudioUnitRender`, ClassInfo state as binary plist, `CFPropertyList`), `dispatch_*` to main thread, `pthread_main_np` | third-party plugins in the rack | split into core + backends; VST3 backend (§2.9) |
| `src/plugins/PluginEditor.mm` | Cocoa windows, `AUCocoaUIView`, AUv3 view controllers, CoreAudioKit generic view | plugin editor windows | per-backend editor windows |
| `src/dsp/LoopStation.cpp` | POSIX `mkdir`, `localtime_r` | loop export | `std::filesystem`, portable localtime |
| `src/library/Library.cpp`, `ToneMatchAndCalibration.cpp`, `ConvolutionEngine.hpp`, `NamComponent.cpp`, `TubeAmpComponents.cpp`, `PluginHost.cpp` | hard-coded `~/Library/Application Support/Headroom/…`, `/Library/Audio/Plug-Ins/VST3` | user data, IRs, NAM captures, amp-trim cache, plugin store | `platform/Paths` ✅ |

### 1.3 Build system and tests

* `CMakeLists.txt`: `LANGUAGES CXX OBJCXX`, `-framework …`, `-Wl,-force_load` (NAM registers its architectures through static initialisers), GCC-style flags everywhere. ✅ now portable (§6).
* `tests/test_plugins.cpp` (Apple AUs), `tests/test_backing_track.cpp` (writes AIFF/M4A with ExtAudioFile; heap check through `DYLD_INSERT_LIBRARIES` + `tests/rtguard.cpp` using Mach-O `__interpose`), `tests/test_dsp_quality.cpp` (vDSP), and POSIX-isms in tests (`setenv`, `posix_memalign`, `mkdir`, `unistd.h`, `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`).
* GCC also exposed a latent link dependency: `test_rack_snapshots` needs `rig_engine` (clang dropped the inline factory call).
* No denormal handling anywhere: fine on Apple Silicon, **a CPU-spike risk on x86** (§2.3).

### 1.4 DAW plugin (`app/plugin`)

* `plugin/au/HeadroomAU.mm` (AUv2, Apple AudioUnitSDK, Apache-2.0): macOS only by nature.
* `plugin/vst3/HeadroomVST3.mm`: the processor is portable C++ on the VST3 SDK (MIT since 3.8); the `.mm` is Cocoa for the editor view.
* `plugin/editor/HeadroomEditor.mm`: a `WKWebView` editor loading the same UI against the instance's hosted HTTP port.
* `plugin/core/HeadroomPluginCore.cpp`: portable C++ (hosted engine). `plugin/tests/hr_host.mm` and `test_plugin_core.cpp` use `mach`/AudioToolbox.
* `plugin/CMakeLists.txt` reads the `rig_engine` source list from `app/CMakeLists.txt` with a regex. Keep that list a plain list (it is).

### 1.5 Already portable

NeuralAmpModelerCore + Eigen + nlohmann/json, all amp/pedal/cab/FX DSP (`src/dsp`, `src/core`), presets, the library and showcase code, the UI (plain HTML/JS/CSS talking HTTP to `127.0.0.1`), the VST3 SDK.

---

## 2. Target architecture

Principle: **keep native C++, keep the HTML UI, put every OS dependency behind a small interface in
`src/platform/` (or an existing pimpl header), and keep the macOS implementation exactly as it is.** The engine
already talks to the UI over HTTP, so the shell is thin by design.

```
            ui/ (HTML/JS, unchanged)  ── HTTP 127.0.0.1 ──►  RealtimeAudioServer (engine)
                 │ HRNative.post                                  │
   ┌─────────────┴──────────────┐                    ┌────────────┴─────────────────────────────┐
   │ Shell (per OS)             │                    │ platform/ + pimpl seams                  │
   │  mac: StudioApp.mm (WKWebView)                  │  AudioIO      CoreAudio | miniaudio | ASIO │
   │  win: shell + WebView2     │                    │  VDsp         Accelerate | portable       │
   │  lin: shell + WebKitGTK    │                    │  AudioDecoder ExtAudioFile | miniaudio(+MF)│
   │  bridge, dialogs, DnD,     │                    │  TimeStretch  AUNewTimePitch | Signalsmith│
   │  secrets, AI fetch proxy   │                    │  MidiOut      CoreMIDI | ALSA seq | WinMIDI│
   └────────────────────────────┘                    │  Paths, Socket, Denormals, Secrets        │
                                                     │  PluginHost   AU | VST3 (| CLAP)          │
                                                     └──────────────────────────────────────────┘
```

### 2.1 Audio I/O: **miniaudio** for everything except ASIO, plus a native **ASIO** backend on Windows

* **miniaudio** (single header, public domain / MIT-0): WASAPI (shared, low-latency shared via `IAudioClient3`, exclusive),
  DirectSound, WinMM; ALSA, PulseAudio, JACK on Linux (PipeWire through its Pulse/JACK/ALSA shims); CoreAudio.
  No build-system dependencies, permissive, and it implements the existing `AudioIO.hpp` interface (`src/audio/AudioIOMiniaudio.cpp`).
  The Mac build keeps `AudioIO.mm`.
  * RtAudio (MIT-like) is the runner-up. It has ASIO, but only by compiling Steinberg SDK files, so it doesn't solve the licensing.
    PortAudio (MIT) is similar and adds a CMake dependency.
* **Latency.** Guitarists feel more than about 10 ms round trip.
  * Windows: WASAPI exclusive or ASIO at 64 to 128 frames. Shared-mode WASAPI is typically 10 to 20 ms; IAudioClient3 can reach about 3 ms periods on supporting drivers. Offer ASIO > WASAPI exclusive > WASAPI shared in the device panel.
  * Linux: JACK/PipeWire with a 64 to 128 quantum, or ALSA `hw:` direct. PulseAudio is a last resort.
  * RT priority: PipeWire/JACK give the callback thread RT priority; for ALSA, request it through rtkit (D-Bus) or document `@audio - rtprio 95` in limits.conf.
* **ASIO (required on Windows; user decision).**
  * Steinberg offers SDK 2.3.4 under GPLv3 or the proprietary Steinberg ASIO licence. A GPLv3 distribution can charge for the app and does not require a countersigned proprietary agreement. For a downloadable GPLv3 Windows binary, offer the corresponding source for the combined work, including the build scripts, and include the applicable licences and notices. Publishing under the proprietary route requires Steinberg’s countersignature. [Official SDK and licence terms](https://www.steinberg.net/asiosdk) · [Steinberg licensing FAQ](https://forums.steinberg.net/t/asio-sdk-proprietary-licensing-faq/1044024) · [GPLv3 sections 5–6](https://www.gnu.org/licenses/gpl-3.0.html)
  * Set `RIG_ASIO_SDK_DIR` to Steinberg’s extracted SDK root. CMake requires `RIG_WITH_ASIO=ON` for every Windows release bundle.
  * CI downloads SDK 2.3.4 directly from Steinberg and verifies the pinned SHA-256 before extracting it. The Windows package includes the official ASIO Compatible artwork for Settings and About.
  * Avoid "clean-room" ASIO headers floating around GitHub: they're of unclear provenance. Wine's `wineasio` is GPL/LGPL and Linux-side only.
  * Backend shape: `AudioBackend::Asio` in the same `AudioIO` pimpl.
    * Enumerate drivers from `HKLM\SOFTWARE\ASIO`, instantiate through COM (`CoCreateInstance` with the driver CLSID), then `init`, `getChannels`, `getBufferSize`, `createBuffers`, and handle `bufferSwitchTimeInfo`.
    * Convert the driver sample type (Int32LSB / Float32LSB / Int24LSB) to float, and drive the same `RenderFn`.
    * One ASIO driver is in and out at once, so there's no drift ring. Support `kAsioResetRequest`, buffer-size and latency change messages, and the driver's control panel (`controlPanel()`, exposed as an "ASIO settings…" button).
* **Mic permission.** Windows 10/11 has a per-app microphone privacy switch (WASAPI returns `E_ACCESSDENIED`), so map it to `micPermission()="denied"` and deep-link to `ms-settings:privacy-microphone`. Linux needs nothing, except a portal under Flatpak.
* Every callback constructs `platform::ScopedNoDenormals` (FTZ/DAZ). See §2.3.

### 2.2 App shell: **native C++ + system webview** (no Electron)

Confirmed: **not Electron.**
* Electron adds about 150 to 250 MB per install and a second JS runtime and process tree, and its renderer and GC compete with the audio thread for CPU and memory bandwidth.
* We'd still need a native addon for audio, so it buys nothing: the UI already speaks HTTP and needs only about 15 native actions.
* Tauri is closer, but brings a Rust toolchain into a C++ project for the same system webviews we can drive directly.

Options:
* **webview/webview** (MIT, single C/C++ library: WebView2 on Windows, WebKitGTK on Linux, WKWebView on macOS) for Windows and Linux. The macOS shell stays `StudioApp.mm`; it has years of polish (drag and drop, Keychain, screenshot mode, menus).
* Where webview/webview's lowest-common-denominator API isn't enough (drag and drop, file-drop paths, context menus), use its `webview_get_native_handle` to reach the `ICoreWebView2` / `WebKitWebView` underneath and add per-OS code in `src/gui/shell/<os>/`.
* **Bridge:** one JS abstraction, `window.HRNative.post(msg)`, defined by every shell at document start:
  * macOS: `HRNative.post = m => webkit.messageHandlers.headroom.postMessage(m)`.
  * Windows/Linux: `HRNative.post = m => window.__hr_post(JSON.stringify(m))` (a webview `bind`).
  * Until the UI migrates, non-mac shells also install a **polyfill** `window.webkit.messageHandlers.headroom.postMessage = HRNative.post`, so `ui/` needs no changes now.
  * Later, one mechanical UI commit can replace the 14 `webkit.messageHandlers.headroom` call sites with `HRNative.post`.
* Native → JS stays the same named callbacks (`headroomSecretReply`, `headroomAiFetch`, …).
* **AI fetch proxy:** WinHTTP (built into Windows, async, streaming) on Windows; **libcurl** (curl licence, MIT-style) on Linux. Same `{id,type:"head"|"data"|"end"}` protocol as `HRAiFetchProxy`.
  * Alternative: serve the UI from the engine's own HTTP server (`http://127.0.0.1:<port>/ui/`) and proxy AI calls through the engine. That removes the `file://` CORS problem for all shells at once and lets the AI proxy live in portable C++ (with libcurl/WinHTTP inside the engine). Worth doing when the shells are unified.
* **WebView2 runtime:** preinstalled on Windows 11 and on updated Windows 10. The installer bundles the **Evergreen bootstrapper** (about 2 MB) and runs it if `GetAvailableCoreWebView2BrowserVersionString` fails. The WebView2 SDK comes from NuGet (`Microsoft.Web.WebView2`); its loader is redistributable.
* **Linux:** WebKitGTK 4.1 (`libwebkit2gtk-4.1`, LGPL, dynamically linked, so it's fine for a sellable app). Ubuntu 22.04+, Debian 12+ and Fedora all ship it.

### 2.3 DSP portability ✅ started

* `src/platform/VDsp.hpp` implements the exact vDSP subset the engine uses: `vDSP_fft_zrip` with vDSP's packing (DC/Nyquist in `realp[0]/imagp[0]`) and scaling (2× forward, unnormalised inverse), `ctoz/ztoc/zvma/dotpr/mmul/mtrans/vadd/vsadd/vsmul/vsma/svesq/distancesq`. It uses the same names, so DSP files only changed their `#include`.
  * On macOS it *is* Accelerate.
  * `tests/test_vdsp_portable.cpp` checks the portable code against a double-precision DFT on every OS and **against Accelerate itself on macOS** (bit-for-bit layout, ≤2e-5 relative error).
* Speed: the portable radix-2 FFT is about 5× slower than Accelerate (M-series: N=256 0.9 µs vs 0.17 µs; N=8192 31 µs vs 5.4 µs). The partitioned convolver's spectral multiply-accumulate dominates for long IRs, so this matters only a little.
  * Next step, behind the same header: **pffft** (BSD-like FFTPACK licence, SSE/NEON, about vDSP speed) for the FFT.
  * `vDSP_mmul`: a contiguous i-k-j kernel (auto-vectorised). If NeuralGuitarMidi profiles hot, use Eigen's GEMM (already vendored, MPL-2.0) on the non-RT worker thread only; Eigen may heap-allocate blocking buffers, so keep it off the audio thread.
* **Denormals:** `src/platform/Denormals.hpp` (`ScopedNoDenormals`: MXCSR FTZ+DAZ on x86, FPCR.FZ on arm64). Required in every non-Apple audio callback and in the VST3 `process()` on x86.
* **MSVC codegen:** the tube-amp solvers are scalar-heavy. Compare `test_circuit_budget` between MSVC and **clang-cl** (an optional VS Build Tools component), and ship whichever meets the per-block budget. clang-cl also keeps GCC/Clang-style diagnostics identical across platforms.

### 2.4 Audio decoding and time-stretch (backing tracks)

* Decoder interface (`platform/AudioDecoder`): probe (length, rate, channels, tags, format) plus chunked decode to float stereo at the engine rate.
  * macOS keeps ExtAudioFile.
  * Elsewhere (✅ implemented): **miniaudio's built-in decoders**, which are dr_wav/dr_flac/dr_mp3 (public domain / MIT-0): WAV (incl. RF64/W64), AIFF, FLAC, MP3, with ID3/Vorbis/RIFF-INFO/AIFF tags. Resampling uses Headroom's Kaiser-windowed sinc: 44.1→48 kHz residual ≤ −82 dB, 96→48 kHz alias rejection about −94 dB. **stb_vorbis** for OGG is not wired yet.
  * Windows: **Media Foundation** (`IMFSourceReader`; part of the OS, no licence issue) for AAC/M4A/ALAC/WMA.
  * Linux AAC: **don't ship an AAC decoder**. FFmpeg/fdk-aac raise patent and licence questions (fdk-aac isn't GPL-compatible; FFmpeg is LGPL/GPL depending on the build). Use the system GStreamer if present, or tell users to convert. MP3 patents have expired.
* Time-stretch interface (`platform/TimeStretch`): macOS keeps AUNewTimePitch. Elsewhere use **Signalsmith Stretch** (MIT, real-time, polyphonic, good quality).
  * **Rubber Band is GPLv2+/commercial**: excluded unless a commercial licence is bought.
  * SoundTouch is LGPL 2.1 (usable dynamically linked, but weaker on polyphonic material).

### 2.5 MIDI out

* **Linux:** ALSA sequencer virtual port "Headroom MIDI Out" (libasound, LGPL, dynamic) is the exact equivalent of the CoreMIDI virtual source. JACK MIDI is optional.
* **Windows** has no app-created virtual ports in classic WinMM.
  * **Windows MIDI Services** (Windows 11 24H2+, open source, MIT) adds app-to-app virtual devices: use it when present.
  * Otherwise list output ports (WinMM via **libremidi**, BSD-2) so users can target loopMIDI or a hardware port.
  * teVirtualMIDI (used by loopMIDI) needs a licence from Tobias Erichsen for redistribution: avoid it.
* Library: **libremidi** (BSD-2, C++20, ALSA/JACK/WinMM/WinUWP/CoreMIDI) rather than RtMidi (MIT-like, older API). Keep `MidiOut.mm` on the Mac.

### 2.6 Paths ✅

`src/platform/Paths.{hpp,cpp}`:

| | user data (`appDataDir()`) | VST3 |
|---|---|---|
| macOS | `~/Library/Application Support/Headroom` (unchanged) | `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3` |
| Windows | `%APPDATA%\Headroom` (UTF-8 via `_wgetenv` / `SHGetKnownFolderPath`) | `%CommonProgramFiles%\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3` |
| Linux | `$XDG_DATA_HOME/Headroom` or `~/.local/share/Headroom` | `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.vst3` |

* `HEADROOM_DATA_DIR` overrides the data dir everywhere (tests, portable installs).
* Windows: engine code passes UTF-8 `std::string` paths to `std::filesystem`/`fopen`. Embed an app manifest with `<activeCodePage>UTF-8</activeCodePage>` (Windows 10 1903+) so narrow APIs are UTF-8 process-wide. That's simpler and safer than converting every call site to `std::wstring`.
* Shipped content (UI, library rigs, IRs, models): `Contents/Resources` on macOS, `<install>\` on Windows, `<prefix>/share/headroom` on Linux. The engine's `repoRoot`-relative lookups need a resource-dir resolver (`platform::resourceDir()`) rather than "parent of `ui/`" (see §3.5).

### 2.7 Secrets

`platform/Secrets` with `get/set/delete(account)` and async-safe semantics (calls may block on a prompt, so they run off the UI thread):
* macOS Keychain: the existing code, moved behind the interface.
* Windows **Credential Manager**: `CredWriteW/CredReadW/CredDeleteW`, target `Headroom/ai/<account>`.
* Linux **libsecret** (LGPL, dynamic; Secret Service on GNOME Keyring/KWallet). If no Secret Service is running (minimal WMs, WSL), report "no keyring available" rather than silently writing plaintext.

### 2.8 Instruments (GM bank)

macOS uses Apple's DLS synth. Elsewhere use **TinySoundFont** (MIT) with a **permissively licensed GM SoundFont**: GeneralUser GS (its licence permits redistribution and commercial use), FluidR3_GM (MIT), or a trimmed set. Keep the 128 "GM ·" list positions identical so rigs are portable; until then these sounds are silent off macOS (current stub).

### 2.9 Plugin hosting

1. Refactor `PluginHost.cpp` into a **format-agnostic core**: registry, ids, persistence, schemas, placeholders, `remapPreset`, `/api/plugins*`, the rack `IComponent`. Under it goes a **`PluginBackend` interface**: scan, describe (params), instantiate, prepare, process, state get/set, parameter get/set/format, latency/tail, editor open/close.
   * The current non-Apple `PluginHostNull.cpp` is scaffolding for exactly this: presets with plugins load as state-preserving pass-through placeholders.
2. **AU backend**: the current code (macOS only).
3. **VST3 backend** on all three OSes, with the VST3 SDK hosting classes (`public.sdk/source/vst/hosting`, MIT).
   * State is the component + controller streams (base64 in the same `state` string param).
   * Editors: `IPlugView` attached to an NSView / HWND / X11 window (Linux hosts must provide `IRunLoop`).
   * The uid is `VST3:<class-uid>`. The placeholder path already recognises it.
4. **CLAP** (MIT) is optional and comes after VST3; the backend interface makes it cheap.

### 2.10 The DAW plugin (`app/plugin`)

| | macOS | Windows | Linux |
|---|---|---|---|
| VST3 | ✓ (today) | ✓ | ✓ |
| AUv2 | ✓ (today) | n/a | n/a |
| CLAP | optional | optional | optional |
| AAX | later (needs an Avid developer agreement + PACE signing) | later | n/a |

* The processor and `HeadroomPluginCore` are portable once the engine is. Needed work:
  * replace `HeadroomVST3.mm` Cocoa bits with per-OS view glue;
  * the editor: WebView2 in a child HWND on Windows. On Linux, embed WebKitGTK in the host's X11 window via a GtkPlug/XEmbed or a reparented X11 window. That's the riskiest piece, so the fallback is a floating editor window that follows the plugin view;
  * the VST3 bundle layout per OS (`Headroom.vst3/Contents/x86_64-win/Headroom.vst3`, `…/x86_64-linux/Headroom.so`);
  * `validator` in CI.
* Windows ARM64 and Linux aarch64 come later.

---

## 3. Packaging

### 3.1 macOS: universal, signed, notarized `.dmg`

* `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. The code has no intrinsics, and NAM/Eigen build universal.
* Sign the app and the plugins with **Developer ID Application**, hardened runtime (`--options runtime`), entitlements `com.apple.security.device.audio-input`. JIT entitlements aren't needed: WKWebView runs JS out of process.
* `.dmg` (drag to Applications). Or a `.pkg` if the AU/VST3 plugins install too: a `.pkg` can put `Headroom.component` / `Headroom.vst3` into `/Library/Audio/Plug-Ins/…`, so **ship a `.pkg` once plugins ship** (it needs a **Developer ID Installer** certificate too).
* `xcrun notarytool submit --keychain-profile <profile> --wait`, then `xcrun stapler staple`.
* Current machine state:
  * only an **Apple Development** identity is installed;
  * there's **no Developer ID Application/Installer certificate** and no stored `notarytool` profile;
  * `xcode-select` points at CommandLineTools (notarytool works from there).
* See `app/packaging/macos/`.

### 3.2 Windows: Inno Setup installer (+ MSIX later)

* **Inno Setup**. Its LICENSE still permits any use, but since 6.5.0 (Aug 2025) **commercial users are asked to buy a licence** (one-time, perpetual; [jrsoftware.org](https://jrsoftware.org/isinfo.php)). Budget for it if Headroom is sold; NSIS (zlib licence) is the no-cost alternative. It installs `Headroom.exe` + `ui/` + content to `{autopf}\Headroom`, Start-menu entry, the VST3 to `{commoncf64}\VST3\Headroom.vst3` (64-bit Common Files, Steinberg's standard), and the WebView2 Evergreen bootstrapper when needed.
* MSIX is optional later: it can't write into Common Files for the VST3, so it's a poor fit while the plugin ships in the same installer.
* **Code signing:** SmartScreen reputation needs an OV or EV code-signing certificate, or **Azure Trusted Signing (now "Artifact Signing")**, about $10/month, identity validation required; check eligibility for the user's country and entity type. OV/EV certificates now have to live on an HSM, which makes the cloud service the practical route. Signing happens in CI with `signtool` (or `azure/trusted-signing-action`), with an RFC 3161 timestamp. Sign the exe, the installer and the VST3.
* ASIO builds carry Steinberg's credit (§2.1).

### 3.3 Linux: AppImage + .deb (Flatpak optional)

* **.deb** via CPack (`Depends: libwebkit2gtk-4.1-0, libgtk-3-0, libasound2, libsecret-1-0`), FHS layout `/usr/bin/headroom`, `/usr/share/headroom/`, `.desktop` + icons, VST3 to `/usr/lib/vst3`.
* **AppImage** via linuxdeploy + its GTK plugin. Bundling WebKitGTK is heavy (about 100 MB) and brittle across glibc versions: build on the oldest supported distro (Ubuntu 22.04), and consider an AppImage that depends on the system WebKitGTK.
* **Flatpak** (GNOME runtime includes WebKitGTK): the cleanest long-term distribution. The sandbox needs `--socket=pulseaudio`/PipeWire, `--filesystem=xdg-music` (or portals), and the VST3 plugin can't be installed from a Flatpak.

### 3.4 What ships

* `ui/` (with the brand assets),
* the library and showcase rigs (`presets/Library`, showcase definitions),
* Headroom's own IRs and generated content,
* the NAM demo captures (MIT, from NeuralAmpModelerCore `example_models`),
* `resources/` (icons),
* the DI tracks that are CC BY 4.0 (Guitar-TECHS, GuitarSet) **with** `samples/di/SOURCES.md` attribution,
* the third-party licence notices (NAM MIT, Eigen MPL-2.0, nlohmann MIT, VST3 SDK MIT, AudioUnitSDK Apache-2.0, miniaudio MIT-0 (incl. dr_libs), webview MIT, Signalsmith Stretch + Linear MIT, and the WebView2 loader licence).

`app/packaging/content-manifest.txt` is the allowlist and `app/packaging/bundle-denylist.txt` the denylist; `app/packaging/check-bundle.sh` enforces both on every staged bundle in CI.

### 3.5 What must NEVER ship

* Anything matched by `app/packaging/bundle-denylist.txt`: local untracked folders, `samples/di`, `work/`, research scripts, the build trees, and disallowed file types.
* DI recordings from a licensed research DI dataset (not distributed). See `app/samples/di/SOURCES.md`.

Content audit results: see §7.2.

---

## 4. CI (GitHub Actions; designed locally, **not pushed**)

* The project isn't a git repo yet. **Decision for the user:** hosting (GitHub vs other), visibility, and when. Until then the workflow files live locally under `.github/workflows/` at the repository root, covered by the allowlist `.gitignore`.
* Matrix, one job each:
  * `macos-14`: universal; ctest on arm64; package `.dmg` (`.pkg` with plugins).
  * `windows-2022`: MSVC (and clang-cl for comparison); ctest; Inno Setup; signtool when secrets exist.
  * `ubuntu-24.04` (+ `ubuntu-22.04` for the AppImage glibc floor): GCC; ctest; `.deb` + AppImage.
* Steps: configure (Ninja, Release) → build → `ctest --output-on-failure` → `check-bundle.sh` → package → upload artifacts.
* Secrets:
  * `APPLE_DEVELOPER_ID_P12` + password, `APPLE_NOTARY_KEY` (App Store Connect API key);
  * `WINDOWS_SIGNING_*` (or the Azure Trusted Signing service principal);

* Signing and notarization run only when the secrets are present (forks and PRs build unsigned).
* Content safety: CI never has access to `samples/di` or other untracked content (it isn't in the repo), and `check-bundle.sh` fails the build if anything on the denylist appears.

---

## 5. Phased plan (one experienced engineer; calendar time is longer with review/QA)

| # | Phase | Effort | Risks / notes |
|---|---|---|---|
| 0 | Abstractions + engine/tests on Linux & Windows (this change) | 3–5 d | ✅ largely done, see Status |
| 1 | Audio I/O: miniaudio backend hardened (device changes, independent in/out with drift ring, exclusive mode, latency report), Windows mic privacy | 5–8 d | Linux latency depends on the user's stack (PipeWire quantum, RT limits); USB interface quirks |
| 2 | ASIO backend | 4–6 d | Confirm distribution licence; driver quirks (buffer-size changes, reset requests); test on 2–3 interfaces (Focusrite, RME, Behringer) |
| 3 | Portable shells: Windows (WebView2) + Linux (WebKitGTK); bridge, dialogs, DnD, reveal, single instance, secrets, AI proxy | 8–12 d | WebView2 runtime presence; WebKitGTK `file://` → `http://127.0.0.1` fetch policy; DnD file paths on GTK/Wayland |
| 4 | Backing tracks: ✅ miniaudio decoders + Signalsmith done; Media Foundation (AAC/M4A/ALAC) left | 1–2 d | stretch quality vs AUNewTimePitch; AAC on Linux (decided: not shipped) |
| 5 | MIDI out (ALSA seq, Windows MIDI Services / port list) + GM SoundFont player | 4–6 d | Windows virtual-port availability by OS version |
| 6 | PluginHost core/backends split + VST3 hosting (+ editors) | 10–15 d | plugin editor embedding per OS; misbehaving plugins (sandboxing out of scope) |
| 7 | DAW plugin VST3 on Windows/Linux (+ CLAP optional) | 6–12 d | **Linux editor embedding of WebKitGTK** (fallback: floating window) |
| 8 | Packaging, signing, notarization, CI | 5–8 d | certificate lead time (Apple Developer ID: same day once enrolled; Windows OV/EV or Trusted Signing: days–weeks of identity validation) |
| 9 | Performance + QA: pffft, MSVC vs clang-cl, denormals, x86 CPU budget, real-hardware latency tests, HiDPI, accessibility | 5–10 d | x86 CPUs without AVX2; the 8× oversampled amps' CPU cost on older laptops |

**Total ≈ 55–85 engineer-days** (about 3–4 months for one person; about 6–8 weeks with two working shell/audio and plugin/packaging in parallel). The standalone Windows beta includes the ASIO host in phase 2.

### Key risks

1. **ASIO licensing**. Select the distribution licence before sharing the ASIO-enabled build externally (§2.1).
2. **Notarization accounts**: a Developer ID certificate needs a paid Apple Developer Program membership (the team Account Holder creates the Developer ID certificates).
3. **WebView2 runtime**: bundled bootstrapper; offline installers can embed the Evergreen standalone installer (about 150 MB) if needed.
4. **Linux audio latency**: document the PipeWire/JACK setup; default to JACK when a server is running; show the measured round-trip latency in the UI.
5. **x86 performance**: denormals (fixed by ScopedNoDenormals), FFT speed (pffft), MSVC codegen (clang-cl).
6. **Content licensing**: enforced by the allowlist + `check-bundle.sh`.
7. **Project licence**. The checkout has third-party notices and no top-level Headroom licence. Document the owner’s licence choice for distribution and retain the notices for each bundled dependency.

---

## 6. Build notes

```sh
# macOS (unchanged)
cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C app/build && ctest --test-dir app/build
# Linux (Ubuntu 24.04: build-essential cmake ninja-build; the app also needs libwebkit2gtk-4.1-dev libgtk-3-dev libasound2-dev …)
cmake -S app -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build && ctest --test-dir build
# Windows (VS 2022 Build Tools 17.14 / MSVC 19.44, Windows SDK 10.0.26100; CMake + Ninja ship with Build Tools)
"%VS%\VC\Auxiliary\Build\vcvars64.bat" & cmake -S app -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release & ninja -C build-win & ctest --test-dir build-win
```

CMake portability:
* `RIG_WARN/RIG_O1..3/RIG_NOWARN` map to GCC/Clang flags or MSVC equivalents.
* MSVC gets `/utf-8 /bigobj /Zc:__cplusplus /permissive-`, `_USE_MATH_DEFINES NOMINMAX`.
* NAM's static registration is kept with `-force_load` (Apple), `--whole-archive` (GNU) or `/WHOLEARCHIVE` (MSVC).
* `PluginHost.cpp` stays in the `rig_engine` list (the plugin project reads it) and is excluded off Apple in favour of `PluginHostNull.cpp`.

---

## 7. Status

### 7.1 What builds and passes (8 Nov 2026)

| OS / toolchain | Engine + `rig_cli` | CTest | App (`Headroom`) |
|---|---|---|---|
| macOS 26, Apple clang (arm64) | ✅ | ✅ 22/22 (`dsp_quality`'s CPU-budget check can trip when the machine is heavily loaded; it passes rerun) | ✅ `StudioApp.mm`, unchanged |
| macOS universal (arm64 + x86_64) | ✅ (`packaging/macos/build-release.sh` dry run) | — | ✅ signed with Apple Development + hardened runtime; dmg 52 MB, plugin pkg 80 MB (`spctl` rejects, as expected without Developer ID) |
| Linux: Ubuntu 24.04 (WSL2), GCC 13.3 | ✅ | ✅ 21/21 | ✅ webview/WebKitGTK shell, miniaudio. Ran under WSLg: UI renders, engine API up, PulseAudio output 256 frames ≈ 10.7 ms, clean quit on SIGTERM. `.deb` 42 MB; AppImage 67 MB (experimental) |
| Windows 11, MSVC 19.44 (VS 2022 Build Tools 17.14), Ninja | ✅ | ✅ 21/21 | ✅ `Headroom.exe` builds and links (WebView2 SDK 1.0.4314.35 from NuGet). Not launched yet. Inno Setup script written, not compiled (Inno isn't installed) |

New portable pieces:
* `src/platform/`:
  * `VDsp.hpp`, `Paths.*` (plus `setAppRoot`/`appRoot`), `Socket.hpp`, `Denormals.hpp`, `Secrets.*` (Credential Manager / libsecret), `AudioDecoder.*`, `TimeStretch.*`.
* Audio and MIDI:
  * `src/audio/AudioIOMiniaudio.cpp` + `AudioBackend.hpp` (`HEADROOM_AUDIO_BACKEND=alsa|pulse|jack|wasapi|wasapi-exclusive|asio`).
  * `src/midi/MidiOutPortable.cpp` (ALSA sequencer port; Windows stub).
* Plugins:
  * `src/plugins/PluginHostNull.cpp` (state-preserving placeholders, same ids and plugins.json as the Mac).
  * `src/plugins/PluginEditorNull.cpp`.
* Shell: `src/gui/shell/` (bridge, dialogs, reveal/openURL, single instance, screenshot mode).
* Tests:
  * `tests/test_vdsp_portable.cpp`, `tests/test_plugins_null.cpp`, `tests/test_compat.hpp`.
  * Linux `rtguard` (LD_PRELOAD).
  * Backing-track fixtures (synthetic CC0 tones).
* Packaging and CI: `packaging/` (Install.cmake, content manifest, `check-bundle.sh`, macOS/Windows/Linux/Flatpak scripts, notices) and `.github/workflows/{build,release}.yml` (local only).
* Vendored libraries (all permissive):
  * `third_party/miniaudio` 0.11.26 (public domain / MIT-0);
  * `third_party/webview` 0.12.0 (MIT);
  * `third_party/signalsmith-stretch` 1.4.0 + `signalsmith-linear` 0.6.4 (MIT).
  * No GPL code is vendored.
* Engine fixes found by the port:
  * NAM demo captures were seeded from a build-machine path compiled into the binary, so installs never got them. They now come from `<appRoot>/app/third_party/NeuralAmpModelerCore/example_models`, and an empty folder gets seeded too.
  * `Library.cpp` saveUser/renameUser path joins (broke on Windows).
  * An aligned-delete mismatch in the tests (heap corruption on Windows).
  * Linux `stop()` hung, because `close()` doesn't wake `accept()`; fixed with `shutdownListener`.
  * x86 FTZ/DAZ guards (`ScopedNoDenormalsX86`) in the live render callback, `renderHost()` (VST3/AU `process()`), the worker, the HTTP and the library threads. They're no-ops on Apple Silicon, so Mac arm64 numerics are unchanged.

Known gaps:
* AI fetch proxy (replies with an error off macOS).
* Native file drag and drop in the new shells.
* AAC/M4A/ALAC decode off macOS.
* GM sounds silent off macOS.
* No plugin hosting off macOS.
* Windows MIDI out is a stub.
* At this November 8 checkpoint the ASIO backend was still pending. The Windows desktop beta subsequently made ASIO a packaging requirement (§2.1).
* The Windows app hasn't been launched.
* First-run amp calibration takes about 20 s at about 90 % CPU on x86 (it's cached afterwards). Show progress and consider a lower priority.

### 7.2 Content audit (what a clean install has, and what it lacks)

A fresh install needs no external content:

| Content | Where | Status |
|---|---|---|
| Cab / reverb / spring IRs | `CabinetIRLibrary::synthesizeImpulse`, plus the user's own `.wav` IRs | **impulses are synthesized**: Studio Cab Room cabs, EMT 140 Plate & Spring Reverb, Compact IR Reverb, amp spring (`TubeAmpComponents.cpp`) |
| Component bank | schema Init presets | schema Init presets (fine) |
| DI player | `repo_root/app/samples/di` + the user's DI Tracks folder | no DI tracks: the default "player" source is silent. Ship the CC BY sets (Guitar-TECHS, GuitarSet) with SOURCES.md, or original DIs (`Lead DI.wav`, `Crunch DI.wav`, `Metal DI.wav`, `Rock DI.wav`) once confirmed as original performances. **Never the licensed research DI dataset (not distributed).** |
| Startup rig, Showcase, Library | `app/presets/{Showcase,Library}` | ship as is: Headroom XML only, no external paths or samples |
| NAM demo captures | NeuralAmpModelerCore `example_models` (MIT) | ✅ fixed (see 7.1); the `.gitignore` allowlist now tracks the three `.nam` files |
| Neural guitar→MIDI model | `resources/models/guitar_midi.hgnn` | ships, but **training-data provenance must be confirmed**: non-redistributable (e.g. NC-ND) training data would be a release risk |
| Unit art | 146 SVGs in `ui/assets/units` | original art; `check-bundle.sh` enforces clean metadata |

* `app/packaging/content-manifest.txt` is the allowlist; `app/packaging/bundle-denylist.txt` is the denylist.
* `check-bundle.sh` fails a staged bundle containing anything on the denylist (`samples/di`, `work/`, disallowed file types, …) or anything off the allowlist. The universal macOS dry run passed it (2137 files), and so did the AppImage.
* The Mac bundle uses `Contents/app -> Resources/app` so the engine's `repoRoot` layout holds inside the `.app`. `RIG_RELEASE_BUNDLE=ON` strips the baked-in source path.
