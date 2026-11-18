<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="resources/brand/headroom-logo-dark.svg">
    <img src="resources/brand/headroom-logo.svg" alt="Headroom" width="340">
  </picture>
</h1>

Headroom is an open-source guitar amp and effects rig targeting Windows x64 and macOS for the standalone desktop beta. It includes physically modelled tube amps, Neural Amp Modeler captures, synthesized and user-imported cabinet and room impulse responses, and a polyphonic guitar → MIDI → instrument path you can blend with the rig.

Both desktop apps share the circuit engine and factory rigs. The Windows standalone supports ASIO® and WASAPI audio and uses WebView2 for the interface. macOS uses Core Audio and WKWebView. External plugin hosting and system MIDI features currently differ between platforms. [Platform details](docs/PORTING.md)

## What's inside

| Area | What it does |
| --- | --- |
| **Tube amps** (27 voicings) | Cascaded 12AX7 stages solved per sample from the Koren triode model, with the cathode bypass capacitor as a live state, so partial-bypass bass cut, bias shift under hard playing and recovery come from the circuit itself. Grid-current blocking is modelled, and the unbypassed cold-clipper stages use solved transfer curves with antiderivative anti-aliasing. Everything runs at 8× elliptic half-band oversampling (≈384 kHz). The tone stack is the Yeh/Smith analytic Fender, Marshall, modern, Vox, Hiwatt or bass circuit. The push-pull power amp has a real negative-feedback loop: presence and resonance act inside the loop, the supply sag is shared with the preamp, and output-transformer band limits, speaker-impedance interaction and speaker power compression (cone excursion and voice-coil heating) are modelled. |
| **Neural Amp Modeler** | Plays any `.nam` capture (WaveNet / LSTM / A2) through [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) (MIT). Captures are loudness-normalised from their metadata, bridged across sample rates, and have a noise gate and a post EQ. Captures marked as amp heads get a cabinet automatically. |
| **Drive pedals** (19) | Ibanez TS808 Tube Screamer feedback clipper, Dallas Rangemaster germanium treble booster, ProCo RAT (LM308 gain-bandwidth limited), Dallas-Arbiter Fuzz Face bias-starve, Electro-Harmonix Big Muff Pi, Boss HM-2 Heavy Metal, Boss MT-2 Metal Zone and others, all oversampled. |
| **Cabinets & rooms** | Zero-latency partitioned convolution (Accelerate). Includes matched cabinets, an 8-mic Studio Cab Room mixer (cabinet, mic, position, pan, phase, room), 34 cabinet captures in the Cabinet IR Loader, custom `.wav` IR import/export, and 358 convolution reverb spaces plus EMT 140 plate and spring IR reverbs. All stock cabinet and reverb impulse responses are synthesized natively at runtime with zero external dependencies. |
| **Guitar → MIDI** | *Pitch → MIDI* unit. In poly mode the default **Neural** engine is a causal network that streams in 5.3 ms frames with no look-ahead. It runs dual causal STFTs on a constant-Q grid with harmonic stacking, a causal input AGC, convolutions with interval mixing and a per-pitch GRU, and onset/sustain heads for MIDI 36–88. The decoder also fires on sustained pitches without a pick attack, which catches legato and notes struck inside dense chords. **Accuracy is measured against guaranteed-correct notes.** The benchmark builds polyphonic electric DI by summing real isolated-note recordings, which is physically accurate because a magnetic pickup's output is the sum of its strings. It uses chord voicings, strums, arpeggios and lines on a guitar held out of training, so every note's pitch, onset and offset is known by construction. Training uses the same construction on other guitars, plus GuitarSet's per-string hexaphonic recordings, real annotated data with timing corrected for annotation lag, and distillation from a non-causal teacher. *Classic* is the earlier DSP detector. Mono mode is YIN with bend tracking. |
| **Instruments** | *Instrument* unit: FM keys and bells, a drawbar organ, a virtual-analog synth, the 128 macOS General MIDI instruments (piano, horns, strings, choir, …), and any installed AU instrument. *Poly Synth* exposes the full analog synth. *MIDI FX* adds octave, transpose, harmony, scale, arpeggiator and hold. A virtual CoreMIDI port, **Headroom MIDI Out**, lets a DAW play along. |
| **Plugins** | Your installed Audio Units (effects, music effects and instruments, AUv2 and AUv3) and VST3 plugins become rack units. Import them once from *Components → Plugins → Import plugins*; each gets a card you drag into the chain or a split lane. Knobs come from the plugin's own parameter list (names, ranges, units, value lists), are macro-assignable and bypassable, and *Editor* opens the plugin's own window. Plugin state is saved inside your rigs, and a rig whose plugin isn't installed still opens, passing audio through. |
| **Routing** | Parallel splits put lanes A and B side by side with a mix balance. A typical rig is guitar in lane A, Pitch → MIDI → Instrument → reverb in lane B, blended at the mix. |
| **Showcase** | 31 bundled `.rig` presets. *MIDI* rigs have the guitar playing instruments alone: concert grand, tine and Wurly keys, a jazz organ through a rotor, a tenor sax, a flute, and arpeggiated synths. Some are multi-instrument: piano & horns, strings & choir, a full orchestra, gospel organ & choir. *Guitar + MIDI* rigs blend an amp lane with instruments, e.g. crunch + analog pad, a metal rhythm with sub bass or with strings and choir, a JCM800 lead with a brass harmony, and a NAM capture with a choir. The rigs are generated in code (`rig_cli make-showcase`) and each lane is balanced by rendering real DI performances. |
| **Library** | Headroom's own rig library in `presets/Library/<Category>/`: genre and style rigs, plus *Artist Tones* researched from interviews and rig rundowns, each with a description, what inspired it and its sources (the ⓘ on the rig header). The rigs are built from JSON specs and loudness-checked by rendering (`tools/library`). Everything is indexed and classified by analysing each signal chain. *MIDI* and *Guitar + MIDI* get their own sections. You can save and load your own `.rig` presets, use per-unit presets, and assign 8 macros (preset-defined, automatic, or your own, including a split's mix). |
| **Standalone audio** | Choose input/output devices, input channel (mono or stereo), sample rate and buffer size. There's a latency readout, input gain with a clip LED, and a monitor switch. Settings persist. A built-in DI player lets you test without a guitar. |

