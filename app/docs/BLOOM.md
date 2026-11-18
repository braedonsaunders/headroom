# Bloom (900006): the arpeggiator the guitar plays

Engine: `src/midi/Bloom.hpp` (header-only, real-time safe). Unit: `BloomComponent` in `src/midi/MidiComponents.cpp`.
UI: the `bloom` kind in `ui/index.html` (`bloomHTML`, `paintBloom`). Tests: `tests/test_bloom.cpp` (CTest `bloom`),
`tests/test_midi_handoffs.cpp`. Real-DI probe: `bloom_probe <di.wav> <out.wav> [param=value …]`
(Pitch → MIDI (neural) → Bloom, prints a timeline, writes Bloom's audio, reports its CPU).

## Signal path

```
guitar ─▶ Pitch → MIDI (poly, neural; Guitar Thru) ─▶ Bloom ─▶ [MIDI FX] ─▶ Instrument (Mix) ─▶ drive ─▶ amp ─▶ cab
                     notes on the lane bus ─────────▶  │
                                                       ├─ MIDI bus: arpeggio notes + pitch bend (Voice Synth / Both)
                                                       └─ audio: dry × duck + Strings voice   (Voice Strings / Both)
```

The Strings voice and any instrument after it are summed before the amp. These are the fused hybrid rigs, not layers.

## What it does

| Part | How |
|---|---|
| Chord memory | Tracked notes, committed 35 ms after the last change, so a strum is one chord. Only new notes change it: decaying strings keep the chord that was ringing, and a Bloom in progress keeps its chord. A bend the tracker relabels as a new note (within 3 st, PLL already deviated towards it) stays the same chord tone. |
| Key | The Listen pane's validated method, run on the unit's own notes: a duration-weighted Temperley profile blended with the harmonic function of ≥ 3-note chord windows, with a sticky decision. Or Key / Scale set by hand. |
| Harmony | Diatonic first: the scale degree whose stacked thirds contain every held pitch class (lowest note = root bonus). A power chord gets its diatonic third, and one note becomes the chord on its degree. Extend adds the 7th, 9th and 11th (13th on major chords, where the 11th clashes). Non-diatonic chords fall back to templates (major → dominant 7th unless the key has the maj7). As Played uses only what is held. |
| Keyed | The arpeggio is a pure function of what you play. The same chord (and the same riff) always gives the same figure: same notes, same rhythm from the pick, the same every pass. A pick attack opens a 60 ms strum window; the chord is what the tracker reports inside it (minus faint guesses under 40 % of the strongest note's velocity). Notes after it never change the chord: ghosts, sympathetic strings, decay. If the onset detector misses an attack (a soft strum over a louder chord, a re-strike), two or more new or re-picked notes within 50 ms count as one; a single stray note never does. Each attack restarts the figure on its own clock: steps run at fixed sample times from the pick, at the tempo the phrase began with. Steps inside the strum window are counted but not played, so the first note heard always comes from the complete chord. Retrigger **Chord** (default) restarts only when the chord changes; **Pick** restarts on every attack. Walk and Random are one-bar motifs seeded by the chord. Pick strength changes only loudness (Touch); density moves only with Motion. Verified: the 2nd and 3rd passes of a looped DI riff through the neural tracker give identical notes (Rig clock: identical timing; Feel: within ~11 ms), and a synthetic riff played softer, with ghost notes, matches to 0.2 ms in all eight patterns. |
| Ladder & patterns | The chord tones from the phrase's lowest note over Range octaves. Patterns: Up, Down, Up/Down, Converge, Walk (a seeded one-bar motif of small steps), Melody (climbs to the top note of the chord just picked and lands on it every beat), Pedal (the bass between every tone), Random (a seeded one-bar motif). |
| Trigger | **Bloom**: a chord still ringing (> −52 dBFS) for Bloom Time with no new attack blooms; the next attack stops it at once (dry back in 2 ms). **Always**: whenever there is a chord. **Engage**: while `engage` is on. **Freeze**: `engage` snapshots the chord and holds it whatever is played over it (dry stays at full). In Bloom mode Engage forces it on. |
| Hands | Onsets from a high-passed envelope (fast/slow ratio), back-tracked to where the attack began. Pick strength is the peak 20 ms after the attack against the running mean: it drives the arpeggio's loudness (Touch), never which notes it plays. A palm mute (≥ 15 dB down 90 ms after the attack) makes the notes staccato (Mute Choke). Muting the strings with the fretting hand (fast envelope ≥ 20 dB down within 107 ms, ≥ 220 ms after an attack) stops Bloom/Always and forgets the chord. |
| Bends & tuning | A heterodyne tracker per string on harmonics 1–3: four one-pole low-passes (≈ 0.07·f, 4–14 Hz), a reference through the same filters (no lag bias), and an oscillator aimed ahead along the bend. During a bend the reading furthest from the resting pitch wins, so an octave-doubled string below can't hold it back. Its output is pitch bend in semitones × Bend Follow (8-cent dead zone), plus the strings' mean tuning offset, so the synth plays in the guitar's tuning. The mono tracker's own bends pass through. |
| Feel clock | Each phrase takes the learned tempo when it starts. Bloom, Engage and Freeze, which begin without a pick, enter on the next 16th of the groove. Tempo comes from a histogram of every inter-onset interval in the last 8 s (all pairs, so swung off-beats still give exact beats), scored at multiples of the beat plus its half-beat, with a log-tempo prior around the rig tempo. The phase is the strongest onset cluster on the beat circle, swing is where the off-beat cluster sits (≥ 0.555), and the accents are the onset weight per 16th. It locks at ≥ 70 % of onsets on the grid. Until it locks, or with Clock = Rig, it uses the rig/host tempo (and the host's ppq when the transport plays). |
| Strings voice | For each tracked note, ~70 ms after it is committed: the exact period (normalised autocorrelation, ±30 cents, parabolic), then a Hann-weighted pitch-synchronous average of K periods (≈ 85 ms) into one 512-point cycle (an FFT, normalised, with 7 band-limited mip tables). Each arpeggio note uses the captured string with the smallest transposition, with a 1.5 ms attack, a decay set by Tone, a two-pole low-pass closing like a string, and the captured pick click (8 ms high-passed at the last attack). Level follows the captured string's RMS. Equal temperament makes some partials of a chord's notes coincide (a fifth's even harmonics, a major third's 5th/3rd 5.5 Hz apart): those blend, as they do on the guitar. |
| Swing | Straight subdivisions are swung by warping the beat. The off-beat 8th moves to the swing ratio and the 16ths in between follow, the way a drummer plays swung 16ths under swung 8ths. |

## Controls and footswitches

- `engage` is a bool. On the face it's the big pad (Latch: tap toggles, a long press is momentary; Hold: every press is momentary).
- Hardware: Headroom now listens to every CoreMIDI source (`src/midi/MidiIn.hpp`, implemented in `MidiOut.mm`). Defaults:
  - CC 85 → `bloom:engage`: on at ≥ 64, so a momentary pedal holds and a latching one latches.
  - CC 86 → `bloom:toggle`: each press flips it.
  - CC 4 (foot controller) → `bloom:motion`.
  - CC 11 is still Macro 2, which the Bloom library rigs map to Motion.
  - These targets act on every Bloom unit in the rig, whatever preset is loaded.
- Telemetry: `midi.bloom[slot]` (ENGINE_API.md).

## Cost

About 0.3–0.4 % of a core on average (preset_rt, 128 frames, 9900X), 75–90 µs p99 in blocks that capture a string, with one capture per block at most. The rig's amp dominates.

## Library

Six "Guitar + MIDI" rigs, specs in `tools/library/specs/bloom/`:
- Bloom Garden: Bloom, Strings + bells.
- Freeze Choir: Freeze, pedal-point choir.
- Feel Machine: Always, Feel clock, glass pluck.
- Cascade Lead: Engage, Melody, saw into a 120 W head.
- One-Finger Orchestra: Extend 11th.
- Pedal Point Bass: Bloom, Pedal pattern an octave down through MIDI FX.
