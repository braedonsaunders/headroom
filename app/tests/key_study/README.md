# Key-detection study (2026-11-08)

Data: GuitarSet (CC BY 4.0) in `work/midi/dl`. It has 360 JAMS files with `key_mode`, `note_midi` and `chord` annotations, plus `gset/*_mix.wav`, which is players 00 and 03. The guitar→MIDI transcriber was trained with those two players held out.

| Method | Data | Exact key, held-out 00/03 |
|---|---|---|
| Krumhansl–Schmuckler, Temperley profile, duration-weighted | true notes, whole take | comping 75 %, solo 68 % |
| Chord function, from lead-sheet chords | oracle chords | 100 % |
| Chords recognised where ≥ 3 notes sound together → function, blended with KS by chordal fraction | transcriber notes | comping 91.7 %, solo 68.3 %, all 80 % |
| Same, streaming in `ListenAnalyzer`, last key shown | audio | comping 83 %, solo 68 %, all 75.8 % |

The streaming version averages under 1 key change per take. For comparison, the chroma/profile version it replaced scored 64 %, and literature state of the art is about 68–80 % exact on pop/rock audio.

- `keylib.py` holds the symbolic methods and scoring (exact / MIREX weighted).
- `hybrid.py` tunes the blend on players 01, 02, 04 and 05 and tests on the 00/03 transcriptions. Transcriptions come from `rig_cli p2m-transcribe resources/models/guitar_midi.hgnn <wav> <csv>`.
- `eval.sh` runs the full audio benchmark: `tests/listen_bench --key-eval2 <gset dir>`, in 8 shards. Pass `LK_*` environment variables to override the decision constants.

The scripts keep their scratch files in `$KEY_STUDY_DIR` (default `/tmp/key_study`).

Open problem: solos (single-note improvisation) stay around 68 %. A learned reranker over the template scores is the next step; see the "key lime" reference in the research notes.