## Build

Clone with the submodules (Neural Amp Modeler Core, the VST3 SDK and Apple's AudioUnitSDK):

```sh
git clone --recursive https://github.com/braedonsaunders/headroom.git
cd headroom/app
```

In an existing clone, run `git submodule update --init --recursive`.

### macOS

Requires macOS 13+, Xcode command-line tools, CMake ≥ 3.22 and Ninja.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/Headroom.app
```

Presets switch instantly and play at exactly the level their knobs give: no hidden loudness matching, nothing re-levels a rig behind your back. The shipped library is levelled once, offline, into each rig's own level knobs (`rig_level`; docs/ENGINE_API.md, "Levels"). Amp-model output trims are measured once per model and cached in `~/Library/Application Support/Headroom`.

When an *Apple Development* signing identity is installed, the build signs the app with it, so macOS keeps its microphone and folder-access grants across rebuilds. Otherwise the build signs ad hoc. To choose a specific identity, set `-DHEADROOM_SIGN_IDENTITY=…`.

Targets:
- `Headroom.app` — the standalone app.
- `rig_cli` — renders, inspects and analyses rigs offline; see `rig_cli` with no arguments. `rig_cli make-showcase <root>` regenerates and re-balances `presets/Showcase`.
- `rig_tests` — regression tests (`ctest --test-dir build`).

### Windows x64

Use an x64 Visual Studio developer prompt with the Windows SDK, clang-cl, CMake, Ninja and the Microsoft.Web.WebView2 NuGet SDK. Extract the [Steinberg ASIO SDK](https://www.steinberg.net/asiosdk) and set `RIG_ASIO_SDK_DIR` to its `ASIOSDK` directory. From this `app` directory, set `RIG_WEBVIEW2_SDK` to the extracted NuGet package directory, then build and stage the standalone app.

```bat
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRIG_BUILD_APP=ON -DRIG_WITH_ASIO=ON -DRIG_RELEASE_BUNDLE=ON -DRIG_DEV_SOURCE_UI=OFF -DHEADROOM_PGO=OFF
cmake --build build-win --target rig_studio
cmake --install build-win --prefix stage-win
stage-win\Headroom.exe
```

Keep `Headroom.exe` and the staged `app` folder together. The portable build needs the WebView2 Runtime and the Microsoft Visual C++ v14 x64 Redistributable. To play through an interface, open **Settings → Audio**, select **Live input**, and choose the input channel and output device. The DI player can also play WAV files you import. For native ASIO, open **Settings → Audio → Audio driver** and choose your interface manufacturer’s ASIO driver. ASIO is a registered trademark of Steinberg Media Technologies GmbH.

Both desktop shells load `ui/index.html`. The packaged app carries its own copy. [Engine API](docs/ENGINE_API.md)

## Content & User Data

Headroom is **100% self-contained** out of the box. Speaker cabinet impulse responses, microphone positions, room ambiences, and reverb spaces are synthesized natively by the DSP engine (`CabinetIRLibrary::synthesizeImpulse`), and user `.wav` impulse responses can also be imported or baked from `~/Library/Application Support/Headroom/User IRs`.

**NAM captures** live in `~/Library/Application Support/Headroom/NAM Captures`. Import them with *Import captures…* on the NAM unit, or drop files into that folder. Free captures are available from TONE3000 / ToneHunt. Three MIT-licensed demo captures from NeuralAmpModelerCore are copied there on first run.

Imported plugins are listed in `~/Library/Application Support/Headroom/plugins.json` (table ids 910000+, never reused).

User rigs are saved in `~/Library/Application Support/Headroom/User Presets/*.rig` (plain XML), and audio settings in `audio-settings.txt` next to them.

## Names and trademarks

Artist, song, amplifier, pedal and other product names in Headroom, its library and its unit descriptions are used only to describe a sound or the equipment a model was studied from. They belong to their owners. Headroom is not affiliated with, sponsored or endorsed by any of them. Library rigs "inspired by" a recording are original settings built from public sources, not captures of the original gear.

## Third-party code

| Component | License |
| --- | --- |
| NeuralAmpModelerCore | MIT |
| Eigen | MPL-2.0 |
| nlohmann/json | MIT |
