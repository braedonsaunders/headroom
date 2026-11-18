# Engine HTTP API (127.0.0.1:17770, GET only — except a few `POST` routes such as `/api/rack/build` and `/api/backing/import` — JSON responses)

Existing endpoints keep working: `/api/rack`, `/api/telemetry`, `/api/transport?action=play|pause|toggle|track&idx=|seek&pos=|loop&val=|gain&val=|bypass&val=|preset&idx=`, `/api/param?slot=&id=&norm=` / `&bypass=0|1`, `/api/macro?col=&norm=`, `/api/rack/add?table_id=`, `/api/rack/remove?slot=`.

Knob routes are sent per pointer move while dragging, so they answer small: `/api/param` → `{"ok":true}`, `/api/macro?col=&norm=` → `{"ok":true,"col":C,"val":V}` (add `&full=1` for the rack JSON; the UI paints macro targets itself and refetches `/api/rack` once the drag ends), and a gain-only `/api/audio/set` (any of `input_gain_db`, `rig_input_db`, `output_gain_db`) → `{"ok":true,"lite":true,"gains":{...}}`, saved to disk 500 ms after the last move. Continuous moves (knobs, faders, macros, IN / OUT) reach the audio thread as prepared edits that inherit the DSP history and glide; switches, selectors and structural edits get a 5 ms output bridge. The UI keeps one request in flight per control (latest value wins) at most every 16 ms. Nothing in the engine changes the level by itself: there is no background loudness match (see "Levels" under the preset routes), so `/api/rack` has no `loudness_pending` / `loudness_rev` any more.

Status legend: **[live]** available in the running build, **[live]** being implemented now (contract fixed).

## Library & User Rigs (presets) — [live]

`GET /api/library` →
```json
{
  "presets": [
    {"idx": 1, "name": "Comfortably Numb Solo", "author": "Headroom",
     "category": "Artist Tones", "amp": "Hiwatt DR103 Custom 100", "tags": ["Artist Tones", "Lead", "Fuzz"],
     "source": "library" | "showcase" | "user", "featured": false, "units": 11, "comment": "Singing, sustaining fuzz lead ...",
     "lib_id": "numb-solo", "order": 10, "style": "Classic Rock", "instrument": "guitar",
     "inspired_by": {"artist": "David Gilmour (Pink Floyd)", "song": "Comfortably Numb", "album": "The Wall", "part": "Second (outro) solo", "year": "1979"},
     "sources": [{"url": "https://...", "title": "..."}],
     "based_on": "", "based_on_path": "", "mtime": 1730985600, "path": "/.../app/presets/Library/Artist Tones/Comfortably Numb Solo.rig"}
  ],
  "categories": ["Featured", "Clean", "Crunch", "High Gain", "Bass", "Ambient & FX", "Acoustic & Studio", "Artist Tones", "Guitar + MIDI", "MIDI"],
  "count": 1600
}
```
- The curated rigs are `featured:true`. Headroom's shipped library (`source:"library"`, `app/presets/Library/<Category>/*.rig`, override with `HEADROOM_LIBRARY_DIR`) comes right after them, ordered by category, then `order`, then name. `lib_id`, `order`, `style`, `instrument`, `inspired_by` (only on rigs inspired by a recording) and `sources` come from the rig's `<headroom-library>` block, which `tools/library/build_library.py` writes. Next are the 31 showcase rigs (`source:"showcase"`, from `app/presets/Showcase`, override with `HEADROOM_SHOWCASE_DIR`), then user presets (`source:"user"`).
- `categories` lists the Library category chips in browse order (Featured first, the MIDI categories last). `"User"` is omitted from `categories` because user rigs have their own dedicated **Rigs** pane.
- Each entry in `presets` includes:
  - `based_on`: the source Library preset name this rig was customized from (e.g. `"JCM800 British Crunch"`), or `""` when built from scratch (`New rig`).
  - `based_on_path`: the source Library preset path or identifier, or `""` when built from scratch.
  - `mtime`: file modification timestamp in Unix epoch seconds (`int64`), for sorting user rigs by **Recent**.
  - `path`: full filesystem path to the `.rig` file (used by `revealRig` / `/api/preset/reveal`).
