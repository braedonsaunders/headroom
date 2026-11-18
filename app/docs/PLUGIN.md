# Headroom as a DAW plugin (AU / VST3; AAX later)

## Build, install, test

```sh
cmake -S app/plugin -B app/build-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release
nice ninja -C app/build-plugin -j2 plugins test_plugin_core hr_host validator
ninja -C app/build-plugin install-user     # ~/Library/Audio/Plug-Ins/{Components,VST3} (no sudo)
auval -v aufx Hdrm Hdrm
app/build-plugin/test_plugin_core          # engine-level hosted-mode regression
app/build-plugin/hr_host                   # offline AudioToolbox host for the installed AU
app/build-plugin/bin/Release/validator ~/Library/Audio/Plug-Ins/VST3/Headroom.vst3
ninja -C app/build-plugin uninstall-user
```

`app/plugin` is its own CMake project. It pulls in the app's `rig_engine` with `add_subdirectory(.. EXCLUDE_FROM_ALL)`,
the VST3 SDK from `app/third_party/vst3sdk` (MIT since 3.8; see its `LICENSE.txt`), and Apple's AudioUnitSDK from
`app/third_party/AudioUnitSDK` (Apache-2.0). Neither SDK needs Xcode: only the Command Line Tools, CMake and Ninja.

| Bundle | Format | IDs |
|---|---|---|
| `Headroom.component` | AUv2 effect, mono→mono, mono→stereo, stereo→stereo | `aufx` `Hdrm` `Hdrm`, bundle id `com.headroom.plugin.au` |
| `Headroom.vst3` | VST3 single-component effect (mono or stereo in, stereo out) | class UID `4EC729B3-C4DC46BD-AE9D3AD7-EC768D30` |

AUv2 rather than AUv3: an AUv3 has to be an app extension inside a host app, which needs Xcode project
machinery and sandboxing. Logic, GarageBand, MainStage and Ableton all still load AUv2 components.

## Layout

```
app/plugin/core/HeadroomPluginCore.*   format-agnostic instance: engine lifetime, host params, render, state
app/plugin/editor/HeadroomEditor.*     NSView + WKWebView editor (shared by every format)
app/plugin/au/HeadroomAU.mm            AUEffectBase adapter + AUCocoaUIBase view factory
app/plugin/vst3/HeadroomVST3.mm        SingleComponentEffect adapter + IPlugView
app/plugin/tests/                      test_plugin_core (CTest), hr_host (offline AU host)
app/src/audio/RealtimeAudioServerHosted.inl   hosted-mode methods, included inside RealtimeAudioServer
```

An AAX wrapper is a third adapter over `PluginCore`, using the same calls as the AU and VST3 adapters:
`prepare`, `suspend`, `process(…, events)`, `saveState`/`loadState`, `createEditorView`. The VST3 SDK
also ships `public.sdk/source/vst/aaxwrapper`, which can wrap `Headroom.vst3` once the AAX SDK is present.

## One engine per instance; shared caches

Each plugin instance owns one `RealtimeAudioServer` in *hosted* mode (`startHosted`):

- **No device.** The host drives audio through `renderHost()`, which is the same `renderBlock` path the
  standalone `AudioIO` callback uses. There is no DI player and no mic request, and `/api/audio*`,
  DI-transport and track routes are refused.
- **No external MIDI out.** `setGlobalMidiSink` is a process-wide singleton, and hosted mode never installs it.
- **No nested AudioComponents in v1.** Rigs containing hosted third-party plugins, or the AU-backed MIDI
  Instrument (900003), are rejected recursively on every preset, state, template and insert path before
  any graph is built. The guitar→synth templates use the AU-free Poly Synth (900004) instead.
- **Lazy, shared background work.** The engine starts at the first `prepare` (AU Initialize / VST3
  setActive). The preset-library scan and the warm-up start only when an editor opens, so DAW plugin
  scans never trigger them.
  - The preset-library index and the component-preset bank are scanned once per process and copied
    into each instance as metadata.
  - IR, NAM-list and neural-MIDI caches were already process-wide statics.
  - A process-wide registry lets exactly one instance run the amp / tube warm-up. When that owner is
    destroyed, its worker is joined first, then ownership passes to another instance that has opened its
    editor.

## Editor ↔ engine bridge: per-instance ephemeral loopback port

- `startHosted` binds `127.0.0.1:0`; `boundHttpPort()` reads the port the OS assigned, with `getsockname`.
- The editor injects `window.RIG_PORT = <port>; window.RIG_HOSTED = true;` at document start. It loads
  `Resources/ui/index.html` from the plugin bundle, never from the source tree.