- Any rig with an Instrument or Poly Synth unit is categorised `"MIDI"` (instruments only) or `"Guitar + MIDI"` (an amp or NAM capture is also in the chain). Its tags list `"Multi-instrument"` when more than one instrument plays, the instrument families (`"Piano"`, `"Keys"`, `"Organ"`, `"Brass"`, `"Strings"`, `"Choir"`, `"Synth"`, `"Synth Lead"`, `"Pad"`, `"Bass"`, `"Mallets"`, `"Sax & Reed"`, `"Woodwind"`, `"World"`, `"Percussion"`, …), `"Poly"`/`"Mono"` tracking, and `"Arpeggiator"`/`"Harmony"` when a MIDI FX unit uses them. A user rig keeps category `"User"`, with the inferred category as its first tag.
- **Levels.** A rig plays at exactly the level its units' settings give, like a preset on a hardware modeller. The engine never measures a rig's loudness, never applies a hidden gain and never glides the level: not on a load, not after an edit (turning a knob, adding a pedal or swapping an amp changes the level the way it would on real gear), not on a sample-rate change. The rack's master section is only the −1 dBFS peak limiter; the global OUT level stays the player's own.
  - The shipped library (`presets/Showcase`, `presets/Library`) is levelled **once, offline**, the way a preset designer masters a library: `rig_level` (tools/rig_level) renders each rig with the calibration DI (bass rigs: the bass DI) and the K-weighted target (`RackGraph::calibrateLoudness`, offline only) and folds the correction into the rig's own level knobs (`core/LevelFold.hpp`), written into the `.rig` as ordinary knob positions; the player sees and turns them like any other knob.
  - Which knobs: from the output backwards, the first units with a **pure post-gain** level knob — Amp-Matched Cab / Classic Amp-Matched Cab / Studio Cab Room / IR loader Volume, Cabinet Level, EQ Shelving / EQ Parametric / EQ Graphic / SSL 4000 E channel EQ Output, Lexicon 224XL / 480L Output, Shimmer Delay / Multi-Mode Delay Output, Tube Saturation Compressor Output, dbx 160VU Output, Instrument Level (at Mix 100 %), Poly Synth Level. Other reverbs, delays, modulation, loopers and MIDI units are walked past (they pass level through); anything else (an amp, a drive, a compressor, a gate) ends the search, because a knob in front of it would change the tone. Amp masters are never touched. Each split lane's knob gets the same change. When the nearest knob runs out of travel the rest goes to the next one back; what no knob can take is reported, never forced. For the shipped library only (`rig_level --add-level-eq 0.5`, run by `tools/library/build_library.py`), a rig that no knob can bring within 0.5 dB ends in one visible EQ Parametric left flat (both bands 0 dB: it passes the signal bit for bit), its Output carrying the whole correction. The migration of user rigs never adds a unit.
  - The SSL, Lexicon and Shimmer / Multi-Mode delay Output knobs are a plain gain on the unit's finished output (`dsp/OutputTrim.hpp`, one law shared with the folding): SSL ±18 dB, Lexicons ±12 dB (0.5 = 0 dB), the delays 0 dB at their catalogue default up to +12 dB at the top. Each is exactly 0 dB at its default, so a rig that never moved it sounds bit-for-bit as before.
  - **User rigs saved by older builds** (which levelled at run time) are migrated once so they don't jump: on a user rig's first load, the gain the retired loudness cache (`loudness-cache-exact.txt`) last gave it is folded the same way into its own level knobs and written into the file (the original is copied to `Level Migration Backups/` first). That gain was measured while the SSL / Lexicon / Shimmer / Multi-Mode delay Output knobs did nothing, so what those knobs now add at the rig's stored values is taken off it first (`levelfold::foldLegacyGain`): the rig plays at exactly the level it played before. Such a knob on the rig's linear tail (where it moves the whole rig) is compensated that way; one in front of an amp, a drive or a compressor (where it would now change the drive) or in one split lane only (where it would change the lanes' balance) is set back to its default, 0 dB, which is how it sounded. The cache is read for this only, copied once to `loudness-cache-exact.txt.pre-level-migration.bak`, and never written or deleted. `level-migration.txt` (data folder) records every rig handled (`migrated`, `no_level_knob`, `write_failed`), so none is migrated twice; a rig with no such knob keeps its raw level. `/api/diag` → `level.migrations` lists them. DAW projects saved by older plug-in builds carry that gain as `preset_gain`; it is folded into the restored rig the same way.
  - Amp models keep their fixed output trims (`amp-trims-<dsp>-exact-lvl.txt`, keyed by amp, rate, oversampling and `kLevelVersion` in `core/EngineFingerprint.hpp`): part of each amp model, measured once per model, never per rig.
  - The old rack crossfades into the new one over 30 ms.
- `category` is inferred from the actual chain (amp model + gain settings + effects), `amp` is the main amp's name ("" if none), `tags` are short descriptors (e.g. "Lead", "Rhythm", "Delay", "Reverb", "Modulation", "Fuzz", "Pedal-only", "Stereo").
- Load: `GET /api/preset/load?idx=N` → rack JSON (same as `/api/transport?action=preset&idx=N`, which also accepts any library idx). Loading a Library preset (`library` or `showcase`) sets the active lineage (`based_on` = preset name, `based_on_path` = preset path); loading a user rig restores the lineage stored in its `.rig` file.
  - The rig is built on the engine's preset-loader thread, not the HTTP thread: every other route (telemetry, meters, edits) keeps answering while it builds, and the old rig keeps playing until the new one swaps in. The reply still comes once the new rig is in (it is the new rack state).
  - The newest load wins. A load request still waiting when a newer one arrives is answered at once with the current rack JSON plus `"superseded": true` (ignore it); its build stops between units.
  - A rig that can't be loaded answers with the current rack JSON plus `"load_error": "<why>"` (bad index, unreadable file).
  - `&async=1` answers at once with `{"ok":true,"loading":true,"preset_idx":N}`; follow `rack_rev` (and telemetry `preset_loading`) for the result.
  - Telemetry `preset_loading`: the idx being built, `-1` when no load is in progress.
- Save current rig: `GET /api/preset/save?name=...&author=...` (optional `&based_on=...&based_on_path=...` override) → `{"ok":true,"idx":N}` (overwrites a user preset of the same name). Persists `based-on` and `based-on-path` in `<metadata .../>` and `<headroom-lineage .../>` inside the `.rig` XML file. Saved rigs appear in `/api/library` with `source:"user"`, category `"User"`, and their `based_on` lineage and `mtime`.
- Rename a user rig: `GET /api/preset/rename?idx=N&name=...` → `{"ok":true,"idx":newIdx,"name":"..."}` (or `{"ok":false,"error":"..."}`). Updates the `.rig` internal `<name>`/`preset-name`, writes the new file, removes the old file if the filename changed, and updates the active preset index/name if the renamed rig is currently loaded.
- Duplicate a user rig: `GET /api/preset/duplicate?idx=N` → `{"ok":true,"idx":newIdx,"name":"<Name> Copy"}` (or `"Copy 2"`, etc.). Preserves all components, macros, tempo, and `based_on` lineage.
- Delete a user preset: `GET /api/preset/delete?idx=N` → `{"ok":true}`.
- Reveal a preset path: `GET /api/preset/reveal?idx=N` → `{"ok":true,"path":"/..."}`. In the macOS app, `window.webkit.messageHandlers.headroom.postMessage({action:"revealRig", path:"...", name:"..."})` selects the `.rig` file in Finder (and `{action:"revealRigsFolder"}` opens `~/Library/Application Support/Headroom/User Presets`).
- New rig: `GET /api/rack/new?template=empty|amp|pedalboard|guitar_synth|instrument` → rack JSON. `amp` = amp + matched cab; `pedalboard` = drive + amp + cab + delay + reverb. `preset_name` becomes "New Rig", `preset_idx` = -1, `preset_source` = `"new"`, `based_on` = `""`, `based_on_path` = `""`, `dirty` true.
- Rack JSON gains `"dirty": true|false` (edited since load/save), `"preset_source": "library"|"showcase"|"user"|"new"`, `"based_on": "..."`, and `"based_on_path": "..."`.

## Components catalog — [live]

`GET /api/components` →
```json
{
  "categories": ["Amps", "Cabinets", "Drive & Fuzz", "Dynamics", "EQ & Filter", "Modulation", "Delay", "Reverb", "Pitch & Synth", "Creative", "Utility"],
  "components": [
    {"table_id": 134000, "name": "Block-Letter 5150 High-Gain Head", "ref": "Peavey 5150", "archetype": "amp_tube",
     "category": "Amps", "engine": "5-stage triode preamp · …",
     "params": [ {"id":"L1","name":"Lead","kind":"continuous","min":0,"max":1,"default":0.5,"norm":0.5,"fmt":"5.0"} ],
     "primary": 9,
     "factory_presets": ["INIT", "Hi Gain", "Crunch", "Clean"]}
  ]
}
```
- `archetype` uses the same values as rack slots (`amp_tube`, `amp_boutique`, `amp_neural`, `cabinet`, `pedal_drive`, `reverb`, `delay`, `modulation`, `compressor`, `eq`, `saturator_matrix`, …) so card thumbnails can reuse the rack renderers.
- `default`/`norm` come from the component's factory INIT preset when one exists (true defaults), else the schema default.
- Param objects (here and in rack slots) gain:
  - `kind`: `"continuous" | "stepped" | "bool"`
  - `labels`: optional array of option names for stepped params (cabinet names, mic names, reverb modes, amp modes/channels…). `labels[i]` is the label for raw value `min + i`.
  - `default`: normalized default (for double-click reset)
  - `on`: bool params only — the switch as the unit's DSP reads it (`IComponent::switchState`; without an instance, `rig::switchStateFor`). Show this, not `norm >= 0.5`: a catalogue switch left at 0.5 ("untouched") is off on circuit units and most effects (`> 0.5`) but on for the behavioural amps / drive pedals and the cabinet rooms' mic switches (`>= 0.5`), and a circuit's two-way selector or inverted parameter range reads it its own way. Away from 0.5 every unit agrees with `norm > 0.5`. `fmt` ("On"/"Off") follows it.
- Rack slots now include **all** visible params (no 9-param cap), ranked most-important first, plus `"primary": N` = how many to show on the compact face (the rest belong in an "expanded/more" view).

## Rack editing — [live]

- Insert: `GET /api/rack/insert?table_id=T&pos=P` → rack JSON. `P` = position in the slot list where the new unit should appear (0 = first; omitted or ≥ count = end). Use this for drag-from-Components drops.
- Move: `GET /api/rack/move?from=A&to=B` → rack JSON (slot indices; `to` is the final index).
- Remove: `GET /api/rack/remove?slot=N` (existing).
- Apply a factory component preset to a unit: `GET /api/rack/component-preset?slot=N&name=Hi%20Gain` → rack JSON.
- Build a whole rig from a unit list: `POST /api/rack/build` with body `{"name":"Brown Sound 80s","tempo":120,"units":[{"table_id":15000,"norms":{"Dr":0.3}},{"table_id":92000,"norms":{"Gain":0.7},"bypassed":false},{"table_id":900100}]}` (or `GET /api/rack/build?spec=<url-encoded JSON>`) → rack JSON plus `"built_units": N`, or `{"ok":false,"error":"bad spec"}`. The rig is assembled off the live graph and swapped in once, like `/api/rack/new` (`preset_idx` -1, `preset_source` `"new"`, no lineage, dirty, `preset_name` = `name` or "New Rig"). Units start from their INIT values; `norms` are 0..1 knob positions mapped exactly like `/api/param` (stepped params snap; bools on at ≥ 0.5). An amp still gets its matched Cabinet unit automatically unless the next unit in the list is a cabinet, which then replaces it. MIDI units get a Pitch → MIDI detector in front when none is upstream. Unknown table ids are skipped (compare `built_units` with the list). Used by the Guitar Tech's `build_rig`.

## Macros — [live]

Rack JSON `macros` entries become:
```json
{"col": 0, "slot": 1, "name": "Drive", "source": "preset" | "auto" | "user" | "empty",
 "target": "Ibanez TS808 Tube Screamer", "val": 0.04,
 "targets": [ {"slot": 1, "param": "Dr", "param_name": "Drive", "unit": "Ibanez TS808 Tube Screamer", "min": 0, "max": 1} ]}
```
- `preset` = assigned by the preset author (from the `.rig` macro entries), `auto` = the engine picked the most useful controls because the preset had none (gain, master, drive, delay/reverb mix…), `user` = assigned in this app, `empty` = unassigned.
- Assign: `GET /api/macro/assign?col=C&slot=S&param=ID&min=0&max=1&label=Gain&add=0|1` (`add=1` appends another target; otherwise replaces). Clear: `GET /api/macro/clear?col=C`. Rename: `GET /api/macro/label?col=C&label=...`.
- Macros are saved with user presets.

## Audio I/O — [live]

`GET /api/audio` →
```json
{
  "inputs":  [{"id": 73, "name": "Scarlett 2i2", "channels": 2, "sample_rates": [44100, 48000, 96000]}],
  "outputs": [{"id": 81, "name": "MacBook Pro Speakers", "channels": 2, "sample_rates": [44100, 48000]}],
  "current": {"source": "player" | "live", "input_id": 73, "output_id": 81, "input_channel": 0,
               "input_mode": "mono" | "stereo", "sample_rate": 48000, "buffer_size": 128,
               "input_gain_db": 0.0, "monitor": true},
  "buffer_sizes": [32, 64, 128, 256, 512, 1024],
  "status": {"running": true, "input_ok": true, "latency_ms": 6.1, "error": "", "mic_permission": "granted" | "denied" | "undetermined"}
}
```
- Top-bar IN / OUT (persisted in the audio settings, both in `current`):
  - `rig_input_db`, −24…+12: the level into the rig for both the DI player and live input. It is the same gain as `/api/transport?action=gain`, which also updates it.
  - `output_gain_db`, −60…+12: the master output level, where −60 = off. It is smoothed in the render loop and applied before the meters and the output guard.
  - Not used in the plugin, which has its own host In/Out trims.
- Change anything: `GET /api/audio/set?source=live&input_id=73&input_channel=1&input_mode=mono&output_id=81&sample_rate=48000&buffer_size=128&input_gain_db=6&monitor=1` (any subset) → same JSON as `/api/audio`, except gain-only requests (`input_gain_db` / `rig_input_db` / `output_gain_db`), which return `{"ok":true,"lite":true,"gains":{"input_gain_db":…,"rig_input_db":…,"output_gain_db":…}}` and save the settings 500 ms after the last change.
- `source=player` feeds the built-in DI tracks (today's behaviour); `source=live` feeds the selected interface input. Telemetry `in_rms` reflects whichever is active; telemetry also gains `"source"` and `"input_clip": true|false`.
- `GET /api/tracks` → `{"tracks":[{"idx":0,"title":"Lead DI","duration":27.1}]}` (replaces the hard-coded HTML list).
- Settings persist across launches.

## Additions (live)

- `/api/telemetry` and `/api/rack` include `rack_rev` (integer, bumps on every preset load / structural edit / macro edit). Detect external changes only via `rack_rev`.
- Telemetry includes `source` ("player"|"live") and `input_clip` (bool).
- Engine-only selector params appear first in a slot's `params` (kind `stepped`, with `labels`):
  - Cabinet IR Loader (168000): `IR1`–`IR4` — "Empty" + 34 bundled cabinet IRs (Bogren Digital, Lancaster Audio, 3 Sigma Audio, Kohle Audio Kult, cabIR, OwnHammer), mixed with the unit's g/p/m/s/pi/ph channel params.
  - Zero-Latency Convolution Reverb (118000): `IR` ("Space") — 358 synthesized halls, rooms, plates, springs and special spaces.
- `/api/audio` `status.underruns` = output buffer underruns since start.

## Synth layer (guitar → MIDI → synth, blended with the rig) — [soon]

The raw input is split: one path feeds the normal rig, the other is resolved to MIDI (polyphonic or
mono-with-bends), passes through MIDI effects, plays the built-in synth (and the virtual CoreMIDI port
"Headroom MIDI Out"), and is blended back with the rig.

`GET /api/synth` →
```json
{
  "enabled": false,
  "route": "parallel" | "into_rig" | "synth_only",
  "blend": 0.5,            // 0 = all guitar chain, 1 = all synth (parallel/synth_only)
  "synth_level": 0.8,
  "detect": {"mode": "poly" | "mono", "sensitivity": 0.5, "bend_range": 2, "low_note": 40, "high_note": 88, "velocity_curve": 0.5},
  "midi_fx": {"octave": 0, "transpose": 0, "harmony": "off" | "octave" | "fifth" | "power" | "third" | "major" | "minor" | "sus4",
              "scale_root": 4, "scale": "chromatic" | "major" | "minor" | "dorian" | "mixolydian" | "pentatonic_minor" | "pentatonic_major" | "blues",
              "arp": "off" | "up" | "down" | "updown" | "random", "arp_rate": "1/16", "hold": false, "bpm": 120},
  "synth": {"preset": "Analog Pad", "params": [ {"id":"osc1_wave","name":"Osc 1","kind":"stepped","labels":["Saw","Square","Triangle","Sine"],"norm":0,"fmt":"Saw","default":0}, {"id":"cutoff","name":"Cutoff","kind":"continuous","norm":0.6,"fmt":"2.4 kHz","default":0.6} ]},
  "presets": ["Analog Pad", "Saw Lead", "Sub Bass", ...],
  "midi_out": true,
  "notes": [ {"note": 52, "vel": 96, "bend": 0.0} ]    // currently sounding (after MIDI fx)
}
```
- Change anything: `GET /api/synth/set?enabled=1&route=parallel&blend=0.4&synth_level=0.8&mode=poly&sensitivity=0.6&bend_range=12&octave=-1&transpose=0&harmony=fifth&scale=minor&scale_root=9&arp=up&arp_rate=1/16&hold=0&bpm=120&preset=Saw%20Lead&midi_out=1` (any subset) → same JSON.
- Synth parameter: `GET /api/synth/param?id=cutoff&norm=0.4` → same JSON.
- Telemetry gains `"synth": {"enabled": bool, "notes": [[note, vel], ...], "pitch": 82.4, "confidence": 0.9, "level": 0.12}` for live note/fretboard displays (pitch in Hz, 0 when none).
- Synth layer settings are saved in user presets.

## Splits, lanes & MIDI/instrument components — [live] (supersedes the global "Synth layer")

The user rejected a global synth switch: guitar→MIDI→instrument now lives **inside the signal chain** as
normal components, typically in lane B of a parallel split. `/api/synth*` (global layer) is deprecated —
don't show it in the UI.

- Rack JSON has `"layout"`: the signal-flow tree. Items are `{"type":"unit","slot":N}` or
  `{"type":"split","id":7,"kind":"parallel"|"crossover"|"ms","xfade":0.45,"lanes":[[items...],[items...]]}`
  (lane 0 = A, lane 1 = B). Render splits as two side-by-side lanes (~50% width each) between a split
  head and a mix/merge unit. Slots referenced in `layout` index into `slots` as before.
- Insert a split: `GET /api/rack/split?pos=P&xfade=0.5` (before slot P; omitted = end) → rack JSON.
- Remove a split: `GET /api/rack/split/remove?split=ID&keep=A|B|both` → rack JSON.
- Split mix (A↔B balance): `GET /api/rack/split/mix?split=ID&xfade=0..1` (0 = all A, 1 = all B) → `{"ok":true}`.
- Insert into a lane: `GET /api/rack/insert?table_id=T&split=ID&lane=A|B&index=K` (K = position within lane) → rack JSON.
- Move into a lane: `GET /api/rack/move?from=SLOT&split=ID&lane=A|B&index=K` → rack JSON.
- New template: `GET /api/rack/new?template=guitar_synth` → split: lane A = Ibanez TS808 Tube Screamer → Marshall JCM800 2203 → cab,
  lane B = Pitch → MIDI → MIDI FX → Instrument → EMT 140 Plate & Spring Reverb. (`template=instrument` = lane B only.)
- New components (in `/api/components`, category "MIDI & Instruments"):
  - 900001 **Pitch → MIDI** (`archetype:"midi_detector"`): mode Poly/Mono, sensitivity, bend range, lowest/highest note (labels = note names), velocity curve, Guitar Thru. Its live notes = telemetry `synth.notes` style data is not per-unit yet; use the rack-level live notes (see below).
  - 900002 **MIDI FX** (`"midi_fx"`): octave, transpose, harmony, scale, key, arpeggiator, arp rate, hold, tempo (all with labels).
  - 900003 **Instrument** (`"instrument"`): `sound` selector (152+ labels: "Keys · Electric Piano" … "Organ · …" … "Synth · …" … "GM · Acoustic Grand Piano" … 128 General MIDI instruments (piano, horns, strings, choir, …) … "AU · <installed instrument>"), plus Tone, Attack, Release, Vibrato, Guitar Follow, Level, Mix. The sound list needs a great searchable browser grouped by the prefix before " · " (Keys / Organ / Synth / GM / AU).
  - 900004 **Poly Synth** (`"synth"`): the full analog synth — the 34 synth params (same ids as before), factory_presets = the 12 synth presets (apply via `/api/rack/component-preset`). Reuse the synth faceplate you built.
  - 900006 **Bloom** (`"arpeggiator"`): the arpeggiator the guitar plays (design: `docs/BLOOM.md`). Params (all stepped ones with labels): trigger Bloom/Always/Engage/Freeze, `engage` (bool: the footswitch), pattern Up/Down/Up-Down/Converge/Walk/Melody/Pedal/Random, rate, motion (expression macro), touch, range 1–4 oct, extend As Played/Triad/7th/9th/11th, key Auto/C…B, scale, gate, swing, clock Rig/Feel, bend follow, mute choke, bloom time (s), voice Synth/Strings/Both, string tone, dry, string level, retrig Chord/Pick (what restarts the keyed figure). Writes arpeggio notes (+ pitch bend for bends and the guitar's tuning) on the lane's MIDI bus and dry × duck + the Strings voice on its audio.
- Inserting MIDI FX / Instrument / Poly Synth / Bloom where no Pitch → MIDI feeds that lane auto-inserts one in front.
- Telemetry `midi.bloom` = `{"<slot>": {"active", "engaged", "idx", "chord", "root", "key" (0–11 major, 12–23 minor, -1), "key_conf", "note", "step", "bpm", "feel", "swing", "strength", "muted", "bend" (cents), "tune" (cents), "ring_db", "beat", "div", "bloom" (0..1 progress to a Bloom), "ladder": [MIDI notes]}}` for every Bloom unit.
- Hardware MIDI in (CoreMIDI, every source, hot-plugged): control changes go through the controller map (`/api/midi-learn`). Targets: `macro:N`, `scene:N`, `looper:*`, `tap_tempo`, `transpose:up|down`, and for every Bloom unit in the rig `bloom:engage` (≥ 64 on, below off: a momentary pedal holds, a latching one latches), `bloom:toggle` (each press flips it) and `bloom:motion` (continuous). Defaults: CC 85 → `bloom:engage`, CC 86 → `bloom:toggle`, CC 4 (foot controller) → `bloom:motion`.

## NAM captures, preset labels, per-unit notes, split macros — [live]

- **900005 "NAM Capture"** (`archetype:"nam_capture"`, category "Amps"): plays Neural Amp Modeler `.nam` captures (MIT NeuralAmpModelerCore).
  Params: `capture` (stepped, `labels` = capture names from metadata, `groups` = sub-folder, e.g. "Demo Captures" / "My Captures"), input (dB), output (dB), noise gate, bass, mid, treble, normalize (loudness-match from capture metadata).
  Captures live in `GET /api/nam` → `{"folder": "~/Library/Application Support/Headroom/NAM Captures"}`; `GET /api/nam?rescan=1` re-reads it.
  Native bridge in the app: `window.webkit.messageHandlers.headroom.postMessage({action:"importNam"})` opens a macOS file picker (files or folders), copies the .nam files in, rescans, then calls `window.headroomNamImported(count)` — refetch `/api/rack` + `/api/components` then. `{action:"revealNamFolder"}` opens the folder in Finder. (Feature-detect `window.webkit?.messageHandlers?.headroom`; in a plain browser hide the import button.)
  Inserting a NAM capture whose metadata says it's an amp head auto-adds a cabinet after it (full-rig captures don't get one).
- Extra (engine-only) params may carry `groups` (parallel to `labels`): use them for grouped browsers. Instrument `sound` groups: "Keys", "Organ", "Synth", "GM Piano", "GM Chromatic Percussion", … "GM Sound FX", "AU Instruments". Drop the positional GM grouping.
- Slots have `component_preset` (name of the last applied factory/synth preset, "" if none) and `component_preset_modified` (true once a knob moved since).
- Telemetry `midi.units` = `{"<slot>": {"notes":[[n,v],…], "pitch":Hz, "confidence":c}}` for every Pitch → MIDI unit (use per-unit; `midi.notes` stays = most recent).
- Macros can drive a split's A↔B mix: `GET /api/macro/assign?col=C&split=ID&min=0&max=1&label=Blend` (add=1 to append). Macro targets carry `"split": ID` (slot = -1) for these.
- The deprecated global `/api/synth*` endpoints are removed.

## Hosted plugins (Audio Units) — [live]

Installed Audio Units (`aufx` effects, `aumf` music effects, `aumu` instruments) are imported once and then behave like native units. Code: `src/plugins/PluginHost.*` (engine), `src/plugins/PluginEditor.mm` (editor windows), `ui/assets/plugins.js` (UI).

- `GET /api/plugins` → `{"store": path, "imported": [P…], "available": [P…], "vst3": {"supported": false, …}}` where
  `P = {"uid": "AU:<type>:<subtype>:<manufacturer>" (8-hex each), "format": "AU"|"VST3", "name", "vendor", "kind": "effect"|"music_effect"|"instrument", "version": "1.6.0", "v3": bool, "supported": bool, "installed": bool, "table_id": int|null, "params"?: n}`.
  `available` = everything installed (no instantiation, fast); `table_id` is set when it's imported. VST3 bundles from `/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/VST3` are listed with `supported:false` (not hostable yet).
- `GET /api/plugins/import?uid=U1,U2` → `{"ok": bool, "results": [{"uid", "table_id"} | {"uid", "table_id": null, "error"}]}`. Instantiates each plugin once (off the audio thread) to capture its parameter list and persists it in `~/Library/Application Support/Headroom/plugins.json`. Table ids start at **910000**, are stable and never reused (re-importing a removed plugin gives its old id back; re-importing an imported one refreshes its parameter list in place). Refetch `/api/components` afterwards.
- `GET /api/plugins/remove?table_id=T` → `{"ok": bool}`: removes it from Components (rigs already using it keep working; the id stays reserved).
- `GET /api/plugins/editor?slot=N` (or `?instance=I`) → `{"ok": bool, "instance": I, "error"?}`: opens the plugin's own UI in a native window (AUv2 `kAudioUnitProperty_CocoaUI`, AUv3 view controller, else CoreAudioKit's generic view). Inside the app prefer the bridge: `window.webkit.messageHandlers.headroom.postMessage({action: "openPluginEditor", instance: I})`.
- `/api/components`: imported plugins are in the new first category **"Plugins"**, `archetype` `"plugin"` (or `"plugin_instrument"` for `aumu`), `engine` like `"Audio Unit · Apple · v1.6.0"`, params in the plugin's own order. Insert them with the normal `/api/rack/insert?table_id=…` (chain or `split=…&lane=A|B`).
- Rack slots of plugins carry `"plugin": {"instance", "uid", "format", "name", "vendor", "kind", "missing", "ready", "editor", "editor_open", "imported", "in_channels", "out_channels", "latency_ms", "tail_s", "render_errors"}`.
- Plugin params: `id` = the AU parameter id (decimal string). `kind`: `"continuous"`, `"stepped"` (indexed AU params, with `labels` from the plugin's value strings) or `"bool"`. `unit` is the AU unit ("Hz", "dB", "%", "ms", "s", "st", "ct", custom…), `fmt` is formatted from the real value. Logarithmic params (frequency-like, or flagged by the plugin) are exposed in a 0..1 space (`min` 0, `max` 1, `unit` "", `"scale": "log"`) and `fmt` shows the real value. `/api/param`, `/api/macro/assign` and bypass work as for any unit; values read live from the plugin, so knobs follow its editor.
- Presets: each plugin unit saves `plugin_id` / `plugin_name` / `plugin_vendor` / `plugin_format` and `state` (base64 binary plist of `kAudioUnitProperty_ClassInfo`) as string parameters next to its values, so user `.rig` files are self-contained. On load plugins are matched by `plugin_id`, not table id (a rig from another Mac works). Plugins that were never imported but are installed get a session-only id (950000+); a plugin that isn't installed loads as a pass-through placeholder (`plugin.missing: true`) that writes its saved state back unchanged.
- Instrument plugins read the lane's MIDI bus (like the Instrument unit); inserting one where no Pitch → MIDI feeds the lane auto-inserts a detector.

## Component detail page: model export, curves, custom components — [live]

Code: `src/dsp/ComponentModel.{hpp,cpp}` (routes, generic descriptions, offline renders, user-component store), the `Introspectable` exports in `src/dsp/TubeAmpComponents.cpp` (amps, drive pedals) and `src/dsp/NamComponent.cpp`, UI in `ui/assets/component-detail.{js,css}`. Every model/curve request builds a **private instance** of the unit (never the live graph).

Common query: `tid` (table id), `norms` = JSON `{param_id: 0..1}` (knob positions, mapped exactly like `/api/param`), `params` = JSON `{param_id: raw}` (optional), `model` = override JSON (see below), `slot` (optional: start from that rack slot's params and overrides).

- `GET /api/component/model?…` → unit model `headroom.unit-model/1`:
  `{"schema", "model_id": tid, "table_id", "name", "ref", "impl", "engine", "kind": "amp"|"drive"|"nam"|"cab"|"reverb"|"delay"|"mod"|"dynamics"|"eq"|"waveshaper"|"midi"|"generic", "sample_rate", "oversampling", "editable", "verification": "none", "netlist": null, "fidelity_legend": {…}, "params": {id: raw}, "overrides": {…}, "custom_name"?, "blocks": [B…], "loops"?: [{"from", "to", "label"}], …kind-specific}`.
  Block `B = {"id" (stable: "in", "s<stock stage index>", "pot", "stack", "pi", "power", "ot", "spk", "opamp", "diodes", …), "type" (symbol: jack, triode, follower, transistor, opamp, diodes, pot, tonestack, pi, power, transformer, speaker, filter_hp/lp/bp, eq, tone, mix, gain, vca, convolver, mic, delay, fdn, matrix, lfo, detector, midi, synth, nam, resampler, spring, clip, plugin, out), "label", "sub", "section", "fidelity": "dsp-block"|"behavioural"|"opaque", "values": [V…]}`.
  Value `V = {"k", "v" (formatted), "raw"?, "note"?, "edit"?: {"path" (dotted override path), "op": "set"|"substitute", "min", "max", "step", "scale"?: "log"} | {"path", "op", "options": [{"v", "label"}]}}`.
  - amp extras: `controls` (the physical control values the knob mapping produced), `active_channel`, `channels: [{index, name, n, stages: [{index, orig, label, role: triode|cold_clipper|cathode_follower|solid_state, spec: StageSpec, curve, rk, ck, cath_corner_hz, vk0, ip0_ma}], removed: [{orig, label}], pot_after, stack_after, bright_db, bright_hz, input_hp, gate}]`, `tone_stack {topology, preset, values {C1..C3, R1..R4}, stack_in, tapers}`, `tone_stack_presets`, `power {spec: PowerSpec, nfb_beta, power_in, derived {drive, sag, headroom, asym, presence_db, depth_db}, speaker}`, `power_profiles` (power sections copied from factory voicings), `builtin`, `trims_db`, `oversampling`, `internal_rate`.
  - drive extras: `topology`, `topology_label`, `designed_topology`, `topologies`, `diode_options`, `values {gain, gain2, vd, bias, out, mix, clip_shape, pre_hp_hz, fb_hp_hz, fb_lp_hz, post_lp_hz}`, `controls`.
  - nam extras: `capture`, `loaded`, `architecture`, `weights`, `version`, `gear`, `gear_type`, `model_rate`, `resampled`, `loudness_db`, `config` (layer summary).
- `GET /api/component/curves?…&what=all|static,harmonics,response&level=0.25&freq=330` → any of:
  `static` (from the unit's own objects — amp: `tonestack {f, db, noon_db}`, `stages {u, curves: [{label, role, gain, y}]}`, `power {x, y, drive, headroom, asym, nfb_beta}`; drive: `unit {x, y}`, `pre {f, db}`, `tone {f, db}`),
  `harmonics {f0, level, h: [dB re H1 …], thd_pct, out_rms_db, wave_in, wave_out}` (amp/drive/nam: a sine through the whole unit),
  `response {f, db, peak_db}` (cab/eq: impulse response; amp/drive: small-signal response from a -60 dBFS impulse minus the response to silence; reverbs: wet spectrum),
  `decay {t, env_db, edc_db, rt60}` (reverbs, delays), `dynamics {in_db, out_db}` (measured with steady sines), `lfo {t, l_ms, r_ms, rate_hz}` (modulation, from the unit's formulas).
- `GET /api/component/apply?slot=N&model=JSON` → `{"ok", "slot", "custom_name"}`: sets the slot's overrides (string parameter `__model`). Candidates are prepared on the control thread and published at a buffer boundary. Rejected edits retain the last valid component, metadata and audio graph; the callback does not acquire the control mutex. Marks the rig dirty; `.rig` presets save `__model` like any string parameter, and the rack slot JSON gains `"custom_name"`.
- `GET /api/usercomponents` → `{"components": [UC…], "path"}`; `GET /api/usercomponents/save?tid=&name=&model=&norms=&id=` (omit `id` to create) → `{"ok", "component": UC}`; `GET /api/usercomponents/delete?id=` → `{"ok"}`. Stored in `~/Library/Application Support/Headroom/User Components.json` as `{"schema": "headroom.user-components/1", "version": 1, "components": [UC…]}` with
  `UC = {"schema": "headroom.user-component/1", "id", "name", "base_model_id": tid, "base": tid, "base_ref", "base_name", "overrides": {…}, "model": {…overrides, "name", "id", "base", "schema": "headroom.overrides/1"} (the exact `__model` string a slot receives), "norms": {param_id: 0..1}, "notes", "created", "modified"}`. Entries without `schema` are migrated on load.

Override JSON (`headroom.overrides/1`; unknown keys ignored, values clamped to safe ranges; stage keys are **stock** stage indices so edits stay attached when another stage is removed):
```json
{"schema": "headroom.overrides/1", "name": "My Plexi",
 "amp": {"channels": {"0": {"stages": {"1": {"gain": 9, "hp_hz": 60, "lp_hz": 11000, "bias": -0.05, "cath_hz": 300, "cath_db": -6,
                                             "blocking": 0.4, "lim_pos": 1, "lim_neg": 1.6, "type": "triode|follower", "off": true}},
                            "pot_after": 1, "stack_after": 3, "bright_db": 4, "bright_hz": 2200, "input_hp": 30}},
         "stack": {"preset": "fender|marshall|modern|hiwatt|vox|bass", "C1": 4.7e-10, "R4": 33000},
         "power": {"drive": 1.3, "sag": 0.25, "asym": 0.06, "knee": 1, "presence_hz": 3500, "presence_db": 7, "depth_hz": 95, "depth_db": 6,
                   "ot_hp_hz": 45, "ot_lp_hz": 10500, "class_a": false, "damping": 0.52, "spk_fs": 88},
         "stack_in": 1, "power_in": 1},
 "drive": {"kind": "screamer|treble_boost|clean_boost|hard_clip|rat|fuzz|muff|percolator|preamp|hm2|metal|saturator", "clip": "soft|hard",
           "vd": 0.5, "drive_db": 0, "gain2_db": 0, "level_db": 0, "pre_hp_hz": 90, "hump_hz": 720, "fb_lp_hz": 9000, "post_lp_hz": 4000, "bias": 0.08}}
```
`bias` selects which solved Koren operating point a stage runs (≥ -0.08 hot Rk 1.5 kΩ, ≥ -0.3 warm Rk 2.7 kΩ, else unbypassed 10 kΩ cold clipper). In the voicing backend, power profiles copy a factory amp's PowerSpec; see the electrical backend below for its separate physical device equations.


## Electrical reference backend and real-time publication

Engine DSP version: `dsp8-circuit1`. Existing presets retain the voicing backend unless an electrical override is explicitly supplied.

- Tube-amp extras: `input_gate` (Off/On, default Off), `level_matching` (Off/On, default On), and `input_voltage_scale` (0.25/0.5/1/2/4 volts per digital full scale, default 1). The voltage reference is **assumed**, not measured interface calibration. Gate and level compensation are utilities, not circuit elements.
- Tables **56000** and **34000** additionally expose `amp_core`: `0` = Voicing model, `1` = Electrical reference. This is a **hybrid**: upstream behavioral voicing stages feed a coupled triode/follower/passive tone network, then a reduced phase-inverter/push-pull/reactive-load circuit with coupled supply states. It does not certify an exact recreation of either named amplifier. Other amp models continue using their behavioral backend.
- Electrical model JSON supplies `backend: "hybrid-circuit"`, `verification: "circuit-reference"`, `verification_scope`, executable `netlist`, `circuit`, `capabilities`, and the actual `circuit_sample_rate`. Solved blocks use `fidelity: "circuit-solved"`; upstream blocks retain `"behavioural"`. Preserve these supplied fields. Verification describes software equation/reference checks, never hardware measurements.
- Electrical blocks' `values[].edit.path` starts with `circuit.values.`. Stable component IDs and SI units come from the same definition applied to the solver. `circuit.components` contains **effective** values; `circuit.editable_components` and block `raw` values contain **base** values before native Sag/Variac/Bias multipliers. Editing an effective value as though it were a base value would compound the multiplier. Notes show the effective value when relevant.
- Native tone/master/presence/depth knobs own their performance controls. Their circuit metadata carries `editable: false`; only `control.feedback` is separately editable. The selector is an extra parameter, so its preset persistence is the same as other extras.
- Arbitrary rewiring is not supported. `netlist` documents the executable fixed connectivity, and `capabilities` specifies the supported substitutions. A topology substitution is distinct from a scalar edit. Supported IDs are declared by `CircuitDefinition::topologyId`.

Example electrical override (nested dotted paths, as produced by the editor):

```json
{"schema":"headroom.overrides/1","circuit":{"values":{
  "preamp":{"slopeR":56000},
  "power":{"sourceR":200},
  "control":{"feedback":0.35}
}}}
```

Unknown/nonfinite/out-of-range physical values and unsupported topology IDs are rejected transactionally. The standalone `rig.circuit-amp.v1` interchange document is a separate complete definition; do not place that whole document inside an application `circuit` override.

`/api/component/curves` renders the selected actual backend for harmonic and impulse analysis. Electrical mode does not substitute legacy static transfer curves for the coupled stateful circuit. NAM uses the same unit-model/preset/ownership contracts, with `backend: "opaque-neural"`; its wrapper controls are editable, but its learned network is not presented as an editable electrical schematic.

The render callback consumes prepared snapshots through a bounded ownership mailbox. Unchanged units retain their instance identity; compatible edited native units adopt their runtime histories at the boundary. Retired graph/component destruction occurs off the callback. Hosted AU identity remains stable across scalar edits. Failed native candidate preparation or publication rolls back the control view. Macro edits validate native candidates before applying shared hosted-plugin setters, so a rejected native control cannot leave an earlier plugin target changed. Hosted plugin APIs retain their own state/parameter semantics. Audio no longer becomes silence merely because the control mutex is busy. A short output bridge handles topology changes without rendering shared state twice.

See `tools/circuit-ref/README.md` for reproducible numerical checks and their limits. `test_circuit_budget` measures the **integrated stereo** path at multiple buffer sizes; it is a manual performance gate, separate from deterministic CTests. Passing numerical checks does not establish that the electrical mode meets a particular machine's audio deadline.

`GET /api/telemetry` also exposes `render_budget`: cumulative `blocks`, `over_budget`, `elapsed_ns`, `budget_ns`, plus `mean_ratio`, `last_ratio` and `peak_ms`. Counters measure wall time inside the application's `renderBlock`; take before/after deltas for a bounded playback test. They include graph handoff and rack rendering, even when monitoring is muted. They exclude device/input callback work and cannot identify driver xruns or late callback delivery. Values are observational atomic samples, not a synchronized trace. No counter reset or allocation occurs on the audio callback.

## Cabinet unit (900100): any cabinet, real microphones and positions — [live]

Code: `src/dsp/CabinetCatalog.hpp` (what the content provides), `src/dsp/CabinetUnit.*` (the unit), `ui/assets/cabinet.js` (faceplate).

**Content** (synthesized natively by `CabinetIRLibrary::synthesizeImpulse` and extensible with user `.wav` IRs):

| Source | Cabinets | Microphones | Positions | Room |
|---|---|---|---|---|
| `Amp-Matched Cab` (156000) | 26 (one per amp) | 2 per cab: "Matched Mic A/B" | fixed | `_room_A/_room_B` |
| `Studio Cab Room` (119000) | 29 studio cabs | 178 recordings: Shure SM57, Sennheiser MD 421, Neumann U87, Royer R-121, Beyerdynamic M 160, Earthworks QTC30, Neumann U47 FET, Audix D6, Electro-Voice RE20, AKG D112, Neumann U67, Neumann M 149 Tube | Cap · on-axis, Cap edge, Cone · off-axis on the 5-mic and bass cabs; cap edge on the specialized multi-mic cabs; Hartke 4x10 QTC30 also "Room · far" | one `_room` per cab |
| `Cabinet IR Loader` | 34 cabinet captures | mic baked into the capture | fixed | — |

Every cabinet/mic/position impulse is synthesized natively with physically modelled speaker resonance, cone breakup, and microphone/position frequency response: cap is brighter than cone, and the Royer R-121 figure-8 ribbon exhibits the strongest proximity bass.

**Unit.** `table_id 900100`, ref `"Cabinet"`, name "Speaker Cabinet & Microphones", archetype `cabinet`, category Cabinets.
- Selectors (extra params, `labels` reflect the current cabinet): `cab` (all cabinets, label `"<group> · <name>"`, `groups` = Matched / Studio / vendor; a missing cabinet adds a last `"Missing · <id>"` entry), `micA`, `posA`, `micB` (`"Off"` first), `posB`.
- Knobs: `blend` (A↔B, equal power; readout "A 73 · B 27"), `alignB` (mic B time offset 0–1.5 ms ≈ 0–51 cm, comb filtering like a real second mic), `phaseB` (polarity), `room` (room mic level), `width` (A left / B right, room spread), `lowcut` (Off, 20–300 Hz), `highcut` (2–20 kHz, Off), `level` (0.5 = 0 dB, ±12 dB). Readouts come through `fmt`.
- Slot JSON adds `"cabinet": {"status": "ok"|"missing"|"no_content"|"approximation", "cab_id", "stored_cab_id", "kind": "matched"|"studio"|"capture", "label", "group", "speakers": "4x12"…, "pair", "room_available", "mic_a": {"id","label","family","note","position","position_label"}|null, "mic_b": …|null, "approximation": bool, "approximation_note"?}`. `pair` is the matched ↔ studio counterpart.
- Persistence: string params `cab_id`, `micA_id`, `posA_id`, `micB_id` (`""` = off), `posB_id` (stable ids such as `studio:Vintage4x12`, `rib121V1`, `capedge`), so a rig keeps its choice on any machine. On a machine without that cabinet the id is kept and reported as `missing`; with no impulse at all the unit runs a fixed speaker-voicing filter (HP 80 Hz, LP 5 kHz) and says so (`approximation`); it is never presented as a microphone.
- Auto-cab: an amp inserted with no cabinet after it now gets a Cabinet unit on that amp's matched cabinet (both matched mics at blend 0.5). It is sample-identical to the old Amp-Matched Cab (156000) default (test: max diff < 1e-5), so new rigs sound the same, but cabinet, mics and positions can be changed on it. Dropping a Cabinet after an amp also picks that amp's matched cabinet. Existing rigs keep their 156000/119000 units.
- `GET /api/cabinets` → `{"content_root", "available", "cabinets": [{"index","id","kind","label","group","speakers","room","pair","mics": [{"id","label","family","note","positions": [{"id","label"}]}]}], "counts": {"matched","studio","capture","studio_mic_recordings"}}`.
- `GET /api/cabinet?slot=N&cab=ID&mic_a=ID&pos_a=ID&mic_b=ID&pos_b=ID` → rack JSON (or `{"ok":false,"error"}` if the slot isn't a Cabinet). Empty `mic_*`/`pos_*` pick the cabinet's default (a 57-style dynamic at cap edge) and `mic_b=""` turns B off. The UI's Cab & mics panel (`ui/assets/stage-room.js`) is the one place that calls it: cabinet list, mic locker, drag-to-position (snapping to the positions each mic was recorded at), and for an amp-matched or captured cab one "switch to the studio cab with the same speakers" (`cab=<pair>`, mics mapped to the nearest studio models).
- `GET /api/cabinet/convert?slot=N` → rack JSON: replaces a legacy cabinet unit (156000, 88000, 119000, 168000) in place with the equivalent Cabinet unit. Amp-Matched Cab (156000) converts sample-exactly (test: 9e-8); Studio Cab Room (119000) carries over its first two active channels. Macros that targeted the old unit are dropped. Legacy cabinet faces show a "Cab & mics" button for this (it converts, moves onto the studio recording of the same speakers and opens the panel in one click); the unit's context menu has the same.
- Index edits (`/api/param?slot=&id=cab|micA|posA|micB|posB&norm=`) work like any stepped param; changing the cabinet keeps the closest mic (same model, then family).

**Hand-off.** Every selection change is prepared off the audio thread (IR load, mix, partition and FFT; finished IRs are shared through a 24-entry LRU, `dsp::CabinetIRCache`). On the audio thread the new unit adopts the old convolver's input history, which doesn't depend on the IR, and re-aims the pending tail at the new IR (`PartitionedConvolver::retarget`, no allocation). A mic or position change therefore continues the cabinet and room tail exactly as if the new IR had always been there (test: max diff 0). Studio Cab Room (119000) and the other cabinet units use the same hand-off and cache now.

**Studio Cab Room (119000) channel mapping.** `Mic` and `MPos` index the cabinet's own lists (`Mic` reaches 12 on the 13-mic Vintage 4x12, and `MPos` 0 on single-position cabinets means cap edge). Those lists use standard ribbon, condenser, and dynamic ordering, while the five-mic cabinets use `(SM57, MD 421, U87, R-121, M 160)` via `CabinetIRLibrary::roomCabMic`.

## Rig tempo — [live]

One tempo per rig (BPM, 20–300, default 120), saved with the rig and drawn in the top bar (`BPM − 120.0 + TAP`, type, scroll or arrow keys; Shift = 0.1). Every synced unit follows it:

| Unit | Sync switch | Synced parameter → value |
|---|---|---|
| Stereo Dual Delay 98000 | `s` Sync, `p` Sync Pre | `tl`/`tr` → 1/64 … 2/1 (straight); `pl`/`pr` → synced predelay |
| Performance Tempo Delay 107000 | synced unless `F4` Free | `R0` → 1/64 … 2/1 |
| Multi-Mode Delay 158000 | `timemode` 1 / 2 / 3 = note / dotted / triplet | `time` → 1/64 … 2/1 (× 1.5 / × 2/3) |
| Shimmer Delay 157000 | engine-side `sync` selector (Free / Tempo) | `time` |
| Stereo Tremolo 13000 `Ts`, Electro-Harmonix Small Stone 20000 `Sy`, Stereo Flanger 28000 `Sy`, Vintage Stereo Chorus Ensemble 29000 `TS` | as named | `Rt` (`VR` on Chorus Ensemble) → LFO period 4 bars … 1/32 |
| Rhythmic Noise Gate 114000 | synced unless `F7` Free | `R0` → step 1/2 … 1/32. Real trance gate: Gate = duty, Shape = edge, Stutter, Noise, Mix |
| Creative FDN Reverb 148000 | `preDSync` | predelay = `preDNumerator` / {64, 32, 16, 8, 4}[`preDDenominator`] × `preDMode` (straight, dotted, triplet) |
| Hosted AU plugins | — | receive the rig tempo through their host callbacks |
| MIDI FX arp 900002 | not yet wired: `src/midi/*` belongs to the MIDI owner; the unit still reads its own `bpm` param (see note) |

A synced parameter's `fmt` shows the note value ("1/8", "1/4 D", "1/16"). Schema defaults of 0.5 on these switches count as off; factory presets store 0/1. Synced delay times glide (~60 ms) when the tempo changes. With a playing host transport, synced LFOs and the gate lock their phase to the song position.

- `GET /api/tempo` → `{"bpm", "rig_bpm", "source": "rig"|"host", "min": 20, "max": 300}`. `?bpm=97.5` sets it, `?nudge=-1` steps it, and `GET /api/tempo/tap` registers a server-side tap (averages up to 6 taps, resets after 2 s; for footswitches and MIDI). The UI taps client-side and sends `?bpm=`.
- Rack JSON gains `"tempo": {"bpm", "rig_bpm", "source"}`; a tempo change bumps `rack_rev`.
- Persistence: `.rig` files carry `<headroom-tempo bpm="97.500"/>`.
- Engine: `RackGraph::setTempo()` / `tempo()` (model; carried by every published plan) and `RealtimeAudioServer::setTempo(bpm)`, the single entry point. `RackGraph::process()` writes the block's tempo into `AudioBlock::bpm` once per block, lock-free and allocation-free. Components only read `block.bpm`.
- Plugin builds: the host drives tempo. Either set `AudioBlock::bpm`, `ppq_position` and `transport_playing` and `host_tempo = true` on the block passed to `RackGraph::process()`, or call `rig::hostTempo().setTempo(hostBpm)` from the process callback (one relaxed atomic store; `rig::hostTempo().clear()` returns to the rig tempo). While the host drives, `source` is `"host"` and the top-bar control shows HOST and is read-only.
- Arp follow-up for the MIDI owner: in `src/midi/MidiComponents.cpp`, where the MIDI FX unit sets `m.bpm = getParamOr("bpm", 120)`, use `block.bpm` when a sync switch is on (for example `bpm` ≤ 0 meaning "follow rig"). The block already carries the rig or host tempo.


## Looper (global loop station, `LoopStation`) — [live]

Three stereo tracks, each a stack of layers: the first recording plus one layer per overdub pass. Code: `src/dsp/LoopStation.{hpp,cpp}`. Tests: `tests/test_loop_station.cpp` (CTest `loop_station`). UI: `ui/assets/looper-view.js`. The rack unit "Studio Looper" (900200) is a separate, single-loop engine (`PhraseLooperEngine` in `LooperUnit.*`).

- **Where it runs.** `pre_rig=1` records the DI before the rig and plays the loop back through it (re-amp: change the sound while it repeats). `pre_rig=0` (default) records the rig output. The metronome click is always added after the rig and is never recorded. Tempo is the host's when a host drives it, otherwise the rig tempo.
- **Footswitch logic** (`action=press&track=N`): Empty → Rec (or Armed, waiting for the bar line or count-in) → Play (or Dub when `after_rec=dub`) → Dub → Play … ; Stopped → Play. Pressing an armed track cancels it. Pressing during a pending stop keeps the track playing.
- **Sync.**
  - `free`: every track is independent.
  - `loop`: the first loop is the master. Another track's length snaps to the nearest of ¼, ½, 1, 2, 3 … master lengths. A shorter take keeps recording to that point, and a longer one is trimmed: what was played past the cut is cross-faded into the loop head. A stopped track restarts on its own grid line, so its offset against the master survives.
  - `tempo`: bars of the rig tempo, latched by the first loop (`tempo_locked`, and `rig_bpm` shows later rig changes). Recording starts on the next bar line, or after a one-bar count-in (`count_in=1`) when nothing is running. Length is `bars=N` (it closes by itself) or `bars=0` (Auto: the nearest whole bar when you press). The integer bar length means the grid never drifts.
  - **All Start/Stop** (`action=all`) stops everything that runs. With nothing running, it starts every recorded track: the longest from its top, the others phase-aligned from their stored origins.
- **Stop modes** (`stop_mode`): `instant` (6 ms de-click), `loop` (at the end of the loop) or `fade` (`fade_s` 0.2–30 s). A second stop while one is pending stops at once.
- **Layers.**
  - `feedback` (0–1): each overdub pass ages the layers under it by this factor, at least once and once per full cycle while it continues. The current pass decays per sample as the head comes back round (Ditto/Blooper "decay").
  - `undo` / `redo` step through passes, up to 12 layers per track. A pool of 16 layer buffers (120 s each) is shared by all tracks. When it runs out, a pass merges into the top layer (`full: true`).
  - Undo during a recording cancels it. A new pass drops the redo stack.
  - Punch-in and punch-out ramp over 6 ms.
- **Per track:** `level` 0–2 (with `track=`), `pan` −1…1 (balance), `mute` (keeps time), `reverse` (flips direction without a jump), `speed` 0.5 / 1 / 2 (octave down / up; overdubbing at ½× or 2× writes at that rate).
- **Real time.** No allocation, locks or system calls on the audio thread.
  - The pool is reserved with `calloc`, so untouched pages cost no RAM.
  - Freed layers are re-zeroed incrementally (128 KB a block).
  - Commands go through a lock-free MPSC queue (64 slots) and are applied at the next block. Settings are atomics.
  - Telemetry is published once per block.

`GET /api/looper` — settings and actions in one call; returns the status below (also embedded as `looper` in the rack JSON).
- Actions: `action=press|record|overdub|play|stop|clear|undo|redo|select` with `track=0..2` (default: the selected track).
  - `play&track=all` starts every stopped track. `stop` with no track stops all; add `mode=instant|loop|fade` to override the stop mode.
  - `clear` with no track (or `track=all`) clears everything. `all` = All Start/Stop. `undo` falls back to redo when there's nothing to undo. `select=N` selects a track.
  - `export&what=mix|stems|track[&track=N]` writes 24-bit WAVs to `~/Library/Application Support/Headroom/Loops`. Stems go in a folder: Mix + Track N. Export covers one full cycle (a common multiple of the loop lengths, ≤ 8× the longest), phase-aligned, of what's playing — or of every recorded track when all are stopped. It returns `{ok, path, folder, files[], error?}`.
  - Legacy aliases: `toggle` = `press`.
- Settings:
  - Looper-wide:
    - `pre_rig`, `monitor` (input heard through the looper), `count_in`, `click`, all `0|1`;
    - `sync=free|loop|tempo`, `bars=0|1..64`, `beats=1..16` (beats per bar), `click_level=0..1`;
    - `stop_mode=instant|loop|fade`, `fade_s`, `after_rec=play|dub`, `feedback=0..1`, `level=0..2` (loop master, no `track`).
  - Per track: `track=N` with `level`, `pan`, `mute`, `reverse`, `speed`.
  - Legacy: `quantize=1|0` (= `sync=tempo|free`), `half_speed=1|0` (= `speed=0.5|1`).
- Response:
  - `{version: 2, selected, sync, bars_setting, beats, count_in, click, click_level, stop_mode, fade_s, after_rec, monitor, pre_rig, feedback, level, bpm, rig_bpm, tempo_locked, clock: {running, beat_pos, bar, beat, count_in}, master, free_layers, max_layers, max_s, active, peak (input), tracks: [...]}`.
  - Each track: `{idx, state: empty|armed|recording|playing|overdubbing|stopped, length_s, pos (read position 0..1), rec_s, close_s, wait_s, armed_rec, layers, redo, full, can_undo, can_redo, bars, level, pan, mute, reverse, speed, stopping: none|loop|fade|instant, fade, peak, env_rev, gains[]}`.
  - Legacy single-loop fields (`state`, `state_id`, `reverse`, `half_speed`, `quantize`, `can_undo`, `can_redo`, `layers` = overdubs, `length_s`, `pos`, `speed`, `peaks[32]`) describe the selected track.
  - `queued: false` means the command queue is full (audio isn't running).

`GET /api/looper/wave?track=N&bins=B` (8–2048) → `{track, rev, frames, length_s, bins, layers: [{gain, redo, peaks[B]}]}`. These are per-layer peak envelopes (pre-gain, 10 ms cells kept by the audio thread) from the bottom layer to the top; `redo: true` marks undone layers. While recording, `frames` is the take so far. Refetch when the track's `env_rev` changes.

MIDI learn targets: `looper:toggle` (selected track's footswitch), `looper:track:1..3`, `looper:stop` (All Start/Stop), `looper:clear` (all), `looper:undo`, `looper:redo`.

## Pitch → MIDI engine (900001)

- `engine`: 0 = **Neural** (default when `resources/models/guitar_midi.hgnn` loads), 1 = **Classic**. Neural applies in Poly mode; Mono always uses the YIN tracker, for bends.
- `sens` (0..1) shifts the neural onset threshold around the model's tuned operating point: 0.5 = the benchmark-optimal threshold, and higher catches more notes.
- The model file is loaded once per process and shared by every unit. Detector state transfers allocation-free on knob-edit rebuilds (`adoptRuntimeStateFrom`).
- Offline check: `rig_cli p2m-transcribe <model.hgnn|-> <in.wav> <out.csv> [classic] [sens=0.6]` runs the real unit and writes `onset,offset,midi,velocity` CSV for scoring against reference notes.

## Guitar Tech (AI shop tech, `ui/assets/guitar-tech.js`) — native bridge

The Guitar Tech view calls LLM providers straight from the page (bring your own key) and acts on the rig through the routes above (`/api/rack/build`, `/api/preset/load`, `/api/param`, `/api/preset/save`, read-only `/api/components`, `/api/component/model`, `/api/library`, `/api/rack`). The macOS app adds:
- Keys: `postMessage({action: "getSecret" | "setSecret" | "deleteSecret", id, account, value?})` → `window.headroomSecretReply({id, ok, found?, value?, error?})`. Generic passwords in the login Keychain, service `com.headroom.studio.ai`, account = `openrouter` | `anthropic` | `compatible:<host[:port]>` (`[A-Za-z0-9._:-]`, ≤ 64 chars). Keychain calls run off the main thread. The page never writes keys to localStorage; outside the app it keeps them in memory for the session.
- Proxy for endpoints a `file://` page can't reach with `fetch()` (no CORS for origin `null`, e.g. `api.openai.com`, Ollama, LM Studio): `postMessage({action: "aiFetch", id, url, method, headers, body})` streams back `window.headroomAiFetch({id, type: "head", status, headers})`, `{id, type: "data", b64}` …, `{id, type: "end", error?}`; `{action: "aiFetchCancel", id}` aborts. http(s) only; ephemeral `NSURLSession`. The page tries `fetch()` first and falls back to the proxy automatically.

## Listen pane: tuner, key, chord, spectra, input health ([live])

`src/analysis/ListenAnalyzer.hpp`. The audio callback copies the **dry input** (post input gain, before the rig) and the rig output into a lock-free ring, but only while someone polls. An analysis thread starts on the first `/api/listen`, and goes idle 4 s after the last poll. Cost is about 2–4 % of one core while active; nothing runs on the audio thread except the ring stores.

- `GET /api/listen` returns the latest analysis. Until the first block arrives it returns `{"ok":true,"warming":true}`. The response fields:
  - `sr` and `t` (seconds analysed), `a4`, `source` (`"live"` or `"player"`), `latency_ms` (device round trip), and `cpu` (last render ratio).
  - `level`:
    - `peak_db`, `hold_db` (1.5 s hold), `rms_db`, `signal`, `silent_s`, `clip` (true for 2 s after a clip).
    - `floor_db`: the 8th percentile of 20 ms RMS. It is only reported once the history contains real gaps, so it is `null` during continuous playing.
    - `hum_hz` and `hum_db`: 50 or 60 Hz plus harmonics, measured while not playing.
  - `pitch`: the mono tuner, or `null`. It contains:
    - `hz`, `midi` (A440 basis), `raw`, `conf`, `stable`.
    - How it is measured: YIN on a ~16 kHz copy, refined at full rate on a multiple of the period after band-passing the fundamental. Accuracy on plucked-string tests is ≤ 0.07 cents from B0 to C7.
    - The lowest pitch is 5 semitones under the configured lowest string; in chromatic mode it is 26 Hz.
  - `trail`: `{step_ms, midi[150]}`, the last 3 s of pitch (`null` when unvoiced).
  - `bend`: a bend that landed in the last 2.5 s, or `null`. Fields: `to` (MIDI note), `from`, `cents` (landing error vs. equal temperament at A440), `age`.
  - `poly`: `{active, strings:[{target, present, hz, cents, level_db}]}`, for the configured strings.
    - `active` is true for 1.2 s after a strum lights all strings, or all but one.
    - How each string is measured:
      - a long FFT pair, with a phase-vocoder frequency for each partial;
      - partials that collide with another sounding string's harmonics are down-weighted;
      - a fit corrects for string stiffness (inharmonicity);
      - readings come only from windows that start after the latest attack.
    - Typical error is under 0.1 cent. The exception is octave-unison strings (drop D's D2/D3 share every partial), which can be off by ~1–3 cents.
  - Harmony fields:
    - `chroma[12]`, and `key_profile[12]` (the ~22 s decaying histogram).
    - `key`: `{root, minor, conf, alt_root, alt_minor, evidence_s, state: "listening"|"live"|"held"}`. `state` is `"listening"` until ~20 s of notes have been heard; the UI shows that as "settling".
      - The key comes from the shipped guitar→MIDI transcriber's notes, run in the analysis thread on the dry input. The transcriber sleeps through silence and costs ~6–9 % of one core while playing.
      - Two kinds of evidence are combined. Single-note lines use a duration-weighted Temperley profile; chordal playing uses the harmonic function of chords recognised where ≥ 3 notes sound together. They are blended by how chordal the playing is.
      - A smoothed-lead decision (about 5 s) keeps the key steady.
      - GuitarSet held-out players: 75.8 % exact (comping 83 %, solos 68 %), under one key change per take. The previous chroma version scored 64 %. Details are in `tests/key_study/README.md`.
      - If the model can't load, it falls back to the chroma version.
    - `chord`: `{root, quality, bass, conf, age}` or `null`. `quality` is one of `maj min 5 7 maj7 m7 sus2 sus4 dim aug m7b5 add9 6 m6 dim7 9 m9`. `bass` ≠ `root` means an inversion.
    - `chordal`: true while a chord is sounding.
    - `note`: a single sustained pitch class, or `null`.
    - `prog`: the last 8 chords, each `{root, quality, bass, dur}`.
  - `spec`: `{f0:40, f1:16000, in[72], out[72]}`, log bands in dBFS, smoothed.
  - `tone`: `{low_hz, mid_hz, top_hz, tilt_db}` from a slow average of the output, or `null`.
- `GET /api/listen/set` takes these parameters, all optional:
  - `a4=440`: the reference pitch, 400–480.
  - `strings=40,45,50,55,59,64`: MIDI string targets from low to high, fractional allowed. Empty means chromatic, which turns the polyphonic tuner off.
  - `reset_key=1`: clears the key histogram.

  It returns `{"ok":true}`.
- Tests: `tests/listen_bench.cpp` and `tests/listen_serve.cpp`.
  - `listen_bench` with no arguments runs the synthetic suite: mono, polyphonic (guitar, drop D, bass 4/5, 7-string), chords, keys and a bend.
  - `listen_bench file.wav` prints a key/chord/pitch timeline and the CPU cost.
  - `listen_serve file.wav PORT` streams a WAV through the real `ListenService` and serves `/api/listen` for UI work. It needs no audio engine and opens no window.

## Per-unit meters (live displays on the illustrated faces) — [live]

The hardware faces' meters, lamps and screens (see `work/illustrations/BRIEF.md` → "Live displays") read per-unit data from the regular telemetry poll. The UI appends `HRUnitArt.query()` to its `/api/telemetry` request, and asks only for the units on screen that have displays:

`GET /api/telemetry?units=0,3,7&spec=3&tune=5&wave=2` adds `"units": [...]`, one object per requested slot (`slot_idx` as in `/api/rack`):

```json
{"s":3, "ir":-21.4, "ip":-12.9, "or":-18.2, "op":-9.6, "gr":4.2, "lfo":0.375, "sp":[-60,-52,...], "hz":110.02, "wv":[0.01,...]}
{"s":7, "idle":1}
```
- `ir` / `ip` / `or` / `op`: input and output RMS and peak of that unit, in dBFS, with −120 as the floor. These are taken before the master section's preset gain and limiter.
  - RMS: a ~60 ms mean square.
  - Peak: instant rise, 300 ms fall, so short transients survive between polls.
  - The UI applies the meter ballistics (VU, peak hold).
- `gr`: present only for units that report gain reduction (`IComponent::liveReadout`), in dB ≥ 0. The behavioural dynamics units (`DynamicsComponent`: compressors, limiters, gates) report the block's maximum GR and the trance gate its own attenuation. Circuit units read it from the circuit, as declared by the netlist's `"readouts": {"gr": …}` (`CircuitUnit`, docs/CIRCUIT_ENGINE.md "Live readouts"): the LA-2A from its T4B meter-bridge photocell (`mtr_cell`, the VU's own node, zeroed at the no-signal operating point), the 1176 from the gate control voltage on C27 mapped through the FET cell's measured attenuation, and the SSL channel dynamics from the dbx 2151's control pins. Faces of units that don't report it estimate GR from `ir − or`, which reads nothing on a unit with make-up gain.
- `lfo`: LFO phase 0–1, for units that report one (`StereoModulationComponent`: chorus/flanger/phaser/vibrato/tremolo; `TranceGateComponent` step phase; behavioural amps' built-in tremolo/vibrato/chorus; circuit units from the netlist's `"readouts": {"lfo": …}` node). `-1` = the unit's oscillator is not running (a vibrato or tremolo switched off, or a circuit oscillator that is not swinging): the face's LFO lamp rests. Absent = not known (yet): the face runs from its rate knob. The UI extrapolates between polls.
- `lfo2`: a second rotor or oscillator phase, same convention as `lfo`. The Leslie 122 (31000) reports its horn rotor's angle as `lfo` and its bass drum's as `lfo2` (`LeslieRotors`: the drum turns the other way, so its angle falls), with the real spin-up and braking; the face's rotors follow them (`spin` with `source: "lfo"` / `"lfo2"`).
- `sp` (only for slots listed in `spec`): 28 log-spaced bands from 40 Hz to 16 kHz (centres at `40·400^((i+0.5)/28)` Hz). Each is the band energy of the unit's mono output in dBFS, from a 4096-point Hann FFT; a full-scale sine reads ≈0 dB in its band.
- `hz` (only for slots listed in `tune`): the pitch of the unit's output (McLeod/NSDF over the last 2048 samples, ≈47 Hz–1 kHz); 0 means no clear pitch.
- `wv` (only for slots listed in `wave`): 64 points of the output (~10 ms), triggered on a rising zero crossing.
- `idle: 1`: the unit hasn't run in the last 0.5 s, e.g. a bypassed unit, an unknown slot, or a lane of a disabled split.
- A bypassed unit reports −120 everywhere.

Cost and real-time safety:
- **Arming.** Metering is armed for ~1.5 s by each poll that has `units=`. With no UI polling, the audio thread does nothing beyond one counter compare per block.
- **Per unit, while armed.** One pass over the unit's input and output (sum of squares and max), plus a `liveReadout` virtual call. Results are published as relaxed single-writer atomics (`src/core/UnitMeters.hpp`, `UnitMeterBank`).
- **Spectrum, pitch and waveform.** At most 4 units at once get a preallocated 8192-sample ring that the audio thread copies their mono output into. The FFT, pitch and waveform work runs on the HTTP thread.
- **No locks or allocation on the audio path.** `tests/test_unit_meters.cpp` (CTest `unit_meters`) checks this with an allocation guard, together with levels, GR, LFO phase, spectrum, pitch, idle/bypass and disarming.

Wiring:
- `RackGraph::setMeterBank`: the server shares one bank across every render plan of the current rig, set in `swapInGraph`.
- `RackGraph::processNode` records each unit after `processAudio`, and records bypassed units as idle.
- `IComponent::liveReadout(UnitLiveReadout&)` is the hook for units to publish `gain_reduction_db` / `lfo_phase` / `lfo2_phase`.

## Backing tracks (Practice view) — [live]

Your own songs to play along with. The engine decodes them and mixes them into the output **after** the rig (after the global FX, the output guard and the meters/scope, so those still show the guitar alone): the guitar goes through the amp, the track doesn't. Code: `src/audio/BackingTrackPlayer.{hpp,cpp}` (decode + real-time player) and `src/audio/BackingTracks.{hpp,cpp}` (library, loader thread, routes). Tests: `tests/test_backing_track.cpp` (CTest `backing_track`). UI: `ui/assets/practice-view.js`.

- **Library.** Imported files are copied into `~/Library/Application Support/Headroom/Backing Tracks` (override with `HEADROOM_BACKING_DIR`); `.headroom-backing.json` there keeps titles and dates. Audio files you put in the folder yourself join the library on the next start; entries whose file is gone are dropped. Formats: anything ExtAudioFile decodes — WAV/RF64, AIFF/AIFC, CAF, MP3, AAC/M4A, ALAC, FLAC, … (up to 45 minutes).
- **Decoding** runs on a loader thread at the engine rate (best-quality sample-rate conversion), to 16-bit stereo, plus a 4000-bin overview and a gated loudness measurement. With `normalize` on (default) every track plays at about −18 dBFS RMS × `volume`, peaks capped near full scale. When the device rate changes the loaded track is decoded again at the new rate, keeping its place.
- **Real time.** The audio thread receives finished buffers through an atomic hand-off and returns old ones through a lock-free ring; it never allocates, locks or blocks (CTest counts malloc/free on the audio thread through `tests/rtguard.cpp`).
- **Speed / transpose.** Speed 0.5–1.5 keeps the pitch and transpose −12…+12 semitones keeps the tempo; both run through Apple's `AUNewTimePitch` (≈85 ms latency, already subtracted from `position`). At 100 % and 0 st the unit is bypassed.
- **Loop A–B** wraps at B back to A with a 8 ms equal-power crossfade (seeks are crossfaded the same way); with the loop on, anything past B (including a seek) lands at A. Without a loop the track stops at the end (`ended:true`); `play` then starts again from 0 (or A).
- **Count-in.** `countin` = number of click beats (0 = off, 4 = one bar, 8 = two) at `bpm`, before every `play`. The track waits; `count_in_left` counts down.
- **Duck.** With `duck=1` the track dips by `duck_db` (default −12 dB) while the pre-rig input is above ≈ −38 dBFS (attack 25 ms, hold 300 ms, release 350 ms).

Routes (GET unless noted; JSON):
- `GET /api/backing` → state: `{"ok":true,"loaded":true,"id":"bt…","title":"…","artist":"","format":"MP3","duration":212.4,"position":37.2,"playing":true,"ended":false,"count_in_left":0,"loading":false,"loading_id":"","error":"","volume":1.0,"speed":0.85,"semitones":-2,"loop":{"on":true,"a":40.0,"b":56.0},"count_in":{"beats":4,"bpm":104},"duck":{"on":false,"depth_db":-12,"amount":0.0},"normalize":true,"norm_gain_db":-4.1,"level":0.08,"stretch":{"engine":"AUNewTimePitch","pitch_preserved":true,"latency_ms":85.3}}`.
- `GET /api/backing?action=…` with any of these settings in the same call; every call returns the state above:
  - `action=load&id=ID[&play=1][&at=SEC]` — starts decoding in the background (`loading:true`, then `loaded`); the track is paused at `at` unless `play=1`.
  - `action=play|pause|toggle|stop` (`stop` = pause and go to 0, or A when looping), `action=seek&sec=S` (or `pos=0..1`), `action=unload`.
  - `volume=0..2` (linear, 1 = unity after normalisation), `speed=0.5..1.5`, `semitones=-12..12`, `loop=0|1&a=SEC&b=SEC`, `countin=0..16&bpm=30..300`, `duck=0|1&duck_db=-30..-1`, `normalize=0|1`.
- `GET /api/backing/list` → `{"ok":true,"dir":"…/Backing Tracks","current":"bt…","tracks":[{"id","title","artist","format","file","duration","added","last_used","size"}]}`, most recently used (or added) first; times are Unix milliseconds.
- `POST /api/backing/import` with `{"paths":["/…/song.mp3","/…/a folder"]}` (or `GET …/import?path=`) → `{"ok":true,"imported":[track…],"failed":[{"path","error"}]}`. Folders import the audio files directly inside them. A file with the same name and size as one already in the library returns that track instead of a copy; a name clash gets `"Name (2).ext"`.
- `GET /api/backing/rename?id=&title=` → `{"ok":true}`; `GET /api/backing/delete?id=` → `{"ok":true}` (removes the copy in the library folder, never the original).
- `GET /api/backing/peaks?bins=N` (16–4000) → overview of the loaded track: `{"ok":true,"id","duration","bins":N,"peak_db","peak":[0..1],"rms":[0..1]}` (normalised to the loudest bin).
- Telemetry gains `"backing":{"id","playing","pos","dur","speed","count","loading","ended","duck","level"}`.
- Telemetry also carries a lock-free looper summary, `"looper":{"state","state_id","tracks":[state × 3]}` (selected track's state as in `/api/looper`, plus every track's), so a tab LED needs no separate `/api/looper` poll.
- `GET /practice/youtube.html` (text/html) — the page the Practice view embeds for YouTube. It hosts the official IFrame player from the engine's http origin, because YouTube refuses embeds without a referrer (errors 152/153) and a `file://` page has none. It relays `postMessage` only: the parent sends `{hr:"yt", cmd:"load", videoId, start, autoplay}`, `{hr:"yt", cmd:"call", fn:"playVideo"|"pauseVideo"|"seekTo"|"setPlaybackRate"|"mute"|"unMute"|"setVolume", args}` and `{hr:"yt", cmd:"loop", on, a, b}`; the page posts `{hr:"yt", type:"host"|"api"|"ready"|"error", …}` and ~10×/s `{type:"state", state, time, duration, rate, rates, title, author, videoId, muted, volume, buffered}`. YouTube audio stays in the web view (it doesn't pass through the engine).
- macOS app bridge: `postMessage({action:"importBackingTracks"})` opens a native audio-file picker and `{action:"revealBackingFolder"}` opens the folder. Audio files dropped anywhere on the window go to Practice. Both call `window.headroomBackingImport(paths)`; while such a drag hovers the window the app calls `window.headroomBackingDrag(true|false)`.

## Diagnostics — `GET /api/diag` [live]

What the engine is doing right now, for chasing dropouts, distortion and level problems while someone plays. The render path only writes relaxed atomics (no locks, allocation or system calls; `core/EngineDiag.hpp`). The HTTP thread reads them. Implemented in `audio/RealtimeAudioServerDiag.inl`.

- `GET /api/diag` → JSON (below). `GET /api/diag?reset=1` clears the cumulative (`total`) counters first; the render thread applies the reset at its next block. Event history (preset loads, level migrations) is kept.
- Most sections have `total` (since start or the last reset) and `window` (the last four complete 250 ms buckets, so 1 s, up to 250 ms old; `window_covered_s` says how much of it had audio). Levels are dBFS (−200 = silence). Times are µs unless named otherwise.
- `HEADROOM_DIAG_LOG=<path>` (standalone app) appends one JSON line per second: the `window` parts and `events` (preset loads, level migrations) since the previous line. A utility-QoS thread writes it, never the audio thread. Capture a session with it, no polling needed.

Top level: `ok`, `t_unix`, `uptime_s`, `since_reset_s`, `reset_pending`, `window_s`, `window_covered_s`, `audio_rendering` (a block within the last 0.5 s), `last_block_age_ms`, `preset`, `engine_build`.

- `render`: `sample_rate`, `callback_frames`, `callback_budget_us`, `device` (`running`, `buffer_frames`, `overloads` = CoreAudio processor-overload notifications, `input_underruns`; `"host"` in a plugin), `pipelining` (`on`, `sub_frames` 0 = auto, `max_stages`, `runs`, `avg_stages`, `last_stages`), and `rtpool` (`workers`, `active`, `forks`, `ran_on_worker`, `reclaimed_inline`, `refused`, `in_device_workgroup`).
  - `total` and `window` each have: `blocks` (device callbacks), `over_budget` and `over_ratio` (render wall time > the callback's period), `mean_us`, `p99_us`, `max_us`, `budget_us`, `load` (render time / period).
  - They also have: `realtime_ratio` (audio pulled per wall-clock second; 1.0 = every device cycle rendered, lower = the device skipped cycles), `late_callbacks`, `missing_ms` and `max_gap_ms` (callbacks that came more than 1.5 periods after the previous one; the missing time is heard as dropouts), and `output_bridges` (non-continuous plan handoffs: 5 ms crossfades).
  - The window's p99 is exact (a ring of block times). The total's comes from a 1/8-octave histogram.
- `slots[]` (the rack's active units, `/api/rack` slot order): `slot`, `table_id`, `name`, `bypassed`, `same_instance` (false: the stats belong to the unit this slot held before an edit), and `rendering`.
  - `total` / `window`: `blocks` (rack renders: one per callback, or per 512-frame chunk of a longer one), `mean_us`, `max_us` (the unit's own processing time per render, summed over pipelined sub-blocks), `max_load` (that time / the period, worst block), `over_share` (blocks over an equal share of the period, period ÷ units rendering), `over_block` (the unit alone took longer than the whole period), `out_peak_db`, `out_rms_db`, `nonfinite` (NaN/Inf output samples).
  - Circuit units add `circuit`: `internal_rate_hz`, `oversampling`, `multirate`, `max_newton_iters`, and `total` / `window` solver counters. These are `samples`, `newton_failures` (every fallback failed: the last solution was kept), `homotopies`, `ptc_rescues` (pseudo-transient continuation solved it), `half_steps` (re-solved as two half steps), `lte_substeps`, and `avg_newton_iters` per sample.
  - Slots past 64 report `"tracked": false`.
- `master` (the rack's master section: no gain of its own, only the limiter and soft ceiling).
  - `total` / `window`: `rig_peak_db` / `rig_rms_db` (the rack's output into the limiter), `post_gain_peak_db` (the same peak; kept for older readers), `limiter_samples`, `limiter_pct`, `limiter_max_gr_db` (the −1 dBFS peak limiter), `soft_ceiling_samples` (above 0.85 after the limiter, shaped by the soft ceiling), and `nonfinite`.
- `output` (after the rack: DC blocker, OUT gain, the last-resort guard that bends samples above 0.95, then the backing track).
  - `total` / `window`: `clipped_l` / `clipped_r` (|x| ≥ 1.0 at the device), `guard_l` / `guard_r` / `guard_pct`, `peak_l_db`, `peak_r_db`, `pre_guard_peak_db`, `rms_db`, `nonfinite`, `input_peak_db`, `input_clip_samples` (|input| > 0.98 after the input gain).
- `level`: `automatic_leveling` (always `false`: the engine never measures or re-levels a rig), `migrations[]` (the one-time user-rig migrations of this session, newest first: `preset`, `path` (`"plugin state"` for a DAW project), `status` (`migrated` | `no_level_knob` | `write_failed`), `cache_gain` / `cache_db` (what the old cache gave the rig), `live_knob_db` (what the formerly dead Output knobs now add at the rig's stored values, taken off the cached gain), `applied_db`, `residual_db` (what no knob could take), `change_db` (the rig's level now minus the level the old engine played it at: 0 = no jump), `moves[]` (`unit`, `table_id`, `param`, `label`, `from`, `to`, `db`), `written`, `backup`, `knob_search_stopped_at`, `error`), `migration_source` (the old cache file) and `migration_ledger`.
- `worker` (background-QoS worker: amp warm-up, plan collection): `queued`, `qos`, `thread_cpu_s`.
- `preset_loads[]` (newest first, 16): `idx`, `name`, `source`, `result` (`ok` | `superseded` | `error`), `error`, `build_ms`, `total_ms` (until the swap), `units`, `spares_used` (units taken already prepared), and `level_migrated` (this load folded the old cache's gain into the rig's knobs).
- `threads` (thread CPU clocks, seconds; −1 = not running): `worker_cpu_s`, `spares_pool` (`cpu_s`, `prepared`, `taken`, `spares`, `preparing_table_id`, `held`), `http_cpu_s`, `preset_loader_cpu_s`, `library_scan_cpu_s`, `diag_log_cpu_s`, and `process_cpu_s`.