- `index.html` only probes `RIG_PORT` when `RIG_HOSTED` is set. A plugin can never attach to the
  standalone app on 17770 or to another instance.
- Client sockets get `SO_NOSIGPIPE`, so a closed browser socket can't raise SIGPIPE in the DAW.
  `stopHosted()` wakes the blocking `accept()` with a loopback self-connect, since `close()` alone doesn't
  wake it on macOS.

The alternative, bridging `fetch` over `WKScriptMessageHandler`, was not chosen. It would need the whole UI's
I/O layer rewritten (it polls JSON endpoints).

Like the standalone app, the loopback port is reachable by any local process.

## Host parameters, automation, transport

| id | name | range | notes |
|---|---|---|---|
| 8 | Input | −24…+24 dB | plugin input trim before the rig |
| 9 | Output | −36…+12 dB | plugin output trim after the rig |
| 10 | Bypass | 0/1 | 10 ms crossfade to dry. The AU `BypassEffect` property and the VST3 `kIsBypass` param both map here |

- All three host parameters are applied on the render thread at their sample offsets:
  - VST3: parameter-queue points.
  - AU: scheduled-parameter slices, plus `SetParameter` at block start.
- Smoothing is a fixed 64-sample ramp, so output doesn't depend on how the host partitions blocks. This is tested.
- **The 8 rack macros are not host parameters in v1.** A macro move publishes a new render plan through
  the engine's edit path, which can't run on the callback. They remain editor and state controls, and their
  values are saved in the plugin state. Ids 0–7 are reserved for them.
- The host tempo is forwarded every block to `rig::hostTempo()` (`core/Tempo.hpp`, lock-free). That makes
  the host's BPM drive every synced unit. Beat position, seek and loop are not used for phase sync yet.

## State

The AU `ClassInfo` key `headroom-state` and the VST3 component state hold the same text blob:

- a `HEADROOM-STATE 1` header with key=value lines:
  - the trims and bypass;
  - rig name, author and source;
  - the rack input gain (there is no loudness-match gain: a rig plays at its own knobs' level; a
    `preset_gain` line from an older build is folded into the restored rig's level knobs once, see
    ENGINE_API.md "Levels");
  - the 8 macro values, captured under the same lock as the rig;
- then `rig_bytes=N` followed by the `.rig` XML, the same format as user presets.

Restores happen off the callback and publish like a preset load. A restore is all-or-nothing, and an
unsupported rig is refused. State that arrives before the engine exists is applied when it starts.

## Real-time contract

- `process()` never allocates, locks or blocks; the CTest's allocation guard checks this while another
  thread publishes edits.
- `prepare` (AU Initialize, VST3 setActive) runs only while the callback is quiescent. It does a full
  re-prepare with a fresh lineage, zeroes the server's output-stage DC and transition history, and never
  re-levels the rig.
- AU `Reset`, which can arrive mid-render, rebuilds the rig from its own state on the worker thread and
  swaps it in.
- Exceptions from the engine are caught in the callback. The instance then outputs dry audio until the
  next prepare.

## Content root

The plugin needs the source checkout (the folder that contains `app/presets`) for its rigs and UI. It resolves the checkout in this order:

1. `$HEADROOM_RESEARCH_ROOT`;
2. `research_root=` in `~/Library/Application Support/Headroom/plugin-settings.txt`, which is written on
   first run;
3. the checkout the plugin was built from.

If no content is found, the plugin passes audio through
dry and the editor explains how to fix the path.

## Pro Tools (AAX): not built. Requirements

1. An Avid developer account, plus acceptance of the AAX SDK license (free, at developer.avid.com). The
   AAX SDK isn't redistributable, so it can't be vendored here.
2. The AAX SDK. Build its `AAXLibrary`, then either:
   - write a native AAX adapter over `PluginCore` (`AAX_CEffectParameters` + `AAX_CEffectGUI` hosting the
     same NSView), or
   - build the VST3 SDK's `aaxwrapper` (configure with `-DSMTG_AAX_SDK_PATH=…`).
3. PACE Eden signing. Pro Tools release builds only load AAX plugins signed with PACE's wraptool, which
   needs:
   - a PACE / iLok developer account;
   - the PACE Eden tools (by agreement with PACE);
   - an iLok and a signing certificate.
   Unsigned builds load only in the *Pro Tools Developer* build, which Avid provides to registered developers.
4. Avid's AAX plugin validation / testing (e.g. DigiShell / AAX Validator), and a 64-bit Pro Tools for
   end-to-end testing.
