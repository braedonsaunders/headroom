# Headroom circuit engine

Status: design plus first implementation (Phase 1 and Phase 2). Code: `app/src/circuit/`. Netlists:
`app/src/circuit/library/*.circuit.json`. Validation: `app/tests/test_circuit_engine.cpp` and
`app/tools/circuit/`. Measured results are in [section 10](#10-results-phase-2), and every source is listed in
[section 11](#11-sources-and-licences).

## 0. Goals and non-goals

**What it is for.** Every amp, pedal and effect in Headroom should run as its real circuit: the schematic,
component values and active-device physics. Users should be able to open any unit, see the circuit, change
parts, swap devices and save the result. The circuit, not a hand-written catalogue entry, becomes the definition
of a unit, including its controls.

**Legal line.** Every model here comes from public circuit knowledge: published schematics and parts lists,
datasheets, papers and permissively licensed code. Nothing comes from proprietary third-party software. GPL and LGPL projects (Guitarix, some Faust libraries, LiveSPICE, which
is MIT but is read only as a reference here, see section 11) are cited for ideas only, and no code is copied.

**Non-goals.**
- Not a general-purpose SPICE. There is no adaptive time step and no full BSIM. Device models are the compact
  audio models from the literature.
- Not cycle-accurate to specific hardware units. Accuracy is defined against **ngspice running the same
  netlist with the same device models** (solver accuracy) and against the published circuit (model accuracy).
  Hardware measurements can be added later as a third tier.

## 1. Architecture at a glance

```
 netlist (.circuit.json | SPICE subset)                 ← single source of truth per unit
        │  parse + validate (Netlist.cpp)
        ▼
 Part graph ──► expand ──► primitive graph               R, C, L, sources, VCVS/nullor, NL ports
        │                      │
        │                      ▼
        │            node classification                 ground / known (sources) / driven (ideal outputs) / free
        │                      │
        │                      ▼
        │            exact partitioner                    union-find on conductive edges, SCC on sense deps
        │                      │
        │                      ▼
        │            per-group solver selection            linear SS | WDF (SP tree + R-type core, 1 NL root)
        │                      │                           | nodal DK + damped Newton (any NL count)
        ▼                      ▼
 controls (pots, switches, value laws) ─► allocation-free matrix refresh at control rate (glided)
                               │
                               ▼
 CircuitProcessor: oversample → groups in topological order → probes/outputs → downsample
                               │
                               ▼
 CircuitUnit (IComponent + Introspectable): .rig param ids → circuit controls; component page model
```

There are three ideas in this design:

1. **Exact partitioning at ideal boundaries.** A node driven by an ideal voltage output (op-amp output,
   VCVS, nullor) and a node that is only *sensed* (an op-amp input, the control port of a VCCS) are one-way
   couplings. No current crosses them, so the circuit can be cut there with **no** approximation. The
   partitioner finds every such cut automatically. Any feedback loop that crosses one (an op-amp and its
   feedback network) shows up as a strongly connected component, and the partitioner merges it back into one
   solve group. The full TS808 splits into 4 groups of 2-3 nonlinear dimensions, instead of one
   9-dimensional Newton problem. Approximate cuts at high-impedance boundaries, which add a one-sample delay, are opt-in
   per netlist (`"cut"` hints, section 2.6) and reported in the model page.
2. **One linear algebra, two numerical forms.** The DK method (Yeh; Holters and Zölzer) and wave digital
   filters (Fettweis; Werner; Chowdhury) are both trapezoidal discretisations of the same MNA system. The
   engine builds both from the same primitive graph and picks a form per group:
   - **Linear state-space** for groups with no nonlinearity.
   - **WDF** for groups with one diode-type nonlinear one-port at the root. The tree is built
     automatically: a series-parallel decomposition gives O(n) adaptors, and an R-type core (Werner),
     derived by nodal analysis, covers what isn't series-parallel, including controlled sources. The root is
     seeded from the Wright-omega closed form. WDF stays passive while parameters change, so it is the
     natural solver for modulated passives: wahs, phasers, LFO-swept filters.
   - **Nodal DK with damped Newton** for everything else: multi-port devices (BJT, triode, op-amp macro)
     and coupled nonlinearities.

   Agreement between the forms is a test: the WDF and DK solves of the same group match to about 1e-9 V
   (measured 2e-9 V during a glided pot sweep).
3. **The netlist is the unit.** Controls, ranges, tapers, the mapping to legacy `.rig` parameter ids,
   schematic layout, probes, provenance and tolerances all live in the netlist. The UI's knobs, the component
   page, `.rig` load/save and the validation harness all read the same file.

## 2. Circuit description format

### 2.1 JSON netlist (`headroom.circuit.v1`)

The JSON netlist is meant to be written and edited by hand and diffed in review, and it is versioned with the
`schema` and `version` fields. Here is a minimal example:

```json
{
  "schema": "headroom.circuit.v1",
  "id": "ibanez-ts808",
  "version": 1,
  "name": "Tube Screamer TS808",
  "description": "JRC4558 non-inverting clipper, 1N914 pair in the feedback loop, active tone, BJT buffers.",
  "sources": [{ "title": "Pedal Parts Tube Screamer v2.0 build document (TS808 BOM)", "url": "https://pedalparts.co.uk/docs/TubeScreamer2.pdf" }],
  "io": { "input": "in", "output": "out", "inputVoltsPerFS": 1.0, "outputVoltsPerFS": 1.0 },
  "oversampling": 2,
  "solver": "auto",
  "models": { "1N914": { "kind": "diode", "is": 2.52e-9, "n": 1.752, "rs": 0.568 } },
  "controls": [
    { "id": "drive", "label": "Drive", "default": 0.5, "params": ["Dr"] }
  ],
  "parts": [
    { "ref": "VCC", "type": "V", "nodes": ["vcc", "0"], "value": "9" },
    { "ref": "R7", "type": "R", "nodes": ["inv", "fb"], "value": "51k", "tolerance": 0.05 },
    { "ref": "DRIVE", "type": "pot", "nodes": ["fb", "out1", "out1"], "value": "500k", "taper": "A", "control": "drive" },
    { "ref": "C3", "type": "C", "nodes": ["leg", "vr"], "value": { "nominal": "47n", "control": "bass", "min": "22n", "max": "100n", "law": "log" } },
    { "ref": "D1", "type": "D", "nodes": ["inv", "out1"], "model": "1N914" },
    { "ref": "IC1A", "type": "opamp", "nodes": ["inp", "inv", "out1", "vcc", "0"], "model": "JRC4558" }
  ],
  "probes": ["out1", "out"],
  "layout": { "columns": { "fb": 6 }, "rows": { "leg": 1 } }
}
```

**Values.** A value is either a number or an engineering string in electronics convention: `4k7`, `2u2`,
`47n`, `51p`, `1M` for mega, `220R`, with an optional unit (`Ω`, `F`, `H`). `meg` is accepted too. The SPICE
importer uses SPICE's own convention, where `M` means milli. A value can also be a *value law* object:
`{nominal, control, min, max, law: "lin"|"log"}`, which makes any R, C or L a continuously controlled part.
That covers a "Bass" knob that is really a cap-value mod, LDR resistance, or a component the user is sweeping
in the editor.

**Part types** (`type` field, and the pin order of `nodes`):

| type | nodes | notes | status |
|---|---|---|---|
| `R`, `C`, `L` | a, b | `tolerance` (fraction) for Monte-Carlo runs; any value may be a value law | **v1** (non-ideal `esr`/`esl`/`leak`/`rs`/`cp`: Wave A) |
| `V` | +, − | DC source. One terminal must be ground (v1), and the node becomes *known* | **v1** |
| `pot` | 1, wiper, 3 | `value` = track resistance; `taper` A/B/C/W or `{type, mid}`; `control`; wire as a rheostat by repeating a node; optional `rend` (end resistance, §10.15); optional `span` [lo, hi] (the part of the control travel the track covers, the wiper resting at the near end outside it: two half-tracks [0, 0.5] / [0.5, 1] and two wiper `switch`es on the same control make a centre-tapped pot, as on the SSL 4000 boost/cut controls) | **v1** |
| `switch` | a, b | `control`; `ron`/`roff`; multi-throw switches are several `switch` parts sharing a control | **v1** |
| `D`, `Dpair` | anode, cathode | `model` → diode; same-model antiparallel `D` pairs are merged into one port automatically; zener breakdown with model `bv` / `ibv` / `nbv` (ngspice law, §10.15; `D` only) | **v1** |
| `npn`, `pnp` | c, b, e | Ebers–Moll transport; Gummel–Poon extensions in section 4 | **v1** |
| `opamp` | +, −, out [, V+, V−] | `model` → `level: ideal` (nullor) or `macro` (GBW, slew, rails from supply pins); optional output current limit `ilim` with `rout` (§10.15, fixed rails); input errors `vos`, `ib`, `ios` (§10.20, default 0) | **v1** |
| `triode` | plate, grid, cathode | Koren plate law with Headroom grid-conduction law; Miller caps from the model | **v1** |
| `vcvs`, `vccs` | out+, out−, ctrl+, ctrl− | `gain`; for behavioural glue (vcvs output referenced to ground) | **v1** |
| `njf`, `pjf` | d, g, s | Shichman–Hodges JFET with channel-length modulation | Wave B |
| `pentode` | plate, screen, grid1, cathode | Koren pentode / beam tetrode with screen current and grid conduction; Cg1k, Cg1p, Cpk from the model | **Wave A done** |
| `transformer` | a1 b1 a2 b2 … | windings `turns`, per-winding `r` (DCR) and `leak` (leakage L), core `lm` (magnetising L) and `lsat` (saturation flux), both referred to 1 turn | **Wave A done** |
| `V` + `"ac"` | +, − | `{amplitude, freq, phase}` mains source (phase 90° = starts at the crest, so supplies fed straight from it start charged; a supply behind a `transformer` starts at its periodic steady state, §3.10) | **Wave A done** |
| `D` with a `vacdiode` model | anode, cathode | vacuum rectifier (Child–Langmuir); GZ34/5AR4, 5U4GB, 5Y3GT | **Wave A done** |
| `ldr` | a, b, lamp+, lamp− | neon lamp → light state (attack, release) → CdS photoresistor (Fender "roach") | **Wave A done** |
| `spring` | coil+, coil−, out, ground | spring reverb tank: electrical transducers (coil R+L, 2.5 kΩ output) around a dispersive two-spring block | **Wave A done** |
| `vca` | in, ec+, ec−, sym, v−, gnd, v+, out | Blackmer VCA (THAT 2180 SIP order; dbx 202: Ec → ec−, ec+ → gnd): current in / current out, gain (dB) = (Ec+ − Ec− − vczero)/mvdb; control pins are bases (high impedance), pin 5 held below GND (`v5`), optional core β; models THAT2180A/B/C, DBX2151, DBX202 (§10.13, §10.18) | **done** |
| `rms` | in, ibias, gnd, sym, v−, cap, out, v+ | log-domain RMS detector (THAT 2252 SIP order): out = 2VT·ln(Irms/Iin0) at mvdb, Iin0 = √(IBIAS·IT)/2.9, τ = CT·VT/IT from the external CT and timing resistor; `mvdb` sets only the output slope, `vtj` the junctions (§10.17); model THAT2252 (§10.13) | **done** |
| `dff` | d, clk, set, reset, q, qb, vdd, vss | CMOS 4013 section: node-level D / CLK / SET / RESET, driven Q / Q̄; sub-sample edge timing, band-limited outputs (§10.14); CD4013B | **done** |
| `jkff` | j, k, clk, set, reset, q, qb, vdd, vss | CMOS 4027 J-K section, the same cell machinery (§10.14); CD4027B | **done** |
| `nand` | a, b, y, vdd, vss | CMOS 4011 gate (§10.14); CD4011B | **done** |
| `inv` | a, y, vdd, vss | CMOS inverter: unbuffered CD4069UB / CD4049UB (the channel pair, analog) or buffered CMOS_INV (logic cell) (§10.14) | **done** |
| `ad536` | vin, nc, −vs, cav, db, bufout, bufin, iout, rl, com, nc, nc, nc, +vs | AD536A rms-to-DC converter, D-14 pin order: implicit rms (I1²/I3 averaged by R1·CAV), linear IOUT, dB pin −3 mV/dB; AD536AJ (§10.17) | **done** |
| `speaker` | +, −, vel | electro-mechanical loudspeaker: Bl(x), Kms(x), Le(x), LR-2 voice coil, voice-coil / magnet thermal network; `vel` is the cone-velocity node (m/s, mobility analogue: mechanical parts such as a box air spring hang on it); `series` / `parallel` fold identical drivers into one exact equivalent; models CELESTION_G12M25_8/16, CELESTION_V30_16, JENSEN_P12Q_8, JENSEN_C12N_8, EMINENCE_LEGEND1258_8 (§10.21) | **done** |
| `sub` | as declared | sub-circuit instance; `of` names a `subcircuits` entry or `lib:<name>` from the shared block library (`library/*.blocks.json`, e.g. Thiele–Small speakers) | **v1** |

**Controls** are the unit's public parameters:

```json
{ "id": "drive", "label": "Drive", "kind": "continuous", "default": 0.5,
  "params": ["Dr"], "paramRange": [0, 1], "unit": "", "group": "Main", "note": "500 kΩ audio-taper pot" }
```

- `kind` is `continuous`, `switch` or `selector` (with `options`).
- `params` lists the legacy parameter ids this control answers to, for `.rig` files.
- `paramRange` maps the legacy raw range onto 0..1.
- `"face": true` on a switch or selector with no legacy parameter: CircuitUnit exports it as a stepped extra parameter
  (raw value = option index), so the unit face lists it (an illustrated face puts it in its "+N more" drawer).
  Opt-in: other such controls stay internal.
- A control drives any number of parts: pots, switches, value laws, and selector-driven part swaps.

**IO.** `io.input` is the node the audio input drives through an implicit ideal source referenced to ground,
and `io.output` is the node that is read. `inputVoltsPerFS` and `outputVoltsPerFS` give the digital-to-volts
calibration, which reuses `rig::circuit::VoltageCalibration`. Optional `io.sourceImpedance` puts a pickup
model (R, L, C) in front of the input.

**Stereo circuits.** Optional `io.outputR` names a second output node. A netlist with it runs as one instance that renders both sides: the left channel reads `io.output`, the right reads `io.outputR`, and a stereo input is fed as its mean. Use it when the stereo image is part of the circuit, as in the JC-120's chorus (dry on one power amp, dry ± delayed on the other). A netlist without `outputR` runs as two independent instances, one per channel. Both output nodes are exported, decimated from the oversampled rate, and never placed in a slow partition. For amps, set `outputVoltsPerFS` to about 1.5× the dimed peak speaker voltage.
Optional `io.inputR` names a second driven input (a stereo circuit fed true stereo, such as a linked bus compressor): L drives `io.input` and R drives `io.inputR`, and the netlist runs as one instance (§10.13).

**Probes** name nodes the component page can scope. Any node can be probed at runtime, and the list here only
sets the defaults.

**Layout** holds hints for the automatic schematic layout: `columns` and `rows` pin a net to a column or row
(section 6). Full per-part coordinates are a v2 schematic-editor feature.

### 2.2 Subcircuits (implemented)

`"subcircuits": {"name": {"pins": [...], "parts": [...], "models": {...}}}` is instanced with
`{"ref": "X1", "type": "sub", "of": "name", "nodes": [...], "controls": {"inner": "outer"}}`.
- The expander (`expandParts` in `Netlist.cpp`, at the JSON level before validation) namespaces internal
  nodes and refs as `X1.node` / `X1.R1`.
- It maps pins to the instance's nodes, keeps ground as ground, remaps control names, and nests up to 8
  deep.
- Models declared inside a subcircuit join the netlist's models.

A test checks that two instanced RC sections equal the hand-flattened circuit bit-exactly. This is how family blocks are reused: the 12AX7 gain stage, the TMB tone stack, the long-tail-pair
PI, the push-pull output.

### 2.3 Versioning and migration of the format

`schema` is a hard gate. A file whose `schema` the engine doesn't know is rejected with an error. `version` is
the unit's own revision. A `.rig` stores the id, version and *only the user's deltas*, as JSON-pointer-like
paths into the netlist (`parts.C3.value`, `models.1N914.is`). That way a factory revision still reaches users
who didn't override that part. Format upgrades are explicit `v1→v2` functions in `Netlist.cpp`, tested against
every library file.

### 2.4 SPICE subset import

**Implemented (v1):** `R C L V D Q E G` element lines, `.model D/NPN/PNP`, SPICE suffixes (`MEG`, `M`=milli),
`+` continuations and `*` comments.
- A source named `VIN…`, or any AC/SIN/PWL/PULSE source, becomes the input port.
- Pots aren't a SPICE concept, so the importer reads a `*@pot REF TOTAL PIN1 WIPER PIN3 TAPER CONTROL`
  annotation.
- `*@io IN OUT` names the ports. Otherwise a node called `out`/`n_out`/`vout` is taken as the output.
- Anything it can't express is reported per line in `SpiceImportReport::warnings` and never silently dropped
  (`circuit_cli import deck.cir`).

**Planned:** `J X`, `.subckt/.ends`, `.param`.

The exporter (`exportSpice`) writes the same subset, plus B-source expressions for devices ngspice doesn't
have natively (Koren triode, op-amp macro, rail clamp) and an XSPICE `filesource` for audio-file input. The
harness uses it, and a test round-trips export → import → identical output.

### 2.5 Catalogue migration: controls come from the circuit

Today a unit's controls come from `src/generated/ComponentCatalog.hpp`, which holds the numeric catalogue ids,
names and parameter ranges. With the circuit engine:

1. **Netlist controls become the unit's schema.** `CircuitUnit` builds its control list from
   `netlist.controls`, so the UI gets ids, labels, kinds, defaults and units from the circuit file.
2. **Compatibility.** Each control lists the legacy `params` it answers to. `setParameter("Dr", x)` is
   routed to `drive`. `.rig` files keep loading, and saving writes the circuit control ids plus the legacy
   aliases while the old catalogue still exists. If table ids and param ids are ever replaced,
   the alias list is the single place where old ids live. Units keep their identity through
   `netlist.id` (`ibanez-ts808`), and the numeric `table_id` becomes a lookup key only.
3. **Controls with no circuit meaning.** A legacy control that has no part in the real circuit (the
   catalogue TS808's `Clean` knob) gets one of three treatments:
   - it is **declared as a documented mod** (the TS808 `Bass` knob is the well-known C3 more-bass mod,
     and `Bright` is the C9 tone-range mod);
   - it is **kept as a stored no-op** with `"legacyOnly": true`, so presets still round-trip;
   - or the unit is renamed.
   The model page lists which treatment each control got.

### 2.6 Supply cuts (implemented)

`"cuts": ["bp1", "ch", "bp2", "bp3"]` names **every node of a supply chain**.
- Elements whose terminals are all cut nodes, ground or sources (rectifiers, reservoir and filter caps,
  chokes, dropping resistors) form the supply partition.
- Every other element touching a cut node is a load. It sees the rail as a known voltage, and the current it
  draws (from resistors, capacitor and inductor companions, or device ports) is injected into the supply
  partition one sample later.
- At DC the two sides alternate to convergence, so the operating point is exact.
- Transformer windings ending on a cut node get their DCR placed on that end automatically.

Without cuts a whole amp is a single 11-port Newton problem; with them the largest is 7 ports. On a
GZ34 + 12AX7 + 6L6 push-pull + OT + speaker amp driven into sag, the cut changes the speaker output by
0.0044 % NRMSE (CTest `circuit_engine`, `testCutError`). If a cut partition has no DC path of its own
(a chain node missing from the list), the DC solve fails with that group named.

**Stability guard (2026-11-10).** The lag is a loop: per sample the rail moves by Z_owner·ΔI_loads, so
Z_owner·Y_loads must stay well below 1. A rail held by a reservoir cap has Z_owner ≈ T/2C (milliohms) and is
fine. A cap-less rail behind a dropping resistor is not: the DR103's screen rail `scr` (470 Ω to four 100 Ω screen
stoppers) had a loop gain of 18.8 and limit-cycled at ~35 kHz as soon as the screens drew (scr jumping 400 V ↔ 70 V
every sample, the plates 20 V ↔ 900 V; spk −1.7 dB in the audio band, +37.6 dB above 30 kHz, 49.6 % NRMSE against
ngspice dimed). The compiler now computes, for each supply cut, Z_owner (the instantaneous driving-point impedance
of the supply network at this rate: trapezoidal companions, rectifier diodes open) and Y_loads (Σ 1/R of the load
resistors on the node), and demotes a node whose product exceeds 0.5 to an ordinary node, solved implicitly with
its loads (exact), with a compile warning. `CE_CUTDIAG=1` prints every cut's gain. Library scan: only the DR103
`scr` trips (since removed from the netlist); the next highest is the Dimension D's `p14` at 0.13, every amp rail
≤ 0.02. Load capacitors and device ports wired straight to a rail are not in the bound (a decoupling cap into a
floating consumer node is no conductance; a device has no bound before the operating point).

**No-reservoir cuts (2026-11-10).** A small bound is not enough when nothing holds the rail. The 1176's `v30` is an
ideal source behind 2 Ω (the zener's dynamic impedance, no reservoir cap) with transistor collectors on it: bound
0.02, yet the plain lag, together with the `pre_ac` signal cut, grew an idle mode with tau ≈ 32 s (1.37x per 10 s
after a kick). From numerical noise it reached Newton rescues after ~17 min of silence (3.0 → 22 it/sample, maxIt
239, PTC every sample), which is what took the live rig to 100x real time every ~16 min after a load. Uncut, the
same netlist is stable even behind 30 Ω, so the growth is the lag's, not the circuit's (behind 0.5 Ω, or with any
reservoir cap on the rail, it is stable cut too). The compiler now also computes Z_owner with the supply caps open;
a loaded supply cut whose caps do not at least halve Z_owner has no reservoir, and its owner carries the loads'
estimate exactly like a signal cut (G of the load resistors ∥ C_hf, below), so only the residual lags. That keeps
the partition (demoting `v30` cost the 1176 2.7x CPU, and demoting the Small Stone's `vcc` merged a 60-port group
with no DC solution). Device ports and load caps count as loads for this rule. `CE_CUTDIAG=1` marks such cuts
`NO RESERVOIR`; `CE_CUT_RESERVOIR=0` restores the plain lag for A/B. Library scan: it applies to the 1176 (`v30`,
`vm10`), the Dyna Comp (`vcc`) and the Small Stone (`vcc`), whose originals have no supply reservoir; every other
netlist renders bit-identically. Against the plain lag the 1176 differs by −128 dB on a guitar DI, the Dyna Comp by
−204 dB, the Small Stone by −80 dB (its LFO phase), and every SPICE gate case of the three is unchanged.
CTest `circuit_engine` (`testNoReservoirCutIdle`) kicks one of two 1176s at the rig's settings and requires the
difference not to grow over 60 s (plain lag: 1.1e-9 → 2.8e-9 V; with the estimate: noise, 1.5e-12 → 2.1e-12 V).

**WDF consumers of a cut.** A consumer group whose cut current flows through a capacitor or inductor (a
choke-input filter's choke on a cut rail) stays DK: the WDF form keeps neither the DK companion state nor the
branch voltage that current needs. Picked WDF, the 6G15's rail read stale companions and drifted ~95 V (ENGINE_QUEUE
item 18: 64 % / 85 % NRMSE at out / bpa with `cuts: ["bpa","bpb"]`, now 0.028 % / 0.051 %).

**Signal cuts** (`{"node": "n1", "loads": ["RMIX1"]}`) name their loads explicitly. The plain one-sample lag
is a loop of gain Y_load·Z_owner that must stay below 1 up to Nyquist, and with trapezoidal companions a
capacitive load path to a fixed node (a coupling cap into a grid with Cgk to a bypassed cathode) has
Y → ∞ there: the Twin's reverb send rang at fs/2 until this was handled. The owner therefore carries an
estimate of the load implicitly, G (the loads' direct resistors) ∥ C_hf (the Schur complement of the loads'
capacitor network seen from the cut node, stamped as a synthetic owner capacitor), and only the residual
current i_load(n−1) − i_est(n−1) lags. Nodes without capacitance of their own are held by 1e-9 × the group's largest
capacitance, capped at a stray 10 pF: an output transformer's 1 F flux node used to make that 1 nF (§10.40; a cut written
`"chfHold": "group"` keeps the uncapped hold, `CE_CUT_CHF=0` restores it everywhere). Prefer resistive loads anyway (mixing resistors): the residual lag of
a capacitive load still costs about 0.5 dB at 5 kHz, a resistive one 0.005 dB. At DC the iteration also
requires the cut voltages to settle (a mixing node held only by gmin behind coupling caps).

**Finding signal cuts.** `circuit_cli autocut` (§10.27) enumerates the nodes that split a large group and ranks them
by the impedance into the node. It measures each against the uncut netlist (DI and hard-sine nulls, every switch
position, idle growth, Newton rescues, aliasing, instructions and the netlist's SPICE cases), and keeps a cut only under
an explicit no-loss rule. Never cut inside a feedback loop.

### 2.7 Live readouts for the illustrated faces (`"readouts"`, 2026-11-10)

A netlist declares where the face's live meters read the circuit (CircuitUnit → `IComponent::liveReadout` →
`/api/telemetry?units=`, ENGINE_API.md "Per-unit meters"). Nodes are exported as probes of the left channel's circuit and
read once per block on the audio thread (no allocation, no locks).

- `"lfo": "<node>"` or `{"node": "<node>", "minSwing": V}`: the LFO lamp's phase from rising crossings of the node (with
  hysteresis). While the swing is below `minSwing` (default: a 1e-4 relative floor) or no crossing comes for 2.5 periods,
  the oscillator counts as stopped and the face's lamp rests (`"lfo": -1`). Twin / Deluxe use the roach lamp's light
  with `minSwing` 0.05: switched off it is dark at any drive (~1e-15, since the roach-driver fix, §10.23), running it
  swings ~0.7. The AC15 reads the Vibravox oscillator's plate `p6` with `minSwing` 5 V: off it moves < 0.03 V
  even played hard, running it swings 110–130 V.
- `"gr": {...}`: gain reduction in dB for the GR meter, three laws:
  - `{"law": "ratio", "node": n}`: a meter node whose voltage falls with GR (a photocell meter bridge):
    20·log10(v_rest / v), v_rest = the node at the no-signal operating point after `reset()` (the meter's zero adjust).
    LA-2A: `mtr_cell`, the second T4B cell in the NE-2 bridge; it reads within 1.5 dB of the gain taken off to 15 dB.
  - `{"law": "vca", "part": "U1"}`: a Blackmer VCA's own gain from its control pins and card, (Ec− − Ec+) / mvdb.
    SSL 4000E dynamics: compressor and expander share the 2151, so the reading is the total.
  - `{"law": "table", "node": n, "table": [[v, dB], ...]}`: a gain cell's control voltage mapped through the cell's
    attenuation measured on this circuit. 1176: `ctl_c27` (the FET gate's control voltage) → the Q1 cell's
    attenuation; one curve holds at every ratio and input setting (the netlist has no meter circuit); the attack ripple of
    other settings moves the reading by up to ~1 dB.
  `"ref": "<node>"` reads node − ref. The result is smoothed over ~10 ms. test_circuit_unit checks each against the
  measured attenuation.

## 3. Solvers

### 3.1 Discretisation

- **Reactive elements** use the trapezoidal companion: a cap is conductance `2C/T` in parallel with a
  history current `s`, with the update `s' = 2G·v − s`. An inductor is `T/2L`, with `s' = 2G·v + s`. The
  trapezoidal rule is the bilinear transform: second order, A-stable, and with no amplitude error at the
  poles of interest at audio oversampled rates. It is also exactly the WDF one-port for C and L
  (port resistance `T/2C`, reflected wave = last incident wave, where the wave is `R·s`). That is why the
  two solver forms share state.
- **Stiffness.** Trapezoidal is not L-stable, so very fast poles ring at Nyquist.
  - Every reactive primitive carries a per-element integrator flag. Backward Euler (`Prim::be`: companion
    `C/T`, `s' = G·v`) is implemented and exposed for the op-amp macro's compensation node through the
    model parameter `be`.
  - Measured on the TS808: turning it on worsened the AC match (0.06 → 0.10 dB) and didn't change rail
    behaviour, so the library leaves it off.
  - Automatic stiff-pole detection (|z| > 0.98 near −1) is planned.
- **Local-truncation-error control (implemented, `CompileOptions::lteTol`, default 1; §10.16).** The step is
  fixed at the internal rate, but a power stage snapping into cutoff, grid conduction or flyback is an event
  faster than one sample, and the trapezoidal step under-resolves it.
  - **Estimate.** Each nonlinear DK group keeps its state variables (capacitor voltage, inductor current) for the
    last five samples. After the Newton solve it estimates the step's LTE, T³x'''/12, from the third difference of
    the pairwise mean (x[n] + x[n−1]) / 2. The pairwise mean is blind to a (−1)ⁿ component, so a stiff mode's
    persistent trapezoidal chatter at fs/2 does not read as error. Tolerance per state: lteTol·(10⁻³·|x| + 1 mV
    or 1 µA). The cost is a few flops per state per sample.
  - **Gate.** Over tolerance, the sample is substepped only if some device port's incremental conductance moved
    by more than 20 % over the step (one extra device sweep, only on those samples). Without that, the estimate is
    reading a linear mode ringing near fs/2 (an output transformer's leakage resonance after a pick attack), which
    the trapezoidal rule renders as designed (bilinear warping). Clean playing therefore never substeps.
  - **Substeps.** The sample is re-solved from the same start as m = 2 or 4 substeps (m from the estimate, which
    scales as h³; 8 cost 10–15 % more CPU dimed for under 0.1 % NRMSE), with the group's inputs and cut injections interpolated linearly across the step. The first
    substep is backward Euler and the rest trapezoidal, as SPICE restarts after a breakpoint (a netlist can choose a
    variable-step BDF2 start with trapezoidal continuation instead: `"lte": {"start": "bdf2"}`, §10.16). All-trapezoidal
    substeps were tried first and are unstable: switching step size with a lossless stiff mode present (the Twin's
    reverb driver flyback) pumped it until Newton failed. All-backward-Euler substeps work but damp: the JCM800
    dimed read 1.7 % NRMSE and −2 dB above 30 kHz, against 0.9 % and −0.3 dB with the backward-Euler start.
    States map between step sizes and integrators through each element's (v, i); a backward-Euler capacitor's
    current is kept by the substep loop.
  - **Inside an event.** After a substepped sample the same estimate is made on the finished solution. If a full
    step would again be over tolerance, the next sample goes straight to substeps instead of solving and rejecting
    a full step first. With that, the Twin dimed solves no more Newton iterations than the fixed step did.
  - **Runtimes.** Each such group holds its matrices at T/2 and T/4, trapezoidal and backward Euler, built at
    compile and refreshed lazily after a control change, like the Newton-failure step halving (which now shares
    the machinery). `CE_LTE=<scale>` overrides lteTol for A/B (0 = the fixed-step engine, bit-identical to it).
    Homotopy anchors inside the substeps are built only when a substep's direct Newton fails (bit-identical).
  - **Scope.** Groups of at most `CompileOptions::lteMaxPorts` (24) nonlinear ports. A substep re-solves the whole
    group, and on the DR103's 29-port group that cost ~100–110 % of a core per channel dimed against 73 % fixed
    step; its fixed step already matches ngspice dimed to 0.21 % (gate `dr103_dimed`).
  - **Default.** lteTol 1 in the engine (SPICE gate, tools). The live units and `circuit_cli alias` use 2
    (`kLiveLteTol`, Engine.hpp): it moves the aliasing gate's worst spur on the Plexi from −58.7 to −65.6 dB, at nearly
    the same accuracy. See §10.16 for the 1/2/3/5 table and the aliasing follow-up.
- **Regenerative loops (comparators, Schmitt triggers; ENGINE_QUEUE 25).** The op-amp macros have no dynamics inside a
  high-gain positive-feedback loop, so its unstable middle solution is a valid solution of every sample's equations.
  - **Start.** At the end of `reset()` each group's operating point is tested: an odd number of real eigenvalues of
    the loop matrix K·∂i/∂v above one flips the sign of det(K·∂i/∂v − I) away from (−1)^nn. Such a group's Newton
    start moves to the stable solution of the first sample (ports pushed in order, first the way the circuit's
    time-varying sources drive them at t = 0+; states and inputs are untouched). Relaxation LFOs (Dimension D,
    CE-1, Small Stone, Deluxe Memory Man, Electric Mistress) now start at t = 0, as in ngspice and in the circuit.
    Before, they sat at the DC point and crept off through their slow integrators: the Dimension D after ~1.8 s with
    fixed steps and not for many seconds with the LTE control, the CE-1 after 1.26 s, the Small Stone after 1.2 s.
  - **Switching.** In such a group a solution that jumps more than 0.5 V from the last one is accepted only if Newton,
    restarted from the last solution without the predictor, finds no nearby solution. Otherwise the jump was
    premature (Newton's start, a chord or the attack predictor's correction landed on the far branch while the near
    one still existed) and the near solution is kept. The switch is the first sample past the fold, whatever the
    solver path.

### 3.2 MNA per group

Each solve group has unknowns `v`: the voltages of free nodes, plus those of driven nodes owned by the group.
The group's equations are:

```
M v = Bu u + Bs s + Bi i(vp),       vp = Pv v + Pu u
```

where:
- `u` = external voltages: known sources, plus nodes owned by upstream groups;
- `s` = reactive states;
- `i` = nonlinear port currents;
- `vp` = nonlinear port controlling voltages.

KCL rows are written for free nodes. A driven node's row is its defining equation instead: `v = A·v_ctrl` for
a VCVS, `v+ − v− = 0` for a nullor. Its KCL row is dropped, because an ideal source supplies any current.

### 3.3 Nodal DK (Holters and Zölzer generalisation of Yeh's DK method)

With `X = M⁻¹`, everything below is precomputed whenever a part value changes:

```
p   = Dx s + Du u                       Dx = Pv X Bs,  Du = Pv X Bu + Pu
vp  = p + K i(vp)                       K  = Pv X Bi
[y; vc] = H [s; u; i]                   y = exported node voltages, vc = reactive branch voltages
s'  = 2G∘vc ∓ s
```

Per sample, the work is one small GEMV for `p`, a Newton solve of dimension `nn` (the group's nonlinear
ports), and one GEMV for outputs and states.

**Newton** (`newtonKernel<NN>` in `Engine.cpp`):
- **Formulation.** The unknown is `vp` and the residual is `F(vp) = p + K i(vp) − vp`, so `J = K·Jf − I`. `Jf`
  is block-diagonal by device, so J costs `nn²·(ports per device)`. Kernels are compiled for nn = 1…8, with
  stack arrays and closed-form 1×1 and 2×2 solves; a general kernel covers nn > 8.
- **Predictor.** Linear extrapolation `2·vp[n−1] − vp[n−2]`, then limited on junction ports by `pnjlim` against the
  last solution, as SPICE limits its predicted junction voltages (`limitPredicted`, 2026-11-11; also on the LTE
  substeps' start and on a cold start, from 0 V). Without it a diode that had just turned on extrapolated far up the
  exponential (the SVT's bridge: +290 V forward), and Newton needed 15-40 iterations to come back.
- **Limiting (damping).**
  - SPICE `pnjlim` on every junction port: diode, pair, BJT b-e and b-c, sign-aware for pnp.
  - **Current-driven junctions** (`kLimPnjF`, 2026-11-11): a single diode port on an island that no linear element
    anchors, not even through a capacitor (`NlDevice::floatingPorts`: a transformer winding floating between bridge
    diodes), has its voltage set by the junction currents alone. Stepping down from forward bias, voltage-space
    Newton approaches the small target current one nVt per iteration there, so a step of more than nVt/2 is taken
    in current: `v = vold + nVt·ln(1 + Δ/nVt)` (pnjlim's formula, the exact inverse of the law), or at least 8·nVt
    down when the linearised current is below −Is. A port whose limited steps reverse turns it off for the rest of
    the solve. Only the Newton path changes, never the fixed point.
  - A rail limiter on the op-amp clamp.
  - A **saturation limiter** on tanh ports: an iterate may not jump from one saturated side to the other
    beyond the linear region. Without it, a ±20 V square into the TS808 produced a 3-cycle limit cycle.
- **Lazy rail clamps.** Inside the op-amp swing, a rail clamp's current is exactly zero.
  - Every group with clamps gets a compile-time reduced kernel without the clamp ports. Each sample solves
    the reduced system, computes the clamp port voltages from it, and falls back to the full system only if
    a clamp would conduct.
  - The result is bit-identical. TS808 cost drops about 15 %, measured as cycles retired.
  - An earlier run-time variant, which solved the active subset with a variable-size solver, was slower than
    the full fixed-size solve and was removed.
- **Convergence.** The step is accepted when `|Δ| ≤ 1e-5·(1+|vp|)` per port and no limiter fired.
  Quadratic convergence leaves `O(κΔ²)` error, so the currents are updated to first order (`i += Jf·Δ`)
  instead of evaluating the devices again. Against ngspice the error budget is entirely discretisation:
  tolerances from 1e-4 to 1e-7 give identical NRMSE.
- **Homotopy.** The nonlinear equation depends on states and inputs only through `p`. If the direct solve
  hits the cap (default 32), the solver walks `p` from the last accepted sample's value in 4, then 16, steps
  with the same matrices. There is no state rollback and no rediscretisation. If that fails, the last
  accepted port voltages are kept and a failure is counted.
- **Measured.** Normal programme material needs 1.0–1.1 linear solves per group per sample and no homotopy.
  The abuse test (±20 V square into the TS808) needs homotopy on 6 % of samples and has 0 failures.
- **Bound.** The worst case per sample is `(1 + 4 + 16)·cap` kernel iterations per group.

**DC operating point.** At prepare time:
- caps are opened and inductors stamped as 1 kS;
- every free node gets `gmin = 1e-12 S`;
- groups are solved in topological order;
- each group runs Newton from `vp = 0`, then adaptive source stepping on `p` (λ from 1/16, doubling or halving) if the direct solve fails;
- if that fails too, gmin stepping (as ngspice): the group's DC matrices get a shunt of 10⁻² S on every node, which
  turns an open-loop integrator inside a servo into a well-conditioned stage. Each solution seeds the next, with the
  shunt relaxed a decade at a time (the step grows ×1.5 on success and halves on failure) down to the normal
  10⁻¹² S. A final Newton on the unmodified matrices polishes the answer, so it satisfies exactly the equations of
  the direct path. Every existing fixture solves before this stage, so none moves. The regression is
  `test-dc-ssl82e10` (ENGINE_QUEUE item 29): a 53-unknown, 22-port SSL 82E10 servo group that fails direct and
  source-stepped Newton. It now matches ngspice's `.op` on all 55 nodes (n_cv −0.23801 V, ngspice −0.238017;
  `ssl82e10_dc`). `CE_DEBUG_FAIL` reports which stage solved;
- states are then initialised so every capacitor current is zero.

So a unit starts at its quiescent point with no turn-on thump. That matters for the TS808's 4.5 V bias, and
even more for 300 V plates.

### 3.4 WDF (automatic tree)

A group is WDF-eligible when:
- it has exactly **one** nonlinear port and that port is a diode or antiparallel pair;
- its reactive elements are trapezoidal.

Linear groups use the state-space form, which is the same algebra and cheaper; WDF is used for them only if
forced with `"solver": "wdf"`. Controlled sources (nullor, VCVS, VCCS) are allowed: they live inside the
R-type core. External voltages (known and upstream nodes) enter the core's nodal analysis as known nodes, so
the core is `b = S·a + Su·u`.

Construction:

1. **Build the graph.** Every R, C and L primitive becomes an adapted leaf: R gives `b = 0`, C gives
   `b = a[n−1]` with `R = T/2C`, and L gives `b = −a[n−1]` with `R = 2L/T`. Their state equals the DK
   history current times R, so states convert both ways.
2. **Series-parallel reduction.** Parallel edges collapse into 3-port parallel adaptors. Degree-2 nodes
   collapse into series adaptors, except nodes pinned by ground, known or upstream voltages, or controlled
   sources. Child polarities are tracked. The cost per sample is O(leaves).
3. **R-type core.** The remaining composite edges, the root edge and the controlled sources form one R-type
   adaptor, whose scattering comes from nodal analysis of the core: each port is a Thévenin source `a_k`
   with series `R_k`, and `S = 2·∂v_ports/∂a − I` (Werner et al. 2015). The root port is adapted to the
   Thévenin resistance it sees (a 1 A test solve), so `S_rr = 0` and the root reflection is explicit.
   Pot changes re-adapt the tree and recompute S at control rate.
4. **Root.** It solves `v + R·i(v) = a`. The start is a warm start from the previous sample's extrapolated
   voltage. When the first step is larger than 0.25 V, the solver reseeds from the Wright-omega closed form
   on the conducting junction (Werner et al. 2015; D'Angelo et al. 2019) and polishes with Newton.
5. **Node voltages** for outputs and probes are core-node rows of the nodal solution, plus leaf voltages
   along a path found once at compile time.

Why bother, when DK covers this? Three reasons:
- O(n) per sample for long series-parallel passives such as RC ladders, passive EQs and multi-section filters,
  where DK is O(n²);
- an explicit root;
- **passivity under modulation**: a pot or LDR swept at audio rate can't inject energy.

On tiny groups DK is faster: 38 ns against 68 ns for the diode clipper. Auto-selection should therefore
compare cost estimates. That comparison is planned; today auto picks WDF for every diode-root group.

Multi-nonlinearity R-type roots (Werner's general case) are mathematically the DK solve of the same core. The
engine runs them in the Kirchhoff domain (DK), which avoids wave↔Kirchhoff conversion inside the Newton loop.
The cross-solver test checks that the WDF and DK forms agree on every WDF-eligible library group.

### 3.5 Exact automatic partitioning

1. **Classify nodes.**
   - *ground*;
   - *known*: driven by a grounded V source (DC rails, the input);
   - *driven*: the output of an ideal voltage-output primitive (VCVS, nullor, the macro op-amp's output
     buffer);
   - *free*: everything else.
2. **Find blocks.** Union-find over free nodes along every conductive primitive (R, C, L, and the injection
   terminals of nonlinear ports). Each driven node is its own block.
3. **Find dependencies.** Block A depends on block B if a primitive in A conducts to a driven node owned by
   B, or *senses* a node in B: VCCS and op-amp inputs, VCVS controls, nonlinear control terminals that are
   not injection terminals.
4. **Merge loops.** Tarjan SCC over the block graph. Each SCC is one **solve group**, and groups run in
   topological order. A driven node merged into a group becomes an unknown with its defining row. Elements
   with no free terminal can't change any voltage and are dropped. They are reported as "current only" in
   the model page, for supply-current readouts.
5. **Export.** Each group writes the node voltages that later groups, the output port or probes need into one
   shared node-voltage array.

The cut is exact: there is no delay and no approximation. On the TS808 (`ts808.circuit.json`) it yields
4 groups:
- input buffer (2 ports);
- op-amp 1 with its feedback network and the merged 1N914 pair (3 ports);
- tone network with op-amp 2 (2 ports);
- output buffer (2 ports).

The largest Newton dimension is 3, against 9 for the whole circuit. The AB763 preamp is one group, because
the plate drives the passive stack, which drives the second grid, with 4 ports.

**Exact small-signal response.** `Circuit::smallSignalResponse()` linearises every group's DK matrices at
the operating point and evaluates the discrete transfer function at `z = e^{jωT}`, group by group. It is
exactly what the engine plays, including trapezoidal warping. The AC gate and the component page use it.

### 3.6 Lookup tables and fixed-point precomputation

**Implemented for triodes.** The Koren law is separable: `Ip = A(vpk)·h(kp/μ + kp·vgk·C(vpk))`.
`KorenTables` (in `Devices.hpp`, cached per parameter set) holds cubic-Hermite tables:
- `h(z)` with its first and second derivatives;
- `A` and `C` over 0–800 V;
- the grid law on 3.4 mV steps;
- exact formulas outside the table ranges and below 4 V plate (where `v^1.4` isn't smooth).

The test gate is: worst current error 8e-8 relative, Jacobian error 1.5e-7, and the SPICE results don't
move. The tables are on by default (model parameter `lut`), and the AB763 costs about 15 % less.

- Device laws with no closed form, or with expensive transcendental functions (Koren triode `pow` and
  `log1p`), can be **tabulated per device instance** with Hermite interpolation, the way the existing
  `TubeDevice` tables are built, behind `"lut": true` in the model.
- A table is accepted only when the harness shows its error is below 1e-6 relative on the operating region
  and the Newton Jacobian stays consistent.
- A group whose nonlinearity depends on one scalar and has no state (a memoryless clipper behind a buffer)
  can be compiled to a **1-D static map**, the "K-method" table of Yeh. The compiler offers this only when it
  proves the group is memoryless.
- Pot-dependent matrices are recomputed, not tabulated. That is cheap at control rate (section 5).

### 3.7 Oversampling and anti-aliasing

- **Oversampling factor.** The netlist's `oversampling` applies at 44.1 and 48 kHz hosts and is halved at
  88.2 kHz and above (`CircuitUnit::onPrepare`).
- **Multirate partitions (implemented).** `"multirate": {"oversampled": ["IC1A"]}` runs only the groups
  holding those parts at 2×. Every other group runs at the host rate, with its companion conductances built
  for `T = 2/fs`.
  - `Circuit::processHost()` walks the groups in topological order.
  - A double-precision polyphase half-band (`Multirate.hpp`, Headroom's HIIR-style design, primed at the
    DC operating point) up-converts host-rate nodes read by oversampled groups. It down-converts in the
    opposite direction.
  - The boundaries are the exact one-way cuts, so no loop crosses a converter.
  - On the TS808 only the clipping op-amp partition is oversampled. Against the whole circuit at 2× (also
    one up and one down conversion) the difference is 0.07–0.12 % NRMSE on chords, and the cost is about
    −22 % (cycles).
- **Rate ladder (implemented, 2026-11-10; ENGINE_QUEUE 33).** Each group runs at a power-of-two multiple of the
  circuit's base rate (`CompileOptions::partLevels`, 1x to 32x), the rest at the base rate. Netlists name the parts
  whose groups alias: `"multirate": {"rates": {"VPA": 16}}` = that group at 16x a 44.1/48 kHz host (halved from
  88.2 kHz up); CircuitUnit turns it into levels above its own factor. `circuit_cli alias` (with `CE_LEVELS=REF=level`)
  measures a candidate exactly as tests/test_aliasing.cpp does.
  - **Schedule.** One base sample runs the groups in topological order, each 2^level times. A group's exports are
    kept per sub-sample, and a node read at another level goes through a double-precision half-band cascade, one
    stage per level, with the coefficient sets of the unit's CircuitOversampler stages (`HalfbandStage`). A group at 16x
    above a 4x unit therefore sees the same input filtering as a whole circuit at 16x. Rails and other known nodes
    are held over the base sample, with the mains evaluated at a faster group's sub-sample times. Cut currents
    count 1/2^level per sub-sample, so their owner sees the mean.
  - **Rules.**
    - A signal cut whose loads include a device port shares one level with its owner (no converter delay inside its
      lagged loop). Linear-load cuts (the Plexi's tone stack behind its cathode follower) may cross a level boundary.
    - Slow partitions stay at the base rate.
    - Circuits with CMOS logic cells and the 2x multirate mode do not use the ladder.
    - LTE substeps run only on base-rate groups (`lteMaxPorts` aside). The raised groups are already at the rate their
      aliasing needs: substeps cost the Plexi's power group +54 % at 16x for 0.6 dB of alias level.
  - **Choice of rates (measured).** In the Plexi and AC30 the power group (tone stack, PI, power stage, OT; 11 ports)
    makes the aliases. At a 4x base (with LTE), the measured alias levels were:

    | Configuration | Plexi | AC30 |
    |---|---|---|
    | Power group alone at 16x | −60.4 dB | −63.2 dB |
    | Whole circuit at 4x | −59.0 dB | −55.9 dB |
    | Whole circuit at 8x | −57.7 dB | −56.5 dB |
    | Whole circuit at 16x | −75.4 dB | −65.6 dB |
    | Power at 8x | −55.7 dB | −57.3 dB |
    | Preamp alone at 16x | −45.5 dB | |

    The power group at 16x it is (the Plexi variac likewise: −61.6 dB).
  - **Not done.** Automatic choice by measured spectral growth at compile time; the overrides above come from the
    `alias` measurements.
  - **Null test** (`circuit_cli alias`, `CE_ALIAS_DUMP`, delay-aligned). Every group at 16x on the ladder against the
    whole circuit at 16x (both without substeps):
    - harmonics H1–H7 agree within 0.26 dB (Plexi) and 0.11 dB (AC30);
    - the time-domain residual is −38 dB (Plexi) and −48 dB (AC30).

    What remains is the cut lag at the base rate, against one sub-sample in the whole circuit: the Plexi's tone-stack
    signal cut and the supply. The shipped configuration (preamp at 4x with LTE, power at 16x) differs from the whole
    circuit at 16x by −33 dB (Plexi) and −43 dB (AC30), harmonics within 0.6 dB below H7. The preamp's rate is most of
    that difference: whole-circuit 4x against 16x differs by −28 dB.
- **Planned.**
  - Quality modes (Eco ×1 / HQ ×2).
  - Antiderivative anti-aliasing for WDF diode roots (Parker, Zavalishin and Le Bivic 2016; Bilbao et al.
    2017).

### 3.8 Real-time guarantees

- **No allocation on the audio thread.** Every matrix, work vector and Newton buffer is sized at compile
  time (`GroupRt`). `process()`, `setControlTarget()`, matrix refresh during glides and
  `adoptStateFrom()` allocate nothing. Newton kernels use stack arrays. The one exception is a refresh scratch
  row for groups wider than 256 columns, which no library circuit has.
- **Bounded time.** Iteration caps are per group, with two homotopy passes, so per-sample work is bounded
  (section 3.3).
- **Denormals.** `ScopedFlushDenormals` (ARM FPCR.FZ or x86 MXCSR FTZ|DAZ, restored on exit) wraps every
  processed block. States also decay to the DC operating point rather than zero, and underflowing
  exponentials are cut at e⁻⁴⁰.
- **NaN containment.** Inputs are clamped to ±100 V and non-finite inputs read as 0. A non-finite Newton
  residual fails the step, which falls back to homotopy and then to the last accepted port voltages. A
  non-finite or wildly large (|v| ≥ 1 GV) export is written as 0.
- **No garbage accepted (2026-11-11).** A small Newton step is accepted only if the residual it leaves with the
  iterate's own device Jacobian, r = F + (K·Jd − I)·d, read as the step it still asks for (|r_k| over the row's
  Jacobian scale 1 + Σ_j |K_kj|·max|Jd_j|), is within one Newton tolerance newtonTol·(1 + |v|) at every port. It is
  checked whenever the iterate's own residual is above the tolerance. Converged solves across the library read at most
  7e-4 of that; the bad steps read 1e4 to 3e6. A raw volt residual would not do: near-floating nodes (K ~ 5e8 Ω) leave
  centivolts of rounding. A chord step that fails goes to a fresh factorisation; a sparse nodal step that fails
  (d = F + C·x cancels when |F| is huge) re-solves that iterate densely. Cause: the JC-120's chorus op-amp on a supply-cut rail, inside a group
  on LTE substeps, drove its rail clamp onto the 1e11 S linear extension; a stale chord, then a cancelled sparse
  step, "converged" with a 2e16 V residual and 1e11 A clamp current, which railDraws fed into the supply group and
  every group then failed (14 911 homotopies, 14 836 failures in 4800 samples). Now 32 homotopies, 0 failures, and
  the output equals the dense-only solve to 3e-9 V. A failed substep sequence restores the hysteresis-play and
  speaker-thermal states it advanced before handing the sample to the full step, and a sample that fails outright
  keeps the last value of any non-finite or ≥ 1 GV export or state instead of writing it.

### 3.9 Noise and hum (ENGINE_QUEUE 35)

**Switches.** `CompileOptions::noise` (thermal + shot), `::hum` (heater-cathode), `::noiseTempK` (300 K) and
`::noiseSeed`. Both default to off, so every SPICE gate, test and tool is bit-identical to before. CircuitUnit turns
them on with the model JSON `"noise"` switch, which also covers the BBD noise.
- **Seed.** `serial·2 + channel + 1 + instance·φ` (φ = 2⁶⁴/golden ratio), hashed (splitmix64) with the group index and
  the group's part refs.
- **instance.** Taken once per unit at construction: a process-wide counter mixed with a salt drawn at process start.
  Two units of the same netlist and serial therefore hiss independently, whether they are two nominal Twins in one rig
  or in two plug-in instances. The circuit itself is unchanged.
- **BBD noise.** The BBD's noise seed takes the same value, so stereo channels and instances get independent noise.
- **Tests.** `HEADROOM_NOISE_FIXED_SEED` sets instance = 0, giving reproducible streams.
  test_circuit_unit, test_param_drag and test_aliasing set it.
- **`circuit_cli`:** `CE_NOISE`, `CE_HUM`, `CE_NOISE_SEED`.

**Thermal noise.** Every netlist resistor, pot segment and rheostat carries a parallel current source of density
√(4kT/R). It enters as node current injections: one column per unknown node that a resistor touches.
- The response is the node-voltage vector z = M⁻¹·inj. Its columns come from the explicit inverse or the LU and are
  refreshed with the matrices, so pots move the noise with their value.
- z is kept over the unknowns that the exports, states and ports read. The exports and capacitor states take z as
  the bias of the per-sample H product. The ports take their voltage differences, plus K·(shot currents), as the
  bias of the Dp product.
- Pruning: a column adding less than 10⁻⁴ of the noise power to every export, state and port is dropped (each
  costs at most 0.02 dB).

**Shot noise.** 2q|I| from the current at the last solution, so it is modulated by the signal.
- Diodes, diode pairs and zeners.
- JFET gate leakage.
- BJT collector (2qIc) and base (2qIb) noise, mapped onto the b→e and b→c ports.
- Tube grid current.
- Tube plate: space-charge-reduced shot noise, through the equivalent noise resistance Req = 2.5/gm, i.e. a plate
  current noise of 4kT·2.5·gm. gm is the operating point's gm scaled by Child's law, gm ∝ Ip^(1/3); no device is
  evaluated per draw.
- The shot currents join the port currents seen by the rest of the group.
- Not modelled: BBD clock noise beyond `bbdNoise`; noise in WDF groups; op-amps at `level` 0 (the nullor).

**Op-amp noise.** For each `opamp` macro (level 1):
- **en** is a voltage noise in series with the input stage's sensing port. It is added to the port's predicted
  voltage; the port draws no current, so this is exact.
- **in** is a current noise at each input pin. It is injected by whichever group solves that pin's node, because a
  sense-only input usually sits in the driving network's group.
- **1/f.** Both have a 1/f corner: e² = en²·(1 + fce/f), i² = in²·(1 + fci/f).
- **Values** come from the model card (`en`, `in`, `fce`, `fci`), otherwise from a per-type table by part-name prefix
  (`opampNoiseDefaults`, ModelLibrary.hpp).

| Type | en at 1 kHz | in at 1 kHz | Source |
|---|---|---|---|
| TL07x | 18 nV/√Hz | 0.01 pA/√Hz | TI datasheet |
| TL06x | 42 nV/√Hz | 0.01 pA/√Hz | TI datasheet |
| LF353 | 16 nV/√Hz | 0.01 pA/√Hz | TI datasheet |
| NE5534A | 3.5 nV/√Hz | 0.4 pA/√Hz | TI datasheet. Corners fce 45 Hz and fci 450 Hz, derived from its 30 Hz points |
| NE5532 | 5 nV/√Hz | 0.7 pA/√Hz | TI datasheet |
| RC4558 / JRC4558 / TA75558 / M5218 | 8 nV/√Hz | 0.15 pA/√Hz | TI RC4558 datasheet |
| LM324 / LM2904 / LM358 | 35 nV/√Hz | assumed | TI datasheet (en only) |
| LM308, 741 / 1458 and unknown types | 20 nV/√Hz | 0.5 pA/√Hz | assumed (general-purpose bipolar class) |

Corners not marked as derived are assumed class values (100 Hz for JFET inputs, 50–200 Hz en and 300–2000 Hz in for
bipolar inputs). A pin's current noise below 1 % of its node's other noise, including the 1/f rise at 20 Hz, is
dropped (≤ 0.04 dB).

**1/f (flicker) noise.** A source with a corner gets a bank of 3–6 unit AR(1) sections, 0.75 decade apart, down to
2 Hz.
- The weights are fitted at compile time (non-negative least squares) so the total follows 1 + fc/f within 0.1 dB
  over 20 Hz–20 kHz.
- Each section takes its own Gaussian per draw.
- **Tube plate flicker:** the triode and pentode model parameter `fcf` (corner, Hz) sets it. It defaults to 0 (off),
  because no measured 12AX7 corner frequency was found to cite. Published 12AX7 measurements give only band totals,
  not a spectrum or a corner:
  - Blencowe's rule of thumb is about 1 µV input-referred.
  - HomeTheaterHifi measured 0.59–0.77 µV over 30 Hz–8 kHz with a 390 Ω source on three brands, and reports flicker
    as dominant. See §10.22.

  Treat any `fcf` value as a fitted one.

**Rate and spectrum.** A draw is made every `noiseDiv = ⌊fs/44.1 kHz⌋` samples (44.1–48 kHz) and held.
- Its variance is that of the band fu/2, so the DC level is exactly 4kT/R.
- The hold's droop |sin(dθ)/(d·sin θ)|² is cancelled on the draw sequence by a symmetric 7-tap FIR, fitted at
  compile time by least squares to 1/√droop over 0–20 kHz with C(0) = 1. The product is flat within 0.2 dB to
  20 kHz at d = 4 and d = 2.
- Gaussians: Irwin–Hall of four bytes (variance exact, tails at ±3.46σ), from a two-lane vectorised xoshiro256**,
  four per step. All per-draw arithmetic is in float.
- The per-draw product (2026-11-12, §10.34) has one column per kept source, its scale and both ends folded in, and
  skips the 4×4 blocks that add under 1e-10 of the power of every output reading them. `CE_NOISELEGACY=1`: per node.
- Samples re-solved as LTE substeps (§3.1) carry no noise. They occur under transients; on a rock DI they are
  7.5 % of the JCM800's group-samples (§10.34).

**Hum.** `hum` adds the heaters' effective AC, coupled to every tube cathode through the heater-cathode leakage
`rhk` ∥ `chk`.
- The AC is `hkv` V rms at the netlist's mains frequency and phase, taken from the 45–65 Hz source with the largest
  amplitude. The hum is therefore phase-locked to the supply ripple.
- One source node per (hkv, rhk·chk) class, because all heaters share a winding. A group gains at most one input.
- chk is folded into the source as V·(1 + jω·rhk·chk), with rhk stamped alone. The cathode sits at a few kΩ or less
  against |1/rhk + jω·chk|⁻¹ ≥ 100 MΩ, so this gives the same cathode current within 0.01 % without a capacitor state
  per tube.
- Netlists without a 45–65 Hz source (ideal DC supplies: the AB763 Normal channel, Butler Tube Driver, LA-2A) get no
  heater hum: there is no mains phase to lock it to.

**Defaults (tube model parameters `rhk`, `chk`, `hkv`): a typical healthy tube, not the worst case.**

| Parameter | Small triodes (12AX7, 12AT7, 12AU7) | Power pentodes (EL34, 6L6GC, EL84) |
|---|---|---|
| rhk | 1 GΩ | 100 MΩ (assumed: 10× the triode leakage for the larger cathode) |
| chk | 5 pF | 10 pF (assumed) |
| rhk·chk | 5 ms | 1 ms |

Sources for the triode defaults:
- **MIL acceptance:** the military 12AT7WA must read above 100 MΩ heater-cathode when new and above 50 MΩ after
  500 hours.
- **Measured batch:** none of 10 vintage 12AX7s measured below 100 MΩ (diyAudio, "Vhk(max) rating").
- **12AT7 population:** a measured population spans about 20 MΩ to 5–10 GΩ, with a log-centre near 0.4–1 GΩ (UK
  Vintage Radio forum).
- **Good valves:** reported as "several hundred MΩ to GΩ"; at that level, heater-cathode capacitance dominates the
  hum current.
- **chk:** the Tube Amp Doctor ECC83-WB sheet lists cathode-heater capacitance as "5.2". The unit is printed as nF,
  read as pF.

Other notes on the defaults:
- **Worst case is still reachable.** The 1962 test criterion (J. B. Dance, Radio Constructor) passes a small valve
  below 20 µA at its rated Vhk. The MIL / datasheet-class limit of 10 µA at 100 V is rhk = 10 MΩ. Set `rhk` (and
  `chk`) on the model card for a leaky tube; at 10 MΩ / 4 pF the hum is about 33 dB higher than the default.
- **hkv** defaults to 0.315 V rms. A centre-tapped (or humdinger-balanced) 6.3 V heater couples only through its
  imbalance; 5 % is assumed.

### 3.10 Start state of mains-fed supplies: settle, cold start, periodic steady state (2026-11-10)

`CompileOptions::settleMs` (the unit's 450 ms, `circuit_cli --settle`, default 600) chooses where `reset()` leaves a
circuit with mains sources; `settleMs = 0` is the DC operating point, which is what the SPICE `.op` gate compares.
- **Rectifiers fed straight from mains sources** (every amp before the SVT: the windings are ideal sources). The DC
  point holds the mains at its crest, so the rails start at the unloaded peak. `settleMs > 0` runs the mains for at
  least that long, until the supply cuts' per-period means stop moving (at most 2 s, or the value if longer), pins the
  cuts at those means and re-solves the other groups' DC (§10.6). Unchanged; every such netlist renders bit-identically.
- **A supply behind a power transformer** (a `transformer` winding on a mains source node: `Circuit::mainsCore`). A
  winding is a short at DC, so the DC point has every rail at 0 V (as ngspice's `.op` does), and the DC point with the
  mains held at its crest puts 85 A through the primary (core flux 29.7 V·s, deep in saturation). The old settle then
  re-solved the transformer's group at DC and handed back a crest charging state: SVT bpa 836 V, screens 243 V, 60 mA
  per tube against a settled 701 V / 202 V / 24.4 mA, whatever the duration (§10.24). Its slowest modes are also far too
  slow to run out: the core's DC flux decays with Lm/R_primary (40 H / 2 Ω = 20 s), the preamp rail behind 8.2k and 10k
  into 430 µF with about 4 s, a 1 µF coupling capacitor behind megohms with about 1 s (the SVT netlist as it was then;
  its supply was re-read from the sheet afterwards, §10.24). For these `settleMs > 0` starts
  the circuit at its **periodic steady state** with zero input (`settlePeriodic`; the value only switches it on):
  1. **Cold start** (`coldStart`): every node, capacitor, inductor and core flux at zero, sources at t = 0 (the mains at
     its crest), then 6 mains periods so the reservoirs charge. Above 160 kHz the same circuit first settles at
     fs/2^m (80–160 kHz) and its state is carried over by part and node name (companion states converted), so the
     full rate only confirms it (the SVT's unit rate, 176.4 kHz: about 0.9 s of CPU instead of 2.4 s).
  2. **Averaged operating point.** `mainsAveragedNetlist` cuts the mains side away at the rectifiers (everything reached
     from the mains source through transformers and passives) and drives each rectifier output (SVT: bpa, bscn, biasn)
     with a DC source (the rectifier outputs are exported whenever `settleMs > 0`, so the live unit measures them too).
     Each step measures the rectifier outputs' means over one mains period, solves that circuit's DC
     with its sources there, and moves every state the two circuits share (by part and node name, as
     `adoptStateByName`) by its DC value minus its mean over the period, so the ripple and the mains side keep their
     own state. Each mains-fed core's flux goes to zero mean: the primary sits across the mains, so it carries no DC, and
     bridges and centre-tapped secondaries draw no net DC ampere-turns (a half-wave secondary would leave a small offset
     that then relaxes with Lm/R). The rectifier outputs move with their new load, so after 4 periods it repeats, until
     the largest capacitor shift is below 1e-3 of the largest capacitor voltage (SVT: 5 steps, 453 V → 17.5 → 3.4 → 0.74
     → 0.29 V).
  3. **Reduced-rank extrapolation** of the engine's own mains-period map for what the averaged circuit misses (the
     ripple's effect on the means): from 10 snapshots one period apart, γ minimises |Σ γ_j (x_{j+1} − x_j)| over the
     capacitor and inductor state voltages (Σ γ_j = 1), the state jumps to Σ γ_j x_{j+1}; repeated (at most 4 times)
     while the jump exceeds 2e-4 of the largest capacitor voltage. One more period, and the phase goes back to t = 0, so
     the audio starts on the crest a cold start has.
  Noise is off while settling. The result is kept (`pss_`): a later `reset()` (controls set after `compile`) starts
  from it, and when its first averaged step moves nothing (below the 1e-3 threshold) it skips the extrapolation (2 mains
  periods in all); `CompileOptions::settleFrom` hands it, with the control positions it was settled at, to another
  circuit of the same structure and rate (CircuitUnit's second channel).
  `CE_PSS_DEBUG=1` prints every step (shift, rectifier means, jumps, time).
- **Cold start** (`CompileOptions::coldMs ≥ 0`, `circuit_cli --cold <ms>`): the same zero state, then that many ms of
  mains with zero input: a real duration, and the counterpart of ngspice's `.tran … uic`. Any circuit.

**SPICE gate for transformer-fed supplies.** `TranCase::start`: `""` is the DC point on both sides (all other cases);
`"cold"` switches both on cold (engine `coldMs` 0, ngspice `.tran … uic`) and compares the second half as usual;
`"settled"` starts the engine at its periodic steady state and ngspice at the same state (every node voltage as `.ic`,
every inductor current as `IC=`, `Circuit::inductorCurrents`, `circuit_cli spice --ic-settled`). `"settled"` is
implemented but not used: from the engine's sampled state ngspice's output stage takes a kick (OT core −3.3 → −21 V in
10 µs, then hundreds of ms to recover), so the window would compare the recovery. The settled operating point is gated
instead on the averaged circuit (the variant `mains_dc=<node>:<V>,…`, the circuit step 2 solves, with the rectifier
outputs at the steady state's means), where both sides start from an ordinary DC point.

## 4. Component model library

Every model is a C++ struct with `eval(vp) → (i, ∂i/∂vp)`, written once and shared by the DK solver, the WDF
root, the DC solver and the SPICE exporter, which emits the identical equations as ngspice B-sources or native
devices. Default parameters live in `src/circuit/ModelLibrary.hpp` with provenance.

| Family | Model | Status |
|---|---|---|
| R, C, L | Ideal. Non-ideal: C ESR, ESL and leakage; L winding R and parallel C; resistor value laws for LDR or tolerance | **v1 implemented** (ideal plus value laws); non-ideal options expand to primitives (Wave A) |
| Pot | 3-terminal, tapers A (log, 10 % at mid), B (lin), C (reverse log), W, custom `{mid}`; wiper end-resistance 1 Ω | **v1** |
| Switch | Ron/Roff, control-bound, selector groups | **v1** |
| Diode | Shockley `Is(e^{v/nVt}−1)` + series `Rs` (internal node) + gmin; models 1N914, 1N4148, 1N4001, 1N34A (Ge), BAT41, red, green and white LEDs; `Dpair` one-port for WDF roots; Zener (`bv`) in Wave A | **v1** |
| BJT | Ebers–Moll transport (`Is`, `βF`, `βR`), NPN and PNP. Gummel–Poon simplified (`VAF`, `IKF`, `RB`, junction caps `CJE`/`CJC` as explicit caps) in Wave B | **v1 EM** |
| JFET/MOSFET | Shichman–Hodges with `λ` (J201, 2N5457, 2SK30A), square-law MOSFET with subthreshold (BS170, 2N7000) | Wave B (fuzz, phaser and switching circuits) |
| Op-amp | `ideal`: nullor. `macro`: tanh-limited transconductance stage (slew = `I_max/C₁`), dominant-pole node `R₁‖C₁` (A₀, GBW), soft rail clamp referenced to the supply pins (output swing), ideal output buffer. Parameters for JRC4558, TL072, NE5532, LM308 (external comp cap), LM741, CA3130 | **v1 both levels** |
| Triode | Koren plate law (μ, ex, kG1, kP, kVB) with the contact-potential offset `vgo` (§4.1) and Headroom's grid-conduction law (smoothed Child–Langmuir, `G·s^1.5`, from `TubeDevice.hpp`), interelectrode caps Cgk, Cgp, Cpk as explicit primitives (Miller effect is emergent, not modelled). 12AX7, 12AT7, 12AY7 fitted to published curves (§4.1); 12AU7 Koren set | **v1**; `vgo` 2026-11-10 |
| Triode (alt) | Dempwolf–Zölzer 2011 (grid current with exponent and knee), Cohen–Hélie 2010 (fitted on measurements) as selectable laws | Wave A |
| Pentode / beam tetrode | Koren pentode law (`E1 = vg2/kp·softplus(kp(1/μ + vg1/vg2))`, `Ip = 2E1^ex/kG1·atan(vpk/kVB)`), screen `(vg1 + vg2/μ)^1.5/kG2` (smoothed), grid conduction law, interelectrode caps. 6L6GC, EL34, KT88 (Koren sets); 6V6GT, EL84, 6550 (circulated Koren-form sets, marked approximate) | **Wave A done**; SPICE: 6V6 SE ≤ 0.035 %, 6L6 PP ≤ 0.13 % |
| Rectifier | Vacuum diode `K·s^1.5`, perveance fitted to the datasheet drop: GZ34/5AR4, 5U4GB, 5Y3GT. Silicon is the ordinary diode. Mains via AC sources. Sag and ripple emerge from the circuit | **Wave A done**; SPICE: GZ34 supply ≤ 0.002 % |
| Transformer | N windings as branch-current VCVS from a core node (`v_ab = N·v_core`, MMF `ΣN·i` into the core). Flux node `λ' = v_core`. Magnetising law `i = (λsat/Lm)·sinh(λ/λsat)` (saturation; incremental L = Lm/cosh). Per-winding DCR and leakage. DC: `v_core = 0`, λ set by the MMF, so single-ended DC magnetisation works. Hysteresis (Jiles–Atherton, Holters and Zölzer 2016) and interwinding capacitance as a model option are next | **Wave A done** (saturation); SPICE ≤ 0.13 % including 25 Hz drive |
| Opto | Neon lamp (soft strike), light state (C with release R, drive ∝ lamp current), CdS LDR `G = 1/Rdark + (λ+ε)^γ/Ron` | **Wave A done**; SPICE 0.001 % |
| OTA | CA3080 and LM13700: `I_out = I_abc·tanh(v_d/2Vt)`, linearising diodes (LM13700), I_abc from a control node | Wave C (phasers, Dyna Comp, Mu-Tron) |
| LDR / opto | Vactrol and T4B: two-state (fast and slow) photoconductor memory, `R = R_dark·(…)`, asymmetric attack/release from the T4B measurements of the LA-2A literature; electroluminescent panel driven by the circuit's own current | Wave C (LA-2A, Uni-Vibe, Fender opto tremolo) |
| BBD | MN3005/MN3007/MN3207: clocked sample-and-hold chain as a fractional delay with anti-image and anti-alias filters from the circuit, clock from the circuit's VCO, NE570/571 compander as its own circuit (Raffel and Smith 2010; Holmes 2015) | Wave C (Memory Man, CE-1, chorus/flanger) |
| Tape | head-gap loss, spacing and thickness losses, Jiles–Atherton hysteresis via RK4 at 2× (Chowdhury 2019, "Real-time physical modelling for analog tape machines"), wow/flutter from the transport | Wave C (RE-201, Studer) |
| Spring tank | Behavioural block (`Blocks.hpp`) with electrical transducers: input coil R+L (the driver sees the real load), output Thévenin source plus 2575 Ω. Two springs: HP, dispersive allpass cascade, flight delay, lossy feedback loop (Välimäki, Parker and Abel 2010, simplified). It senses the coil current after each solve and drives the next sample (exact for a 30+ ms flight) | **Wave A done**; first arrival 32 ms, T60 2.7 s (4AB3C1B target 2.9 s) |
| CMOS logic | 4000B square-law channel (θ, λ, subthreshold κ) fitted to the TI IOL / IOH tables; unbuffered inverters as the channel pair; buffered gates and the 4013 as threshold cells with sub-sample crossings, VDD-dependent tpd and minBLEP-band-limited rail-to-rail outputs (§10.14) | **done**; SPICE ≤ 0.85 %, jitter −167 dBc |
| VCA / RMS | Blackmer log/antilog cell (four two-junction paths, internal amplifier, bias generator) with fitted symmetry and ohmic nonidealities; log-domain RMS detector (squarer, Q6 antilog into the external timing cap). THAT 2180A/B/C, dbx 202, THAT 2252 (§10.13) | **done**; SPICE ≤ 0.14 % |
| Speaker | Thiele–Small electrical equivalent (Re, Le, motional Res ∥ Lces ∥ Cmes) as `lib:` blocks: 12″ American, 12″ British 8/16 Ω, 10″ American (approximate datasheet-class values). The load sits on the OT secondary inside the solve, so amp/speaker interaction and NFB through the speaker are circuit-level | **Wave A done** |
| Speaker (nonlinear) + cabinet | `speaker` part: Bl(x), Kms(x), Le(x) polynomials, LR-2 coil, reluctance force, voice-coil thermal compression, cone velocity out; cabinets as `lib:cab-*` blocks (box air spring + acoustic path fitted to CC0 IRs), so a netlist can end in speaker + cab instead of an IR (§10.21) | **done**; SPICE ≤ 0.03 % (≤ 0.5 % at 3 kHz) |

### 4.1 Valve cards fitted to published curves (2026-11-10)

ngspice proves the solver, not the model. The valve cards are now least-squares fits to digitised plate
characteristics instead of Koren's 1996 sets or single-point fits.

**Law change: contact-potential offset.** The triode plate law is

`E1 = vpk/kp · softplus(kp·(1/μ + (vgk + vgo)/√(kvb + vpk²)))`, `Ip = 2·E1^ex/kG1`.

- `vgo` (card key, volts, default 0) shifts the grid voltage seen by the plate law only. The grid-conduction law
  keeps the raw `vgk`.
- With `vgo = 0` the law is Koren's exactly. Renders are bit-identical to the previous engine (CSV at 12 digits and
  WAV), and the engine and unit suites pass unchanged.
- The SPICE exporter writes `(V(g,k)+vgo)` only when a card sets `vgo`, so decks of other cards are unchanged
  (Netlist.cpp). Devices.hpp evaluates it in both the exact and the table path.
- Why it is needed: without it every triode fit drove `ex` below 1 to imitate the offset (the RCA 12AX7-A gives
  0.5 mA at 100 V / −1 V; Koren's set gives 0.10 mA). With it, `ex` and μ return to physical values
  (12AX7: ex 1.45, μ 98.7, vgo 0.61 V) and the low-current, low-plate-voltage region fits.
- Not adopted: the grid current taken out of the plate current (Ia = Ik − Ig). In the fitter it halves the 12AX7
  positive-grid error (7.0 → 3.3 %), but it is an evaluation-only option in `tube_fit.py` for now.

**Data.**
- Digitised from the manufacturers' published plate families with `tools/circuit/tube_digitise.py`, plus
  `tube_trace.py` / `tube_guided.py` (seed-and-track tracers for dashed or merged curves).
- Stored as values only in `work/tube-curves/<tube>/curves.csv`. Each folder has the digitising config with the
  citation (chart title, drawing number, date, scan URL) and `fit.json` (reference source, weights, bounds,
  sheet check points). No images.
- Sources (scans: frank.pocnet.net):
  - RCA 12AX7-A, 92CM-6879 (10-60)
  - RCA 12AT7, 92CM-7056 (1954)
  - RCA 12AY7, 92CM-7861 (1952), including Ec = +1 V
  - RCA 12BH7-A, 92CM-7742R1 (1955)
  - RCA 6L6GC, 92CM-10133 and 92CM-10327 (1960)
  - RCA 6V6-GTA, 92CM-4807R2, including Ec1 +5..+15 V and Ic1
  - RCA 6973, 92CM-9380 and 92CM-9389 (1957)
  - Philips EL34 (1969)
  - Philips EL84 (1969)
  - Philips Electronic Tube Handbook EF86 (1953–55 drawings)
  - RCA 6K6-GT, 92CM-4881R2 and 92CM-6311R1 (1948), 92CM-5209R2
- 12AX7 positive grid and grid current: the measured RSD-1 tube of Dempwolf and Zölzer, "A physically-motivated
  triode model for circuit simulations", DAFx-11, Fig. 6. That figure is vector: the markers are read exactly
  (`work/tube-curves/12AX7/dempwolf_extract.py`). The fitter gives this second source its own perveance scale and
  grid offset, so a single tube shapes the positive-grid region without moving the datasheet average.

**Fit.** `tools/circuit/tube_fit.py`:
- The engine laws term for term; scipy least squares on log-parameters.
- Residual tolerance 1 % of the reference's largest plate current plus 5 % of the point.
- The sheet's tabulated typical points can be anchored (3 % tolerance).
- Pentode knee weight 0.15 where the law's fixed atan knee would otherwise pull the plate region; 1.0 for EL34/EL84.

**RMS error by region, old card → new card.** Plate and screen errors are % of the reference's largest plate
current; grid-current errors are % of its largest Ig.

| Card | Regions | Notes |
|---|---|---|
| 12AX7 | Negative grid 5.3 → 1.9; Va < 50 V 8.5 → 0.9; positive grid 7.3 → 7.0 | Sheet point 250 V / −2 V: 1.24 mA (1.2), gm 1.54 (1.6), rp 63.5k (62.5) |
| 12AT7 | 6.9 → 2.2 | |
| 12AY7 | Negative grid 4.6 → 1.1; positive grid 4.4 → 1.8 | Now a library card; the 5F6-A and 5E3 used McLean's set locally |
| 12BH7A | 11.7 → 1.4 | Netlist-local (LA-2A) |
| 6L6GC | Plate 2.4 → 1.0; knee 20 → 15; screen 0.8 → 0.6 | |
| EL34 | Plate 5.5 → 1.8; knee 8.1 → 5.6; screen 0.3 → 0.4 | The sheet's table and curves disagree by 14 %; the card follows the curves |
| EL84 | Plate 4.8 → 2.2; knee 4.3 → 4.7; screen 0.2 → 0.15 | |
| 6973 | Plate 2.7 → 1.9; knee 29 → 22; screen 0.7 → 1.4 | Netlist-local (Supro) |
| EF86 | Plate 23 → 0.9; knee 25 → 3.3 | Class-A point 3.07 / 0.60 mA (sheet 3.0 / 0.6); netlist-local (AC15) |
| 6V6GT | Grid current 312 → 12 % (gscale 1.0e-3 → 1.79e-4) | Plate law kept: the law's fixed Ig2/Ia ratio cannot follow the sheet |
| 6K6GT | Plate 2.6 → 1.3; knee 9.5 → 4.5; screen 5.7 → 0.8; table 0.37; grid 4.7 % of 29 mA | New library card (2026-11-12): RCA 92CM-4881R2, 92CM-5209R2 (Va = Vg2 sweep), 92CM-6311R1 Ic1 plateaus; "old" = the 6G15's earlier inline table-only card. rp 309k vs sheet 90k (no plate slope in the law). Fender 6G15 driver |

- The grid laws of the other cards have no published data and are unchanged.
- A 12AX7 grid-law refit to the RSD-1 Ig (gknee 0.17) is parked: with it ngspice cannot regenerate the
  `plexi_drive` reference ("timestep too small").

**Bound amps.**
- Preamp plates move 6–17 V: the old 12AX7 set ran 10–20 % less current than the RCA average.
- DI nulls against the previous cards (3 s riff, 0.5 V peak, default controls) are −12 to −34 dB. The exception is
  the Tube Driver (−6 dB): its low-plate-voltage stages are the region where Koren's set was furthest off.
- `outputVoltsPerFS` was retuned where the shipped level moved by more than 1 dB: AB763 Normal, Twin, Supro, AC15.
- The AC15's EF86 stage was also rewired to the factory drawing (V-1-5). R14 22k is the supply dropper with C3,
  R15 220k is the plate load, R20 1M the screen dropper, and C8 the screen bypass. The drawing prints 90 V at the
  plate; the new card gives 111.6 V, the old card 249.8 V in the same wiring.

**Aliasing.**
- The new 12AX7 card raises the aliasing of the Plexi, Plexi variac and JCM800 by 1–3 dB (gate: −58.7, −58.9 and
  −58.7 dB against −61.5, −63.0 and −61.6 with Koren's set).
- This is not a defect of the law. `vgo` is a linear shift of the grid voltage, and the law stays smooth: the third
  derivative of Ip(vgk) is bounded and smaller than before.
- The cause is a sharper cut-off knee. At 300 V the knee width is q/kp = 0.32 V against 0.50 V, and the tail slope
  d ln Ia/dVg between 8 and 120 µA is 3.9/V against 2.6/V.
- The measured RSD-1 tube has 2.8/V there. The RCA curves stop at 0.1 mA and do not constrain the tail. Weighting
  the tail in the fit (tolerance 8 µA) only reaches 3.5/V and costs main-region accuracy (negative grid
  1.9 → 2.1 %).
- So the card is kept, and the rates of the three amps are re-picked with `circuit_cli autorate`.

**Hardware comparison tier** (`tools/circuit/hw_compare.py`, `tools/circuit/hw_nam_render.cpp`):
- Our unit and a NAM capture of the real amp get the same signal: a small-signal sweep; a 220 Hz level ladder
  giving the compression curve and H2..H5 corrected for each system's own linear response; a level-step burst;
  and a DI.
- Agreement is reported calibrated and with a ±5 dB level slack, sweeping the gain control.
- Public captures of these amps are mic'd amp + cab under non-redistributable licences. They are compared locally
  only (`work/hw-compare/captures-manifest.json` lists URLs and licences; no capture is in the tree).
- NAM captures hold only tens of milliseconds of memory, so they cannot test sag.

## 5. Performance

### 5.1 Budget (one Apple M-series performance core, 48 kHz host)

| Unit | Internal rate | Target | Measured (section 10) |
|---|---|---|---|
| TS808, macro op-amps, BJT buffers | 96 kHz | < 2 % of a core | 2.4–2.8 % (**over target**; see section 10) |
| TS808, ideal op-amps (Eco) | 96 kHz | < 1 % | not yet measured as a unit option |
| AB763 V1 + stack + volume + V2 | 96 kHz | < 3 % | 1.9–2.3 % |
| Full 3-stage preamp + LTP PI + push-pull + OT + speaker | 96 kHz | < 10 % | Wave A |

### 5.2 Strategies

- **Exact partitioning.** Newton cost grows as nn³, so splitting 9 into 4+2+2+... is a large win. Each group
  is also cache-resident.
- **Precomputed DK matrices.** The per-sample work is two small dense GEMVs over contiguous row-major
  storage, plus the fixed-size Newton kernel. Only nodes another group, the output or a probe needs are
  exported, which keeps the output GEMV small.
- **Device evaluation.**
  - Analytic Jacobians with reciprocals precomputed.
  - `limexp` returns 0 below e⁻⁴⁰, so a reverse junction costs a compare.
  - One `exp` per antiparallel pair (`e⁻ˣ = 1/eˣ`).
  - The rail clamp skips its exponentials inside the swing.
  - Optional tables are planned (section 3.6).
- **Parameter updates without stalls.**
  - Control values glide with a one-pole of about 15 ms, matching the drive pedal's existing 12 ms glide
    and the `SmoothingToolbox` time constants.
  - While a glide is active, the matrices of the affected group are recomputed every 32 internal samples,
    in place and allocation-free. For an n=10 group that is about 3 k flops per refresh, under 100 flops per
    sample.
  - Only groups containing a part bound to the moving control are refreshed. A pot change in the tone
    stage never touches the gain stage.
  - Variable capacitors keep their voltage continuous across a change (`s += ΔG·v_c`; WDF leaves:
    `z += ΔR·i`), so cap sweeps don't click. The tested drive glide has no step larger than the signal's
    own per-sample slope.
  - **Sherman–Morrison (implemented).** Groups with n ≥ 6 keep the explicit inverse X. A changed
    conductance is `ΔG·r·cᵀ` (KCL rows, unknown columns), plus an input term `q`, so
    `Z' = Z + (X r)·wᵀ` with `wᵀ = γ(qᵀ − cᵀZ)`, `γ = ΔG/(1 + ΔG·cᵀXr)`, and X is updated the same way.
    This costs O(n² + n·W) instead of O(n³ + n²·W).
    - Switches and every 64th update refactor fully.
    - Matches full refactoring to 3e-9 V of 31 V on the AB763 with every pot gliding.
- **State hand-off.** The server swaps in a prepared copy of a unit when knobs change
  (`adoptRuntimeStateFrom`).
  - Same structure hash: reactive states, Newton warm starts and the old control positions are adopted, and
    the new targets glide in. The test shows identical continuation (< 1e-9 V) and a first-sample step
    below the signal's own slope.
  - A different netlist (a part edit): `adoptStateByName()` matches elements by part ref, kind and
    terminal names. Each capacitor and inductor keeps both its voltage and its current
    (`s = G_new·v + i` for C, `i − G_new·v` for L). Devices keep their Newton warm starts, and nodes and
    controls carry by name.
    - Changing R7 on a running TS808: the first-sample step is 1.5e-5 V against a typical 4e-4 V.

## 6. User creation: the circuit editor on the component page

**The view.** Every circuit unit's model JSON has `kind: "circuit"`. It carries:
- the parts list with ids, types, values, models, nodes and edit paths;
- the controls;
- the groups with their solver and dimensions;
- the probes;
- the schematic.

`ui/assets/circuit-view.js` renders it, plugged into `component-detail.js` through one hook.

- **Schematic (v1, implemented).** An automatic left-to-right **net-bus layout**:
  - each net is a vertical bus placed by longest-path layering of the signal-flow graph, seeded with BFS
    depth from the input jack, where active devices point inputs → output;
  - series parts sit in lanes between buses, and shunts hang below their net;
  - rails (DC sources) are labelled flags, with ground symbols;
  - op-amps, BJTs and triodes are drawn at their output net;
  - `layout.columns` and `layout.rows` pin nets.

  Parts are tinted by solve group, and the legend shows each group's solver and port count.
  - Pots show their wiper position live, and it works both ways: turning a faceplate knob moves the wiper,
    and **dragging a pot on the schematic** (or its inspector slider) turns the knob through the page's
    own knob path, so it also auditions on the slot.
  - Switch parts have an Open/Close action.
  - Stored per-part coordinates are v2.
- **Probes (v1, implemented).** Click any net to scope it, up to five at a time. `curvesJson()` renders a
  private instance at the current settings, on a 250 mV, 330 Hz tone, and returns steady-state traces for
  every netlist node plus the DC operating point, which the schematic prints above each net. Live probing of
  the playing instance, through a lock-free ring buffer per slot, is planned.
- **Editing.**
  - Every part value is an `edit` path (`circuit.parts.R7.value`) in the existing overrides JSON (`__model`).
  - The existing customiser machinery (save as **My Components**, insert, audition) works unchanged.
  - Device swaps are selector edits over the model library (`circuit.parts.IC1A.model` → TL072, NE5532,
    LM308, ...; diodes → 1N914, 1N34A, LED, ...; tubes → 12AX7, 12AT7, 12AU7, ...).
  - **Topology edits (implemented, 2026-11-10).** In edit mode the schematic is a circuit editor: a parts
    palette (R, C, L, pot, diode, NPN, PNP, JFET, op-amp, triode, pentode, DC supply, with library defaults) is
    dragged onto the canvas (onto a net to connect the first pin); a pin is dragged onto a net, a pin or a rail
    to wire it, or onto empty canvas to disconnect it; the drawer edits every pin's net by name (a new name makes
    a new net); clicking a net's name renames it (a taken name merges the two nets); Delete removes the selected
    part; ⌘Z / ⌘⇧Z undo and redo. The edits are a diff in the overrides, `circuit.topology`, applied by
    `CircuitUnit.cpp` `applyTopology` before the value edits: `rename` {old: new} (simultaneous), then `remove`
    [ref], then `wire` {ref: [nets]} (an existing part's full pin list), then `add` {ref: {part JSON}}; probes,
    cuts, cut loads, slow nodes and multirate parts that named a vanished node or part are pruned, and the result
    is parsed and validated like any netlist. A preset or My Component stores only the diff, so factory revisions
    still reach untouched parts; an added part's value or model is edited inside its `add` entry. The model JSON
    carries `base_netlist` (the netlist before the topology edits; the editor draws its draft over it),
    `circuit.lint` (authoring lint of the edited circuit) and `circuit.model_library` (device models by kind).
    The page lints the draft live (unconnected pins block sending it; only-sensed nets and nets with no DC path
    are warned) and sends it once every pin is wired; it joins the overrides only if the engine accepts it.
    `test_circuit_unit` checks an added part's response, a deleted part, rejected edits, and that the edit path
    round-trips every bound unit's netlist bit for bit.
- **Safety: solver limits.** A user netlist is compiled off the audio thread, and an edit is applied only if
  it compiles. Errors name the offending part or node.
  - **Implemented:**
    - parse and type validation (arity, unknown models, model and part kind mismatch, unknown controls,
      schema gate);
    - floating nodes that are only sensed;
    - two ideal outputs on one node;
    - sources on driven nodes;
    - group size (nn ≤ 24);
    - DC convergence;
    - a malformed override leaves the last valid circuit in place.
    - a rejected edit also restores the stored model string, so presets, My Components and the page reading the slot
      get the circuit that is playing.
  - For user circuits (any `__model` edit), the rebuild is transactional. Both channel circuits must
    compile and converge at DC, a 50 ms full-scale smoke render must stay finite and under 1 kV, and the
    estimated cost must be under 2e5 flops per sample. Otherwise the previous circuit keeps running and
    the page shows why.
  - **Planned:** a DC-path-to-ground fix-it.
- **Import (implemented).** In edit mode, "Import SPICE…" reads a `.cir` file into the override
  `circuit.spice`, and the unit imports it with the engine's own `importSpice`. `circuit.netlist` takes a
  JSON netlist instead.
  - Imported controls inherit the host unit's knob aliases in order: the first `*@pot` answers to the
    host's first knob.
  - Import warnings are listed on the page.
  - Save as My Components keeps the user circuit.

## 7. Validation

Every model and every library circuit is checked offline against **ngspice**. ngspice is a dev-only tool,
installed with Homebrew and never shipped. Its core is BSD-licensed, and it is run as an external program.

**Pipeline** (`app/tests/test_circuit_engine.cpp`; `circuit_cli spice|render|ac` for manual work):

```
library/<unit>.circuit.json ── exportSpice() ──► deck (same equations: native D/Q, B-sources for triode/op-amp)
            │                                       │
            │                                       ▼  ngspice -b  (.tran with max step T/8, .options interp at T; .ac; .op)
            │                         tests/fixtures/circuit/<case>.{tran,ac,op}.txt   (frozen references)
            ▼                                       │
   Circuit at 96 kHz, same stimulus ───────────────►│
                                                    ▼
          compare: NRMSE per node, max error, H1..H5 per harmonic, AC magnitude, DC per node
```

- **Stimuli.**
  - 9 transient cases from 10 mV to 2 V, including drive and tone extremes, a low-frequency case, the
    overdriven AB763, and bright on;
  - 4 AC cases (`.ac` against the engine's exact small-signal response);
  - 2 DC operating-point cases.
- **Gates** (CTest `circuit_engine`):
  - transient NRMSE per node ≤ 0.5–3 % by case;
  - H2–H5 within 1.5 dB where above −60 dBc;
  - AC within 0.1 dB from 20 Hz to 5 kHz and 0.5 dB from 5 to 10 kHz, at the real 96 kHz rate, so this
    includes trapezoidal warping;
  - DC within 1 mV or 0.1 %.

  The test runs against **frozen references**, so CI needs no ngspice. With `HEADROOM_NGSPICE=1` and
  ngspice on PATH, the references are regenerated first.
- **Also in the CTest.**
  - WDF vs DK (≤ 1e-8 V);
  - diode-pair merge (≤ 1 µV);
  - rate convergence (96/192/384 kHz error ratio ≈ 5, second order);
  - control glide continuity;
  - state hand-off;
  - ±20 V square-wave abuse (finite, bounded, no Newton failures);
  - SPICE export/import round trip;
  - parser error reporting.
- **`circuit_unit` CTest.**
  - embedded library == JSON files;
  - factory flag on and off;
  - legacy parameter ids reach the pots;
  - part overrides;
  - state hand-off;
  - probes and DC;
  - Twin preamp and bright switch.
- **Guitar-DI proof** (`circuit_ab`): the same DI through ngspice (XSPICE `filesource`) and the engine.
- **Monte-Carlo (implemented, informative).** `circuit_cli tolerance <netlist>` (`CE_RUNS`, `CE_LEVEL`)
  varies every part with a `tolerance` uniformly. It reports mean, σ, min and p5/p95 of the small-signal
  response per octave and of the 1 kHz THD.
  - TS808, 200 units: σ is 0.4–0.6 dB per band and THD is 8.5 ± 0.2 % at 0.3 V.

## 8. Migration plan

**Rule.** The circuit implementation of unit X ships behind `HEADROOM_CIRCUIT_ENGINE` (all units) or
`HEADROOM_CIRCUIT_UNITS=15000,30000` (per unit) until it passes:
1. SPICE validation;
2. an A/B render report against the current model: Library and Showcase rigs that use the unit, loudness
   within 1 LU after the netlist's output calibration, spectra plotted;
3. review.

Then the flag flips per unit. Planned: the old behavioural class stays selectable as `"engine": "legacy"` in
the unit's `__model` for one release, so a user can A/B.

**Parameter compatibility.** `.rig` param ids are matched through the netlist's control aliases. Values keep
their 0..1 normalisation. When the hardware has fewer controls than the catalogue entry, the extras follow
the rule in section 2.5. Unit ids stay stable through `netlist.id`.

| Wave | Units | New models needed | Notes |
|---|---|---|---|
| **P2 (done)** | 15000 TS808; 30000 Twin (normal-channel preamp: V1, AB763 stack, volume, V2) | R C L, pots, diode, BJT-EM, op-amp macro, triode | proves both solvers and the partitioner |
| **A: tube amps** (UNIT_FIX_PLAN 1E, 2C, 3B) | Twin and Deluxe full (PI, 6L6 push-pull, OT, speaker, reverb driver and recovery, bias-vary tremolo); 5E3; 2204/2203/Plexi; 5150/SLO/Recto/Uberschall/Bogner/BE-100 cascades; AC30/AC15; Hiwatt; OR120; SVT | pentode/tetrode, transformer core, vacuum rectifier, LTP PI, speaker load, bias-modulated tremolo | the existing `src/dsp/circuit` triode/power work merges into this as library subcircuits |
| **B: drives** (2D, 3C, WP-9) | TS9DX; RAT (LM308 macro); Big Muff (4 BJT); Fuzz Face Si/Ge with guitar-impedance interaction; DS-1; HM-2 (post-clip Color Mix gyrators); MT-2; Jackhammer; SansAmp; Butler Tube Driver (starved 12AX7); Culture Vulture | JFET, Gummel–Poon extras, germanium temperature | uses WDF roots for the diode-to-ground clippers |
| **C: modulation and dynamics** (1A, 1D, 2A) | Cry Baby / Vox wah (inductor, BJT); Small Stone (OTA); Phase 90 (JFET); Uni-Vibe (lamp + LDR); Dyna Comp (CA3080); LA-2A (T4B); 1176 (FET); CE-1, Memory Man (BBD + compander); RE-201 (tape); spring tanks | OTA, LDR, BBD, tape, spring, FET-as-VCR | WDF passivity under modulation matters most here |
| **D: the rest** | Studio units that are DSP by nature (reverbs, pitch, digital delays) stay DSP. Their analog I/O stages (transformers, op-amp gain stages) can be prefixed as circuits | — | — |

**Regression for every wave:**
- `test_circuit_engine` (SPICE gate) for each new netlist;
- `unit_audit --units …` before and after;
- the A/B renders (`circuit_ab <out_dir> <di.wav>`): level-matched legacy/circuit WAV pairs, octave-band
  deltas and an ngspice-vs-engine render of the same DI, written to `work/circuit-proofs/` with `report.md`.
  Extending it to every Library and Showcase rig that uses a unit is part of each wave.

## 9. Prior art studied

- **chowdsp_wdf** (Chowdhury, BSD-3): C++ templated WDF with R-type adaptors and diode roots using
  Wright-omega. The adaptor equations and the omega approximation are textbook (Werner; D'Angelo). Headroom's
  WDF is a runtime-compiled tree, not templates, because user circuits are only known at runtime. No
  chowdsp code is copied, so no licence file is needed. If code is ever vendored, its BSD-3 licence goes
  in `third_party/chowdsp_wdf/LICENSE`.
- **ACME.jl** (Holters, MIT): the generalised DK method with automatic state-space derivation and solving of
  the nonlinear core via homotopy and parametric caching. Headroom's group-level DK follows its formulation
  (Holters and Zölzer 2015), reimplemented in C++ without copying code.
- **Holters and Zölzer**, "A generalized method for the derivation of non-linear state-space models from
  circuit schematics", EUSIPCO 2015; and "Circuit simulation with inductors and transformers based on the
  Jiles–Atherton model of magnetization", DAFx 2016.
- **D. T. Yeh**, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*, PhD
  thesis, Stanford (CCRMA), 2009; Yeh, Abel and Smith, "Automated physical modeling of nonlinear audio
  circuits for real-time audio effects", IEEE TASLP 2010 (the DK method); Yeh and Smith, "Discretization of
  the '59 Fender Bassman tone stack", DAFx 2006; Yeh, Abel and Smith, "Simplified, physically-informed
  models of distortion and overdrive guitar effects pedals", DAFx 2007 (TS808 and DS-1 analysis).
- **K. J. Werner**, *Virtual Analog Modeling of Audio Circuitry Using Wave Digital Filters*, PhD thesis,
  Stanford, 2016; Werner, Nangia, Smith and Abel, "Resolving wave digital filters with multiple/multiport
  nonlinearities", DAFx 2015; Werner, Nangia, Bernardini, Smith and Sarti, "An improved and generalized
  diode clipper model for wave digital filters", AES 139, 2015.
- **A. Fettweis**, "Wave digital filters: theory and practice", Proc. IEEE 1986.
- **D'Angelo, Gabrielli and Turchet**, "Fast approximation of the Lambert W function for virtual analog
  modelling", DAFx 2019.
- **Dempwolf and Zölzer**, "A physically-motivated triode model for circuit simulations", DAFx 2011;
  Dempwolf, Holters and Zölzer, "Discretization of parametric analog circuits for real-time simulations",
  DAFx 2010.
- **N. Koren**, "Improved vacuum tube models for SPICE simulations", Glass Audio 1996 / normankoren.com
  (triode and pentode equations and parameter sets).
- **Cohen and Hélie**, "Measures and parameter estimation of triodes, for the real-time simulation of a
  multi-stage guitar preamplifier", AES 129, 2010.
- **Macak**, *Real-time Digital Simulation of Guitar Amplifiers as Audio Effects*, PhD thesis, Brno 2012.
- **Pakarinen and Yeh**, "A review of digital techniques for modeling vacuum-tube guitar amplifiers",
  Computer Music Journal 2009.
- **J. Chowdhury**, "Real-time physical modelling for analog tape machines", DAFx 2019 (tape hysteresis);
  ChowTape (BSD-3), reference.
- **Välimäki, Parker and Abel**, "Parametric spring reverberation effect", JAES 2010; J. Parker, "Efficient
  dispersion generation structures for spring reverb emulation", EURASIP 2011.
- **Raffel and Smith**, "Practical modeling of bucket-brigade device circuits", DAFx 2010.
- **Parker, Zavalishin and Le Bivic**, "Reducing the aliasing of nonlinear waveshaping using continuous-time
  convolution", DAFx 2016; Bilbao, Esqueda, Parker and Välimäki, "Antiderivative antialiasing for
  memoryless nonlinearities", IEEE SPL 2017.
- **ElectroSmash** circuit analyses (TS808, RAT, Big Muff, Fuzz Face) for stage-by-stage descriptions. The
  site was unreachable from this machine during Phase 2, so the TS808 values here are cross-checked against
  the sources in section 11.
- **Geofex**, R. G. Keen, "The technology of the Tube Screamer".
- **NAM** (Neural Amp Modeler, MIT, already vendored): the user-facing sonic reference. A circuit unit and a
  NAM capture of the same hardware are compared by ear and by spectrum, not as a validation gate.
- **LiveSPICE** (D. Sparks, MIT): a real-time SPICE-like simulator for guitar circuits that compiles a
  netlist to code. It was studied only through its public documentation for its approach (it solves the
  full MNA with Newton, using symbolic compilation). No code was read.
- **Guitarix** (GPL): DK-generated pedal and amp modules. Reference for which circuits matter; no code was
  read or copied.

## 10. Results (Phase 2)

All numbers come from this machine (Apple M4 Pro) on 2026-11-08/09. Other builds were running at the same
time, with a load average of 13 to 17, so CPU figures are the minimum of 10 to 12 timed runs and are quoted as
ranges.

### 10.1 What is implemented

| Area | Files |
|---|---|
| Netlist model, JSON parser and validator, engineering values, tapers, value laws, overrides, SPICE export/import | `src/circuit/Netlist.{hpp,cpp}` |
| Device library with provenance | `src/circuit/ModelLibrary.hpp` |
| Device laws (diode, pair, BJT-EM, op-amp OTA + rail clamp, Koren triode with grid law), small dense LA | `src/circuit/Devices.hpp` |
| Compiler (expansion, node classes, exact partitioner), DK runtime, DC solver, controls/glide, hand-off, exact small-signal response | `src/circuit/Engine.{hpp,cpp}` |
| WDF realisation (SP tree + MNA R-type core + diode root) | `src/circuit/Wdf.{hpp,cpp}` |
| Rack unit, factory flag, model/curves JSON | `src/circuit/CircuitUnit.{hpp,cpp}`, one hook in `src/dsp/ComponentFactory.cpp` |
| Library netlists | `src/circuit/library/{ts808,ab763-normal,diode-clipper}.circuit.json`, embedded by `tools/circuit/embed_library.py` |
| Component page | `ui/assets/circuit-view.js`, one hook in `ui/assets/component-detail.js` |
| Tools | `tools/circuit/circuit_cli.cpp` (info, spice, import, render, ac, bench), `tools/circuit/circuit_ab.cpp` (A/B + SPICE DI proof, UI fixture dump) |
| Tests | `tests/test_circuit_engine.cpp` (CTest `circuit_engine`), `tests/test_circuit_unit.cpp` (`circuit_unit`), fixtures in `tests/fixtures/circuit/` |

How the units are wired:
- **15000 (TS808).** The full circuit runs as the unit.
- **30000 (Twin).** Since Wave A the unit runs the complete AB763 Twin Reverb netlist
  (`ab763-twin-reverb.circuit.json`): mains, rectifiers and filter chain, vibrato-channel preamp, reverb
  driver, transformer and tank, recovery and mixer, the roach vibrato, the long-tail phase inverter with the
  tail feedback, 4x 6L6GC, the output transformer and two speakers. No behavioural post section (§10.6).
- **Selection.** Both sit behind `HEADROOM_CIRCUIT_ENGINE=1` or `HEADROOM_CIRCUIT_UNITS=15000,30000`, and
  default to off.
- **Legacy parameters.** For 15000, `Dr`, `Tn` and `Vol` reach the pots, `Bs` reaches C3 (the more-bass
  mod, 47 nF at 0.5) and `Br` reaches C9 (the tone-range mod, about 220 nF at 0.5). `Cl` is stored only.
  For 30000, `Vol`, `Tr`, `Md`, `Bs` and `Br` (switch, > 0.5 = on) reach the circuit, and `Sg`, `Vr` and
  `Bia` reach the power section.

### 10.2 SPICE validation (CTest `circuit_engine`, 96 kHz internal rate)

| Case | Node | NRMSE vs ngspice | H2–H5 worst Δ |
|---|---|---|---|
| clipper 0.1 V / 2 V, 1 kHz (WDF) | out | 0.005 % / 0.115 % | 0.00 / 0.02 dB |
| TS808 10 mV | out, out1, out2 | 0.04 / 0.02 / 0.04 % | ≤ 0.02 dB |
| TS808 0.3 V | out, out1, out2 | 0.11 / 0.20 / 0.11 % | ≤ 0.10 dB |
| TS808 1 V, drive 1, tone 1 | out, out1, out2 | 0.31 / 0.74 / 0.31 % | ≤ 0.84 dB |
| TS808 0.5 V 200 Hz, drive 0, tone 0, level 1 | out, out1 | 0.003 / 0.002 % | 0.00 dB |
| AB763 50 mV | out, p1 | 0.02 / 0.002 % | 0.01 dB |
| AB763 2 V 400 Hz, volume 1 (both stages driven, grid current) | out, p1 | 0.05 / 0.001 % | 0.00 dB |
| AB763 bright on | out | 0.04 % | 0.01 dB |

- **AC vs ngspice `.ac`.** The worst error from 20 Hz to 5 kHz is 0.060 dB (TS808), 0.039 dB (TS808 tone 0)
  and 0.009 / 0.014 dB (AB763 default / scooped). From 5 to 10 kHz it is 0.28, 0.19, 0.05 and 0.04 dB.
  This is pure bilinear warping at 96 kHz: below 1.3 kHz the engine and ngspice agree to 0.002 dB.
- **DC operating point.** 23 TS808 nodes and 15 AB763 nodes agree within 0.4 % of the 1 mV tolerance.
  The TS808 bias is 4.500 V at the op-amp inputs and 3.328 V at the emitters; the 12AX7 plates sit at
  183.3 V and the cathodes at 1.301 V.
- **Guitar DI.** On 4 s of a GuitarSet DI (CC BY 4.0) through the TS808 at default settings, the engine
  (88.2 kHz) matches ngspice (XSPICE file source, same samples) to **0.064 % NRMSE**. The difference
  signal is −64 dB. ngspice ran at 3× slower than real time; the engine at about 4 % of real time.
- **Other CTest results.**
  - WDF vs DK: 2.3e-9 V, including a glided pot sweep.
  - Diode-pair merge: 1.5e-9 V.
  - Rate convergence: the 96/192 vs 384 kHz error ratio is 5.02 (second order).
  - Drive glide: no step above the signal's own per-sample slope.
  - Hand-off: identical continuation.
  - ±20 V square abuse: no Newton failures; homotopy rescues the steepest edges.

### 10.3 CPU (per unit, one core, 48 kHz host, 2× internal)

| Circuit | ns per internal sample | % of one core | Notes |
|---|---|---|---|
| TS808 full-rate (4 groups: 2+3+2+2 ports) | 248–292 before / about 205 with lazy clamps | 2.4–2.8 % before / about 2.0 % | first Phase-2 measurement / after lazy clamps |
| TS808 as shipped in the unit (lazy clamps + multirate) | about 160 equivalent | **about 1.5 %** | **meets the < 2 % target** (cycle counts; 2.3 % wall-clock in situ under load average 15, including prepare) |
| AB763 V1 + stack + V2 (1 group, 4 ports, 13 states) | 194–236, then about −15 % with triode tables | 1.9–2.3 %, then about 1.7–2.0 % | under the 3 % target |
| Diode clipper as WDF / as DK | 74–110 / 38–45 | 0.4–1.1 % | DK is cheaper on tiny groups; see section 3.4 |
| Trivial RC (per-sample floor) | 8 | 0.08 % | |

For comparison, inside `circuit_ab` (44.1 kHz host, including oversamplers and prepare):

| Unit | Legacy model | Circuit engine |
|---|---|---|
| TS808 | 0.1 % | about 4 % |
| Twin | 7–12 % | 3.5–4.3 %, including the same behavioural power section |

The circuit Twin is already cheaper than the current model.

**Where the TS808 time goes,** and the next steps toward 2 %:
- About 30 % is the op-amp macros' rail-clamp ports, which are inactive at normal levels. The fix is
  compile-time level of detail ("rail-free" op-amp when the closed-loop swing can't reach the rails) or the
  WDF-ideal Eco level.
- Multirate partitions (section 3.7) are estimated at −35 %.
- The input and output BJT buffers are two whole Newton groups for a near-unity follower.

### 10.4 A/B against the current models

`work/circuit-proofs/report.md`, produced by `circuit_ab` from a GuitarSet solo DI, contains level-matched
WAV pairs for 3 TS808 and 3 Twin settings, plus the SPICE-vs-engine DI renders.

**TS808.**
- At Level noon the circuit is 12–14 dB quieter than the legacy model. That is physical: a 100 kΩ A-taper
  pot passes about 10 % at noon.
- After level matching, the circuit has much more energy above 2 kHz: +3 to +15 dB at default and up to
  +21 dB at 8 kHz with max drive and dark tone. The legacy model's 2-pole post low-pass is far darker than
  the real circuit's first-order 723 Hz pole, tone stage and 51 pF feedback corner.
- Because flipping the default changes the voicing of 19 Library rigs, it needs a listening review and
  probably re-levelling of those rigs.

**Twin.**
- Clean and edge settings agree with the legacy model within about ±3 dB per octave band (the circuit is
  slightly darker).
- The bright-scooped setting differs more: the circuit has a deeper mid scoop and the real 120 pF bright cap.
- The circuit's raw level differs by 7–15 dB, so the output trim needs calibrating against the loudness
  calibration before the default flips.

### 10.5 Honest gaps

- **Foundations follow-up.** Since the first report, these are done: lazy clamps, multirate, Sherman–Morrison,
  FTZ, triode LUTs, subcircuits, adopt-by-name, Monte-Carlo, SPICE import in the UI, pot dragging and the
  user-circuit safety gate.
- **Next:** Wave A device models (pentode/tetrode, transformer core, vacuum rectifier, speaker load), then the
  amp netlists.
- **Auto-layout.** It is legible but dense around op-amp feedback loops. Hand layouts for library circuits
  would read better.
- **Op-amp rails.** These are a smooth exponential clamp: a macro, not a vendor model. The 2SC1815
  saturation current is approximate. Both are recorded in `ModelLibrary.hpp`.
- **AB763 B+.** The preamp B+ is an ideal 270 V node. Supply sag and the real PI and power stage are
  Wave A.

### 10.6 Wave A: the complete Twin Reverb (unit 30000, behind the flag)

**Netlist.** `ab763-twin-reverb.circuit.json`, 129 parts, from Robinette's AB763 Twin layout and the Fender
AB763 schematics (vibrato and phase-inverter wiring shared with the Deluxe Reverb drawing). Every assumed
value carries an `unverified:` note. Six solve groups: supply 2 ports, preamp + reverb driver 9, vibrato
oscillator + roach driver 5, light 1, reverb recovery + mixer 6, PI + power + OT + speakers 11.

**Settled DC (running, not crest)** vs the chart: B+ 460 / 451 / 440 / 410 V against 460 / 458 / 450 / 410 V;
bias −52 V; 6L6 plates 458 V. The HT winding resistance (30 Ω per half) is fitted to that chart.

**SPICE gate (CTest `circuit_engine`, the whole amp, 96 kHz internal, reverb return shorted).**

| Case | Nodes | NRMSE vs ngspice | H2–H5 worst Δ |
|---|---|---|---|
| `twin_clean` 20 mV 220 Hz | spk, p2b, mix, pa, bp1 | 0.16 / 0.016 / 0.16 / 0.16 / 0.048 % | ≤ 0.02 dB |
| `twin_drive` 0.3 V 110 Hz, volume 0.8 (power stage clipping, B+ sagging) | spk, pa, g6a | 0.14 / 0.16 / 0.09 % | ≤ 0.01 dB |
| `twin_ac` (`.ac`, with all cuts) | spk | 0.060 dB to 5 kHz, 0.099 dB to 10 kHz | |
| `twin_dc` (crest operating point) | 70 nodes | worst 1.2 % of the 1 mV / 0.1 % tolerance | |

**Function.** Reverb: a 50 ms burst leaves a tail −18 dB at 0.1–0.3 s decaying with T60 ≈ 3 s; the
footswitch removes it. Vibrato: 5.3 / 8 / 12 Hz at Speed 0.2 / 0.5 / 1, 16 / 13 / 10 dB deep at full
Intensity. Feedback is negative (−3.7 dB at 200 Hz; the other OT phasing oscillates). No Newton failures
from 5 mV to 2 V input with every control at its extremes.

**CPU.** 3.1 µs per internal sample (≈ 30 % of one core at 96 kHz under load average 10–12), 35–45 %
in `circuit_ab` including prepare, 65 % dimed (more Newton iterations). The legacy Twin is 6–11 %.
Fixed-size Newton kernels now cover 9–16 ports (−30 %). A multirate split with only the power group
oversampled saves another 20 % but runs the preamp at the host rate; it is available, not shipped.

**Physics found on the way.**
- **OT winding capacitance needs its loss.** With an ideal (lossless) leakage + core model, a lumped
  plate capacitance rings with the leakage (~95 kHz) and the tail feedback turns that into a sustained
  oscillation; with no capacitance, a tube cutting off throws sub-microsecond kilovolt flyback spikes. Both
  are in ngspice too. 470 pF + 2.2 kΩ from each plate to the centre tap (self-capacitance with its loss)
  removes both, and engine and ngspice then agree within 0.2 % at heavy drive.
- **Speakers.** The voice coil is now Le + (Le2 ∥ Re2) (semi-inductance), so the load turns resistive above a
  few kHz instead of rising as an ideal inductor.
- **Phase inverter.** The 0.1 µF on the second PI grid goes to the feedback node, not ground; with it to
  ground the tail feedback is almost common-mode and does nothing.

**Engine features added for Wave A (all in `src/circuit/`, all tested).**
- **Mains settle** (`CompileOptions::settleMs`, 450 ms in the unit): the crest operating point overstates
  B+ by ~30 V. `reset()` runs the mains in, averages each supply-cut node over the last cycle, re-solves
  every other group's operating point at those rails (coupling caps, cathodes, bias), then runs 60 ms more.
- **Step halving** (`Diagnostics::substeps`): each nonlinear group keeps a second runtime at T/2; when
  Newton and the homotopy both fail, the sample is re-solved as two half steps with the companion states
  mapped between the two sets. This removed the remaining failures at full volume with 1–2 V input.
- **Core hysteresis**: transformer `hc` (coercive MMF, A) and `hw` (play width, V·s): a Prandtl–Ishlinskii
  sum of three smooth play operators on top of the anhysteretic sinh law; play centres advance once per
  converged sample. Off at DC, not exported to SPICE (noted in the deck). CTest: loss per cycle appears and
  scales with `hc`; the anhysteretic core stays lossless.
- **Spring tank round trip**: forward and return lines per spring with half the dispersion each, so the
  first arrival is at the flight time and echoes follow every round trip (t, 3t, 5t …). CTest: nothing at 2t,
  the echo at 3t is 103 dB above it.
- **Exact small-signal response with cuts**: groups linearised with their cut-current inputs and swept to
  the fixed point per frequency (previously the cut loads were simply missing from `.ac`).
- **Multirate with spring tanks**: the block runs once per host sample when its drive group is at host rate.
- **Selective fixture regeneration**: `HEADROOM_NGSPICE_ONLY=id,id`.
- **Roach recalibrated**: `FENDER_ROACH` ≈ 3 kΩ at ~1 mA lamp current, 35 ms response.

### 10.7 Shared models and the CPU gate (in progress)

**Pentode screen law.** Koren's separate screen law, (vg1 + vg2/µ)^1.5/kg2, is zero at ordinary class-AB bias
(vg1 < −vg2/µ) while the plate still conducts. The screen now takes Q/kg2 of the plate law's own space current
Q = E1^ex, plus what the plate fails to collect below its knee, (2/kg1)·Q·(π/2 − atan(vpk/kvb)). So the cathode
current is independent of plate voltage, and screen current rises in the knee as in a real pentode. The engine
and the SPICE export implement it identically. Datasheet checks at Va = Vg2 = 250 V:

| Tube | Point | Model | Datasheet | Change |
|---|---|---|---|---|
| EL84 (refitted) | −7.3 V | Ia 47 mA, Ig2 5.4 mA, gm 11.4 mA/V | 48 mA, 5.5 mA, 11.3 mA/V | The old set gave 25 mA. |
| 6V6GT | −12.5 V | Ia 46 mA, Ig2 4.7 mA | 45 mA, 4.5 mA | kg2 is now 8000. |
| 6L6GC | −14 V | Ia 81 mA, Ig2 5.8 mA | 72 mA, 5 mA | kg2 is now 12000. |
| EL34 (unchanged) | −12.2 V | Ia 113 mA, Ig2 13 mA | 100 mA, 15 mA | |

All pentode fixtures were regenerated and pass. The Twin's settled B+ still matches the chart (460 / 450 / 439 / 408 V).

**CPU (Twin, one circuit, 48 kHz host).** Each step was checked against ngspice:

| Step | Default | Dimed (bench) | Twin vs ngspice (clean / drive) |
|---|---|---|---|
| Wave A as landed | 30.5 % | 33.8 % | 0.16 / 0.14 % |
| Newton tolerance 1e-5 → 1e-4 (error after a converged step is ~κΔ²; whole amp moves < 1e-7) | 24.9 % | | unchanged |
| Identical parallel speakers as one block (`"parallel": 2` on subcircuits; exact to 2e-12) | | | unchanged |
| Register-blocked two-lane gemv for the linear updates | 21.5 % | | unchanged |
| Block-structured Jacobian; fixed kernels to 16 ports; active-set (current-space) Newton that drops grids below cut-in and control-only ports | 18.4 % | | unchanged |
| Vibrato oscillator as a 12 kHz slow partition (`multirate.slow`) | 16.3 % | 20.1 % | 0.16 / 0.24 % (the held supply current of the oscillator costs 0.1 %) |
| Power stage only at 2×, the rest at the host rate (shipped) | **10.6 %** | **15.9 %** | **0.70 / 0.42 %** (85 µs latency) |

In `circuit_ab` on a real guitar DI, including prepare, the shipped Twin costs 14 % clean, 19–21 % edge or
bright, and **38 % fully dimed**. Dimed on a DI averages 12 Newton iterations per sample, with about 560 homotopy
rescues per second at pick attacks driving the power stage into flyback. That regime is the remaining gap to the
10 % gate.

**Tried and rejected.**
- Chord Newton (reusing the last factored Jacobian): it is unsafe unless the residual is already converged, and
  then it is rarely usable.
- Cutting inside the NFB loop (at the power grids or the feedback node): it diverges.
- Dropping the power-stage interelectrode capacitances: it changes the response by 0.2–0.8 dB.
- An adaptive predictor: it costs more iterations.

**Supporting fixes.**
- The multirate path averages an oversampled group's cut currents into host-rate rails (they were counted twice).
- Spring tanks run at the host rate even with an oversampled driver.
- Tube ports get damped Newton steps (5 V on grids, 60 V on plates).

**Dimed attack path (DI, volume 10; continuing).**
- The voltage-space Newton runs first. The active-set kernel is now its fallback before the homotopy: a better
  rescue than the homotopy, though as a primary path it needed more iterations.
- Tube step damping now applies only where the law bends: grids within 1 V of cut-in, plates and screens below
  80 V.
- The real-DI dimed render fell from 38 % to about 30 % of a core in `circuit_ab` (load average about 5.5).
- **Where the time goes.** The PI + power group runs at 3.5 iterations per sample, at about 0.5 µs per iteration,
  at 2×. The preamp (host rate) runs at 2.8 iterations. Traces show linear, not quadratic, convergence when a stage
  slides into cut-off. Examples are the reverb driver on huge dry signals, or a plate collapsing into flyback.
- **No help.**
  - Newton tolerance up to 1e-3: −5 %.
  - Fewer maximum iterations: more rescues.
  - Predicting currents through the known linear drive: +35 % iterations.
  - Exact triode laws instead of tables: same iterations, slower.
- **Next.**
  - A fixed-size kernel that solves the 11-port group's Newton in about half the cost: tabulated pentode laws, and
    a block LU that exploits Jd's device blocks.
  - Sparse linear updates.
  - Then re-measure the host-rate preamp check across the band.

### 10.8 CPU gate, round 2 (2026-11-09, new lead)

**Tooling.**
- `circuit_ab --cpu <tid> <di.wav> [param=value ...]` renders the circuit unit alone on a DI and prints % of one core
  (prepare included) and Newton diagnostics. `CE_RATE=48000` resamples the DI to another host rate.
- `CE_PROFILE=1` group lines now carry an iteration histogram: the share of solves needing 1 / 2 / 3 / 4–6 / 7–12 / >12
  iterations.
- `CE_KBENCH=1` on `circuit_cli render` times each group's Newton solve and device sweep on the last sample
  (`Circuit::benchNewton`), and prints the H / Dp sizes and sparsity.
- Measured split of one 11-port Twin power-group iteration: eval+F 85 ns, Jacobian build 97, LU 81, step 45, limiter 15,
  accept 24. The dense algebra is latency-bound, not flop-bound.

**Steps.** Twin DI (`work/circuit-proofs/wave-a-twin/00_di_input.wav`, 12 s, 44.1 kHz), cycles from `/usr/bin/time -l`
on `circuit_cli render` (`CE_MULTIRATE=1`). Every step was checked against the full SPICE gate, which is unchanged
(twin_clean spk 0.163 %, twin_drive 0.238 %, AC 0.060 dB, DC 0.013 of tolerance, multirate 0.70 / 0.42 %).

| Step | Clean (Gcycles) | Dimed (Gcycles) | Notes |
|---|---|---|---|
| Baseline (as handed over) | 9.17 | 15.05 | |
| Tabulated pentode (`evalPentodeLut`: P = A(vg2)·h(z) with the triode A and h tables, atan table, grid table) | 8.77 | 14.21 | worst error vs the exact law 2e-8 (current) / 3e-8 (Jacobian), CTest `testTriodeLut` |
| Low-rank (Woodbury) Newton step on the live device rows (exact: rows below 1e-9 after scaling by K's column) | 7.15 | 12.46 | identical iterates |
| Chord confirmation (reuse the factorisation for the confirming iteration when the previous step was within 300× tolerance; full Newton otherwise; off at DC) | 6.75 | 12.34 | |
| Contiguous live-K block; LU at compile-time sizes | 6.66 | 11.70 | |
| Tube limiter lands at the bend edge instead of walking 60 V per iteration; a port's limit halves when limited steps alternate | 6.84 | **10.48** | homotopies 8323 → 483, step halvings 23 → 0 |

**Unit, real DI, 44.1 kHz, prepare included (`circuit_ab --cpu 30000`).** 13.1 % clean (was 19 %) and 20.5–21.5 %
dimed (was 31 %). The gate is under 10 % clean and under 15 % dimed, so it is **not met yet**.

**Tried and rejected in this round.**
- Jacobian reuse across samples, with chord steps while the device Jacobian drifts by less than 5 %. A chord step leaves
  about ρ·|Δ| error, not Newton's C·|Δ|², and the tolerance is relative to the plate voltage, so twin_clean worsened
  0.16 → 0.61 %. With an error-estimate acceptance rule it is back to 0.164 %, but then saves nothing (7.09 vs
  7.12 Gcycles).
- A quadratic predictor: −3 % clean, +10 % dimed.
- The roach light group on the 12 kHz slow partition: −45 ns/sample, but twin_drive multirate worsened 0.42 → 0.58 %.

**Where the time is now (clean, per host sample).** Power group (2×) about 55 %, preamp group 20 %, reverb
recovery / mixer 13 %, supply 4 %, vibrato + light 7 %. About 30 % of each group's time is outside Newton: dense
H / Dp products at 85 % fill, so sparse storage will not pay.

**Preamp at the host rate, across the band (CTest `testTwinHostRateAc`).** The shipped multirate Twin's small-signal
response at the speaker uses each group's own rate: preamp, reverb and mixer at the host rate, the power stage at 2×. It
is compared with ngspice `.ac`.

| Host | to 2 kHz | 2–5 kHz | 5–10 kHz | 14.2 kHz | 15.9 kHz | 17.8 kHz | 20 kHz |
|---|---|---|---|---|---|---|---|
| 44.1 kHz | 0.07 dB | 0.08 dB | 0.07 dB | −1.4 dB | −2.8 dB | −6.0 dB | −14.8 dB |
| 48 kHz | 0.07 dB | 0.08 dB | 0.09 dB | −1.1 dB | −2.0 dB | −3.9 dB | −8.1 dB |

The top-octave loss is bilinear compression of the preamp's high-frequency roll-off toward Nyquist. The CTest gate is
0.1 / 0.5 / 1.5 dB to 2 / 5 / 10 kHz. Running the preamp at 2× removes the loss at about +20 % CPU. That is the
user's call: a guitar cab removes 20–40 dB up there anyway.

### 10.8b CPU gate, round 3: specialised kernels (2026-11-09)

Twin dimed, `circuit_cli` cycles (12 s DI); the SPICE gate is unchanged at every step.

| Step | Clean | Dimed |
|---|---|---|
| Round 2 end | 6.84 | 10.48 |
| Woodbury factor and step compiled per (ports, live rows), dispatched by table (`wbFactor<NN,NA>`, `wbStep<NN,NA>`); live columns of K cached per live-row mask (`KaCache`) | 6.08 | 9.35 |
| Gemv with 8 independent accumulators; double-buffered device Jacobian (no copy for chord steps) | about 5.9 | about 9.2 |
| Pivot-free elimination of I − Jd·K (dominant diagonal), falling back to partial pivoting when a pivot is under 0.1 of its row | about 5.75 | 8.44 |
| Fixed-length residual dot products vectorised (`#pragma clang fp reassociate` on that loop only) | 5.67 | 8.35 |

**Rejected.**
- A transposed-K residual with column axpys: slower.
- Retuning the chord gate: no effect.
- Other tube step limits: 5 V / 60 V is best.

**Audio-thread cost.** `circuit_ab --cpu` now separates prepare (compile, DC and the 450 ms mains settle: about
170 ms, on the control thread) from the render. Real DI, 44.1 kHz:

| Unit | Clean | Dimed |
|---|---|---|
| Twin | 9.0–9.6 % | 15.1–15.4 % |
| Deluxe | 9.6 % | 16.2 % |
| Bassman | 9.9 % | 12.3 % |
| Plexi | 12.2 % | 15.2 % |
| JCM800 | 9.8 % | 14.0 % |
| AC30 | 7.8 % | 11.7 % |

The Twin at 48 kHz is about 11.5 / 17 % including prepare.

**Gate.** It is met clean everywhere except the Plexi, and at the line dimed (Twin, Plexi) except the Deluxe
(16.2 %). Per a `sample` profile of the dimed Twin, the rest is:
- the Woodbury factor, about 20 %;
- device evaluation (triode / pentode tables), about 14 %;
- dense H / Dp products, about 10 %;
- the iteration count in knee transitions (the power group at 3.3 per sample);
- exponential walks in the reverb driver (preamp group).

### 10.9 Wave A: the five amps (behind the flag)

**Netlists.** The netlists drafted in `work/circuit-netlists/` are now in `src/circuit/library/`:
`ab763-deluxe-reverb`, `5f6a-bassman`, `marshall-1959-plexi`, `marshall-jcm800-2203` and `vox-ac30-tb`
(`.circuit.json`). Each got the output-transformer fix: 470 pF + 2.2 kΩ from each plate to the centre tap, replacing
the bare 470 pF to ground. Each runs multirate with only its power group at 2×; the Deluxe's tremolo oscillator
(`p5a`) sits on the 12 kHz slow partition.

**Bassman partition.** The preamp, PI and power stage were one 19-port group.
- A signal cut at the cathode-follower output `kcf` (loads `CT`, `RSL`: the tone stack seen from a ~600 Ω source)
  splits it into 8 + 11 ports.
- A first cut at the PI grid `g3a` (loads `RG3A`, `V3A`) passed at 96 kHz (0.50 % driven). In the shipped multirate
  configuration it failed (7.2 %): V3A's grid current crossed the rate boundary through the half-band converter's
  delay.
- **Rule:** never put a cut whose load is nonlinear on a rate boundary.

**Bindings.** `CircuitUnit.cpp` maps 172000, 62000, 34000, 56000 and 38000 to these netlists, behind
`HEADROOM_CIRCUIT_ENGINE=1` / `HEADROOM_CIRCUIT_UNITS`. No default is flipped. The control aliases cover every legacy
parameter except the generic `CASSt` / `Pwr` (as on the Twin).

**SPICE gate (CTest `circuit_engine`).** Whole amp, 96 kHz internal, Deluxe reverb return shorted. Transient tolerances
are 1 % clean (20 mV, 220 Hz) and 3 % driven (0.3 V, 110 Hz, volume 0.8).

| Amp | Clean spk / first plate / PI / power plate | Driven spk / pa | AC to 5 kHz / 5–10 kHz | DC (nodes, worst / tol) | Multirate (shipped) clean / driven |
|---|---|---|---|---|---|
| Deluxe Reverb | 0.172 / 0.018 / 0.173 / 0.172 % | 0.188 / 0.188 % | 0.016 / 0.016 dB | 71, 0.012 | 0.574 / 0.290 % |
| Bassman 5F6-A | 0.007 / 0.003 / 0.006 / 0.010 % | 0.078 / 0.087 % | 0.060 / 0.128 dB | 48, 0.011 | 0.431 / 0.339 % |
| Plexi 1959 | 0.009 / 0.037 / 0.010 / 0.012 % | 0.100 / 0.136 % | 0.013 / 0.037 dB | 54, 0.011 | 0.051 / 0.395 % |
| JCM800 2203 | 0.011 / 0.040 / 0.013 / 0.014 % | 0.260 / 0.280 % | 0.050 / 0.067 dB | 58, 0.011 | 0.069 / 0.564 % |
| AC30 Top Boost | 0.017 / 0.033 / 0.024 / 0.023 % | 0.056 / 0.062 % | 0.057 / 0.075 dB | 48, 0.018 | 0.260 / 0.561 % |

H2–H5 agree within 0.10 dB everywhere. From 20 mV to 0.5 V at full volume there are no Newton failures.

**A/B.** `work/circuit-proofs/{deluxe-reverb,bassman-5f6a,plexi-1959,jcm800-2203,ac30-tb}/`, each with `report.md`, WAV
pairs and a validation section. Command:
`circuit_ab <dir> work/circuit-proofs/wave-a-twin/00_di_input.wav 12 --only <tid>`.
- **Level.** `io.outputVoltsPerFS` is set to the mean RMS match gain across the A/B settings: Deluxe 94, Bassman 62,
  Plexi 548, JCM800 317, AC30 168 V/FS. Calibrating on the clean setting, as for the Twin, would leave the dimed Plexi
  23 dB below the legacy model, because the legacy Plexi is very quiet at low volume.
- **Tone.** As with the TS808 and the Twin, the circuits carry much more energy above 2 kHz than the legacy models:
  +3 to +21 dB at 8 kHz after level matching. The Plexi clean has a −17 dB band at 125 Hz against the legacy model,
  from the 1959's small coupling caps and bright network. These are listening-review items before any default flips.

**CPU (circuit_ab, 44.1 kHz, prepare included).**

| Amp | Clean | Edge / crunch | Dimed / lead |
|---|---|---|---|
| Deluxe | 13.7 % | 16.8 % (reverb on 14.8 %) | 24.0 % |
| Bassman | 12.2 % | 14.1 % | 17.6 % |
| Plexi | 16.3 % | 21.6 % | 21.7 % |
| JCM800 | 14.2 % | 18.0 % | 21.0 % |
| AC30 | 12.3 % | 14.7 % | 17.8 % |

Like the Twin, these are over the 10 % / 15 % gate (§10.8).

### 10.10 Wave B: devices and pedals

**Devices (`Devices.hpp`, `Engine.cpp`, SPICE export in `Netlist.cpp`; models in `ModelLibrary.hpp`).**
- **JFET** (`njf` / `pjf`, nodes `[drain, gate, source]`; `NlKind::Jfet`, 2 ports g→s / d→s).
  - The law is SPICE level 1: Shichman–Hodges with λ, B = 1, drain/source symmetric (inverse mode on vgd), and gate
    junctions igs / igd with gmin. P-channel negates every voltage and current; VTO keeps the SPICE sign.
  - `cgs` / `cgd` are linear caps. The gate port gets pnjlim.
  - It exports as a native ngspice `J` with an `NJF` / `PJF (LEVEL=1 … B=1)` model.
  - Library: J201 (the InterFET card; the onsemi card's VTO −3.95 V contradicts the datasheet and is not used),
    2N5457, 2N5458 (derived), MPF102, 2SK30A, J113, 2N5952.
  - CTest `test-jfet.circuit.json` covers a J201 common source, a 2N5458 follower in its triode region, a 2N5952 VCR
    with vds through zero, and a p-channel follower. Results: 0.010 / 0.004 % (0.05 V / 2 V with gate conduction),
    AC 0.000 dB, DC 0.001 of tolerance.
- **BJT extensions (Gummel–Poon subset, all off by default).**
  - `vaf`: Early effect through qb = 1/(1 − vbc/VAF).
  - `ise` / `ne` and `isc` / `nc`: non-ideal base currents.
  - `rb`: base resistance, as an internal node.
  - `cje` / `cjc`: linear caps.
  - **Germanium `icbo` / `vleak`:** collector–base leakage icbo·tanh(vbc/vleak). It is bounded, unlike ISC, whose
    forward branch would add milliamps of base current when a fuzz transistor saturates. β multiplies it into ICEO.
    The export writes the same law as a B-source behind RB.
  - Library: AC128 (card), NKT275 (derived; ICBO 2 µA assumed).
- **Linearly floating islands (the series-diode bug, fixed without bleeders).**
  - **Cause:** in a series diode string with Rs, the mid-node and the next junction's internal node form an island
    joined by a resistor, with no linear path to ground or a source. The DK matrix M was singular, the group went NaN
    and silent (Bluesbreaker: failures 3, output 0).
  - **Fix:** at compile time a union-find over R / C / L / windings finds islands that contain no ground, known or
    driven node. Every nonlinear two-terminal port touching one gets a conductance g0 = 1 nS stamped into M, and
    exactly g0·v subtracted from its law (`NlDevice::gShift`). The total current, and so the solution, is unchanged.
    A compile note names each split.
- **Op-amp supply pins on circuit nodes ("rail-node mode", automatic when a 5-pin op-amp's V+ or V− is not a grounded
  DC source).**
  - The swing node is clamped relative to the real rails: two one-sided `RailClamp` devices sensing x − V+ and
    x − V−. Their bands are fixed in port voltage, so the lazy-clamp kernel still applies.
  - The output is a push-pull Norton stage, g·(±v + √(v² + δ²))/2, injecting V+ → out and out → V−. Its open-loop
    output resistance is `rout` (default 100 Ω); the crossover overlap is the quiescent supply current `iq`
    (default 1 mA). Load current is drawn from the rails, so filtered rails and charge pumps sag.
  - The SPICE export writes the same B-sources. Op-amps on DC-source rails are unchanged.
- **DC robustness.**
  - The cut-interface DC estimate (`dcLoadG_`) no longer counts a resistor whose far node reaches only junctions,
    such as a diode's 0.65 Ω Rs on a bias rail. That estimate made the DS-1's unmerged-diode DC crawl and fail.
  - The SPICE export adds a `.nodeset` at mid-swing on each op-amp swing node. This is a convergence hint only:
    ngspice's operating point put the DS-1's follower on the spurious R1·Imax = 13 kV solution.

**Pedals (SPICE gate, 96 kHz).**

| Pedal (unit) | Small / hot transient | AC to 5 kHz / 5–10 kHz | DC | A/B (`work/circuit-proofs/…`) | CPU (circuit_ab) |
|---|---|---|---|---|---|
| Boss DS-1, DS1-A circuit (17000) | 0.21 / 0.78 % | 0.008 / 0.110 dB | 0.020 | `ds1/` | 8.6–9.4 % |
| Big Muff Pi 1971 (43000) | 0.61 / 0.53 % | 0.064 / 0.190 dB (gated at 192 kHz) | 0.036 | `big-muff-1971/` | 4.0–4.6 % |
| Fuzz Face Si (41000) | 0.07 / 0.30 % | 0.000 / 0.000 dB | 0.040 | `fuzz-face-si/` | 1.6 % |
| Fuzz Face Ge (166000) | 0.02 / 0.20 % | 0.000 / 0.000 dB | 0.012 | `fuzz-face-ge/` | 2.0 % |
| ProCo RAT 2 (42000), LM308 on its real R10 47 Ω / C11 100 µF rail (rail-node op-amp; D3 is a shunt protection diode) | 0.11 / 0.38 % (rail ripple 1.6 %) | 0.026 / 0.116 dB (gated at 192 kHz) | 0.002 | `rat2/` | 8.2–9.6 % |

- **Big Muff.** Four Miller-compensated stages fall at about 60 dB/decade above 1 kHz, so 96 kHz trapezoidal warping
  alone is 0.24 dB at 5 kHz and 0.9 dB at 10 kHz. The AC gate therefore runs at 192 kHz; the unit runs at 2× host.
- **Fuzz Faces.** Pedal outputs are physical volts (`outputVoltsPerFS` 1). The circuits sit 19–26 dB below the legacy
  models at volume 0.6, which is a listening-review item.
- **Ge Fuzz Face convergence.** It needs about 20–40 homotopy rescues per 0.4 s at 0.05–0.5 V input, but has no
  failures.
- **Bindings.** All four are bound behind the flag; no defaults are flipped.
- **DS-1 relabel (user decision: the DS1-A circuit).** The unit name is now "1994 Hard-Clipping Orange Distortion".
  - `ComponentCatalog.hpp`: one string. No catalog.json → hpp generator exists; `tools/library/refresh_catalog.py`
    builds catalog.json *from* the header.
  - catalog.json: regenerated with `refresh_catalog.py --offline`; the diff is that line only.
  - Art sources `work/illustrations/{meta,svg}/17000.*` (face text "1994 HARD-CLIPPING DISTORTION"), then
    `app/tools/sync_unit_art.py` (new hash f41fdf4cdf; index.js / index.json updated).
  - Parameter aliases are unchanged.
- **Rail-node op-amps: SPICE gate (`test-opamp-rails`).** A TL072 at gain 11 on a 47 Ω / 100 µF filtered rail, into
  a 1 kΩ + 1N914-pair clipper.
  - Small signal 0.015 %, AC 0.000 dB, DC 0.001 of tolerance.
  - Static clamp level at 20 Hz: 7.519 V in both engine and ngspice.
  - Hard into the rails (0.5 V, 300 Hz): op-amp output 0.61 %, rail 0.006 %.
  - **Open issue:** the diode-clipped node behind it differs by 2.3–3.8 %, non-monotonically from 96 kHz to 3 MHz.
    It is not stiffness (backward Euler changes little) and not slewing. It looks like a sensitive clamp/rail
    interaction; that node is left out of the gate until understood.
- **Unit test.** CTest `circuit_unit` now loops over every bound unit (`unitBindings()`):
  - builds it through the factory and plays a 0.1 V sine at 48 kHz;
  - requires finite, non-silent output and zero Newton failures;
  - requires control aliases for every legacy catalogue parameter except `CASSt` / `Pwr`. This found the TS808's
    `Lmp` (indicator lamp), now a `legacyOnly` control.
- **Klon and Bluesbreaker: netlists done, engine blocked.** `work/circuit-netlists/wave-b/` holds the netlists with
  their real supplies:
  - Klon: MAX1044 doubler and inverter as Thevenin sources (60 / 65 Ω, datasheet-typical, unverified), with D7 / D8.
  - Bluesbreaker: D5 series feed and C5 47 µF (C5 unverified).

  ngspice solves both exported decks: Klon rails 16.66 / −8.87 V; Bluesbreaker VCC 8.39 V, Vref 4.19 V. The engine's
  DC fails. Two limitations of rail-node mode v1 cause it:
  1. **No power-up path.** The output halves draw current from V+ whatever V+ is. A rail fed only through diodes
     (the Klon doubler) floats during source stepping, or runs away when cut.
  2. **Groups merge.** A non-ideal output no longer splits groups (15-port groups in both pedals).
- **Proposed redesign (next).** Keep the op-amp output an ideal VCVS (a driven node, so the exact partition is
  unchanged) and measure its load current the way cut-node consumer currents are measured. Inject it, split by sign,
  plus `iq`, into the V+ / V− rail nodes one sample late, exactly like supply cuts. The relative rail clamps stay as
  they are. Also scale the supply draw by a soft step in rail headroom, so a dead rail draws nothing. Then revalidate
  the RAT (its numbers above are with v1) and land the Klon and the Bluesbreaker.
- **No catalogue unit.** The Klon and the Bluesbreaker have none. Adding units (IDs, names, art) is the user's
  call.

### 10.10a Rail-node op-amps v2; Klon and Bluesbreaker landed (2026-11-09)

**Redesign (replaces v1's push-pull output stage).**
- In rail-node mode the op-amp output is again an ideal VCVS, so stages partition exactly as with DC rails.
- Its load current is measured like a cut node's consumer current: cut terms on the output node, excluding the VCVS
  itself.
- `Circuit::railDraws` (called from `finishCutSample` and from each DC cut iteration) draws that current from the
  rails one sample late: sourcing current plus `iq` from V+, sinking current plus `iq` into V−.
- Each draw is gated by a logistic of the rail headroom (centre 0.2 V, width 50 mV), so a dead or uncharged rail
  supplies nothing. That gives a physical power-up path.
- Rail pins must be cut nodes; compile says so if not. Their owner groups subscribe to the draws even with no other
  load.
- The relative rail clamps are unchanged. `rout` is no longer used.
- The SPICE export writes the same law with the VCVS branch current (`B…_dp` / `B…_dn` using `i(E…)`).
- Exported decks now carry `rshunt=1e12` (1 TΩ per node) so ngspice's DC stays regular on capacitor-isolated nodes.

**Results.**
- `test-opamp-rails`: the v1 open issue (2–4 % at the clipped node) is gone. Hard into the rails: clipped node
  0.035 %, op-amp output 0.004 %, rail 2.1 %.
- RAT 2: 0.096 / 0.162 %, smaller groups.

**Klon and Bluesbreaker** (netlists in `work/circuit-netlists/wave-b/`).
- **Klon:** cuts are `vb`, `v18`, `vn`, `vd7`, `cp`. Every node of the pump chain must be cut, or D8 becomes a "load"
  of `v18` and its partition holds only C21.
- **Klon DC:** rails 16.66 / −8.87 V, matching the ngspice run exactly.
- **Bluesbreaker DC:** VCC 8.389 V, Vref 4.195 V. It runs with its series diode strings and no bleeders.

| Unit | Small / hot | Rails | AC to 5 kHz / 5–10 kHz | DC |
|---|---|---|---|---|
| Transparent Charge-Pump Overdrive (Klon circuit), **179000** | 0.034 / 0.054 % | 3.8 / 3.9 % of millivolt ripple (one-sample draw lag; gated at 5 %) | 0.085 / 0.321 dB | 0.007 |
| British Blues-Breaker Overdrive (Bluesbreaker circuit), **180000** | 0.027 / 0.014 % (o2 0.019 %) | | 0.010 / 0.100 dB | 0.002 |

**New catalogue units.**
- Decision: 179000 and 180000, with no trademarks in either name field.
- `ComponentCatalog.hpp` gets two schema rows and parameter arrays (gain / treble / output and gain / tone / volume,
  plus CASSt / Pwr). The `test_main` count is now 168. catalog.json is regenerated (`refresh_catalog.py --offline`;
  its offline category reads "Utility" until the API half is refreshed by a running engine, which computes
  "Drive & Fuzz" from `EngineFamily::CircuitWaveshaper`).
- They are added to RackGraph's drive-table list.
- The bindings are `circuitOnly`: always the circuit, since there is no legacy model to flag.
- **No unit art yet** (work/illustrations has none for them).

**Quality option ("HQ").**
- `__model` JSON `{"quality":"hq"}` runs the whole circuit at the oversampled rate, preamp included, instead of the
  multirate split; `"standard"` is the default. modelJson reports `quality`. CTest `circuit_unit` checks it.
- Rule: HQ is the default for a unit whose output is not followed by a cab. The rack's
  `coupleSpeakerLoad` already calls `setSpeakerLoadProfile` on amps every block (invalid profile = no cab). The unit
  records that and applies HQ at its next prepare, since a recompile can't run on the audio thread.
- **Open (rack owner):** a control-thread re-prepare when the cab state changes, so the switch is immediate.

### 10.10b Wave C, first units (behind the flag)

Research netlists are in `work/circuit-netlists/wave-c/` (notes there), copied into
`src/circuit/library/`.

| Unit | SPICE (96 kHz) | AC (192 kHz) | DC | A/B | CPU |
|---|---|---|---|---|---|
| MXR Phase 90 script (75000), LFO running at full Speed | out 0.79 % | | 0.017 | `phase90-script/` | about 10 % |
| Dunlop Cry Baby GCB-95 (71000) | heel 0.18 % / toe 0.05 % | 0.022 / 0.042 dB | 0.019 | `crybaby-gcb95/` | 1.7 % |
| Vox V846 Clyde McCoy (72000) | heel 0.22 % / toe 0.05 % | 0.013 / 0.034 dB | 0.018 | `clyde-mccoy-v846/` | 0.8 % |

**Phase 90.**
- **LFO.** It is a real circuit: 0.13 / 1.2 / 10 Hz at Speed 0 / 0.5 / 1, and ngspice gives the same 10.0 Hz. Being
  free-running, its bias node drifts in phase against ngspice, so the gate is the audio output.
- **Unverified values.**
  - RSPD 4.7 kΩ is a stand-in for the speed pot's end resistance and the op-amp's current limit.
  - The bias trimmer is an internal control with no legacy alias.

**Wahs.** They sweep 421 Hz to 2.2 kHz (GCB-95) and 437 Hz to 2.2 kHz (V846), with Q from 14–19 at the heel to
2.5–4 at the toe.

**Unit 21000** (Cry Baby Wah) is not bound. Its eleven legacy tuning parameters (LBP, D, PMin … LMax) have no
counterpart in a real GCB-95, which is a product decision.

**Device research for the rest of Wave C** is in `work/circuit-netlists/wave-c/DEVICES.md`:
- OTA (CA3080 / LM13700) as a new exportable `OtaHalf` kind;
- BBD as a clock-edge sampling block;
- NE570 compander;
- light-source / cell split of `ldr`;
- a JFET `ksub` smoothing for the 1176;
- tape as a length-domain block;
- a 3-spring tank.

Its open questions need decisions:
- the CE-1 uses an MN3002 and the Memory Man a CD4047 clock;
- BBD noise defaults;
- tape v1 or v2;
- fits without published data (T4B, Uni-Vibe lamp);
- which Small Stone version unit 20000 is;
- whether to cite Holters & Parker (DAFx-18), whose authors list an affiliation with an audio-software company. It is a public
  paper, not that company's code.

**Engine features the netlists needed that were missing:**
- a zener diode (since landed, §10.15);
- pot end resistance (since landed, §10.15);
- op-amp output current limit (since landed, §10.15, fixed rails);
- inductor series resistance and core loss;
- a finer `ac` grid;
- per-instance JFET VTO spread.

### 10.12 HQ switch and Wave C devices (2026-11-09, fourth lead)

**HQ switch, immediate.**
- The cab-follow state is now control-thread state, computed structurally rather than reported from the audio
  thread:
  - `IComponent::cabFollows` / `setCabFollows` / `cabFollowNeedsPrepare`;
  - `RackGraph::syncCabFollow()` uses the same rule as `coupleSpeakerLoad`.
- `RealtimeAudioServer::publishRenderPlanLocked` calls it before every `renderSnapshot()`.
  - A circuit amp whose quality changes is re-prepared there, on a detached copy (`detachForEdit(n, cab)`) when a
    published plan shares it.
  - The plan then carries the new instance; its handoff is non-continuous, so it gets the usual 5 ms bridge.
  - The audio thread never compiles or allocates.
- `buildFromPreset` runs the pass too (offline renders, loudness).
- **Bug fixed on the way.** Every knob edit detaches a fresh component (copy-on-write), which used to lose the
  audio-thread `cabSeen_`, so HQ silently fell back to standard after any edit. `detachForEdit` now carries the
  state to the copy before `prepare`.
- CTest `circuit_unit` "rack HQ switch":
  - amp + matched cab → standard;
  - bypassing the cab → hq, re-prepared;
  - re-enabling it → standard, on a detached copy;
  - a knob edit keeps hq with no further re-prepare.

**New device kinds** (Devices.hpp; parts in Netlist / Engine; SPICE export of the same laws; models in
ModelLibrary).

| Part | Engine form | Library models |
|---|---|---|
| `ota` `[out, inp, inn, abc, V−, V+]` | abc junction = Diode (n·junctions); two `OtaHalf` (3 ports: vd, v_abc, headroom) = IABC·σ(±vd/Vt)·compliance·(1 + u/VA); input base current IABC·σ/bf | CA3080, CA3094 (+ `CA3094_Q` npn), LM13700 / LM13600 (2 junctions; `LM13700_DIODE`, `LM13700_Q`) |
| `lamp` `[a, b]` + `light` | `LampTh`: thermal state θ = T/T_hot (1 F), R = R_hot·θ^1.214, heat P/P_rated − (θ⁴ − θa⁴)/(1 − θa⁴) over τ0, Wien light exp(−Tλ/T_hot·(1/θ − 1)) | UNIVIBE_LAMP |
| `led` `[a, k]` + `light` | Diode + `DiodeLight` (Shockley current / iref into the light node) | VTL5C3_LED, NSL32_LED |
| `neon` `[a, b]` + `light` | Neon + LightDrive (\|i\| / iref) | NE2 |
| `cds` `[a, b]` + `light`, `couple` | `CdsRate` (carrier n, 1 F: dn/dt = kr/(1 + mr·h)·(c·λ − n\|n\|), dark leak as a resistor) + `CdsCell` (G = 1/Rdark + ((n+ε)^{2γ} − ε^{2γ})/Ron/(1 + mh·h)); optional history h (1 F, R = τh, VCCS from n) | VTL5C3_CELL, NSL32_CELL, UNIVIBE_LDR |
| JFET model `ksub` | vgst → κ·softplus(vgst/κ) (subthreshold tail, DEVICES.md §5.2); exported as a B-source channel + gate diodes | |
| `sub` `lib:ne570` `[rect_in, crect, dg_in, inv_in, r3, out, vcc, gnd]` | datasheet Figs. 2, 6, 9, 10: R1 → VREF summing node, rectifier `absi` (\|v\|/R1 into CRECT), R5 10 k → mirror junction Q8, ΔG `gaincell` IG·tanh(IIN/I1) (IG = 2·V_R5/R5, I1 140 µA) into INV IN, R3 / R4, op-amp NE570_OPAMP, VREF = GND + 1.8 V; 50 nA rectifier bias (A) | NE570, NE570_OPAMP, NE570_Q8 (`compander.blocks.json`) |
| `bbd` `[in, out1, out2, ctl]` + `clock` | block `BbdLine` (below); shell: sensed input, two sources behind `rout`, sensed clock control | MN3002, MN3005, MN3007, MN3207, SAD1024 |

- **Light nodes.**
  - A named node, 1 F, leak 1/τ_light (the first emitter's).
  - Emitters inject Φ/τ_light, so λ = 1 at full drive.
  - Several cells may read one node. The partitioner keeps the lamp, each cell's carrier state and the signal stages
    as separate groups (one-way coupling, solved in order in the same sample).
- **State ports and DC.**
  - State ports are written as "current leaving the state node", so the exact linear split (`gShift`) can anchor them
    at DC: the lamp's θ gets its radiation slope.
  - The floating-island split now also covers nodes that float **at DC**: a union-find without capacitors. Example: an
    OTA output whose only linear element is its integrating capacitor. That node had only gmin at DC, so its Newton
    was ill-conditioned (−36 mV against ngspice's −0.16 mV). Every existing fixture is unchanged.
- **ngspice hints.**
  - `SpiceExportOptions::nodeset` takes the engine's DC values for op-amp swing nodes (and the BBD source level). The
    test harness passes them when regenerating.
  - ngspice settled the NE570 expander's op-amp on a spurious −7953 V point with the generic mid-swing hint. With the
    engine's value it converges to the identical operating point.
  - A nodeset is released after the first DC phase, so ngspice's answer still satisfies its own equations.
- **Pitfall.** SPICE node names are case-insensitive. Light node `L1` and a lamp terminal `l1` used to merge in the
  deck. Since §10.19 the export renames the later one (`n_l1_c2`) and lint warns, but a case-only pair is still
  easy to misread, so avoid it.

**BBD block (`Blocks.hpp` `BbdLine`).**
- **Clock.** A phase accumulator advanced by f_clk(V_ctl)·T through the unit's piecewise-linear VCO table. The LFO
  stays circuit-level.
- **Input.** Bucket samples are taken at the exact sub-sample clock instants, by centred 6-point Lagrange
  interpolation (the input timeline runs two samples behind).
- **Buckets.** A FIFO of N/2. Then:
  - Poisson transfer-loss FIR (Weckler);
  - H2 term about the window centre, hard limit at the input window;
  - optional noise per bucket sample (`CompileOptions::bbdNoise`, on by default; tests off).
- **Output.**
  - The full-period hold steps half a clock period after the bucket leaves (Holters & Parker DAFx-18, cited for the
    output structure only).
  - Each step is band-limited: a Blackman-windowed sinc integral, ±8 samples, cut-off 0.45·fs.
  - Events are predicted up to the kernel half-width ahead with the current clock, so neither the kernel nor the
    one-sample block coupling adds latency.
- **DC.** The output is the input's DC map; `settleBbdDc` re-solves with the block outputs set until they agree.

**Validation.**

| Check | Result |
|---|---|
| `otaopt_small` / `_hot` (CA3080 VCA with IABC swept 70–440 µA at 3 Hz, LM13700 OTA-C follower, lamp → 2 cells, one with history, LED → VTL5C3 cell, 2N5457 VCR with `ksub` 0.2 V below VTO), vs ngspice | ≤ 0.076 % on every node (out, o2, c1–c4, light nodes); H2–H5 ≤ 0.01 dB |
| `otaopt_ac` / `otaopt_dc` | 0.006 dB / 0.39 of tolerance |
| `comp_small` / `_hot` (NE570 datasheet Fig. 7 expander + Fig. 8 compressor, 15 V) | ≤ 0.42 % (CRECT ripple nodes), ≤ 0.005 % outputs |
| `comp_ac` / `comp_dc` | 0.063 / 0.253 dB; 0.011 of tolerance. DC bias 3.00 V expander, 7.44 V compressor (datasheet formulas 3.0 / 7.4 V) |
| Expander gain law at 0.3 V, 1 kHz (measured from the render) | 0.27 (datasheet 2·R3·Vavg/(R1·R2·I1) = 0.273) |
| BBD, 40 kHz clock: gain / phase vs sinc(f/fc), delay N/(2fc) + 1/fc | 1 / 7 / 15 kHz: +0.000 / +0.001 / −0.020 dB, 0.000° |
| BBD transfer loss N·ε = 0.2 at 15 kHz | −2.966 dB (Weckler −2.965 dB) |
| BBD clock ramp 40 → 30 kHz/s | 995.838 Hz (tape equation 995.820 Hz) |
| BBD at the CE-1's 85 kHz clock, 96 kHz rate | folded image at 16 kHz −106 dB |
| EL panel (T4B_EL behind 4n7, 85 V peak, 700 Hz) → NSL-32 cell, in `otaopt_*` | light node 0.51 %, cell 0.017 % |
| Tape, head-1 echo delay at full / slowest speed | 69.01 / 177.34 ms (69.00 / 177.33) |
| Tape playback loss, 5.3 vs 2 kHz at full speed, the same wavelengths at 0.389× | −7.07 / −7.04 dB (gap·spacing·coating formula −6.90 dB) |

**More devices.**
- **`elpanel`** `[a, b]` + `light`, model T4B_EL:
  - Rs + Cp as real parts plus `ElLight`: k·|i|·exp(−b/√|v_Cp|), Alfrey–Taylor.
  - k is normalised at compile time so λ = 1 for `vref` rms at `fref`.
  - Its values are assumed: no T4B L(V, f) data is published.
- **`tape`** `[recA, recB, pb1, pb2, pb3]` + `control` (Repeat Rate), model RE201. Block `TapeLoop`:
  - **Spatial buffer:** 2 cells per engine sample at full speed; the heads read at fixed distances, so delay and pitch
    follow the speed exactly (speed-type).
  - **Record:** anhysteretic Langevin record with Chowdhury's tape constants (v1).
  - **Playback:** dΦ/dt through a 127-tap spatial loss FIR (gap sinc · spacing e^{−kd} · coating (1 − e^{−kδ})/kδ),
    so the cut-off scales with speed.
  - **Speed:** servo-slewed; wow 3.5 Hz, flutter 22 Hz and an Ornstein–Uhlenbeck drift.
  - **Shell:** record head R + L (current sensed); each playback head is a source behind R + L.
  - **Known limit:** the FIR truncates the shallow |k| cusp of the spacing and coating losses at long wavelengths
    (about −1 dB at λ = 380 µm is missing).
  - **Unsourced (A):** tape speed, head impedances, geometry, drive, gain, wow / flutter.
- **`spring`:** `delay3` > 0 makes a 3-spring tank (RE201_SPRING; Accutronics 8AB2D1A electrical data).

**UI metadata for the new kinds.** Parts carry their netlist family / package (Part::family, package);
`partLook` in CircuitUnit.cpp supplies defaults. See the unit-wiring step.


### 10.13 `vca` and `rms` devices; stereo input (2026-11-09)

The dbx 160VU (125000), the SSL G bus compressor (130000) and the SSL E channel dynamics (133000) all sit on the
dbx 200-series Blackmer VCA and an RMS detector, sold as sealed modules with no public schematic. Both are now
engine devices, modelled from the published transistor-level cells and fitted to the THAT datasheets. Fits and
their scripts: `work/vca-rms/fit/`.

**`vca` (Blackmer log/antilog cell).** Nodes in the THAT 2180 SIP order: `[in, ec+, ec-, sym, v-, gnd, v+, out]`.
The dbx 202 module's pins (DN127 Fig. 2: In, Ec, VEE, Sym, Gnd, Out, VCC) map as Ec → `ec-` and `ec+` → GND.
- **Structure** (2180 Fig. 5). An internal amplifier holds IN at GND by driving the log node `#x`. This is a nullor,
  plus an exact linear split (a VCCS of the idle transconductance, subtracted from the law), because its only
  feedback is through the junctions. The bias generator holds `#v3 = #x − Vb`. Four two-junction paths, each a
  transistor in series with a diode, use β → ∞, which cancels exactly when β is constant:
  - Q1/D1 (pnp, input): from `#x` to IN, base Ec+.
  - Q2/D2 (pnp, output): from `#x` to OUT, base Ec−.
  - Q3/D3 (npn, input): from IN to `#v3`, base Ec−.
  - Q4/D4 (npn, output): from OUT to `#v3`, base SYM (`rsym` to Ec+).
  - Each path obeys I = Is·(exp(v/2VT) − 1) on its own junction pair, behind an ohmic resistance.
  - So I_out = g·I_in, with g = exp((Ec+ − Ec−)/2VT). Hence 20·log10 g = (Ec+ − Ec−)/mvdb, with
    mvdb = 2VT·ln10/20 = 6.1 mV/dB at a 35 °C chip (2180 Eq. 2).
- **Nonidealities** (model parameters):
  - `vsym`: output-npn offset. It gives even harmonics, and a control feedthrough of about vsym/2VT·iq·√g at the output.
  - `rs`: ohmic drop per path. It gives odd harmonics that grow with |g − 1| and vanish at 0 dB.
  - `rd`: output-path ohmic asymmetry. It gives even harmonics that grow with level.
  - `iq`: idle current per path (20 µA).
  - `rc`: control-port resistance to GND (the 202's 203 Ω).
  - `icc`: quiescent supply current from V+ to V−.
- **SPICE export.** It is exactly the same circuit:
  - `E` 1e7 for the amplifier;
  - a `B` voltage source for the bias generator;
  - resistors for the ohmic drops;
  - a `B` current source per path.

**`rms` (log-domain RMS detector).** Nodes in the THAT 2252 SIP order: `[in, ibias, gnd, sym, v-, cap, out, v+]`.
- **Structure** (2252 Fig. 3).
  - OA1 holds IN at GND. Iin is read across an internal 1 kΩ at `#x`.
  - The rectifier and 2·log stage reduce to the squared current (`RmsSq`). The positive half passes the Q1/Q2
    mirror, ratio m = exp((V_sym − V_in − vsym)/VT).
  - The input-bias floor adds iflr².
  - Q6 (`RmsLog`) sources i = (Iin²·k²/IBIAS)·exp(((V_gnd + vofs) − V_cap)/VT) from V+ into the CAP pin.
  - IBIAS is the pin-2 current through `nb` = 3 junctions to V−. With 560 kΩ to GND, pin 2 sits at VEE + 2.1 V
    (datasheet).
  - OA3 makes OUT = V_cap − vofs.
- **Timing comes from the user's external CT and timing resistor, not from parameters.** The external CT with the
  timing current IT gives CT·dV/dt = i − IT. In the linear domain that is a first-order RC average of Iin², which
  yields:
  - the output Vout = 2VT·ln(Irms/Iin0), at 6.1 mV/dB;
  - Iin0 = √(IBIAS·IT)/2.9;
  - the time constant τ = CT·VT/IT;
  - a release that is a straight line of −IT/CT in V/s.
  These are the datasheet's own equations.
- **SPICE export.** The same B-sources, plus a native diode for pin 2.

**Library models** (`ModelLibrary.hpp`). The `vsym` / `rs` / `rd` values are least-squares fits of THD+N, with the
datasheet noise floor added, at Rin = Rout = 20 kΩ and 1 kHz.

| Model | Fitted / sourced values | Fit targets (datasheet typ) |
|---|---|---|
| THAT2180A | mvdb 6.1, iq 20 µA, vsym +30.9 µV, rs 0.383 Ω, rd 0.108 Ω, rsym 25 Ω, icc 2.4 mA | 0.005 % (0 dB, 0 dBV); 0.024 % at 10 Vrms (Fig. 10); 0.02 % at −15 dB/+10 dBV and +15 dB/−5 dBV |
| THAT2180B | vsym −29.0 µV, rs 0.542 Ω, rd 0.062 Ω | 0.010 % (Fig. 13: H2 −80.1, H4 −104 dBc); 0.032 % at 10 V; 0.03 % at ±15 dB |
| THAT2180C | vsym −104.1 µV, rs 0.376 Ω, rd 0.060 Ω | 0.030 %; 0.063 % at 10 V; 0.04 % at ±15 dB |
| DBX202 | mvdb 6.0, rc 203 Ω, rsym 51 Ω, vsym 7.4 µV, rs 0.076 Ω, rd 0 | THAT 202R (the documented Black Can drop-in): 0.003 % at 0 dB, 0.02 % at ±20 dB, IIN+IOUT 700 µA |
| THAT2252 | mvdb 6.1, kgeo 2.9, nb 3, vbias 2.1 V at 24 µA, vsym 0 (trimmed), iflr 16 nA, vofs 1.4 V, icc 1 mA | iflr from the 1 dB linearity error at 31.6 nA |

Notes on the models:
- The fit's 0.5 Vrms points were not fitted, so they are predictions. They land on Fig. 10: A 0.0045 %, B 0.0067 %,
  C 0.020 % (curves 0.0045 / 0.007 / 0.02 %).
- No dbx 202 Black Can distortion or port data is published, and nothing is published for the RMS 209 module.
- `iq` is the datasheet's 20 µA "idling current", read as per path (an interpretation).

**Results** (CTest `circuit_engine`, `testVcaRms` / `testStereoInput`, 96 kHz):

| Check | Result |
|---|---|
| Gain law, −60 … +40 dB, both ports (2180 Eq. 2) | worst 0.25 % of the dB value (datasheet linearity typ 0.5 %, max 2 %) |
| THD+N at the 12 table / Fig. 10 points, three grades | all within 7 % of typ; 2180B at 0 dB, 0 dBV: H2 −80.1, H4 −103.6 dBc (Fig. 13: −80.1 / −104) |
| Output offset, no signal, 2180A, 20 kΩ | 0.22 / 0.46 / 0.80 mV at 0 / +15 / +30 dB (typ 0.5 / 1 / 3, max 1 / 3 / 12) |
| `vca` against the same cell built from engine BJTs and diodes | 1.75e-4 % NRMSE; 12 against 4 Newton ports; β 100 instead of 1e8 moves the output 1.3e-5 % |
| RMS law, 1 µA–1 mA rms (Fig. 4 circuit) | equal to 2VT·ln(Irms/Iin0) to 0.05 dB; Iin0 = 4.51 µA = 45 mV at 10 kΩ (datasheet "0 dBr ≈ 43 mV") |
| True RMS | sine and square of equal rms 0.000 dB apart; equal peaks 3.01 dB |
| Ripple at 100 Hz, τ 35.5 ms | 419.4 µV rms (datasheet formula VT/(4π√2·f·τ): 419.5 µV) |
| Tone burst, +30 dB, 10 µF (Fig. 5) | attack within 0.034 dB of the mean-square exponential; release −0.746 V/s against IT/CT 0.749 V/s (−122 dB/s) |
| SPICE gate `vcarms_small` / `_hot` (linked stereo pair, R fed its own sine) | out / outr ≤ 0.001 %, ctl / cap ≤ 0.14 % (µV ripple), H2–H5 ≤ 0.07 dB |
| `vcarms_ac` / `vcarms_dc` | 0.000 dB; 0.003 of tolerance |

**What was not modelled**, and why:
- **Noise.** The 2180's −98 dBV is shot noise in the class-AB core. The engine has no noise sources outside the BBD.
- **The internal amplifiers' finite bandwidth.** This covers the 2180's high-frequency distortion with Rin below
  10 kΩ, and the 2252's high-frequency error at low levels (1 dB at 4 kHz for 100 nA).
- **The 2252's crest-factor error.** The model is ideal here: the datasheet's 1 dB at crest factor 8 depends on
  internal current limits it does not publish.
- **The cell's hard clipping.** The NPN side current is limited to Icell, about 1.8 mA. That is above the
  datasheet's 1 mA peak recommendation.

**Discrete or compact.** The brief preferred discrete BJTs where they are feasible and accurate. They are accurate:
the compact device is the transistor cell at β → ∞, and β cancels. They are not practical:
- **Ports.** The discrete cell needs 12 Newton ports instead of 4.
- **Conditioning.** Its log-amplifier loop is badly conditioned. It needs a 1e-10 Newton tolerance to agree with
  the compact device to 1e-4 %, and at the default tolerance it is off by 0.07–0.6 %.
- **Topology.** It cannot use an ideal amplifier at all. A nullor whose only feedback is through junctions leaves
  the linear matrix singular, and the engine then returns zeros. Any hand-built transistor log amp has the same
  limit.

**Stereo input (`io.inputR`).** A second driven input, handled like `io.input` everywhere:
- an implicit known source, with `sourceImpedance` applied to both inputs;
- multirate up-conversion;
- the audio-path rule (a group that reads either input is never slowed);
- the small-signal response, driven common-mode;
- SPICE export as a second source `VINR`, using `SpiceExportOptions::inputSourceR` or, by default, the left
  stimulus.

API and tools:
- **API.** `Circuit::setInputR(v)` before `process` / `processHost`, or the two-argument forms.
- **CircuitUnit.** A netlist with `inputR` or `outputR` runs as one instance. With `inputR`, L drives `io.input`
  and R drives `io.inputR`, each through its own up-sampler; a mono host feeds both. Without `outputR`, both sides
  play `io.output`.
- **`circuit_cli render`.** It takes `--sineR f amp`, `--wavR` or `--csvR`, and the right input defaults to the
  left signal. For `outputR` netlists, the right output is the CSV column after the left one.

Tests (`testStereoInput`):
- independent L/R drive is exact against the closed-form solution;
- `inputR` round-trips through JSON;
- the export carries `VINR`;
- `inputR` equal to `io.input` or to ground is rejected;
- multirate: with the right VCA and I/V stage at 2×, R matches the whole circuit at 2× to 0.0013 %, and L matches
  the whole circuit at 1× to 0.0000 %.

The demo `test-vca-rms.circuit.json` is a linked stereo compressor: two 2180Bs under one 2252 whose virtual-ground
input sums both channels.

### 10.14 CMOS 4000B logic: `dff`, `nand`, `inv` (2026-11-10)

Clocked logic blocked the Boss OC-2 (35000) and every CMOS-divider octaver, and a `switch` is bound to a control,
not a node. The engine now has node-level CMOS parts. Code:
- `Logic.hpp`: the cells and the minBLEP table;
- `Devices.hpp`: `CmosOut` and `MosHalf`;
- `Engine.cpp`: the parts, the schedule and the DC settle;
- `Netlist.cpp`: the SPICE export.

**Parts** (supply pins are real nodes):

| Part | Nodes | Models |
|---|---|---|
| `dff` (one 4013 section) | `[d, clk, set, reset, q, qb, vdd, vss]` | CD4013B |
| `jkff` (one 4027 section) | `[j, k, clk, set, reset, q, qb, vdd, vss]` | CD4027B |
| `nand` (one 4011 gate) | `[a, b, y, vdd, vss]` | CD4011B |
| `inv` | `[a, y, vdd, vss]` | CD4069UB and CD4049UB (unbuffered), CMOS_INV (buffered) |

**Channel law.** One law, `cmosId`, serves every MOSFET. It is a square law with:
- mobility degradation θ;
- channel-length modulation λ;
- a softplus subthreshold tail κ = 50 mV;
- source/drain swap for negative vds.

Fitted parameters: β 1.215 mA/V², Vt 1.659 V, θ 0.202 /V, λ 0.0153 /V. The fit targets were:
- the TI IOL/IOH tables, identical for the 4013B, 4011B and 4069UB;
- the CD4069UB Fig. 4 sink curves;
- the Fig. 3 crowbar currents.

The CD4049UB uses the same law with βn ×6.5 and βp ×1.2, from its IOL/IOH table.

**Unbuffered inverters** (`ub` 1) are the complementary pair itself: two `MosHalf` channels. The transfer curve, the
gain and the crowbar current all come from the law; there is no logic cell. The SPICE gate matches it to 0.002 %.

**Buffered parts** have a logic cell plus a `CmosOut` stage per output.
- **Input.** The cell compares each input pin with VSS + vth·(VDD − VSS); vth is 0.5.
- **Crossing time.** It locates the crossing between samples with a cubic Lagrange root through the last four
  samples.
- **Logic.** It runs the logic at each crossing in time order:
  - the flip-flop's set and reset are asynchronous and level-sensitive; set and reset together give Q = Q̄ = 1;
  - the clock's rising edge loads D;
  - the CD4027B J-K (datasheet truth table, §8.2) loads J·Q̄ + K̄·Q on the clock's rising edge, so J = K = 1
    toggles; its set and reset behave as the 4013's, and tpd is 150 / 65 / 45 ns at 5 / 10 / 15 V.
- **Output time.** Each output changes at the crossing time plus tpd(VDD). tpd is interpolated log-log through the
  datasheet's 5, 10 and 15 V figures.
- **Logic nets.** An input wired straight to a logic output pin, including a cell's own Q̄ as D, takes that output's
  events (time and level) directly. Chained stages therefore keep exact timing.
- **Output stage.** `CmosOut` is driven by λ, the cell's normalised drive, held on a known node.
  - Its open-circuit level is VSS + λ·(VDD − VSS): exactly rail to rail, and linear in λ.
  - The current toward that level is the full-drive channel law of the drop, symmetric for sourcing and sinking.
  - The current is split smoothly between the rails, so the supply current is drawn from VDD and returned to VSS.
    The split's ε is the quiescent current, 5 nA per output.

**Band-limited edges.** Each change enters λ as a minimum-phase band-limited step (minBLEP, Brandt 2001):
- the table is a Kaiser-windowed sinc (β 12, cut-off 0.45·fs, 32 samples), made minimum phase through the real
  cepstrum;
- it is stored at 64 points per sample with cubic Hermite;
- its residual is −126 dB above 0.79·fs.

Edge timing is therefore not quantised to the sample grid. The cost is a constant latency: the step's centroid,
3.01 samples (31 µs at 96 kHz), the same on every edge.
- A constant delay is not a pitch error and not jitter.
- Logic-to-logic edges do not accumulate it, because nets pass exact times.
- The SPICE export carries it as a lossless line (`SpiceExportOptions::logicRate`).

**Scheduling.** A cell runs just before the first group that reads its drive when every node it senses is solved
earlier in the sample. That gives exact timing, with no latency beyond the minBLEP. Otherwise it runs after the
sample, with one more sample of delay, and compile warns. That happens for a loop through the cell (an RC
oscillator), a multirate circuit or a slow partition.

**DC and state.** The DC settle re-solves until no output changes. Flip-flops power up with Q = `q0` (0).
`adoptStateFrom` carries the cell state; `adoptStateByName` does not.

**SPICE export.** The channel law becomes `.func` definitions and behavioural nodes. The logic becomes
continuous-time sources:
- input sigmoids 1 mV wide;
- for the flip-flops, a master–slave pair of 1 ns tracking states whose enables do not overlap (the master closes
  10 mV below the threshold, the slave opens at it); the J-K master tracks J·Q̄ + K̄·Q of the slave's state;
- the output stage's current written as Δ·f(|Δ|)/|Δ| rather than sgn(Δ)·f(|Δ|) (ngspice's slope of the latter is zero
  at exactly Δ = 0 and stalled an unloaded pin), plus the engine's exact linear split (a conductance β across the
  sink port, subtracted again), so an unloaded output pin stays regular;
- each output passes through a one-pole lag of tpd, then a lossless line of the minBLEP latency;
- inputs on logic nets read the source's drive before the latency, as the engine does.

ngspice's XSPICE digital models are not used.

**Results** (CTest `circuit_engine`, `testCmos`, plus the SPICE gate):

| Check | Result |
|---|---|
| CD4069UB transfer at 5 / 10 / 15 V | VIL/VIH guarantees met (Vout 5.000 / 9.977 / 14.890 V at VIL, 0.000 / 0.023 / 0.109 V at VIH); switches at VDD/2; midpoint gain 150 / 34 / 18 |
| CD4069UB crowbar current | 0.38 / 4.36 / 10.6 mA at 5 / 10 / 15 V (Fig. 3: about 0.4 / 4.8 / 11) |
| Buffered drive at the IOL/IOH table points | 0.92 / 1.84 / 6.34 mA against typ 1.0 / 2.6 / 6.8 (the 10 V point is −29 %: a single square law cannot hit all three); IOH at 2.5 V (5 V supply) 3.94 against 3.2 |
| Propagation delay (CMOS_INV) | tpd(5 V) − tpd(15 V) = 80.00 ns (datasheet 125 − 45); edge − crossing − latency = 125.00 ns at 5 V |
| 4013 truth | Q 0 at power-up; Q 10 V / Q̄ 0 V after a clock edge with D high |
| Divide-by-2 of a swept sine (300 → 1200 Hz exponential, 1 s, clock straight from the sine), against the exact toggle built from its analytic crossing times | minBLEP: error below 20 kHz −167.4 dBc peak, −153.4 dB total re the octave tone (gate −100). Sample-quantised edges, at their best constant offset: −39.9 / −27.2 dB |
| SPICE gate `cmos_small` / `cmos_hot` (`test-cmos-divider`: 4069UB squarer, 4013 ÷2 and ÷4, 4011 NAND, each output through 10k + 10n) | filtered outputs 0.31–0.85 %, H2–H5 ≤ 0.85 dB; unbuffered stage 0.002 / 0.027 % |
| `cmos_dc` | 0.000 of tolerance |
| CD4027B truth table | all 8 J/K/Q rows correct after a rising clock; reset alone gives Q 0 / Q̄ 1; set and reset together give Q = Q̄ = 1 |
| SPICE gate `cmosjk_small` / `cmosjk_hot` (`test-cmos-jk`: a J = K = 1 toggle, a D stage with J = Q1 and K = Q̄1, a divide-by-four clocked by Q1) | filtered outputs 0.31–0.84 %, H2–H5 ≤ 1.09 dB; `cmosjk_dc` 0.000 of tolerance |

**Not modelled:**
- **Setup and hold times**, and the 4013's double-clocking on clock edges slower than 10 µs. The model is an ideal
  edge on the threshold.
- **Hysteresis.** That leaves out the 40106 and 4093.
- **Input protection diodes.**
- **Transition time** (40–100 ns). It sits inside the band-limited edge.
- **Separate set/reset-to-Q delays.** They use the clock-to-Q figure.
- **The output stage's drive at intermediate λ.** λ only places the edge in time.

**Oscillators** (a CD4047 or a 4011 RC astable) run with the one-sample-delay schedule, plus the 31 µs latency per
transition. That is a frequency error for clocks in the tens of kHz. They need sub-stepping at the edge, which is
accuracy work.

**Developer switch:** `CE_LOGIC` prints each cell's schedule.

### 10.15 Zener breakdown, pot end resistance, op-amp output current limit (2026-11-10)

**Zener (`D` with `bv`, `ibv`, `nbv`).** This is the ngspice level-1 breakdown law, so the export is the plain
`.model D(... BV= IBV= NBV=)`.
- **Law.** Below −xbv the diode current is −IS·exp(−(xbv + v)/(NBV·Vt)). xbv is BV adjusted so the current at −BV is
  IBV. It is computed at compile time with ngspice's own iteration (diotemp.c), ngspice's thermal voltage
  (0.02586491704 V) and the deck's RELTOL 1e-5.
- **Defaults.** NBV defaults to N, as in ngspice 47. IBV defaults to 1 mA.
- **Implementation.** A separate kind (`NlKind::Zener`), so existing diodes are untouched.
  - It uses ngspice's DIOlimit: pnjlim in forward conduction, and on the shifted coordinate −(v + xbv) in breakdown
    (`kLimZener`).
  - Zener pairs are never merged into a `Dpair`.
  - A knee inside the forward region (xbv < 3·n·Vt) gets a compile warning. There the law steps at −3·n·Vt and uses
    xbv in the exponent, exactly as ngspice does. (Until the item 29 regression it kept the knee at 3·n·Vt, which
    moved a soft 1.4 V zener by 70 mV against ngspice.)
- **Library cards.** Vz, Izt and Zzt come from the series datasheets (S); nbv = (Zzt − rs)·Izt/Vt (D).

| Card | Vz at Izt | Zzt | Notes |
|---|---|---|---|
| 1N4731A | 4.3 V at 58 mA | 9 Ω | nbv 4 and rs 7.2 Ω, because a larger nbv puts the SPICE knee inside the forward region; bv moved to the junction so the terminals read 4.3 V |
| 1N5235B | 6.8 V at 20 mA | 5 Ω | |
| 1N961B | 10 V at 12.5 mA | 8.5 Ω | |
| 1N4744A | 15 V at 17 mA | 14 Ω | |
| 1N5245B | 15 V at 8.5 mA | 16 Ω | |

Results:
- **At the test current.** Every card reads Vz exactly (4.3000 / 6.8000 / 10.0000 / 15.0000 / 15.0000 V). Zzt comes
  out 8.90 / 4.77 / 8.11 / 13.35 / 15.27 Ω; the residual is the forward-law Vt (kVt) against the breakdown's
  ngspice Vt.
- **Law check.** The engine's breakdown matches an ngspice DC sweep to 2.4e-8 relative.

**Pot end resistance (`rend` on `pot`).** At each end of the track, rend ohms stay between the wiper and the pin. The
taper sweeps the remaining value − 2·rend, so the pin-to-pin total is unchanged. It is exported through the same
`potSegments`. With rend 200 on 10 kΩ, the wiper sees 200 / 9800 Ω at the ends. With rend 0 the arithmetic is
bit-identical to before.

**Op-amp output current limit (`ilim` on op-amp models, with `rout`).** The ideal output source drives an internal
node, and the pin is fed through i = ilim·tanh(v/(ilim·rout)) (`NlKind::OutLim`).
- An exact linear split (1/rout, subtracted from the law) keeps a pin loaded only by capacitance regular.
- The export is the same tanh B-source.
- It is fixed-rail macros only for now. Supply pins on circuit nodes with `ilim` are refused at compile time, because
  their load current is drawn through `railDraws`.
- Library models do not set `ilim`: that would move every frozen fixture.
- Measured: a follower with ilim 25 mA into 10 Ω delivers 25.00 mA when asked for 5 V. At 10 mV it delivers
  9.9994 mV.

**SPICE gate.** `test-zener.circuit.json` holds:
- ±15 V shunt regulators (1N4744A, 1N5245B) on 24 V rails with 2 V of 100 Hz ripple;
- a back-to-back 1N4731A clipper;
- a TL072-class op-amp with ilim 25 mA into 100 Ω;
- a pot with rend 200.

| Case | Result |
|---|---|
| `zen_small` | every node 0.000 % |
| `zen_hot` (8 V input, clipper in breakdown and forward, op-amp in current limit) | 0.000 % except the current-limited op-amp output at 1.06 % (gate 3 %, as other hot cases; its recovery from the open-loop excursion) |
| `zen_dc` | 0.000 of tolerance |

### 10.16 Supply-cut guard and LTE-controlled substeps (2026-11-10)

**What was wrong under hard drive.**
- The DR103's dimed error (−1.7 dB in the audio band, 74–108× ngspice's energy above 30 kHz) was **not** step
  resolution: it was the explicit supply cut at the cap-less screen rail limit-cycling (§2.6). Without that cut it
  matches ngspice to 0.21 %. The earlier rate sweep shrank it only because a finer step changed when the loop tripped.
- The `/tmp/drc` harness compared an engine settled 600 ms (circuit_cli's default `--settle`) with ngspice started at
  the crest operating point. That alone reads 13–21 % NRMSE on every dimed amp. Compare with `--settle 0`.
- Real step-resolution error remained on the Twin (+9.2 dB above 30 kHz) and the JCM800 (3.4 % NRMSE), dimed at
  176.4 kHz. Both converge as the rate goes up (Twin +0.9 dB at 2×, +0.0 dB at 4×), so they are discretisation
  error, which the LTE control (§3.1) addresses.

**Dimed, 0.3 V 220 Hz, 176.4 kHz, against ngspice at maxstep T/8** (speaker; the plates are similar):

| Amp | Fixed step: NRMSE / >30 kHz | LTE control: NRMSE / >30 kHz | Substepped group-samples per sample |
|---|---|---|---|
| Hiwatt DR103 (normal 1, master 0.8) | 0.211 % / +1.5 dB | 0.067 % / −0.1 dB | 5.5 % |
| Fender Twin (volume 1) | 0.822 % / +9.2 dB | 0.357 % / −0.1 dB | 13 % |
| JCM800 (preamp 1, master 1) | 1.96 % (pb 3.37 %) / −1.0 dB | 0.70 % (pb 0.91 %) / −0.4 dB | 13 % |
| Plexi (volume 1) | 0.95 % (pa 1.46 %) / +1.1 dB | 0.44 % (pa 0.61 %) / −0.7 dB | 10 % |
| AC30 (volume 1) | 0.94 % (pa 1.19 %) / −1.6 dB | 0.23 % / −0.3 dB | 9 % |
| Bassman (normal 1) | 0.211 % / +0.7 dB | 0.186 % / −0.2 dB | 10 % |
| Supro (gain 1) | 0.019 % / −1.0 dB | 0.021 % / −0.7 dB | 1.3 % |

Audio-band level is within 0.01 dB of ngspice in every case, with and without the control. On 0.12 s of a real guitar DI
(JCM800 at its defaults, against ngspice with the DI as a PWL source) the error falls from 4.1 % to 1.9 % NRMSE,
from −24 to −27 dB at 8–20 kHz, and from −9.6 to −18 dB above 20 kHz.

**Gate cases** (CTest `circuit_engine`): `dr103_dimed`, `plexi_dimed`, `jcm_dimed`, `ac30_dimed` and `twin_dimed`
run at 176.4 kHz (`TranCase::rate`) and gate NRMSE (3 %) and the energy above 30 kHz (`TranCase::hfDb`, ±3 dB;
a 4th-order Butterworth high-pass at 30 kHz on both signals). With `CE_LTE=0` they fail as they should (jcm pb
3.37 %, twin +7–8 dB above 30 kHz). Every existing case still passes. At 96 kHz the TS808, Tube Driver, zener and
Big Muff hot cases improved (ts_hot out1 0.74 → 0.12 %, tdrv_drive 1.77 → 0.70 %); a few pedal hot cases at
96 kHz moved the other way within their 3 % gates (ds1_hot 0.78 → 1.16 %, hm2_drive 0.68 → 1.09 %, rat_hot out
0.16 → 0.36 %), and the hot power-pair cases by a few hundredths of a percent.

**CPU** (`circuit_ab --cpu`, 12 s guitar DI, 44.1 kHz host, HQ 176.4 kHz, prepare excluded; load average 8–9,
so ±3 points is noise; Newton iterations are exact):

| Unit, setting | Fixed step | LTE control | Newton iterations |
|---|---|---|---|
| Twin, volume 0.25 (clean) | 31.2 % | 34.3 % | identical (no substeps) |
| Twin, defaults | 30.1 % | 35.5 % | +0.03 % |
| Twin, dimed | 42.1 % | 44.5 % | +11 % |
| AC30, volume 0.25 (clean) | 22.4 % | 25.4 % | +0.7 % |
| AC30, defaults / dimed | 29.0 / 28.4 % | 28.2 / 49.8 % | +6 / +28 % |
| Plexi, defaults / dimed | 34.0 / 31.3 % | 48.7 / 57.0 % | +37 / +34 % |
| JCM800, defaults / dimed | 28.3 / 33.2 % | 57.0 / 74.0 % | +33 / +57 % |
| DR103, defaults / dimed | 52.3 / 77.6 % | 61.8 / 119.4 % | +0.1 / +35 % |

Clean playing costs nothing beyond the estimate. Crunch and dimed settings on the Marshalls cost up to 2.2×,
because a guitar DI keeps their power stages crossing regimes on most pick attacks; that is where the fixed step
was wrong. `lteTol` 3 keeps most of the gain for less (JCM800 DI excerpt 2.0 % instead of 1.97 % NRMSE, Twin dimed
+2.2 dB instead of 0 above 30 kHz, about half the substeps).

**Not done.**
- WDF groups (single diode roots) have no LTE control.
- CMOS cells inside a feedback loop (RC astables, CD4047) still run a sample late, and their minBLEP latency is inside
  the loop as well. Group substeps cannot fix that: the cell is evaluated outside the solve. It needs event
  location inside the sample with the group re-solved at the edge (ENGINE_QUEUE 23b).
- Regenerative switching is resolved to the sample, not inside it (the fold time is not interpolated). The LFO rates
  still match ngspice to 0.002–0.03 % (the gate's LFO checks).

**Follow-up (2026-11-10): tolerance and CPU, regenerative loops.**

*Tolerance.* Dimed gate cases (176.4 kHz, substeps capped at 4), NRMSE at spk / pb and energy above 30 kHz:

| lteTol | DR103 | Plexi | JCM800 | AC30 | Twin |
|---|---|---|---|---|---|
| 0 (fixed step) | 0.21 / 0.30 %, +1.2 dB | 0.95 / 1.41 %, +1.0 dB | 1.96 / 3.37 %, −0.8 dB | 0.94 / 0.92 %, −1.0 dB | 0.82 / 0.99 %, +7.1 dB |
| 1 | fixed step (above 24 ports at the time; 0.08 % with substeps, see below) | 0.53 / 0.70 %, −1.0 dB | 0.73 / 1.01 %, −0.4 dB | 0.26 / 0.26 %, −0.4 dB | 0.36 / 0.37 %, −0.1 dB |
| 2 | fixed step | 0.55 / 0.71 %, −1.0 dB | 0.74 / 1.02 %, −0.5 dB | 0.33 / 0.33 %, −0.4 dB | 0.50 / 0.54 %, +2.2 dB |
| 3 | fixed step | 0.55 / 0.73 %, −1.0 dB | 0.76 / 1.05 %, −0.5 dB | 0.55 / 0.53 %, −0.8 dB | 0.54 / 0.58 %, +2.5 dB |
| 5 | fixed step | 0.58 / 0.75 %, −1.1 dB | 1.07 / 1.56 %, −1.2 dB | 0.76 / 0.70 %, −1.2 dB | 0.64 / 0.71 %, +4.4 dB (fails ±3) |

*CPU* (`circuit_ab --cpu`, 12 s guitar DI, HQ, % of one core per channel, minimum of 3 interleaved runs; the machine ran
at load average 10–20 from other jobs, so differences under ~5 points are noise):

| Unit, dimed | lteTol 0 | 1 | 2 | 3 | 5 |
|---|---|---|---|---|---|
| DR103 (normal 1, master 0.8) | 73 % | 73 % (excluded) | 73 % | 73 % | 73 % |
| JCM800 (preamp 1, master 1) | 33 % | 55 % | 56 % | 51 % | 50 % |
| Plexi (volume 1) | 31 % | 45 % | 44 % | 42 % | 39 % |
| AC30 (volume 1) | 29 % | 40 % | 39 % | 38 % | 36 % |
| Twin (volume 1) | 34 % | 47 % | 43 % | 42 % | 40 % |

Deterministic work on 4 s of the same DI (substepped group-samples, Newton iterations per sample, lteTol 0 → 1 → 3 → 5):
JCM800 0 → 255k → 206k → 188k, 7.15 → 10.4 → 8.7 → 8.4; Plexi 0 → 179k → 128k → 107k; AC30 0 → 153k → 102k → 80k; Twin
0 → 64k → 36k → 26k. The tolerance moves the cost by only 10–20 %: substeps are driven by how often a guitar makes a
power stage change regime, not by the tolerance. lteTol 1 is the tightest setting and keeps every bound amp's dimed
channel near or under 55 %. It is the default in the engine and in the live units. The DR103's 73 % is its fixed-step
cost (CPU work), since its 29-port group is outside the control's scope.

**Follow-up (2026-11-10): substeps and aliasing; the live tolerance is now 2.** CPU measurements showed that the
substeps cost the Plexi about 2 dB in the aliasing gate: −58.7 dB with LTE against −60.6 dB without. The suspected
cause was the switch between trapezoidal and backward-Euler substeps acting as a time-varying operation that makes
spurs. Tested with `circuit_cli alias --table` (Plexi 34000, variac 92000, JCM800 56000), worst non-harmonic bin in dB:

| Variant | Plexi | Variac | JCM800 |
|---|---|---|---|
| Fixed step (CE_LTE=0) | −60.6 | −61.2 | −51.6 |
| lteTol 1, backward-Euler start (shipped) | −58.7 | −58.9 | −58.7 |
| lteTol 1, trapezoidal substeps only | −57.9 | −58.2 | −53.5 |
| lteTol 1, substeps held 8 / 64 samples after a trigger | −58.6 / −57.6 | | −58.2 / −52.0 |
| lteTol 1, exports averaged over the substeps | −60.7 | −55.7 | −59.6 |
| lteTol 0.25 / 0.5 / 0.8 / 0.9 / 1.1 / 1.25 / 1.5 | −60.3 / −58.9 / −59.4 / −58.8 / −59.0 / −59.5 / −64.8 | | (0.8–1.25) −58.1 … −59.0 |
| lteTol 2 | −65.6 | −63.6 | −58.2 |
| lteTol 3 / 4 | −68.9 / −67.5 | −63.0 / | −55.1 / |

What the table shows:
- **The method switch is not the cause.** Trapezoidal-only substeps are worse on all three amps. Holding substeps on
  longer (less switching, more substeps) is also worse.
- **The JCM800 needs the substeps.** Fixed step reads −51.6 dB.
- **The Plexi's −58.7 is a band of tolerances.** Every tolerance from 0.5 to 1.25 puts the worst spur at 15.7 kHz,
  and it clears above 1.5.
- **Mechanism, not settled.** A more accurate in-sample trajectory carries more energy above the internal Nyquist
  into a point-sampled output; that fits the hold and average results. Settling it needs a uniformly L-stable
  integrator (TR-BDF2) or band-limited substep outputs, which were not built.

Change:
- `kLiveLteTol` = 2 (Engine.hpp, used by CircuitUnit and by `circuit_cli alias` through CE_LTE).
- test_aliasing: Plexi −58.7 → −65.6 dB and variac −58.9 → −63.6 dB, both now passing. JCM800 −58.7 → −58.2,
  still failing; its rate picks remain open.
- The other 29 units still pass.
- Accuracy at 2 is the table above: Plexi and JCM800 unchanged, AC30 0.26 → 0.33 %, Twin 0.36 → 0.50 % and
  +2.2 dB above 30 kHz.
- CPU drops (instructions, dimed, 176.4 kHz, 4 s DI): Plexi −3.7 %, JCM800 −3.9 %, Twin −1.9 %, AC30 −4.8 %.
  Substepped group-samples: Plexi 171k → 140k, JCM800 249k → 220k.
- The SPICE gate keeps the engine default 1 and is unchanged.

**Big groups (2026-11-10).** `lteMaxPorts` was 24, so the 29-port DR103 and the 35-port 5150 power groups never
substepped. The new gate case p5150_dimed (176.4 kHz) diverged from ngspice by 3.3 % NRMSE where the 6L6s cross
cut-off. With substeps on every group (`CompileOptions::lteMaxPorts` = 96 = kMaxNlPorts, the engine default now):

| Gate case | 24 ports | All groups |
|---|---|---|
| p5150_dimed spk | 3.33 % | 1.17 % (tolerance tightened from 10 % to 2 %) |
| dr103_dimed spk / pa / pb | 0.20 / 0.32 / 0.28 %; +1.05 / +1.48 / +1.48 dB above 30 kHz | 0.08 / 0.08 / 0.09 %; −0.14 / −0.20 / −0.02 dB |
| slo_od spk | 0.19 % | 0.12 % |
| p5150_lead spk | 0.013 % | 0.012 % |
| hm2_drive out | 0.68 % | 1.09 % (tolerance 3 %) |

Every other case is identical.

CPU (instructions, dimed, 176.4 kHz, 4 s DI, lteTol 2):

| Unit | Change |
|---|---|
| DR103 | +35 % |
| SLO-100 | +55 % |
| HM-2 | +3 % |
| 5150 | −4 % |

The live units use every group too (`kLiveLteMaxPorts` = 96, Engine.hpp, also used by `circuit_cli alias`): quality
first. `CE_LTEPORTS=<n>` overrides the limit for A/B.

Live unit CPU (`circuit_ab --cpu`, 48 kHz host, guitar DI, nice, minimum of 3, % of one core). The machine was loaded by
other jobs (load average 15–56), so read the deltas on the units with a > 24-port group:

| Unit | > 24-port group | Defaults, 24 → 96 | Dimed, 24 → 96 |
|---|---|---|---|
| DR103 | 29 ports | 75 → 71 | 118 → 142 |
| SLO-100 (OD channel dimed) | 45 ports | 169 → 198 | 196 → 244 |
| HM-2 | 26 ports | 47 → 62 | 57 → 54 |
| 5150 (lead dimed) | 35 ports | 178 → 264 | 226 → 211 |
| JCM800 | none | 55 → 48 | 51 → 52 |
| Plexi | none | 90 → 83 | 82 → 94 |

The JCM800 and Plexi do not change; their differences are the machine's noise. None of these units crossed 90 % because
of the change. The SLO-100, 5150 and dimed DR103 were already over one core per channel at 24 ports and go further over.

**BDF2 substep start (2026-11-10; CPU_THEORY.md R2.3).** On the 5150, DR103 and AC30, the backward-Euler first substep was the largest error left in the substeps. A netlist can now opt into a variable-step BDF2 start with `"lte": {"start": "bdf2", "tol": n}`. The defaults stay backward Euler and the caller's tolerance (`CompileOptions::lteStart`; `CE_LTESTART=be|bdf2` for A/B).
- **Scheme.** A substepped sample whose step size changed starts on a variable-step BDF2 step. "Changed" means the last accepted sample was a full step, or used another substep count.
  - The step runs through the last two samples' states, which the LTE history already holds. With ω = 1/m: x[n+1] = a·x[n] − b·x[n−1] + β·h·x′, where a = (1+ω)²/(1+2ω), b = ω²/(1+2ω), β = (1+ω)/(1+2ω).
  - The companion is C/(βh) (inductor βh/L), which is the trapezoidal companion at step 2βh. So the two backward-Euler runtimes become trapezoidal runtimes at 0.75 T (m = 2) and T/2.4 (m = 4), refreshed lazily as before and on the sparse substep paths.
  - Their history source is G·(a·v[n] − b·v[n−1]) for a capacitor and a·i[n] − b·i[n−1] for an inductor. After the step, the runtime's own trapezoidal update hands (v, i) to the trapezoidal substeps unchanged.
  - A sample that keeps the last sample's substep count carries on in plain trapezoid with no restart.
  - Groups with parts individually flagged backward Euler (`Prim::be`) keep the backward-Euler start.
- **Opted in, with the tolerance that is cheapest while every SPICE case stays at or under today's live setting (backward Euler, tol 2) and the aliasing gate figure is no worse:**

| Netlist | Setting | Gate NRMSE, live today → now (%) | Alias, today → now (dB) |
|---|---|---|---|
| 5150 | bdf2, 16 | p5150_dimed 1.2419 → 1.0376 | −33.74 → −33.45 (report-only) |
| SLO-100 | bdf2, 16 | slo_od 0.1295 → 0.0840 | −60.86 → −64.35 |
| DR103 | bdf2, 8 | dimed spk/pa/pb 0.0914/0.0963/0.1053 → 0.0541/0.0937/0.0631 | −69.41 → −73.77 |
| AC30 | bdf2, 16 | dimed 0.319/0.339/0.316 → 0.251/0.286/0.256; drive 0.129/0.135 → 0.052/0.058 | −64.37 → −64.35 |
| AC15 | bdf2, 8 | ac15_drive 0.0494 → 0.0240 | −59.57 → −60.60 |
| Bassman 5F6A | bdf2, 16 | bassman_drive 0.092/0.093 → 0.075/0.079 | −64.33 → −70.52 |
| 5E3 | bdf2, 16 | tweed_drive 0.0093 → 0.0049 | −65.61 → −67.41 |
| Twin | bdf2, 16 | dimed 0.423/0.448/0.430 → 0.373/0.412/0.384 | −68.67 → −84.79 |
| SVT 1969 (2026-11-11) | bdf2, 16 | against the gate before the key: svt_clean spk/pau0/bpa/bscn/biasn 0.084/1.032/1.018/0.089/0.086 → 0.080/1.005/0.991/0.034/0.029; svt_drive 0.074/0.111/1.143 → 0.071/0.108/1.118; svt_set_drive 0.013/0.015/0.021 → 0.006/0.008/0.008; svt_set_clean unchanged | −82.3 → −97.4 |

- **Known, measured, negligible regressions (accepted):**
  - p5150_lead (the 0.5 mV small-signal case) goes 0.0120 → 0.0129 %, about −78 dB of error. That is the LTE-off figure: backward-Euler substeps buy those 0.0009 points there and BDF2 does not, at any tolerance.
  - twin_drive goes 0.1994/0.1898 → 0.2028/0.1928 %. The Twin is in for its −12 % gate-tone CPU.
  - The AC30's alias figure moves by 0.02 dB (the same 1.9 kHz spur).
  - The 5150's alias moves by 0.3 dB, at −33 dB on a report-only unit.
  - Against the gate's tools tolerance 1, ac30_dimed (0.243 → 0.251) and dr103_dimed pa (0.082 → 0.094) read higher, because the gate now runs these netlists at their own tolerance.
- **Not opted in:**
  - The JCM800 gets 2.4-3.3 dB worse aliasing and no NRMSE gain, because backward Euler's damping helps its figure. (2026-11-14, §10.46: after §10.43 merged its groups, BDF2 improves its alias figure, −52.3 → −59.6 dB at tol 4, and every node of every fine-step reference reads closer, by up to 90 %; opted in at tol 4.)
  - The Plexi and Plexi variac do not hold NRMSE at tol 4-16.
  - The Deluxe holds only at tol 2, which saves no CPU.
  - Pedals: not evaluated for CPU. Several improve in NRMSE (DS-1 1.20 → 0.86 % at tol 2) but others do not (TS808, OC-2, CMOS cells).
  - Every netlist without the key is bit-identical to before (62 of 70 library renders at live LTE, plus unit-path nulls).
- **CPU, unit path, minimum of 3** (thread CPU on a loaded machine; instructions are load-independent and include two prepares):

| Unit | 4 s rock DI | Aliasing gate tone |
|---|---|---|
| 5150 | 152.6 → 121.6 % (instr 197.5 → 176.3 G) | 348 → 255 % (instr 119.8 → 97.6 G) |
| SLO-100 | 181.0 → 159.2 % (209.6 → 197.6 G) | 356 → 302 % (124.3 → 102.4 G) |
| DR103 | 40.9 → 43.1 % (83.6 → 82.9 G; unchanged) | 144 → 133 % (59.9 → 58.3 G) |
| AC30 | 75.6 → 63.9 % (196.9 → 197.1 G; unchanged) | 154 → 153 % (79.5 → 78.6 G) |
| Twin | 39.9 → 41.4 % (70.4 → 69.8 G; unchanged) | 103 → 78 % (38.7 → 33.9 G) |

- **Proof.** All of these pass: test_circuit_engine (all cases), test_aliasing (31/31 gated), test_circuit_unit (including the topology round trip of the new key), test_param_drag, and test_effect_handoffs (`subPrev` is handed over with the LTE history).
  - Knob glides during dimed events on all eight units were run with BDF2 and with backward Euler on the same input: no Newton failures, outputs finite, and the click metric is the same as with backward Euler.
  - `circuit_cli alias` and `autorate` now leave a netlist's own `lte.tol` in place, as the unit does.

**BDF2 start on pedals and studio units (2026-11-10).** Every pedal and studio netlist with a SPICE case was evaluated under the amps' rule:
- every case at or below today's NRMSE (live: backward Euler, tol 2);
- the alias figure no worse;
- CPU no worse, in instructions on the gate tone and on a 4 s DI (minimum of 3).

Opted in:

| Unit | Setting | NRMSE today → now (%) | Alias (dB) | Gate-tone instr | DI instr |
|---|---|---|---|---|---|
| DS-1 | bdf2, 4 | ds1_hot 1.1945 → 0.8779 (ds1_small 0.2084 unchanged) | −63.06 → −75.54 | 7.744 → 7.266 G (−6.2 %) | 12.191 → 12.180 G |
| HM-2 | bdf2, 8 | hm2_drive 0.7851 → 0.6767 (hm2_small 0.1074 unchanged) | −74.13 → −75.85 | 39.16 → 35.21 G (−10 %) | 75.88 → 75.51 G |

Not opted in:
- **NRMSE regresses at every BDF2 tolerance:** TS808, TS9DX, OC-2, RAT (v9), Mu-Tron III, Dyna Comp, RE-201 (q6e), and the CMOS cells.
- **Aliasing worse:** CE-1 (tol 2 and 16 are the only passing tolerances, at −45.8 and −47.36 against −47.45), Tube Driver (−89.7 against −90.7 dB), Deluxe Memory Man (−43.9 against −55.6 dB).
- **Gate-tone CPU worse at tol 2, the only tolerance that holds NRMSE:** Fuzz Face Si (+2.6 %), Fuzz Face Ge (+3.6 %), vintage Ge Fuzz Face (+5.4 %).
- **No measurable NRMSE or CPU change:** Big Muff (0.4657 → 0.4643 %), Harmonic Percolator, LA-2A, Klon. No gain, so no reason to move them.
- **No LTE activity in their gate cases:** everything else.

Proof: the full engine suite, test_aliasing, test_circuit_unit, test_param_drag and test_effect_handoffs pass. Knob glides during events on the DS-1 and HM-2 show no failures and the same click metric as backward Euler. The other 68 library netlists render byte-identical, and the unit path is byte-identical for the TS808, OC-2, Big Muff and 5150.

### 10.17 AD536A rms-to-DC converter; the `rms` output slope (2026-11-10)

**`rms`: slope decoupled from the junctions (ENGINE_QUEUE item 27).** Before this change, `mvdb` also set the cell's
thermal voltage. So `mvdb` 3.0 halved every junction's VT, and the pin-2 IBIAS string stopped converging: n_ib read
−0.006 V where ngspice reads −15.89 V.
- **Now.** The junctions run at the physical `vtj` (new parameter; default 6.1 mV/dB × 10/ln10 = 26.49 mV, the
  2252's 35 °C chip). `mvdb` is only the output slope: OA3's gain is mvdb / (2·vtj·ln10/20).
- **Existing cards.** At 6.1 that gain is exactly 1, so the THAT2252 card and every fixture are unchanged.
- **Export.** The gain is exported in the output B-source.
- **Test.** The same detector with `mvdb` 6.1 and 3.0 puts pin 2 at −12.9032 V both times, with slopes
  6.090 / 2.995 mV/dB (ratio exactly 3.0/6.1).

**`ad536` part (AD536A, Analog Devices rev. G).** The SSL 82E10 side-chain detector. Nodes are in the D-14 pin
order:
`[vin, nc, −vs, cav, db, bufout, bufin, iout, rl, com, nc, nc, nc, +vs]` (pins 1–14).
NC pins may be left on bare stub nodes; a 1 GΩ to COM keeps such a node defined.

The model follows Fig. 6:
- **Input.** The input resistance is 16.67 kΩ to COM.
- **Absolute value and squaring.** The absolute-value stage gives I1 = |VIN|/R4, with R4 = 50 kΩ. The
  squarer/divider gives I4 = I1²/I3; the rectifier's sign drops out of the square.
- **Averaging.** I4 is drawn from the CAV pin, through the internal R1 = 25 kΩ from +VS, into the external CAV. The
  mirror current is I3 = V(+VS, CAV)/R1, so I3 = Avg(I1²/I3) = I1 rms with τ = R1·CAV (25 ms/µF), the datasheet's
  implicit rms loop.
- **Linear output.** IOUT = 2·I3 (40 µA/V rms) behind R2 = 25 kΩ to RL.
- **dB pin.** dB = COM − (3 mV/dB)·20·log10(I3/IREF), with IREF = 20 µA (0 dB at 1 V rms).
- **Buffer.** BUF IN → BUF OUT is an ideal follower with a 10⁸ Ω input.
- **Supply.** The quiescent current is 1.2 mA from +VS to −VS.
- **Assumptions (A).**
  - An input floor of 1 mV rms, added in rms. It keeps I3 above zero with no input.
  - A soft ceiling on I4, i4max = 49 mA, fitted so a crest-factor-7 pulse train reads −1.0 % (spec).
  - The dB pin is taken as an ideal source: the Q5 emitter's load dependence is not modelled.
- **Export.** It is the same law in B-sources, plus `G` / `E` for IOUT, the dB follower and the buffer.

**Results** (CTest `circuit_engine`, `testAd536`):

| Check | Result |
|---|---|
| dB law, 7 mV – 7 V rms (the datasheet's dB-error range) | worst 0.019 dB (AD536AJ spec ±0.4 dB typ) |
| Linear output | worst 0.21 % (spec ±5 mV ± 0.5 %) |
| Sine and square at 1 V rms | 0.99969 / 0.99980 V |
| Crest factor (1 V rms, 200 µs pulses) | CF 7 −1.09 % (spec −1.0 %); CF 3 −0.27 % (spec −0.1 %: the single ceiling over-reads the low crest factors) |
| Averaging time constant with 1 µF | 25.000 ms (25 ms/µF) |
| SPICE gate `ad536_small` / `ad536_hot` (`test-ad536`: the 82E10 wiring, CAV 3n3 to +15 V, the dB pin into a ×32.9 amplifier, plus a 1 µF section with a buffered linear output) | every node ≤ 0.095 % |
| `ad536_dc` | 0.001 of tolerance |

### 10.18 `vca` control port, `vczero`, pin 5 and the dbx 2151 card (2026-11-10)

**What the chip has.** This was checked against the THAT 2150-series data sheet (rev. 10/25/96, Figs. 3 and 6) and
dbx Application Note 103:
- **Control pins are transistor bases.** On the 2150/2151 (and the 2180), Ec+ (pin 2), Ec− (pin 3) and SYM (pin 4,
  the second Ec+) are the bases of the core transistors.
  - AN103: "The control ports present a high impedance load", to be "sourced from a low impedance (<1 Ω)", because
    "higher source impedances will cause distortion".
  - There is **no internal resistor**: the chip has no current-mode control port. The 203 Ω input belongs to the
    *modules* (THAT 202R / dbx 202), whose control pin is an internal divider. It is the `DBX202` card's `rc`, and
    it is wrong for a 2151.
- **Current drive is the external network's.** It happens when the control node is fed through resistors, and the
  pin then simply reads that node.
- **Pin 5 is a voltage.** AN103: it is "normally four diode drops below ground". The external resistor to VEE sets
  the bias current; THAT's 5.1 kΩ to −15 V draws 2.4 mA. AN103's "DC offset nulling" adds 10 kΩ from the control
  port to pin 5. The 82E10's R75 10 K is that resistor, so its CV-node offset current comes from pin 5's fixed
  voltage, not from any internal control offset.

**Model changes** (all exported identically):
- **`vczero`** (V): the control offset at Ec−. gain (dB) = (Ec+ − Ec− − vczero)/mvdb. The data sheet's "gain at 0 V
  control" is 0 ±0.1 dB for the 2151, i.e. vczero within ±0.61 mV.
- **`v5`, `r5`.** Pin 5 is held v5 below GND behind r5, and its current is drawn from V+. Values: THAT2180A/B/C
  2.85 V (data sheet −2.85 V typ), replacing the old constant `icc`; DBX2151 2.76 V (Fig. 3: 2.4 mA through
  5.1 kΩ). r5 is 20 Ω (A).
- **`bf`**: the core β. With bf > 0, each control pin draws its transistors' base current I/β, so a control node
  fed from a high impedance moves with the signal, as AN103 warns. It defaults to 0 (ideal): β is unpublished.
- **`isc`** (A, off by default): the output transistors' collector–base junctions, which give OUT compliance when
  the I/V op-amp clips. It is not gated yet; see the note below.
- **New card `DBX2151`.**
  - 6.1 mV/dB; high-impedance control pins (`rc` 0); SYM is a separate base (`rsym` 0: wire pin 4, 51 Ω to GND as
    in Fig. 3); pin 5 at −2.76 V.
  - vsym 11.9 µV and rs 0.515 Ω are fitted to THD 0.004 % at 0 dB and 0.025 % at ±15 dB, IIN + IOUT 180 µA, 1 kHz.

**Results** (CTest `circuit_engine`, `testVcaControl`):

| Check | Result |
|---|---|
| Voltage drive (low-impedance source), Ec− −122 / 0 / +122 mV | +19.991 / +0.001 / −19.989 dB (data sheet ±20 dB) |
| Current drive: 1 V through 1k into 111 Ω | node 0.1000 V, gain −16.385 dB (−node/6.1 mV −16.393) |
| AN103 10 kΩ to pin 5, Ec− fed 0 V through 1k | pin 5 −2.845 V; node −0.2586 V, exactly the divider; gain follows the node |
| `vczero` 0.61 mV | −0.1000 dB at Ec− = 0 |
| β 100 through 10 kΩ | the control node moves by the base current (3 µV here) |
| SPICE gate `vca2151` (`test-vca-2151`: Fig. 3 plus AN103 nulling, a 2 Hz control swing through 100 Ω / 10 kΩ, plus a β-100 instance with vczero) | every node ≤ 0.001 % |
| `vca2151_dc` | 0.001 of tolerance |
| `vcarms_dc` | regenerated: the 2180s' pin 5 moved from −2.76 V (constant-current model) to −2.90 V (the pin-voltage model); no signal node moved |

**Open.** An I/V op-amp driven into clipping by a VCA at about +40 dB (0.5 V in), with no compliance, runs the OUT
node away in the engine. With `isc` the engine holds OUT within a junction drop and stays bounded, but still
counts Newton failures in the overload, and ngspice stops on the same deck ("timestep too small"). So `isc` stays
off by default and ungated.

### 10.19 SPICE export robustness: case-only node pairs, full-node `.nodeset`, OTA abc mirror (2026-11-10)

ENGINE_QUEUE 32. The EHX Micro Synth WIP (`work/circuit-netlists/wave-e/ehx-micro-synth-76000.wip.json`) solved in
the engine. ngspice's `.op` on the exported deck, however, either aborted (when seeded with the engine's DC) or
landed 18 V away on the envelope/attack nodes (`q5e`, `atkw`, `Xsq`).

**Root cause: a node-name collision that exists only in SPICE.** The netlist has `nC2` (the VCF input RC: R21,
C9, R22) and `nc2` (A17A's CD4013 Q). These are two nets in the engine. SPICE folds case, so both became one
node: the filter capacitor was shorted onto the flip-flop output. The circuit ngspice solved was therefore a
different circuit. Pinning every node at the engine's DC, the only net whose KCL broke was that pair.

**Fixes (export only; the engine is untouched):**

- **Case renames.** `exportSpice` gives each netlist node that folds onto an earlier name a `_c<k>` suffix:
  `nc2` → `n_nc2_c2`.
  - The deck lists every rename in a header comment (`* node "nc2" is n_nc2_c2`).
  - `spiceNodeName(n, node)` returns the same name, so it can be used to address deck nodes.
  - A netlist without such a pair exports byte-identically.
  - Lint warns: `nodes "nC2" and "nc2" differ only in case`.
- **Full-node `.nodeset`.**
  - Every `#`-free key of `SpiceExportOptions::nodeset` whose node exists in the deck gets a `.nodeset`.
  - Engine-internal names (for example a diode's `#j` Rs node, which the deck models as a native `RS`) and unknown
    names are skipped. ngspice warns about, or aborts on, vectors that do not exist.
  - `circuit_cli spice` now seeds every netlist node from the engine's DC, as well as the `#x`/`#src1` hints;
    `--no-hints` still turns all seeding off.
  - The test harness's `engineHints` is unchanged (`#x`/`#src1` only), so no fixture moved.
- **OTA abc mirror.**
  - The CA3094/LM13700 output halves and input base currents now take the abc diode's own current through a 0 V
    sense source: `I(V<ref>_abc)` minus the diode's 1e-12 S gmin term, which is the same `is·(e^x − 1)` law.
  - Previously each half carried its own `exp(V(abc,vm)/nVt)`. A Newton step that moved the abc node by volts
    overflowed it to NaN, because ngspice's junction limiting only protects the diode element.
  - Regenerated against this form (and then restored), `otaopt`, `dyna` and `sstone` moved by solver noise only:
    DC ≤ 40 µV, AC ≤ 0.0065 dB, transient ≤ 0.63 mV at 4.3 V. Their gate cases passed on the regenerated
    references too.

**Results.**

| Check | Result |
|---|---|
| Micro Synth `.op`, seeded with every node (`circuit_cli spice`) | converges directly; 131 of 131 nodes within 66 µV of the engine (that bound is the 5-digit print of `info`) |
| Micro Synth `.op`, no seeds at all | converges (dynamic gmin) to the same point, 131 of 131 nodes |
| `msynth_dc` (new; snapshot `library/test-dc-microsynth.circuit.json`, fixture with `#x` hints only) | 134 nodes, worst 0.029 of tolerance |
| `testSpiceExportRobust` | case pair → `n_nC2` / `n_nc2_c2`; `.nodeset` only on deck nodes; OTA sense source; lint |

**Harness note (`gate.py` in the wave-e work).**

- Its `CLI` is hard-wired to `app/build/circuit_cli`, so it picks up this fix only when that build is rebuilt.
- Its `sp()` must honour the deck's rename comments.
- It compares the case-folded names ngspice prints with mixed-case keys, so it skipped mixed-case nodes (76 of
  131 compared).
- It no longer needs to append its own `.nodeset` lines. It seeded nodes that do not exist (`n_d1_j`), and it
  stripped the deck's `#x` hints.
- The Micro Synth's quiescent AC (VCA closed, about −180 dB) is a leakage floor, not a gain to compare.

### 10.20 Op-amp input errors: `vos`, `ib`, `ios` (2026-11-10)

ENGINE_QUEUE 31, from the GK 800RB power amps. Their LF353 input stage has only 1 MΩ in series with 0.1 µF as
feedback, plus a 1N4002 clamp across the pair, so there is no DC feedback path. The macro had no input errors, so it
sat on the all-zero point and starved the VAS. A real unit is held off zero by the op-amp's offset and bias current.

**Model parameters** (on any `opamp` card or netlist model, level 0 and level 1; all default to 0):

| Parameter | Meaning | Engine | SPICE export |
|---|---|---|---|
| `vos` | series voltage at the + input: the stage sees V(+) + vos | `Vcvs` node `<ref>#ip` = V(+) − (−vos), with the known node `#kvos`; the input stage (OTA law or nullor) senses `#ip` | `V<ref>_vos n_<ref>_ip <+> DC vos` |
| `ib` | DC bias current into each input pin (positive into the op-amp; a PNP input stage has ib < 0) | `Vccs` from the pin to ground, driven by a known 1 V node | `I<ref>_ibp <+> 0 DC …`, `I<ref>_ibn <−> 0 DC …` |
| `ios` | offset current: I(+) = ib + ios/2, I(−) = ib − ios/2 | as `ib` | as `ib` |

- A card with all three at 0 compiles and exports exactly as before. No library card sets them, so no fixture moved.
- The sign of `vos` is part of the netlist. In the 800RB idiom, +5 mV drives the output up until the clamp conducts,
  a unique point (`outa` +0.115 V here). −5 mV drives it to the rail with the clamp reverse-biased
  (`outa` −13.60 V), and ngspice agrees on both. Pick the sign that reproduces the sheet's annotated voltage.
- Typical values to use (data-sheet typ): LF353 5 mV / 50 pA / 25 pA; TL072 3 mV / 65 pA / 5 pA; JRC4558
  0.5 mV / 25 nA / 5 nA; NE5532 0.5 mV / 200 nA / 10 nA; LM741 1 mV / 80 nA / 20 nA.
- This does not replace the export's `rshunt=1e12`. A capacitor-isolated node still needs it in SPICE.

**Validation** (`library/test-opamp-vos`: an LF353-class macro at 5 mV / 50 pA / 25 pA):

- Stage A is the 800RB input idiom (47k in; 1M + 0.1 µF feedback; 1N4001 clamp).
- Stage B is a non-inverting ×101 stage fed from 1 MΩ.
- Stage C is an ideal follower with vos 2 mV.

| Check | Result |
|---|---|
| Stage B vs the closed form with finite a0 | 0.498188 V vs 0.498188 V; + pin −62.500 µV (= −ib(+)·1 MΩ) |
| Ideal follower | output exactly 2.000 mV |
| Series-C stage | summing node 4.9988 mV, output 0.1151 V (the clamp's drop at 106 nA); without vos/ib it sits at 0 V |
| `vos_dc` (ngspice .op, also converges with no seeds) | 10 nodes, 0.003 of tolerance |
| `vos_small` (20 mV, 440 Hz) | outa 0.012 %, sa 0.035 %, outb 0.002 %, outc 0.000 % |
| `vos_hot` (0.3 V, 220 Hz; A clamps, B clips) | outa 0.000 %, sa 0.301 %, outb 0.007 %, outc 0.000 % |

### 10.21 Electro-mechanical speaker and the cabinet as a circuit (2026-11-10)

The netlist speaker was a linear Thiele–Small load, and the cab was an IR convolution after the amp (a measured IR
that cannot ship). The `speaker` part is a nonlinear electro-mechanical driver whose cone velocity is a node,
and `lib:cab-*` blocks turn that velocity into the microphone signal. A netlist can therefore end in speaker + cab.

**Equations** (one equivalent driver; x in mm, positive outward for positive current into pin 1):

```
v(+,−) = Re0·(1 + α·ΔTv)·i + v(L2 ∥ R2) + dφ/dt + Bl(x)·u          φ = Le(x)·i
Mms·du/dt = Bl(x)·i + ½·i²·dLe/dx − Rms·u − Kms(x)·x                dx/dt = u
Bl(x) = Bl0·(1 + b1 x + b2 x² + b3 x³ + b4 x⁴)    Kms(x) = Kms0·(1 + k1 x + k2 x² + k4 x⁴)    Le(x) = Le0·(1 + lx1 x + lx2 x²)
Ctv·dTv/dt = Re(T)·i² − (Tv − Tm)/Rtv            Ctm·dTm/dt = (Tv − Tm)/Rtv − Tm/Rtm                (per coil)
```

- **Clamp.** Bl and Le are evaluated at x clamped to [xlo, xhi], the minima of the Bl polynomial on each side (Bl is
  C¹ there; about ±7 mm and 0.3–0.4·Bl0 for the library cards). Kms is not clamped: the suspension stiffens without
  bound, which is what limits excursion. The reluctance force is zero outside the clamp, where Le is held.
- **Circuit form** (`Engine.cpp` "speaker", `Devices.hpp` `NlKind::Speaker`). Electrical side in nodal form; mechanical
  side in the mobility analogue (velocity = voltage, force = current): Mms is a capacitor on `vel`, 1/Rms a resistor,
  a box air spring an inductor 1/K. The flux node ψ = φ/Le0 (a capacitor of Le0 farads, read in amps) integrates the
  coil voltage minus the EMF; the excursion node `#xm` integrates 1000·u. The rest-point linear parts (Bl0 both ways,
  Kms0·x, i = ψ) are primitives; a 3-port law (x, u, ψ) carries the exact remainder, so the device adds 3 Newton
  ports and its small-signal response is the matrix alone.
- **Thermal.** Tv and Tm are integrated (trapezoidal) after each solved sample from the converged coil current, and
  ΔRe = Re·α·Tv enters the next sample (`Circuit::advanceSpeakers`; the device's `hs[]` hold ΔRe, Tv, Tm, so a state
  hand-off carries them; `reset()` starts cold). The lag is one sample against time constants of seconds.
  ngspice integrates the same network implicitly.
- **Wiring.** `series` × `parallel` identical drivers fold into one exact equivalent (R, L × s/p; Bl × s; Mms, Kms, Rms ×
  s·p; heat shared by s·p coils). A 2s2p 4x12 is one 3-port device, not four.
- **LTE gate.** The regime test of the truncation-error control reads a speaker's whole incremental slope (law +
  rest-point part); on the law alone (near zero) it substepped clean 3 kHz tones and made them worse (1.38 % vs
  0.51 % NRMSE against ngspice).

**Model cards** (`ModelLibrary.hpp`; S = maker's sheet, M = measured set, D = derived, A = assumed):

| Card | Small signal | Source |
|---|---|---|
| CELESTION_G12M25_8 | Re 6.7, fs 75 Hz, Mms 29.22 g, Bl 12.14, Qms 9 (Qes 0.626), Kms 6489 N/m, Rms 1.53, Sd 525 cm², Le 0.41 mH @1 kHz, Xmax 1.2 mm | S: Celestion G12M Greenback sheet (Re, fs, 25 W). M: T/S set posted by "fyl", Music Electronics Forum, 2010-05-18 (read fs 85 Hz; Kms refit to the sheet's 75 Hz) |
| CELESTION_G12M25_16 | Re 13.1, Bl 16.98, Le 0.80 mH; otherwise as the 8 Ω | S: Re from the sheet. D: same cone and motor, coil scaled (Bl ∝ √Re, Le ∝ Re, Qes unchanged) |
| CELESTION_V30_16 | Re 12.9, fs 75 Hz, Mms 29 g, Bl 16.4, Qms 9.2 (Qes 0.655) | S: Celestion Vintage 30 sheet (Re, fs, 60 W). M: a forum set attributed to the V30 only by its 12.91 Ω Re. A: Le 0.8 mH, Sd and shapes as the G12M |
| JENSEN_P12Q_8 | Re 5.6, fs 90.5 Hz, Mms 26.8 g, Bl 5.88, Qms 11.57 (Qes 2.47), Le 0.67 mH, Xmax 1 mm, coil 8 mm / gap 6 mm | S: Jensen specification sheet (its Qes 3.33 contradicts its own Qms/Qts; the card's 2.47 follows from Bl, Mms, Re) |
| JENSEN_C12N_8 | Re 6.05, fs 113 Hz, Mms 29.9 g, Bl 10.46, Qms 7.52 (Qes 1.17), Le 0.9 mH, Xmax 1 mm, coil 10 / gap 8 mm | S: Jensen specification sheet |
| EMINENCE_LEGEND1258_8 | Re 7.44, fs 94 Hz, Mms 32 g, Bl 10.9, Qms 6.15 (Qes 1.18), Le 0.70 mH, Xmax 0.5 mm, gap 7.92 mm | S: Eminence datasheet |

Large-signal shapes and thermal values are not published for any guitar speaker; they are assumed and labelled A:
- **Coil.** LR-2 split L2 = 2·Le, R2 = 3·Re, with Le solved so the coil's inductance at 1 kHz equals the sheet's Le (G12M-8: Le 0.137 mH + 0.275 mH ∥ 20.1 Ω).
- **Bl(x).** A least-squares quartic to the field integral of an overhung coil (height = gap + 2·Xmax unless the sheet gives it) in a gap with exponential fringes, 0.25·gap in front and 0.40·gap behind. That gives Klippel's 82 % point X_Bl at −2.97 / +2.41 mm for the G12M (b1 −0.01384, b2 −0.02708, b3 1.47e-4, b4 2.72e-4 per mm^k).
- **Kms(x).** 133 % at 1.5·Xmax (Klippel's X_C criterion, Cms 75 %), 300 % at 2.5·Xmax, +5 % asymmetry at X_C (G12M: k1 0.0278, k2 0.0341, k4 0.0209).
- **Le(x).** −15 % per Xmax outward, +2 % curvature (lx1 −0.125, lx2 0.0139 for the G12M).
- **Thermal.** Copper α 0.00393/K. 110 K steady rise at rated power (about 3 dB power compression), 70 % coil-to-magnet, 30 % magnet-to-air; τ coil 8 s, τ magnet 15 min (G12M: Rtv 3.08 K/W, Ctv 2.60 J/K, Rtm 1.32 K/W, Ctm 682 J/K).
- **Script.** The derivation is `tools/circuit/speaker/spk_params.py`.

**Cabinets** (`library/cabs.blocks.json`, `tools/circuit/speaker/`): pins `[vel, out]`, for a speaker part with
series × parallel = 4.
- **Box.** A closed-box air spring on `vel` (K = ρc²Sd²/(V/4) per driver, 130 L internal from the outside dimensions:
  11 818 N/m per G12M, 4× on the 2s2p equivalent; that puts the boxed resonance at 126 Hz, where the 1960AX IR
  peaks) and a leak/absorption resistance (1.5 N·s/m per driver, A).
- **Acoustic path.** The IR divided by the model's small-signal cone velocity per terminal volt (voltage drive
  assumed), smoothed to 1/24 octave, made minimum phase, vector-fitted (Gustavsen–Semlyen) with 24 stable pole pairs
  and no feedthrough. Each pair is a two-integrator resonator driven from `vel` (a VCCS), summed into `out` through
  1 Ω. The engine runs each as its own 2-state linear group; the export is exact.
- **Units.** `out` is the terminal voltage convolved with the IR, times 0.1 (about the speaker voltage in the midband, so a
  dimed amp's mic node stays far below the unit's 1 kV smoke-test bound). A netlist moving from the IR path keeps its level
  with `outputVoltsPerFS` × 0.1.

| Block | IR (CC0) | Fit error, 60 Hz–8 kHz (rms / max) | Engine chain vs smoothed IR (96 / 192 kHz) |
|---|---|---|---|
| `cab-1960ax-g12m25-sm57` | Jester's Emerald Pack 1.0, "1_Nacho_Guacamole_48" (Marshall 1960AX, G12M-25, SM57 upper left) | 0.67 / 2.26 dB | 0.68 / 0.63 dB rms |
| `cab-bg412s-v30-sm57` | Jester's Brutal Pack 1.0, "1_Cookie_Monster" (BG412S, V30, SM57) | 0.94 / 3.70 dB | 1.24 / 1.12 dB rms |

**Ending a netlist in speaker + cab** (replace the `lib:spk-*` loads; `io.output` becomes the mic node):

```json
{"ref": "SPK", "type": "speaker", "nodes": ["spk", "0", "cone"], "model": "CELESTION_G12M25_16", "series": 2, "parallel": 2},
{"ref": "CAB", "type": "sub", "of": "lib:cab-1960ax-g12m25-sm57", "nodes": ["cone", "mic"]}
```

**Gates** (CTest `circuit_engine`; `library/test-speaker`: free-air G12M-25 behind 0.5 Ω, a boxed G12M with a 30 ms
coil time constant behind 2 Ω, a 2s2p P12Q; `library/test-speaker-cab`: the netlist above, voltage-driven):

| Case | Drive | NRMSE vs ngspice |
|---|---|---|
| `spk_small` | 0.5 V, 110 Hz | vel 0.001 %, a 0.000 %, vel2 0.001 %, vel3 0.002 % |
| `spk_hot` | 20 V, 60 Hz (2.9 mm) | vel 0.001 %, x 0.000 %, vel2 0.005 %, x2 0.002 %, vel3 0.000 % |
| `spk_xmax` | 60 V, 30 Hz (3.7 mm; past X_Bl) | vel 0.009 %, x 0.001 %, vel2 0.032 %, vel3 0.004 % |
| `spk_thermal` | 20 V, 220 Hz, 0.3 s (coil heating, 30 ms) | vel2 0.002 %, b 0.001 %, vel 0.002 % |
| `spk_hf` | 20 V, 3 kHz | vel 0.51 %, ψ 0.21 % (trapezoidal phase error at 96 kHz; 0.16 % at 192 kHz) |
| `spkcab_small` / `spkcab_hot` | 1 V 220 Hz / 50 V 82 Hz | mic 0.002 % / 0.003 %, cone 0.003 % / 0.000 % |
| `spk_ac`, `spkcab_ac` (192 kHz) | .ac | 0.026 / 0.044 dB to 5 kHz, 0.100 / 0.416 dB 5–10 kHz |
| `spk_dc` | .op | 7 nodes, 0.000 of tolerance |

H2–H5 agree within 0.01 dB in every transient case. Unit checks (`testSpeakerDevice`):
- **Linearised = Thiele–Small.** With no polynomial terms and no heat, the device equals the classic T/S equivalent (Re + L2∥R2 + Le + Res∥Lces∥Cmes) to 5e-15. |Z| peaks at 102.6 Ω, 75.4 Hz (Re + Bl²/Rms = 103.0).
- **2s2p fold.** One 2s2p P12Q equals four wired instances at 40 V, 50 Hz to 1.5e-13.
- **Thermal.** The coil rise is 21.78 K against Rtv·⟨P⟩ = 21.75 K, and a hand-off carries the temperatures.
- **Abuse.** A ±80 V, 20 Hz square peaks at 5.0 mm with no Newton failures.

Every non-speaker gate is unchanged (the whole `test_circuit_engine` log diffed against the pre-change build). The
one failure in both, `ffsi_small` H2–H5 2.36 dB, predates this work.

**Against the IR path** (Plexi 1959 netlist, 4x12 16 Ω: its four linear `lib:spk-12in-16ohm-british` loads replaced
by one 2s2p CELESTION_G12M25_16 + `lib:cab-1960ax-g12m25-sm57`; 8 s of the rock DI at 88.2 kHz, decimated
to 44.1 kHz; IR path = the amp's speaker voltage convolved with the same Jester IR. Third-octave levels, dB, cab path
minus IR path):

| Band (Hz) | 63 | 100 | 160 | 250 | 400 | 630 | 1k | 1.6k | 2.5k | 4k | 6.3k | 8k |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clean (volume1 0.3, DI ×0.08), same load | +0.49 | +0.43 | +0.63 | +0.48 | +0.22 | −0.21 | +1.39 | +0.06 | +0.19 | +0.69 | −0.75 | +1.12 |
| clean, vs the shipped linear load | −0.57 | +0.85 | +1.27 | +0.73 | +0.31 | −0.27 | +1.02 | −0.62 | −0.56 | +0.13 | −1.10 | +0.89 |
| loud (volume1 1.0), same load | −0.47 | −0.09 | +0.30 | −0.34 | −0.96 | −1.28 | −0.24 | −0.90 | −0.49 | +0.11 | −0.67 | +0.83 |
| loud, vs the shipped linear load | −4.35 | +0.30 | +2.17 | +0.53 | −0.29 | −0.89 | −0.52 | −1.80 | −1.54 | −0.61 | −1.18 | +0.24 |

- **Clean.** "Same load" is the cab path against the IR convolved on the speaker voltage of the same amp + nonlinear
  speaker. At clean it is the fit residual: within ±0.75 dB except 1 kHz (+1.4) and 8 kHz (+1.1). Overall level
  +0.31 dB.
- **Loud.** Loud minus clean, on that difference, is what the speaker adds: −0.3 to −1.6 dB through the mids (Plexi
  output 46 V rms, 2.75 mm peak excursion). Over the 8 s, voice-coil heating accounts for −0.42 dB of level, and
  Bl/Kms/Le for −0.16 dB (renders with the thermal network off, then fully linear).
- **The shipped linear load.** Against it, the boxed impedance peak (126 Hz instead of free-air 75 Hz) moves the amp's
  own low end: −4 dB at 63–80 Hz and +2 dB at 160 Hz when loud. That is amp/speaker interaction, not the cab filter.
- **Sine at 82.4 Hz, dimed.** The fundamental at the mic is −1.5 dB against the IR path, and harmonics relative to it
  (H2…H7) read +2.9, −6.0, −13.7, −27.0, −15.7, −25.2 dB, against +0.1, −12.3, −11.3, −47.8, −18.1, −25.2 dB for the
  IR path: more odd-order distortion from Bl(x) and Kms(x).
- **Sine at 82.4 Hz, clean.** H2 is −35.5 dB at the mic against −61.2 dB (asymmetric Kms and Le(x)).

**CPU** (M4 Pro, one core, 88.2 kHz internal): the Plexi with the IR-path load 14.4 %, with speaker + cab 19.0 %. The
power group grows from 11 to 14 Newton ports, and the 24 cab sections cost about 11 ns each. Speaker + cab alone at
96 kHz: 3.6 %.

**Integration (2026-11-10, second pass).**
- **Rack flag.** `netlistEndsInCab()` (Netlist.hpp): a netlist with a `speaker` part whose output is none of the speaker's
  pins ends in a cab. `IComponent::includesCabinet()` reports it for circuit units, and the model JSON carries
  `"ends_in_cab"`. The Details tab shows a "Speaker + cab in the circuit" chip and a note.
- **Rack behaviour.** `RackGraph::afterInsert` adds no matched cabinet after such an amp. `syncCabFollow` bypasses a
  cabinet unit after it once, remembered on the node (`cab_muted_by_amp`), so a cab the user turns back on stays on. It
  turns the cab back on when the netlist returns to a plain load, and it counts the amp as followed by a cab for the
  quality rule. Tested in `test_circuit_unit` ("rack speaker + cab"): flag, bypass, user override kept, again, restored.
- **Cab-follow coupling.** In a netlist that does not end in a cab, a following cab unit's speaker profile drives the
  `speaker` part, as it drives the T/S blocks. Mms × (fs0/fs)² puts the resonance at the profile's fs, Rms ÷ kq scales
  the impedance peak (Res × kq, as the blocks), and Le and L2 take the Le scale; the law's reluctance term follows.
  Kms is held, so the bandwidth follows from the cone mass. `testSpeakerDevice`: the |Z| peak moves 75.0 → 119.8 Hz
  (103.0 Ω kept) at a 120 Hz profile, setSpeakerLoad(0) restores it to 6e-11 Ω, and a speaker + cab netlist does
  not move at all.
- **Schematic and drawer.** `circuit-view.js` draws `speaker` parts as a loudspeaker glyph (magnet, cone, sound
  arcs), electrical pins left and the cone pin right (pin names +, −, cone; + and − conduct at DC; models swap within
  the `speaker` kind). `circuit-parts.js` draws the basket with a CONE (m/s) tag on the dust cap, the card's name, the
  driver count and the wiring. Checked in WKWebView at 2× (work/spk-ui).
- **Listening A/B** (work/circuit-proofs/<amp>-speaker/, listen.html "Speaker + cab"): Plexi, JCM800, Twin and AC30
  at clean, edge and dimed on the calibrated DI. The IR path is the netlist as shipped, its speaker volts convolved with
  the Jester 1960AX IR. The speaker + cab path is 4 × CELESTION_G12M25_16 at the amp's tap impedance (2s2p 16 Ω; the
  Twin 4p 4 Ω) + `lib:cab-1960ax-g12m25-sm57`. Both run at 176.4 kHz and are level-matched.

**Not done / limits.**
- No library amp is switched over: the IR path stays the shipped default (a listening decision; see the A/B).
- The nonlinear shapes and thermal values are assumed (labelled A), not measured on these speakers. Klippel LSI data
  for any of them would replace them directly.
- The cab fit is minimum phase and 1/24-octave smoothed: no mic delay and no comb finer than about 1/24 octave.
- The IR is divided by a voltage-driven speaker; an IR captured through a tube amp's output impedance would fold that
  amp's damping into the acoustic filter.
- The box is a lumped air spring and leak only: no port, no panel resonance, no standing waves beyond what the
  fitted filter carries. Cone breakup is linear (in the fitted filter); its level dependence is not modelled.

### 10.21 Rate ladder: per-group power-of-two rates (2026-11-10)

Implementation in §3.7. Aliasing gate (tests/test_aliasing.cpp, through CircuitUnit, 48 kHz host): the Plexi, Plexi
variac and AC30 are gated again and pass. They read −61.1, −62.9 and −65.0 dB, with the power group at 16x on a 4x base.
All 32 static units are below −60 dB.

**CPU per channel** (`circuit_ab --cpu`, 12 s guitar DI, LTE as shipped, load average 9–15 from other jobs; read the
ratios rather than the absolute values):

| Unit | Whole circuit 16x | Rate ladder (power 16x, rest 4x) | Whole circuit 4x (before) |
|---|---|---|---|
| Plexi, defaults / dimed | 268–341 % / 235–298 % | 83–88 % / 81–83 % | 55–57 % / 51–53 % |
| Plexi variac, dimed | 242–283 % | 84–109 % | 50–59 % |
| AC30, defaults / dimed | 174 % / 211–289 % | 87 % / 75–105 % | 35 % / 46–54 % |

Against the whole circuit at 16x the ladder costs about a third: the substeps are off on the 16x group, and the
preamp and supply stay at 4x. Against the 4x amps it replaces, it costs 1.5–1.8x.

The SPICE gate is unchanged: it compiles without levels, and every case reads as before. test_circuit_engine,
test_circuit_unit, test_aliasing and test_param_drag pass.

### 10.22 Noise and hum (2026-11-10)

Implementation in §3.9. All figures are from `circuit_cli` with `CE_NOISE` / `CE_HUM`, on the tree as of the 12AX7
card refit.

**Thermal noise.** A 100 kΩ / 100 kΩ divider (plus 1 pF) should read √(4kT·50 kΩ) = 28.78 nV/√Hz.
- Measured at 176.4, 96, 88.2 and 48 kHz: 28.63–28.90 nV/√Hz in every band from 100 Hz to 20 kHz.
- In 2 kHz bands at 176.4 kHz: −0.05 to +0.13 dB through 20 kHz.

**12AX7 stage.** 250 V, 100 kΩ plate load, 1.5 kΩ ∥ 22 µF cathode, 1 kΩ source, current 12AX7 card.
- Gain 57.7, plate noise 394 nV/√Hz, so 6.84 nV/√Hz input-referred.
- Without the source's 4.07 nV/√Hz that leaves 5.5 nV/√Hz. Over 30 Hz–8 kHz with a 390 Ω source this is 0.54 µV,
  against 0.59–0.77 µV measured on three 12AX7 brands under the same conditions (HomeTheaterHifi). That is 0.8–3 dB
  under, and the article names flicker (off by default here, §3.9) as dominant.

**Op-amps.**
- A TL072 follower reads en·√(1 + 100/f) within ±0.25 dB from 20 Hz to 10 kHz.
- A JRC4558 follower on 1 MΩ reads en, in·1 MΩ (with its 1/f corner) and the 1 MΩ thermal noise within
  −0.02/+0.29 dB from 30 Hz to 10 kHz.

**Hum.** Same 12AX7 stage plus a 300 V 60 Hz mains source.
- With the typical defaults (1 GΩ ∥ 5 pF) the cathode carries 0.113 µV at 60 Hz; theory gives 0.113 µV
  (0.445 V·|1/rhk + jωchk| into Zk ≈ 119 Ω).
- An explicit netlist with the same parts gives the same amplitude and phase, phase-locked to the mains source.

**Idle at the speaker.** Default controls, 176.4 kHz, input shorted by the source, 20 Hz–20 kHz, mV rms. "Hum" is the
mains harmonics; "heater" is the part the heater adds, √(on² − off²).

| Amp | Hiss (A-wtd) | Hum total | Of which heater, typical | Heater, worst case (before) | Hiss re rated output (assumed nominal load) |
|---|---|---|---|---|---|
| Twin Reverb | 3.36 (2.54) | 2.03 | ≈ 0 | 2.0 | about −75 dB |
| Deluxe Reverb | 5.46 (3.47) | 1.89 | 0.3 | 1.4 | about −68 dB |
| 5F6A Bassman | 2.76 (1.82) | 0.042 | 0.040 | 1.7 | about −70 dB |
| 1959 Plexi | 65.4 (52.6) | 0.88 | 0.88 | 15.1 | about −56 dB |
| JCM800 2203 | 67.9 (47.6) | 0.96 | 0.96 | 56 | about −55 dB |
| AC30 (50 Hz) | 18.3 (14.2) | 35.2 | ≈ 0 (the ripple dominates) | 4.5 | about −62 dB |
| Supro 1624T | 0.64 (0.61) | 6.24 | ≈ 0 | 2.0 | about −87 dB |
| DR103 (50 Hz) | 3.38 (2.16) | 9.25 | ≈ 0 | | about −80 dB |

The hiss differs from the first version of this table (Plexi 76, Twin 4.2 mV) because of the 12AX7 card refit.

**Against published figures.** Few exist for these exact amps. Searches of the Fender, Marshall and Vox manuals and
spec sheets found no hum-and-noise rating.

| Quantity | Ours | Published | Source |
|---|---|---|---|
| 12AX7 input noise, 30 Hz–8 kHz, 390 Ω source | 0.54 µV | 0.59–0.77 µV (JJ, Sovtek, Tung-Sol) | HomeTheaterHifi, "Dual Triode Vacuum Tubes 12AX7 and 12AU7" |
| Preamp triode input noise, rule of thumb | 0.61 µV (1 kΩ source, 30 Hz–8 kHz) | about 1 µV; high-gm types under 0.5 µV | Blencowe (*Designing Tube Preamps*), as quoted on Music Electronics Forum |
| Healthy 12AX7 heater-cathode leakage | 1 GΩ ∥ 5 pF (default) | > 100 MΩ (10 of 10 vintage tubes); MIL 12AT7WA > 100 MΩ new; population 20 MΩ – 10 GΩ | diyAudio "Vhk(max) rating"; UK Vintage Radio |
| Heater hum in a healthy amp | 0.04–0.96 mV at the speaker; under the hiss and ripple everywhere | "c-h capacitance dominates the hum of typical 12AX7s"; a leaky valve (2 MΩ) lifted hum 15 dB above the noise floor | diyAudio; GroupDIY "heater cathode leakage" |
| Small Fender combo idle (room) | Deluxe at volume 0.5: about 42 dB(A) SPL at 1 m (3.47 mV A-wtd into 8 Ω, about 100 dB/W/m speaker, estimated) | Vibrolux at volume 0: 5.4 dB above a 39 dB room (about 44 dB, distance not given) | jazzguitar.be forum, "Tube amp noise floor" |

The amp-level comparisons are order-of-magnitude only: the published conditions (controls, distance, speaker) are not
the same.

**Pedals and studio units (BBD-switch default, noise and hum on).**
- All 31 units without tubes render bit-identically with and without `hum`. No heater means no hum.
- Tube units without a mains source (AB763 Normal, Butler Tube Driver, LA-2A) are also hum-free.
- Idle output, 96 kHz, 20 Hz–20 kHz, µV rms:

| Unit | Idle | Unit | Idle | Unit | Idle |
|---|---|---|---|---|---|
| TS808 | 2.0 | TS9DX | 6.7 | Bluesbreaker | 2.1 |
| Fuzz Face (Ge / Si) | 4.4 / 4.5 | Rangemaster | 4.2 | Dyna Comp | 2.5 |
| DS-1 | 6.8 | RAT 2 | 16 | Klon | 35 |
| Big Muff | 324 | Memory Man | 151 | RE-201 | 93 |
| 1176LN | 88 | LA-2A | 60 | CE-1 | 4.7 |
| Crybaby | 23 | Phase 90 | 22 | Uni-Vibe | 2.6 |

- The diode-clipper test circuit reads 0: it is a single WDF group, which has no noise (§3.9).
- No published idle measurement of a TS808 or the other pedals was found to compare against.

**CPU.** Instructions retired, `circuit_cli render`, 4 s guitar DI, whole circuit at one rate.

| Netlist | Off | Noise | Noise + hum | Rate |
|---|---|---|---|---|
| Twin Reverb | 29.7 G | +6.8 % | +8.1 % | 4x (176.4 kHz) |
| JCM800 2203 | 32.9 G | +5.0 % | +6.2 % | 4x |
| 1959 Plexi | 36.2 G | +4.9 % | +6.1 % | 4x |
| DR103 | 46.1 G | +4.5 % | +5.1 % | 4x |
| 5F6A Bassman | 25.6 G | +6.4 % | +7.6 % | 4x |
| Twin / JCM800 / Plexi | 30.2 / 29.7 / 31.9 G | +12.4 / +11.7 / +10.5 % | +13.6 / +12.8 / +11.8 % | 2x (88.2 kHz) |
| TS808 / RAT 2 / DS-1 / Klon | 5.9 / 6.0 / 9.9 / 8.6 G | +25.6 / +19.5 / +18.2 / +27.0 % | the same | 2x |

Cost of each item:
- **Op-amp en/in with their 1/f banks:** the TS808 went from +19.9 % to +25.6 % at 2x.
- **Typical heater leakage:** hum stays at +0.6–1.3 % (two heater classes: triode and pentode time constants differ).
- **Per-instance seeds:** no cost.
- **Tube flicker:** off by default, so no cost.
- **Live LTE tolerance 2 (§10.16):** 2–5 % less.

Pedals pay the most relative to their own small cost: the TS808's +1.5 G is about 5 % of one Twin. Cycle counts, best
of one or three under load from other jobs, follow the instructions within about ±10 points.

**Tests.** With noise and hum on in the unit and `HEADROOM_NOISE_FIXED_SEED` set:
- test_circuit_engine, test_circuit_unit and test_param_drag pass.
- The SPICE gate output is unchanged.
- test_aliasing reads 30 of 32 below −60 dB. The JCM800 (two tables) is at −58.2 dB, waiting for its rate picks.

### 10.23 Vibrato wiring: the AB763 roach driver and the AC15 Vibravox (2026-11-10)

**AB763 Twin Reverb and Deluxe Reverb: the lamp followed the playing with the vibrato off.**
- **Symptom.** With the footswitch open, the roach light swung 0.006–0.82 (Twin, 110 Hz bursts at volume 0.8)
  against 0.04 in silence. At a steady 0.1 V tone at volume 0.8, the vibrato node's envelope rippled 9.9 dB p-p
  (Twin). At 0.5 V and volume 0.3 it rippled 8.0 dB (Twin) and 9.0 dB (Deluxe). Playing dynamics were modulating
  the LDR.
- **Cause.** V5B (the neon driver) had its grid on `ot`, the 0.02 µF / speed-rheostat node, whose DC return is
  ground. The driver idled lit, and the B+2 ripple of the playing signal reached its grid through the 0.02 µF cap
  (`ot` swung ±8–15 V).
- **Schematic.** Both factory drawings take the driver grid from the junction of the two 0.01 µF caps and the
  1 MΩ leg (`om`). Its DC return is that 1 MΩ to the footswitch node, which sits at the bias supply through 2.2 MΩ
  when the switch is open. With the switch open, the oscillator and the driver are both cut off. Sources: the
  Fender "Twin Reverb-Amp AB763" and "Deluxe Reverb-Amp AB763" schematics (§11).
- **Fix.** One node per netlist: V5B grid `ot` → `om`.
- **After.**
  - Vibrato off: light ≤ 1e-15 at every drive. Envelope ripple at the vibrato node ≤ 0.05 dB, against the previous
    0.4–9.9 dB.
  - Vibrato on, speed 0 / 0.5 / 1: Twin 3.75 / 7.31 / 11.5 Hz (was 4.35 / 7.97 / 11.75), Deluxe 3.73 / 7.32 /
    11.5 Hz. Typical AB763 is about 3–10 Hz (sluckey, The Amp Garage forum, "Ab763 tremolo frequency range").
  - Intensity 0 / 0.5 / 1 at a 0.1 V tone gives an envelope swing of 0.01 / 8.5 / 15.9 dB (Twin) and
    0.01 / 8.7 / 15.5 dB (Deluxe).
- **Level.** The idle glow (light ~0.04) had been shunting the vibrato channel, so the DI riff rose (clean 0.05 V
  peak / driven 0.5 V peak). `outputVoltsPerFS` was retuned on both amps:

  | amp | rise before retune (clean / driven) | outputVoltsPerFS | after retune (clean / driven) |
  |---|---|---|---|
  | Twin | +2.2 / +1.83 dB | 61.672 → 76.131 | +0.36 / 0.00 dB |
  | Deluxe | +2.00 / +0.80 dB | 94 → 110.44 | +0.60 / −0.60 dB |

- **Fixtures (all regenerated).**
  - DC: `p5b` 403.3 → 472.5 V (Twin) and 373.9 → 442.6 V (Deluxe), and `k5b` 5.1 → 0 V (the driver is now cut off).
    No other node moved more than 0.02 V.
  - AC at the speaker: +3.5 dB at 20 Hz and +2.3 dB at 1 kHz (Twin); +3.4 / +2.2 dB (Deluxe).
  - Transient speaker rms: twin_clean +2.88 dB, twin_drive +0.32 dB, twin_dimed +0.02 dB, deluxe_clean +0.54 dB,
    deluxe_drive +0.03 dB.
  - Engine against the new references: twin_clean 0.193 %, twin_drive 0.170 %, twin_dimed 0.387 %, deluxe_clean
    0.202 %, deluxe_drive 0.146 % NRMSE; twin_ac 0.090 / 0.169 dB, deluxe_ac 0.045 / 0.104 dB; both DC sets at
    0.001 of tolerance. Multirate: twin 0.657 / 0.250 %, deluxe 0.569 / 0.227 %.
  - The Twin host-rate AC check's ≤ 2 kHz bound moved from 0.1 to 0.11 dB. Its worst point is the band edge at
    a 44.1 kHz host (bilinear warping of the host-rate preamp; the whole circuit at 2x is 0.054 dB there), and it
    went from 0.091 to 0.102 dB once the LDR no longer loads the vibrato node.

**AC15: the Vibravox never oscillated.**
- **Cause.** The netlist's oscillator was not the factory circuit. Its "phase-shift network" put R45 1 MΩ in
  series to the grid and had a single 47 kΩ shunt leg, plus two speed pots from the grid to that leg. Driven from
  the plate, the network's phase stays between +83° and −2° from 0.5 Hz to 1 kHz, so the loop around the
  inverting stage never reaches 360°. No loop gain or start kick could make it oscillate.
- **What else was fabricated.**
  - The "modulator" was V4B with its plate on V4A's cathode (a misread pin 3).
  - The vibrato channel had one V1 triode, the five-section ladder before V4, and V4A with 180 kΩ / 1.5 MΩ.
  - V4A's plate fed V5B's grid through a fabricated C25 33 nF.
  - The 210 V node hung off the screen rail through R10.
- **Rebuilt to the factory drawing V-1-5**, with the Sluckey redraw for the vibrato channel (§11):
  - V6 pins 1–3: a phase-shift oscillator, R61 270 kΩ, R59 3.9 kΩ ∥ 25 µF. Ladder C42 / C41 / C40 0.01 µF;
    R68 and R65 1 MΩ go to the footswitch return SW6. SW5 picks the middle leg: R64 1 MΩ, R66 680 kΩ or
    R67 330 kΩ.
  - LFO path: R50 220 kΩ / C34 0.1 µF / C31 0.1 µF / R45 1 MΩ into the depth node (C35 0.02 µF; R46 500 kΩ
    preset + R47 150 kΩ), then C37 into V6 pins 6–8. Those pins are a split-load LFO phase splitter (R58 22 kΩ,
    R55 1.5 kΩ + R52 22 kΩ, R51 560 kΩ).
  - The two LFO phases (C29 / C30, R31 / R34 470 kΩ, R32 / R33 1 MΩ) drive the V4 ECC82 pair (12AU7 card). The
    pair has a common plate node (R36 / R37 10 kΩ, R35 47 kΩ ∥ C25 0.033 µF, R10 12 kΩ from the 210 V node) and a
    shared unbypassed 1.5 kΩ cathode.
  - V1 pins 6–8 is a split-load driver feeding the HI path (to V4A) and the LO path (to V4B). SW4 grounds the LO
    path through C22, which gives tremolo.
  - The output ladder (5 × 0.005 µF / 1 MΩ) feeds R30. R7 22 kΩ from +315 V feeds the 210 V node (C1 32 µF).
- **Controls.**
  - `vibratoSpeed` is now a Slow / Medium / Fast selector (SW5).
  - `depth` is the R46 preset.
  - `effectsSelector` is Off / Tremolo / Vibrato (raw 0 / 1 / 2, paramRange [0, 2], matching the engine catalogue's
    labels for the legacy parameter). Tremolo closes SW4. Off opens SWE, a switch in series with SW6 that stands in
    for the legacy Off position; it is not on the drawing.
  - New `footswitch` (SW6), default open, as on the Twin and Deluxe.
  - New `input` (Normal / Vib-Trem / Both (jumpered), default Normal), see the V5 rebuild below.
  - `tremoloSpeed` stays legacyOnly: the amp has one oscillator.
- **Readout.** `"lfo": {"node": "p6", "minSwing": 5}`.
- **Start kick.** VKICK sits in series with the speed resistors' ground return. Fed through a resistor into the
  grid, it would also be a DC grid return and keep the oscillator running with SW6 open (ENGINE_QUEUE 36).
- **Measured.**
  - Speed Slow / Medium / Fast: 5.9 / 6.6 / 8.1 Hz.
  - Plate swing 109–127 V p-p, full after ~1.3 s from the kick.
  - LFO at the V4 grids: 6.9 / 14.4 / 19.1 V p-p at depth 0 / 0.5 / 1.
  - Footswitch open: the plate moves 6 mV, and < 0.03 V with a riff played hard at full volume.
  - Through the vibrato channel (scratch copy with the input on it, 440 Hz at 50 mV):
    - vibrato: 24 / 51 / 69 cents p-p of pitch swing at depth 0 / 0.5 / 1;
    - tremolo: 14 / 31 / 31 dB of level swing;
    - footswitch open: 0.4 cents.
  - No Hz figures are published for the three switch positions. The later 3M-RA speed-pot version is quoted at
    3–10 Hz (Hoffman forum).
  - DC against the drawing and redraw: 210 V node 201.5 V (drawing 210, redraw 202); V4 plates 71.2 V (redraw
    71), cathode 3.05 V (redraw 3.2); V6 pins 6–8 182 / 20.9 V (drawing 200 / 18.5).
- **Superseded the same day: V5 rebuilt, and the vibrato channel joined as drawn.** At first the vibrato channel was
  left unjoined. The netlist's V5 was a gain stage plus cathodyne, and C28 on V5B's grid loaded the normal channel
  by 2.6 dB clean. V5 was then rebuilt as drawn.
- **V5.** V5 is now the drawing's long-tailed pair:
  - V5A's grid takes the normal channel (C27 from R29). V5B's grid takes the vibrato channel (C28 0.01 µF from
    R30).
  - Cathodes go through R42 1.2 kΩ to the tail node, then R38 47 kΩ (it was 1 kΩ) to ground. R40 / R41 1 MΩ return
    the grids to the tail node.
  - R44 / R48 100 kΩ plates come from R43 22 kΩ / C26 8 µF off the +315 V node. C32 / C33 couple the plates to the
    EL84 grids.
  - Removed: the cathodyne's tail and cathode resistors, the extra V5A grid leak, and the V5A-to-V5B coupling cap.
  - DC: plates 214.7 V (drawing 220), cathodes 59.5 V and tail node 58.0 V (redraw 61 / 59).
- **Input.** `input` is a selector driving two switches from the new input node `in`: SJN to the normal jack J1
  (`in1`) and SJV to the vib/trem jack J3 (`in3`), both closed for "Both (jumpered)".
  - 440 Hz at 50 mV with the footswitch on: Vib-Trem input with Vibrato gives 51 cents p-p of pitch swing; with
    Tremolo, 30 dB of level swing.
  - Normal input: 0.1 cents (no effect, as on the real amp). Both: 7 cents, because the louder normal channel
    dominates.
- **Face.** Controls with no legacy parameter never reached a face, because the rack lists only catalogue
  parameters and `IComponent::extraParams`.
  - New netlist control flag `"face": true` (§2.1). CircuitUnit exports such a switch or selector as a stepped
    extra parameter, and binds its raw value as the option index. The rack maps the UI's norm to
    round(norm × (n − 1)).
  - The flag is opt-in, so only the AC15 changes.
  - Checked in the offscreen WKWebView harness against a mock serving the engine's slot JSON. The illustrated face
    shows "+2 more", and the drawer has Input (Normal / Vib-Trem / Both) and Vib-Trem on (Off / On).
  - Clicking Vib-Trem and On sent `input` norm 0.5 (raw 1) and `footswitch` norm 1, and the face updated.
  - test_circuit_unit checks the export and that input index 1 reaches the vib jack.
- **Normal-channel change from the corrected V5.** The riff was measured against the unjoined interim version,
  same scale:

  | band | clean (0.05 V peak) | driven (0.5 V peak) |
  |---|---|---|
  | level | +1.06 dB | +0.93 dB |
  | 40–150 Hz | +1.5 dB | +1.4 dB |
  | 150–500 Hz | +2.4 dB | +1.5 dB |
  | 0.5–1.5 kHz | −0.2 dB | +1.0 dB |
  | 1.5–5 kHz | −3.9 dB | −1.1 dB |
  | 5–12 kHz | −9.5 dB | −2.9 dB |

- **Level.** `outputVoltsPerFS` 116.16 → 130.17. Against the release before today the DI riff is +0.81 dB clean and
  −0.80 dB driven.
- **The top cut was wired backwards (fixed in the third round, below).** CUT is `[g7, cutw, cutw]`, so R49 = 250 kΩ·taper(x):
  position 0 is maximum cut, although the control's note says "0 = cut off". The legacy default 0.5 therefore sits
  near full cut.
  - With the cathodyne this was masked, because one side of the network was the low-impedance cathode. Across the
    pair's two 100 kΩ plates it bites, and it causes most of the treble loss above.
  - Flipping it to `[cutw, cutw, g7]`, against the interim version at the unit default, gives +2.0 dB clean,
    +2.6 to +3.3 dB at 150 Hz–5 kHz and −1.9 dB at 5–12 kHz.
  - With the flip, the aliasing figure becomes −59.2 dB. Left for a decision.
- **Groups.** The vibrato section stays its own 12-port group through the cut at `vt` (load C6). The ladder, R30 and
  C28 join the amp group (14 ports).
- **Aliasing.**
  - At the gate's default controls: −67.3 dB at 12.8 kHz with no rate change. This is report-only for this unit,
    which has an LFO readout. It was −60.6 dB before today and −58.7 dB in the unjoined interim version.
  - At volume 1: −56.1 dB, and −53.6 dB with cut 1. The release before today was −44.7 / −47.5 dB there.
  - autorate for that class picks the amp group at 16x: −69 dB, but CPU goes from about 65 % to 140–160 % of a core
    per channel, which cannot run in real time. Not applied.
- **CPU.** Unit path, single thread, noisy machine: about 67–73 % of one core against 61–69 % for the interim
  version.
- **Fixtures (all four regenerated).**
  - DC: 89 nodes. `p5a` 160.6 → 235.5 V and `kn` 0.8 → 62.8 V at the fixture's DC point; the screen node moves
    +8.6 V.
  - Speaker AC at the netlist defaults (cut 0 = full cut): +6.4 dB at 20 Hz, +2.1 dB at 100 Hz, −2.5 dB at 1 kHz,
    −12.6 dB at 5 kHz.
  - Transient speaker rms: clean +3.0 dB, drive +1.2 dB.
  - Engine against the new references: ac15_clean 0.002 %, ac15_drive 0.004 %; AC 0.019 / 0.139 dB; DC 0.000 of
    tolerance.

### 10.24 Ampeg SVT 1969 (54000): a transformer-fed supply, bound (2026-11-10)

The SVT's supply is behind its power transformer (P.T. 8930007 as a `transformer`, lm 40 H at 2 Ω), the first such
netlist (wave-e, `work/circuit-netlists/wave-e/ampeg-svt-1969.notes.md` sessions 2b-2d). Bound as 54000 →
`ampeg-svt-1969`, with `"minRate": 176400` (aliasing −58.9 dB at 2x, −66.7 dB at 4x). Method: §3.10.

**What was wrong.** `settleMs > 0` re-solved the transformer's group at DC after running the mains, so every positive
value ended at the same crest charging state (bpa 836 V, screens 243 V, 60 mA per tube; 5, 100, 600 and 2000 ms gave
byte-identical renders), and `settleMs = 0` started from a DC point with 85 A in the primary and the core 29.7 V·s
deep in saturation; a run from there diverged after 11 s (the flux node to −4e6, then Newton failures). Neither is
ngspice's `.tran … uic`; that the old "cold start" comparisons agreed to 1.5-3.7 % at all was luck of the window.

**Netlist re-read (wave-e notes, session 3).** The settled rails sat above the drawn ratings of capacitors the netlist
put on them (bpb 529 V on the 70 µF and 40 µF 450 V parts, bpd 447 V on a 90 µF/200 V and a 200 µF/250 V, the screen
rail 202 V on a 90 µF/200 V). Neither sheet prints a rail voltage; the capacitor ratings are the evidence. Re-read at
1:1: the 100 µF/450 V is the upper half of the reservoir and the 70 µF/450 V ∥ 40 µF/450 V the lower half (220K 2 W
across each); the 8.2k 15 W output carries only the 30 µF/600 V and feeds the 12BH7 driver plates, a 15K 1 W to the
power-amp 12DW7's 40 µF/450 V rail and (preamp sheet) an 8.2K 2 W to the preamp's 40 µF/400 V rail; the 200 µF/250 V is
the only screen-rail cap and the 90 µF/200 V the only bias-rail cap. The power amplifier was also re-transcribed: a
12DW7 input stage (220K, global feedback 47K ∥ 120 pF into its 220 Ω cathode resistor), a 12DW7 cathodyne, the 12BH7
drivers, a 12BH7 cathode follower per bank driving the 6146B grids (bias divider 22K / 15K lin / 39K), 10 Ω 5 W plate
resistors (an earlier read had them as 10 µF screen bypass capacitors), one 1 Ω sense per bank and the drawn 4 Ω load
(the OT turns were fitted to it; the netlist had one 8 Ω block). A 22K DC stand-in on the preamp rail carries the
current of the preamp's unmodelled V3–V5 stages (disclosed in the netlist). Bias winding refitted so the Bias trims sit
centred at the datasheet idle.

**Settled operating point** (96 kHz, controls at the netlist defaults; the 60 s cold run of the same engine in
brackets; the capacitor on that node, the sheet or the RCA 6146B datasheet last):

| node | settled | 60 s cold run | rating / target |
|---|---|---|---|
| bpa (B+) | 695.29 V, 15.6 Vpp ripple | 695.29 V | 690 V (+0.77 %) |
| bpm (reservoir midpoint) | 347.6 V | | 450 V caps above and below |
| bpb (after the 8.2k 15 W) | 418.00 V | 418.00 V | 30 µF/600 V; 33.8 mA, 9.4 W in the 8.2k |
| bpc (power-amp V1, after 15K 1 W) | 317.93 V | 317.93 V | 40 µF/450 V |
| bpd (preamp, after 8.2K 2 W) | 274.77 V | 274.77 V | 40 µF/400 V; 17.5 mA, 2.5 W in the 2 W resistor |
| bscn (screens) | 200.51 V | 200.51 V | 200 µF/250 V; datasheet 200 V |
| biasn | −159.92 V | −159.92 V | 90 µF/200 V (winding fitted) |
| 6146B grids | −47.9 V | | −47 / −48 V |
| cathode sense kku / kkl | 73.2 mA per bank, 24.4 mA per tube | 24.4 mA | 24 / 23 mA |

The drift over the first 0.1 s is ≤ 0.3 mV on every rail; the speaker carries 3 mV of hum at the start. The 60 s
cold run at 96 kHz had one Newton failure at 5.0 s (the bridge diodes, group 1; it recovered and ended on the settled
values); cold starts at 88.2 and 176.4 kHz (8 s) and 8 s from the settled start had none. (Since 2026-11-11 a 60 s cold run has none, at a fixed step or with LTE: see "Convergence at a fixed step" below.)

**SPICE gate** (`test_circuit_engine`; ngspice options for the SVT in the harness: BDF, itl4 500, reltol 1e-4, node
shunts 10 GΩ with the transformer, 100 GΩ without):

| case | start | result | gate |
|---|---|---|---|
| svt_clean (10 mV, 220 Hz) | cold, 0.1-0.2 s | spk 0.08, pau0 1.03, bpa 1.02, bscn 0.09, biasn 0.09 % | 1.5 % |
| svt_drive (0.2 V, 110 Hz, volume 1) | cold, 0.1-0.2 s | spk 0.07, pau0 0.11, bpa 1.14 % | 1.5 % |
| svt_set_clean | `mains_dc` variant, DC | spk 0.004, pau0 0.004, p1a 0.03, p2 0.02 % | 1 % |
| svt_set_drive | `mains_dc` variant, DC | spk 0.01, pau0 0.02, p2 0.02 % | 1 % |
| svt_ac | `mains_dc` variant | 0.057 dB to 5 kHz, 0.358 dB 5-10 kHz | 0.1 / 0.5 dB |

(Fixtures regenerated for the re-read netlist; the figures before it were spk 0.56 / pau0 0.90 / bpa 0.92 % clean
cold, and 0.023 / 0.225 dB AC.)

With ngspice's shunts at 1 GΩ the cold cases read twice the rail error (bpa 1.26 %), and the settled case's 220k-loaded
plate p1a moved by 50 mV (32 % NRMSE on its 0.4 Vpp), so the shunts are as large as ngspice still converges with.
Harmonic deltas H2-H5 ≤ 0.02 dB in every case.

**Other netlists.** Only a `transformer` winding on a mains source switches the new path on (`Circuit::mainsCore`), and
no other library netlist has one: all 71 render bit-identically at `--settle 0` and `--settle 600` (CSV compare against
the engine before this change), and the whole SPICE gate is unchanged.

**CPU and prepare.**
- **Audio.** `circuit_cli bench --table 54000` (the unit's 192 kHz at a 48 kHz host, guitar-like chord): 14.7 µs per
  internal sample, **about 2.8 cores** (default and volume 1 alike; 3.7 Newton iterations per sample, 0 failures). At
  2x (96 kHz) it was 12.8 µs, 1.2 cores. The re-read netlist (49 NL ports in the main group, was 42) benched 17.1 µs
  against 10.4 µs for the previous one in the same run (load average about 17): about 1.6× the cost, 3.3 cores. Most of that was homotopies; since the convergence fix and the output-stage fold (below) the unit costs 1.4 cores. Wall-clock under a load average of about 14, so pessimistic, but this unit
  does not run in real time on one core: the whole amplifier is one 42-port group (123 unknowns), which is CPU work
  for CPU work, not a correctness hold (kPendingValidation is for correctness only).
- **Prepare** (both channels, off the audio thread; `circuit_ab --cpu 54000` at a 44.1 kHz host): 1.2-1.4 s, serial or
  with the two-core channel build (the Plexi's read 2.2 s in the same tool, under more load).
  The cold settle runs at 88.2 kHz and is confirmed at 176.4 kHz (about 1 s instead of 2.4 s); the second reset and the
  second channel (`settleFrom`) take two mains periods each. The old settle cost about 1.6 s per reset, four per
  prepare, and ended in the crest state.

**Open.**
- **Level staging (done, session 3).** At the catalogue defaults a 0.35 FS-peak DI had driven the old netlist to
  +3.7 to +5 dBFS with −115 to −192 / +57 V at the speaker. That excursion was not a physical flyback: with only the 4 Ω
  load restored it fell to −68 V, and with the drawn cathode-follower drive and feedback the output clips symmetrically
  at ±50 V (300 W into 4 Ω is 49 V peak). Dimed, DIs and 41-1000 Hz sines peak at 47-58 V, so `outputVoltsPerFS` is
  87 (1.5 × 57.7 V). Default-control DIs now peak at −4.8 to −4.3 dBFS at the unit rate; test_circuit_unit reads
  0.272 rms (was 0.476; the amps there read 0.04-0.28).
- The 8.2K 2 W preamp dropper dissipates 2.5 W; that rests on the stand-in for the preamp's unmodelled stages.
- The `"settled"` SPICE start (ngspice seeded with the engine's state) is implemented and unused (§3.10).
- `settleMs = 0` (the DC point) of a transformer-fed supply still holds the mains at its crest: 85 A in the primary.
  Only `--settle 0` and the `.op` comparison use it; the SVT has no `.op` case.

**Convergence at a fixed step, and the output stage folded (2026-11-11).**
- **Symptom.** At a fixed step the netlist did not converge. From the settled state, 0.05 s of DI took 6959
  homotopies and 6892 outright Newton failures, every other sample. The cold settle itself diverged: a bridge diode
  was accepted at 1.68 V junction (4.6 MA), the states exploded, and prepare ran for more than 10 minutes. With live LTE
  it converged, but with about 260-320 homotopies per second. The folded netlist failed every sample from its cold
  start (output 0).
- **Cause: the HT winding floats.** Nothing anchors `sa`/`sb` but the four bridge diodes: no capacitor, and the
  winding is differential. With all four off, which is most of each mains cycle, the winding's common mode is set
  only by nA junction leakage, behind port impedances of 2.5e8 Ω (the 1 GΩ linear split). It moves by hundreds of
  volts from one sample to the next. The linear predictor extrapolated those moves into +290 V forward bias on a
  junction (pnjlim limits Newton steps, not the start). From forward bias, voltage-space Newton comes down only one
  nVt per iteration toward the small current a floating junction carries: 15-40 iterations against a limit of 32,
  then homotopy, PTC and failure. The cold start had the same problem: its first Newton start puts every diode at
  `p` (412 V forward at the mains crest). The cuts are not involved. Every supply cut's loop gain is ≤ 0.02, `biasn`
  is already demoted (its diodes' 30 mΩ count as loads), and `bpc` is 9e-6 behind its 40 µF.
- **Engine fix (general; §3.3 Newton).**
  1. The predicted start is pnjlim-limited against the last solution on every junction port (SPICE's MODEINITPRED
     rule), also on LTE substeps, and from 0 V on a cold start.
  2. `kLimPnjF`: diode ports on islands that float even through capacitors take their downward Newton steps in
     current (the inverse law).
  Neither changes a fixed point.
- **Result, unfolded** (2 s of the rock DI, and a dimed 0.3 V 110 Hz sine, at the unit's 192 kHz; wall time
  includes about 1 s of prepare):

  | run | before | after |
  |---|---|---|
  | fixed step (`CE_LTE=0`), DI / sine | did not finish the settle in 10 min | 0 / 0 homotopies, 6.5 / 6.6 s, max 23 iterations |
  | live LTE, DI / sine | 519 / 636 homotopies, 8.8 / 11.1 s | 0 / 0 homotopies, 5.7 / 6.2 s |
  | 60 s cold at 96 kHz, LTE / fixed step | (earlier: one failure at 5 s) | 0 homotopies, 0 failures in both |

  The live output is the same solution: −121 dB (DI) and −144 dB (sine) against the old engine's render.
- **The fold.** In each bank the three 6146B are now one part with `"parallel": 3`, and the 47K grid, 22 Ω screen
  and 10 Ω plate stoppers are divided by 3. The drawing gives each valve identical resistors between the same nodes,
  so the fold is exact: −169 dB against the unfolded netlist at a fixed step with noise off. With LTE it reads −92 dB,
  because the substep decisions differ.
  - The main group goes from 49 to 37 NL ports.
  - Unfolded, the old engine's Newton happened to pass svt_drive and the folded netlist failed it. With the fix, both
    read the same figures to three digits.
- **Gate.** The whole SPICE gate is unchanged. Every case, the SVT's included, reads the same to the printed digits;
  the only difference is svt_set_drive p2's max error, 9.71e-2 → 9.70e-2 V.
- **Library null** (every netlist, 0.5 s DI at 88.2 kHz, against the engine before the fix):
  - 43 render bit-identically.
  - 28 differ at −138 dB or lower: the Rangemaster −138 dB, the 5150 −150 dB, the rest −156 to −241 dB. The
    predictor limit changes the start where an extrapolation went up a junction, so iterations drop: the 5150's worst
    sample goes from 124 to 19 iterations, the JCM800's from 25 to 12, the Mu-Tron's from 143 to 32. The Mu-Tron's
    2 homotopies and the microsynth's 1 are gone.
  - test_circuit_engine, test_circuit_unit, test_aliasing (33/33), test_param_drag and test_effect_handoffs pass.
    The SVT's alias figure is −82.3 dB before and after.
- **CPU**, with a load average of 7-10:

  | measure | before | fix, unfolded | fix + fold |
  |---|---|---|---|
  | `circuit_cli bench --table 54000` (3 s, min of 3) | 365 % of a core (19.0 µs per sample, 3064 homotopies) | 186 % (9.7 µs) | **141 % (7.3 µs)** |
  | `circuit_ab --cpu 54000` (4 s DI, 44.1 kHz) | 350 % (973 homotopies) | 199 % | **138 %** |
  | `circuit_cli alias` gate tone | 571 % | | 352 % |

**AC15, third round: Tone Cut, the EL84 screens and the OT centre tap as drawn; face switches on nine more units.**
- **Tone Cut.** CUT is now `[cutw, cutw, g7]`: 0 = no cut (R49 250 kΩ), 1 = full cut, as the note always said.
- **Power stage as drawn.**
  - The EL84 screens go through R62 / R63 100 Ω from the +315 V node.
  - The OT centre tap (and the winding-capacitance returns) moves from the reservoir to the +315 V node after the
    choke.
  - EL84 cathodes: R60 130 Ω / C39 50 µF (the drawing's refs; they were RK / CK 25 µF).
  - Removed (not on the drawing): RD2 16 kΩ, C210, the "R60 5 kΩ screen dropper" and the 50 µF screen cap.
- **Supply fitted to the drawing's labels.**
  - Choke DCR RL1 300 → 110 Ω: 325 V → 315 V at the drawn currents (still unverified: no DCR is drawn).
  - HT 346.48 → 375.5 V peak per half, 265.5 Vrms.
  - Running state: reservoir 324.6 V (drawing 325), +315 V node 314.6 V (315), EL84 plates 308.5 V (310), screens
    314.2 V, V5 plates 222.9 V (220), vib/trem node 210.7 V (210), EL84 cathodes 10.9 V.
- **Cuts.** `["sa1", "sb1", "bpb"]` no longer works.
  - With the centre tap and the screens on the +315 V node, the DC cut iteration between the reservoir group and the
    capacitor-only owner of `bpb` diverged (1e213 V).
  - Dropping `bpa` from the list compiled, but put the choke in the rectifier's Newton group: 0.84 % / 2.4 % NRMSE
    against ngspice (ac15_clean / ac15_drive).
  - Dropping the `bpb` cut is exact (0.013 % / 0.045 %), but merges the amp into one 26-port group.
  - Shipped: `["sa1", "sb1", "bpa", "v210", vt→C6]`. The vib/trem rail `v210` is the supply cut that keeps the
    vibrato section separate (cut loop gain 4e-5). Groups: 12 + 14 ports.
- **Sound change**, against the V5-rebuilt version at the same output scale:

  | change | clean, level | driven, level | per band |
  |---|---|---|---|
  | Both changes | +3.06 dB | +2.98 dB | clean +0.7 / +2.5 / +5.6 / +8.2 / +9.4 dB; driven +1.2 / +2.8 / +5.2 / +5.6 / +5.8 dB |
  | Power stage alone | +0.94 dB | +2.57 dB | driven +2.4 / +3.0 / +3.1 / +0.9 / −0.8 dB |

  The bands are 40–150 Hz, 150–500 Hz, 0.5–1.5 kHz, 1.5–5 kHz and 5–12 kHz. The power stage adds more power and
  headroom; the treble comes from the Tone Cut fix.
- **Level.** `outputVoltsPerFS` 130.17 → 184.36. Against the release before today the riff is +0.85 dB clean and
  −0.85 dB driven.
- **Aliasing.** At the gate's defaults it is −70.0 dB, with no rate change needed. At volume 1 it is −54.0 dB
  (informational).
- **CPU.** About the same: 2370–2530 ns/sample against 2350–2600 ns/sample, unit path at 192 kHz.
- **Fixtures (all four regenerated).**
  - DC: `bpb` 331.7 → 349.1 V at the fixture's DC point; `s7` / `s8` replace `scr` / `bpc`.
  - Speaker AC at the netlist defaults (cut 0 is now no cut): +0.2 dB at 20 Hz, +0.6 dB at 100 Hz, +7.4 dB at 1 kHz,
    +19.7 dB at 5 kHz, +25.1 dB at 10 kHz.
  - Transient speaker rms: clean +2.1 dB, drive −0.1 dB.
  - Engine against the new references: 0.002 % / 0.004 % NRMSE; AC 0.042 / 0.071 dB; DC 0.001 of tolerance.
- **Face switches on nine more units** (ENGINE_QUEUE 37). These controls now carry `"face": true`, with option
  labels:

  | unit | controls |
  |---|---|
  | JCM800 (both bindings) | `input_high` Low / High |
  | Uni-Vibe | `vibrato` Chorus / Vibrato, `cancel` Effect / Cancel |
  | Dimension D | `inmode` Stereo / Mono |
  | RE-201 | `mode` (12 positions), `echo_normal` Normal / Echo |
  | CE-1 | `sens` Low / High, `stereoPlug` Mono / Stereo (wet only) |
  | SLO-100 | `crunch` Clean / Crunch, `bright` Normal / Bright |
  | Surveyor | `input` −33 dB / +12 dB |

  - CircuitUnit seeds each one at its netlist default's option index (constructor), so the face shows the real
    position.
  - Defaults are unchanged, so the circuits get the same control values and the sound is the same.
  - WKWebView harness, against a mock serving the engine's own extraParams output: each control appears in its
    illustrated face's "+N more" drawer at the right default. The art's own bindings are unchanged. Clicking another
    position sends the right norm, and the engine-side value and the face both follow. For example, the RE-201 mode
    goes from 11 to 10 with norm 0.818, and the Surveyor input from +12 dB to −33 dB.

### 10.25 5E3 Tweed Deluxe: tied Volume wipers, the Input selector, and silent library rigs (2026-11-11)

- **Symptom.** "Tweed Clean Edge", "Hollowbody Bebop" and "Jump Blues Bounce" played 24–78 dB below the loudness
  target. The calibration measured −80 / −74 / −96 dB, hit the 40× clamp (the first two) or the −90 dB floor (the
  third), and cached that.
- **Cause.** The rigs set the unused channel's Volume to 0 (`vn` 0 with the guitar in Bright; Bebop `vb` 0 and meant
  the Normal channel). The old behavioural model mixed the channels with max(vb, vn), so 0 meant "channel off". On the
  5E3 the two Volume wipers are tied at V2A's grid, so a Volume at 0 grounds that grid and mutes the amp. That is
  authentic and is kept: about −80 dB at the speaker.
- **Input selector.** New `input` control (Bright / Normal / Both (jumpered), default Bright, `"face": true`). SJB and
  SJN feed the Bright and Normal Hi inputs from `in`. The Normal input network (RINN 1 MΩ, RGN 68 kΩ) is new, so V1A's
  grid now has its DC return. At the defaults (Bright input) the level is unchanged. The floating-grid fix moved the
  5E3 transient references (tweed_clean 1.8 % NRMSE, small changes to tweed_drive's harmonics), so the four tweed
  ngspice fixtures were regenerated. Against them the engine reads 0.003 % / 0.005 %, DC 42 nodes at 0.000.
- **Measured** (rack, calibration DI, raw rack loudness before the master limiter, catalogue defaults, with cab):
  Bright −10.6 dB, Normal −14.2, both −10.0. Unused Volume (Bright input, vb 0.5) at 0 / 0.05 / 0.1 / 0.2 / 0.5 /
  0.85 / 1 reads −92 / −23.1 / −17.5 / −13.2 / −10.6 / −10.2 / −12.6 dB. It is quieter at the top because the wiper
  then sits on the idle V1 plate.
- **Library rigs** (specs in tools/library/specs, then the .rig files). The unused Volume is set to 0.5 and the
  input is explicit (Bebop: Normal). South Side Bite's "Fatness" macro on `vn` started at 0 (mute), so its range now
  starts at 0.12. The cab MV had been staged against the silent amp (1.0 on two rigs), so it was re-staged for the
  three (0.223 / 0.435 / 0.193). All four calibrate to the −17.5 dB target without clamping. refresh_catalog.py now
  adds circuit `"face"` controls to the catalogue, so specs can set `input`.
- test_circuit_unit checks the export, that each input reaches its channel, and that either Volume at 0 mutes.

### 10.26 Legacy-parameter audit: the AC30 volume, dead macros, level caches (2026-11-11)

- **AC30 Top Boost (38000): silent rigs.** The catalogue's `Vol` is "N Volume" (the Normal channel) and `Br` is
  "B Volume" (Brilliant). The netlist bound `Vol` to VR2, the Brilliant volume (the only channel it models), and took
  `Br` for a legacy "bright switch". Rigs that used the Brilliant channel with Normal at 0 were therefore silent:
  Chime With Teeth −83.7 dB raw, Where the Streets Have No Name −61.4, Worship Clean Ambience −60.0, all clamped at 40×.
  Their `Br` macros ("Drive", "Volume") did nothing. Now `volume` takes `Br`, and `Vol` is a legacy-only
  `normal_volume`: the Normal channel is not modelled and its mixer resistor is taken at volume zero. On the real amp
  the channels mix through resistors, so an unused volume does not load the other. circuit_ab's AC30 settings now set
  `Br` (with `Vol` 0). After the rebind, and with the cab MV re-staged (0 / 1 / 1 had been staged against the silent
  amp → 0.783 / 0.889 / 0.875): raw −17 … −21 dB, gain −0.2 … +4 dB. The two showcase AC30 rigs (Vol 0.49, Br
  0.55 / 0.4) move by 0.0 / −0.3 dB raw.
- **Audit method.** Every library and showcase rig (101) went through the rack with the calibration DI, read before the
  master limiter (median −21.2 dB). Every circuit unit was also rendered alone at its catalogue defaults (bypass
  −19.1 dB). A static pass then listed, for each bound unit, the catalogue parameters it leaves legacy-only or unbound,
  and the rig values and macros that drive them.
  - Silent or clamped rigs: only the three AC30 rigs, after the 5E3 fix (§10.25).
  - No circuit unit is silent at its defaults. The quietest are fuzzes and drives, measured with no amp after them:
    Si Fuzz Face −24 dB against bypass, Ge −18, Uni-Vibe −19 to −20, Dyna Comp and Percolator −17, TS808 −14.
  - The lowest live rig is Desert Fuzz Lead at −39.9 dB. Its Fuzz Face at volume 0.6 drops the rig 17 dB against the
    fuzz bypassed. That is a level, not a binding: it is normalised.
- **Dead macros fixed.**
  - Dyna Comp (48000) `T` (Threshold, legacy-only: the M102 has Output and Sensitivity) → `S` (Sensitivity) on ten
    clean rigs, labelled "Squeeze" / "Sustain" / "Round Attack". Max clamped to S's 0.955.
  - CE-1 (29000) `Mx` (Mix, legacy-only: the CE-1 has no mix) → `CI` (chorus Intensity) on four library rigs and
    the Tine Lounge showcase (also in Showcase.cpp).
  - The AC30 `Br` macros are live through the rebind.
- **The remaining three, resolved.** Rule: retarget a dead macro only where the nearest real
  control keeps the rig's sound at the macro's stored position. A macro's position is derived from its target's
  current value (RackGraph::currentMacroValue), so the loaded sound never moves.
  - Brown Sound Lead: Electric Mistress `x` was 0, meaning the flanger was off; the circuit flanger has no mix and was
    always in. The unit is now bypassed (`Pwr` 0; spec `"on": false`). The "Flanger" macro targets `Pwr` (0–1): it
    rests at 0 and switches the flanger in, as the description promises. Raw −32.9 → −31.9 dB with the flanger
    out; the cab MV was re-staged 0.5 → 1.0, giving −19.9 dB (loudness gain +2.4 dB).
  - Pride and Joy Rhythm: Dimension D `mix` (no mix on the unit: IC112 sums at fixed ratios). "Dimension" now
    targets `mode`, the four DIMENSION MODE buttons (1→4 = more), over the full 0–3. It rests on the stored mode (0.5,
    i.e. mode 2), so the sound is unchanged.
  - Funk Clav (showcase, and Showcase.cpp): Small Stone `DW` (no mix on the pedal). "Phaser" now targets `Pwr`
    (0–1). It rests at 1 (the phaser is on, as stored) and switches it out.
  - SVT (54000) `G`: not a macro but the wrong binding. `volume` listed `["V", "G"]` and the first id the catalogue
    has wins, so it took `V`. `V` is the catalogue's Master, which the 1969 SVT does not have, while `G` (Gain) is the
    channel volume, so rigs with Master 0 were silent. `volume` now binds
    `G`, and `V` is a legacy-only `legacy_master`. Defaults are unchanged (both 0.5). Hysteria Bass (G 0.6,
    V 0.2) reads −12.95 → −13.18 dB raw.
  - Still stored only, with no counterpart and no macro: the tremolo and Sag values on AC30 / AC15 / 5E3 / Plexi rigs.
  - No dead macro is left in the library or the showcase (static re-check).
- **Amp-trims cache.** The legacy amps' output-trim cache (`amp-trims-<dsp>-exact-fp.txt`, and the `custom-` one) is
  now keyed like the loudness cache. Each line starts with the engine build fingerprint (core/EngineFingerprint.hpp:
  LC_UUID plus netlists, shared with RealtimeAudioServer::loudnessEngineTag). Other builds' lines are dead, and the file
  is compacted at 256 dead lines. It is a new file name, so older builds reading the unprefixed `-exact.txt` never see
  the new format.
- **Tests never touch the user's data folder.** Every test executable links `tests/isolated_data_dir.cpp`. That
  covers `rig_tests` and `test_*` (a deferred CMake pass, so new tests are covered) and the plug-in project's
  `test_plugin_core` and `hr_host`. Before `main()` it points `HEADROOM_DATA_DIR` at a fresh temp folder and removes
  it at exit; folders left by aborted runs are swept at the next start once their process is gone. If no folder can
  be made, the test exits rather than fall back. A `HEADROOM_DATA_DIR` already set is respected. `unit_audit_smoke`
  gets one through ctest. Before this, any test that built a legacy amp appended to the live amp-trims cache. Running
  every test with no data folder set left the live folder untouched; only the running app's own files changed.
  test_cabinet_tempo and test_plugin_core now also set `HEADROOM_NOISE_FIXED_SEED`, for the same reason as
  test_level_staging below: their bit-identical instance comparisons had been failing since the per-instance noise
  salt. test_circuit_budget still fails on its own (`backend == "hybrid-circuit"` assertion, independent of the data
  folder; not looked into).
- **test_level_staging: rigs with a guitar input keep the guitar calibration DI.** Two calibrations of the same Plexi rig differ
  by up to 2e-4 dB (6.99428 / 6.99435 / 6.99442 dB) because circuit units salt their noise per instance (§3.9). With
  `HEADROOM_NOISE_FIXED_SEED` they are bit-identical. The test now sets it, as test_circuit_unit, test_aliasing,
  test_effect_handoffs and test_param_drag already do. The check (same DI → same gain, 1e-6) stays exact.

### 10.27 `circuit_cli autocut`: an automated cut finder, and what it found (2026-11-11)

The user asked: "why don't we just split all the amps and effects down until they have zero quality loss and are extremely
efficient?" The exact partitioner (§3.5) already splits a netlist at every point where that costs nothing. Anything
finer needs an explicit signal cut (§2.6), and a signal cut lags the loads' current by one sample. `autocut` searches
those cuts systematically and keeps only the ones that measure as lossless.

**Usage.**

```
circuit_cli autocut <netlist> [--table <catalogue id>] [--gate <build>/test_circuit_engine] [--wav di.wav] [--seconds s]
                    [--top N] [--minports P] [--try "node:LOAD,LOAD;node2:LOAD"] [--full]
```

- `--table` sets the controls where the unit's defaults put them. Measurements run on the unit's path: its
  oversampling, minRate, ladder levels, live LTE, and the 450 ms mains settle.
- `--gate` points at test_circuit_engine. Without it the SPICE criterion is skipped (and reported as "not run").
- `--top` caps how many candidates are measured (12 by default). `--minports` sets which groups are searched (6 or more
  nonlinear ports by default).
- `--try` measures a given cut set and skips the search. `--full` measures every criterion even after one fails.
- `CE_AUTOCUT_SINE` sets the hard-sine level (0.5 FS by default). `CE_AUTOCUT_DIMED=0` skips the dimed sine.
  `CE_AUTOCUT_TMP` sets the scratch directory (/tmp/autocut by default).

**Gate-only runs.** test_circuit_engine has two env hooks for this. `CE_GATE_FILE=<netlist file>` runs only that
netlist's transient, AC and DC SPICE cases and skips the unit tests. `CE_GATE_LIBDIR=<dir>` reads the netlists from
another directory. The cases still run against the same frozen fixtures, at each case's own rate (96 kHz unless the
case says otherwise).

**Method.**
1. **Candidates.** For each group with at least `--minports` nonlinear ports, take every named node the group solves.
   Skip nodes that are already cuts, the output node, and subcircuit internals. Remove the node, and union the group's
   parts along the nodes the group still solves.
   - The parts touching the node then fall into sides. A part whose other pins are all rails or ground stays with the
     owner.
   - Each side, or for three or more sides each side's complement, is a candidate load set.
   - An option is dropped if its loads would include the active device that drives the node (a plate, cathode,
     collector, emitter, drain, source or op-amp output pin on the node) while another option does not.
   - Every option is compiled. It is kept only if the original group splits and its largest piece has at least 2
     fewer ports.
   - **Loop flag.** A candidate is flagged *loop* when the owner group depends, directly or transitively through the
     groups' inputs, on a group holding one of its loads. The lag would then sit inside a feedback loop.
2. **Ranking.** Each candidate gets |Z| into its node at 100 Hz, 1 kHz and 10 kHz, at the operating point. It comes
   from the engine's own small-signal response (`smallSignalResponse`, the discrete transfer the engine plays): the
   response with and without a probe current gm·v(in) injected at the node, so Z = ΔH / gm. Candidates are ordered
   by the largest of the three; loop candidates go last.
3. **Measurement** against the uncut netlist, in this order. A candidate stops at its first failure unless `--full` is given.
   1. **Idle work probe.** 50 ms of silence from the DC point. The candidate is dropped if it needs more than 8x the
      uncut's instructions or PTC-rescues more than 10 % of samples. Such a cut would otherwise spend minutes in the
      unit's mains settle, which nothing can interrupt.
   2. **DI null.** 6 s of a rock DI on the unit path (noise off, hum on), with the rms and peak of the difference.
   3. **Hard sine.** 220 Hz at 0.5 FS.
   4. **Idle.** 10 s of silence, checked for growth.
   5. **Dimed sine.** The same sine with every continuous control at 1 (legacyOnly and mod controls excluded).
   6. **Newton.** Failures and PTC rescues over all of the above. Every run gives up beyond 4x (DI) or 8x (other runs)
      the uncut's work per sample.
   7. **CPU and aliasing.** A fresh unit-path compile with noise on runs the aliasing gate's tone (`circuit_cli alias`)
      and then 4 s of DI. CPU is counted in retired instructions per audio-second (`proc_pid_rusage`).
   8. **SPICE.** Every SPICE case of the netlist, through `--gate`.
   9. **Modes.** Every other position of each switch and selector, one at a time, with 2 s of the DI.

   The uncut references for the SPICE cases and the modes are made the first time a candidate needs them.
4. **Greedy combination.** Passing cuts are sorted by DI instructions. Starting from the cheapest, each further cut
   is added only if the whole set still passes every criterion, stays outside loops, and saves at least another 2 %.

**The rule.** A cut set is applied only if all of these hold against the uncut netlist:

| Criterion | Bound |
|---|---|
| Structure | splits a group into smaller Newton problems (largest piece at least 2 ports smaller) |
| Feedback | not inside a feedback loop: the owner does not depend on its loads. Loop cuts are listed but not measured (`--try` measures one) |
| DI null, unit settings | ≤ −60 dB rms, ≤ −40 dB peak |
| DI null, every other switch / selector position | ≤ −60 dB rms, ≤ −40 dB peak, no Newton failure |
| Hard sine (0.5 FS, 220 Hz), unit settings and dimed | ≤ −50 dB rms |
| Stability | output finite; no new Newton failures or PTC rescues; the idle difference does not grow (last 1 s window ≤ 1.5x the smallest earlier one, or under −140 dBFS) |
| SPICE gate | every case passes; each case-node NRMSE ≤ uncut + 0.01 percentage points; each AC error ≤ uncut + 0.01 dB |
| Aliasing | gate figure ≤ uncut + 0.5 dB, or ≤ −70 dB |
| CPU | at least 3 % fewer DI instructions per audio-second (2 % more for each further cut) |

**Results (2026-11-11).** The tool was run on every bound unit except the heavy amps (worked on separately): Rectifier, 5150, JC-120,
Uberschall, 1959RR, SVT, Fairchild, JCM800 and HM-2. Settings were `--top 5` or `--top 6`, the unit's catalogue defaults
and the lead DI `samples/di/Lead DI.wav`. CPU is G retired instructions per audio-second on 4 s of DI, unit
path, noise on. Before it is the uncut netlist; after it is the selected cut set.

Two cuts pass and are applied:

| Unit | Cut | Groups (ports) | DI G instr/s | Gate-tone instr | DI null rms / peak | Sine / dimed / worst mode | Alias | SPICE worst ΔNRMSE |
|---|---|---|---|---|---|---|---|---|
| SSL 4000 E EQ (121000) | `fo`, loads C22 | 25+19+3+1 → 19+19+6+3+1 | 6.07 → 5.43 (−10.6 %; −10.8 % on a re-run against the current library) | −9.7 % | −115.7 / −112.6 dB | −125.4 / −116.3 / −74.3 dB (HPF in) | −104.6 → −104.3 dB | +0.001 pp |
| EAE Surveyor (170000) | `em`, loads Q5 | 15+8+3+… → 13+8+3+… | 9.39 → 8.29 (−11.7 %) | −18.1 % | −143.2 / −136.1 dB | −145.0 / −141.4 / −140.9 dB | −117.2 → −115.5 dB | +0.000 pp |

- On the EQ, `fo` is the filter section's output: 0.1 Ω, an op-amp output through the S1 in/out switches. It drives
  only the 220 µF C22 into the 100 kΩ EQ input.
- On the Surveyor, `em` is the long-tailed pair's common emitter. Its load Q5 is the tail current sink, whose collector
  current hardly depends on that node.

Every other unit: no cut passes. The table gives the best candidate per unit and what stopped it.

| Unit | Uncut groups (ports) | DI G instr/s | Best candidates, measured |
|---|---|---|---|
| SLO-100 (122000) | 21+18+… (k3a already cut) | 15.03 | k4a −30.7 dB; p4b (V4A grid as load) −44.9; p1b −32.2; mv_out: Newton trouble. See also the k3a audit below |
| SLO-100 MK II (163000) | 27+4 | 14.92 | p1a −27.6 dB, k3b −24.9; mvo / pii (switched paths) −0 dB |
| Friedman BE-100 (151000) | 24+14+… | 16.40 | k1c (0.1 Ω at defaults): DI −148 dB, −6 %, but the relay K1 un-bypasses it on HBE: be100_dimed +0.24 / +19.5 pp, gate fails. sat2/SW7: −152 dB, only −2.7 % |
| Hiwatt DR103 (87000) | 23+2 | 8.30 | kcf2 cathode follower: −76 dB, only −2.1 %; mvw −59.9 dB |
| Supro 1624T (152000) | 15+2 | 5.43 | p1a: DI −144 dB but dimed −61.7 dB, only −2.2 % on the DI (−11.5 % on the gate tone) |
| 5E3 Tweed Deluxe (79000) | 15+2 | 3.19 | p1n: DI −86.8 dB, dimed −29.2 dB; cn: DI −87.4 dB, but the dimed sine takes over 8x the uncut's work (Newton trouble) |
| Twin Reverb (30000) | 11+9+6+5+… | 8.42 | k12 (V2B / V1B shared cathode): −95.8 dB but +2.0 % instructions; stack nodes −32…−57 dB |
| Deluxe Reverb (172000) | 11+9+6+5+… | 9.03 | k12: −95.7 dB, +2.1 %; stack nodes −27…−54 dB |
| Bassman 5F6A (62000) | 11+8+2 | 6.83 | g2a −25.8 dB; V2A as load: Newton trouble |
| Plexi (34000) / variac (92000) | 11+8+2 | 18.36 / 18.25 | g2a −7.6 / −7.0 dB; mixer side: Newton trouble |
| AC30 (38000) | 11+8+2 | 17.87 | k1 (shared cathode) −118 dB but +1.6 %; vw / p1a −0…−47 dB |
| AC15 (176000) | 14+12+2 | 9.96 | Vibravox nodes p6 / lw / lw2: −169…−180 dB, but at most −0.8 % |
| SSL 4000 G bus compressor (130000) | 52+4+4+… | 7.83 | nC / t2bm / t3m −24…−26 dB; the transformer as a load diverges |
| dbx 160 (125000) | 22+4+… | 5.62 | gl / q9c (R101): −187 dB, only −1.2…−1.8 %; m (R89) splits 22 into 11+11 and costs +3.9 % |
| SSL 4000 E dynamics (133000) | 17+8+6+… | 6.71 | n_send (D18): −210 dB but +1.5 %; n_gp −120 dB with new PTC rescues; n_gm DI peak −39 dB |
| LA-2A (124000) | 13+5+4+… | 1.43 | V1A grid as load: DI −74.8 dB, dimed −5.4 dB |
| Boss OC-2 (35000) | 32+5+5+… | 8.32 | jout −80.6 dB, only −0.9 %; mix −21…−46 dB |
| Mu-Tron Bi-Phase (144000) | 28+28+10+… | 9.23 | b9m −116 dB but more PTC rescues than uncut; a9m / z7am −18…−59 dB |
| Dyna Comp (48000) | 16+3 | 8.00 | ob: Newton trouble |
| Small Stone (20000) | 44+18 | 4.15 | l5e −43.9 dB (the 44-port group has no splitting node) |
| Uni-Vibe (164000) | 10+6+6+6+6 | 1.89 | lamp / phase nodes −49…−57 dB |
| Dimension D (142000) | 12+11+8+… | 6.87 | gctl −79…−81 dB, 0 % saving |
| RE-201 (78000) | 10+6+4+… | 3.25 | q11c / c6n / c3n −54…−57 dB |
| CE-1 (29000) | 12+8+6+… | 2.15 | q10e −55.8 dB; others −14…−31 dB |
| TS9DX (167000) | 9+2+2+2 | 3.85 | inv1 −1.6 dB |
| Mu-Tron III (47000) | 10+7+… | 4.78 | renv / c9n −46 dB |
| MT-2 (46000) | 6+6+4+… | 4.18 | m4b −47 dB at best |
| SansAmp GT2 (45000) | 14+2+2 | 9.42 | x −57.7 dB |
| JH-1 (84000) | 6+5+… | 4.49 | vp −32.5 dB |
| Klon (179000) | 6+3+2+2 | 2.94 | s −32.6 dB |
| Bluesbreaker (180000) | 7+3+1 | 5.14 | t: Newton trouble |

No group of 6 or more ports has a node that splits it in the TS808, DS-1, Big Muff, the three Fuzz Faces, RAT, Phase 90,
both Cry Babies, V846, Electric Mistress, Deluxe Memory Man, Rangemaster, Micro Amp, Harmonic Percolator, Tube Driver,
Tremulus Lune or 1176. The 1176's 9-, 10- and 13-port groups are each held together by more than one path.

**The SLO-100's shipped k3a cut, re-measured.** With today's engine, the shipped cut misses this rule. Measured by
`--try k3a:R24 --full` on the netlist without it:
- DI null −59.7 dB rms / −44.2 dB peak; sine −73.0 dB, dimed −41.6 dB;
- slo_od_hot spk 0.311 → 0.548 % (k3a 0.089 → 0.148 %, p4b 0.098 → 0.143 %);
- −38 % instructions on the DI;
- aliasing −66.3 → −65.2 dB.

The netlist note's "0.78 → 0.74 %" predates the engine changes since. The cut was left in place: without it, the unit
is about 1.6x the instructions and over one core. The trade is left open.

**What this says about the user's question.** The exact partitioner already splits every netlist wherever that costs
nothing. Below that, a one-sample lag is only inaudible where three things hold together:
- the node is stiff in every mode, which a relay or switch can change (the BE-100's k1c);
- the load's current hardly depends on the node (a current sink's collector, a coupling cap into a high impedance);
- the split actually removes Newton work.

The last is the rarest. Most lossless candidates sit on small side groups, and a split adds a group's fixed cost
(the dbx 160's 22 → 11 + 11 split costs +3.9 %). Across 34 units, 2 cuts pass. High-gain preamps and every node inside
the power-amp NFB loops are measured at −8 to −60 dB, nowhere near inaudible. Low impedance alone does not predict the
error: on the SLO, fxs (682 Ω, one resistor from the follower) nulls at −20 dB, against −60 dB at the follower.

**Proof** (2026-11-12, build-autocut, the library at that time, load average 110-180 from other jobs):
- **test_circuit_engine:** every ssleq_* and surv_* case passes, unchanged against the uncut netlists to within 0.001 pp.
  The suite's 18 failures are all in other work in progress: the open-contact partition and cut-bias tests,
  rect_tube_spongy, re201_*, and hm2_ac.
- **test_circuit_unit and test_aliasing:** pass.
- **test_effect_handoffs:** aborts at its first check, the Fairchild S301 switch-topology handoff (open-contact work).
- **test_param_drag:** fails with "loudness match never settled" on the JCM800 / TS808 rig. That rig does not use
  either changed netlist.
- **Re-measured:** both cuts on the current library and engine, with `--try … --full`. Same figures: EQ −10.8 %
  instructions, Surveyor −11.7 %.

**Uncut-netlist findings on the way** (not cut-related; for the owners):
- the RE-201 at its defaults on the DI: 70,984 Newton failures and 816 PTC rescues;
- PTC rescues on the DI at defaults: CE-1 5,579; Bi-Phase 2,422; OC-2 2,415; SSL dynamics 1,866; SLO-100 MK II 1,715;
  GT2 1,626; MT-2 1,071; Mu-Tron III 829; Small Stone 339; JH-1 96; Dimension D 32; DR103 23.

### 10.28 Heavy amps in real time: floating zener junctions, the HM-2 bias rail (2026-11-11)

**Floating zener junctions (Engine.cpp, `kLimZenerF`, newtonKernel's `portRatio`).** A back-to-back zener clamp
whose midpoint carries nothing else, such as the Dual Rectifier's master clamp ZM1/ZM2 or the BE-100's Z1/Z2, gets the
exact linear split (§10.10). Below breakdown, only the leakage currents set the midpoint. The Newton therefore spent
iterations moving a femtoamp node across every sign change of the signal: the Rectifier's amp group averaged 4.3
iterations, 7 % of samples needed more than 12, and there were 140 homotopies per 10 s. The amp's signal at the clamp
peaks at 7 V, so the 15 V zeners never conduct.
- **Convergence test.** Such a port also passes on its current: |g·Δv| ≤ 1e-12 A + tol·|i|, SPICE's abstol test,
  with g the junction's own slope. A limited step whose full step moves no such current does not hold the solve back.
- **Limiting.** Forward and below the knee it gets `kLimPnjF`'s current-space step down; in breakdown it keeps
  `kLimZener`.
- **Scope and A/B.** Only zener ports on floating islands are affected, so every other netlist is identical by
  construction. `CE_NOFLTCONV=1` restores the old path.
- **Proof.**
  - Full SPICE gate: no printed NRMSE moved.
  - Aliasing: Rectifier −80.4 dB, BE-100 −66.6 dB, both unchanged.
  - Rectifier DI null against the old path: −73 dB rms.
- **Rectifier CPU** (unit path, Tampa Death Rhythm settings, lead DI): 32.2 → 26.7 G instructions per audio-second,
  and Newton 6.6 → 4.8 iterations per sample.
- **What is left.** A 1 TΩ anchor from the midpoint to ground would save another 11 % (bench: 20.6 → 18.4 G), so the
  island still costs something. A composite series-junction port (the midpoint solved inside the device law) would
  remove it exactly. Not built.

**HM-2 bias rail.** The pedal's 4.5 V divider node `vb` (10k/10k, 100 µF + 47 µF) returned every stage's bias resistor,
so the whole pedal was one 26-port group. The netlist cut `"cuts": ["vb"]` (Z_owner 0.035 Ω, loop gain 2e-6) splits it
at the op-amp outputs into six groups, the largest of 9 ports.
- **CPU** (unit path, distortion 1): 17.0 → 6.5 G instructions per audio-second, 1477 → 0 homotopies per 10 s.
- **Accuracy.** hm2_small moved 0.107 → 0.108 %; the other hm2 cases did not move. Aliasing −75.5 → −77.3 dB.

**JC-120 bias rails.** The same idea applies to the four bias dividers `vb7`, `vsw15`, `vsw15b` and `vb15r` (loop
gains ≤ 5e-5): bench 26.2 → 24.4 G, worst gate change +0.004 pp, aliasing unchanged. `vref` stays uncut, because cutting
it moved the chorus triangle (jc120_chorus tri 0.33 → 0.81 %).

**Signal cuts that failed the gate (not landed).** Each was a resistor or coupling cap into a grid, or a tone-stack
feed, at a node of 7–60 kΩ:
- Rectifier o2b/RS3A plus o1/R220O: rect_raw 0.04 → 0.55 %, rect_modern 0.18 → 2.6 %, AC +0.29 dB.
- 5150 nb3 (tone-stack feed, about 7 kΩ): p5150_dimed 1.04 → 2.35 %, AC +0.14 dB.
- 5150 p2b/R32: −24 % instructions and the gate holds within 0.13 pp, but the DI null is −35 dB.
- Uberschall chs/C101: broken.

The one-sample residual of a load at such a node is not small, even when its impedance ratio looks safe. Only
cathode-follower outputs into non-conducting loads have passed so far.

**Slow supply partition (not landed).** The Rectifier's B+ group at 48 kHz cost rect_clean pa 0.03 → 0.24 % and
rect_dimed 1.14 → 1.34 %, so it was rejected.

### 10.29 Open contacts as no edge, back-to-back zener pairs, supply cuts through the loads' network (2026-11-12)

The three levers §10.28 left, each a general engine feature. A/B switches: `CE_ROFFSW=1`, `CE_NOZPAIR=1`,
`CE_CUTLOADNET=0`. CPU is instructions per audio-second, (10 s − 2 s)/8 from `/usr/bin/time -l`; "bench" is `circuit_cli
bench --table` (the unit's rate and controls), "unit" is `circuit_ab --cpu` on the lead DI at a 48 kHz host, defaults.

**1. Open switch contacts as no edge (`CompileOptions::openSwitches`, on; `CompileOptions::controls`).**
- A `switch` part on a switch or selector control that is open at the compile's control positions is left out (its
  prim's terminals go to ground: no stamp, no partition edge), so the exact partition can split at it. Contacts on a
  continuous control (a pot's switch) keep their roff: a glide can close them at any sample.
- **Leakage rule.** The 1 GΩ roff is *not* negligible everywhere. Leaving every open contact out nulled at −33 dB on
  the AC15 (input 1), −63 dB on the 5E3 (the open input-jack contact leaks into a 1 MΩ grid), −66 dB on the BE-100 and
  −76 dB on the JCM800. A contact leaves the circuit only where its leakage is provably below −120 dB: both ends'
  resistance to ground through the resistors alone (caps, inductors, windings and devices open; each resistor at its
  largest value over pots and variable parts) at most `kOpenLeak`·roff = 1 kΩ. An RC network's |Z(jω)| never exceeds
  that resistance, so the roff current moves either end by at most 1e-6 of the voltage across the contact. A contact
  whose end would lose its linear path to ground (an unused throw) also keeps its roff. `CE_SWDIAG=1` prints each
  decision.
- **Recompiles.** The partition now depends on those switches. `reset()` recompiles when a contact left out would close
  or one compiled closed has opened (`topologyStale()`); between resets a contact compiled open stays open.
  `CompileOptions::controls` compiles at given positions directly (the unit passes its parameters).
- **CircuitUnit.** A switch or selector parameter that changes the topology marks the unit on the control thread
  (`onParameterChanged`). The next control-thread `processControlRate` rebuilds both channels (compile + DC, no settle,
  about 0.15 s CPU for the Fairchild) and hands the running state over. The rack always does both on a unit no published
  plan holds (ComponentEdit, detachForEdit, applyPresetParams).
- **Hand-off across topologies (`adoptStateFrom` → `adoptAcrossTopology`).** This is the same netlist, rate and
  options (`netHash_`), so nodes, prims, devices and slots match index for index. Each reactive state keeps its branch
  voltage and current; devices keep warm starts and history; node voltages, time bases, cut currents, blocks, logic and
  rate converters carry over. Switch positions stay the new circuit's own; continuous controls glide on from the old.
  Allocation-free (`primeLadderCuts` no longer allocates either).
- **Two engine bugs found on the way, fixed.**
  - `adoptStateByName` carried a capacitor's state as G·v + (G·v − s), which negates the capacitor's current (an older
    sign convention). Now `carriedState`, the post-update convention s = G·v + i that `mapStates` uses. Forcing the
    Fairchild through the new path at the *same* topology (`CE_FORCETOPO=1`) gave −39 dB in the first ms before, and
    −109 dB after.
  - A compile at non-default control positions did not scale control-scaled sources (the Rectifier's Bold/Spongy:
    B+ 475 V instead of 427 V). `compile` now calls `refreshScaledSources()`. Compiling at random positions vs
    compiling at the defaults and setting them: identical, or within Newton tolerance (≤ 1e-8 relative; dbx 160 at −112
    dB), on every library netlist (2 random settings each).
- **Results.**
  - Fairchild 670: 126 of 136 contacts leave at the defaults (the S301 matrix and the attenuators sit on 150–600 Ω
    transformer lines). The 60-port group splits into two 30-port channels. Bench 14.82 → 11.24 G (−24 %); unit
    17.07 → 12.54 G (−27 %, all three levers; only this one applies).
  - Dual Rectifier 16000 / Rev C: no gain (11.41 → 11.39, 19.17 → 19.09 G). Their relays sit on 0.1–5 MΩ grid and
    plate-load nodes, where the roff leaks −60 to −80 dB. Only two contacts in parallel with a closed one leave.
- **Proof.**
  - Switch null: `openSwitches` on vs off at every option of every switch and selector, on every library netlist, over
    0.25–0.4 s of a chord.
    - A netlist that removes nothing is bit-identical.
    - JCM800, 1959RR, JC-120 and Mu-Tron III: −177 to −232 dB. Both Rectifiers: bit-identical.
    - Fairchild: −101.7 to −126 dB over its 21 input positions. At Newton tolerance 1e-9 it is unchanged, and with LTE
      off −111.8 dB, so part of it is the per-group LTE substep schedule.
    - Bi-Phase: −76.7 dB, from a third engine dependency on the partition. Slow partitions were staggered by group
      index (`phase = gi % div`), and a contact that reorders the groups moved their update samples. They are now
      staggered by the first slow node's place in the netlist's list (`CE_OLDSTAGGER=1` restores the old order), and
      the Bi-Phase nulls at −163.7 dB.
    - The stagger change on its own, gate A/B over the 10 slow-partition netlists: la2a_hot out 0.078 → 0.085 %,
      uvibe_sweep 0.128 → 0.119 %, ce1/bp/uber ≤ 0.001 pp. The rest are unchanged.
  - Full SPICE gate: no case moved except f670_* (≤ +0.007 pp) and bp_* (≤ 0.002 pp).
  - test_effect_handoffs: new `checkSwitchTopology`. The Fairchild's mode switch is flipped through an edit copy and
    the topology hand-off, against a witness running every contact as roff and flipping in place. Before the flip
    −129 dB; after the switching transient −109 dB. The first 10 blocks read −51 dB because the witness's contacts move
    at its next control refresh (0.33 ms later), not at the swap; a 20-sample shift alone gives −18 dB on the bare
    circuits.
  - Engine test `testOpenContacts`. test_circuit_unit passes.
  - test_param_drag fails ("loudness match never settled") with every lever off too, on a machine at load 80–300.

**2. Back-to-back zener pairs (`NlKind::ZenerPair`).**
- Two zener diodes joined anode to anode or cathode to cathode at a node that nothing else touches (no part, probe,
  cut, readout) become one two-terminal port, and the midpoint is eliminated.
- **Law.** With u the first junction's voltage, f(u) = I1(u) + I2(u + v) = 0 is monotone, with its root in
  [min(0, −v), max(0, −v)]. A safeguarded Newton, warm-started from the last solution, solves it to 1e-15. The port
  current is I2(u + v) and di/dv = g1·g2/(g1 + g2): the series pair exactly, each junction ngspice's law with its gmin.
- The pair's series resistances move to one resistor at the outer end (series elements commute). Limiting is
  `kLimPair`: pnjlim on the distance past either breakdown knee.
- **Rectifier ZM1/ZM2 (both Rectifiers).** Bench 11.43 → 9.66 G (−15.5 %) and 19.16 → 16.92 G (−11.7 %). DI null
  against the two junctions (unit path, noise off, 6 s): −172.6 dB and −169.6 dB. Full SPICE gate: no rect_* or
  revc_* number moved.
- **The BE-100's Z1/Z2 do not qualify.** Their midpoint carries the junction capacitors CJZ1/CJZ2 (25 pF), so it is a
  real dynamic node.
- Engine test `testZenerPairComposite`: anode- and cathode-joined, with rs, driven through breakdown both ways. The
  composite matches the two junctions to 1.5e-9 V of 7 V.

**3. Supply-cut stability through the loads' network (`CE_CUTLOADNET`).**
- **The bound.** It counts each load resistor at the cut node as if its far end were grounded. A rectifier's 30 mΩ
  series Rs therefore read as 33 S. When the bound fails, the loads' admittance is now measured through their own
  network:
  - load resistors, inductor and capacitor companions beyond the node;
  - ideal windings as shorts (their incremental impedance, the reflected source, is ≥ 0);
  - device ports as shorts (a junction or tube at its largest conductance);
  - every other rail, source and ideal output at AC ground.
- At the cut node itself only resistors count, as before. The network is passive, so the result never exceeds the
  bound: no cut that passed before can fail, and nothing else in the library changed.
- **SVT.** biasn: bound 66.7 S (loop gain 3.86) → network 2.5 mS (loop gain 1.5e-4), now cut. With `bscn` added to
  the netlist's cuts (loop gain 0.0071), the 37-port group splits into the PT and rectifiers (9 ports) and the amp (28).
  - Bench 29.77 → 14.13 G (−53 %).
  - DI null against the fully uncut supply: −88.1 dB (max 0.12 V of 50 V; the JC-120's accepted bias cuts null at −81
    dB).
  - Gate: svt_clean spk 0.089 → 0.093 %, pau0 0.885 → 0.894 %, bpa 0.991 → 0.999 %, the rails themselves (ripple
    waveforms) bscn 0.026 → 0.560 % and biasn 0.025 → 0.583 %; svt_drive spk 0.046 → 0.049 %. svt_ac unchanged.
  - Aliasing (`circuit_cli alias --table 54000`): −99.7 dB uncut → −101.0 dB split. test_aliasing: 46 of 46 units below −60 dB with all three levers
    (SVT −101.0 dB, Rectifier −62.0 dB, Rev C −66.9 dB, Fairchild −158.5 dB).
- Engine test `testCutLoadNetwork`: a 40 Ω-winding bias rail with a 30 mΩ-Rs diode stays cut and tracks the uncut rail
  within 10 mV of 59 V.

### 10.30 JC-120 CPU: series diode strings, kept factorisations on lazy-clamp groups, straight-line LU (2026-11-12)

The JC-120 (49000) ran at 122-129 % of one core on a Ryzen 9 9900X test machine (`bench --table 49000 --rate 96000`, 13.4 µs
per sample, 21.5 Newton iterations over 14 groups). Five groups of 16-18 ports take 12-17 % each: the supply (0), the
CH-2 preamp (1), the mixer (6) and the two power amps (7, 11). Group 8, the reverb driver, averaged 4.4 iterations.
Every group runs about 1.2 dense factorisations per sample: in the big groups all 16-18 ports stay live (a junction's
gmin keeps its Jd row above the low-rank threshold), so the all-live path (J = K·Jd − I, partial-pivoting LU) carries
them. `sample` profile, M4 Pro: `luFixedN<18>` 15 %, `luSolveFixedN<18>` 9 %, the kernel body 11 %, `gemvT` 12 %, noise
draws 8 %, LTE estimate 4 %.

**Changes** (Engine.cpp; each has its own A/B switch):
1. **Series diode strings as one port** (`CE_NOSERIESD=1`: one port per diode). m identical plain junction diodes (one
   model, no breakdown) joined cathode to anode at nodes nothing else touches (no part, probe, readout, cut or held
   node) carry one current. A strictly monotonic law then puts 1/m of the string voltage on each junction. One port
   with n·m, Rs·m, gmin/m and 1/m of the shot-noise variance is the same circuit, without the midpoints. Those islands
   are held by the junctions alone, so Newton had to converge them on leakage currents. On the JC-120's D4/D5
   reverb-driver bias string, group 8 drops from 4.38 to 1.80 iterations and from 1230 to 746 ns per sample.
2. **Lazy-clamp groups keep their factorisations for the predictor correction** (`CE_NOCLAMPKS=1`: as before). This
   is a Newton start only. Both the clamp-free solve (ksR) and the full solve (ks) keep one.
3. **Straight-line LU at 12-24 ports** (`luFixedN`, `luSolveFixedN`; `CE_LULOOP=1`: the loop forms).
   - `luSmall`'s operations run in `luSmall`'s order, step K then K + 1, so the factors are bit-identical. Checked
     bytewise on 10 000 real 18- and 16-port JC-120 Jacobians, on arm64 and x86-64-v3.
   - Measured on those Jacobians: 505 → 388 ns (Ryzen) and 583 → 430 ns (M4) at 18 ports, 342 → 243 ns at 16.
   - The solve keeps `luSolveSmall`'s sum orders. Unrolled, every product fuses into its multiply-subtract. That is
     bit-identical on x86-64-v3. On arm64 the loop form leaves some products unfused, so the two differ in the last bit.
   - Code size: 27 KB for the 18-port factor, the same pattern as `wbEliminateFrom`.

**Proof.**
- **DI null** (unit path, `CE_NOISE=0`, 6 s lead DI, both channels, change on vs off):
  - Serial string: −207.3 / −203.3 dB rms, −174.7 dB peak.
  - Changes 1 + 2 together: −195.7 / −166.1 dB rms, −124.7 dB peak.
  - LU: byte-identical render on its own. With the arm64 solve, −222.2 / −213.3 dB rms and −179.8 dB peak.
- **SPICE gate:** every jc120 case is identical, with changes 1 and 2 on and off and with `CE_LULOOP` on and off. The
  full test_circuit_engine output is identical with `CE_LULOOP` on and off apart from timing lines. Its only failures
  are or120_* and re201_play_ac, the same on both runs.
- **Suites:** test_aliasing passes (48 of 48 gated units) and test_effect_handoffs passes. test_circuit_unit fails
  only on the embedded-netlist check for vox-ac30-tb.circuit.json, which was being edited at the time; the unit renders
  pass.

**CPU** (Ryzen 9 9900X, `bench --table 49000 --rate 96000 --seconds 3`, min of 5, one pinned core; same session):

| Build | % of one core | Newton it/sample |
|---|---|---|
| Pre-change binary | 122.3-122.6 | 21.48 |
| Current tree with changes 1 and 2 off | 118.3-119.7 | 21.48 |
| With 1 + 2 | 114.8-115.1 | 18.41 |
| With 1 + 2 + 3 | 111.2-111.4 | 18.41 |

On the M4 Pro, change 3 saves 14 % of instructions (21.9 → 18.8 G per audio second).

**Measured, not landed:**
- **Power-amp folds.** None is exact. The nine transistors of each 60 W amp are all distinct parts: no identical
  parallel pairs, no series junction strings. The Q21/Q22 bias spreader's midpoints carry C57 and R119.
- **Active-set Newton.** It runs only as the fallback before the homotopy, and the JC-120 never gets there. As a
  primary path it needs ports that carry exactly no current. No junction port qualifies: leakage and gmin flow at
  every bias.
- **Cross-sample chord** (the kept factorisation as the first iterate's chord, the in-solve chord's acceptance test):
  −8.9 % instructions, but the DI null is −98 dB rms / −55 dB peak from the first second. Rejected, as in §10.8. The
  AC30/Plexi work's 1/1000-gated `CE_XCHORD=all`: −3.7 % on the Ryzen, null −120.6 dB rms / −87.1 dB peak.
- **Woodbury form with every port live** (I − Jd·K, pivot-free): −5 % instructions, rounding-level. Superseded by
  change 3.
- **The §10.31 quadratic predictor down to 5 ports** (`kPredQuadPorts` 24 → 4): −5.6 % instructions and 24.69 → 23.59
  iterations per sample on the DI. Null −164.5 dB rms / −124.6 dB peak, the same class as change 2. Widening it is a
  library-wide decision; §10.31 measured 0 to −0.8 % on the big amps.
- **Per-sample parallel groups** (prototype: `work/circuit-netlists/cpu-9900x/jc120-parallel-lanes-prototype.patch`).
  - **The dependency chain.** A consumer reads its cut node's voltage in the same sample, and only the current goes
    back a sample late. So the groups form a chain: G0 → G1 → G6 → G7 → G11 → G12 → G13. G11 reads `lineout`, which
    G7 owns, and the LFO group G12 reads `spkL` through R146. That chain is 86-89 % of the work, so a second lane
    could take only about 11 %.
  - **Splitting the power amps.** Giving `lineout` its own group (cut loads `["R49_1", "R118_2"]`: the 100 Ω LINE OUT
    divider is fed only through the two 47 kΩ resistors) decouples the two amps. DI null against the shipped netlist:
    −180.0 / −185.2 dB rms, −148 dB peak.
  - **The prototype.** The helper lane runs on a spinning thread; groups wait on per-group done counters; cut-current
    contributions are recorded per group and replayed in group order. It renders byte-identically to the sequential
    run, noise on and off.
  - **Result.** On the Ryzen, two physical cores: 111.9 → 84-85 % of one core (the critical path predicts about 77 %).
    On the loaded M4 Pro: 12.3 → 9.7-10.2 µs per sample.
  - **Landed in §10.33.** The list below is what landing it needed.
  - **What landing it needs.** The helper lane as an RtPool task forked per block. Claim-per-group execution, so the
    audio thread runs any group the helper has not started and never waits on a helper that is not running. Per-group
    diagnostics counters (`diag_` races in the prototype). Automatic lane assignment from the DAG and group costs.

### 10.31 One-group heavy amps: a quadratic predictor and two exact shortcuts (2026-11-12)

The Uberschall (81000), 5150 (134000), SVT (54000) and Dual Rectifier (16000) spend 70-94 % of their time in one
28-36-port group. Supplies are already cut on all four, and the output tubes are already folded. The groups are held
together by the signal path itself: grid-current ports, interelectrode capacitors and the NFB networks. On the Rectifier,
the open relay contacts (roff 1 GΩ on 0.1-5 MΩ nodes, §10.29) do the same.

**Signal cuts measured, not landed** (`autocut --try … --full`, unit path, lead DI):
- 5150 `k3b:C30B` (V3B follower into V3A, 660 Ω): 29 → 15 + 14 ports and −35 % instructions, but the DI null is
  −38.4 dB, dimed −0.1 dB.
- Uberschall `k5a:C100` (V5A follower into the loop send, 711 Ω): 36 → 20 + 16 and −32 %, but the DI null is
  −49.6 dB, dimed −21.9 dB, and the worst mode +13.4 dB.

The next grid conducts in both, so the lagged load is far from linear. No cathode-follower cut in these four amps meets
the rule. The SVT has no low-impedance node between the preamp mix and the V4T grid.

**Profile** of the big group (SIGPROF PC samples, Uberschall / 5150):
- Woodbury factor and steps (18-21 live of 29-36 ports): 22 / 25 %;
- the sparse linear update's two solves and scatters (`linPorts`, `linOut`): 24 / 29 %;
- K·cur: 5 / 4 %; `residualOk`: 3 / 2 %; device laws: about 9 %; noise draws: 5 / 4 %.

**Changes** (Engine.cpp):
1. **Quadratic predictor on groups above 24 ports** (tolerance-level; `CE_NOPREDQ=1` restores the linear one).
   - With three samples of history, Newton starts from 3·v1 − 3·v2 + v3. The predictor correction then takes p's
     deviation from its own quadratic trend.
   - With the correction, the linear form's error is second order in p's per-sample change, and the quadratic form's
     is third order.
   - Uberschall big group: 67.6 → 83.7 % of solves in one iteration.
2. **`linOut` starts from `linPorts`' right-hand side** (bit-identical). The state and input columns are summed once,
   in the same order. Only the port-current and cut columns are added again.
3. **Lazy row norm in `residualOk`** (bit-identical). |K|·|Jd| is summed only for a row whose residual fails the bound
   at rn = 1. Since rn ≥ 1 and rounding is monotone, a pass at 1 is a pass at rn.

`CE_NOBIGEXACT=1` turns 2 and 3 off. A render byte-compare (unit path, DI, noise on) is identical on all four amps.

**Proof.**
- **DI null** (unit path, noise off, 6 s lead DI, change on vs off): Uberschall −164.4 dB rms / −143.7 dB peak,
  5150 −177.5 / −142.6, SVT −175.0 / −141.9, Rectifier −180.7 / −150.9.
- **Dimed 0.5 FS 220 Hz sine:** −166 to −182 dB rms.
- **Other netlists with a group above 24 ports:** Ecstasy −195, SLO-100 MK II −190, Rev C −182, OC-2 −177,
  Small Stone −167 and Fairchild −163 dB rms. The SSL bus compressor and the Bi-Phase are bit-identical: their unit
  groups stay at or under 24 ports.
- **No new homotopies or failures** anywhere.
- **SPICE gate** (full test_circuit_engine, on vs off): identical. The one printed change is uber_lead_small's max
  error, 9.59e-05 → 9.60e-05 V. The run fails only on or120_*, identically on and off; that is other work in
  progress.
- **Aliasing** (`circuit_cli alias --table`): unchanged to 0.1 dB. Uberschall −58.5, 5150 −50.3, SVT −101.0 and
  Rectifier −62.0 dB, all report-only as before where above −60 dB.
- **Suites:** test_aliasing, test_circuit_unit and test_effect_handoffs pass.

**CPU** (Mac; `bench --table`, unit path, noise on; 1.25 s minus 0.25 s, G instructions per audio-second; wall clock
is the min of 5 reps at load 11-15):

| Amp | G instr/s before → after | wall % of one core before → after |
|---|---|---|
| Uberschall | 18.43 → 15.67 (−15.0 %) | 104-117 → 88-97 |
| 5150 | 15.10 → 13.16 (−12.8 %) | 88.6-88.9 → 78-84 |
| SVT | 13.55 → 12.43 (−8.3 %) | 68.5-68.6 → 64.5-65.4 |
| Rectifier | 9.42 → 8.67 (−8.0 %) | 56.5-57.1 → 52.5-53.5 |

The predictor alone saves 7-11 % on the Ecstasy (13.87 → 12.65), SLO-100 MK II (12.42 → 11.17) and Rev C
(17.12 → 15.64). OC-2, Bi-Phase and SSL bus compressor: within noise.

**Measured, not landed:**
- **Iteration-0 chord from the kept factorisation.** Without the quadratic predictor it costs 3-4 %. With it, it is
  neutral on the 5150. The AC30/Plexi work's `CE_XCHORD=all` gives the Uberschall a further −4 % (16.86 → 16.18 G)
  on top of the predictor, and the 5150 nothing. Its default gate (ladder groups only) does not reach these amps.
- **The quadratic predictor on all groups above 4 ports:** SVT −0.8 %, Uberschall 0. Not widened.
- **Sparse linear update on the Rectifier** (cost weight 10 → 2): −2.3 % instructions. Indexed code costs more
  cycles than that, so it was not taken.

### 10.32 AC30 / Plexi 16x power groups: why 16x, and what is cheaper at no cost (2026-11-12)

The power group (tone stack, PI, power tubes, OT, speaker; AC30 39 unknowns, Plexi 35, 11 ports each) runs at 16x a
48 kHz host (768 kHz, "rate x4" over the 4x base) and takes about 80 % of each amp.

**Why 16x.** The aliasing comes from the OT model's leakage inductance. When a push-pull tube cuts off, its leakage
current is forced into the 470 pF winding capacitance (CWA/CWB with RWA/RWB): a flyback edge with content near 380 kHz.
On the gate tone (2.9 kHz, 0.5 FS), harmonics 127-138 sit at −48 to −54 dB, and at 4x or 8x they fold about 384 kHz
onto 1.7-16.2 kHz. Aliasing gate, AC30 / Plexi (−60 dB):

| Power group at | 4x | 8x | 16x (shipped) | 32x |
|---|---|---|---|---|
| AC30 | −49.5 dB | −47.2 dB | −64.3 dB | −64.8 dB |
| Plexi | −53.1 dB | −51.0 dB | −64.8 dB | −66.4 dB |

8x folds the same harmonics as 4x (384 kHz = 2·192 kHz). The LUT tube laws are not the cause: −64.3 / −47.2 dB with
`CE_NOPENTLUT` and `CE_NOTRIODELUT`. Diagnostic variants of the AC30 at 8x: OT leakage 0 gives −59.4 dB; winding
capacitance 1 fF gives −27.9 dB; grid stoppers 1 Ω or `lsat` 1000 change nothing. So 16x is needed.

**Splits.** `autocut` finds no node that splits the power group. Moving the linear parts to the base rate would need
signal cuts at the tone stack's output (about 100 kΩ into the PI grid) and at the speaker (about 6 Ω against the OT's
output impedance). Both lag a load current that is not small against the node's own. The tone stack is about 3 of 30
states, so the cut would save little anyway. Not done.

**Landed (each bit-identical unless noted; SPICE gate figures identical; aliasing −64.3 / −64.8 / −65.8 dB unchanged):**
- **AC30 speakers as one `"series": 2` block.** Two identical drivers in series carry one current, so every R and L is
  doubled and every C halved (Netlist "series", §2). The power group loses 4 states. Against the two blocks: DI null
  −183.5 dB rms, and −184.5 dB with the amp dimed and the DI at 2x. The `ac30_dc` fixture was regenerated: only the
  speaker-internal nodes changed (all ~0 V), 50 nodes compared instead of 54.
- **`gemvT`: one four-lane accumulator per row and 8-row blocks**, plus remainder rows run side by side. Each lane runs
  the same FMAs in the same order as the old pair of two-lane accumulators, so the results are bit-identical (checked
  on 20 000 random shapes on arm64 and on x86-64-v3 under Rosetta). x86-64-v3 now gets 256-bit FMAs: a 56×50 product
  takes 222 → 93 ns on the Ryzen 9 9900X, and 187 → 144 ns on the M4 Pro.
- **Mains phase `fmod2Pi`.** `fma(−k, 2π, x)` with the quotient corrected by one when needed is exact, the same bits
  as `std::fmod` (149 M arguments checked). It replaces the libm `fmod` in `advanceAc` and in the ladder's sub-sample mains.
- **Newton kernel prologue.** The floating-zener table is filled only when a floating port exists. The step-limit
  scales are set on the first full step. Both were per-solve `memset_pattern16` calls.
- **Ladder converters.** `runLadderConv` ping-pongs its stage buffers instead of copying them. `HalfbandStage::up`/`down`
  run unrolled copies for 4, 6 and 10 coefficients (same operations, same order).

**Cost** (the unit path, `bench --table`, 3 s chord): instructions net of compile on the M4 Pro, AC30 49.8 → 44.9 G
(−9.9 %), Plexi 46.5 → 43.5 G (−6.5 %), Plexi variac 48.6 → 45.5 G (−6.4 %), 1959RR 52.1 → 48.3 G (−7.2 %). Wall
clock, min of 4 interleaved runs at load average 10-15: AC30 70.7 → 63.6 %, Plexi 65.1 → 59.8 % of a core. On the
Ryzen (the existing build, netlist change only), the speaker block alone is −1 to −2 %. The gemv change is larger
there; confirm it on the 9900X.

**Measured, not on by default: cross-sample chord (`CE_XCHORD`).** Its first Newton iteration steps with the last
sample's kept factorisation. The step is accepted only by the in-solve chord's test, with the residual gate tightened
1000x (`CE_XCHORDGATE`), and only while Σ_k max_b|ΔJd|·max|K[:,k]| ≤ 0.1 (`CE_XCHORDDJ`). On the rock DI the
instructions fall 3 % (AC30) and 7 % (Plexi), and the outputs null at −164..−183 dB. The 5150, SVT and Rectifier
(`CE_XCHORD=big`) gain nothing; the Uberschall gains 1.4 %.

It stays off for two reasons. Without the Jacobian bound it glitched the Uberschall DI once: −60 dB peak at 5.146 s,
where a device turning on loosened the residual test's normalisation. With the bound, the AC30 dimed on a 2x DI moved by
−17.5 dB peak at the power group's solver events (Newton failures + homotopy, 28 in 20 s).

These events depend on the solver's path, whatever perturbs it: `CE_TOL=1e-10` moves them by −57 dB peak, and the
predictor's old attack-only gate (`CE_PREDTH=1e30`) by −11.6 dB peak. Exact changes (all of the above) cannot move
them. That sensitivity is an open item of its own.

### 10.33 Intra-unit lanes: independent groups on two cores, bit-identical (2026-11-12)

One circuit's solve groups run on two cores within each sample: the audio thread runs lane A, and an RtPool task forked
per block runs lane B. The output is byte-identical to the sequential loop. It lands the prototype of §10.30 ("Per-sample
parallel groups") without its spinning thread or races. Off switches: `CE_NOLANES=1`, `CompileOptions::lanes = false`,
or `"lanes": false` in the netlist.

**The plan** (`Circuit::buildLanes`, at compile).
- **Ordering.** Two groups must keep their order within a sample when one writes a node the other reads or writes.
  Writes are a group's exports. Reads are its inputs, its own cut nodes, its cut terms' nodes and VCCS controls. The
  earlier group in the partition's order finishes first. That covers a consumer reading its cut node, and an earlier
  group reading a later one's last-sample value (delay cuts). Groups linked only by cut currents, which are read one
  sample late, are independent.
- **Group cost** (ns per sample): the linear update's flops, about 1.5 factorisations per sample, and fixed per-group
  and per-port overheads, fitted to `CE_PROFILE` on the 9900X. A slow partition's cost is divided by its `div`.
- **Assignment.** Two starts: everything on lane A, and a two-lane list schedule (upward rank, earliest finish). Each is
  improved by single-group moves. Each candidate is simulated with both lanes in group order, plus 150 ns per cross-lane
  dependency and 400 ns per sample.
- **When it engages.** All of: the estimated sample shortens by at least max(800 ns, 20 %); lane B has at least 800 ns
  of work; the circuit is estimated at 30 % of a core or more at its rate. The helper spins on a second core for the
  whole block, so a light unit is not worth it.
- **Not planned for:** the rate ladder, the 2x multirate mode, CMOS logic cells, or a single group.
- **Developer switches:** `CE_LANEDIAG=1` prints each compile's analysis and the dependency lists. `CE_LANES=7,8,9`
  forces lane B. `CE_LANEIDLE=<ns>` sets the helper's idle bound.

**The runtime.**
- **Block.** `LaneBlock` (src/circuit/LaneBlock.hpp) calls `beginLanes()`, then `RtPool::fork(laneHelper)`, the block's
  `process()` calls, `endLanes()` and the join. CircuitUnit uses it in both of its render paths; `circuit_cli bench` uses
  it too. Offline renders (`AudioBlock::offline`) don't fork.
- **Before the helper attaches.** Samples run the plain loop. The helper attaches at a sample boundary (`present`).
- **From then to the end of the block,** each sample:
  - The audio thread publishes `(s << 1) | 1` in `stamp`.
  - It runs lane A in group order. A lane-A group's `done` word is published only where lane B depends on it.
  - Before a lane-A group, it ensures that group's lane-B dependencies. At the end, it ensures every lane-B group from
    the helper's cursor on.
- **Ensure.** Claim the group by a CAS of its sample number, then run it (after its own lane-B dependencies). If the
  helper already holds the claim, spin until it publishes `done`. So the audio thread waits only on a group a running
  helper has claimed, never on a helper that is not running. With no helper, it runs every group itself.
- **The helper.**
  - It runs lane B in order. It skips claimed groups, waits for dependencies, claims by CAS, runs, publishes `done`, and
    moves its cursor. At the end of the sample it publishes `doneB`.
  - It detaches after 250 µs without a new sample or a finished dependency, never while it holds a claim. At 30 µs,
    vCPU stalls under WSL2 detached it in 27 % of 64-frame blocks; at 100 µs in 2 %; at 300 µs in 0.04 %.
  - It copies the audio thread's floating-point mode (flush-to-zero).
- **Real-time safety.** The audio thread does no allocation, takes no locks and makes no syscalls per sample. Only the
  helper reads a clock.
- **Bit identity.**
  - A group's arithmetic depends only on its own state and on the nodes it shares, which are ordered as above.
  - Its noise draws come from its own generator (`GroupRt::rng`, already per group).
  - Its cut currents go to a per-group record. Records are added to `cutAcc_` in group order after the sample: the
    sequential sums, term for term.
  - Its solver counters go to per-group `Diagnostics` (`Circuit::dg`). `endLanes` merges them as sums and maxima.
- **Synchronisation cost.**
  - Each word sits on a cache line of its own: claim, done, `doneB`, the cursor and the stamp.
  - Each group's records and their count share a cache line, and lane B's regions come together.
  - `laneDuo_` is written twice per block, not per sample.
  - With a 50 ns lane B, the protocol costs +140 ns per sample on the 9900X (+530 ns before these layouts).
- **Steals.** The audio thread claims about 10 % of lane-B group-samples, mostly when a vCPU stall delays the helper.

**Pipelining and the pool.**
- The helper is one RtPool task per block (or per pipelined sub-block).
- If no worker takes it (busy pool, refused fork), the block runs on the audio thread alone. The join then reclaims the
  task, which returns at once.
- Nothing waits on a helper that is not running a claimed group. The series pipeline's stages wait only on each other's
  progress, and its join reclaims unstarted stages in order, so the two cannot deadlock.
- Under oversubscription, a helper that the OS preempts while it runs a claimed group delays the audio thread, as any
  RtPool join would. On macOS the workers are time-constraint threads in the device's workgroup. Elsewhere they run at
  normal priority.

**The JC-120 netlist.** The `lineout` cut's loads are now `["R49_1", "R118_2"]`. The 100 Ω LINE OUT divider is fed only
through the two 47 kΩ resistors, so it becomes its own group, and the two power amps (now groups 8 and 12) no longer read
each other within a sample.
- DI null against the shipped netlist (`autocut --try`, unit path, lead DI): −153.3 dB rms, −137.6 dB peak. Sine
  −152.8 dB, dimed −154.0 dB. No new Newton failures.
- SPICE gate: every jc120 figure is identical, except jc120_dist d3's max error, 7.81e-05 → 7.80e-05 V.
- The plan: lane A = {0 1 6 12 13 14} (supply, CH-2 preamp, mixer, left power amp, vibrato). Lane B = {2 3 4 5 7 8 9 10
  11} (reverb return, lineout, right power amp, reverb driver). The prototype used the same split, chosen by hand.

**Proof of bit identity.**
- **`circuit_cli bench`** (3 s chord, reps 3-5): the FNV hash of the double output, lanes against `CE_NOLANES=1`, noise
  on and off, arm64 and x86-64-v3. Identical on the JC-120, Fairchild 670, Dimension D, RE-201, Deluxe Memory Man and
  SSL 4000E dynamics (the last three measured under an earlier, looser engage gate; they now stay sequential). Newton counts are identical too.
- **Unit path** (`circuit_ab --cpu`, `CE_BLOCK=64`, `HEADROOM_NOISE_FIXED_SEED=1`, 6 s lead DI, noise on): the float
  render is byte-identical, with identical Newton counts, for 49000, 24000 and 142000.
- **Engine test `testLanes`.** JC-120, Fairchild and Dimension D, noise and hum on, 0.3 s of picked chords. Blocks
  rotate between three cases: an RtPool helper as the unit uses, a `std::thread` helper that attaches 0-0.4 ms late,
  and no helper. Byte-identical, with identical solver counters.
- **test_circuit_engine:** with `CE_NOLANES` on and off, identical apart from timing lines. It fails only on or120_*
  and re201_play_ac, the same on both runs (other work in progress, §10.30).
- **Suites:** test_circuit_unit, test_effect_handoffs, test_aliasing (48 of 48 gated units below −60 dB), rig_tests and
  test_graph_runtime pass.
- **Pipelining.** Three rigs, the Jazz Box Warmth (JC-120), Pride and Joy Rhythm (Dimension D in a series chain) and 16
  Mallet Dreams, rendered through RackGraph at 64 frames with a fixed noise seed. Pipelined and serial, each with lanes
  on and off: all four renders are byte-identical per rig.

**CPU on the Ryzen 9 9900X** (Windows 11 under WSL2; `bench --table 49000 --rate 96000 --seconds 3`, min of 5; four
interleaved rounds):

| JC-120 | % of one core |
|---|---|
| Pre-change binary, shipped netlist | 113.0-115.0 |
| This tree, `CE_NOLANES`, shipped netlist (no sequential regression) | 112.7-114.5 |
| This tree, `CE_NOLANES`, split netlist | 113.3-115.9 |
| **Lanes, split netlist, unpinned** | **87.8-88.0** |
| Lanes, two pinned physical cores (`HEADROOM_RTPOOL_WORKERS=1`) | 87.9-89.2 |
| Prototype patch, two pinned cores, same session | 92.4-94.0 |

- **Per 64-frame block** (128 internal samples, `CE_BLOCKTIME=1`, 10 s): the median goes from 1476-1498 µs to 1145-1162
  µs, and p99 from 2025-2057 µs to 1658-1838 µs. Blocks over the 1333 µs deadline drop from 94-95 % to 9-13 %. The
  JC-120 alone still misses on the chord attacks, where Newton iterations rise.
- **The critical path.** Measured group costs put lane A (0 → 1 → 6 → 12 → 13 → 14) at about 70 % of the sequential
  sample, against 77 % measured. A third lane would need another cut: the vibrato groups 13 and 14 follow the left amp
  through R146.

**Other units.** `CE_LANEDIAG` was run over every bound unit at its unit rate. Results with noise on, on the 9900X:

| Unit | Estimated two-lane sample | Sequential → lanes, % of one core | Plan |
|---|---|---|---|
| Fairchild 670 (24000) | 54 % | 59.6 → 38.1 | engaged (the two channels) |
| Dimension D (142000) | 57 % | 39.1 → 24.9 | engaged |
| JC-120 (49000), split netlist | 74 % | 115.7 → 89.5 | engaged |
| RE-201 (78000) | 80 % | 18.3 → 15.8 | gated off (load) |
| SSL 4000E dynamics (133000) | 87 % | 35.0 → 31.7 | gated off (gain) |
| Deluxe Memory Man (82000) | 86 % | 17.5 → 16.1 | gated off (load) |
| 1176LN, Tremulus Lune, dbx 160VU, BE-100, SVT, Bi-Phase, CE-1, OR120, SSL bus comp | 79-99 % | | sequential |

- The one-group heavy amps (Rectifier, 5150, Uberschall, SLO-100, Rev C, DR103) have no second branch.
- Not planned: the netlists with a rate ladder (Plexi, Plexi variac, 1959RR, AC30, SansAmp, Jackhammer, Ecstasy, ...),
  whose ladder loop would need its own lane plan; the OC-2 (CMOS logic); and single-group circuits.

**Real time:** `preset_rt` on the M4 Pro, 64 frames, 10 s of the lead DI, two interleaved runs each. The
load average was 43-55, so absolute misses reflect the machine; the comparison is what counts.

| Rig | Without lanes: mean / p99 / max % of deadline, misses | With lanes |
|---|---|---|
| Jazz Box Warmth (JC-120) | 132-134 / 164 / 186-200 %, 7500 of 7500 | 98-99 / 119-121 / 128-133 %, 3030-3285 of 7500 |
| 16 Mallet Dreams (Dimension D) | 48-49 / 56-58 / 61-63 %, 0 | 33-37 / 44-48 / 49-53 %, 0 |
| Pride and Joy Rhythm (Dimension D) | 57-58 / 62-63 / 66-67 %, 0 | 52-53 / 58-59 / 61-65 %, 0 |

- The JC-120 unit's mean per-callback time goes from 1734-1749 µs to 1292-1301 µs, and its p99 from 2154-2159 µs to
  1566-1596 µs.
- The barrier adds no misses: every rig that met its deadlines without lanes meets them with lanes, at lower peaks.

### 10.34 Noise and hum: where their cost is now, and cheaper draws (2026-11-12)

CPU plan item P3. The starting point was COST_BREAKDOWN finding 3 (2026-11-10): noise and hum cost 6-21 % of an amp,
mostly as extra Newton work after each held draw. Measured again on this tree, that is no longer where the cost is.
Figures are retired instructions per audio-second, net of compile (`bench --table`, chord, unit path, `CE_NOLANES=1`,
Mac), noise and hum on against `CE_NOISE=0 CE_HUM=0`; the split comes from knockouts in a scratch build.

**Where the cost is now.**

| Part | 23 amps | Pedals and studio units |
|---|---|---|
| Noise + hum, total | +3.7…+13.8 % (median +7.2 %) | +3…+48 % (48 kHz modulation and studio units highest) |
| Draw generation (`updateNoise`) | 2-13 %, most of the total | most of the total |
| Bias additions per sample (dP, dOut, dI) | 0.5-2 % | small |
| Hum: two more input columns per heater class, and the mains at sub-sample times on a raised ladder group that reads a heater | 0-1.4 %; AC30 3.8 %, 1959RR 3.0 % | 0 |
| Extra Newton work | ≈ 0 (within ±1 %) | up to +20 % |

- **Newton on the amps.** The every-sample predictor correction (T4, `kEnvPredTh = 0`, 2026-11-11) already follows the
  held steps. Iterations per sample, noise off → on: chord, JCM800 5.16 → 5.19, Plexi 8.22 → 8.32, Twin 7.33 → 7.38,
  5150 3.64 → 3.65, Rectifier 3.38 → 3.59; lead DI, JCM800 7.232 → 7.240, Plexi 10.901 → 10.902, Twin 7.753 → 7.757, 5150 5.483 → 5.485, AC30 10.361 → 10.361,
  Rectifier 5.255 → 5.444. With the correction
  off (`CE_PREDTH=1e30`) the noise would still cost the Plexi +24 % and the JCM800 +24 %.
- **Where Newton still pays:** groups that draw every sample (48 kHz units, `noiseDiv` 1) and keep no factorisation for
  the correction. Dimension D's op-amp groups 2, 4, 14 and 30 take 1.00 iterations without noise and 1.50-1.66 with it;
  the op-amp input noise `en` alone does it (about 2.8 µV rms per sample against a 1 µV tolerance). `ks` and `ksR` are
  invalid on those lazy-clamp groups, so `CE_PREDTH=0` changes nothing. Dimension D +20 % of its noise-off instructions, SSL
  bus compressor +12 %, Rangemaster +7 %, Butler +4 %. A kept factorisation there is predictor work (P1), not a noise change.
- **Band-limited, continuous draws: measured as not worth it.** On the groups that take the every-sample correction the
  held step already costs nothing. A linear-interpolated draw only moves the miss of the attack-gated small groups
  (TS808, DS-1, Butler, Rangemaster: Newton 0.5-4 % of the unit) from two samples per draw to one, and needs a stronger
  hold compensation (sinc² instead of sinc: −6.3 dB at 20 kHz at `noiseDiv` 16). At 48 kHz there is nothing to
  interpolate.
- **Hum as a Norton current through the noise injection: not taken.** It would remove the heater input columns, but
  samples re-solved as LTE substeps carry no noise (§3.9), and on a rock DI that is 7.5 % of the JCM800's group-samples
  (lead DI, 260 k of 3.5 M), so the hum would drop out while playing. The noise has the same holes today.

**Change** (Engine.cpp, noise section; `CE_NOISELEGACY=1` restores the old draw path bit for bit):
1. **Folded sources.** `Zf` has one column per kept source, scale and both ends folded in (`scale·(Zd[:, b] − Zd[:, a])`,
   formed in double), instead of one per node. A draw copies its Gaussians; the per-resistor scatter and its zero fill
   are gone.
2. **Skipped blocks.** `gemvFChunks` multiplies by 4×4 blocks and skips a block whose every entry adds less than
   `kNoiseChunkEps` (1e-10; `CE_NOISECHUNK`, 0 keeps all) of the noise power of each export, state and port that reads
   its rows. The response is local to its stage: on the one-group amps (Rectifier, 5150, Uberschall) 22-33 % of the
   entries carry all but 4e-8 of every output's noise power, and 42-55 % of the blocks are kept. A dropped block moves an output by under 2·nt·1e-10
   of its power.
3. **Tube-plate gm** ∝ Ip^(1/3) takes a float cube root (`cbrtNoise`, within 1e-7) instead of libm `cbrt`.

Same Gaussians, same sources and injection nodes, same 1/f banks, hold compensation and seeds: the noise realisation is
the old one to float rounding. Instructions per draw round (all groups once), old → new: Rectifier 19.1 k → 12.7 k,
5150 15.3 k → 11.4 k, Twin 10.4 k → 9.1 k, JCM800 7.4 k → 6.2 k, Phase 90 9.1 k → 8.6 k.

**Proof.**
- **Noise off, bit-identical:** the `bench` output hash with `CE_NOISE=0 CE_HUM=0`, old tree against this change, on
  all 67 bound netlists, and the unit-path lead DI render (6 s) byte-identical on all 67.
- **`CE_NOISELEGACY=1`:** bit-identical to the old tree with noise and hum on, 67 of 67.
- **Spectral gate** (unit path, 48 kHz host, `CE_NOISE_SEED` fixed, 6 s after the unit's settle; 1/3-octave Welch floor
  20 Hz-20 kHz with the mains harmonics masked, and the mains harmonics 1-10 standing 10 dB over the floor), all 67 units:
  - idle: worst floor band 0.028 dB, worst hum line 0.000 dB;
  - DI (lead DI; the noise part is the render minus the noise-off render): worst floor band 0.078 dB (Leslie, whose noise
    part is 4e-7 FS), worst hum line 0.014 dB.
  - The renders also null against the old ones: idle −78…−174 dB relative to the noise itself, the DI noise part
    −51…−181 dB. The exception is the EAE Surveyor (−28 dB, floor within 0.028 dB). There the old per-node float
    product cancelled two nearly equal end responses: the folded path with the old per-entry float rounding emulated
    differs from both by the same −28 dB, so that difference was the old path's rounding error.
- **Tests:** test_circuit_unit, test_effect_handoffs and test_aliasing pass (48 of 48 gated units below −60 dB, the
  report-only figures unchanged). test_circuit_engine (the SPICE gate runs with noise off): output identical to the old tree; it fails
  only on or120_* and re201_play_ac, as before (§10.33).

**CPU.** Instructions (Mac, load-independent): noise and hum cost, old → new, Rectifier +13.8 → +8.9 %, Uberschall
+6.1 → +4.5 %, SLO-100 MK II +8.3 → +6.6 %, Supro +9.0 → +6.9 %, DR103 +9.1 → +7.2 %, 5E3 +12.3 → +10.6 %, SVT +7.0 →
+5.6 %, JCM800 +8.7 → +8.0 %, 5150 +7.0 → +6.5 %; the rest of the amps within ±0.5 to −1.4 points (repeat spread
±0.5 points); median over the 23 amps +7.2 → +6.5 %. Studio units up to −3.9 points (SSL bus compressor +41.0 → +37.1 %,
1176, RE-201, LA-2A −2.8…−3.4).

On the 9900X (scratch build, `taskset -c 10,11`, the machine shared with two other benching jobs; min of two
interleaved rounds; bench `CE_REPS=3`; DI and hard material 6 s of `p3render`, a scratch harness of the unit path with
64-frame lane blocks), noise on, the 23 amps summed: bench 1307 → 1304 % (−0.2 %), DI 1624 → 1624 % (0.0 %), hard
material 3486 → 3507 % (+0.6 %). The per-amp spread between identical runs there is ±4 %, larger than the change, so
the instruction counts above are the measure. Examples (bench / DI / hard, % of one core, before → after): JCM800
33.2 → 33.5 / 46.3 → 46.9 / 105.1 → 105.5; Rectifier 45.0 → 44.0 / 74.0 → 74.0 / 192.7 → 191.3; 5150 67.9 → 68.0 /
109.9 → 111.2 / 268.3 → 280.7; Plexi 76.6 → 77.5 / 91.2 → 91.1 / 129.1 → 128.8. The 44 pedals and studio units (one round,
spread ±5-10 %) sum to bench 832 → 820 % (−1.5 %), DI 913 → 902 % (−1.3 %), hard material 1405 → 1396 % (−0.6 %).

**What is left of the noise cost, and the levers.**
- Generation is still 2-9 % of an amp: Gaussians about 15 % of it, the block product 25-45 %, shot noise, the hold
  FIR, the gathers and the shot currents' K product the rest.
- Hum on the rate-ladder amps (AC30, 1959RR, Plexi): the heater nodes are inputs of the 16x power group, which then
  evaluates every AC source at its sub-sample times. Evaluating only the sources a group reads, or deriving the heater
  sines from the mains sine by angle addition, would remove most of it. That is mains/ladder code.
- The 48 kHz op-amp groups' extra Newton iteration (above): a kept factorisation for the predictor correction.
- 1/f banks at 48 kHz (Phase 90, Memory Man, CE-1): 3-6 AR sections per op-amp source, each with a Gaussian per
  sample. The sections under a few hundred Hz could run decimated; that needs its own spectral check.

### 10.35 Worst-case CPU: `bench --real`, the budget table, and why the LTE share is not exactly reducible (2026-11-12)

CPU plan items P0 and P1. The tools and profiling counters landed. No P1 change met the bar, so the engine's arithmetic
is unchanged (output hashes identical, below).

**P0: `circuit_cli bench <netlist> --table <id> --real`** (tools/circuit/circuit_cli.cpp, `realBench`). After the chord
bench, it renders two materials through the unit's own path. That path is a 48 kHz host, the CircuitOversampler, 64-frame
blocks with the lanes forked per block, noise and hum on, and a freshly compiled circuit per run.
- **di:** the lead DI, 6 s from 2 s in (`--wav` replaces it).
- **hard:** sustained sines at −6 dBFS (2.5, 3.18, 5, 7, 10 and 14 kHz, 0.5 s each), a 0–20 kHz linear sweep at −12 dBFS
  (2 s), and full-scale bursts (82.4 Hz for 1 s, 110 Hz for 0.5 s, with 0.25 s gaps).
- **Per material it prints:**
  - % of one core: each block's minimum wall time over `CE_REALREPS` runs (default 3), summed. The render is
    deterministic, so this is the cost without other load; "single" is the fastest whole run.
  - Newton iterations and LTE substep events per internal sample, halvings, homotopies and failures.
  - p50 / p99 / max per 64-frame block against the 1333 µs period, and how many blocks exceed 60 % of it.
  - An output hash, and on macOS the retired instructions per audio second.
  - For hard, the cost of each segment.
- **Switches:** `CE_REAL=di|hard` selects a material. `CE_BENCH_DUMP=<prefix>` writes the host-rate output as raw doubles,
  for nulls between builds (with `CE_NOISE=0`).

**The budget table.** `work/circuit-netlists/cpu-9900x/budget.sh` runs `bench --real` for every bound unit on the
9900X test machine, pinned to two physical cores. It writes bench, DI, hard, hard ÷ DI, iterations, LTE events, block p99 /
max, the worst hard segment and the factor still needed against §3.5. The tables are `budget-20261112-after.md` (the one
to quote) and `budget-20261112-before.md` (the first run, with a noisier estimator).
- **Where the units stand** (after table): no amp is within §3.5. The amps' hard ÷ DI has a median of 2.31 (range
  1.32–3.93), the pedals' 1.54, the studio units' 1.62. Six light pedals are within: both Cry Babies, the Micro Amp,
  Electric Mistress, Rangemaster and Clyde McCoy. The JCM800 is at 34 / 46 / 105 % (bench / DI / hard), a factor 2.4.
- **Measurement noise:** other jobs bench on the same machine. Single runs of one deterministic render varied by up to
  30 % (5150 DI 111 % against 142 %, JCM800 hard 105–126 %). Hence the per-block minima.
- **What drives "hard":** cost rises with the sine's frequency in every unit, and the 14 kHz sine is the worst segment in
  63 of 68. The full-scale LF bursts cost less than the DI (JCM800 bursts 40 %, DI 46 %). Hard ÷ DI therefore measures
  sustained HF at −6 dBFS.

**P1 diagnosis, JCM800 hard material** (M4 Pro, unit path; `CE_PROFILE=1` prints the counters below per group):
- **The totals.** Newton work is 18.6 iterations per sample against 7.1 on the DI, with 1.18 LTE events per sample
  against 0.21.
  - On hard material, the preamp group (8 ports) substeps in 51 % of samples and the power group (11 ports) in 66 %, mostly
    at m = 4.
  - 98 % of the events are continuations: only 33 k of 1.58 M were found by rejecting a full step.
  - The substeps take 76 % of the hard-material time: 1.24 of 1.73 µs per sample in the preamp group, 3.12 of 3.76 µs in
    the power group.
- **Iterations per solve.**
  - Substep solves take 3.0–4.0 iterations, and none converges in one. The full steps take 1.5–1.8.
  - The substeps sit on the knees: they happen exactly where a device changes regime (`nonlinearEvent`).
- **Which states fail the LTE test** (`CE_LTEWHO=1`).
  - Preamp group: V2B, V2A, C8, V1B, C4 and C5, i.e. every stage after V1A.
  - Power group: everything except the tone stack: the OT winding capacitors and primary, the speaker's Le, the power tubes'
    and PI's capacitances, and CPP.

**P1 measured, not landed** (only Newton's start moves in each; LTE event counts unchanged):

| Change | JCM800 hard, it/sample | Other units | Verdict |
|---|---|---|---|
| Substep start through t[n−1], t[n] and the rejected full step's solution (quadratic, at the substeps' real times) | 18.63 → 18.91 | — | worse |
| Each substep runtime's own (v, p) history plus the kept-factorisation correction, as the full step does | 18.63 → 19.19 | — | worse |
| Quadratic full-step predictor on all groups (`CE_PREDQPORTS=0`; default 24) | 18.05 (−0.7 % instructions); DI 7.11 → 6.15 (−2.3 %) | 5150 −0.3 / −0.1 %, Rectifier −0.4 / −0.1 %, JC-120 (> 4 ports) DI −9.7 % but hard +3.0 % | no gain on the worst case |
| Chord gate 30 → 300 → 1e5 (`CE_CHORDGATE`) | 18.63 / 18.63 / 18.62 | — | no effect |

This confirms CPU_THEORY R2.3: the substep iterations come from the knee, not from the guess.

**A kept factorisation on 1–2 port solves (P3 follow-up, §10.34): measured, not landed.**
- **The idea.** The closed-form kernel (NN ≤ 2) keeps no factorisation. So groups that draw noise every sample (the
  48 kHz units' op-amps) cannot take the predictor correction. The prototype factorised I − K·Jd at acceptance and
  corrected every sample on those groups.
- **Iterations per sample on the DI fell:** Dimension D 47.3 → 37.4, SSL bus comp 18.1 → 15.6, Phase 90 21.8 → 17.3,
  CE-1 15.2 → 12.4.
- **Instructions did not** (M4 Pro, no lanes, DI / hard; 11 units): −1.5 % to +2.0 %.
  - Dimension D −1.4 %, Phase 90 −1.5 %, CE-1 −1.3 / −0.8 %, Small Stone +0.2 %, Lune +1.7 / +2.0 %.
  - A 1–2 port iteration costs about what the correction plus the kept factorisation cost.
  - Dimension D's noise cost is in the draws, not in Newton: noise off → on is 5.22 → 6.68 G instructions/s, but only
    46.1 → 47.3 iterations per sample.
- **The clamp-free solves had to be excluded.** Applied to the lazy-clamp clamp-free solves too, the change altered the
  Deluxe Memory Man's LFO (U5A/U5B): DI null −26 dB. The corrected start landed on the in-band middle branch that
  `Circuit::newton`'s clamp comment warns about.
- **Without those solves** the nulls are −153 to −255 dB rms on all 15 48 kHz units. It was still left out, for no CPU gain.

**P1 items 2 and 3 are already what the engine does.**
- LTE control is per group (`GroupRt::lteOn`), and an event re-solves only its own group (`substepGroup(gi, …)`). Groups
  on the rate ladder (level > 0) have no LTE at all.
- The finer step is substepping per stage inside a group. That needs the torn block solve. ENGINE_QUEUE ("Torn solve and
  per-block LTE") bounded it at 1.2–1.35× on the 5150, with a Newton step no cheaper than the sparse LU.
- On the JCM800 the failing states cover every nonlinear stage of both groups, so a split would leave out only V1A and
  the tone stack.

**Where that leaves the worst case.** No exact change brings hard ÷ DI from 2–3× to 1.5×. The ratio is the LTE control
doing its job: at 192 kHz, a −6 dBFS sine at 5–14 kHz pushes the clipping stages through a knee every few samples.
Three levers move it, and each changes the output, so each is a decision for the user:
- the per-netlist BDF2 start with a looser tolerance (§10.16, CPU_THEORY R2.3). It is already taken on the 5150, SLO-100,
  DR103, AC30, AC15, Bassman, 5E3 and Twin. The JCM800 and Plexi stayed on backward Euler for aliasing, so it remains open
  only there and on units never evaluated;
- a higher fixed rate for the groups that substep on every sample (P4);
- budgeting "worst" on material closer to playing than sustained −6 dBFS tones at 10–14 kHz.

**Proof.**
- The engine edits are counters and `describeGroups` output only: `lteEvents`, `lteDetect`, `subNs` (under `CE_PROFILE`),
  `lteWho` / `lteTally` (`CE_LTEWHO`), and `CE_PREDQPORTS` (default 24, as before).
- They were applied alone to the pre-change tree on the 9900X test machine.
- DI and hard output hashes, Newton counts and LTE counts are identical on 14 units: JCM800, 5150, JC-120, Rectifier,
  Plexi, AC30, Twin, DS-1, HM-2, Fuzz Face Si, Dimension D, Deluxe Memory Man, Phase 90 and Fairchild.
- Suites (Mac, with the P3 work's concurrent edits in the tree):
  - test_circuit_unit, test_effect_handoffs and test_aliasing pass (48 of 48 gated).
  - test_circuit_engine fails only on or120_* and re201_play_ac, as in §10.30 and §10.33.

### 10.36 Per-group rates (CPU plan P4): measured, no netlist change lands at flat cost (2026-11-13)

The question: can the rate ladder raise only the groups that alias and lower the linear ones, so that aliasing improves
at about the same CPU? Measured on a Ryzen 9 9900X test machine for the three 2x amps and nine 4x amps. Data,
scripts and the full table: `work/circuit-netlists/cpu-9900x/p4/`.

**Method.**
- **Probe.** The black-box study's matched-drive metric: 3183, 7001 and 9973 Hz at −6 dBFS plus the unit's matched-drive
  offset, the study's knob settings, noise and hum off, through the unit path (`circuit_cli alias --wav`). The figure is
  the total non-harmonic power below 20 kHz, in dBc (`p4probe.py`).
  - It reproduces the study's `def_match` numbers to 0.2 dB: 5E3 −33.0 (study −33.2), Rectifier −24.6 (−24.7), JCM800
    −40.9 (−40.7), Plexi −48.3 (−48.2).
- **CPU.** `bench --table` plus `bench --real` (DI and hard material, §10.35).
- **Accuracy.** A circuit-level render against the whole circuit at 16x, scored as the residual after a 96-tap LTI fit.
  The fit takes out the ladder converters' IIR phase.

**What the netlists allow.**
- In 9 of the 12 amps, everything after the supply is one Newton group: the tone stack, NFB network and speaker sit inside
  the clipping groups. There is no exact multirate form for linear blocks (CPU_THEORY "Rejected"), so the only
  separately rated group is the supply.
- Lowering the supply below the amp group costs no aliasing. On both 2x amps, the amp group at 4x over a 2x supply
  equals every group at 4x to 0.1 dB. But it saves only 2–5 % of bench CPU.

**What decides the aliasing: LTE substeps, which the ladder turns off.**
- Whole circuit at 4x, with LTE against without:

  | Unit | 3183 Hz probe, with LTE | without LTE |
  |---|---|---|
  | Rectifier | −39.8 dBc | −28.1 dBc |
  | 5E3 | −50.7 dBc | −41.9 dBc |
  | JC-120 | −40.1 dBc | −35.2 dBc |

- A raised ladder group has no LTE (§3.7), so every "raise only the clipping group" candidate gives that back.
- On the 4x amps, a 2x base with the amp groups at their shipped rate is 2–21 dB worse: 5150, Uberschall, BE-100, Supro,
  Deluxe and JCM800. It is equal or up to 2.3 dB worse on the Plexi, AC30 and SVT.
- Tightening the LTE tolerance at 2x buys nothing:
  - Rectifier, 4 → 0.5: −24.6 → −23.5 dBc.
  - 5E3, 16 → 1: −33.0 → −30.9 dBc.
  - JC-120, 16 → 1: +4.5 dB at 3183 Hz, worse at 7 kHz, and +13 % DI and +27 % hard CPU.
- `circuit_cli autorate`, with the gate tone at matched drive and the gate tightened from −60 to −70 dB:
  - Rectifier: amp group 16x (−43.8 → −74.5 dB).
  - 5E3: amp group 8x at −60, 16x at −65 and −70 (−57.9 → −87.2 dB).
  - JC-120: left power amp 16x, plus the CH-2 preamp 4x at −70 (−56.2 → −73.8 dB). Any ladder rate also turns off the
    JC-120's lanes (§10.33).

**The trades (not applied: each raises CPU, so each is a decision).** Probe at 3183 / 7001 / 9973 Hz in dBc; CPU as
bench / DI / hard, % of one core.

| Unit | Shipped | Option | Aliasing change | CPU change |
|---|---|---|---|---|
| Rectifier | −24.6 / −23.3 / −21.5; CPU 43 / 74 / 193 | whole 4x | +15.2 / +17.0 / +14.5 dB | ×1.74 / 1.42 / 1.52 |
| Rectifier | | whole 8x | +25.6 / +25.0 / +25.2 dB | ×3.06 / 2.25 / 2.31 |
| 5E3 | −33.0 / −28.4 / −24.8; CPU 14.3 / 16.0 / 52.6 | whole 4x | +17.7 / +9.7 / +11.7 dB | ×1.88 / 1.74 / 1.57 |
| 5E3 | | amp group 4x on the ladder (no LTE) | +8.9 / +2.6 / +0.8 dB | ×1.73 / 1.61 / 0.82 |
| JC-120 | −31.0 / −25.5 / −21.6; CPU 86 (lanes) / 104 / 271 | whole 4x | +9.1 / +7.0 / +7.4 dB | ×1.80 / 1.68 / 1.38 |
| JCM800 | −40.9 / −30.6 / −28.5; CPU 34 / 46 / 105 | whole 8x | +9.2 / +7.6 / +5.6 dB | ×1.77 / 1.60 / 1.66 |
| JCM800 | | power group 16x | +5.5 / +9.7 / +4.9 dB | ×2.32 / 1.88 / 1.17 |

Accuracy against the 16x reference improves with each whole-circuit raise. Rectifier DI −51.3 → −68.7 dB and hard
−25.2 → −45.3 dB; 5E3 DI −59.4 → −72.4 dB.

**Cheaper but measurably different (not applied).** The SVT with a 2x supply and its amp group at 4x on the ladder:
- aliasing identical;
- CPU ×0.73 / 0.86 / 0.82;
- DI residual against the 16x reference −84.5 → −76.5 dB.

**Found on the way: in a rack, minRate and rates are ignored on netlists without an `oversampled` list.**
- `RackGraph::syncCabFollow` sets cabFollows on every amp. `CircuitUnit::onPrepare` then sets `hq_ = wantHq()`, which is
  false unless `multirate.oversampled` is listed, and `minRate` and the ladder rates apply only when `hq_` is true.
  `p4/hqcheck.cpp` shows it.
- **Amps affected:** measured on the Uberschall, SVT, BE-100 and Supro. By the netlists, also the Ecstasy, DR103, OR120,
  AC15 and Leslie 122 where they are bound as amps. They run at 2x in a rack, but at 4x in the aliasing gate and in
  `circuit_cli`. Pedals are not affected, because the rack tracks cabs only for amps.
- **Measured at 2x:**

  | Unit | 3183 Hz probe, 2x (4x) | Gate | Bench |
  |---|---|---|---|
  | Uberschall | −39.2 (−51.2) dBc | −47.8 dB | 48 % |
  | BE-100 | −27.6 (−38.2) dBc | −57.8 dB (fails −60) | 41 % |
  | Supro | −27.5 (−44.4) dBc | −69.4 dB | 16.5 % |
  | SVT | unchanged | −94.2 dB | 36 % |

- **The fix:** `wantHq` true when the netlist has a minRate or rates, as well as an `oversampled` list. It would raise
  those amps' live CPU to their bench figures, so it is left for the user's decision.

**Landed: the ladder evaluates only the AC sources a raised group reads** (Engine.cpp `ladderAcIdx_`;
the P3 note in §10.34).
- Before, a raised group that reads a mains or heater node evaluated every AC source at each of its sub-samples. Now it
  evaluates the ones it reads. Its last sub-sample still writes them all, so the groups after it read the same values.
- **Bit-identical:** bench chord and `--real` DI output hashes are equal on the Plexi, Plexi variac, 1959RR and AC30
  (Mac tree), and DI / hard hashes equal on the 9900X.
- **Gain:** about 1–2 %, at the noise level. 1959RR bench −1.6 to −1.9 %; AC30 `--real` DI −1.7 % and hard −1.5 %;
  Plexi bench −1.1 %.
- **Suites (build-p4):** test_aliasing (48 of 48 gated), test_circuit_unit and test_effect_handoffs pass.
  test_circuit_engine fails only on or120_* and re201_play_ac, as in §10.35; neither netlist uses the ladder.

**Antiderivative anti-aliasing (stateful, Holters DAFx-19) for the DK groups: not a lever here.**
- CPU_THEORY "Rejected" already prototyped it as the average-vector-field integrator: 0 to 10 dB worse than the
  trapezoid at 192 and 384 kHz. Every tube port sits across a capacitor, so the alias floor is the trajectory's
  truncation error at the knees, not the static law's spectrum.
- The data above agree. The LTE substeps, which refine time at the knees, are worth 9–12 dB; a tighter tolerance is not.
- The AC30 / Plexi aliasing is an OT leakage flyback at about 380 kHz (§10.32): a linear mode that no antiderivative of a
  device law removes.
- ADAA remains plausible only for memoryless clippers that are not across a capacitor (WDF diode roots, the §3.7
  plan). There, roughly 2x oversampling's worth (6–12 dB) for about 1.3x device cost and a half-sample delay to
  compensate.

**What would make P4 pay.**
1. LTE on ladder-raised groups, as a per-netlist option. A 4x amp group over a 2x supply would then match the whole
   circuit at 4x while keeping the supply cheap.
2. Lanes on ladder circuits (the JC-120).
3. The torn block solve (P5), so that tone stacks and NFB networks become separate groups that can run slower.

### 10.37 Small-signal path (CPU plan P2): a certified frozen linearisation per group, and where it pays (2026-11-13)

CPU plan item P2 asked whether the solid-state units, the JC-120 first, could stop paying Newton while they run
linearly. Now a group can solve a frozen linear system instead of Newton, while its ports stay inside a box where that
system is certified to within a small fraction of the Newton tolerance.
- **Default:** on.
- **Off switch:** `CE_NOSMALLSIG=1` or `CompileOptions::smallSignal = false`. Off is bit-identical to the engine
  without the path.
- **Code:** Engine.cpp, section "Small-signal path"; `SmallSig` sits beside `GroupRt`.
- **Logs and scripts:** `work/circuit-netlists/cpu-9900x/p2/`: the 9900X sweep, the Mac instruction counts and nulls, the measuring harness `p2cli.cpp` (circuit_cli with a silence / −30 dB material, `CE_REAL=silence`), and `apply_p2.py`, which applies this change alone to an Engine.cpp.

**The path.**
- **Freeze.** After a converged Newton sample, the group's devices are evaluated once at the solution v0, giving
  currents i0 and Jacobian Jd0.
  - Frozen solve: v_L = v0 + S·(p + q0), with S = (I − K·Jd0)⁻¹ and q0 = K·i0 − v0.
  - Currents: i_lin(v_L) = i0 + Jd0·(v_L − v0).
- **Per sample:** one nn×nn product and 2·nn comparisons. There is no device evaluation, factorisation or residual.
  linPorts, linOut, noise, LTE and the predictor history run as before.
- **The certificate.** Let r be the linearisation's remainder. The exact solution differs from v_L by e = S·K·r(v*)
  at the ports, and by G·r in the exports and state voltages, where G = Hi·(I + Jd0·S·K) and Hi is H's current
  columns.
  - Each port gets a remainder budget from the column maxima of |S·K| and |G|, each row weighted 1/(1 + |value|).
  - Each device gets a box around v0. Inside it, the device's sampled remainder stays within the budget of every one
    of its ports. The box is found like this:
    1. Search along each port's axis by doubling, then bisection.
    2. Test every corner and edge midpoint of the device's box, at full and half size.
    3. While a cross term fails, halve the widest port.
  - **Budgets** are tried at n×, √n× and 1× the equal share. A larger share is kept only if the exact rows pass with the
    remainders sampled in the final boxes. This lets reverse-biased junctions and open clamps hand their share to the
    curved ports.
  - **Taking v_L.** A sample takes v_L when every port lies at least its error bound η_k inside its box. The map
    v ↦ v_L + S·K·r(v) then sends the η-ball into itself, so the exact solution lies within η of v_L.
  - **Leaving.** Any port outside its box, or a NaN, ends the stretch, and that sample runs Newton.
- **When a freeze is tried.**
  - Only once the ports move less per sample than 1/8 of the last box.
  - The box search stops at the first device whose box the recent motion would cross within 8 samples, or that has no
    room inside its error bound.
  - A stretch under 256 samples, or a failed freeze, doubles the wait before the next try (up to 32768 samples).
  - A change of K (deriveFromZ: controls, refreshes) ends the stretch.
  - The countdown runs at the call site, so a group that never certifies pays about one decrement per sample.
- **Excluded groups:**
  - groups with hidden device state (hysteresis cores, speakers' thermal ΔRe);
  - WDF groups;
  - regenerative loops;
  - groups whose device-port currents another group reads as a cut term. That excludes every tube stage on a cut B+,
    and the JC-120's supply-fed preamp, mixer and power amps.
- **Rigour.** The boxes are sampled, not interval bounds. `CE_SSCHECK=1` runs Newton from v_L on every hit and records
  the worst port and output error, in tolerances. On the JC-120 and the dbx 160 it read 0.02–0.05 of the bound.
- **Developer switches:** `CE_SSSIGMA`, `CE_SSDIAG=1` (every freeze's box), `CE_SSONLY=<group>`. `CE_PROFILE` adds
  hits, freezes and freeze time to `describeGroups`.

**σ = 1e-4 of the Newton tolerance, not 1.**
- **The trap.** A frozen linearisation's remainder keeps one sign over a stretch, and a long time constant integrates
  it. The dbx 160's log-domain RMS detector does exactly that.
- **The dbx 160 DI null** against the path off (noise off, 4 real DIs; "driven" is the same DIs through the bench's
  fuzz stage):

  | Setting | real: rms / peak | driven: rms / peak |
  |---|---|---|
  | σ = 1e-2 | −123 / −78 dBFS | −150 / −111 dBFS |
  | σ = 1e-3 | −138 / −92 dBFS | −169 / −138 dBFS |
  | σ = 1e-4 | −166 / −118 dBFS | −178 / −139 dBFS |
  | Path off, Newton tolerance 1e-6 → 1e-7 (the yardstick) | −153 / −116 dBFS | −111 / −83 dBFS |

  At 1e-4 every null is at or below that tolerance change.
- **What the smaller σ costs:** little where the path pays, because a silent circuit sits far inside any box. The TS808
  in silence is −29 % at 1e-4 against −38 % at 0.1.
- **The first version certified the ports only**, not the outputs. It failed d160_*, otaopt_*, mt3_* and opto_trem by
  up to 6000 % NRMSE. The RMS detector's square law injects into a 1 Ω node that is an output, not a port, and an
  infinite box passed it. The G rows fixed it.

**Why the JC-120 does not pay.**
- **The scale mismatch.** At the 1e-6 contract, a forward junction's box is 1–60 µV; for a 2.8 mA pnp it is 0.75 µV.
  The amp's "linear to −65 dBc" is 5.6e-4 at the output: 500× the tolerance. It is larger still at the device ports,
  because the feedback hides it at the output.
- **What moves the ports in silence.** The supply ripple moves the power-amp collector ports by 30–500 µV per sample,
  and the preamp's by 2–8 µV. Thermal noise moves the junctions by tenths of a µV.
- **Which of the 14 groups take the path:**
  - The four small op-amp groups (3, 5, 10, 14) hold the path 100 % of the time in silence. Each costs 1–2 % of the
    amp.
  - The supply (0), the reverb driver (9) and the vibrato LFO (13) never certify.
  - The preamp, mixer and power amps read supply cuts, so they are excluded. In a test at σ = 0.1 with the exclusion
    lifted, the preamp and mixer froze but left within 1–30 samples, and the power amps never certified.
- **The result:** Newton work in silence falls from 14.3 to 10.6 iterations per sample, but instructions fall only
  1.2 %. The rest of the work is in groups that cannot take the path.

**Tube amps' quiescent stretches.** Every tube stage reads the B+ cut, so it is excluded.
- With that lifted for a test, the JCM800's preamp group tried 177 freezes in 6 s of silence, with hum and noise off,
  and certified none.
- The supply groups hold 0.9 % of samples.
- The Twin's small groups engage: −1.4…−1.9 % of instructions on every material.
- JCM800, 5E3 and Dual Rectifier: no change.

**Instructions per audio second** (M4 Pro, load-independent; unit path, lanes off, noise and hum on). Each cell is G
instructions per audio second, path off → on, with Newton iterations per sample in parentheses.

- Silence: 6 s.
- Quiet: the four real DIs at −30 dB, 24 s.
- Real: the four DIs, 24 s.
- Driven: the DIs through the bench's fuzz stage.

The pedal and studio rows come from a build without the last two changes (the early stop for boxes with no room, and
the countdown at the call site). Those only make failed attempts cheaper: the Big Muff went from +0.7 % to +0.1 %.

| Unit | Silence | Quiet | Real | Driven |
|---|---|---|---|---|
| TS808 15000 | 1.93 → 1.36 (−29.2 %; 4.98 → 1.04) | 1.94 → 1.81 (−6.6 %) | 2.06 → 2.03 (−1.2 %) | 2.15 → 2.15 (−0.3 %) |
| TS9DX 167000 | 2.63 → 1.56 (−40.5 %; 4.02 → 1.02) | −4.7 % | −2.0 % | 0.0 % |
| Klon 179000 | 2.83 → 2.05 (−27.6 %; 4.11 → 0.00) | −10.0 % | −8.2 % | −4.6 % |
| Micro Amp 65000 | 0.60 → 0.46 (−23.3 %; 1.50 → 0.00) | −23.3 % | −23.3 % | −5.8 % |
| Electric Mistress 74000 | 0.49 → 0.41 (−15.4 %) | −15.1 % | −6.7 % | −0.6 % |
| RAT 42000 | 3.31 → 2.87 (−13.4 %; 3.51 → 1.28) | −7.0 % | −5.4 % | −4.8 % |
| Phase 90 75000 | 1.60 → 1.39 (−12.9 %; 20.3 → 11.1) | −12.8 % | −9.1 % | −2.1 % |
| Bluesbreaker 180000 | 3.76 → 3.31 (−12.0 %) | −12.1 % | −11.6 % | −3.7 % |
| HM-2 165000 | 4.93 → 4.35 (−11.8 %) | −3.5 % | −0.7 % | −0.9 % |
| DS-1 17000 | 2.80 → 2.50 (−10.8 %) | −4.5 % | −0.5 % | +0.3 % |
| dbx 160 125000 | 5.45 → 4.89 (−10.3 %; 14.3 → 5.9) | −3.4 % | −3.2 % | −3.2 % |
| MT-2 46000 | 3.70 → 3.33 (−10.0 %) | −6.3 % | −0.5 % | +0.3 % |
| Memory Man 82000 | 3.04 → 2.77 (−9.0 %) | −7.5 % | −3.2 % | −1.6 % |
| Dimension D 142000 | 6.43 → 5.91 (−8.1 %) | −7.8 % | −5.9 % | −0.7 % |
| SSL 4000E dynamics 133000 | 5.63 → 5.23 (−7.1 %) | −2.4 % | −1.7 % | −1.6 % |
| SSL 4000G bus comp 130000 | 6.02 → 5.83 (−3.2 %) | −3.1 % | −2.8 % | −2.6 % |
| SSL 4000E EQ 121000 | 4.85 → 4.70 (−3.1 %) | −3.2 % | −3.1 % | −3.0 % |
| CE-1 29000 | 1.74 → 1.71 (−2.1 %) | −2.1 % | −0.1 % | +0.1 % |
| Twin Reverb 30000 | 7.02 → 6.89 (−1.9 %) | −1.8 % | −1.6 % | −1.4 % |
| JC-120 49000 | 16.75 → 16.55 (−1.2 %; 14.3 → 10.6) | −0.4 % | 0.0 % | 0.0 % |
| Small Stone 20000 | −0.8 % | −1.0 % | −0.9 % | −0.2 % |
| JCM800 56000, 5E3 79000, Dual Rectifier 16000, Dyna Comp 48000 | 0.0…+0.1 % | 0.0…+0.1 % | 0.0 % | 0.0 % |
| Big Muff 43000, Cry Baby GCB-95 71000 (no hits) | +0.1…+0.2 % | +0.1…+0.2 % | +0.2 % | +0.2 % |

**Ryzen 9 9900X wall clock** (the final code; `bench --table <id> --seconds 3
--real`, `CE_REPS=5 CE_REALREPS=2`, pinned to CPUs 4 and 6 as budget.sh).
- Columns: % of one core, and the p99 64-frame block in µs (1333 µs period).
- Base and P2 builds were run back to back. Identical renders spread by ±3 % here: the JCM800 and the Rectifier read
  +2…+4 % with identical instruction counts.

| Unit | Bench chord | Real DIs: mean / p99 | Driven: mean / p99 | Silence | Quiet |
|---|---|---|---|---|---|
| Micro Amp 65000 | 2.4 → 1.8 | 2.5 → 1.9 / 34 → 36 | 2.6 → 2.5 / 41 → 42 | 2.7 → 1.8 | 2.7 → 1.8 |
| TS808 15000 | 7.9 → 7.3 | 9.0 → 8.8 / 139 → 138 | 9.4 → 9.7 / 152 → 157 | 7.9 → 5.3 | 8.1 → 7.5 |
| TS9DX 167000 | 13.5 → 12.5 | 14.4 → 13.5 / 215 → 208 | 16.2 → 15.9 / 257 → 254 | 12.9 → 6.1 | 12.8 → 12.5 |
| Klon 179000 | 13.5 → 12.3 | 13.8 → 12.9 / 206 → 197 | 14.9 → 14.8 / 233 → 232 | 13.3 → 8.0 | 13.2 → 11.9 |
| RAT 42000 | 16.1 → 14.8 | 17.6 → 16.5 / 277 → 257 | 18.6 → 17.2 / 297 → 278 | 14.5 → 11.9 | 14.8 → 13.8 |
| DS-1 17000 | 15.7 → 15.0 | 17.0 → 17.0 / 300 → 299 | 18.9 → 18.8 / 354 → 354 | 14.7 → 12.7 | 14.7 → 14.1 |
| MT-2 46000 | 21.6 → 19.9 | 22.6 → 22.4 / 348 → 349 | 24.2 → 23.9 / 379 → 373 | 20.4 → 17.8 | 20.7 → 19.2 |
| HM-2 165000 | 30.4 → 27.2 | 31.4 → 31.6 / 499 → 508 | 33.7 → 33.0 / 606 → 600 | 27.0 → 23.2 | 28.4 → 27.6 |
| Phase 90 75000 | 7.2 → 6.2 | 7.1 → 6.6 / 106 → 106 | 7.5 → 7.4 / 120 → 118 | 7.0 → 6.2 | 7.1 → 6.2 |
| Memory Man 82000 | 16.3 → 14.3 | 16.1 → 15.4 / 245 → 238 | 16.7 → 16.1 / 258 → 250 | 15.7 → 14.1 | 16.0 → 14.6 |
| Dimension D 142000 | 23.5 → 20.3 | 22.9 → 20.8 / 359 → 336 | 23.2 → 23.1 / 366 → 366 | 22.1 → 19.6 | 22.3 → 20.1 |
| CE-1 29000 | 10.3 → 10.3 | 10.6 → 10.7 / 191 → 192 | 11.6 → 11.7 / 218 → 222 | 9.4 → 9.0 | 9.6 → 9.1 |
| Big Muff 43000 | 11.6 → 11.6 | 12.3 → 12.2 / 190 → 186 | 12.8 → 12.7 / 205 → 197 | 10.6 → 10.6 | 11.3 → 10.8 |
| SSL 4000E EQ 121000 | 24.4 → 23.5 | 24.1 → 22.8 / 348 → 331 | 23.6 → 23.1 / 344 → 337 | 24.1 → 23.3 | 24.4 → 23.0 |
| SSL 4000E dynamics 133000 | 33.3 → 33.4 | 33.2 → 33.5 / 514 → 521 | 36.6 → 36.8 / 605 → 613 | 30.6 → 29.1 | 31.1 → 31.6 |
| SSL 4000G bus comp 130000 | 28.4 → 28.0 | 33.3 → 32.2 / 652 → 638 | 36.5 → 35.0 / 709 → 686 | 27.6 → 26.8 | 29.6 → 29.0 |
| dbx 160 125000 | 29.2 → 28.9 | 28.6 → 28.1 / 417 → 409 | 29.1 → 28.2 / 425 → 413 | 28.4 → 25.4 | 28.8 → 28.0 |
| Twin Reverb 30000 | 46.2 → 45.5 | 44.4 → 43.5 / 683 → 667 | 51.5 → 51.4 / 969 → 963 | 39.6 → 38.6 | 40.5 → 40.0 |
| JCM800 56000 | 32.4 → 33.1 | 47.0 → 46.9 / 1523 → 1523 | 58.5 → 58.9 / 1523 → 1514 | 32.0 → 33.3 | 32.3 → 32.6 |
| 5E3 79000 | 14.3 → 14.2 | 14.7 → 14.6 / 295 → 291 | 19.0 → 19.2 / 475 → 481 | 11.8 → 11.9 | 12.3 → 12.5 |
| Dual Rectifier 16000 | 42.3 → 42.9 | 63.3 → 63.8 / 1770 → 1786 | 71.4 → 72.2 / 1926 → 1954 | 29.1 → 29.7 | 33.6 → 33.9 |
| JC-120 49000, lanes (two interleaved rounds) | 85.7 / 82.7 → 84.6 / 85.5 | 99.8 / 98.0 → 97.8 / 97.4; 2228 / 2199 → 2207 / 2188 | 125.2 / 123.5 → 124.3 / 123.0; 3076 / 3022 → 3047 / 3008 | 74.3 → 73.6 | 80.3 → 80.2 |
| JC-120 49000, no lanes (same rounds) | 109.7 / 111.6 → 109.7 / 107.1 | 131.5 / 129.8 → 130.6 / 129.4; 2955 / 2994 → 2941 / 2897 | 161.1 / 159.7 → 160.8 / 160.0; 3968 / 3965 → 4039 / 4009 | – | – |

- **The JC-120 rows** come from two rounds of base and P2, lanes on and off. They are interleaved because a first
  back-to-back pair had the base's lane helper idle-detaching in 94 % of blocks: 132 % against 83 %, a load artefact.
  The silence and quiet figures are from that first pair (lanes engaged in both).
- **The JC-120's change is within the run-to-run spread:**
  - bench +1 / −3 %;
  - real DIs −1…−2 %;
  - combined p99 block 2860–2874 → 2828–2862 µs.
- Its instructions agree: 0.0 % on real DIs and the driven set.

**The JC-120 against §3.5 after P2** (9900X, lanes engaged):

| Measure | JC-120 | Budget | Factor over |
|---|---|---|---|
| Bench | 83–86 % | 17–21 % | 4–5× |
| Real DIs, mean | 97–98 % | 32 % | 3× |
| Real DIs, p99 64-frame block | 2.19–2.21 ms (164–166 % of the period) | | |
| Driven set | 123–124 % | 43 % (worst case) | 2.9× |

- Without lanes: 107–112 % bench, 129–131 % real DIs, 160 % driven.
- P2 does not move it. What is left: the torn solve of its 16–18-port groups (P5) and further lanes (P6). The
  alternative is accepting a linear model of the amp, which is a sound change and so the user's decision.

**Where it pays.**
- **Op-amp pedals at rest:** −10…−52 % in silence and −3…−13 % on −30 dB DIs. Ranked by the silence figure:
  - TS9DX, Klon, TS808, Micro Amp: −24…−52 %;
  - RAT, HM-2, MT-2, DS-1: −11…−18 %;
  - modulation pedals: −9…−13 %.
- **While playing:** up to −7 % on the bench and −2…−9 % on real DIs where a stage the signal does not reach stays put
  (RAT, Klon, TS9DX, Phase 90, Dimension D, Memory Man, the Micro Amp at −21 %). The p99 block moves with the mean,
  e.g. RAT 277 → 257 µs.
- **Studio units:** −3…−10 % at rest and −2…−5 % while playing: the dbx 160, the SSL cards and the bus compressor.
- **Not here:** the JC-120, the tube amps, the BJT fuzz pedals (Big Muff, Fuzz Faces) and the Dyna Comp gain nothing.
  Their ports carry ripple or noise beyond the boxes, or they read supply cuts. Their overhead is at most +0.2 % in
  instructions.

**Gates** (Mac build-p2, the tree as of this change, which also holds the P5 work in progress):
- **test_circuit_engine, path on against off.** Both runs fail only re201_play_ac, the existing failure.
  - Every jc120 case is identical.
  - Four printed figures move in their last digit:
    - opto_trem max 2.53 → 2.54e-07 V;
    - bp_drive worst H2–H5 0.01 → 0.02 dB;
    - zen_small / zen_hot max ±1e-9 V.
  - Informational lines that move: the Small Stone PTC check's iterations, 3.18 → 2.19 per sample.
  - testLanes is byte-identical with the path on (JC-120, Fairchild, Dimension D).
- **test_circuit_unit:** passes. **test_effect_handoffs:** passes.
- **test_aliasing:** passes, 48 of 48 gated units, and its output is identical with the path on and off.
- **DI null against the path off** (noise off; the four real DIs, the driven set, 6 s of silence and the −30 dB DIs):
  - Nulled at σ = 1e-4: the JC-120, all three SSL cards, the dbx 160, TS808, TS9DX, RAT, DS-1, Klon, MT-2, HM-2,
    Phase 90, Memory Man, CE-1, Dimension D and Micro Amp.
  - All but the dbx 160 are at or below −166 dBFS rms and −132 dBFS peak.
  - JC-120, with the final build: −197 / −151 dBFS (real) and −179 / −139 dBFS (driven).
  - dbx 160, with the final build: −185 / −145 dBFS (real), −181 / −145 dBFS (driven) and −163 / −130 dBFS (−30 dB
    DIs). These sit below what tightening Newton's own tolerance does to that unit (the σ table above, from the earlier
    build).
- **Off switch:** `CE_NOSMALLSIG=1` is bit-identical to the build without the path. The real and driven output hashes on the 9900X match for the JC-120, TS808, SSL EQ, dbx 160, JCM800 and RAT.

**Notes for the other engine work.**
- The path reads `GroupRt::K` and `H`. If the torn block solve (P5) ever stops forming the dense Z for its groups,
  ssFreeze must skip them.
- A cut-term row (the device-port currents a supply cut reads) would let tube and supply-fed stages take the path. It
  pays nothing today, because B+ ripple keeps those ports moving.

### 10.38 Block-bordered (torn) DK solve (CPU plan P5): exact, but slower than the joint solve; not landed (2026-11-13)

CPU plan item P5 and CPU_THEORY #2. The idea: solve each amp's big group as small stage blocks joined at separator nodes,
exactly, so the per-sample cost scales with the sum of the blocks. It was built, it is exact, and it is slower on every
measured path. The engine is unchanged. The prototype is kept as `work/circuit-netlists/cpu-9900x/p5/p5-torn-prototype.patch`
(it applies to Engine.cpp / Engine.hpp as of this date), with the 9900X logs in `p5/res-9900x/`.

**Method (the prototype).**
- **Partition** (at reset, per DK group with ≥ 24 unknowns). The group's unknown graph is M's pattern plus each
  device's nodes as a clique. Its articulation points are the candidate separators. They are merged back greedily while
  an operation-count model of the per-sample work falls.
  - 5150 big group (96 unknowns, 29 ports, W 125): 13 blocks over 9 separators (`ka4 g2a g2b nb2 fxpad nfb n4b pa pb`).
    The largest block is the PI plus power tubes: 10 ports, 14 interior nodes, 4 separators. The OT/speaker block has 15
    nodes and 1 port.
  - JCM800 power group (44 unknowns, 11 ports): 5 blocks over 3 separators (`nfb pa pb`). The PI, the power tubes and the
    tone stack stay in one 10-port block.
- **Algebra.** With x_S held, each block is a small dense system. x_S solves the m×m Schur complement
  Σ = M_SS − Σ_b M_Sb·M_bb⁻¹·M_bS. So K = Kd + Ls·Σ⁻¹·G: a block-diagonal Kd plus a rank-m border.
  - The linear update (p, exports, states) becomes block-local gemvs plus one m×m separator product.
  - The Newton step (I − K·Jd)·d = F becomes a block-bordered Woodbury solve: per block P_b = I − Kd_b·Jd_b, then the m×m
    capacitance matrix C = Σ − G·Jd·P⁻¹·Ls.
  - F and the residual test keep the dense K, so convergence decisions are the joint solve's.
- **Implementation.** Column-major block matrices (the products run down the rows), an explicit Σ⁻¹, closed-form 1- and
  2-port blocks, reciprocal-pivot LU elsewhere. Refreshed allocation-free on every M / R change (refreshGroup, smUpdate),
  and built for the substep runtimes too. Switches: `CE_NOTORN`, `CE_TORN=lin|newton`, `CE_TORNFORCE`, `CE_TORNDIAG`.

**Exactness.** The prototype is exact. 5150, unit path, the four real-playing DIs, noise off, torn against joint:
−142.0 dB rms (real) and −145.0 dB rms (driven), peak −129.7 dBFS. That is the class of the landed sparse LU (−135 dB).
Newton iterations, LTE events and homotopies were identical per sample.

**CPU, Ryzen 9 9900X** (scratch build of the same tree, `taskset -c 4,6`, `bench --table` 3 s chord + `bench --real`
with two runs' per-block minima; % of one core; p99 / max is the combined 64-frame block against the 1333 µs period):

| Unit | Path | Bench | Real DI | Driven DI | Block p99 | Block max | Newton solve (`CE_KBENCH`) |
|---|---|---|---|---|---|---|---|
| 5150 | joint (shipped) | 66.2 | 113.9 | 117.3 | 2803 µs (210 %) | 3814 µs | 950 ns |
| 5150 | torn linear update only | 69.9 | 116.5 | 120.4 | 2858 µs (214 %) | 3918 µs | 997 ns |
| 5150 | torn Newton step only | 85.1 | 163.7 | 166.3 | 4147 µs (311 %) | 5704 µs | 1676 ns |
| 5150 | both | 92.8 | 166.5 | 171.9 | 4235 µs (318 %) | 5746 µs | 1676 ns |
| JCM800 | joint (shipped) | 33.5 | 47.0 | 59.3 | 1525 µs (114 %) | 1970 µs | 282 ns (power) |
| JCM800 | torn linear update only | 38.8 | 54.5 | 68.6 | 1785 µs (134 %) | 2294 µs | 277 ns |
| JCM800 | torn Newton step only | 49.1 | 76.7 | 101.4 | 2876 µs (216 %) | 3785 µs | 783 ns |
| JCM800 | both | 52.3 | 84.1 | 110.9 | 3133 µs (235 %) | 4049 µs | 848 ns |

A second joint run of the 5150 read 68.2 / 115.4 / 119.9 %, so the run-to-run noise is about ±2 %. The M4 Pro agrees.
5150 Newton solve 1023 ns joint against 2086 ns torn; JCM800 power group 260 against 935 ns. 5150 retired instructions:
+4 % with the torn linear update and +38 % with the torn Newton step.

**Why it loses.**
- **The joint solve already uses the structure.**
  - The sparse linear update is the flop-minimal elimination: about 800 indexed operations per solve. The block form needs
    about 3000 multiply-adds, because a block's rows read all of its columns.
  - The Woodbury factorisation drops dead ports (18–21 of 29 live on the 5150). It runs unrolled fixed-size kernels.
- **The power stage is one block.** The PI, the power tubes, the NFB and presence networks and the OT form a biconnected
  10-port block. It carries most of the factorisation in both forms, so tearing saves at most about 40 % of the
  factorisation flops (about 1900 against 3200 per factorisation on the 5150).
  - That saving is spread over a dozen small blocks of 1–4 ports and a 9×9 capacitance matrix. Each pays loop and
    indexing overhead that the unrolled 19-live-port kernel does not.
  - Even at the flop bound, the gain would be about 35 % of the factor-and-step share (31 % of the 5150's time):
    roughly 1.1×.
- **On 8–11-port groups** (the JCM800, Plexi and AC30 power groups) the fixed-size kernels are already near optimal.
  Tearing only adds work.

**Not tried, and why.**
- Per-block Newton (block Gauss–Seidel) and per-block chord reuse converge to the same fixed point but along a different
  path. Such path changes have nulled poorly here (the cross-sample chord: −98 dB rms, −55 dB peak, §10.30), so they are
  not the exact reformulation asked for.
- Per-block LTE substeps (CPU_THEORY #3) are the one gain the torn structure would unlock. They are not exact, and
  ENGINE_QUEUE bounded them at 1.2–1.35×.
- Linear passive blocks as precomputed transfer blocks: the DK elimination already folds them into K, Dp and H. In the
  torn form they are blocks without ports (the prototype's tone-stack, winding-capacitance and speaker blocks), so they
  are covered by the measurement above.

**Side observation (not acted on).** On the 9900X, the 5150's dense linear update (`CE_DENSELIN=1`) measured 66.1 / 110.8
/ 115.8 % against the sparse update's 66.2–68.2 / 113.9–115.4 / 117.3–119.9 %. That is within noise, and the sparse/dense
weight was tuned on the M4. Re-tuning `buildSparseLin`'s weight per architecture would need a proper interleaved run.

### 10.39 Waveform relaxation across a cut (one amp on 2–3 cores): exact, but 11–17 sweeps per block; not landed (2026-11-13)

**The idea.** Let one heavy amp use 2–3 cores, exactly to solver tolerance. Split its big group at weakly coupled
separators, and iterate the parts over one block (waveform relaxation, WR): each part integrates the whole window from
the other part's separator waveform of the last sweep, until the waveforms stop moving. If the coupling were mostly
one-way (preamp → power amp), a pipelined Gauss–Seidel sweep would converge in 1–3 sweeps and each part could run on
its own core.

It was built as an offline prototype on the 5150. It is exact, but it needs 4–6× more sweeps than would pay, on any
number of cores. The engine is unchanged. The prototype is in `work/circuit-netlists/cpu-9900x/wr/`: the engine hook
(`wr-prototype.patch`), the probe (`wr_probe.cpp`), the variant netlists (`make_variants.py`), the run scripts and the
logs (`res-mac/`, `res-9900x/`).

**Method.**
- **The split.** A signal cut (§2.6) at the separator node x. The owner (the upstream part) solves x and carries an
  estimate of the loads (G of their direct resistors, plus the synthetic C_hf). The loads (the downstream part) read x as
  an input. Normally the residual r = i_load − i_est reaches the owner one sample late.
- **The relaxation.** Per window of 256 internal samples (one 64-frame block at the 5150's 4×):
  - Sweep 1 is the plain cut, with its lag.
  - Each later sweep restores the window's start state and replays the last sweep's r[n] at the same sample n. With
    the engine hook (`wrBind`), the owner injects `play[n]` and the sample's own r[n] is recorded.
  - The window has converged when the separator's waveform moves by at most tol·(1 + |v_x|) at every sample. tol is
    1e-8 below; the Newton tolerance is 1e-6.
- **Why it is exact.** At the fixed point (replayed r equal to recorded r at every sample), both parts satisfy the
  uncut group's discrete equations at every sample, whatever estimate the owner carries. The estimate's own current
  cancels inside r.
  - So the estimate (G, C_hf) is a free transmission (Robin) parameter. It only sets the convergence rate.
  - The hook exposes it (`wrSetRobin`, `wrSetRobinC`).
- **The probe** (`wr_probe.cpp`):
  - **Input:** the four real DIs as `bench --real` builds them (6 s each, 24 s, 18 000 windows), through the unit's 4×
    oversampler. Noise off, hum on. Both circuits start from the DC operating point with no settle, so the cut and uncut
    circuits start identically.
  - **Renders:** the uncut netlist, the cut netlist with its lag, and the relaxation.
  - **Reports:** sweeps per window and the host-rate nulls. Under `CE_PROFILE` it also gives each group's time per sweep
    per window.
  - **The cost model.** For c cores, the best assignment of groups to cores for each window. Each core's load is its
    groups' time summed over every sweep the window needed, with perfect overlap and no synchronisation cost. That is a
    lower bound on any real schedule.
  - **LTE.** Off on both sides; see "With the unit's LTE" below.

**Separators on the 5150** (its big group: 96 unknowns, 29 ports; the separators of §10.38). Sweeps to 1e-8 over 18 000
windows; nulls in dBFS at the host rate, against a signal at −9.4 dBFS rms. Tolerance-level changes to the uncut engine
null at −135 to −178 dB rms (§10.31, §10.38).

| Cut (owner → loads) | Ports | Robin estimate | Sweeps: mean | ≤ 4 sweeps | Most windows | Converged vs uncut (rms / peak) | Plain lag vs uncut (rms / peak) |
|---|---|---|---|---|---|---|---|
| k3b: V3B cathode follower → C30B → V3A grid | 14 + 15 | C_hf 10 pF | 13.0 | 9.7 % | 14–16 | −152.7 / −132.5 dB | −72.8 / −36.6 dB |
| fxpad: loop-send pad → C52 → V3B grid | 12 + 17 | C_hf 3.3 pF | 11.3 | 3.2 % | 12–13 | −160.1 / −135.0 dB | −65.7 / −31.9 dB |
| nb3: V5A coupling → tone stack | 12 + 17 | C_hf 1 pF | 12.8 | 0 % | 13–15 | −158.7 / −135.0 dB | −62.1 / −32.3 dB |
| n4b: V3A coupling → inverter grid | 16 + 13 | shipped (G only) | 17.4 | 0 % | 17–18 | −151.6 / −135.0 dB | −40.0 / −16.0 dB |
| n2b + k3b: three parts | 6 + 8 + 15 | shipped / 10 pF | 13.9 | 0 % | 14–16 | −152.8 / −131.4 dB | −42.0 / −6.1 dB |

- **The tolerance.** At tol 1e-6, k3b takes 7.5 sweeps on average (14 % of windows in 2), but its converged null
  falls to −126.2 dB rms and −91.1 dB peak. The separator has to converge well below the Newton tolerance for the
  output to stay at the engine's own level.
- **The same on the JCM800.** At its shipped `kcf` cut (cathode follower → tone stack; preamp 8 ports, power 11), 4
  sweeps on half of the windows and more than 20 on a third (0.3 s excerpt).

**Why it converges slowly.**
- **The coupling is not one-way over a block.** Within one sample it is weak: replaying inside a single sample
  (window 1) contracts by 0.02–0.04 per sweep at k3b and fxpad, once the estimate is right.
  - Over a window it is not. While playing, the window's error shrinks by only 0.3–0.5 decades per sweep.
  - The downstream grids conduct and cut off (V3A, V3B, the inverter, the power tubes), and the coupling caps charge
    through them. That memory carries the separator's error from sample to sample.
  - At −60 dB input, where everything is linear, the same cut converges in 3 sweeps.
- **No separator sits in front of a linear or unilateral load.**
  - k3b and fxpad feed V3A's and V3B's grids.
  - nb3 feeds the tone stack, and V5A's high-impedance plate makes that coupling strong: 0.74 per sample with the
    shipped estimate.
  - n4b feeds the inverter inside the NFB loop.
  - The inverter, power stage, NFB and OT form one block (§10.38), so there is no cut inside it.
- **Anderson acceleration** of the replayed waveform (depth 3 and 6) gains about 0.15 decades per sweep. The error is
  not in a few modes.

**Cost on the Ryzen 9 9900X** (each probe pinned to its own physical core, `CE_PROFILE` group clocks).
Per 64-frame block (256 samples, period 1333 µs), mean / p99:

| | Monolithic, 1 core | WR, 1 core (total CPU) | WR, ideal 2 cores | WR, ideal 2 cores, re-sweeps at 50 % | WR, ideal 3 cores |
|---|---|---|---|---|---|
| k3b | 1227 / 2189 µs (92 / 164 %) | 10 642 / 18 572 µs | 5598 / 9948 µs (420 / 746 %) | 3010 / 5283 µs | 5580 / 9903 µs |
| fxpad | 1208 / 1941 µs (91 / 146 %) | 11 488 / 28 585 µs | 6974 / 19 411 µs (523 / 1456 %) | 3795 / 10 981 µs | 6942 / 19 384 µs |
| n2b + k3b | 1199 / 1782 µs (90 / 134 %) | 13 751 / 30 307 µs | 7868 / 19 840 µs | 4216 / 11 288 µs | 7669 / 19 781 µs (575 / 1484 %) |

- **"Re-sweeps at 50 %"** halves every sweep after the first. A warm start from the last sweep's solution could not do
  better: the linear update is about half a sweep and is not skipped. In the probe a re-sweep costs what the first
  sweep does.
- **The Mac** (M4 Pro, unpinned, load 7–20 from other jobs) gives the same sweep counts and nulls. k3b: monolithic
  1662 / 5318 µs (125 / 399 %); WR on 1 core 10 335 / 23 267 µs; ideal 2 cores 5413 / 12 750 µs (406 / 956 %); ideal 3
  cores 5365 / 12 621 µs. The Mac's p99 carries the machine's load spikes on both sides.
- **The split itself is cheaper.** Per sweep, k3b's parts cost 51 + 332 + 420 µs against the monolithic 1227 µs:
  0.65×, because a 14- and a 15-port Newton solve are cheaper than one 29-port solve. The 2-core critical path is the
  15-port part, 420 µs per sweep. Break-even needs at most 2.9 sweeps with full re-sweeps, or 4.8 with half-cost ones.
  Only 7.3 % (k3b) and 0.03 % (fxpad) of windows converge in 3.
- **A third core buys nothing.** The 15–17-port part holding the inverter, power tubes and OT is the critical path in
  every split, and a second cut slows the convergence of both.

**With the unit's LTE (tol 16), the relaxation is not even exact.** Each part keeps its own substep control, so the
converged waveform belongs to a different discretisation.
- k3b at the unit's settings nulls at −48 dB rms against the uncut engine (0.3 s). The plain lag is the same −48 dB.
- Exactness would need the parts to substep together: the union of their decisions (the larger error estimate, a regime
  change on either side), with the separator exchanged at every substep. That costs more substeps on both sides. It
  was not built.

**Side findings.**
- **The signal-cut C_hf estimate is inflated by its regularisation.** The compiler adds 1e-9·cmax to every diagonal of
  the loads' capacitance matrix, to hold cap-free nodes (Engine.cpp, "Signal cuts: the owner carries an estimate").
  - In the 5150's power group, cmax is the OT core's flux node, a farad-scale capacitance. The regularisation (about
    1 nF) then swamps the grid's few picofarads behind a coupling cap.
  - Result: C_hf = 0.96 nF at both k3b and fxpad. With it, fxpad's per-sample contraction is 0.95 and its plain lag
    nulls at −13 dB; the table above uses corrected values.
  - Shipped cuts with a nonzero C_hf are listed by `chf_list.cpp`. On the JCM800's `kcf` (320 pF), 10 pF gives a plain
    lag of −69.3 dB against the uncut netlist, against −59.3 dB shipped (0.3 s, LTE off).
  - The estimate also keeps the lag stable at fs/2: fxpad with C_hf near 0 diverged. A fix therefore needs the autocut
    rule on each unit. The hold is now capped at 10 pF, and each affected cut was measured (§10.40).
- **The k3b split with a corrected estimate** costs about 0.55–0.65 of the monolithic group. Its plain lag nulls at
  −72.8 dB rms / −36.6 dB peak with LTE off, and at −48 dB with the unit's LTE (the substep pattern moves). It fails
  autocut's peak rule either way.

**Recommendation.** Do not pursue WR for the tube amps.
- Exact multi-core execution of one amp needs separators whose downstream part is linear or truly unilateral.
- A tube amp's grids, coupling caps and NFB make every one of the 5150's separators bidirectional and nonlinear over a
  block.
- The exact intra-amp parallelism that exists is the lanes' (§10.33): groups that do not read each other within a
  sample.
- With §10.38, this closes the exact-structure lever for the 5150-class amps. What is left changes the output (LTE
  tolerance and start, rates, or a lag cut under the autocut rule), so it is the user's call.

### 10.40 The signal-cut C_hf hold: capped at a stray 10 pF; applied where it measures better (2026-11-13)

**The bug** (§10.39 side finding, ENGINE_QUEUE "Cut load-capacitance estimate…"). A signal cut's owner carries C_hf, the
Schur complement of the loads' capacitor network seen from the cut node (§2.6). To keep that matrix regular, the compiler
held every node with 1e-9 × the load group's largest capacitance. In a group holding an output transformer, that is the
core's 1 F flux node, so the hold was 1 nF. Behind a coupling cap into a resistive node (a tone stack's treble cap) the
true C_hf is 0: the far node follows x. The hold turned it into the coupling cap in series with 1 nF.

| Cut | Old C_hf | New C_hf |
|---|---|---|
| 5F6A, Plexi, Plexi variac, 1959RR, JCM800, BE-100 `kcf` | 200–333 pF | 9.6–9.8 pF |
| AC30 `kcf` | 47.6 pF | 8.3 pF |
| AC15 `vt` | 1.76 nF | 48.9 pF |
| 1176 `ctl_c27` | 171 pF | 29 pF |
| 1176 `v30` (no-reservoir supply cut, §2.6) | 14.8 / 3.5 pF | 9.2 / 2.9 pF |

Every other cut in the library has the same C_hf to the last bit (the hold of a group without such states is ≤ 0.5 pF).

**The fix** (Engine.cpp, "Signal cuts: the owner carries an estimate"):
- The hold is now min(1e-9 × the group's largest capacitance, `kChfStray` = 10 pF), a wiring-scale stray capacitance.
- **Zero is not an option.** Two variants were measured first:
  - **The exact network value.** The hold follows only x's own capacitor network, which gives C_hf ≈ 0 (≤ 1e-18 F) on
    every `kcf`. With G alone, the lag runs away at Nyquist once the owner's impedance rises.
    - 5F6A dimed sine: −44.0 → −11.1 dB, with 355 PTC rescues.
    - AC15 dimed sine: −68.6 → −13.8 dB, with 2071 PTC rescues.
    - This is the instability §10.39 saw on the 5150's fxpad.
  - **A 10 pF floor on every load node.** It removes both instabilities (0 PTC). But it moves every floating-network cut in
    the library, for example the SSL EQ's `fo` and the Uni-Vibe's eight cuts from about 1 fF to 10 pF.
- The cap changes only the groups whose hold was above 10 pF.

**The per-cut opt-out.**
- A signal cut written `{"node": "kcf", "loads": [...], "chfHold": "group"}` keeps the uncapped hold
  (`Netlist::chfGroupHold`).
- `CE_CUT_CHF=0` restores the uncapped hold on every cut. `CE_CUT_CHF=1` applies the cap even to the marked cuts (developer
  A/B).
- `CE_CUTDIAG=1` prints each cut's C_hf and its hold.

**Validation.** Every affected cut was measured with `circuit_cli autocut <netlist without that cut> --table <id> --try
"<node>:<loads>" --full --gate test_circuit_engine --wav "Lead DI.wav"`, once with `CE_CUT_CHF=0` and once with
the fix. The run is the unit path with the live LTE. The SPICE figures are the shipped netlist's own gate, per case, in
both modes. Nulls are against the uncut netlist, in dB. Idle is the 10 s idle difference in dBFS and its growth.

| Cut | DI rms / peak: old → new | Dimed sine: old → new | Idle, Newton fail / PTC | Alias (autocut) | SPICE, per case | Applied |
|---|---|---|---|---|---|---|
| 5F6A `kcf` (62000) | −69.0 / −46.5 → −68.4 / −49.2 | −44.0 → −42.0 | −222 dBFS x1.05, 0 / 0 | −70.5 → −70.6 | all ≤ +0.001 pp; AC 5–10 kHz 0.147 → 0.118 dB | **no**: DI rms −0.6 dB |
| BE-100 `kcf` (151000) | −34.2 / −24.7 → −47.2 / −37.7 | −49.7 → −44.9 | −206 x1.00, 0 / 0 | −66.6 → −62.5 | hbe_drive spk 1.58 → 1.20 %, sat spk 1.13 → 0.94 %; dimed pb 1.50 → 1.57 %; AC 5–10 kHz 0.141 → 0.193 dB, hbe 0.233 → 0.285 dB | **no**: SPICE cases and alias worse |
| Plexi `kcf` (34000) | −36.0 / −23.2 → −41.7 / −25.6 | −23.2 → −31.4 | −176 x1.00, 0 / 0 | −64.8 → −65.8 | dimed spk 0.387 → 0.401 %; AC 5–10 kHz 0.019 → 0.071 dB | **no**: SPICE worse |
| Plexi variac `kcf` (92000) | −35.4 / −22.1 → −41.5 / −28.2 | −22.8 → −29.2 | −176 x1.00, 0 / 0 | −65.8 → −67.9 | AC 5–10 kHz 0.018 → 0.069 dB; the rest ± 0.003 | **no**: SPICE AC worse |
| 1959RR `kcf` (91000) | −41.4 / −25.5 → −41.7 / −24.1 | −24.7 → −29.5 | −151 x1.01, 0 / 0 | −74.3 → −71.5 (`circuit_cli alias`: the same; uncut −74.9) | dimed spk 2.06 → 0.89 %, pa 2.40 → 1.06 %, pb 2.56 → 0.98 %; AC 0.024 → 0.011 dB | **no**: alias −2.8 dB, DI peak −1.4 dB |
| JCM800 `kcf` (56000) | −35.1 / −23.8 → −43.3 / −36.4 | −35.5 → −31.1 | −202 x1.00, 0 / 0 | −58.2 → −60.7 | dimed spk 0.909 → 0.983 %, pb 1.094 → 1.132 %; AC 0.056 → 0.044 dB, 5–10 kHz 0.089 → 0.050 dB | **no**: SPICE dimed worse |
| AC30 `kcf` (38000) | −49.8 / −33.8 → −45.5 / −30.2 | −59.7 → −56.6 | −144 x1.00, 0 / 0 | −64.3 → −64.0 | AC 0.061 → 0.043 dB, 5–10 kHz 0.084 → 0.051 dB; the rest ≤ +0.001 | **no**: DI −4.3 dB |
| AC15 `vt` (176000) | −99.6 / −66.5 → −104.3 / −66.6 | −68.6 → −75.7 (worst mode −33.8 → −54.0) | −225 x1.03 → −220 x1.04, 0 / 0 | −70.0 → −70.0 | unchanged (≤ 0.000) | **yes** |
| 1176 `ctl_c27` + `v30` (123000) | −60.6 / −57.0 → −60.6 / −57.0 (renders; the new against the old: −127.8 dB) | – | kicked idle over 240 s: flat at 2.8e-13 V (old 6.0e-13 V); 0 / 0 | −75.9 → −75.9 | unchanged (≤ 0.000) | **yes** (equal) |

- **The 1176's `ctl_c27`** is a delay cut that breaks a feedback loop, which autocut does not take. It was measured by
  rendering the DI at the unit's settings through three netlists: the netlist without `ctl_c27`, the old shipped one and
  the new shipped one.
- **The seven `kcf` cuts carry `"chfHold": "group"`.** Each got worse on at least one criterion. They render
  bit-identically to before.
- **The final test runs** (build-cutcap): test_circuit_unit, test_effect_handoffs and test_aliasing pass.
  test_circuit_engine fails only re201_play_ac (0.136 dB). That failure is known and predates this change (ENGINE_QUEUE,
  2026-11-12), and it is the same with `CE_CUT_CHF=0`.
- **test_aliasing** gives identical figures for every unit with the marks in place. With `CE_CUT_CHF=1` the
  JCM800 goes from −58.2 to −60.7 dB, and nothing else moves. No SPICE case of the AC15 or the 1176 moved, so no fixture
  was regenerated.

**What this says.**
- C_hf is a free transmission parameter of the lag (§10.39). Its best value depends on the signal, so no single value
  wins on every cut.
- The JCM800 hold scan shows this. The hold was set to 10 pF, 100 pF, 300 pF, 1 nF and 3 nF, so C_hf ran from about
  10 pF to 410 pF.
  - The DI null falls steadily from −43.3 to −31.7 dB.
  - The dimed null rises from −31.1 to −35.5 dB.
  - The dimed SPICE case is best at about 180–320 pF (spk 0.909 % old, 0.912 % at about 180 pF, 0.983 % at 10 pF).
- So the accidental 300 pF of the old hold sits near the dimed optimum of a Marshall-style cathode-follower `kcf`.
- A small estimate wins on the DI, on small-signal AC and in the 1959RR's and BE-100's hot cases.

**Open, for the user.** These marked cuts would be worth turning on. Their trade-offs are in the table.
- The 1959RR: its dimed SPICE error falls 2.1 → 0.9 %, against 2.8 dB more aliasing (still −71.5 dB).
- The BE-100: DI −13 dB, and the HBE / sat cases 0.2–0.4 pp better, against dimed pb +0.07 pp and +0.05 dB AC at
  5–10 kHz.
- The JCM800: DI −8 dB, against dimed +0.07 pp.

To turn one on, remove its `chfHold` mark. A per-cut C_hf value is the other option. About 180 pF passes every SPICE
case on the JCM800, with DI −40.7 dB. It is not built.

### 10.41 Rosenbrock (linearly implicit) integration with an embedded error estimate: measured, slower and no more accurate; not landed (2026-11-13)

**The question.** Replace each nonlinear DK group's trapezoidal step + Newton (+ LTE substeps) by a linearly implicit
Rosenbrock step: one device Jacobian, one factorisation and 2–3 linear solves per sample, no Newton iteration, an
embedded error estimate, and today's substeps as the fallback when the estimate is over tolerance. The quality rule is the
user's "no worse against the true circuit": the reference is ngspice at a fine step, not today's engine.

The engine is unchanged. The prototype is kept as `work/circuit-netlists/cpu-9900x/ros/rosenbrock-prototype.patch`
(Engine.cpp / Engine.hpp as of this date, behind `CE_ROSENBROCK=1`), with the reference scripts and the logs next to it.

**Method (the prototype).**
- **Scheme.** ROS2 (Verwer et al. 1999): two stages, order 2, L-stable, γ = 1 − 1/√2 (`CE_ROSGAMMA`; 1 + 1/√2 was also
  run), the order-1 embedded solution as the estimate.
- **Fit to DK.** A Rosenbrock stage on M·y' = f(y) solves (M/(γh) − J)·U = rhs. With the device Jacobian taken at the step
  start, that is exactly one solve of the group's companion network at a backward-Euler step γh. The trapezoidal runtime at
  2γh has that companion, so the prototype keeps one more runtime per group (the unused T/8 slot), refreshed lazily as the
  substep runtimes are. Each stage:
  - sets the history from the stage's state: x̂ = x[n] (stage 1), x[n] + (1/γ − 2)·U1 (stage 2);
  - takes the inputs at u[n] + γΔu and u[n+1] − γΔu (the ∂f/∂t terms, inputs linear over the step);
  - linearises the device currents about the stage point Z: i(Z) + Jd·(z − Z), Jd from the step start;
  - then runs one `linPorts`, one solve with I − K·Jd, and one `linOut`.
  - y[n+1] = y[n] + (3/(2γ))·U1 + (1/(2γ))·U2 is applied to every component: states, ports, exports and state branch
    voltages. The estimate (U1 + U2)/(2γ) is tested on the capacitor voltages, inductor currents, ports and exports
    against the LTE control's per-state tolerance × `CE_ROSTOL`.
- **Factorisation.** I − K·Jd is factorised once per sample over its live columns only (a column whose Jacobian moves no
  port by more than 1e-13 V/V is an identity column), as the Newton kernel's Woodbury step does.
- **The rest of a sample.**
  - The device sweep at the end point is the next step's linearisation.
  - A capacitor's current at t[n+1] is not part of a Rosenbrock step. The trapezoidal companion state and the cut terms
    take it from the BDF2 derivative of the states.
  - The fallback is `substepGroup` with the backward-Euler or BDF2 start, which reads only the state values.
- **Scope.** Every DK group except WDF, lazy-clamp, hysteresis, speaker, regenerative, ladder and slow groups. Noise was
  off throughout, because the prototype's stage runtime carries no noise.

**The reference: fine-step ngspice on real DIs** (scripts in `cpu-9900x/ros/scripts/`).
- **Material.** Excerpts of `Lead DI.wav` (from 2.0 s) and `Rock DI.wav` (from 0 s), mono,
  resampled to 192 kHz (polyphase), 5 ms fades, normalised to 0.35 FS peak (0.35 V on both amps). The rock excerpt starts
  with 0.15 s of silence, then a pick attack.
- **Start state.** `circuit_cli spice --rate 192000 --settle 450 --ic-settled` (`.tran … uic`) with the PWL input. The
  references are 0.5 s at maxstep T/32, plus JCM800 sines at T/16, run on a Linux build machine with ngspice 47.
  - **3 s at T/16 was not finished.** These runs slowed from about 1.1 s to 0.2 s of audio per hour past 2 s, and were
    stopped at 2.1–2.3 s (JCM800) and 1.6–1.8 s (5150) after 2.9 h. For long references, run 0.5 s windows in
    parallel; each window starts from the engine's state at its start (the same --ic method).
  - **Two corrections the method needs on direct-rectifier supplies (JCM800, 5150).** The mains settle of §3.10 ends
    mid-cycle (450 ms plus the 60 ms tail, 3.6 periods past the crest). The deck's SIN sources must therefore start at
    the engine's phase (`scripts/phase.py`: 306° and 126° here, not 90° and 270°). The deck's t = 0 is the state before
    the engine's sample 0, so engine sample i ↔ t = (i + 1)·T.
  - Without the phase correction, the plate nodes carry the 120 Hz ripple out of phase: 12 % NRMSE at the JCM800's power
    plate, all of it ripple. With it, the first samples agree to 1e-4 V.
  - The existing `"settled"` gate cases do not hit this: they use transformer-fed supplies, whose settle ends on the
    crest.
- **Controls.** The unit's defaults at a 48 kHz host. For the JCM800 these are the netlist defaults. For the 5150 they are
  channel 0.5, higain 0.5, p8 0 and bright 0, which `--table 134000` sets and the decks repeat.
- **Checking the reference.** The engine itself at 4× the rate (768 kHz) moves 2–5× closer to it (JCM800 Lead, speaker,
  audio band: 1.29 → 0.24 %). So the remaining error is the engine's 192 kHz discretisation, and the reference resolves
  it.

**Accuracy against the reference** (engine renders at the unit's 192 kHz, `--table`, live LTE, noise and hum off; NRMSE
full band / 20 Hz–20 kHz band-limited, %, first 0.5 s against maxstep T/32):

| Amp, DI, node | Today | ROS2, tol × 1 | ROS2, tol × 16 |
|---|---|---|---|
| JCM800 Lead, speaker | 1.954 / 1.289 | 2.046 / 1.445 | 2.084 / 1.297 |
| JCM800 Lead, V2A plate (preamp) | 0.201 / 0.193 | 0.330 / 0.349 | 0.204 / 0.189 |
| JCM800 Rock, speaker | 3.254 / 2.008 | 3.377 / 2.203 | 3.459 / 2.077 |
| JCM800 Rock, PI plate p4a | 6.256 / 4.373 | 6.255 / 4.356 | 6.310 / 4.346 |
| 5150 Lead, speaker | 2.015 / 0.489 | 1.784 / 0.369 | 1.793 / 0.497 |
| 5150 Lead, PI plate p4a | 0.318 / 0.270 | 0.358 / 0.318 | 0.329 / 0.287 |
| 5150 Rock, speaker | 3.692 / 1.122 | 3.191 / 0.809 | 3.347 / 1.094 |

- **Spectral error.** On the speaker, per band from 20 Hz to 20 kHz, the error relative to the reference moves by
  −2.6…+3.4 dB between methods. At 1.3–5 kHz ROS2 is worse on the JCM800: 3.4 dB (tol × 1) and 0.5 dB (tol × 16). On the
  5150 at tol × 1 it is 2.6 dB better; at tol × 16 it is within 0.5 dB of today.
- **Level.** Within 0.1 dB in every band up to 5 kHz. Above 5 kHz ROS2 is slightly closer: JCM800 Lead at 10–20 kHz is
  +0.33 dB today, +0.14 / +0.27 dB with ROS2.
- **Harmonics** (JCM800, 0.35 V sine, last 0.1 s, every harmonic above −80 dBc up to 20 kHz):

  | Tone | Today | ROS2, tol × 1 | ROS2, tol × 16 |
  |---|---|---|---|
  | 110 Hz, worst / rms deviation | 1.11 / 0.42 dB | 1.18 / 0.44 dB | 1.11 / 0.42 dB |
  | 1 kHz, worst / rms deviation | 2.32 / 0.85 dB | 2.15 / 0.76 dB | 2.49 / 0.82 dB |

  At 1 kHz the time-domain NRMSE is 3.57 % today, 3.62 % and 3.93 % with ROS2.
- **SPICE gate** (`test_circuit_engine`, everything else equal):
  - jcm_drive spk 0.263 → 0.295 % and jcm_dimed spk 0.909 → 1.067 % (pb 1.094 → 1.261 %);
  - p5150_dimed 0.073 → 0.048 %;
  - with the prototype on every eligible group in the library, the unit pp_mid, vcarms_small and busg_* cases and the
    aliasing gate on both Cry Babies, the Clyde McCoy and the 1176 fail. The prototype was not tuned beyond the two amps.
- **Aliasing** (test_aliasing figure, report-only on both amps): JCM800 −58.2 → −55.2 dB; 5150 −50.4 → −54.3 dB.
- **Verdict on accuracy.** ROS2 is not "equal or better" on the JCM800 at any tolerance: slightly worse on the speaker,
  the plates, the gate and aliasing. On the 5150 it is better on the speaker and worse on the inverter plate.

**CPU.**
- **9900X** (`taskset -c 4,6`; `bench --table --real`, 4 real + 4 driven DIs, per-block minimum of 3
  runs, noise and hum off; two interleaved rounds agree within 0.6 points). Real / driven are % of one core; p99 is the
  combined 64-frame block as % of the 1333 µs period:

  | | Today | ROS2, tol × 1 | ROS2, tol × 16 |
  |---|---|---|---|
  | JCM800 real / driven | 43.2 / 55.5 % | 92.2 / 88.2 % | 64.9 / 73.9 % |
  | JCM800 p99 block | 110 % | 128 % | 118 % |
  | 5150 real / driven | 105.9 / 109.6 % | 197.5 / 195.9 % | 166.3 / 166.3 % |
  | 5150 p99 block | 199 % | 294 % | 236 % |

- **M4 Pro, JCM800 retired instructions** (load-independent; real / driven DIs): today 8.42 / 10.93 G/s; ROS2 tol × 1
  18.30 / 17.55; tol × 4 14.79 / 15.74; tol × 16 12.98 / 14.84.

**Why it loses.**
1. **In DK form, Newton is already about one factorisation per sample at 192 kHz.**
   - The state-space part (the linear update H·w, Dp·w) runs once per step, however many port-space iterations follow.
   - The predictor puts the first iterate inside the quadratic basin: 1.5 iterations per solve on the JCM800's DI, many
     of them chord confirmations.
   - Loosening the Newton tolerance from 1e-6 to 1e-3 cuts the JCM800's iterations from 5.98 to 3.59 per sample, but its
     time only by 10 % (M4) to 17 % (9900X). A Rosenbrock step cannot go below one factorisation either.
   - What Rosenbrock adds is a second stage. That is a second linear update, a second device sweep and a second
     substitution.
   - The JCM800 power group's step (M4, `CE_PROFILE`): linear updates 294 ns, device sweeps 154 ns, W, its LU and the
     solves 750 ns. Today the whole sample costs 1166 ns, including its share of substeps.
   - On the 5150's 29-port group the step costs 12.2 µs against 6.1 µs. The stage runtime has no sparse linear update,
     and the live LU is about 20 ports.
2. **The embedded estimate is first order.**
   - It measures the error of the order-1 solution, about h·ω larger than the order-2 result's. Against the LTE control's
     per-state tolerance it rejects far more samples than the trapezoidal third-difference estimate does.
   - JCM800 power group acceptance: 59 / 85 / 97 % at tol × 1 / 4 / 16 (preamp 92 / 98 / 99.5 %, supply 99.3 %).
   - Fallback events: 1.07 / 0.67 / 0.39 per sample on the real DIs, against today's 0.25 LTE events.
   - Every rejection pays the ROS2 step and then the substeps. So the p99 block rises as well, although the events were
     the target.
   - An order-3 method with an order-2 estimate (ROS3P class) would fix the estimate, at three linear updates per step.
3. **At the events the step is not small.**
   - The substeps fire where 3–6 µs modes around the inverter and output plates cross a knee (CPU_THEORY R2.1): about
     one sample.
   - ROS2's error constant for a linear mode is half the trapezoid's (0.040 against 0.083), and it is L-stable. But
     freezing J across a knee adds an error that Newton does not have.
   - So the events still need substeps, and there they cost about as much as Newton's: 2.3–3.6 iterations per substep.

**What did come out of it (no code change; for the user's decision).**
- **The Newton tolerance is free under the new quality rule.**
  - JCM800 at `CE_TOL=1e-3` against the same references: speaker, plates and preamp NRMSE identical to 3–4 digits
    (Lead 1.954 / 1.289 %, Rock 3.254 / 2.009 %).
  - Aliasing −58.2 dB at both tolerances (1e-2: −58.1).
  - 5150: identical or 0.01 points better.
  - CPU: 9900X real 43.2 → 36.1 %, driven 55.5 → 45.9 %, p99 block 110 → 90 % of the period. M4 instructions −19 / −20 %.
  - 1e-2 starts to move the error, by 0.001–0.006 points.
  - Before any default changes, it needs the full gate, the regenerative / comparator groups (§3.1) and the pedals
    (§10.42 did this: 1e-3 is lossless on tube ports only and is now their default; every other port keeps 1e-6). Until
    then the units took `CompileOptions::newtonTol` 1e-6, the "quality first" audit value, which was measured against a
    1e-8 engine render, not against SPICE.
- **The LTE tolerance is not free.**
  - JCM800 Lead / Rock speaker NRMSE at lteTol 1 / 2 (live) / 4 / 8: 1.99 / 1.95 / 2.02 / 2.14 % and 3.31 / 3.25 /
    3.39 / 3.64 %.
  - The BDF2 start (`CE_LTESTART=bdf2`) is worse on the JCM800: 2.19 / 3.55 %. This confirms §10.16's choice.
- **The JCM800's largest true-circuit error is in the preamp and the phase inverter.** On the Rock DI, the inverter
  plate (p4a, power group) and the preamp's cathode follower (kcf) read 4.4 % and 3.7 % NRMSE in the audio band. At 4×
  the rate they read 1.2 % and 0.8 %.
  - The LTE control hardly moves them: with LTE off they read 4.9 % and 3.9 %.
  - That is the place to spend accuracy, not the integrator of the power group.
- **The references.** `cpu-9900x/ros/refs/*.npz` holds, for future "against the true circuit" work:
  - both amps on both DIs, 0.5 s at T/32, five nodes each;
  - the JCM800's 110 Hz and 1 kHz sines;
  - each with its 192 kHz input and the alignment note.

  `scripts/` rebuilds the decks: `prep.py`, `phase.py`, `mkdeck.py` and `run_ref.sh`, then `table.py`, `cmp.py` and
  `harm.py` compare. With them, a change that does not null against today's engine can still be judged on whether it is
  no worse against the truth.

### 10.42 Newton tolerance against the true circuit: 1e-3 on tube ports, 1e-6 everywhere else (2026-11-13)

**The question.** §10.41 found that loosening `CompileOptions::newtonTol` from 1e-6 to 1e-3 left the JCM800's and the
5150's error against fine-step ngspice unchanged, for 10–20 % less CPU. Is the loosest tolerance that is lossless against
the true circuit the same for every unit? The 1e-6 had been chosen by nulling the engine against a 1e-8 engine render,
not against SPICE.

**Answer: no, the tolerance has to be per device class.**
- **Tubes (triode, pentode, vacuum diode).** 1e-3 is lossless against the truth on every tube unit checked.
- **Junction, op-amp, OTA, CMOS, optical, rms and VCA ports.** These lose at 1e-3, and the rms detectors already lose at 1e-5.

The engine now converges each port to its own class's tolerance:
- Tube ports use `kTubeNewtonTol` = 1e-3.
- Every other port uses `kDefaultNewtonTol` = 1e-6, as before.
- A group of only tube ports runs at 1e-3 throughout, including its predictor gate and small-signal box.
- A mixed group (tubes beside a rectifier diode, a zener, a magnetising branch or a CdS cell) carries per-port tolerances, `GroupRt::ptol`. Its group-level tests (predictor, PTC, generic Newton, small-signal path, the lazy-clamp reduced kernel) keep the tight one.
- In such a group, a diode or zener junction also converges on its current, |g·Δv| ≤ 1 pA + tol·|i|. This is SPICE's abstol test, until now used only for floating zeners.
  - Without it, a junction that carries no current pins the tube port it shares a voltage with to 1e-6. The 5150's flyback clamps CR200 / CR201, reverse biased across the plates at hundreds of volts, did that: the saving was 6 % instead of 15 %.
  - A conducting junction still meets the voltage test first, because tol·|i| / g is tol·nVt there.
- DC operating points are solved at 1e-6 on every port, as before. With the junction rule, the BE-100's zener mid-node (zm) moved 4 mV off ngspice in `be100_dc`.
- Circuits without tubes are **byte-identical** to the 1e-6 engine. Checked by the `bench --real` output hash on all 40 solid-state units, and by a new test.

**Why the classes differ.** A Newton step accepted at tol·(1 + |v|) leaves an error of O(step²) after a full Newton step, but O(ρ·step) after a chord or lazy step.
- **Tube ports.** The laws bend on a scale of volts (μ·Vg + Vp ~ 10–100 V), so even a linear-convergence error of 1e-3·(1 + |v|) is far below the discretisation error.
- **Junction ports.** The scale is n·Vt ≈ 26 mV, so 1 mV is 4 % of the current.
- **rms detectors and log-domain VCAs.** The control ports sit at microvolts with mV/dB gains (THAT 2180 −6.1 mV/dB). The absolute floor tol·1 V is the error itself, and the detector integrates it.

**Why not a single loose tolerance.** Uniform `CE_TOL=1e-3` against SPICE (gate fixtures: ngspice at T/8; NRMSE in %):

| Case (solid state) | 1e-6 | 1e-4 | 1e-3 |
|---|---|---|---|
| d160_hot rms_out / x5 | 0.012 / 0.182 | 0.012 / 0.182 | 1.213 / 1.404 |
| vcarms_small ctl (fails gate at 1 %) | 0.139 | 2.227 | 2.227 |
| rms tone-burst attack vs law (fails) | 0.034 dB | 0.769 dB | 1.321 dB |
| jh1_od nb / gt2_drive o2b | 0.021 / 0.174 | 0.021 / 0.174 | 0.081 / 0.352 |
| be100_sat ze (a zener in a tube amp) | 0.362 | 0.362 | 0.741 |
| Small Stone LFO (ngspice 4.9492 Hz) | 4.9491 Hz | 4.9491 Hz | 4.9482 Hz |
| zener 1N961B at Izt | 10.0000 V | 10.0000 V | 10.0003 V (fails) |

- **Aliasing gate.** MT-2 −48.7 dB and JH-1 −56.0 dB at 1e-3 (gate −60 dB); everything passes at 1e-4 and 1e-5.
- **New fine-step references** (`work/circuit-netlists/cpu-9900x/tol/`). NRMSE full / audio band, %:
  - MT-2 out: 0.862 / 0.613 (1e-6), 0.864 / 0.614 (1e-4), 0.934 / 0.700 (1e-3). Clip node: 0.204, 0.209, 0.475 audio.
  - Fuzz Face Si out: 0.026 audio (1e-6 and 1e-4), 0.027 (1e-3).
- **Self-nulls** (`bench --real`, 24 s of real and of driven DI, against the same unit at 1e-9).
  - At 1e-3, solid-state units read as high as −25 dB (Electric Mistress), −34 (Small Stone), −36 (CE-1), −44 (Bi-Phase), −47 (Uni-Vibe) and −48 (MT-2). Those are the LFO phase drift.
  - At 1e-6 they are −96 to −239 dB.

**Tube units at 1e-3 on tube ports (the new default) against the truth.**
- **Fine-step references** (0.5 s, maxstep T/32, ngspice 47; JCM800 / 5150 from §10.41). NRMSE full / audio band, %:

  | Excerpt, node | 1e-6 (old) | tube 1e-4 | **tube 1e-3** | tube 1e-2 |
  |---|---|---|---|---|
  | JCM800 Lead, speaker | 1.9541 / 1.2891 | 1.9541 / 1.2891 | 1.9539 / 1.2890 | 1.9555 / 1.2903 |
  | JCM800 Rock, speaker | 3.2540 / 2.0082 | 3.2540 / 2.0082 | 3.2541 / 2.0086 | 3.2580 / 2.0116 |
  | JCM800 Rock, PI plate p4a | 6.2559 / 4.3731 | same | 6.2558 / 4.3729 | 6.2604 / 4.3765 |
  | JCM800 Rock, cathode follower kcf | 5.2549 / 3.7016 | same | 5.2549 / 3.7016 | 5.2607 / 3.7059 |
  | JCM800 1 kHz sine, speaker | 3.5435 / 2.1923 | same | 3.5436 / 2.1923 | 3.5358 / 2.1871 |
  | 5150 Lead, speaker | 2.0149 / 0.4893 | same | 2.0149 / 0.4892 | 2.0245 / 0.4857 |
  | 5150 Rock, speaker | 3.6920 / 1.1219 | 3.6921 / 1.1219 | 3.6922 / 1.1216 | 3.7095 / 1.1186 |
  | Twin, driven Clean Strumming, reverb 0, speaker (1 s, ngspice 42) | 5.9622 / 0.4546 | 5.9621 / 0.4545 | 5.9634 / 0.4550 | 5.9637 / 0.4566 |
  | LA-2A, Rock 0–1 s, out / T4B light (48 kHz) | 0.5103 / 0.5098, light 2.3098 | same | same | same |
  | AC15, Vibravox running up from its kick (footswitch on, Vib-Trem input, Rock 0–1.2 s): speaker / oscillator plate p6 | 0.3200 / 0.3200, p6 0.0014 / 0.0092 | same | same | 0.3200 / 0.3200, p6 0.0009 / 0.0040 |
  | Butler Tube Driver, Lead 0.3 s (768 kHz, ngspice 42): out | 0.0033 / 0.0022 | same | same | 0.0035 / 0.0023 |
- **The SPICE gate** (`test_circuit_engine`, T/8 fixtures).
  - 14 of 614 node figures move, all in the third decimal.
  - Up: rr_dimed spk 2.058 → 2.064, pa 2.403 → 2.409, pb 2.564 → 2.572 (+0.3 % of the error); +0.001 on jcm_dimed spk (0.909 → 0.910), twin_dimed spk / pa / pb, uber_dimed spk / pa, or120_dimed spk and ab_hot p1 (0.002 → 0.003).
  - Down: f670_hot p1071 0.646 → 0.645, f670_set_hot gy1 2.446 → 2.445, pent_small g2 0.040 → 0.037.
  - At tube 1e-4 (before the junction rule), only f670_small pu1 moved (0.009 → 0.010).
  - Every other printed figure is unchanged: every solid-state case, every DC and AC case, the LFO frequencies, the rms, VCA, zener and 1176 device checks.
  - The one failure, `re201_play_ac` 0.136 dB, fails identically at `CE_TOL=1e-6`. It predates this change.
  - test_circuit_unit, test_aliasing and test_effect_handoffs pass. The lanes check (JC-120, Fairchild 670, Dimension D) stays byte-identical to sequential.
- **Tube oscillators.** The AC15 Vibravox grows and swings identically at every tube tolerance from 1e-6 to 1e-2, and as in ngspice: plate peak-to-peak per 0.2 s 1.665 / 1.985 / 4.119 / 14.4 / 35.46 / 125.4 V, last half-periods 83.801 / 82.219 / 79.659 ms.
- **Comparator, regenerative and sidechain classes.** These are all solid state, so their ports keep 1e-6 and their output is byte-identical:
  - Schmitt and LFO units: Uni-Vibe, Lune, Bi-Phase, Phase 90, Small Stone, and the CE-1, Memory Man, Dimension D and JC-120 BBD clocks;
  - 1176, dbx 160, SSL dynamics and bus compressor, Dyna Comp.
  - The tube sidechains (Fairchild 670, LA-2A) are mixed groups. Their junction and CdS ports stay at 1e-6.
  - The Fairchild's SPICE cases are unchanged (f670_attack, f670_ms, f670_small) or 0.001 better (f670_hot, f670_set_hot). A 0.5 s Fairchild deck was not feasible: ngspice reached 2 ms in two hours.
  - The Fairchild's lanes stay byte-identical to sequential. So do the JC-120's and the Dimension D's.
- **Aliasing.** The `circuit_cli alias` figure of all 27 tube units moves by at most 0.4 dB: Twin −83.4 → −83.0, Deluxe −62.7 → −62.6, AC30 / OR120 / Leslie ±0.1, the rest identical. The aliasing gate passes.
- **Self-nulls** (uniform tolerance, against 1e-9). Tube units read −71 to −144 dB at 1e-3, with two exceptions:
  - The Twin's and the Deluxe's driven material read −47…−54 dB. That is local divergence where the speaker node rings near Nyquist (an AB763 power-stage mode): any perturbation, even 1e-4 (−48 / −65 dB), moves it.
  - Against the truth it is neutral: Twin clean-driven speaker audio band 0.4546 → 0.4550 % (row above).
  - The SVT and both Rectifiers read about −80 dB even at 1e-6 against 1e-9. That is their own sensitivity, independent of the tolerance.

**The criterion.** "Lossless" here means three things:
- every truth figure is unchanged to its third significant digit;
- moves in both directions are inside that;
- every gate verdict and every aliasing figure is unchanged.

Tube 1e-3 meets it. The largest move is the dimed 1959RR's speaker, 2.058 → 2.064 % (+0.3 % of its error). Every fine-step reference above is unchanged to four digits.

Tube 1e-2 does not: JCM800 Rock speaker +0.003 points audio band, 5150 Rock speaker +0.017 points full band.

Tube 1e-4 is the strictly-unchanged choice, at three decimals everywhere. It buys about three quarters of the saving (table below). Set it per unit with `"newton": {"tube": 1e-4}`, or globally with `CE_TOLTUBE=1e-4`.

**CPU.**
- **M4 Pro, retired instructions** (load-independent; `bench --table --real`, noise and hum off, 24 s each of real and driven DI). G instructions/s, real / driven:

  | Unit | real: 1e-6 → tube 1e-4 → **tube 1e-3** | driven: 1e-6 → tube 1e-4 → **tube 1e-3** | Newton it/sample (real) |
  |---|---|---|---|
  | 34000 marshall-1959-plexi | 17.10 → 15.09 → 14.23 (-16.8 %) | 18.04 → 15.98 → 14.99 (-16.9 %) | 11.36 → 7.70 |
  | 56000 marshall-jcm800-2203 | 8.47 → 7.55 → 7.09 (-16.3 %) | 11.00 → 9.76 → 9.13 (-17.0 %) | 7.46 → 5.56 |
  | 162000 bogner-ecstasy-101b-162000 | 15.58 → 13.96 → 13.05 (-16.2 %) | 20.77 → 18.20 → 16.95 (-18.4 %) | 2.36 → 1.53 |
  | 92000 marshall-1959-plexi-variac | 17.11 → 15.16 → 14.36 (-16.1 %) | 17.95 → 15.94 → 14.99 (-16.5 %) | 11.20 → 7.68 |
  | 151000 friedman-be100-151000 | 15.23 → 13.46 → 12.85 (-15.6 %) | 16.06 → 14.17 → 13.59 (-15.4 %) | 7.72 → 6.10 |
  | 134000 peavey-5150-134000 | 21.42 → 19.39 → 18.11 (-15.5 %) | 22.14 → 20.08 → 18.82 (-15.0 %) | 5.78 → 4.62 |
  | 27000 fender-6g15-27000 | 2.04 → 1.81 → 1.73 (-15.3 %) | 2.69 → 2.39 → 2.18 (-19.0 %) | 3.86 → 3.10 |
  | 79000 fender-5e3-tweed-deluxe | 2.58 → 2.27 → 2.20 (-14.7 %) | 3.64 → 3.15 → 2.91 (-20.1 %) | 3.76 → 2.09 |
  | 62000 fender-5f6a-bassman | 6.14 → 5.51 → 5.27 (-14.2 %) | 7.50 → 6.50 → 6.15 (-17.9 %) | 5.11 → 3.27 |
  | 91000 marshall-1959rr-91000 | 16.89 → 15.11 → 14.52 (-14.0 %) | 17.41 → 15.68 → 14.97 (-14.0 %) | 10.74 → 7.35 |
  | 80000 orange-or120-1972-80000 | 6.09 → 5.47 → 5.24 (-13.9 %) | 8.40 → 7.46 → 6.93 (-17.5 %) | 5.27 → 4.54 |
  | 176000 vox-ac15-handwired | 8.50 → 7.72 → 7.36 (-13.4 %) | 10.05 → 8.92 → 8.45 (-15.9 %) | 5.09 → 3.37 |
  | 38000 vox-ac30-tb | 14.97 → 13.46 → 13.11 (-12.4 %) | 16.99 → 14.95 → 14.27 (-16.0 %) | 10.24 → 6.12 |
  | 163000 soldano-slo100-mk2-163000 | 12.18 → 11.15 → 10.70 (-12.1 %) | 13.35 → 12.10 → 11.54 (-13.6 %) | 3.59 → 2.82 |
  | 16000 mesa-dual-rectifier-16000 | 11.63 → 10.65 → 10.30 (-11.4 %) | 13.13 → 11.85 → 11.32 (-13.8 %) | 4.89 → 3.97 |
  | 96000 mesa-dual-rectifier-revc-96000 | 19.35 → 17.73 → 17.22 (-11.0 %) | 20.91 → 18.96 → 18.32 (-12.4 %) | 3.95 → 3.20 |
  | 111000 butler-tube-driver | 7.03 → 6.43 → 6.31 (-10.3 %) | 7.60 → 7.07 → 6.87 (-9.6 %) | 1.96 → 1.34 |
  | 172000 fender-ab763-deluxe-reverb | 8.06 → 7.40 → 7.26 (-9.9 %) | 10.81 → 9.63 → 9.11 (-15.8 %) | 6.79 → 4.58 |
  | 152000 supro-1624t-dual-tone | 6.21 → 5.67 → 5.60 (-9.9 %) | 9.38 → 8.26 → 7.63 (-18.7 %) | 3.49 → 2.06 |
  | 81000 bogner-uberschall-81000 | 19.26 → 17.80 → 17.58 (-8.7 %) | 20.68 → 18.96 → 18.60 (-10.0 %) | 4.40 → 3.78 |
  | 87000 hiwatt-dr103 | 6.75 → 6.26 → 6.23 (-7.6 %) | 8.82 → 7.73 → 7.16 (-18.8 %) | 3.37 → 2.86 |
  | 122000 soldano-slo100-122000 | 13.52 → 12.72 → 12.68 (-6.2 %) | 15.25 → 14.45 → 14.41 (-5.6 %) | 6.28 → 5.72 |
  | 30000 fender-ab763-twin-reverb | 7.34 → 6.94 → 6.89 (-6.1 %) | 8.88 → 8.01 → 7.64 (-14.0 %) | 6.09 → 5.08 |
  | 124000 teletronix-la2a | 1.17 → 1.11 → 1.10 (-5.9 %) | 1.26 → 1.16 → 1.14 (-9.9 %) | 4.29 → 3.54 |
  | 54000 ampeg-svt-1969 | 12.10 → 11.68 → 11.66 (-3.6 %) | 14.21 → 13.38 → 13.21 (-7.0 %) | 4.88 → 4.40 |
  | 31000 leslie-122-31000 | 4.19 → 4.18 → 4.18 (-0.2 %) | 4.74 → 4.34 → 4.24 (-10.6 %) | 2.50 → 2.46 |
  | 24000 fairchild-670-24000 | 13.20 → 13.15 → 13.36 (+1.2 %) | 14.41 → 13.90 → 14.03 (-2.6 %) | 6.05 → 5.80 |
  | **All 27 tube units** | 294.1 → 268.8 → 260.2 (−11.5 %) | 336.1 → 303.0 → 289.5 (−13.9 %) | |

  The 40 solid-state units are byte-identical: same output hash and same instructions.
- **9900X** (`taskset -c 4,6`, `bench --table --real`, 3 runs per-block minimum, modes interleaved, noise and hum off). % of one core:
  | Amp | 1e-6 (old): real / driven, p99 | tube 1e-4 | **tube 1e-3 (default)** |
  |---|---|---|---|
  | JCM800 | 44.4 / 56.3 %, p99 110 / 111 % | 40.3 / 51.0 %, 100 / 101 % | **37.8 / 47.7 %, 93 / 93 %** |
  | 5150 (two rounds, within 0.6 points) | 107.1 / 110.4 %, 197 / 204 % | 96.8 / 100.4 %, 181 / 189 % | **90.7 / 94.4 %, 167 / 176 %** |
  | 1959 Plexi | 85.5 / 90.3 %, 107 / 122 % | 77.1 / 81.7 %, 96 / 108 % | **72.6 / 76.8 %, 87 / 99 %** |

  - For comparison, §10.41's uniform 1e-3 measured JCM800 36.1 / 45.9 % and 5150 85 / 88 %.
  - The per-class default keeps about 90 % of that saving on the JCM800 and about 80 % on the 5150. The rest is the supply group's diodes, and the non-tube ports of mixed groups, which keep 1e-6.
  - p99 is the per-material 64-frame block as % of the 1333 µs period.
  - JCM800 and Plexi are single rounds; an earlier two-round run before the DC / junction refinements agreed within 0.7 points.

**The other finding: where the JCM800's true-circuit error is, and what rate would cost** (measurement only, nothing changed).
- The inverter (p4a) and the preamp cathode follower (kcf) are the largest true-circuit errors: Rock DI audio band 4.37 % and 3.70 %.
- The PI shares the power group with the output tubes, OT and speaker, so it cannot be raised alone. Raising it means running groups 1 and 2, the whole amp but the supply, faster.
- Against the T/32 references, the whole engine at 2× (384 kHz) reads 2.33 / 1.69 % and at 4× (768 kHz) 1.18 / 0.79 %. The speaker reads 2.01 → 0.86 → 0.46 % (Rock) and 1.29 → 0.52 → 0.24 % (Lead).
- What the tolerance saving buys of that:

  | JCM800, default tolerance | 9900X real / driven % of one core | p99 64-frame block, real / driven | M4 G instructions/s, real / driven | PI / CF error, Rock audio band |
  |---|---|---|---|---|
  | unit rate (192 kHz) | 37.0 / 47.2 | 91 / 93 % | 7.08 / 9.10 | 4.37 / 3.70 % |
  | whole engine at 2× (384 kHz) | 60.9 / 76.3 | 135 / 146 % | 11.47 / 14.28 | 2.33 / 1.69 % |
  | whole engine at 4× (768 kHz) | 111.2 / 123.9 | 174 / 201 % | 20.77 / 23.15 | 1.18 / 0.79 % |
  | groups 1+2 at 2× (`CE_LEVELS=V3A=1,V2B=1`, supply at 192 kHz) | – | – | 9.62 / 9.97 | not comparable (converter latency differs per node) |
  | groups 1+2 at 4× | – | – | 17.84 / 18.36 | – |

  - The tolerance saves 6.6 / 8.6 points of a 9900X core on the JCM800 (44.4 → 37.8 real, 56.3 → 47.7 driven).
  - That pays for about 28 / 30 % of the 2× step (+23.9 / +29.1 points) and 9 / 11 % of the 4× step.
  - Spending accuracy there means raising the whole amp, at a net +17 / +20 points (2×) over the 1e-6 engine. It is a quality opportunity for the user's decision, not something the tolerance alone pays for.

**Code.**
- `kDefaultNewtonTol` 1e-6 and `kTubeNewtonTol` 1e-3 (Engine.hpp).
- `CompileOptions::newtonTol` / `tubeNewtonTol`: 0 is the default; an explicit `newtonTol` applies to tube ports too unless `tubeNewtonTol` is also set, so the tests' 1e-9 cases are unchanged.
- Netlist `"newton": {"tol": x, "tube": y}`. No library netlist sets it. (§10.55 adds `"groups": {"<part>": x}`, a tolerance for one group's non-tube ports, which five netlists set.)
- `GroupRt::tol` / `ptol`; `newtonKernel` / `activeKernel` take the per-port array.
- `describeGroups` prints each group's tolerance (`tol 0.001`, or `tol 1e-06 / tube ports 0.001 (10 of 11)`).
- Test: `testNewtonTolerance`.
- `circuit_cli spice --table` now sets the unit's controls, as render and bench do.
- **A/B.** `CE_TOL=x` sets every port (`CE_TOL=1e-6` is the engine before this section, byte for byte); `CE_TOLTUBE=x` sets the tube ports only; `CE_NOTUBEJCT=1` turns the junction current test in tube groups off.
- **References and scripts.** `work/circuit-netlists/cpu-9900x/tol/`.
  - `refs/*.npz` hold t, v (one column per node), nodes, input (the engine input per sample) and an alignment note (row i is engine sample i at t = (i+1)·T).
  - `scripts/` rebuild and compare them:
    - `mk.sh` with `prep2.py`, `phase2.py` and `mkdeck2.py` builds the decks. Circuits without mains need `NOUIC=1`, a `.op` start; uic with `.ic` does not converge on op-amp pedals.
    - `runref.sh` runs a deck on a build machine.
    - `rend.sh` renders the engine per mode.
    - `cmp2.py` and `lfo.py` compare.
    - `bench9.sh` is the 9900X CPU bench.
  - `logs/` holds the gate tables, the uniform sweep and both CPU sweeps.

### 10.43 Cut audit: every approximate cut against the true circuit; 15 units lose lossy cuts, 6 keep them for real time (2026-11-13)

**The question.** §10.40 showed shipped `kcf` cuts nulling at −22 to −46 dB against the uncut netlist. The user's rule is
"no quality loss against the TRUE circuit". So every entry of every bound netlist's `cuts` was measured: supply chains,
signal cuts and `delay` cuts, 353 entries in 52 netlists. The netlists are the only thing that changed; the engine did not.

**Method.** For each entry, the netlist was measured with that entry removed. When the netlist did not compile with that
supply node removed alone, the node was removed together with the rest of its chain, before it or after it (`-x+pre` /
`-x+post`). Every netlist was also measured with all supply cuts removed, and with every cut removed.
- **Null.** `bench --real` material through the unit path: four DIs plus their fuzz-driven copies, 24 s each, noise off.
  - Each render starts after **10 s of silence**. Without it the null measures the settle, not the cut. The BE-100's
    bias rail `b2` sits 6 V (cut) or 30 V (uncut) away from its settled −36.9 V after the unit's 450 ms settle, and takes
    about 4 s to arrive. That alone nulls at −8 dB in the first half-second. This is a settle finding for its owner, not
    a cut effect.
- **Truth.**
  - Every SPICE gate case of the netlist with and without the entry (`CE_GATE_FILE` / `CE_GATE_LIBDIR`). The decision
    reads the output node of each case and the AC cases; internal nodes are reported only.
  - Fine-step ngspice renders of a real DI. 2 s of the lead DI excerpt (§10.41's input) at maxstep T/16, from the engine's
    settled state, with the mains phase taken from the engine: JCM800, Plexi, BE-100, 1959RR, SLO-100, AC30 and 5F6A.
    Also 1 s at T/16 for the DS-1, HM-2, Big Muff and LA-2A, plus §10.42's MT-2 and LA-2A references (rechecked on the current engine). The MT-2, Bluesbreaker and 1176 decks do not run from the
    engine's `.ic` state.
  - The Plexi and AC30 renders are compared at a 4-sample lag, which is the latency of the VPA 16× rate converter.
- **Stability.** A kick followed by 20 s of silence (the difference must not grow), a dimed sine (Newton failures and
  homotopies), and `circuit_cli alias`.
- **CPU.** Mac retired instructions, measured on the engine before §10.42. Then `bench --real` on the 9900X (3 reps, two
  pinned cores), re-run on the current tree *with* §10.42's tolerances. Every 9900X figure below is the current tree.

**Rule.**
- **(A)** The null is ≤ −100 dB rms, or no output case or AC case is closer to ngspice uncut by more than 0.01 pp /
  0.01 dB.
- **(B)** Uncut is closer to the truth, and it is stable.
- **(C)** The netlist cannot drop the cut: it does not compile, the DC point fails, it hits the port limit, Newton
  trouble, or removing it breaks the gate.
- **C (real time).** Removing the cut is (B) on accuracy, but the uncut unit needs more than one 9900X core (mean, real or driven
  material). Every block would then overrun on any machine, which the user has already heard as "a broken amp" (§10.28). These
  cuts are kept, and the trade is stated below; flipping one is one `dropcuts` edit.

**Removed (B).** The spk/out NRMSE is against ngspice. "DI" is the fine-step render, in the 20 Hz–20 kHz band; the rest
are gate cases. CPU is the 9900X mean on real / driven material, and the combined p99 of a 64-frame
block (the period is 1333 µs). Mac instruction deltas are in `results/macinstr_*.log`.

| Unit | Cuts removed | Null real / driven (rms dB) | Truth: shipped → uncut | CPU |
|---|---|---|---|---|
| JCM800 (56000/101000) | `kcf` | −33.5 / −29.1 | DI 1.248 → 0.689 %; §10.41's 0.5 s refs: lead 1.352 → 0.665, rock 2.212 → 1.034; jcm_dimed −0.54 pp. Alias −58.2 → −52.4 dB (not gated) | 40.1 / 49.8 → 47.6 / 57.5 %, p99 1260 → 1569 µs |
| 5F6A (62000) | `kcf` | −60.3 / −51.8 | DI 0.084 → 0.074 %; bassman_drive −0.049 pp | 30.8 / 34.9 → 33.0 / 39.7 %, p99 747 → 1035 µs |
| Twin (30000) | `n1`, `vib`, `dry` | −55.9 / −49.3 | twin_dimed spk −0.243 pp, twin_ac 5–10 kHz −0.138 dB | 41.2 / 45.1 → 44.8 / 55.5 %, p99 763 → 1226 µs |
| Deluxe (172000) | `n1`, `vib`, `dry` | −44.4 / −38.0 | deluxe_clean spk 0.202 → 0.003 %, deluxe_drive 0.146 → 0.033 %; deluxe_ac 5–10 kHz 0.104 → 0.257 dB (worse, at the gate's 96 kHz) | 43.7 / 53.3 → 51.1 / 78.3 %, p99 1233 → 2506 µs |
| JC-120 (49000) | `v12` | −68.2 / −71.3 | clean spkL / spkR 0.258 / 0.226 → 0.211 / 0.181 %, chorus spkL 0.299 → 0.268 % | 98.0 / 123.0 → 97.9 / 122.7 % (lanes kept) |
| LA-2A (124000) | `t4b_light` (delay), `sc_tap` | −50.8 / −50.5 | DI 0.306 → 0.023 %; la2a_small 0.246 → 0.005 %, la2a_hot 0.085 → 0.037 % | 7.2 / 7.4 → 8.8 / 9.4 % |
| 1176 (123000) | `pre_ac`, `ctl_c27` (delay) | −55.2 / −60.1 | u1176_hot out 0.030 → 0.001 %, the ctl_c27 node 1.913 → 0.304 % | 12.9 / 13.5 → 14.4 / 15.2 % |
| Dyna Comp (48000) | `env` (delay) | −77.8 / −79.0 | dyna_hot out 0.085 → 0.004 % (the env node itself 0.87 → 1.48 %) | 42.0 / 45.9 → 47.4 / 53.4 % |
| Dimension D (142000) | `XR.q101e`, `XL.q101e` | −65.4 / −65.5 | dimd_small XR.bin −0.049 pp | 20.8 / 22.9 → 21.8 / 23.5 % |
| RE-201 (78000) | `n18`, `ta` | −46.7 / −50.0 | re201_play 0.937 → 0.037 %, re201_play_hot 0.659 → 0.020 %, re201_play_ac 0.136 → 0.075 dB (the known failing case now passes). Alias −64 → −59 dB (tape, not gated) | 17.4 / 19.6 → 21.3 / 29.5 %, p99 326 → 740 µs |
| DS-1 (17000) | `vb` | −56.9 / −49.1 | DI 0.178 → 0.046 %; ds1_hot 0.878 → 0.322 %. Alias −75.5 → −70.4 dB | 17.3 / 19.2 → 17.3 / 20.7 % |
| HM-2 (165000) | `vb` | −70.1 / −53.3 | DI 0.082 → 0.074 %; hm2_drive 1.480 → 0.670 %. Homotopies on the driven DI 1103 → 107 (defaults), 1064 → 122 (distortion 1) | 31.9 / 34.0 → 44.6 / 56.6 %, p99 584 → 1408 µs |
| Big Muff (43000) | `n12` | −66.2 / −70.8 | DI 0.074 → 0.037 %; muff_small 0.610 → 0.391 %, muff_hot 0.277 → 0.399 % (worse) | 12.3 / 12.6 → 12.7 / 13.4 % |
| 6G15 (27000) | `bpa` | −69.2 / −63.0 | g615_clean −0.033 pp | 10.6 / 12.9 → 10.6 / 12.9 % |
| BE-100 (151000) | `kcf`, `c9o` | −37.1 / −32.7 | DI 1.164 → 0.063 %; be100_hbe_drive 1.577 → 0.264 %, AC 5–10 kHz +0.08 dB. Alias −66.6 → −61.5 dB | 70.0 / 74.3 → 87.3 / 94.7 %, p99 1254 → 1822 µs |
| OR120 (80000) | `bm1`, `chx`, `ch`, `bm2`, `c`, `d` (the chain after `a`; `raw` and `a` stay) | −72.2 / −68.8 | or120_clean spk −0.041 pp, or120_dimed +0.009 pp | 28.6 / 36.9 → 30.4 / 41.5 % |
| SLO-100 (122000) | `k3a` (moved from C rt in §10.44) | −64.7 / −53.2 | DI 0.149 → 0.129 %; slo_od_hot 0.548 → 0.311 % (`k3a` 0.148 → 0.089 %, `p4b` 0.143 → 0.098 %). Alias −66.3 dB | 69.1 / 78.1 → 69.1 / 81.0 %, p99 1443 → 1706 µs (§10.44; was 99.0 / 113.3 % pre-§10.44) |

- **Entangled entries.** In each of these pairs, removing one entry merges the groups and makes the other entry
  inert. The renders are identical or within −107 dB.
  - Twin and Deluxe: `n1` with `vib`, and `bp4` with `dry`.
  - BE-100: `kcf` with `c9o`.
  - LA-2A: `t4b_light` with `sc_tap`, and `b_275` with `sc_tap`.
  - 1176: `pre_ac` with `ctl_c27`; `v30` adds only −107 dB.
  - Dyna Comp: `env` with `vcc`.
  - SLO-100: `b2` and `plates` with `k3a`.

  A supply node in such a pair was judged on its own residual, which is A wherever it was measured.
- **Stability.**
  - Every removal idles flat: the difference falls to a constant floor, no growth.
  - Every removal runs the dimed sine with no Newton failure and no homotopy.
  - Every removal passes the gate (test_aliasing included).

**Kept for real time (C rt).** Each of these is closer to the truth uncut, but uncut it runs over one 9900X core.

| Cut | Truth: shipped → uncut | 9900X real / driven mean, shipped → uncut (pre-§10.44; §10.44) | Note |
|---|---|---|---|
| Plexi `kcf` | DI 0.813 → 0.653 %; plexi_dimed −0.226 pp, AC 5–10 kHz +0.046 dB. Alias −64.8 → −75.6 dB | 78.7 / 83.2 → 113.3 / 115.2 % (76.7 / 80.6 → 107.6 / 111.6 %) | the merged group runs at VPA's 16× |
| Plexi variac `kcf` | pvar_drive −0.048 pp, AC +0.044 dB. Alias −65.8 → −76.1 dB | 78.2 / 81.8 → 114.4 / 118.1 % (76.4 / 79.7 → 109.9 / 111.0 %) | |
| 1959RR `kcf` | DI 0.553 → 0.486 %; rr_dimed spk 2.06 → 0.40 % | 78.8 / 81.7 → 114.6 / 115.4 % (78.5 / 81.5 → 108.6 / 109.3 %) | |
| AC30 `kcf` | DI 0.520 → 0.325 %; ac30_ac 5–10 kHz −0.028 dB | 72.1 / 78.7 → 102.3 / 111.8 % (73.4 / 79.4 → 99.5 / 106.9 %) | real under one core in §10.44; driven > 100 % |
| JC-120 `mi1` + `mi2` | dimed spkL / spkR 1.459 / 1.477 → 0.028 / 0.029 % | 97.9 / 122.7 → 146.6 / 259.3 % (83.2 / 102.5 → 126.5 / 171.5 %) | the cuts are what give the two-lane split (§10.33); closed in §10.50: with R22_1 / R53 out of the loads the cut reads 0.027 / 0.026 % |

(2026-11-14, §10.48: the four Plexi-family `kcf` rows are now (B). Uncut, their merged group runs at the base rate with 16×
substeps on events, at 29–47 / 41–54 % of a 9900X core real / driven.)

**§10.40's open question.**
- **BE-100.** Moot: `kcf` is gone. It was kept for real time at first, on the pre-§10.42 bench (99.5 / 107 %), and moved
  to (B) once the current tree's bench put it under one core. The capped estimate would have been 0.398 % on the DI,
  against 1.164 % for the group hold and 0.063 % uncut.
- **1959RR and Plexi.** They keep the group hold, which is closer: 1959RR DI 0.553 % group against 0.674 % capped; Plexi
  0.813 % against 1.115 %.
- **JCM800.** Moot: `kcf` is gone. The capped estimate would have been 0.744 %, against 1.248 % for the group hold and
  0.689 % uncut.

**Kept (C), with the reason.**
- **Op-amp supply pins.** The engine requires them cut, because rail-node op-amps draw from cut rails one sample late
  (§10.10a):
  - CE-1 `p14` `m14` `p13` `m13`; dbx `s_p` `s_m`; Surveyor `vaf` `vbf`; Memory Man `a` `b`;
  - Klon `v18` `vn`; Bluesbreaker `vcc`; JH-1 `vs`; every Bi-Phase cut; RAT `v9`; SSL EQ `vp` `vn`; RE-201 `b11`;
  - Dimension D `vccR` / `vccL`; JC-120 `v15` `v30`.
- **No DC point, or over the port limit, or over 15 minutes for 48 s of audio.**
  - Dimension D `p20` `m20` `qb7` `sns`; Dimension D `p14` / `m14`, whose groups would be 120–123 ports;
  - Klon `vd7` `cp`; JC-120 `zb15` `vraw` `zb30`; SLO MK II `n6` / `m6` alone; Uberschall `bres` `rm` `lch` alone;
    Rectifier `a` alone;
  - Fairchild `b450` `m450` `b240`.
- **Newton trouble.** Fairchild `g302`: 287,050 homotopies on the bench and 7× the CPU.
- **Removing the cut breaks the result.**
  - Fairchild `sg303` / `k303` / `g303` / `r313t` / `r313b`: f670_attack_cv off by 86–660 pp.
  - OR120 `raw` alone: or120_fac off by 1.3e5 %.
  - Rectifier and Rev C `rm`: the output nulls at 0 dB.

**MT-2 `vb` is (A).** The SPICE cases are mixed: mt2_drive improves 1.204 → 0.597 % but mt2_small worsens 0.140 →
0.152 %. On the real DI the cut is closer to the truth: the out node is 0.612 % with the cut and 1.002 % without it,
against §10.42's 0.5 s `mt2_lead_d32` reference (current engine). Uncut, the aliasing would also fail the −60 dB gate
(−61.4 → −58.4 dB). Kept.

**Everything else is (A).** Some notes:
- **Supply chains** null at −68 to −150 dB with outputs within ±0.01 pp. This covers every Marshall, Fender, Mesa,
  Soldano, Bogner, Hiwatt, Supro, SVT and 5E3 chain.
- **Uncut is worse** in several places. Twin `hra`/`hrb` and Bassman / Deluxe `bpa` fail the gate uncut. So do the
  Rectifier `hra`/`d1m` (rect_tube_spongy +81 pp) and Uni-Vibe `v2`. Fairchild `v2`/`na`/`nc` are slightly worse uncut.
- **SVT `bscn` / `biasn`.** Only the rail's own waveform improves (0.35–0.42 pp); spk stays within 0.004 pp, for
  +57 % CPU.
- **Fairchild with every supply cut removed** improves only the f670_attack_cv control node (−0.28 pp) and costs 3.2×.
- **Small modulation and side-chain gains.** Klon `vb` improves only the v18 rail. Bluesbreaker `vref` gains 0.016 pp
  but its aliasing goes −81.8 → −65 dB. Electric Mistress `vbias` gains 0.015 pp. These are LFO-phase or rail-only
  gains.
- **Uni-Vibe.** Its single cuts are mixed. Removing every supply cut costs 10 dB of aliasing.
- **JH-1 `ref`** is mixed (−0.015 / +0.037 pp) for +190 % CPU.
- **Signal cuts that measured clean.** JC-120 `vb7` `vsw15` `p37` `sar` `sbr` `c2r` `mi2`-only `lineout` `spkL`
  `c15`; Bogner `k5a`, OR120 `hra`/`hrb` and Fairchild `nr`, which are no-ops (bit-identical).

**Tests.** test_circuit_engine, test_circuit_unit, test_effect_handoffs and test_aliasing pass. They ran on the engine
with §10.42.
- re201_play_ac passes now.
- `testNewtonTolerance` checks only that a JCM800 group reports "tube ports 0.001". With `kcf` gone, no JCM800 group is
  all-tube.
- No fixture changed: ngspice decks carry no cuts.

**Files.** `work/circuit-netlists/cpu-9900x/cutaudit/` holds the following:
- the fine-step references (`refs/*.npz`);
- the scripts (`scripts/`);
- the per-cut sweep and gate results (`results/`);
- the shipped versions of every edited netlist (`orig/`), for a per-unit revert.

To drop a kept cut later, run `dropcuts.py <netlist> <node>...`; it edits only the `cuts` array.

### 10.44 Ahead-of-time sparse specialisation (`circuit_codegen`, 2026-11-14)

**The bottleneck.** After §10.42 and §10.43, two shipped amps still exceeded one Ryzen 9 9900X core (`5150` at 104.3 / 107.8 % and `Uberschall` at 106.6 / 112.2 %), and `SLO-100`'s `k3a` cut stayed in `C rt` at 99.0 / 113.3 % uncut. Profiling `5150` (18-port preamp + 11-port power stage at 192 kHz), `BE-100` (21-port merged preamp after `-kcf -c9o`), and `Uberschall` (21-port preamp) showed that the dense `nn × nn` port-space Newton solver and dense linear state-space updates (`pfree` / `yfree` / `out`) were doing 75–88 % multiply-by-zero work — yet a generic CSR/CSC sparse interpreter (`CE_NOCODEGEN=1`) is *slower* than dense at `nn = 16..24` (`5150` 120.0 / 124.5 % vs 104.3 / 107.8 % dense) because indirect `rowPtr` / `colIdx` loads and short-trip loop branches stall out-of-order execution.

**Design.** Ahead-of-time C++ specialisation turns the sparse structures of every shipped unit into branch-free scalar functions compiled at `-O3`, while keeping the runtime interpreter as a bit-identical fallback for edited or imported circuits (`__model` topology edits, SPICE imports):

1. **Two sparse representations in `Engine.cpp` (`GroupRt::SparseLin` and `GroupRt::SparseLu`).**
   - **`SparseLin`** compresses the per-sample linear port/output operators (`A0`, `B0`, `C0`, `Cx`, `Du`, `Dy`) into CSR rows and enables when `3 · ops_sparse < ops_dense` (`kEnvLinWeight = 3.0`, `CE_LINWEIGHT`).
   - **`SparseLu`** solves the nodal DK Newton system directly on the `Mraw` sparse pattern for DK groups with `nn >= 16` (`kEnvSpMinPorts`, `CE_SPMINPORTS`), switching from chord (`spStep`) to full refactorisation (`spFactor` + `spStep`) when `nn / sp.factorRatio >= 4.2` (`kEnvSpFullRatio`, `CE_SPFULL`).
2. **Canonical DC-independent Markowitz ordering (`Circuit::buildSparse`, `Engine.cpp`).**
   - Running Markowitz pivot selection (`spOrder`) on `Mraw + Jd(vp)` made the elimination tree drift slightly with the DC operating point `vp` across knob sweeps (`BE-100` produced 59 distinct `Lu` trees and `Dual Rectifier` 63), and occasionally pivoted on a pure-`Jd` entry that vanished when a tube or diode cut off.
   - `buildSparse` now runs `spOrder` on `Mraw` alone while tracking symbolic fill across `Sw = Sbase` (all structural `Mraw` nonzeros plus every nonlinear `st` stamp slot), falling back to `Mraw + Jd(vp)` only if `Mraw` alone is structurally singular or DC `spFactor` fails.
   - Result: pivot order depends only on switch topology, not continuous knob positions. `BE-100` collapsed from 59 → **6** `Lu` structures (one per switch state), `Dual Rectifier` from 63 → **3**, and `5150`, `Uberschall`, `Ecstasy`, `SLO-100`, `SLO-100 MkII`, `SVT`, `Twin`, `Deluxe`, and `DR103` each have **1** `Lu` structure total across all continuous knob positions and LTE substep runtimes.
3. **64-bit structural FNV-1a dispatch (`Codegen.hpp`, `circuit_codegen.cpp`, `CMakeLists.txt`).**
   - Each `SparseLin` and `SparseLu` hashes its exact dimensions and index arrays (`r.linHash`, `sp.hash`) and looks up `cg::findLinKernel` / `cg::findLuKernel` at `buildSparseLin` / `buildSparse` time (`CompileOptions::codegen`, disabled by `CE_NOCODEGEN=1`).
   - `circuit_codegen` surveys all 68 bound units across host rates `44.1 / 48 / 88.2 / 96 kHz`, hum off/on, readout probes, open-switch topologies (caching compiled `Circuit` instances whenever `!topologyStale`), and ganged control sweeps in parallel across worker threads, emitting 16 balanced translation units (`generated_kernels/circuit_kernels_00..15.cpp`) compiled into `rig_circuit_kernels`.
   - Full library generation takes **8.7 s** on the 9900X (**9.3 s** on Mac M4 Pro) and emits **242 `SparseLin` + 105 `SparseLu`** kernels (`0` failed). Every generated kernel executes the exact same IEEE-754 floating-point operations in the exact same order as the interpreter (`0` ULP difference; identical SHA-256 hashes over 96 s renders).

**9900X results** (`taskset -c 10,12 nice -n 10`, `bench --table --real`, `CE_REALREPS=3`, 24 s × 4 DIs real and fuzz-driven, noise and hum off):

| Unit | Pre-§10.44 (§10.43): real / driven %, p99 | `CE_NOCODEGEN=1` (interpreter): real / driven %, p99 | **§10.44 (`circuit_codegen`): real / driven %, p99** | Delta vs pre-§10.44 (vs interpreter) |
|---|---|---|---|---|
| 5150 (134000) | 104.3 / 107.8 % | 120.0 / 124.5 %, 3011 µs | **85.0 / 87.9 %, 2123 µs** | −18.5 / −18.5 % (−29.1 / −29.4 %) |
| Uberschall (81000) | 106.6 / 112.2 %, 2447 µs | 109.4 / 115.7 %, 2513 µs | **76.2 / 80.8 %, 1765 µs** | −28.5 / −27.9 % (−30.3 / −30.1 %) |
| BE-100 (151000) | 87.3 / 94.7 %, 1822 µs | 97.9 / 105.4 %, 2008 µs | **69.5 / 74.5 %, 1446 µs** | −20.4 / −21.3 % (−29.1 / −29.3 %) |
| SLO-100 MkII (163000) | 83.1 / 88.3 %, 1690 µs | — | **62.5 / 66.4 %, 1286 µs** | −24.8 / −24.8 % (p99 < 1333 µs) |
| SLO-100 (122000, uncut `-k3a`) | 99.0 / 113.3 %, 2436 µs (69.1 / 78.1 % cut) | 97.3 / 113.4 %, 2367 µs | **69.1 / 81.0 %, 1706 µs** | −30.2 / −28.5 % vs uncut (matches old cut) |
| JC-120 (49000) | 97.9 / 122.7 % | 103.3 / 127.3 %, 2798 µs | **83.2 / 102.5 %, 2286 µs** | −15.0 / −16.5 % (−19.4 / −19.5 %) |
| Dual Rectifier (16000) | — | — | **51.6 / 57.1 %, 1554 µs** | 3 `Lu` kernels across all modes |
| JCM800 (56000) | 47.6 / 57.5 %, 1569 µs | 58.3 / 70.5 %, 1940 µs | **47.5 / 57.5 %, 1574 µs** | unchanged vs dense (−18.5 / −18.4 % vs interp) |
| Twin (30000) | 44.8 / 55.5 %, 1226 µs | — | **41.5 / 50.0 %, 1118 µs** | −7.4 / −10.0 % (`SparseLin` + `SparseLu`) |
| Fairchild 670 (24000) | — | — | **33.1 / 42.8 %, 1268 µs** | ganged input/release selectors specialised |

- **Every shipped mono amp now runs under one 9900X core on both real and driven material** (`5150` 85.0 / 87.9 % and `Uberschall` 76.2 / 80.8 % were the last two over 100 %); `JC-120` (stereo two-lane amp) drops under one core on real guitar (97.9 → 83.2 %).
- **`SLO-100` (`122000`) `k3a` promoted from `C rt` to `(B)`.** Removing `k3a` (`soldano-slo100-122000.circuit.json`) drops fine-step `T/16` lead DI audio-band NRMSE from 0.149 % to **0.129 %** (`slo_od_hot` spk 0.548 → 0.311 %, `k3a` 0.148 → 0.089 %, `p4b` 0.143 → 0.098 %; null −64.7 / −53.2 dB rms), idles flat over 20 s (0 homotopies, 0 failures), runs dimed with 0 homotopies and 0 failures, passes `alias` (−66.3 dB), and runs at **69.1 / 81.0 %** (the pre-§10.44 cut amp was 69.1 / 78.1 %).
- **The other five `C rt` cuts stay kept.** Specialised uncut on the 9900X, `Plexi` (107.6 / 111.6 %), `Plexi variac` (109.9 / 111.0 %), `1959RR` (108.6 / 109.3 %), and `AC30` (99.5 / 106.9 %) still exceed one core because dropping `kcf` pulls the 20-port merged preamp+PI+PA group up to VPA's 16× (768 kHz); `JC-120` without `mi1` + `mi2` loses its two-lane split and runs at 126.5 / 171.5 %.

**Verification (`test_circuit_unit`, `test_circuit_engine`, `test_aliasing`, `test_effect_handoffs`, `test_unit_param_mapping`).**
- `Circuit::codegenStats(bound, total)` (`Engine.hpp`) checks 100 % kernel binding (`bound == total`) across full-step and LTE substep runtimes at both default controls and unseen random continuous knob positions (`0x42424242u`) on all 14 sparse/heavy units in `test_circuit_unit`, plus 0.0 max sample difference over a 2048-sample `5150` control glide against `CompileOptions::codegen = false`.
- 96 s `bench --real` hashes on `5150`, `BE-100`, `Uberschall`, `SLO-100`, `JCM800`, and `JC-120` match bit-for-bit between default and `CE_NOCODEGEN=1`, and 0.5 s `render` SHA-256 hashes match across default, `CE_NOCODEGEN=1`, and `CE_NOLANES=1`.

### 10.45 Ahead-of-time Newton kernels: the dense port-space solve specialised per group structure (`circuit_codegen` phase 2, 2026-11-14)

**Profile first (9900X, after §10.44).** `rdtsc` regions around every step of `newtonKernel` and `processGroup` (a
throw-away instrumented build, `bench --real`, one rep, real + driven material; the per-region overhead is about 32
cycles). Shares of the unit's total cycles:

| Bucket | 5150 | JCM800 | Plexi (uncut `kcf`) | JC-120 |
|---|---|---|---|---|
| Newton, full step (`processGroup`) | 39 % | 31 % | 57 % | 58 % |
| LTE substeps (their own Newton, `linPorts` / `linOut`) | 39 % | 44 % | — (no LTE at 4×) | 12 % |
| `linPorts` + `linOut` (full step) | 7 % | 5 % | 31 % (no generated `SparseLin` for the uncut structure) | 6 % |
| LTE check (`lteSubsteps`, `nonlinearEvent`) | 3 % | 6 % | 0 % | 3 % |
| inputs, predictor, history and state update | 6 % | 7 % | 6 % | 13 % |

Inside the Newton kernel (all calls, substeps included), the 5150 / JCM800 / Plexi uncut / JC-120 split was: device
evaluation 23 / 26 / 21 / 18 %, the factorisation (`wbFactor`, `luFixedN`, `spFactor`) 21 / 15 / 18 / 24 %, the chord
attempt 9 / 9 / 4 / 6 %, the step from a factorisation 9 / 7 / 8 / 6 %, the per-call setup (current-convergence scan,
predictor correction) 8 / 6 / 13 / 8 %, `F = K·cur + p − vp` 7 / 8 / 7 / 7 %, the limiter loop 6 / 4 / 4 / 8 %, the
residual check 6 / 6 / 9 / 3 %, the live-row scan and `K` gather 3 / 5 / 3 / 3 %, accept 2 / 2 / 3 / 2 %.

Two findings set the design:
- The Newton kernel is where the time is, in the full step and in the LTE substeps alike (substeps are Newton solves
  at 3-4 iterations with a fresh factorisation each). Specialising it pays on both paths at once, and on the 16× VPA
  ladder groups, which call the same kernel.
- The per-sample bookkeeping around a group (inputs and cut injections, predictor, history, the state update
  `w = G2·v + sgn·w`) is 6-13 % of a unit and already plain vector loops over the group's sizes; it was not specialised
  in this round (at most a few percent is reachable there without changing arithmetic). The LTE substep path and the
  multirate ladder groups (the 16× VPA groups) get the specialised kernel through `Circuit::newton`, which both call;
  their `linPorts` / `linOut` already ran §10.44's generated `SparseLin` kernels.

**Design.**
1. **One kernel source (`src/circuit/NewtonKernel.hpp`).** `newtonKernel`, its helpers (limiters, small dense LU,
   Woodbury factor / step, the `SparseLu` interface) and the switches they read moved out of `Engine.cpp` into an
   internal header, unchanged. The kernel is now `newtonKernel<NN, Spec>`: every loop or branch over devices and ports
   reads the group structure through `Spec`. `DynSpec` (the interpreter, any circuit) reads it from the arguments at run
   time, exactly as before. `StaticSpec<G>` reads it from a generated `G` of compile-time constants: each device's law
   (`NlKind`), port count, first port and exact-split shift flag, and each port's device, block size and
   current-convergence class.
2. **What the static spec removes.**
   - Device evaluation: `NlDevice::evalAs(kind, shift, np, …)` (`Devices.hpp`) is `eval` with the kind, shift flag and
     port count as arguments; with constants the law's switch, the shift test and the per-device loop fold away, and
     the devices are straight-line code the core can overlap.
   - The per-call current-convergence scan (`currentConv` over every device and port, §10.42) becomes a constant mask.
   - The accept, residual-check, live-row and dense-Jacobian loops over devices are unrolled by device.
   - The limiter loops stay loops: unrolled per port with constant limiter kinds they measured *slower* (5150 limiter
     region 207 → 314 cycles per call; the per-port copies of `limitPort` and `portRatio` outgrow the branch-predicted
     loop). The predictor correction and the chord test likewise.
3. **Bit identity by construction.** A specialised kernel is the interpreter's source instantiated with constants for
   what decides control flow, so every floating-point operation, operand and order is the interpreter's. The one place
   where that was not enough: `F = K·cur + p − vp` was a `#pragma clang fp reassociate(on)` dot product inlined into
   each kernel, whose summation order is the compiler's choice per inlining context. It is now `nkResidual<NN>`, one
   out-of-line copy per size that the interpreter and every specialised kernel call (the linker keeps one), and it
   compiles to the same order as the inlined form did: the engine's output is unchanged against §10.44.
4. **Generation and dispatch.** `circuit_codegen` collects `Circuit::codegenNewtonPatterns()` (every full-step and LTE
   substep runtime, the lazy-clamp clamp-free set too) over the same survey as §10.44, and emits per structure a `G`
   and one line, `return nk::newtonKernel<NN, nk::StaticSpec<G>>(…)`, into the same 16 shards. The structure signature
   (`newtonPattern` in `Engine.cpp`: port count, per device kind / ports / shift, per port limiter kind and
   current-convergence classes) is looked up in `cg::findNewton` when a group runtime is built (`makeGroupRt`) and bound
   to `GroupRt::nkFull` / `nkRed`; `Circuit::newton` calls it with the interpreter's arguments. A structure the build did
   not see (an edited or imported circuit) has no entry and runs the interpreter. `CE_NOCODEGEN=1` or
   `CompileOptions::codegen = false` switch all generated kernels off; `CE_CGDIAG=1` prints each group's binding;
   `Circuit::codegenStats` now counts the Newton kernels too (test_circuit_unit requires 100 % binding on its 14 units).

**Bit-identity proof.**
- `bench` output hashes (FNV-1a of every output sample, 0.4 s of the driving chord) of all 68 bound units, at table
  defaults and at seeded random positions of every continuous control (136 renders), on the M4 Pro (arm64) and the
  9900X (x86-64-v3): §10.45 default = `CE_NOCODEGEN=1` = the engine before §10.45, 136 of 136 on both machines.
- `bench --real` hashes (real and driven, 24 s each) of every unit in the CPU table below are identical between the
  engine before §10.45 and §10.45, on the 9900X.
- Lanes: `CE_NOLANES=1` against default, 136 of 136 identical (JC-120 ran 37 882 two-lane samples).
- The refactor alone (the move into `NewtonKernel.hpp` and `DynSpec`), before any kernel was generated, was checked the
  same way: 136 of 136.

**Size and build time.** 177 Newton structures (1 to 60 ports), next to §10.44's 242 `SparseLin` and 105
`SparseLu`. Text: arm64 9.58 → 12.58 MB (+3.0 MB), x86-64 14.74 → 19.78 MB (+5.0 MB) for `circuit_cli`; the 16 shard
objects 9.2 → 28.6 MB (x86, unstripped). Generation 8.9 s on the 9900X (§10.44: 8.7 s); a clean `circuit_cli` build
89 → 108 s (16 threads). Most structures are small groups (1-8 ports: 118 of 177) whose kernels are a few KB; the
40 groups above 12 ports take most of the text (12-15 KB each).

**9900X results** (`taskset -c 8,10 nice -n 10`, `bench --table --real`, `CE_REALREPS=3`, the engine
before §10.45 and §10.45 run back to back per unit; two other benches ran on other cores throughout, so absolute
figures sit 0-3 % from §10.44's; the deltas are like for like, and every pair's hashes are identical):

| Unit | Before (§10.44): real / driven %, p99 | **§10.45: real / driven %, p99** | Delta real / driven | p99, % of the 1333 µs period | Factor still needed (§3.5) |
|---|---|---|---|---|---|
| 5150 (134000) | 84.6 / 87.0 %, 2139 µs | **78.7 / 80.3 %, 1992 µs** | −6.9 / −7.6 % | 149 % | 3.5× |
| JC-120 (49000) | 83.8 / 103.3 %, 2296 µs | **79.8 / 100.1 %, 2267 µs** | −4.8 / −3.1 % | 170 % | 4.0× |
| Uberschall (81000) | 78.1 / 82.8 %, 1811 µs | **72.5 / 76.8 %, 1685 µs** | −7.1 / −7.2 % | 126 % | 2.9× |
| BE-100 (151000) | 71.9 / 77.1 %, 1489 µs | **66.6 / 71.2 %, 1366 µs** | −7.3 / −7.6 % | 102 % | 2.4× |
| SLO-100 (122000) | 72.0 / 82.8 %, 1734 µs | **65.5 / 76.9 %, 1627 µs** | −9.0 / −7.2 % | 122 % | 2.8× |
| SLO-100 MkII (163000) | 64.5 / 68.7 %, 1326 µs | **57.7 / 60.7 %, 1187 µs** | −10.6 / −11.6 % | 89 % | 2.1× |
| Dual Rectifier (16000) | 51.0 / 55.8 %, 1501 µs | **48.6 / 53.3 %, 1423 µs** | −4.7 / −4.6 % | 107 % | 2.5× |
| JCM800 (56000) | 49.3 / 60.3 %, 1691 µs | **42.7 / 51.7 %, 1436 µs** | −13.4 / −14.3 % | 108 % | 2.5× |
| Twin (30000) | 41.7 / 50.1 %, 1121 µs | **37.7 / 45.8 %, 1038 µs** | −9.6 / −8.7 % | 78 % | 1.8× |
| Plexi (34000) | 75.0 / 79.2 %, 1307 µs | **67.8 / 71.4 %, 1194 µs** | −9.6 / −9.9 % | 90 % | 2.1× |
| Plexi variac (92000) | 75.6 / 79.1 %, 1289 µs | **67.7 / 70.9 %, 1172 µs** | −10.5 / −10.3 % | 88 % | 2.1× |
| 1959RR (91000) | 77.7 / 80.6 %, 1274 µs | **69.8 / 72.1 %, 1141 µs** | −10.2 / −10.5 % | 86 % | 2.2× |
| AC30 (38000) | 73.7 / 79.2 %, 1219 µs | **64.8 / 70.5 %, 1098 µs** | −12.1 / −11.0 % | 82 % | 2.0× |

The factor is max(real / 32 %, p99 / 43 % of the period), §3.5's M1/M2-class budget on the 9900X. On the M4 Pro the
kernels cut retired instructions by 12 % (JCM800) and 16 % (5150) on the same material.

**The five lossy cuts, re-evaluated (§10.43's rule).** Accuracy and stability are §10.43's: the change is bit-identical,
so every uncut render, null, gate case and stability run is unchanged. What decides is the 9900X mean, with the uncut
netlists in the embedded library (so their own `SparseLin`, `SparseLu` and Newton kernels are generated, as they would
ship):

| Cut | Uncut, §10.44: real / driven % | **Uncut, §10.45: real / driven %, p99** | Decision |
|---|---|---|---|
| Plexi `kcf` | 107.6 / 111.6 % | **100.7 / 104.0 %, 1706 µs** | kept (C rt): over one core on both |
| Plexi variac `kcf` | 109.9 / 111.0 % | **102.2 / 102.4 %, 1657 µs** | kept (C rt) |
| 1959RR `kcf` | 108.6 / 109.3 % | **101.8 / 102.1 %, 1616 µs** | kept (C rt) |
| AC30 `kcf` | 99.5 / 106.9 % | **90.2 / 103.6 %, 1685 µs** | kept (C rt): real under one core, driven over |
| JC-120 `mi1` + `mi2` | 126.5 / 171.5 % | **119.7 / 165.7 %, 4174 µs** | kept (C rt): the cuts are the two-lane split; §10.50 brings the cut to uncut accuracy |

No cut is removed: none of the five uncut circuits runs under one 9900X core on both real and driven material. The three
Marshalls are now 1-4 % over and the AC30's driven figure 3.6 % over; dropping a cut stays one `dropcuts.py` edit when
the next round gets them under. (The uncut Plexi's `bench --real` hashes equal the pre-§10.45 interpreter's.)

**Verification.** test_circuit_engine, test_circuit_unit (Newton kernels included in `codegenStats`' 100 % binding and in
the 5150 glide's 0.0 difference against `CompileOptions::codegen = false`), test_effect_handoffs, test_aliasing and
rig_tests pass on the M4 Pro. test_circuit_engine's SLO-100 guard still asked for the `k3a` split that §10.44 removed
(39-port group); it now guards the uncut group instead.

**Developer switches.** `CE_NOCODEGEN=1` also turns the Newton kernels off; `CE_CGDIAG=1` prints each group's Newton
binding (`codegen newton: group g (nn n) kernel | interpreter`).

**What is left, and where.** The Newton kernel now spends its time in arithmetic the bit-identity rule keeps: the
factorisation of every first iteration (one per sample, about a fifth of Newton), the device laws themselves (Koren and
junction tables, about a fifth), and the LTE substeps' 3-4 refactorised iterations (39-44 % of the 5150 and JCM800). The
next levers change operations, so they are measured against the fine-step references rather than hashed: fewer
factorisations per sample (a cross-sample chord that holds through an attack, §10.32's `CE_XCHORD`), the LTE substep
policy (§10.35), and SIMD across devices of one kind (four triode table lookups per AVX2 register is bit-identical per
lane, but needs gathers and per-lane range fallbacks).

### 10.46 LTE substep bursts: where p99 / mean comes from, a substep predictor, and the JCM800's BDF2 start (2026-11-14)

**The question.** The user's words: "the re-step on the pick attacks. We want a more level CPU load, regardless of playing
style." The aim was a lower p99 / mean ratio of the 64-frame block cost, and cheaper substep bursts. The rule was equal or
better error against the true circuit and on every gate. Everything below sits behind the circuit-engine flags, like the
rest of §10.

**Where p99 / mean comes from.** `bench --real` now prints the combined block mean and p99 / mean. With `CE_BLOCKINSTR=1`
(macOS) it also counts each block's retired instructions in the first run, a ratio that does not depend on machine load.
M4 Pro, real + driven material, instructions per block, p99 / mean:

| Unit | LTE as shipped | LTE off (`CE_LTE=0`) | LTE share of the mean |
|---|---|---|---|
| JCM800 | 2.28 | 1.47 | +57 % real / +78 % driven |
| 5150 | 1.88 | 1.63 | +33 / +28 % |
| Uberschall | 1.70 | 1.34 | +27 / +28 % |
| SLO-100 | 1.72 | 1.41 | +9 / +17 % |
| JC-120 | 2.05 | 1.93 | +9 / +9 % |
| Plexi | 1.27 | — | LTE nearly idle (0.009 events per sample on real) |

- **The substep bursts are the ratio on the LTE amps.** On the JCM800 they take the p99 block from 9.6 M to 25.0 M
  instructions.
- **Not on every amp.** The JC-120's ratio comes from elsewhere: its Newton iterations on driven material, 31 per sample
  against 25 on real. The Plexi is already flat; its problem is the mean (the 16× power group).
- **Event statistics** (`CE_LTESTAT=1`, printed per group by `describeGroups`):
  - **Most events continue an earlier one.** JCM800 real: 101 k detected on a rejected full step, 914 k continued. 5150:
    78 k and 652 k.
  - **Chains are long.** On both amps they peak at 12–16 samples, and the JCM800 driven at 20–25. That is sustained
    curvature, not the estimator's 5-sample window.
  - **Some continued samples change no device regime.** 32 % on the JCM800 and 10 % on the 5150 have no regime change
    inside the sample: ringing that the continuation follows on purpose (§3.1).
  - **Substep solves take 2–3.9 iterations.** Full steps take 1.0–1.4. Each still factorises once and spends its
    remaining iterations as chord steps.
  - **Where the extra iterations go (5150).** On its T/4 and T/2 substeps, the extra iterations sit on the flyback clamps
    CR200 / CR201 and the power tubes: 40–50 % of non-converged iterations from the third on are the clamps. That is the
    knee itself, not the guess.

**Change 1: the substep predictor (`CE_SUBPRED`, default 30; `CE_SUBPRED=0` is the engine before).** Only Newton's start
moves: same equations, tolerance and acceptance test. Bits:
- **16:** a detected event's first substep heads for the rejected full step's solution.
- **2:** a sample that continues the last one's substep count starts from the last substeps' own port voltages. They are
  still in the trapezoidal runtime, and a bitwise match with `vpPrev` proves they are this circuit's.
- **8:** the full step's predictor correction (§10.31) along the substep runtime's own p history, through its kept
  factorisation, wherever that history is the same runtime's.
- **4:** quadratic extrapolation along the substep grid once three real grid points exist.
- **Not in the default:** 1, the shoot-out's "mode 1" (the shoot-out notes §3.1).

It applies to groups of at least 19 ports (`CE_SUBPREDPORTS`); every smaller group is byte-identical to before. By
`bench --real` hashes the JC-120 (8–18-port groups), Plexi, Plexi variac, 1959RR and AC30 are byte-identical. Units with a
group of 19 or more ports take it: JCM800, 5150, Uberschall, both SLO-100s, BE-100, Twin, OR120, the Rectifiers, the
Fairchild, Small Stone and Bi-Phase.
- **Mode 1, ported and measured.** On the 9900X (three rounds, rotated core pairs) it gives JCM800 −1.0 / −1.4 % but 5150
  +0.7 / +0.9 % mean and +1.7 % p99. The slope through the last two samples overshoots on the 5150's BDF2 T/4 starts:
  3.5 → 5.9 iterations per solve. An earlier measurement predates codegen phase 2 and before the JCM800's kcf removal. So it is a
  bit, not the default.
- **The 19-port floor.** Without it the predictor costs the JC-120 (8–18-port groups) +1.5 / +3.3 % real / driven on the
  9900X and +1.7 % driven instructions: a saved iteration on a small group is worth about the correction step. Bit 4 alone
  is what costs the JC-120. On the large groups it adds a further 1–1.7 % on the JCM800 (head-to-head against 26 on the
  9900X) and is neutral on the 5150, Uberschall and SLO-100.
- **Bits 8 and 2 carry the gain.** JCM800 T/4 substeps go from 2.31 to 1.83 iterations per solve, T/2 from 2.67 to 2.22.
  But a saved iteration is mostly a chord step, so the instructions fall by much less than the iterations.

**Change 2: the JCM800 starts its substeps on BDF2, at tolerance 4** (`"lte": {"start": "bdf2", "tol": 4}`). The
backward-Euler start restarted every continued sample with a damped step. On an amp whose events chain for 12–25 samples,
that was its largest error left.
- **History.** §10.16 kept the JCM800 on backward Euler for −2.4 to −3.3 dB of aliasing. That was measured before §10.43
  merged its groups (the `kcf` cut went). Re-measured now, BDF2 improves its alias figure.
- **Truth** (fine-step ngspice; NRMSE % full / 20 Hz–20 kHz, before → after):

| Reference | spk | pa | p4a | kcf | p3 |
|---|---|---|---|---|---|
| JCM800 Lead T/32 | 0.721 / 0.651 → 0.257 / 0.169 | 0.709 / 0.637 → 0.306 / 0.175 | 0.283 / 0.282 → 0.074 / 0.048 | 0.220 / 0.235 → 0.052 / 0.051 | 0.233 / 0.242 → 0.049 / 0.048 |
| JCM800 Rock T/32 | 1.283 / 0.972 → 0.352 / 0.151 | 1.322 / 0.961 → 0.398 / 0.150 | 0.749 / 0.646 → 0.187 / 0.093 | 0.604 / 0.570 → 0.092 / 0.080 | 0.580 / 0.553 → 0.084 / 0.074 |
| JCM800 1 kHz sine T/16 | 0.844 / 0.690 → 0.317 / 0.078 | 0.814 / 0.684 → 0.296 / 0.066 | 0.269 / 0.190 → 0.149 / 0.094 | 0.244 / 0.214 → 0.067 / 0.064 | 0.216 / 0.198 → 0.046 / 0.046 |
| JCM800 110 Hz sine T/16 | 0.0038 / 0.0037 → 0.0036 / 0.0035 | 0.0077 / 0.0071 → 0.0077 / 0.0071 | 0.0030 / 0.0023 → 0.0028 / 0.0020 | 0.0004 / 0.0003 → 0.0002 / 0.0002 | 0.0010 / 0.0010 → 0.0002 / 0.0002 |
| JCM800 Lead 2 s T/16 (cutaudit) | 0.751 / 0.681 → 0.259 / 0.162 | 0.742 / 0.660 → 0.318 / 0.162 | 0.292 / 0.285 → 0.073 / 0.046 | 0.227 / 0.237 → 0.050 / 0.050 | — |

- **SPICE gate** (T/8 fixtures, before → after):
  - jcm_drive spk / pa / p3: 0.101 / 0.106 / 0.029 → 0.012 / 0.022 / 0.002 %.
  - jcm_dimed spk / pa / pb: 0.368 / 0.368 / 0.479 → 0.261 / 0.296 / 0.464 %, and the energy above 30 kHz closer on
    all three.
  - jcm_clean is unchanged.
- **Why tolerance 4.** It is the loosest tolerance that keeps every gate figure at or below today's:

| BDF2 tol | jcm_dimed spk / pa / pb (%) | Alias (dB) | Mac G instr/s real / driven (before: 7.43 / 9.00) |
|---|---|---|---|
| 2 | 0.112 / 0.187 / 0.218 | −58.8 | 7.58 / 9.38 |
| 3 | 0.136 / 0.208 / 0.248 | | |
| **4** | **0.261 / 0.296 / 0.464** | **−59.6** | **6.94 / 8.80** |
| 5 / 6 | 0.247 / 0.498 / 0.494, 0.248 / 0.505 / 0.506 | | |
| 8 | 0.251 / 0.509 / 0.513 | −59.5 | 6.42 / 8.22 |
| 16 | 0.274 / 0.508 / 0.556 | −60.9 (passes the gate) | 6.03 / 7.69 |

  - The dimed fixture is good enough to decide on. A fresh T/64 deck of jcm_dimed (ngspice 47) differs from T/8 by only
    0.012 / 0.014 / 0.018 %.
  - The engine's ranking against it is the same: live today (backward Euler, tol 2) 0.399 / 0.399 / 0.519; tol 4
    0.272 / 0.314 / 0.480; tol 16 0.285 / 0.523 / 0.572.
  - So 8 and 16, though 50–80 % closer on every real-guitar reference, really are worse on the dimed plates.
- **Aliasing** (test_aliasing): −52.3 → −59.6 dB. The JCM800 is still short of −60 dB, so it stays reported, not gated.

**What else moved** (the predictor; the JCM800's rows are above):
- **Fine-step references.** Every other figure (5150, BE-100, Bassman, SLO-100, AC30, Plexi, 1959RR, AC15, Twin, Tube
  Driver, DS-1, HM-2, MT-2, Big Muff, Fuzz Face Si, LA-2A) moves by at most 0.07 % of itself, in both directions. The
  largest is 5150 Lead p5a, 0.0876 → 0.0877 % audio band.
- **The JCM800's own figures** are from this default. Without bit 4 (`CE_SUBPRED=26`) they read 0.1–0.2 % of themselves
  lower on the Rock excerpt; both are 70–87 % under the engine before.
- **SPICE gate** (third decimal, both directions):
  - p5150_dimed spk 0.073 → 0.072; uber_dimed spk / pa 0.759 / 0.915 → 0.758 / 0.914;
  - f670_attack outl 0.137 → 0.135, ctl1 6.840 → 6.838, gy1 (internal) 0.791 → 0.800; f670_hot p1071 0.645 → 0.646.
  - The Fairchild's 30-port side-chain group is the one place an internal node reads 1 % of its own error higher. Its output
    reads lower.
- **Aliasing:** the OR120 reads −68.2 → −68.3 dB. No other unit's figure moves, the time-varying units included.

**Measured and rejected** (switches removed; the patch is `work/circuit-netlists/level/experiments-engine.patch`):
- **A T/3 substep level** (an estimate of 8–27× the tolerance takes 3 substeps instead of 4): −1.6 to −3.5 % instructions.
  But JCM800 error +5 to +18 %, 5150 −8 to +6 % by node, Fuzz Face Si +5 %. The estimate's h³ scaling does not hold
  across the start step.
- **Continuing an event only while a device changed regime** (the full step's gate, applied to the continuation):
  - JCM800 −2.7 / −4.1 %, 2–11 % closer on the guitar references, but +0.1 to +0.6 % on the 1 kHz sine;
  - but the BDF2 units get worse: BE-100 +2.6 %, SLO-100 +1.5 %, Twin +3.8 % full band, 5150 +0.03 %.
  - The ringing it drops is real error on those units.
- **Trapezoidal continuation on backward-Euler groups.** JCM800 −58 to −68 % error. But the Plexi (+0.32 %), 1959RR
  (+0.45 %) and MT-2 clip node read worse, and the JCM800's alias −0.4 dB. The BDF2 start above supersedes it.
- **BDF2 on the Plexi and 1959RR:** their LTE is nearly idle (0.0007–0.009 events per sample on real). Truth +0.4 to
  +0.7 % audio band. Not opted in.

**9900X** (`bench --table --real`, `CE_REALREPS=3`, noise and hum off. The engine before this section, this section, and
this section without bit 4 (`CE_SUBPRED=26`) ran side by side on three core pairs, rotated over three rounds; the table
gives means of the rounds. % of one core real / driven; block µs over real and driven together):

| Unit | Before: real / driven %, p99 / max µs, mean µs, **p99 / mean** | **§10.46**: real / driven %, p99 / max µs, mean µs, **p99 / mean** | Mean, p99 |
|---|---|---|---|
| JCM800 | 41.4 / 50.7 %, 1434 / 1868 µs, 613.9 µs, **2.34** | **37.2 / 47.3 %, 1332 / 1811 µs, 563.4 µs, 2.36** | −8.2 %, −7.1 % |
| 5150 | 75.6 / 78.3 %, 1969 / 2687 µs, 1026.0 µs, **1.92** | **75.4 / 78.2 %, 1973 / 2698 µs, 1023.8 µs, 1.93** | −0.2 %, +0.2 % (noise) |
| Uberschall | 67.9 / 72.2 %, 1616 / 2216 µs, 933.7 µs, **1.73** | **67.5 / 71.7 %, 1573 / 2182 µs, 928.4 µs, 1.69** | −0.6 %, −2.7 % |
| SLO-100 | 63.4 / 74.6 %, 1598 / 2296 µs, 919.7 µs, **1.74** | **62.3 / 72.9 %, 1558 / 2272 µs, 901.7 µs, 1.73** | −2.0 %, −2.5 % |
| JC-120 | 71.7 / 90.1 %, 2105 / 3203 µs, 1078.8 µs, **1.95** | byte-identical | — |
| Plexi | 64.9 / 68.7 %, 1158 / 1438 µs, 891.1 µs, **1.30** | byte-identical | — |

- **Where the JCM800's gain comes from.** A run of the same layout compared the engine before with this section at
  `CE_SUBPRED` 26 and 0. Without the predictor, the BDF2 start alone gives 42.1 / 51.7 → 39.5 / 51.1 % and p99 1459 →
  1463 µs. Its mean falls 3 %; its p99 does not.
- **What the predictor adds** on the large groups (the new binary with `CE_SUBPRED` 0 / 1 / 30, three rotated rounds):
  - JCM800 −3.6 / −4.8 % and p99 −6.1 %; mode 1 alone −1.0 / −1.4 %.
  - 5150 −0.9 / −0.1 %, at the noise level; mode 1 alone +0.7 / +0.9 %.
  - Uberschall p99 −2.7 %.
- **Noise.** Two binaries running bit-identical 5150 arithmetic differ by about 1 % (code layout).
- **M4 Pro, instructions per block** (deterministic; real + driven):
  - JCM800: mean 10.95 → 10.03 M (−8.4 %), p99 25.0 → 23.1 M (−7.5 %), p99 / mean 2.28 → 2.31.
  - 5150: −1.1 % / −1.6 %, 1.88 → 1.88.
  - Uberschall: −1.8 % / −3.6 %, 1.70 → 1.67.
  - SLO-100: −0.8 % / −2.1 %, 1.72 → 1.70.

The JC-120 and Plexi rows are from the first two-round run. Both units are byte-identical to before (same `bench --real`
hashes).

**The honest summary on p99 / mean: it did not fall.**
- **What fell.** The JCM800's p99 and mean, by 7–8 %, together. Elsewhere 0–3 %.
- **Why the ratio stays.** The bursts are not a cost the solver wastes:
  - the substeps run where devices cross knees;
  - their solves are near one factorisation each;
  - on the BDF2 amps every continuation we could drop carried truth.

**Event-local oversampling for the 16× groups (shoot-out idea #3): not built.**
- **What it needs.** A merged group whose rate changes per sample, in the rate ladder (Multirate.hpp, the converters and
  `processLadder`). Its output stream to the 16× decimator would be synthesised by half-band interpolation between events
  and taken from the substeps during them.
- **Cost.** That is a new piece of the ladder, not a policy change. Its alias figure can only be measured once it exists.
- **The data so far.** The shoot-out notes show 4× with LTE at −58.9 dB against 16×'s −66.8 dB on the uncut Plexi. And the
  shipped Plexi is already at p99 / mean 1.30. The ratio problem it would solve belongs to the uncut variants' mean, not to
  bursts.
- **So.** It stays the open structural lever for dropping the Plexi family's `kcf` cuts.

**What is left.**
- **The bursts are close to their per-substep floor.** One factorisation and one or two device sweeps per substep, and
  most of the remaining iterations are at device knees (the 5150's flyback clamps).
- **On the BDF2 amps,** the 5150, Uberschall and SLO-100, every substep we tried to skip carried truth.
- **The levers left change the integrator:**
  - a higher-order or L-stable substep scheme that needs m = 2 where trapezoid needs 4 (§10.41's Rosenbrock was slower;
    TR-BDF2 inside the substeps is untried);
  - for the JCM800, tolerance 8–16 once the dimed plates are closer at that tolerance.
- **Not LTE.** The JC-120's ratio comes from its driven-material Newton iterations. The Plexi family's issue is the 16×
  mean.

**Tools and files.**
- `CE_LTESTAT=1`: per-group event statistics in `describeGroups` (`CE_PROFILE=1` prints them in `bench`).
- `CE_BLOCKINSTR=1`: per-block instructions in `bench --real` (macOS).
- `bench --real` now also prints the combined block mean and p99 / mean.
- `work/circuit-netlists/level/scripts/`:
  - `truth.py` renders every fine-step reference (ros, tol and cutaudit) through the unit path;
  - `cmp.py` compares two runs;
  - `mbench.sh` and `matrix.sh` are the Mac instruction and configuration sweeps;
  - `summ9.py` builds the 9900X tables.
- The shipped JCM800 netlist is in `level/orig/`.

**Verification** (Mac):
- test_circuit_engine, test_circuit_unit (including the codegen kernel binding, 5150 20 / 20, glide difference 0),
  test_effect_handoffs and test_aliasing (48 of 48 gated) pass.
- `bench --real` hashes are identical between default, `CE_NOCODEGEN=1` and `CE_NOLANES=1` on the JCM800, 5150 and
  JC-120.
- EmbeddedLibrary.hpp is regenerated, and the codegen kernels with it. Every runtime of the JCM800's BDF2 configuration
  binds a generated kernel.

### 10.47 TR-BDF2 inside the LTE substeps: L-stable and better for aliasing, but no cheaper at equal truth; not landed (2026-11-14)

**The question.** §10.46 left the LTE substep bursts as the source of the p99 / mean ratio on the JCM800, 5150, Uberschall
and SLO-100. The lever it named was the integrator: an L-stable, second-order scheme inside the substeps with an
embedded error estimate. The hope was about two solves where trapezoid substeps need four, at equal or better truth.
Full steps keep the trapezoid. The rule was equal or better against the fine-step ngspice references, every gate and
p99.

**Method** (in Engine.cpp behind `CE_TRBDF2=1`, off by default; the diff is also kept as `work/circuit-netlists/trbdf2/trbdf2-prototype.patch`).
- **Scheme.** An event sample runs as M macro steps of H = T/M (M = 1 or 2). Each macro step is one TR-BDF2 step (Bank
  et al. 1985; Hosea and Shampine 1996):
  - a trapezoidal stage to t + γH, then a BDF2 stage through (t, t + γH) to t + H, with γ = 2 − √2;
  - with that γ, both stages have the companion C/(γH/2), so one trapezoidal runtime at γT/M serves the whole sequence.
    They sit in the unused [2] slots of `rtTr_` / `rtBe_` and are refreshed lazily like the other substep runtimes;
  - the BDF2 stage only sets its history: s = G·(a·x_γ − b·x_0) for a capacitor and a·i_γ − b·i_0 for an inductor, with
    a = 1/(γ(2 − γ)) and b = (1 − γ)²/(γ(2 − γ)). The runtime's own trapezoidal update then leaves s = G·v + i,
    which is what the next stage starts from;
  - L-stable and self-starting from (v, i) at t[n], so it needs no backward-Euler or history-based BDF2 start;
  - inputs and cut injections are interpolated to each stage time, and the speakers and hysteresis plays advance by
    each stage's own length;
  - the generated kernels bind (CE_CGDIAG: the 5150's γT and γT/2 runtimes take the same SparseLin, SparseLu and Newton
    kernels as the other runtimes).
- **Embedded estimate.** est = H·Σ(b − b̂)ᵢ·fᵢ over the three stage derivatives, with b − b̂ = ((√2 − 1)/3, −1/3,
  γ/3) (Hosea and Shampine's third-order companion, unfiltered). The derivatives come from each stage's capacitor
  current or inductor voltage.
  - For x = t³ it equals the step's own error, γ(1 − γ)/6·H³·x''' = 0.0404·H³·x'''. The trapezoid's is H³·x'''/12.
  - Inside the event it replaces the third-difference history estimate. The next sample is a full step if the
    trapezoid's predicted error, est·M³/(2γ(1 − γ)), is within the LTE control's per-state tolerance. Otherwise it takes
    one or two macro steps, using lteCount's boundary of 8 (`CE_TBSCALE` scales it).
- **Detection** is unchanged: the full step, the third-difference estimate and the regime gate. A detected event takes
  one macro step where the trapezoid took 2 substeps and two where it took 4. This is the **equal-solves mapping**.
- **Newton start.** A detected event's first stage heads for the rejected full step's solution. Each stage extrapolates
  linearly at the stage spacing, with the predictor correction through the kept factorisation along the stage grid.
  This is valid across stage types, because one runtime means one v = p + K·i(v) manifold. A continued sample starts
  from the last sample's stages when it is on the same runtime. That last part cut 5150 stage iterations by 5–8 %.

**Why it cannot win on smooth error.** One TR-BDF2 step costs two solves and has error 0.0404·H³·x'''. Two trapezoidal
half steps also cost two solves, and have 2·(H/2)³/12 = 0.0208·H³·x'''. Per solve, TR-BDF2 is about 2× less accurate.
L-stability costs error constant; it does not buy it. So "two solves where the trapezoid needs four" means accepting
about 31× the trapezoid m = 4's local error per sample. What TR-BDF2 does buy is damping of stiff and near-fs/2
components, with no start step.

**Truth** (Mac, fine-step references through the unit path, noise and hum off; geometric mean over every node of the
amp's references of variant / base NRMSE, full band / 20 Hz–20 kHz; "worst" is the worst single node's ratio).
- The references are JCM800 ×5 (ros, cutaudit), 5150 ×2, SLO-100, BE-100 and Twin.
- The Uberschall has no fine-step DI reference; its SPICE gate cases stand in for one.

| Amp | Equal-solves TR-BDF2 | Always one macro step (`CE_TBMAX=1`) | Equal-solves, CE_TBSCALE 0.25 |
|---|---|---|---|
| JCM800 | −5.6 % / +2.0 % (worst 1.52) | +30.7 % / +13.5 % (worst 3.58) | +8.0 % / +5.9 % |
| 5150 | −14.9 % / −20.5 % (worst 1.66) | +33.7 % / +10.7 % (worst 2.00) | +3.1 % / −9.3 % |
| SLO-100 | +1.8 % / −3.5 % | +7.4 % / −5.7 % | +3.9 % / −4.3 % |
| BE-100 | +1.6 % / +0.2 % | +6.4 % / −0.8 % | +3.5 % / −0.0 % |
| Twin | +2.9 % / −7.7 % | −3.5 % / +7.1 % | +1.5 % / −2.4 % |
| Uberschall gate, uber_dimed spk / pa / pb | 0.759 / 0.915 / 1.111 → 0.602 / 0.668 / 0.896 % | | |

- On the Uberschall, the other gate cases move by the third decimal: uber_drive spk 0.025 → 0.023 %, p3a 0.006 →
  0.008 %; uber_clean_drive spk 0.011 → 0.013 %.
- Speaker figures, full / audio band (%):

| Reference | Base | Equal-solves | One macro step |
|---|---|---|---|
| JCM800 Lead T/32 | 0.257 / 0.169 | 0.247 / 0.173 | 0.315 / 0.173 |
| JCM800 Rock T/32 | 0.352 / 0.151 | 0.371 / 0.160 | 0.994 / 0.278 |
| JCM800 1 kHz T/16 | 0.317 / 0.078 | 0.190 / 0.086 | 0.632 / 0.160 |
| 5150 Lead T/32 | 2.016 / 0.489 | 1.666 / 0.616 | 3.052 / 0.686 |
| 5150 Rock T/32 | 3.695 / 1.121 | 3.049 / 1.217 | 5.637 / 2.248 |

- TR-BDF2 lowers the energy above 20 kHz, the trapezoid's high-frequency error, so full-band figures fall. In the audio
  band the JCM800 and 5150 speakers read worse.

**Substeps per event and Newton iterations** (`CE_LTESTAT`, JCM800, real DIs). Solves per event sample: base 2.61,
equal-solves TR-BDF2 2.81, one macro step 2.00.
- The embedded estimate keeps more continued samples on two macro steps: 325 k against base's 249 k on m = 4. It flags
  the same states as the history estimate (the output transformer, CPPW, the screen-grid caps CWA / CWB, the speaker
  inductance), plus the power tubes' interelectrode caps more often.
- Stage solves take 1.9–2.1 iterations on the JCM800. Its trapezoid T/2 and T/4 substeps take 1.5–1.7, because the
  §10.46 substep predictor puts them closer.
- On the 5150 the stages take 2.7–2.9 iterations, against 3.5 (T/2) and 2.4 (T/4) on the trapezoid.
- Overall Newton per sample: JCM800 real / driven 3.79 / 4.48 → 4.00 / 4.90 (equal-solves) → 3.82 / 4.37 (one macro
  step).

**Tolerance sweep: no point is at or under the base on both truth and CPU** (Mac instructions, real / driven; TR-BDF2 at
1×, 2× and 4× the netlist tolerance, with `CE_TBSCALE` 1 and 0.5).

| Amp | Equal truth or better | Cost there | Cheaper points (truth) |
|---|---|---|---|
| JCM800 | 1×: −5.5 / +2.0 % (audio worse) | +2.3 / +4.2 % | 2×: −4.5 / −1.7 % instr, truth +6 / +10 %; 4×: −10 / −7 %, truth +17 / +16 % |
| 5150 | 1×: −15 / −21 % | +3.8 / +3.4 % | 2×, scale 0.5: −1.6 / −1.3 %, truth +14 / −2 % (worst node 2.3×); 4×: −3 %, truth +71 / +28 % |
| SLO-100 | none (1× full +1.8 %) | +0.4 / +1.6 % | 2×: −1.6 / −3.1 %, truth +39 / −0.3 % |
| Twin | none (1× full +2.9 %) | +0.2 / +2.4 % | 2×, scale 0.5: +0.2 / −1.6 %, truth +1.5 / +0.4 % |
| BE-100 | none at its default; at tol 2 vs base at tol 2: spk 0.0775 / 0.0557 → 0.0722 / 0.0562 % (mixed) | +1.8 / +4.0 % (tol 2) | 4× (tol 8): +0.3 / +0.5 %, truth +2 / +0.2 % |

The BE-100 row's "×" is against `CE_LTE=2`. `render --table` does not run a netlist without `lte.tol` at `CE_LTE=2`
(base 0.0928 % against 0.0775 % at an explicit 2), so its pairs above were rerun at equal explicit tolerance.

**Aliasing gate** (`circuit_cli alias --table`, worst non-harmonic bin): TR-BDF2 is **better on all six**. This is
§10.16's open hypothesis: a uniformly L-stable substep integrator removes the spurs that substep switching makes.

| Amp | Base | Equal-solves TR-BDF2 | One macro step |
|---|---|---|---|
| JCM800 | −59.6 dB | **−62.9** (would pass the −60 gate) | −61.6 |
| 5150 | −50.4 | −52.9 | −48.9 |
| Uberschall | −58.5 | −59.4 | −63.1 |
| SLO-100 | −66.3 | −66.7 | −63.7 |
| BE-100 | −61.5 | −67.1 | −67.9 |
| Twin | −83.7 | −85.3 | −85.2 |

**CPU, 9900X** (`bench --table --real`, `CE_REALREPS=3`, noise and hum off. One binary, three configurations on three
core pairs rotated over three rounds; means of the rounds. Real / driven are % of one core; block µs are over real and
driven together):

| Unit | Base: real / driven %, mean / p99 µs, p99 / mean | Equal-solves TR-BDF2 | One macro step |
|---|---|---|---|
| JCM800 | 38.5 / 49.4, 586 / 1388, **2.37** | 39.1 / 50.9, 600 / 1442, 2.40 | 36.2 / 42.1, 522 / 969, **1.86** |
| 5150 | 78.9 / 82.2, 1074 / 2104, **1.96** | 81.4 / 84.4, 1105 / 2182, 1.97 | 71.8 / 75.0, 978 / 1777, 1.82 |
| Uberschall | 70.7 / 72.5, 955 / 1635, **1.71** | 71.5 / 73.3, 965 / 1681, 1.74 | 66.8 / 68.1, 900 / 1394, 1.55 |
| SLO-100 | 64.5 / 74.5, 927 / 1583, **1.71** | 64.3 / 75.8, 935 / 1647, 1.76 | 63.9 / 72.8, 911 / 1502, 1.65 |
| BE-100 | 62.6 / 66.9, 863 / 1314, **1.52** | 63.1 / 67.9, 873 / 1351, 1.55 | 62.2 / 66.0, 854 / 1230, 1.44 |
| Twin | 36.9 / 45.1, 547 / 1009, **1.85** | 36.9 / 46.1, 553 / 1132, 2.05 | 36.9 / 44.7, 544 / 942, 1.73 |

**M4 Pro instructions** (deterministic; G/s real / driven; per-block mean / p99 in M, combined; p99 / mean):

| Unit | Base | Equal-solves | One macro step |
|---|---|---|---|
| JCM800 | 6.69 / 8.38; 10.0 / 23.1; 2.31 | 6.93 / 8.91; 10.6 / 25.3; 2.40 | 6.45 / 7.55; 9.3 / 17.8; 1.91 |
| 5150 | 12.52 / 12.94; 17.0 / 31.9; 1.88 | 13.25 / 13.71; 18.0 / 34.8; 1.94 | 11.78 / 12.13; 15.9 / 28.3; 1.78 |
| Uberschall | 11.36 / 12.01; 15.6 / 26.0; 1.67 | 11.56 / 12.32; 15.9 / 27.4; 1.72 | 11.01 / 11.41; 14.9 / 23.3; 1.56 |
| SLO-100 | 10.77 / 12.55; 15.5 / 26.4; 1.70 | 10.81 / 12.83; 15.8 / 27.8; 1.76 | 10.74 / 12.44; 15.4 / 25.6; 1.66 |
| BE-100 | 10.78 / 11.54; 14.9 / 22.0; 1.48 | 10.81 / 11.70; 15.0 / 22.7; 1.52 | 10.75 / 11.48; 14.8 / 21.3; 1.44 |
| Twin | 6.58 / 7.87; 9.6 / 17.3; 1.79 | 6.59 / 8.07; 9.8 / 19.9; 2.04 | 6.59 / 7.84; 9.6 / 16.5; 1.72 |

**Verdict: not landed; no amp takes it by default.**
- **The brief's target configuration flattens the bursts.** Always one macro step is two solves per event sample. It
  takes the JCM800's p99 / mean from 2.37 to 1.86 and its p99 from 1388 to 969 µs, and the other five amps' p99 down
  by 5–16 %. But it reads 31–34 % further from the true circuit on the JCM800 and 5150, with worst
  nodes 2–3.6×. That breaks the user's rule.
- **At equal solves it is not cheaper.** Mean +0.8 to +3 % and p99 +3 to +12 %, because the embedded estimate continues
  more events on two macro steps. Its truth is mixed: the 5150 is 15–21 % closer, the JCM800, SLO-100 and Twin read
  worse on some nodes, and the BE-100 is about equal.
- **No tolerance or scale reaches equal truth at equal or lower CPU** on any of the six. TR-BDF2's per-solve error
  constant is twice the trapezoid's, and the substep bursts are spent on that error.
- **What it is good for.** Aliasing improves on all six: the JCM800 to −62.9 dB, and the BE-100 by 5.6 dB. That
  confirms the L-stability mechanism §10.16 suspected. If the aliasing gate, not CPU, is ever the binding constraint on
  the JCM800 or 5150, the equal-solves form is the candidate. Its cost there is +2–3 % mean and +4 % p99 on
  the 9900X.
- **Not tried, and why.** Hosea and Shampine's stiff filter (I − γH/2·J)⁻¹·est: in DK form it is one extra linear solve
  per event sample with a fresh factorisation, more than the continuation it could save. A third-order ESDIRK with a
  second-order estimate would need three solves per step.

**Gates and identity** (Mac, `app/build-trbdf2`). With the switch off the engine is bit-identical: `bench --real` hashes
match the engine before on the JCM800, 5150 and JC-120 (all three hashes of each). test_circuit_engine (lanes
bit-identity included), test_circuit_unit (codegen kernel checks included: 244 Lin + 105 Lu kernels, 5150 20 / 20
bound, glide difference 0), test_effect_handoffs, test_aliasing (48 of 48 gated) and rig_tests pass. With
`CE_TRBDF2=1`, test_circuit_engine, test_aliasing (48 of 48) and test_effect_handoffs pass too. The report-only rows
move: JCM800 −59.6 → −62.9 dB, 5150 −50.4 → −52.9 dB, Uberschall −58.5 → −59.3 dB.

**Files.** `work/circuit-netlists/trbdf2/`:
- `trbdf2-prototype.patch` (Engine.cpp / Engine.hpp: `substepTrBdf2`, `tbRuntime`, switches `CE_TRBDF2`, `CE_TBSCALE`,
  `CE_TBFULL`, `CE_TBMAX` and `CE_TBPRED`);
- the truth TSVs, the Mac instruction logs and the 9900X logs;
- the scripts (`cfg.sh`, `amp.sh`, `tab.py`, `ssum.py`, `alias.sh`, `bench9.sh`).

**The code stays in Engine.cpp behind `CE_TRBDF2`.** Its section is marked "TR-BDF2 LTE substeps". It is off by default, and with it
off every unit is bit-identical, so no default changes. It stays for two reasons. It is the measured candidate if the
aliasing gate ever binds on the JCM800 or 5150. And the event-local oversampling work already reads `GroupRt::tb`.
To adopt it for a netlist: add `"trbdf2"` to the `lte.start` values Netlist.cpp accepts and map it to
`CompileOptions::lteStart` 2; the runtime allocation already reads that value. Then regenerate EmbeddedLibrary.hpp and
the kernels.

### 10.48 Event-local oversampling: the Plexi family runs uncut, at the base rate with 16× substeps on events (2026-11-14)

**The question.** The Plexi (34000), Plexi variac (92000), 1959RR (91000) and AC30 (38000) kept lossy `kcf` cuts (§10.43,
C rt). Uncut, their merged preamp + power group (19 ports, 49 states) ran on the rate ladder at 16× (768 kHz) all the
time, only to keep the output transformer's flyback from aliasing (§10.32), at 100–104 % of a 9900X core (§10.45). The
shoot-out's idea #3 (the shoot-out notes): run that group at the base rate, take 16× only where a flyback happens, and give the
decimator a band-limited 768 kHz stream.

**Method** (`Engine.cpp`, section "Event-local oversampling"; netlist `"multirate": {"rates": {"VPA": 16}, "events": true}`):
- **Rate.** A group the ladder would raise to level L (L ≤ 2) stays at the base rate (192 kHz at a 48 kHz host) with its
  LTE control on. Every LTE decision over one substep, detected or continued, takes M = 2^L = 4 substeps (`lteSubsteps`,
  `lteSubstepsFromHistory`). The event detector is the LTE estimate itself, with its device-regime gate (§10.16): a power
  tube cutting off into the OT leakage is exactly where it fires. With no other group raised, the ladder is off.
- **The output stream at M×.** For each base interval (n − 1, n]:
  - an event sample gives its three substep values of the output node (`evosRecord`, from the same rows the substep
    computes: the sparse path's `out`, the port-defined row, or the export's row of `Hc` / `H`);
  - any other sample gives a Kaiser-windowed sinc interpolation of the base-rate samples (`CE_EVOS_P` = 4 samples each
    side, cutoff 0.9 of the base Nyquist, β 7). Where those samples are clean it puts nothing above 96 kHz into the stream;
  - the end of the interval is the base-rate sample itself in both cases.
- **The decimator.** A linear-phase Kaiser FIR at M× (41 taps, `CE_EVOS_Q` = 5 base samples each side, cutoff at the base
  Nyquist, β 11). The output delay is therefore an integer: P − 1 + Q = 8 base samples (42 µs at 48 kHz), against the
  ladder's fractional ≈ 3.9 (its up- and down-converting half-band stages). Every other node is read undelayed, as on any
  base-rate group. The ladder's IIR stages (`CE_EVOS_IIR=1`) give the same alias figures, but a frequency-dependent delay.
- **The integrator.** Event samples start on BDF2 (§10.16), tolerance 16 on the Plexi, variac and 1959RR, 8 on the AC30
  (was 16). Backward-Euler starts at the live tolerance 2 were tried first; the BDF2 start is better on every measure (below).
- **Measured, rejected** (switches kept for A/B, off by default):
  - a continued sample at the estimate's own count, 2 or 4 (`CE_EVOS_CONTM=1`): Plexi alias −53.0 dB (8× folds 380 kHz);
  - tolerance 64 / 128: −53.7 / −61.9 dB; 32: −72.5 dB, but spk 0.118 % against 0.079 %;
  - continuing an event only while a device changes regime (`CE_EVOS_REGIME=1`): −1.4 / −3.4 % instructions and −6 % on
    the p99 block, for truth 0.5–1.5 % of itself worse. Not taken;
  - the substep predictor bits (§10.46) at 26 / 18 / 2 / 0: no better than the default 30.

**Aliasing** (dB re the tone; gate = `circuit_cli alias` = test_aliasing; ideal = `CE_ALIAS_IDEAL=1`, re-implemented from
the shoot-out notes §3.3: the tone generated at the circuit's rate and every non-harmonic 10 Hz bin of 20 Hz–20 kHz of its output,
a perfect decimator; it reproduces the gate on the shipped Plexi, −64.9 against −64.8):

| Unit, 0.5 FS (gate level) | Shipped (`kcf` cut, VPA 16×) | Uncut, ladder 16× | Uncut, whole circuit 4× (LTE) | **Uncut, events** |
|---|---|---|---|---|
| Plexi | −64.8 / −64.9 | −75.7 / −66.8 | −59.0 / −58.9 | **−76.4 / −67.0** |
| Plexi variac | −65.8 / −66.6 | −76.0 / −67.0 | | **−76.7 / −67.4** |
| 1959RR | −74.3 / −66.4 | −74.9 / −66.3 | | **−73.7 / −66.3** |
| AC30 | −64.3 / −64.7 | −64.7 / −62.9 | | **−63.9 / −65.2** (tol 16: −63.2 / −64.1) |

| Drive level, gate / ideal | Plexi shipped → events | Variac | 1959RR | AC30 |
|---|---|---|---|---|
| 0.25 FS | −72.1 / −66.2 → −74.3 / −66.3 | −72.7 / −67.1 → −75.1 / −66.4 | −73.9 / −66.6 → −74.5 / −66.6 | −65.4 / −65.9 → −66.8 / −65.2 (tol 16) |
| 1.0 FS | −65.0 / −64.8 → −73.4 / −66.6 | −62.4 / −61.6 → −72.7 / −66.6 | −67.1 / −66.3 → −75.3 / −66.4 | −57.5 / −58.7 → −57.1 / −57.5 (tol 8) |

- **Seams** (the switch between true and interpolated sub-samples, §10.16's worry) measured three ways:
  - every sample substepped, so no seam exists (`CE_EVOS_FORCE=1`): Plexi −75.6 / −66.8, AC30 −64.1 / −65.7. The Plexi with
    seams is no worse; on the AC30 the difference (≤ 1.6 dB at tol 16, 0.2 dB at tol 8) is the base-rate stretches between
    events, not the interpolation:
  - interpolator half-width P = 1 / 2 / 4 / 8: Plexi gate −74.9 / −75.8 / −76.4 / −77.8, ideal −66.7 … −67.0; AC30 gate
    −63.6 / −63.4 / −63.2 / −63.5 (tol 16). A seam artefact would follow the interpolator's quality; it does not;
  - decimator cutoff 0.5–1.0 of the base Nyquist and the IIR stages: identical figures.
- **The AC30 is the one unit not at least level with shipped on the gate metric:** −63.9 against −64.3 dB at 0.5 FS, and
  −57.1 against −57.5 at 1.0 FS (where every variant fails −60, the ladder's uncut −57.3 included). On the ideal metric
  it is better (−65.2 against −64.7). Tolerance 0.5 would reach −64.1 dB; the gate (−60) passes with 3.9 dB to spare.

**Truth** (the 2 s lead DI references at T/16, `cutaudit/refs`; NRMSE % full band / 20 Hz–20 kHz; noise and hum off; each node
at its best integer lag, which is the shipped harness's 4 on spk; the fractional-lag audio figure, which removes any pure
delay, in the last column):

| Ref, node | Shipped | Uncut, ladder 16× | **Events** | Audio at the best fractional lag: shipped / uncut / events |
|---|---|---|---|---|
| Plexi spk | 2.039 / 0.801 | 1.605 / 0.648 | **0.595 / 0.079** | 0.796 / 0.106 / 0.079 |
| Plexi pa | 3.489 / 2.847 | 4.126 / 3.246 | **0.773 / 0.072** | 0.790 / 0.138 / 0.072 |
| Plexi kcf | 1.909 / 1.929 | 1.468 / 1.470 | **0.018 / 0.018** | 1.514 / 0.059 / 0.018 |
| Plexi p3a | 1.871 / 1.905 | 2.213 / 2.263 | **0.171 / 0.040** | 0.529 / 0.098 / 0.040 |
| AC30 spk (tol 8) | 0.521 / 0.518 | 0.326 / 0.324 | **0.066 / 0.061** | 0.198 / 0.074 / 0.061 |
| AC30 pa | 1.591 / 1.576 | 1.425 / 1.409 | **0.067 / 0.060** | 0.209 / 0.116 / 0.060 |
| AC30 kcf | 1.189 / 1.227 | 1.053 / 1.086 | **0.015 / 0.012** | 0.084 / 0.038 / 0.012 |
| 1959RR spk | 0.642 / 0.546 | 0.557 / 0.480 | **0.111 / 0.046** | 0.394 / 0.069 / 0.046 |
| 1959RR pa | 2.616 / 2.336 | 2.675 / 2.381 | **0.123 / 0.050** | 0.399 / 0.106 / 0.050 |
| 1959RR kcf | 1.232 / 1.232 | 1.047 / 1.052 | **0.011 / 0.010** | 0.842 / 0.013 / 0.010 |
| 1959RR p3a | 1.346 / 1.345 | 1.408 / 1.422 | **0.023 / 0.017** | 0.347 / 0.105 / 0.017 |

- Every node is closer than shipped and than the uncut ladder, at every lag convention. The internal nodes gain most:
  they are now solved at the truth's own sample instants, where the ladder read them behind a half-band converter.
- The tolerance and start choice (Plexi, Mac): backward Euler at 1 / 2 / 4 / 8 / 16 gives spk audio 0.168 / 0.159 / 0.152 /
  0.145 / 0.141 % and gate alias −65.7 … −66.5 dB; BDF2 at 4 / 16 gives 0.070 / 0.079 % and −73.8 / −76.4 dB.
- `level/scripts/truth.py` now aligns these three cases per node (spk 8, the rest 0); through it: Plexi spk 0.595 / 0.080,
  AC30 0.066 / 0.062, 1959RR 0.111 / 0.047 % (shipped 2.038 / 0.802, 0.521 / 0.518, 0.642 / 0.546).

**SPICE gate** (test_circuit_engine's cases compile without levels, so they read the netlist change: uncut, BDF2 start;
NRMSE %, shipped → uncut at the old start → **now**):
- Plexi: drive spk 0.085 → 0.054 → **0.023**, var0 spk 0.112 → 0.064 → **0.026**, dimed spk / pa / pb 0.387 / 0.437 / 0.425 →
  0.160 / 0.225 / 0.212 → **0.253 / 0.396 / 0.370**, clean spk 0.013 → **0.002**.
- Variac: drive spk 0.112 → **0.026**, clean spk 0.015 → **0.002**.
- 1959RR: in1 spk 0.486 → 0.084 → **0.038**, var0 spk 0.257 → 0.081 → **0.050**, dimed spk / pa / pb 2.064 / 2.409 / 2.572 →
  0.401 / 0.534 / 0.462 → **0.572 / 0.979 / 0.596**.
- AC30 (tol 16 → 8): drive spk 0.052 → **0.030**, dimed spk / pa / pb 0.250 / 0.286 / 0.256 → **0.171 / 0.220 / 0.157**.
- AC 5–10 kHz: Plexi 0.019 → 0.065 dB and variac 0.018 → 0.062 dB worse, the uncut circuit's own figure (§10.43 measured
  it at +0.046 dB); 1959RR 0.041 → 0.014 and AC30 0.084 → 0.056 dB better. DC unchanged.
- Every case is closer than shipped except those two AC figures. The dimed Marshall cases at tolerance 16 sit between
  shipped and the uncut backward-Euler start; tolerance 4 brings them to it (Plexi dimed spk 0.163, 1959RR 0.309) for
  about +10 % CPU. 16 was taken for CPU.

**CPU, 9900X** (`bench --table --real`, `CE_REALREPS=3`, noise and hum off, base and new trees built side by side; base,
uncut and events run at once on rotated core pairs (2,4 / 6,8 / 10,12), three rounds, means; the block figures combine real
and driven material; the period is 1333 µs):

| Unit | Shipped: real / driven %, mean, p99, max µs, p99/mean | Uncut, ladder 16× | **Uncut, events** | Mean real / driven against shipped; p99 |
|---|---|---|---|---|
| Plexi | 67.7 / 73.0 %, 938, 1290 (97 %), 1902, 1.38 | 100.6 / 101.7 %, 1348, 1688 (127 %), 2113, 1.25 | **46.9 / 54.0 %, 673, 1431 (107 %), 2167, 2.13** | −31 / −26 %; p99 +11 % |
| Plexi variac | 66.8 / 71.5 %, 922, 1245 (93 %), 3126, 1.35 | 101.4 / 103.2 %, 1364, 1757 (132 %), 2135, 1.29 | **46.3 / 53.1 %, 663, 1398 (105 %), 3094, 2.11** | −31 / −26 %; p99 +12 % |
| 1959RR | 67.5 / 70.6 %, 920, 1163 (87 %), 1611, 1.26 | 99.4 / 99.0 %, 1322, 1602 (120 %), 2022, 1.21 | **38.6 / 44.7 %, 555, 1221 (92 %), 2000, 2.20** | −43 / −37 %; p99 +5 % |
| AC30, tol 16 | 61.6 / 66.4 %, 853, 1117 (84 %), 2055, 1.31 | 88.8 / 96.1 %, 1233, 1618 (121 %), 3055, 1.31 | 28.7 / 40.4 %, 461, 1153 (86 %), 3882, 2.50 | −53 / −39 %; p99 +3 % |
| AC30, tol 8 (a second, noisier run) | 63.5 / 69.0 %, 884, 1343 (101 %), 4930, 1.52 | | **30.0 / 43.9 %, 493, 1300 (97 %), 5152, 2.64** (tol 16 there: 29.6 / 41.0 %, 1185 µs) | −53 / −36 %; p99 −3 % |

**Mac (M4 Pro), retired instructions** (load-independent; interpreter on all three, `CE_NOCODEGEN=1`, because the uncut
structures were not in the Mac build's registry at the time; G instructions per audio second real / driven, and the
combined 64-frame block p99):

| Unit | Shipped | Uncut, ladder 16× | **Events** |
|---|---|---|---|
| Plexi | 17.3 / 18.1, 28.9 M | 24.1 / 24.6, 38.7 M | **10.8 / 12.2, 31.3 M** |
| Plexi variac | 17.5 / 18.1, 28.6 M | 24.5 / 24.7, 38.4 M | **10.7 / 12.1, 30.5 M** |
| 1959RR | 18.0 / 18.5, 28.2 M | 24.6 / 24.8, 37.8 M | **9.2 / 10.5, 27.5 M** |
| AC30 (tol 8) | 16.5 / 17.6, 26.7 M | 22.0 / 23.6, 37.5 M | **7.1 / 9.7, 27.4 M** |

- **The goal is met on the mean:** all four run uncut at 29–47 % of a 9900X core on real material and 41–54 % driven, a
  third to a half under the shipped cut amps and about half the uncut ladder.
- **The p99 block is not:** it is 3–12 % over shipped (17–29 % under the uncut ladder). p99 / mean rises from 1.3 to
  2.1–2.6, because the work now comes in bursts. On real material 3.8 % (Plexi) to 0.5 % (AC30) of base samples substep;
  each costs four solves at 2.0 iterations (BDF2 starts 4.0) against 1.5 for a full step, and the events chain for 15–17
  samples through a pick attack. By §3.5's budget, max(real / 32 %, p99 / 43 % of the period), the Plexi's factor goes
  2.25 → 2.49, the variac's 2.17 → 2.44, the 1959RR's 2.11 → 2.13, the AC30's 1.95 → 2.0 (tol 16) or about 2.2 (tol 8,
  +10 % p99 over tol 16 in the second run); the uncut ladder's was 2.8–3.2.
- **Where the burst could still fall:** substeps cost 2.0 iterations where the 16× ladder's solves at the same step cost
  1.0, because they sit on the knees. A cheaper substep integrator (the TR-BDF2 work) or a cheaper event (the regime-only
  continuation above, −6 % on p99) are the levers; none was taken here at a cost in truth.

**Shipped.**
- `marshall-1959-plexi`, `marshall-1959-plexi-variac`, `marshall-1959rr-91000`, `vox-ac30-tb`: the `kcf` cut removed
  (`cutaudit/scripts/dropcuts.py`), `"multirate": {…, "events": true}`, and `"lte": {"start": "bdf2", "tol": 16}` (AC30:
  tol 16 → 8). The shipped versions are in `work/circuit-netlists/evos/orig/`; reverting one is copying it back and
  regenerating EmbeddedLibrary.hpp.
- EmbeddedLibrary.hpp regenerated; circuit_codegen now emits 244 sparse-lin + 105 sparse-lu + 173 Newton structures (242 /
  105 / 177 before), and every runtime of the four binds a kernel (test_circuit_unit).
- §10.43's C rt table: the four `kcf` rows move to (B). The JC-120's `mi1` + `mi2` stay.

**Switches** (`CE_EVOS` is new; the rest are diagnostics): `CE_EVOS=0` (the ladder at 16× for these netlists, the uncut
reference above) / `1` (events on every ladder-raised group, whatever the netlist says); `CE_EVOS_P`, `CE_EVOS_FC`,
`CE_EVOS_BETA` (interpolator), `CE_EVOS_Q`, `CE_EVOS_DBETA`, `CE_EVOS_DFC`, `CE_EVOS_IIR=1` (decimator), `CE_EVOS_FORCE=1`
(every sample substepped), `CE_EVOS_NOGATE=1`, `CE_EVOS_CONTM=1`, `CE_EVOS_REGIME=1` (the measured alternatives above);
`CE_ALIAS_IDEAL=1` in `circuit_cli alias`.

**Files.** `work/circuit-netlists/evos/`: `scripts/` (`al.sh` gate + ideal alias, `truth3.py` the three references at fixed,
integer and fractional lags, `sweep.sh`, `gate.sh` one netlist's SPICE cases, `summ9.py` the 9900X tables), `data/9900x*`
(the bench outputs), `orig/` and `final/` (the netlists before and after).

**Verification** (Mac, this tree, the four netlists switched):
- test_circuit_engine (all cases, including the 2× multirate Plexi / AC30 against ngspice: 0.060 / 0.253 and 0.262 /
  0.440 %), test_circuit_unit (the codegen binding of the Plexi, 1959RR and AC30 at defaults and unseen knobs), test_effect_handoffs
  (the stream's state is handed over with the circuit's), test_aliasing (48 of 48 gated) and rig_tests pass.
- `bench` output hashes are identical between default, `CE_NOLANES=1` and `CE_NOCODEGEN=1` on the JC-120 and the four units.
- Every other netlist is untouched: the section is inert without the key (no group gets `evos`, `nEvo_` is 0), so their
  arithmetic is the engine's before.

### 10.49 The last aliasing exemptions, and slow supply rails at load (2026-11-14)

Two quality items. test_aliasing exempted the Peavey 5150 (134000, −50.4 dB) and the JCM800 (56000, −59.6 dB after
§10.46) from its −60 dB gate; and the BE-100's bias rail started seconds away from its running level (ENGINE_QUEUE,
2026-11-13), which turned out to be one case of a wider settle problem.

#### Part 1. The 5150 and JCM800 aliasing exemptions

**The alias source.** Both amps' worst spur is the output transformer's flyback, the AC30 / Plexi mechanism of §10.32:
- 5150: 16.2 kHz = harmonic 138 (400.2 kHz) folded about 384 kHz. JCM800: 4.1 kHz = harmonic 131 (379.9 kHz) folded
  about 384 kHz (16.2 kHz, harmonic 138, at tolerance 16). The Uberschall's −58.5 dB is the same 16.2 kHz spur.
- Diagnostic netlists (`circuit_cli alias`, 9900X; dB re the tone):

  | Variant | 5150 | JCM800 |
  |---|---|---|
  | shipped | −50.4 | −59.6 |
  | OT leakage 0 | −52.4 | −60.5 |
  | winding capacitance (CWA/CWB + loss R) removed | −45.2 | −58.2 |
  | CW 1 fF | −45.2 | −58.5 |
  | plate-to-plate CPPW removed | | −61.2 |
  | flyback clamps CR200/CR201 removed | −49.4 | |

  No single element is the source: the edge is the tube cutting off into the leakage, and the winding RC damps it (taking
  it out makes it worse). The content sits at 380–400 kHz, between 2× and 2.1× the 192 kHz internal rate.
- **Why no per-group rate.** In both amps everything after the supply is one Newton group (5150: 93 unknowns, 29 ports;
  JCM800: 55 unknowns, 19 ports). Raising "the power group" is raising the whole amp.

**Every candidate** (alias = `circuit_cli alias` = test_aliasing; truth = `level/scripts/truth.py` against the fine-step
references, NRMSE % full / 20 Hz–20 kHz, `spk` node, noise and hum off, old settle so that the start matches the references):

| Option | 5150 alias | JCM800 alias | Truth (5150 Lead, JCM800 Lead / Rock / 1 kHz sine spk) | CPU, 9900X |
|---|---|---|---|---|
| shipped | −50.4 | −59.6 | 2.016 / 0.489; 0.257 / 0.169, 0.352 / 0.151, 0.317 / 0.078 | see below |
| LTE tolerance (5150 16 → 8 / 4 / 2 / 1.5 / 1.25 / 1 / 0.75 / 0.5) | −49.3 / −50.4 / −52.1 / −52.0 / −54.5 / **−61.0** / −60.9 / −61.7 | | 5150 at 1: 1.048 / 0.226 | |
| LTE tolerance (JCM800 4 → 0.5 / 1 / 2 / 3 / 6 / 8 / 12 / 16 / 32) | | −65.0 / **−66.1** / −58.8 / −57.0 / −58.1 / −59.5 / −64.1 / −60.9 / −57.1 | JCM800 at 1: 0.192 / 0.126, 0.228 / 0.094, 0.201 / **0.117** | |
| fixed step (`CE_LTE=0`) | −48.4 | −64.3 | (much worse truth, §10.46) | |
| backward-Euler start | −50.4 | −57.0 | | |
| whole circuit 8× / 16× | −57.9 / −72.9 | −69.6 / −83.2 | | ×1.4 / ×1.7–1.9 (alias-run CPU) |
| **event-local 16× (§10.48), netlist tolerance** | **−72.0** | **−76.1** | 3.091 / 0.784 (see below); **0.229 / 0.163, 0.207 / 0.111, 0.154 / 0.128** | below |
| event-local 16×, JCM800 tolerance 8 / 16 | | −77.3 / −75.0 | tol 8: 0.251 / 0.181, 0.243 / 0.142, 0.156 / 0.122 | |
| event-local 16×, 5150 tolerance 8 / 4 / 2 | −72.0 / | | 3.109 / 0.607, 3.081 / 0.588, 3.079 / 0.586 | |

- The LTE-tolerance figure is not monotone: the decision to substep or not is itself a time-varying operation on the
  signal, and its seams fold. Only tolerance 1 and below pass on either amp, the 5150 with 1 dB to spare.
- The whole circuit at 16× passes by 13–23 dB at 1.7–1.9× the CPU.
- **Event-local 16× passes by 12–16 dB.** It is §10.48's mechanism unchanged: the group runs at the base rate and every
  LTE event takes 4 substeps whose values feed a band-limited 768 kHz output stream.

**CPU, 9900X** (`bench --table --real`, `CE_REALREPS=3`, noise and hum off; the live tree before and after this section built
side by side; each pair run at once on two core pairs, swapped between two rounds; means of the rounds; % of one core real
/ driven, the combined 64-frame block's mean and p99 in µs; the period is 1333 µs):

| Unit | Shipped | **Events (shipped)** | LTE tolerance 1, no events | Events, tolerance 8 |
|---|---|---|---|---|
| 5150 | 74.4 / 77.4 %, 1012, 1947 | **81.9 / 85.9 %, 1118, 2211 (+10 / +11 %, p99 +14 %)** | 88.8 / 93.0 %, 1212, 2421 (+19 / +20 %) | 88.2 / 93.0 %, 1208, 2402 |
| JCM800 | 36.8 / 46.9 %, 558, 1325 | **41.3 / 51.5 %, 618, 1406 (+12 / +10 %, p99 +6 %)** | 44.5 / 53.9 %, 656, 1438 (+21 / +15 %) | 39.1 / 51.5 %, 604, 1455 |

- The tolerance-1 and tolerance-8 columns ran in the second set of pairs, whose neighbour load differed; events at
  tolerance 16 measured 86.7 / 91.3 % for the 5150 there. Within a pair, events cost less than tolerance 1 on both amps.
- **The cheapest change that passes, at equal or better truth, is events on both amps.** On the JCM800 it is cheaper than
  tolerance 1 and 10 dB further under the gate. On the 5150 nothing cheaper passes: tolerance 1 passes by 1 dB at about twice
  the added cost.
- The 5150 already missed its 64-frame deadline at p99 (146 % of the period), and now misses it by more (166 %). Its mean
  stays under one core.

**Truth with events, node by node** (Lead, % full / audio):
- JCM800: every guitar reference is closer (above; `pa`, `p4a`, `kcf`, `p3` likewise). The 1 kHz sine's audio band reads
  0.078 → 0.128 while its full band halves (0.317 → 0.154). The engine at LTE tolerance 1 and 0.5 shows the same audio-band
  figure (0.117, 0.098): the shipped 0.078 is a cancellation of the tolerance-4 trapezoid's errors on that one tone, not a
  closer waveform.
- 5150: every internal node is closer (`p5a` 0.135 / 0.088 → 0.090 / 0.062, `p4a` 0.319 / 0.270 → 0.158 / 0.137, `k3b`
  0.298 / 0.192 → 0.205 / 0.104, `p1a` 0.020 / 0.007 → 0.006 / 0.007), and closer still at tolerance 8. The speaker node
  reads further (2.016 / 0.489 → 3.091 / 0.784), and tightening the tolerance does not move it (3.08 / 0.59 at 2), while
  every node before the output stream converges. The reference rows are point samples, at 192 kHz, of the fine-step
  waveform, so they carry its content above 96 kHz folded into the band: the very aliasing the output stream removes. The
  5150's speaker node carries the most of it (3 % full band, against 0.3 % on the JCM800). A best-lag scan puts the
  minimum at exactly the stream's 8 samples, so it is not misalignment.
- SPICE gate: unchanged. Its cases compile without rate levels, so the events key does nothing there.

**Shipped.**
- `peavey-5150-134000`: `"multirate": {"rates": {"V5": 16}, "events": true}`.
- `marshall-jcm800-2203`: `"rates": {"VPA": 16}, "events": true` added to its multirate block.
- Originals in `work/circuit-netlists/qfix/orig/`.
- test_aliasing gates both. The Uberschall stays reported, not gated: with events it reads −61.6 dB at its tolerance 8
  (−59.0 at 16), too thin a margin to take without its own truth and CPU study, and it is a time-varying unit (its slow
  LDR partition) for the gate anyway.
- The output of both amps is now 8 base samples (42 µs at 48 kHz) later, as on the Plexi family.

#### Part 2. Slow supply rails at load

**The finding.** A drift survey: every bound unit through `circuit_cli` at its unit settings (compile, then the unit's two
settles at 450 ms), then 6 s of silence, every node's mean over each 60 Hz period. Most amps were not at their running
state when the unit loaded. The largest node offset at load that is gone by 6 s (`level`-style 0.25 s windows; mains-side
and oscillator nodes left out):

| Unit | Before | After |
|---|---|---|
| JCM800 | 33.8 V (`bp5` 246 → 280 V, 5 s) | 0.04 V |
| Plexi / variac / 1959RR | 29.8 / 22.6 / 29.7 V (`bp5`) | 0.03 / 0.02 / 0.05 V |
| BE-100 | 17.2 V (screens; `bpl` 17 V, `bp1` 10 V, bias `b2` 6.5 V) | 0.39 V |
| SLO-100 MK II | 14.4 V (`n2`) | 14.3 V (falls back, below) |
| OR120 | 11.2 V (`d`) | 11.0 V (falls back) |
| Uberschall | 9.8 V (`bz`) | 0.00 V |
| AC15 | 8.2 V (`p4a`) | 0.03 V |
| Dual Rectifier / Rev C | 8.1 / 7.5 V (`e`) | 0.06 / 0.03 V |
| Ecstasy | 7.6 V (`hvF`) | 0.01 V |
| 5150 | 7.3 V (`p2a`) | 0.00 V |
| DR103 | 6.8 V (`rl`) | 0.00 V |
| SLO-100 | 6.2 V (`p3b`) | 0.00 V |
| Supro 1624T | 3.4 V (`bpc`) | 3.3 V (its tremolo's load; the supply itself is settled) |
| 5E3 / Deluxe / AC30 | 2.8 / 2.7 / 2.1 V | 0.00 V |
| RE-201 / 6G15 / Twin / 5F6A | 1.1 / 0.85 / 0.84 / 0.61 V | 0.00 / 0.11 / 0.00 / 0.02 V |
| SVT, JC-120 (transformer-fed, settlePeriodic) | 0.00 V | unchanged |

**The two causes.**
1. **The run's stopping test measures speed, not distance.** It ends when every supply cut's per-period mean moves by less
   than max(10 mV, 1e-3 of itself) per period. A rail behind 1–2 s of RC (decoupling chains, the BE-100's 33 kΩ / 22 µF
   FX-loop rail) passes that test at 450 ms while 15–35 V away; a rail inside a group (the Ecstasy's and OR120's chains)
   is not a cut and is not watched at all. Step 2 then holds the supply groups where they are.
2. **Step 2 solved the BE-100's bias rectifier at DC.** Its group (RS3, R58, D9, C40, R57) owns no supply cut, so step 2
   re-solved it at DC with the bias winding frozen at the period boundary. That source is at 270°, so the boundary is its
   negative crest, and the DC point charged C40 through the crest divider (−85 V); after the 60 ms tail `b1` started at
   −59.6 V instead of −46.2 V, and `b2` at −43.4 V instead of −36.9 V.

**The fix** (`Engine.cpp`, `Circuit::settleSlowRails` and `reset()`'s mains-fed branch; `CE_SLOWSETTLE=0` is the settle
before, byte for byte):
- Where the netlist's supply has an averaged circuit, the run ends at its 450 ms. The averaged circuit is
  `mainsAveragedNetlist` with capacitors as the mains side's boundary (they are open at DC), sources controlled from the
  mains as part of it (the BE-100's Norton windings), and a mains-side node with a capacitor into the rest held like a
  rectifier output (the BE-100's split reservoir centre).
- `settleSlowRails` follows when a rail the run watches (the supply cuts and, now, the rectifier outputs) is still more
  than its tolerance from its running level by the geometric tail of its last three period means, or when the circuit
  has no supply cut to watch (the Ecstasy: its whole supply is inside one group). Where the rails have arrived nothing
  more runs. The JC-120's averaged circuit has no DC point, and each failed try costs about 2 s; its rails arrive, so it
  is not tried.
- **settlePeriodic's step 1, for a supply fed straight from mains sources.** Each step measures every state's mean over a
  period, solves the averaged circuit at DC with the rectifier outputs pinned, and moves every state it shares by (DC value
  − mean), keeping the ripple. That puts every decoupling chain, filtered and bias rail, and every consumer on them, at its
  running level at once, whatever its time constant.
- **The pins.** A reservoir straight on a bridge is at its running mean within a period; a bias rectifier behind 27 kΩ is
  not (seconds). Each pin is the geometric tail of the output's last three period means after the step (4 periods), and,
  once the pins' own sequence decays monotonically, the tail of that sequence. The BE-100's `b1` goes −64.6, −56.2, … and
  lands within 0.4 V of −46.2 V.
- Done when the pins hold to max(10 mV, 1e-3) and no state moves by more than 1e-3 of the largest capacitor voltage.
  Where step 2 follows, the measure is the supply groups' states only (step 2 re-solves the rest), and a transformer's core
  is left out of it (its DC point and its running mean differ by a constant it returns to). At most 12 steps.
- **If the moves never shrink,** the state before is restored and the run goes on exactly as before (the OR120's and
  SLO-100 MK II's stacked supplies, which this averaged circuit does not describe: a 220–260 V move every step; the
  cause was their series rectifier diodes, and §10.56 fixes it).
- **Step 2 now also holds every group on the mains side** (cause 2).
- The rectifier outputs are exported at compile, so the unit (which exports only the nodes it reads) sees their voltages.
- **WDF groups** keep their state in the tree, not in `vcLast` / `w` (those stay at the compile's values). The settle reads
  it out (`WdfGroup::stateTo`, the inverse of `resetFrom`) before using the arrays and writes it back after. Note for
  `settlePeriodic` and `adoptStateByName`: both read a WDF group's arrays as if current; none of the transformer-fed units
  has a WDF group today, so nothing changed there.
- The group holding a unit's LFO (`readouts.lfo`, now parsed as `Netlist::lfoNode`) keeps its running state: its DC point
  is the oscillator's unstable equilibrium. Moved, the Supro's slow tremolo did not restart within the test's 2.5 s.
  `readouts` now survives the netlist's JSON round trip (`Netlist::readouts`), so a unit's edit path keeps it: before, an
  edited Twin, Deluxe, Supro or AC15 lost its LFO readout, and with this settle it also settled differently from the
  unedited unit (test_circuit_unit's round trip).
- Every moved state starts the predictor and the LTE history afresh. A BDF2 substep start through a pre-move sample reads
  the move as a current.

**Measured and rejected.**
- Minimal polynomial extrapolation of the period map (settlePeriodic's step 2) over the supply groups alone diverged on
  every unit: the decoupling chains' slow modes couple to the consumers, which it does not see.
- The same over the whole state converged on the JCM800 in 4 cycles of 17 periods, but not on the BE-100. Its open switch
  contacts leave capacitors on 1 GΩ (100 s modes), its WDF group's arrays were stale, and the extrapolation's coefficients
  ran to ±16 000.
- A longer run: the slowest rails need 4–5 s, three to five times the load time.

**Load time** (9900X, one core, `taskset`; one channel: compile and the unit's two settles, the compile's own and the one after
`setControlImmediate`, which is what a unit does per channel, two channels in parallel; unit exports; min of 3, seconds;
the trees before and after this section, both without §10.48's netlist changes):

| Unit | Before | After | | Unit | Before | After |
|---|---|---|---|---|---|---|
| BE-100 | 2.13 | **1.77** | | Twin | 0.35 | 0.45 |
| AC30 | 1.07 | **0.90** | | Deluxe | 0.51 | 0.51 |
| Uberschall | 0.87 | **0.67** | | 5F6A | 0.27 | 0.32 |
| Dual Rectifier Rev C | 0.73 | **0.68** | | 5E3 / 6G15 | 0.14 / 0.08 | 0.13 / 0.12 |
| JC-120 | 0.74 | 0.76 | | DR103 | 0.36 | 0.41 |
| 1959RR | 0.69 | 0.82 | | Supro | 0.23 | 0.29 |
| Plexi / variac | 0.68 / 0.74 | 0.76 / 0.77 | | SLO-100 | 0.44 | 0.57 |
| 5150 | 0.43 | 0.58 | | AC15 | 0.45 | 0.60 |
| JCM800 | 0.28 | 0.35 | | RE-201 | 0.23 | 0.30 |
| Ecstasy | 0.41 | 0.74 | | Dual Rectifier | 0.39 | 0.42 |
| SLO-100 MK II | 0.44 | 0.72 (falls back) | | OR120 | 0.21 | 0.48 (falls back) |
| SVT, Leslie, LA-2A, Uni-Vibe | unchanged within 0.01 | | | | | |

- The slow units load faster: they no longer run up to the 2 s limit. Most others take 0.05–0.15 s longer: 2–6 steps of
  four periods each, and one helper compile.
- The Ecstasy costs 0.33 s, because it has no supply cut to watch, so the steps always run.
- The OR120 and SLO MK II cost about 0.27 s for steps that are then undone. Remembering the failure between a circuit's
  two settles would halve that.
- The two settles per channel are the unit's own (`CircuitUnit::buildChannel` compiles at the unit's controls, then sets
  the same controls and resets). Dropping the second, when nothing changed, would halve every amp's load time; that is a
  separate change.

**Verification** (Mac, `app/build-qfix`, the live tree with this section):
- test_aliasing: 51 of 51 gated below −60 dB (48 before, plus the two JCM800 bindings and the 5150). The JCM800 reads
  −77.0 dB and the 5150 −71.5 dB; the Uberschall −58.2 dB is reported.
- test_circuit_engine: every case passes, including the Supro tremolo (speed 0 / 1 = 2.93 / 5.26 Hz, depth 55.8 dB) and
  the RE-201's PS-15D rails (17.68 / 10.61 V).
  - The three Ecstasy cases with a "settled" start (`xtc_clean`, `xtc_ch2_small`, `xtc_ch3_drive`) start ngspice from the
    engine's settled state, so their decks follow the new settle. They were regenerated with ngspice 47
    (`HEADROOM_NGSPICE=1 HEADROOM_NGSPICE_ONLY=…`). Every node reads within 0.003 % of its old figure, except
    `xtc_ch2_small p2a` (0.082 → 0.193 %, gate 1 %). The old decks are in `work/circuit-netlists/qfix/fixtures-orig/`.
  - With `CE_SLOWSETTLE=0`, those three cases now fail against the new decks; that switch is a developer A/B, not a
    supported configuration.
- test_circuit_unit (including the edit-path round trip and the codegen kernel binding), test_effect_handoffs and
  rig_tests pass.
- **Lanes and codegen bit-identity** (9900X): the `bench --real` hashes are identical with default, `CE_NOLANES=1` and
  `CE_NOCODEGEN=1` on the JCM800, 5150, JC-120 and BE-100.
- EmbeddedLibrary.hpp is regenerated; the codegen kernels regenerate with the build.

**Switches.**
- `CE_SLOWSETTLE=0` turns off the slow-rail settle, the mains-side hold, the rectifier-output exports and the early end of
  the run, giving the settle before.
- `CE_PSS_DEBUG=1` prints the steps.

**Files.**
- `work/circuit-netlists/qfix/orig/`: the two netlists as shipped.
- `work/circuit-netlists/qfix/fixtures-orig/`: the old Ecstasy decks.
- `work/circuit-netlists/qfix/scripts/`:
  - `drift.cpp` is `circuit_cli` plus a `drift` command: 6 s of silence after the unit's settle, per-period node means,
    and the build-and-settle time;
  - `dan.py` is the survey's analysis;
  - `truth_q.py` is `level/scripts/truth.py` with a library override and an output-lag override.

### 10.50 JC-120: where its Newton iterations go, the `mi1` / `mi2` cut at uncut accuracy, and per-circuit Newton hints (2026-11-14)

**The question.** The JC-120 (49000) ran at about 72 / 90 % of a 9900X core (real / driven), with a 64-frame p99 of about
2105 µs (1.95× the mean). §10.46 put its peaks on Newton iterations under drive, not on LTE. It also shipped a lossy cut:
`mi1` + `mi2` (§10.43, jc120_dimed 1.46 % cut against 0.03 % uncut). The task: which groups iterate on driven input and
why; whether a lossless fix exists; a fine-step truth reference; then whether `mi1` / `mi2` can go.

**Truth references (new).** `work/circuit-netlists/jcspike/refs/`, 2 s each at 96 kHz: the lead DI (real, 0.35 V peak),
the same excerpt through the bench's drive stage (lp 6 kHz of tanh 12x), and 2 s of the rock DI driven.
- Table 49000 with `em=0` and `reverb=0`, as the SPICE gate: a deck carries no BBD or spring signal.
- Each starts from the engine's settled state, with the mains phase matched (`scripts/mkjc.sh`).
- **The deck needed fixing first.** `--ic-settled` leaves out the nodes the engine does not have: each op-amp's swing node
  `n_<IC>_x`, the merged D4/D5 midpoint and the tank coil node. ngspice then stalls at the first step ("trouble with jq1";
  without `uic`, at `eic5b#branch`). `scripts/fixic.py` adds them, and sets the BBD output source to the engine's DC level.
- **Trap stalls on x86.** ngspice-47 trap stalls at t = T on these decks on the Linux build machine, while the same deck runs on
  the Mac's arm64 build. So the references are Gear (BDF2) at T/32 on the Linux build machine. They agree with trap at T/16 on the
  Mac to 0.0008 % (spkL), 0.0004 % (spkR) and at most 0.0021 % on every node.
- Nodes: spkL spkR mi1 mi2 dmix c15 d5 gq10 c25 c84p tri p37. `scripts/jctruth.py` renders them through the unit path.

**Where the iterations go.** Per-group profile (Mac Newton counts, 9900X times, sequential), and a census of the port
that blocked convergence at each iteration (`diag-instrumentation.patch`, `CE_NKDIAG=1`):

| Group (lane) | Role | Newton it/sample real → driven: full step, substeps | ns/sample real → driven | Blocking ports |
|---|---|---|---|---|
| 0 (A) | supply, regulators | 1.77 → 1.88, 0 | 1092 → 1126 | D10, D4_1 (rail-feed diodes), D2PS (LED), bridge |
| 1 (A) | CH-2 preamp | 1.57 → 2.35, 0.01 → 0.36 | 1830 → 2619 | D1 (pair), Q15 / Q16 (feedback pair saturating) |
| 6 (A) | mixer | 1.30 → 1.60, 0 | 1297 → 1418 | D6, Q5_1, Q6 |
| 12 (A) | left power amp | 2.22 → 2.25, 0.33 → 1.42 | 1552 → 1955 | Q14_1, Q10_1 (output stage), Q9_1, Q13_1 |
| 13 (A) | LFO + Q25 envelope detector | 2.73 → 2.66, 1.06 → 1.07 | 1450 → 1430 | Q25 (b-c), D11, D12 |
| 14 (A) | LFO buffer Q24 | 1.14 → 1.19, 0 | 276 → 278 | Q24 |
| 8 (B) | right power amp | 2.23 → 2.46, 0.23 → 1.52 | 1492 → 2062 | Q29_2, Q18_2, Q17_2, Q20_2 |
| 9 (B) | reverb driver | 2.27 → 2.41, 0.85 → 1.83 | 720 → 999 | Q7 / Q8 push-pull |
| 4 (B) | BBD + clock buffer Q23 | 1.98 → 2.07, 0 | 245 → 251 | Q23 (b-c) |
| 5, 10 (B) | IC2 op-amps (rail clamps) | 1.75 → 2.26, 1.82 → 2.64 | 152 → 219, 165 → 263 | the clamp ports |

- **Transistor knees, at the tolerance.** On an audio-rate signal the linear predictor misses a forward junction by about
  nVt·(ωh)², around 1e-4 V, against a tolerance of 1.6 µV. So most samples take two iterations whatever the start: the
  first step lands quadratically close, the second confirms it. Limiting is not the issue: under 0.03 limited steps per
  port per sample on full steps.
- **Under drive** the extra work is the preamp's Q15 / D1 saturation (+0.8 full-step and +0.35 substep iterations a sample)
  and the power amps' LTE substeps (+1.1 to +1.3 a sample).
- **Rail clamps** sit only in the op-amp groups 3, 5, 9 and 10, all on lane B. The clamp-free solve falls through 0.012–0.016
  times a sample on driven, for 0.2–0.26 µs. Not on the critical path.
- **The BBD clock path** (group 4) is 2.0 iterations a sample on lane B, about 0.25 µs.
- **The chorus LFO group** costs as much as a power amp. Its Q25 envelope detector, fed from spkL through R146, switches on
  every cycle of the output: 2.7 full-step plus 1.1 substep iterations a sample, on real and driven alike.

**Where p99 comes from.** Per block and per group (`CE_PROFILE` times, the fastest of three runs, 9900X; `scripts/bganal.py`):
- The critical lane's p99 / mean is 1.60 real and 1.88 driven.
- In the slowest 1 % of driven blocks the extra time is group 1 (+522 µs, ×2.6), group 12 (+368 µs, ×2.5), group 13
  (+175 µs, ×2.0) and group 6 (+60 µs). The supply and the small groups stay within ×1.2.
- So the peaks are the preamp and the left amp at the knees (iterations and substeps), plus the envelope detector.

**The lane split cannot improve exactly.**
- **The chain is a cycle.** In-sample reads run 0 → 1 → 6 → 12 → 13 → 14 (13 reads `spkL`; 14 reads `tri`). Groups 13 and
  14 draw on the `v15` cut, so group 0 at the next sample needs them. Every sample therefore costs 0 + 1 + 6 + 12 + 13 + 14,
  about 7.5 of the 10.5 µs (real). Lane B (3.0 µs) waits.
- **Breaking it means a delay cut on `spkL` → R146.** Planned, it is 13.0 µs estimated against 14.5. But it moves the
  envelope detector a sample late:
  - c25 truth goes from 1.49 to 11.3 %;
  - in Chorus the LFO hold timing shifts, for a spkR null of −63 dB against shipped;
  - the shipped `spkL` cut nulls at −100 dB or better.
  Rejected (`orig/variant-spkL-delay.circuit.json`).

**Change 1: `mi1` / `mi2` without the 100 kΩ loads.** The shipped cuts listed `["C15_1", "R22_1"]` and `["C44", "R53"]` as
loads.
- **Why that was lossy.** R22_1 / R53 (100 kΩ to ground) are each node's only resistive path. In the mixer's solve the node
  is otherwise held only through the 1 µF coupling capacitors C12_1 / C25 by the CH-1 follower and the mixer. So its low-frequency mode ran on a
  one-sample-late current: mi1's full-band error was up to 12.5 % on the Rock reference (1.0 % in the audio band).
- **The fix.** The loads are now the 82 nF coupling capacitors alone, and the resistors solve with the mixer. Groups, ports,
  lane plan and cost are unchanged.

| Truth (NRMSE %, spkL / spkR) | shipped | **loads without R22_1 / R53** | uncut |
|---|---|---|---|
| jc120_dimed | 1.448 / 1.469 | **0.027 / 0.026** | 0.026 / 0.025 |
| jc120_drive | 0.784 / 0.779 | **0.039 / 0.038** | 0.038 / 0.037 |
| jc120_dist | 0.990 / 0.968 | **0.048 / 0.051** | 0.047 / 0.051 |
| jc120_clean | 0.211 / 0.181 | **0.092 / 0.096** | 0.079 / 0.085 |
| jc120_chorus spkL (tri) | 0.268 (0.322) | **0.069 (0.365)** | 0.079 (0.363) |
| jc120_ac 20 Hz–5 kHz / 5–10 kHz (dB) | 0.062 / 0.142 | **0.021 / 0.154** | 0.021 / 0.151 |
| Lead T/32, full band | 0.568 / 0.575 | **0.143 / 0.150** | 0.140 / 0.146 |
| Lead driven T/32 | 1.007 / 1.064 | **0.365 / 0.374** | 0.369 / 0.388 |
| Rock driven T/32 | 1.542 / 1.561 | **1.085 / 1.072** | 1.088 / 1.079 |

- **Every output reads within 0.013 pp and 0.003 dB of the uncut circuit.** The two figures that moved the other way
  (chorus `tri`, AC 5–10 kHz) read the same uncut. The shipped cut was closer there only by cancellation.
- **Null against uncut** (unit path): −45 → −74 dB real, −40 → −60 dB driven.
- **c25 and c84p** (internal) improve 1.49 → 0.54 % and 0.37 → 0.19 %.

**Change 2: per-circuit Newton hints** (`"newton": {"chordGate", "quadPorts", "subPred", "subPredPorts"}`; Netlist.hpp,
Engine.cpp "Newton hints per circuit", `KernelState::chordGate`).
- **What moves.** Only Newton's start and its choice between a chord and a fresh factorisation. The equations, tolerances and
  acceptance test (step within tolerance, residual gate) are the same.
- **What each sets.** The netlist replaces the in-solve chord gate (30), the quadratic predictor's port floor (24), and the
  substep predictor's bits and port floor (30, 19).
- **The JC-120's values.** 300 / 4 / 26 / 4:
  - a chord confirms the second iteration wherever the first step was within 300 tolerances;
  - the quadratic predictor runs above 4 ports;
  - the substep predictor runs from 4 ports, without its grid-quadratic bit 4, which §10.46 measured as the bit that cost
    the JC-120.
- **Switches.** Every `CE_CHORDGATE` / `CE_PREDQPORTS` / `CE_SUBPRED` / `CE_SUBPREDPORTS` still wins. `CE_NONEWTONHINTS=1`
  ignores the hints: hashes equal to the engine before.
- **Bit identity.** Generated kernels on and off (`CE_NOCODEGEN`) give the same hashes, and so do lanes on and off.
- **Truth and gate.** The fine-step references are unchanged to four decimals; the SPICE gate to three.
- **Iterations.** Newton per sample goes 24.75 → 23.62 real and 31.33 → 29.90 driven; homotopies 42 → 38.

| Mac retired instructions (sequential), real / driven G/s, p99 block | |
|---|---|
| shipped | 15.76 / 19.33, 45.3 M |
| chord gate 300 alone | 15.23 / 18.61, 43.6 M |
| + quadratic predictor > 4 ports | 14.94 / 18.34, 43.8 M |
| **+ substep predictor 26 from 4 ports (landed)** | **14.89 / 18.28, 43.4 M** (−5.5 / −5.4 / −4.2 %) |
| chord gate 100; 1e9 | 15.41 / 18.87; 15.39 / 18.93 |
| chord tried while the device Jacobian moved ≤ 0.01–1 (xchord's bound) | within 0.4 % of shipped |

**`mi1` / `mi2` removal, re-evaluated.**
- Uncut is one 52-port group of mixer and both power amps, so no lane split: 124.9–126.9 / 176.4–178.8 % on the 9900X (with
  the hints), p99 4655–4734 µs.
- With change 1 the cut buys nothing back by going. The cuts stay, now at uncut accuracy.
- §10.43's "C rt" row for the JC-120 is closed.

**CPU.** 9900X, `bench --table 49000 --real`, `CE_REALREPS=3`, noise and hum off. Shipped and new ran side by side on two
pinned core pairs, rotated over three rounds; the ranges are over the rounds.

| JC-120 | real % | driven % | p99 µs | mean µs | p99 / mean |
|---|---|---|---|---|---|
| shipped | 75.0–75.4 | 93.5–94.4 | 2160–2184 | 1123–1132 | 1.93 |
| **§10.50** | **69.8–71.0** | **88.3–89.2** | **2071–2092** | **1054–1068** | **1.97** |
| change | −6.6 % | −5.4 % | −4.3 % | −6.0 % | |

- **The ratio rose** because the mean fell more than the peaks.
- **The peaks** are the knee iterations and substeps of groups 1 and 12 on the critical cycle, and §10.47 found no cheaper
  substep scheme at equal truth.
- **Still more than one core.** The JC-120 stays above one core's budget at p99 (156 % of the 1333 µs period): the cycle
  (0 → 1 → 6 → 12 → 13 → 14) is what is left.

**Measured and rejected:**
- the `spkL` delay cut (above);
- the Jacobian-change chord criterion (`CE_JCCHORDDJ` in the diagnostic patch, ≈ 0);
- chord gates of 100 and 1e9;
- the quadratic predictor alone (p99 +0.7 %).

**Verification** (Mac, shared tree):
- **Pass:** test_circuit_unit, including the codegen kernel checks (every JC-120 group binds its generated kernels),
  test_effect_handoffs, test_aliasing and rig_tests.
- **test_circuit_engine:** every jc120 case reads as in the table. Its three failures (Supro tremolo ×2, RE-201 PS-15D
  rails) come from the mains-settle work in progress: they pass with `CE_SLOWSETTLE=0`, and fail the same with
  `CE_NONEWTONHINTS=1`.
- **Lanes bit-identity:** `bench --real` hashes are the same with lanes on and off, with noise off and with noise on at a
  fixed seed.
- EmbeddedLibrary.hpp is regenerated.

**Files** (`work/circuit-netlists/jcspike/`):
- `refs/` (`raw/` keeps the ngspice logs);
- `scripts/` (deck, truth, null, census and bench tools);
- `engine-newton-hints.patch`;
- `diag-instrumentation.patch`;
- `orig/`, the shipped netlist.

### 10.51 Two exact levers re-examined in the generated kernels: nodal charge states (nothing left to remove) and two-lane tube laws (landed, bit-identical) (2026-11-14)

**The task.** Two exact CPU levers had been listed but never built: nodal charge states (the shoot-out notes §2 idea 6,
CPU_THEORY R6.2: drop the 17–23 % of trapezoidal "artefact" state modes, estimated 1.03–1.08×) and two-lane device
evaluation (idea 5, COST_BREAKDOWN T6: two tubes per SIMD pass with interleaved tables, measured −1.3 % once, before
§10.44 / §10.45). Both were to be re-judged against today's engine, where §10.44's sparse linear update and §10.45's
generated Newton kernels changed what the per-sample work is.

#### Lever 1: nodal charge states, not built

**What the modes are, exactly.** A group's solve sees its capacitor states only through `A·s`: each capacitor stamps
±1 into the KCL rows of its terminals (ground, known and input nodes stamp nothing). In a capacitor loop the columns are
dependent, so the loop's circulating combination never reaches a node voltage. These are R6.2's eigenvalue −1 modes.
They can be removed exactly without any balancing:
- take a spanning forest of the eligible capacitors over the vertices {KCL unknowns} ∪ {one datum};
- each co-tree capacitor's column equals Σ γ_t · (tree column t), with γ = ±1 along the forest path between its ends;
- keep the co-tree states out of the matrices, and fold G2·v of each co-tree capacitor into its path's tree states every
  sample (Σ|γ| adds).

Eligible capacitors are trapezoidal (not `be`), not read one by one elsewhere (cut terms, a core's flux node). LTE, the
substep mapping and hand-offs read capacitor voltages, or are linear with coefficient 1 in the state, so they would have
carried over.

**Measured structure.** `nodalPlan` builds that forest per group, breadth-first from the datum, so the loops are as
short as possible. `CE_NODALINFO=1` prints, per group, after reset has chosen the linear update: its kind, states,
loops, the loops' R nonzeros and the fold's adds. For the full-step runtime of each heavy unit's big group:

| Unit | Group: states, loops | Linear update | R state nonzeros (in loops) | Fold adds | Saving, sparse / if dense (multiply-adds per update) |
|---|---|---|---|---|---|
| 5150 | 87, 17 (20 %) | sparse | 158 (34) | 36 | 34 / 2890 |
| Uberschall | 96, 19 (20 %) | sparse | 174 (38) | 34 | 38 / 3496 |
| BE-100 | 91, 16 (18 %) | sparse | 161 (30) | 39 | 30 / 2944 |
| SLO-100 | 71, 12 (17 %) | sparse | 125 (24) | 24 | 24 / 1716 |
| SLO-100 MkII | 73, 12 (16 %) | sparse | 136 (24) | 25 | 24 / 1848 |
| Dual Rectifier | 97, 11 (11 %) | sparse | 172 (22) | 23 | 22 / 2332 |
| Plexi | 49, 9 (18 %) | sparse | 85 (18) | 23 | 18 / 765 |
| JCM800 | 50, 9 (18 %) | sparse | 89 (18) | 19 | 18 / 810 |
| Twin | 60, 12 (20 %) | sparse | 107 (24) | 30 | 24 / 1308 |
| AC30 | 46, 8 (17 %) | sparse | 82 (16) | 17 | 16 / 680 |
| JC-120 (groups 2, 8) | 44 / 32, 3 / 4 | sparse | 80 / 56 (6 / 8) | 5 / 7 | 6 / 8 |

The loop counts reproduce R6.2: the 5150's 17 loops plus its three inductor modes are R6.2's 20.

**Why it no longer pays.**
- R6.2 and the shoot-out notes derived 1.03–1.08× from the dense linear update. The DR103 went from 5.9k to 4.4k multiply-adds there,
  because a dense update pays nn + rows multiply-adds per state column.
- Since §10.44, every big group of every heavy unit runs the sparse update (`SparseLin`, its generated kernel). There a
  state column costs its R nonzeros in the right-hand-side scatter, once per update, because linOut reuses linPorts'
  `bPart`. Every loop found is two nonzeros: an interelectrode or bypass triangle between two KCL nodes.
- So on the 5150, removing all 17 loops saves 34 of the group's ~1436 sparse operations (2.4 % of a linear update that
  is 5–7 % of the unit, §10.45). Keeping the loop states consistent costs 36 adds a sample. The net is zero or slightly
  negative on every unit above.
- T10 (port-defined rows) had already taken the loop capacitors' output rows away.
- Where it would still pay is dense groups, and those are small: supply groups and the Twin's group 1 (4 loops, 104
  multiply-adds a sample).
- An attempted upper-bound probe (the co-tree columns simply not stamped, ignoring the fold) is no measurement: without
  the fold the circuit is a different one (5150: LTE substeps every sample, 9.9 against 4.7 iterations a sample). The bound
  above is the exact operation count of the generated kernel.

**Decision.** Not built. The full implementation would touch the state update, the substep mapping (BE / BDF2 starts), the
glide and Sherman–Morrison state corrections and the hand-offs, and save nothing measurable. `nodalPlan` / `nodalInfo`
(Engine.cpp, section "Nodal charge states") stay as the diagnostic behind `CE_NODALINFO=1`. Without the variable the
compile does no work there.

#### Lever 2: two-lane tube laws, landed

**Where the time is.** Device evaluation is about a fifth of Newton time (§10.45), and the triode and pentode table laws
are most of it on the tube amps. The static kernels already inline every device as straight-line code, so the core
overlaps independent devices. What is left is the laws' own instructions and latency: a triode does three table lookups
(grid, plate A/C, z), each a division, an index, two knots and two cubic Hermite evaluations.

**Design.**
1. **Interleaved knots (T6).** `KorenTables` also holds each knot's values together, 4 doubles a knot:
   - `ac`: {a, c, a′, c′};
   - `gK`: {g, g′, g″};
   - `hK`: {h, h′, h″};
   - `atK`: {atan, atan′, atan″}.

   A lookup's two knots are one or two cache lines instead of four to six (`korenTables`, Engine.cpp).
2. **Two lanes per device (`evalTriodeLut2`, `evalPentodeLut2`, Devices.hpp).** The Hermite evaluations that share a
   knot interval run as one 2-wide vector, with operands loaded straight from the interleaved knot:
   - (A, C) and (A′, C′) on the plate tables;
   - (Ig, gg) on the grid table: y = (g, g′) and m = (g′, g″) are adjacent;
   - (h, h′) on the z table;
   - the pentode's (atan, atan′).
3. **Two devices per pass (`evalTriodePair`, `evalPentodePair`; `nk::NkPairs`, NewtonKernel.hpp).** In a static spec,
   consecutive triodes and consecutive pentodes are paired at compile time from `G::kind`. The leader evaluates both, and
   the follower's slot is skipped. The lanes are the two devices, each with its own tables and constants. The divisions,
   the z / plate algebra and the final products run 2-wide over the pair, and the knot pairs as in 2. When either lane
   leaves a table range (cut-off, grid past GMAX, plate under 4 V), both take the one-device path. The interpreter
   (DynSpec, `CE_NOCODEGEN`) evaluates one device at a time with the lanes of 2, and gives the same results.

**Why it is bit-identical.**
- Each lane computes the scalar code's expression, with the same text on vector operands: `hermiteL` is `hermite` with
  y / m as vectors and the basis on the scalar t. So clang's `fmuladd` contraction makes the same fusions per lane,
  and IEEE arithmetic rounds each lane as the scalar code rounds.
- The out-of-range branches are the scalar code's.
- The pairs' range tests choose between two bit-identical paths.

**Junction pairs: not done.** A diode or BJT junction's cost is its `std::exp` (libm). A vector exp is not bit-identical to
libm, and the arithmetic around it is a few operations.

**Proof.**
- `bench` hashes (0.4 s chord, table defaults):
  - on the 9900X, base, new and `CE_NOTUBELANES=1` match on the 11 heavy units;
  - on the M4 Pro, new, `CE_NOTUBEPAIRS=1`, `CE_NOTUBELANES=1` and `CE_NOCODEGEN=1` match on the 5150, JCM800, Twin and
    BE-100.
- `bench --real` hashes (real and driven, 24 s each) match on all 11 units:
  - on the 9900X: base, new and new without pairs, in every round;
  - on the M4 Pro: base binary against new binary.
- Every library netlist on the 9900X (bench 0.2 s, netlist defaults): base, new, `CE_NOTUBELANES=1` and `CE_NOCODEGEN=1`
  match on 91 of 91.
- Lanes: `CE_NOLANES=1` against default on the JC-120 (two lanes engaged) and the tube units, identical.

**CPU, M4 Pro (retired instructions per audio second, `bench --real`, noise and hum off, base binary against new
binary; load-independent).**

| Unit | real | driven |
|---|---|---|
| 5150 | −2.2 % | −1.7 % |
| Uberschall | −2.1 % | −1.6 % |
| BE-100 | −1.0 % | −1.0 % |
| SLO-100 | −2.2 % | −2.1 % |
| SLO-100 MkII | −1.9 % | −1.8 % |
| Dual Rectifier | −2.1 % | −2.0 % |
| Plexi | −2.4 % | −2.0 % |
| JCM800 | −2.0 % | −1.7 % |
| Twin | −1.7 % | −2.2 % |
| AC30 | −1.3 % | −1.2 % |
| JC-120 (no tubes; lanes on, so its count moves with the helper's waits) | −1.1 % | −1.1 % |

Within one binary, against `CE_NOTUBELANES=1`, the counts fall 1.1–3.8 %, of which the pairs are 1.2–2.1 %. The switched-off
path pays for its switch tests, so the base binary is the honest reference.

**CPU, 9900X.** `taskset` on three core pairs rotated over three interleaved rounds, `nice -n 10`,
`CE_REALREPS=3`, noise and hum off. Columns: real / driven % of one core, combined p99 of the 64-frame blocks (µs), mean
block (µs). The machine was shared with the PGO benches throughout (load 9–15), so read ±2–3 % as noise.

| Unit | Base: real / driven %, p99, mean | **New: real / driven %, p99, mean** | New without pairs | Change real / driven / p99 / mean |
|---|---|---|---|---|
| 5150 | 87.6 / 92.2 %, 2403, 1199 µs | **85.2 / 90.2 %, 2367, 1169 µs** | 88.6 / 92.9 %, 2429, 1210 µs | −2.7 / −2.2 / −1.5 / −2.4 % |
| Uberschall | 70.4 / 75.3 %, 1697, 971 µs | **69.3 / 74.1 %, 1647, 956 µs** | 71.7 / 76.3 %, 1708, 987 µs | −1.6 / −1.6 / −2.9 / −1.6 % |
| BE-100 | 65.3 / 70.5 %, 1406, 905 µs | **64.4 / 69.4 %, 1388, 892 µs** | 65.1 / 70.2 %, 1400, 902 µs | −1.4 / −1.6 / −1.3 / −1.5 % |
| SLO-100 | 67.5 / 78.8 %, 1729, 975 µs | **65.2 / 76.7 %, 1714, 946 µs** | 67.7 / 79.9 %, 1762, 984 µs | −3.4 / −2.7 / −0.9 / −3.0 % |
| SLO-100 MkII | 61.1 / 63.4 %, 1297, 830 µs | **56.9 / 59.7 %, 1217, 777 µs** | 59.0 / 61.6 %, 1278, 804 µs | −6.9 / −5.8 / −6.2 / −6.3 % |
| Dual Rectifier | 46.6 / 50.7 %, 1397, 648 µs | **45.2 / 49.2 %, 1382, 630 µs** | 46.8 / 51.1 %, 1434, 652 µs | −3.0 / −3.0 / −1.1 / −2.9 % |
| Plexi | 47.7 / 54.6 %, 1460, 682 µs | **47.6 / 54.7 %, 1453, 682 µs** | 49.3 / 56.1 %, 1507, 703 µs | −0.2 / +0.2 / −0.5 / −0.1 % |
| JCM800 | 44.3 / 55.1 %, 1523, 663 µs | **42.9 / 53.7 %, 1480, 644 µs** | 44.5 / 55.3 %, 1536, 665 µs | −3.2 / −2.5 / −2.8 / −2.9 % |
| Twin | 38.8 / 47.3 %, 1101, 574 µs | **38.3 / 46.2 %, 1068, 563 µs** | 39.7 / 48.2 %, 1125, 586 µs | −1.3 / −2.3 / −3.0 / −1.8 % |
| AC30 | 30.1 / 43.0 %, 1231, 488 µs | **30.7 / 43.3 %, 1255, 494 µs** | 31.0 / 43.7 %, 1272, 498 µs | +2.0 / +0.7 / +1.9 / +1.2 % |
| JC-120 (no tubes: the noise reference) | 85.5 / 107.9 %, 2540, 1289 µs | 83.6 / 104.1 %, 2466, 1251 µs | 83.7 / 104.2 %, 2430, 1253 µs | −2.2 / −3.5 / −2.9 / −2.9 % |

**Reading it.**
- The JC-120 runs no tube law, so its new code is idle there. It still moved −2.9 % on the mean, which shows the noise
  floor of this shared machine.
- Against that floor, the 9900X agrees with the load-independent instruction counts: lever 2 is worth about 1–3 % on
  the tube amps (median −2.4 % mean block), with p99 following the mean.
- It is no larger on time than on instructions, so the interleaved layout did not unlock a hidden latency win either: the
  static kernels already overlap the devices' table loads.
- The pairs are worth having: without them the new tree is at or above the base on most units. The one-device lanes
  alone are under 1 % on time.
- Nothing here changes a cut decision (§10.43 / §10.45) or the budget factors of §10.45. The 5150 still needs about
  2.7× on the mean and 4.1× on p99.

**Switches.** `CE_NOTUBELANES=1`: the one-lane laws on the separate tables, as before (pairs off with them).
`CE_NOTUBEPAIRS=1`: one device at a time in the static kernels. Both give the same results as the default.
`CE_NODALINFO=1`: the loop diagnostic of lever 1.

**Gates (M4 Pro, final tree).** All pass:
- test_circuit_engine;
- test_circuit_unit (codegen 244 Lin + 105 Lu, the 5150 20 / 20 bound, the 2048-sample glide max diff 0);
- test_effect_handoffs;
- test_aliasing (51 of 51 below −60 dB);
- rig_tests.

**Artefacts.** `work/circuit-netlists/exactmisc/`:
- `data/`: the loop survey, the instruction tables and the 9900X logs' summary;
- `scripts/`: the push, build, bench and hash scripts.

### 10.52 Load time: one supply settle per channel, and the slow-rail fallback remembered (2026-11-14)


**The cause.** `CircuitUnit::buildChannel` compiles each channel at the unit's control positions (`CompileOptions::controls`).
`compile()` ends in `reset()`, which, with `settleMs`, runs the whole supply settle: the 450 ms mains run,
`settleSlowRails` (§10.49), step 2's DC hold and the 60 ms tail. `buildChannel` then called `setControlImmediate` with the
same positions and `reset()` again: the same settle a second time, on every channel of every mains-fed unit.

**Is the second settle a no-op?** `Circuit::stateDigest()` (new) hashes every `stateArrays` array, the noise generators, the
source phase and the sample count. Taken before and after the second reset, and backed by 3 s output hashes, the answer
is: on most units, bit for bit; on some, no. `reset()` is not a pure function of the controls:
1. **Variac- or power-scaled mains** (`"scale": {"control": "variac" | "power"}`: the Twin, Bassman, 5E3, Plexi, Plexi
   variac, 1959RR, JCM800, Uberschall and both Dual Rectifiers).
   - `setControlImmediate` calls `refreshScaledSources`, which writes those sources' `knownValue_` at the current
     `phase_`. After the first settle, that is wherever the mains happened to be when it ended.
   - The second reset's DC point then solved the mains, heater and bias sources at that instant instead of at t = 0.
     Its settle started from a different point and ended 10–30 mV away on rails of a few hundred volts.
2. **Per-sample history that `reset()` does not clear.** Examples: the small-signal path's back-off counters, the
   substep predictor's grid counters and the junction limiters' state. The first settle's run leaves them where it
   ends, and the second settle's Newton solves take other iteration paths. The result differs at rounding level
   (AC30, AC15, Deluxe, SLO-100 MK II, JC-120, Supro, RE-201, Uni-Vibe, Dimension D, CE-1, DR103 at some settings).
3. **Transformer-fed supplies** (SVT, Fairchild 670). The second reset warm-starts `settlePeriodic` from the first
   one's periodic state: one averaged step and a period, about 0.1 s.

So for classes 1 and 2, the state the old code reached cannot be had without running the first settle. Those units now
load in the first settle's state.

**The fix** (`CircuitUnit::buildChannel`, `Circuit::settlesSupply`).
- The second `setControlImmediate` + `reset()` is skipped when all three hold:
  - the compile settled a mains supply (`settleMs` with a source of 40 Hz or more, or `coldMs`);
  - the supply is not transformer-fed (`hasPeriodicState()`): those keep their short second reset, so they stay
    bit-identical;
  - every control already reads what `buildChannel` would set (`controlValue(i) == controlFromParams(i)`).
- It stays wherever the reset is only DC solves: pedals without mains, edit copies (`tl_prepareForAdoption`) and
  topology rebuilds. It also stays on the five pedals whose only AC source is an LFO start kick under 40 Hz (Memory Man,
  Small Stone, Electric Mistress, Phase 90, Tremulus Lune). Those cost 0.02–0.17 s and stay bit-identical. The Memory
  Man would otherwise load with its LFO and BBD clock elsewhere: a null of +1.5 dB, the same sound decorrelated.
- `CE_DOUBLESETTLE=1` restores the double settle. With `CE_SLOWMEMO=0` it is the tree before, byte for byte (below).

**The slow-rail fallback, remembered** (`Engine.cpp`, `settleSlowRails`; `CE_SLOWMEMO=0` turns it off).
- Where the averaged circuit does not compile, has no DC point, or its steps never shrink, the fallback is recorded in a
  process-wide memo. The key is a hash of the netlist JSON (values included), the partition hash (so switch positions
  count), the period and the solver options. Continuous controls are not in the key.
- A later settle of the same circuit skips the averaged circuit and goes to the same exit as the failure: the saved
  state is scattered back (predictor and LTE histories restarted, converters primed), and phase, tick and the AC
  sources are restored.
- Measured fallbacks (9900X, both channels; the defaults plus 5 random settings each, all fall back):
  - OR120 and SLO-100 MK II: 0.11–0.25 s per attempt (four steps).
  - CE-1: about 1.7 s per attempt, because its averaged circuit has no DC point.
  - The JC-120 is never tried (its rails arrive, §10.49).
- The first load in a process pays once; every later load skips the attempt.
- Identity: OR120 and CE-1 are bit-identical with and without the memo. The SLO-100 MK II differs at −167 dB, because
  its failed steps leave the class-2 history behind.

**Identity proof** (9900X; `work/circuit-netlists/loadtime/scripts/loadtime.cpp`).
- **Method.** Every bound unit through the unit path (factory, `prepare(48000, 256)`), in three modes:
  - **def:** prepared at the defaults;
  - **rnd:** seeded random parameters, then prepared (the deferred cab-follow order);
  - **app:** prepared at the defaults, then the random parameters and `processControlRate` (`prepareWithParams`).
- **Signal and comparison.** 3 s per run: 0.3 s of silence, then plucked harmonic notes. Both outputs are hashed (float
  bits) and dumped for nulls (`null.py`). Noise is off, as in the tests (with noise on, each instance draws its own
  stream anyway).
- **Results** (68 bindings × 3 modes = 204 cases):
  - `CE_DOUBLESETTLE=1 CE_SLOWMEMO=0` against the tree before this section, built side by side: **204 of 204 hashes equal**.
  - **New, against the tree before:**

| Class | Units | Null (dB re output rms, 3 s) |
|---|---|---|
| Bit-identical, all three modes (46 of 68 bindings) | BE-100, 5150, Ecstasy, OR120, SLO-100, 6G15, SVT, Fairchild 670, Leslie, Memory Man, every pedal and studio unit | identical |
| 1. Variac / power-scaled mains | JCM800 (both ids) | −70 to −81; −93 to −96 per 0.25 s window, steady over the 3 s |
| | Plexi, Plexi variac, 1959RR | −77 / −81 to −84 / −81 to −84 |
| | Twin | −65 (def) to −95; the first note's window −68, decaying to −157 by 3 s |
| | Bassman, Dual Rectifier, Rev C, Uberschall | −94; −99 to −110; −98 to −109; −81 to −110 |
| | 5E3 | −120 to −136 |
| 2. Rounding level | AC30, AC15, Deluxe, SLO-100 MK II, JC-120, Supro, RE-201, DR103 (rnd / app only), Dimension D, CE-1, Uni-Vibe | −126 (Uni-Vibe) to −267; peaks −130 to −168 dBFS |

- **Rigs** (`--rig`, RackGraph's app path) hash equal where every unit is bit-identical: Odd-Meter Rhythm (BE-100),
  Breakdown Chug (5150) and Cascade Lead (OR120).

**Load time, one unit** (9900X; `prepare` wall time, the two channels in parallel, `taskset -c 2,3`; the defaults; min of
3 × 2 rounds, base and new alternated. Another LTO build was running on the machine, which costs about ±10 %; an
earlier, quieter round gave the same ratios. "First" is the first load in the process, before the memo knows the circuit.)

| Unit | Before (s) | After (s) | First | | Unit | Before | After | First |
|---|---|---|---|---|---|---|---|---|
| Fairchild 670 | 7.42 | 7.34 | 7.67 | | AC30 | 0.45 | 0.23 | 0.26 |
| CE-1 | 3.55 | **0.03** | 1.77 | | DR103 | 0.45 | 0.23 | 0.25 |
| BE-100 | 1.96 | **0.97** | 1.01 | | Plexi variac | 0.42 | 0.18 | 0.21 |
| JC-120 | 0.83 | **0.39** | 0.43 | | 1959RR | 0.41 | 0.18 | 0.19 |
| Ecstasy | 0.80 | **0.41** | 0.42 | | SVT | 0.40 | 0.42 | 0.44 |
| SLO-100 MK II | 0.79 | **0.24** | 0.38 | | Plexi | 0.40 | 0.17 | 0.20 |
| Uberschall | 0.79 | **0.36** | 0.37 | | JCM800 | 0.38–0.40 | 0.17 | 0.17–0.19 |
| Dual Rectifier Rev C | 0.74 | 0.36 | 0.38 | | Bassman | 0.37 | 0.18 | 0.18 |
| AC15 | 0.63 | 0.31 | 0.39 | | Supro | 0.34 | 0.16 | 0.19 |
| SLO-100 | 0.61 | 0.31 | 0.37 | | RE-201 | 0.32 | 0.16 | 0.16 |
| 5150 | 0.61 | **0.31** | 0.33 | | Dimension D | 0.26 | 0.13 | 0.15 |
| Deluxe | 0.53 | 0.26 | 0.27 | | Leslie | 0.21 | 0.10 | 0.10 |
| OR120 | 0.51 | **0.15** | 0.30 | | Small Stone / Memory Man | 0.17 / 0.10 | unchanged | |
| Twin | 0.48 | 0.25 | 0.30 | | 5E3 / 6G15 / Uni-Vibe | 0.13 / 0.13 / 0.12 | 0.07 / 0.07 / 0.06 | |
| Dual Rectifier | 0.46 | 0.21 | 0.25 | | all 68 bindings, summed | 26.4 | 15.0 | |

- The mains-fed amps load in about half the time. The OR120 and SLO-100 MK II save more on later loads, through the memo.
- The CE-1 was the slowest unit after the Fairchild: 3.6 s for two failed averaged-DC attempts. Its first load now
  takes 1.8 s, and later loads 0.03 s.
- The Fairchild 670 (7.4 s) and the SVT keep their periodic settle. The Fairchild's `settlePeriodic` is a separate item.

**Whole rigs** (9900X; `RackGraph::buildFromPreset` with parallel prepare, no spares, plus `syncCabFollow`, as
`RealtimeAudioServer::loadResolvedPreset` builds them; `taskset -c 0-7`; min of 3 × 2 rounds):

| Rig (circuit units) | Before (s) | After (s) |
|---|---|---|
| Odd-Meter Rhythm (BE-100) | 1.97 | 1.12 |
| Djent Precision (Uberschall) | 0.72 | 0.40 |
| Breakdown Chug (5150) | 0.72 | 0.39 |
| Reggae Skank (JC-120, Dyna Comp) | 0.84 | 0.42 |
| Cascade Lead (OR120) | 0.53 | 0.17 |
| Chime With Teeth (AC30) | 0.52 | 0.28 |
| Seventies Arena Crunch (JCM800) | 0.41 | 0.21 |
| Hundred-Watt Crunch (Plexi) | 0.40 | 0.17 |

**Mac** (arm64, `app/build-loadtime`, the same binary with `CE_DOUBLESETTLE=1 CE_SLOWMEMO=0` as "before", which the
9900X showed equal to the old tree; load average 30–40 from other builds, so the absolute figures are 3–5× the
9900X's; min of 3 × 2 rounds):

| Unit | Before (s) | After (s) | First | | Rig | Before (s) | After (s) |
|---|---|---|---|---|---|---|---|
| CE-1 | 19.8 | **0.10** | 6.6 | | Odd-Meter Rhythm | 8.94 | 4.22 |
| BE-100 | 10.8 | 6.7 | 7.9 | | Djent Precision | 2.67 | 1.58 |
| Ecstasy | 3.91 | 1.98 | 2.07 | | Breakdown Chug | 2.72 | 1.24 |
| Uberschall | 3.78 | 1.66 | 1.66 | | Seventies Arena Crunch | 1.49 | 0.43 |
| JC-120 | 3.28 | 1.81 | 1.81 | | Cascade Lead | 2.51 | 0.91 |
| SLO-100 MK II | 3.17 | 1.15 | 1.34 | | | | |
| 5150 | 2.98 | 1.51 | 1.70 | | | | |
| OR120 | 2.62 | 0.91 | 1.32 | | | | |
| Twin | 2.60 | 1.29 | 1.54 | | | | |
| Plexi | 2.28 | 0.79 | 0.79 | | | | |
| AC30 | 2.02 | 1.02 | 1.06 | | | | |
| JCM800 | 1.57 | 0.62 | 0.62 | | | | |

**Gates** (the final code, 9900X scratch build and Mac `app/build-loadtime`). These all pass:
- test_circuit_engine;
- test_circuit_unit, including the codegen kernel checks and the edit-path round trip;
- test_effect_handoffs;
- test_aliasing (51 of 51 gated);
- rig_tests;
- test_graph_runtime.

**Left as found.**
- `reset()` is not idempotent for classes 1 and 2. A later reset (RackGraph::reset → `onReset`) of those units does not
  return to the load state, as before.
- Making it idempotent is a separate change:
  - re-evaluate the scaled sources at phase 0 at the start of `reset()`;
  - clear the per-sample histories.
  It would not change any first reset.

**Switches.**
- `CE_DOUBLESETTLE=1`: the second reset after every compile, as before.
- `CE_SLOWMEMO=0`: every settle tries the averaged circuit again.
- `CE_LOADPROBE=1`: prints each channel's state digest before and after the second reset; use it with
  `CE_DOUBLESETTLE=1` to see whether that reset changes anything.

**Files** (`work/circuit-netlists/loadtime/`):
- `scripts/loadtime.cpp`: the unit and rig load timer and hasher (link it against `rig_engine`);
- `scripts/null.py`: nulls between two `LT_DUMP` directories;
- `results/`: the runs' text output (hashes per unit and mode, timings, gates; 9900X and Mac).

### 10.53 Fairchild 670 load time: the transformer-fed settle's cold run, and DC-only helpers (2026-11-15)

**The cause.** The Fairchild 670 (24000) took 6.6 s to load on the 9900X and 9–11 s on the Mac (`loadtime`,
§10.52). Almost all of it went into the first 1/60 s of `settlePeriodic`'s cold run. Profile (9900X, `CE_PSS_DEBUG=1`,
before this section):

| Part of the load | Time |
|---|---|
| Compile, and reset's DC point (the DC cut sweeps run to their 400-sweep limit without converging) | 0.08 s |
| **Cold run, period 0** (1600 samples at 96 kHz) | **6.1–7.6 s** |
| Cold run, periods 1–5 | 0.012 s each |
| Averaged-DC steps (5 steps; shifts 133, 50, 2.7, 0.80, 0.13 V; 21 periods run) | 0.38 s |
| Extrapolation (one cycle of 9 periods, jump 0.019 V) and the last period | 0.08 s |
| Second (warm) reset: DC point, one averaged step, one period | 0.09–0.17 s |

- **Period 0.** The two channel groups (groups 3 and 4, one per channel, 30 Newton ports each) fail Newton on
  samples 878–1474, 9–15 ms into the supply's first charge. That is 991 group-samples. Every other period of the
  settle solves with no failure.
- The Newton iteration cycles: PTC's residual falls to 5e-3 and jumps back to 0.4, four steps a cycle, never under
  tolerance. Each failure keeps the last port voltages (`vpPrev`), so the next sample starts from the same point. It
  fails again, until the inputs move back near it (each group recovers twice, after 4–6 iterations).
- **What a failure cost: about 7.7 ms.**
  - `newton()`: 32 iterations, then its own 240-iteration PTC fallback;
  - the homotopy: 4 + 16 steps, each a full `newton()` with that PTC fallback;
  - the half-step rescue;
  - `processGroup`'s 200-iteration PTC.
  - At the defaults every one of them failed, on all 991 samples (991 homotopies, 991 failures, 0 PTC rescues). At
    the rnd case's settings one rescue in 989 succeeded. The rnd result is bit-identical too, so that sample was not a
    held one: a first failure still tries every rescue.

**The fix** (`Engine.cpp`; `CE_PSSFAST=0` is the settle before, bit for bit).
1. **The cold run skips rescues that cannot change the outcome.** This covers only `settlePeriodic`'s cold run
   (`pssQuick_`, set around `runMains(6 * N)`).
   - It applies to a group whose last sample failed and kept its ports (`GroupRt::pssHeld`), and whose Newton fails
     again.
   - Such a sample goes straight to the failure exit. It runs no homotopy, no half steps, no PTC, and no `newton()`
     PTC fallback (`GroupRt::pssNoPtc`).
   - Newton itself still runs every sample at the full 32 iterations, so a group recovers on the same sample as before.
   - The first failure after a solved sample still tries every rescue.
   - Period 0 now takes 0.08–0.11 s instead of 6–7.6 s.
2. **The averaged circuits are DC-only** (`Circuit::dcOnly_`). This covers `settlePeriodic`'s helper and `settleSlowRails`'
   (§10.49).
   - Those helpers are only ever reset (solved at DC) and read. Each `reset()` still rebuilt the sparse, sparse-linear
     and substep structures (`buildSparse`, `buildSparseLin`, `buildSubSparse`).
   - Only a transient sample reads those structures, and a DC solve does not use them (`r.dcMode` passes no sparse LU and
     no kept factorisation).
   - On the Fairchild they took 32 ms a reset, 25 of them in `buildSubSparse` for the two 30-port groups. That is 8
     resets a load: in the cold settle, the helper's own compile and 5 steps; in the warm reset, its compile and 1 step.
   - The helpers now skip them. Every mains-fed amp's `settleSlowRails` helper gains the same.

**Why it is the same state.** Fix 1 changes what the cold run does only on samples where today every rescue fails.
There the failure exit runs as before: the kept ports and the currents at them.
- The skipped calls leave nothing behind that the next sample reads. The proof is empirical (below).
- One caveat: on another circuit a skipped rescue might have succeeded. That circuit's cold run would then hold one
  sample longer, and its settled state would differ at the settle's own tolerance. On the two transformer-fed units
  bound today it is bit-identical in every mode tried.

Fix 2 removes work whose results nothing reads.

**Identity proof** (`work/circuit-netlists/loadtime/scripts/loadtime.cpp`, unchanged; 3 s renders, both outputs hashed
as float bits, noise off).
- **9900X, the whole library.** 68 bindings × def / rnd / app = 204 cases. The tree before this section (built side by
  side), the new tree, and the new tree with `CE_PSSFAST=0`: **204 of 204 hashes equal.** The null over every 3 s render
  is therefore exact.
- **State digests** (`CE_LOADPROBE=1`, `Circuit::stateDigest` before and after the second reset). They are equal, new
  against `CE_PSSFAST=0`, for the Fairchild and the SVT in def, rnd and app, on the 9900X and on the Mac.
  - Fairchild def: `b72e4ed9694778b9 → b9218b3d10eadf70` (9900X).
- The Mac's render hashes, fast against `CE_PSSFAST=0`, are also equal for both units in all three modes.

**Gates** (9900X scratch build, Mac `app/build-f670load`): test_circuit_engine (all `f670_*` cases),
test_circuit_unit, test_effect_handoffs (including the Fairchild S301 switch-topology hand-off) and test_aliasing pass.

**Load time** (`prepare`, defaults, `taskset -c 2,3` on the 9900X; min of 3 reps; 3 rounds alternated on the 9900X, 2 on
the Mac; the Mac at load average 15–17 from other jobs):

| Unit | 9900X before | 9900X after | Mac before | Mac after |
|---|---|---|---|---|
| Fairchild 670 | 6.60 s | **0.53 s** (first load in the process 0.67) | 9.2–11.0 s | **0.77–0.84 s** |
| SVT | 0.385 s | **0.295 s** | 0.54–0.58 s | **0.42–0.45 s** |

Every bound binding at its defaults (9900X, min of 3, one pass each tree): the sum goes from 13.7 s to 7.1 s.
- Fairchild 6.59 → 0.53 s.
- BE-100 0.88 → 0.73 s, Ecstasy 0.37 → 0.31 s, Dual Rectifier Rev C 0.34 → 0.30 s, Dual Rectifier 0.21 → 0.16 s and
  RE-201 0.15 → 0.12 s: the `settleSlowRails` helper.
- The rest within ±0.02 s.

**Where the Fairchild's 0.53 s goes now:**
- the DC points of its two resets, 0.08 s each (the DC cut sweeps hit their 400-sweep limit);
- the sparse and substep builds of its two resets, 0.03 s each;
- 37 + 2 mains periods, about 0.3 s;
- 8 helper DC points, 3 ms each.

**Measured and rejected.**
- **Not solving reset's DC point before `coldStart()`** (0.08 s). `coldStart` overwrites every array in `stateArrays`,
  and the digest right after it is equal. The settle that follows is not: one group's `GroupRt::p` (Newton's port
  scratch) keeps the DC values, and the rnd case's digest and render differ (the extrapolation's jump moves from
  1.902e-2 to 1.903e-2 V). Not landed: it is not the same state bit for bit.
- **Capping the held samples' Newton at 8 iterations.** Bit-identical here (the recoveries take 4–6), but it saves only
  0.05 s and could move a recovery elsewhere.

**Not tried** (they would move the state at the settle's tolerance, not keep it bit for bit):
- skipping the transformer-fed second reset (0.09–0.14 s; its averaged step moves C307B by 4.9 mV);
- warm-starting the helper's DC points;
- Anderson acceleration of the averaged steps.

**Switches.**
- `CE_PSSFAST=0`: the settle before this section (both fixes off).
- `CE_PSS_DEBUG=1` now also prints the cold run's time, the period length and the state dimension.

**Files** (`work/circuit-netlists/f670load/`):
- `results/`: the 9900X and Mac runs (hashes per unit and mode, digests, timings, gates);
- `scripts/timing.sh`: the alternated 9900X timing.

Everything below sits behind `HEADROOM_CIRCUIT_ENGINE` / `HEADROOM_CIRCUIT_UNITS`; no default is flipped. All 24 CTest
tests pass in `app/build-circuit`.

**Done.**
- §10.8 CPU round 2. The gate is still open.
- §10.9 the five amps (bound).
- §10.10 Wave B devices, plus DS-1, Big Muff and both Fuzz Faces (bound).
- The DS-1 relabel to its DS1-A circuit.

**In progress or next, in order.**
1. **Done (§10.10a): rail-node v2; Klon (179000) and Bluesbreaker (180000) landed.** The netlist models the
   real supply networks in
   `work/circuit-netlists/wave-b/` (RAT R10/C11/C12 rail filter, Klon MAX1044 Thevenin rails with D7/D8, Bluesbreaker
   D5 feed), using rail-node op-amps.
   - Then copy each into `src/circuit/library/` and run `embed_library.py`.
   - Add tran / AC / DC cases to `tests/test_circuit_engine.cpp`, then
     `HEADROOM_NGSPICE=1 HEADROOM_NGSPICE_ONLY=<ids>`.
   - Bind 42000 in `CircuitUnit.cpp` and add an A/B setting in `circuit_ab.cpp`.
   - The Klon and the Bluesbreaker have no catalogue unit: the user decides IDs and names.
2. **Wave C.**
   - The Phase 90 and both wahs have landed (§10.10b).
   - Next: an OTA device, the optical split, and BBD / tape blocks per `work/circuit-netlists/wave-c/DEVICES.md`,
     then Small Stone, Dyna Comp, Uni-Vibe, LA-2A, 1176, CE-1, Memory Man and RE-201.
   - Decisions (2026-11-09):
     - Follow the real hardware: MN3002 for the CE-1, a CD4047 clock for the Memory Man.
     - Tape v1 (simplified magnetisation) first, v2 (Jiles–Atherton plus bias) later.
     - BBD noise is on by default at the authentic level, with an off switch (off in tests).
     - Small Stone: the version unit 20000's catalogue name/description indicates; note it in the netlist.
     - Holters & Parker (DAFx-18) may be cited and implemented from the paper.
     - 21000 stays on its legacy model.
     - Levels and brightness go to the user with the A/B pages.
   - Device research (OTA, BBD, compander, T4B / Uni-Vibe optics, the 1176 FET, tape) goes to
     `work/circuit-netlists/wave-c/DEVICES.md`.
   - Engine work after that: an OTA device; optical-cell generalisation of `ldr`; BBD and tape as behavioural blocks
     with electrical ports (like `SpringTank`).
3. **CPU gate (round 3, §10.8b: met clean except the Plexi, at the line dimed except the Deluxe).** Remaining
   levers:
   - a code-generated per-group kernel (unrolled K with the live-row structure; latency, not flops, dominates);
   - fewer dimed iterations in the power group (3.3 per sample: knee transitions);
   - the preamp-at-2× question, which is the user's call (§10.8).
   - Rejected so far: see §10.8.
4. **Listening review and level calibration before any flip.** The circuit units are brighter than the legacy models
   everywhere, and pedal outputs are physical volts.

**Developer switches.**
- `CE_NOLOWRANK`: dense Newton step.
- `CE_DRIFT` is gone; `CE_CHORDGATE` sets the chord-confirm gate (default 300).
- `CE_LIMWALK`: the old 60 V walk limiter.
- `CE_NOPENTLUT`: exact pentode law.
- `CE_KBENCH`, `CE_PROFILE`, `CE_TRACE_FAIL=<n>`: trace the n-th homotopy's direct Newton (with `CE_DEBUG_FAIL`).
- `CE_TOL=x`: every port's Newton tolerance (`1e-6` is the engine before §10.42); `CE_TOLTUBE=x`: tube ports only.
- `CE_TOLGROUPS="ref=x,..."`: replace a netlist's `"newton": {"groups"}` (the non-tube ports of the group holding each part at x; `*=x` every group; `none=1` none, the engine before §10.55).
- `CE_TRBDF2=1`: TR-BDF2 LTE substeps (§10.47; off by default), with `CE_TBMAX` (macro steps, 2), `CE_TBSCALE` / `CE_TBFULL` (the decision boundaries, 1) and `CE_TBPRED` (Newton start bits, 7).
- `CE_SUBPRED=<bits>` (default 30; 0 = the zero-slope substep start before §10.46), `CE_SUBPREDPORTS=<n>` (default 19): the LTE substeps' Newton start; `CE_LTESTAT=1`: LTE event statistics per group (§10.46); `CE_BLOCKINSTR=1`: per-block retired instructions in `bench --real` (macOS).
- `CE_EVOS=0|1`: event-local oversampling off for every netlist / on for every ladder-raised group (unset: the netlist's `"multirate": {"events": true}`); `CE_EVOS_P`, `CE_EVOS_Q`, `CE_EVOS_FORCE=1` and the rest in §10.48; `CE_ALIAS_IDEAL=1`: `circuit_cli alias` on the any-rate metric (§10.48).
- `CE_NOCODEGEN=1`: disable ahead-of-time kernels, sparse (§10.44) and Newton (§10.45) (`CompileOptions::codegen = false`); `CE_CGDIAG=1`: log per-group kernel hits/misses; `CE_NOSPARSE=1`, `CE_SPMINPORTS=<n>` (default 16), `CE_SPFULL=<r>` (default 4.2), `CE_SPWEIGHT=<w>`, `CE_LINWEIGHT=<w>` (default 3.0): sparse solver thresholds.
- `CE_SLOWSETTLE=0`: the mains-fed settle before §10.49 (no averaged-circuit steps for slow rails, no hold of mains-side groups in step 2); `CE_PSS_DEBUG=1` prints the steps.
- `CE_NONEWTONHINTS=1`: ignore a netlist's `"newton"` chord / predictor hints (§10.50; the JC-120 sets chordGate 300, quadPorts 4, subPred 26, subPredPorts 4).
- `CE_NOTUBELANES=1`: the one-lane Koren table laws (§10.51; also turns the tube pairs off); `CE_NOTUBEPAIRS=1`: one tube at a time in the generated Newton kernels; `CE_NODALINFO=1`: per group, capacitor loops and what removing them would save (§10.51). All bit-identical to the default.
- `CE_DOUBLESETTLE=1`: every circuit unit channel resets (and settles) a second time after its compile, as before §10.52; `CE_SLOWMEMO=0`: no memo of slow-rail fallbacks; `CE_LOADPROBE=1`: state digests around that second reset.
- `CE_PSSFAST=0`: the transformer-fed settle's cold run tries every rescue on every failed sample, and the averaged (DC-only) helpers rebuild their transient structures at each reset, as before §10.53 (the default is bit-identical, only faster).
- `CE_DIODECHAIN=0`: a rectifier output inside a series diode chain stays at the chain's midpoint, as before §10.56 (the OR120 and SLO-100 MK II then fall back to the old supply run).

### 10.54 Compiler-level CPU: PGO and ThinLTO, byte-identical on both architectures, and why arm64 PGO needs a pin (2026-11-15)

(Updated the same day: arm64 now builds with `-force-ordered-reductions=false` and ships unpinned PGO,
ThinLTO is the default for the shipped Mac binaries, and both profiles were retrained. The current settings are in
"Update: arm64 PGO unpinned, ThinLTO on the products, retrained profiles" at the end of this section; the text before
it is the original investigation.)

**The question.** Can compiler optimisations make the engine faster without changing a single output bit and without
CPU-specific tuning? The targets stay portable: arm64 generic on the Mac, `-march=x86-64-v3` on Linux and Windows,
never `-march=native`. Three levers were tried: profile-guided optimisation (PGO), ThinLTO, and hot/cold splitting.
The gate is `bench --real` output hashes (real and driven material) of all 68 bound units, identical to the build
without the lever, on the M4 Pro and on the 9900X.

**What landed (all build-side; no engine source changed).**
- `app/cmake/HeadroomOptimise.cmake` holds the switches. Both `app/CMakeLists.txt` and `app/plugin/CMakeLists.txt`
  include it before any target, so Headroom.app, the AU / VST3 plugins, `circuit_cli` and the tests build the same way.
  - `HEADROOM_PGO=AUTO|OFF|GENERATE|USE`. `HEADROOM_PGO_PROFILE` defaults to `app/pgo/headroom-<arch>.profdata`.
    A `.proftext` is indexed at configure, and a profile this clang cannot read stops the configure.
  - `HEADROOM_PGO_PIN` (USE only) is described below. It was default ON on arm64 until the update at the end of this
    section; it is now OFF everywhere and kept as a diagnostic.
  - `HEADROOM_LTO=OFF|THIN|FULL` (since the update also `AUTO` and `PRODUCTS`). On Linux it links with lld and archives with llvm-ar, because GNU ld cannot read
    bitcode.
  - The flags reach only clang-compiled sources. On Linux, miniaudio is built by GCC.
  - No `-ffast-math`, `-ffp-contract` or `-fno-math-errno` flag is added or removed, so the FP model stays clang's
    default `-ffp-contract=on`.
- `app/pgo/headroom-arm64.profdata` (Apple clang 21) and `app/pgo/headroom-x86_64.profdata` (clang 20), 7.2 / 7.1 MB.
- `app/tools/pgo/train.sh` and `training_di.py` retrain a profile. Each of the 68 bound units plays two clips through
  the unit path, and the 11 heavy amps play six.
  - The runs use `bench --real` with `CE_REAL=synthetic`: the clip, then the synthetic HF / sweep / burst suite.
  - Each unit also plays one clip on the interpreter (`CE_NOCODEGEN=1`). The plugins and edited circuits run the
    interpreter, and without training it would be profiled cold.
  - The clips are GuitarSet comping and solos, clean and through the bench's fuzz pre-stage. They are not the four DIs
    that `bench --real` times, so the measurement is held out from the training.
- Front-end instrumentation (`-fprofile-instr-generate` / `-use`) is used, not IR PGO.
  - Its records are keyed on function name and AST hash, and file-local functions are prefixed with the source
    *basename*. One profile therefore matches any checkout path, any build directory, the app and the plugins.
  - In the plugin build, all 1902 functions in Engine.cpp found their records.
  - One profile cannot serve both architectures: libstdc++ / libc++ mangling and `uint64_t` (`m` / `y`) leave 4964
    of 17457 names shared. So the profiles are per platform.

**Identity: hashes of the 68 bound units, real + driven, against the OFF build of the same tree.**

| Build | arm64 (M4 Pro) | x86-64-v3 (9900X) |
|---|---|---|
| ThinLTO | 68 / 68 identical | 68 / 68 identical |
| PGO, unpinned | **57 of 68 differ** | 68 / 68 identical |
| PGO + ThinLTO, unpinned | **57 of 68 differ** | 68 / 68 identical |
| PGO + split-machine-functions, PGO + hot-cold-split | — | 68 / 68 identical |
| PGO, pinned (`HEADROOM_PGO_PIN`) | 68 / 68 identical | 68 / 68 identical |
| PGO pinned + ThinLTO | 68 / 68 identical | — |
| PGO with a stale profile (tree 4 h newer than training) | — | 68 / 68 identical |

The gate tests also pass on every build that ships: test_circuit_engine, test_circuit_unit, test_effect_handoffs,
test_aliasing and rig_tests. Their output is line-identical to OFF, except for wall times and lane-scheduling counts.

**Why unpinned PGO changed arm64 output.** No translation unit uses fast-math or contract(fast). The
`reassociate(on)` sums were checked as well: `nkResidual<NN>` compiles to the same code in both builds, with blocks
reordered only.

Bisection found the cause. Object mixing showed that only Engine.cpp.o matters. Bisecting its 17459 profile records,
four ways in parallel on TS808, isolated `Circuit::updateNoise`'s `draw` lambda: the 1/f noise bank, a float
`v += w[j] * s[j]` loop.
- In the OFF build, the lambda is inlined and LoopVectorize vectorises that loop. Its reduction is not reassociable,
  so AArch64 (which, unlike x86, enables in-order FP reductions) vectorises it *in order*. In doing so it splits the
  `llvm.fmuladd` into `fmul.4s` followed by scalar in-order `fadd`s, which is unfused.
- With the profile, the lambda stays out of line, `k` stays in memory, and the loop stays scalar with `fmadd`, which
  is fused.

Both are legal under `-ffp-contract=on`, because fmuladd *permits* fusion and does not require it. The rounding moves
by about −165 dB, and every hash changes. x86 has no in-order reductions, so the loop is never vectorised there and
the hazard does not exist.

At least one more record (a disjoint quarter of the profile) reproduced on its own. Any profile-driven change to
inlining, peeling or vectorisation of such a loop is a candidate.

**The pin.** With `HEADROOM_PGO_PIN`, the profile no longer drives the decisions that can move fusion:
- the inliner's hot, cold and locally hot call-site thresholds, all held at the default 225, with no cost-benefit mode;
- indirect-call promotion;
- the loop vectoriser's profile trip counts and cold-region limits (`-loop-vectorize-with-block-frequency=false`);
- PGSO;
- profile-driven peeling.

The profile still drives branch probabilities, block placement, register allocation, if-conversion and switch
lowering. Neither half of the pin was enough alone on TS808: the inliner flags alone and the vectoriser flags alone
each still changed the output. With the whole pin, all 68 units are identical. The pin is also passed to the ThinLTO
link (`-Wl,-mllvm,…`).

A check confirmed the mechanism. With `-mllvm -force-ordered-reductions=false` in *both* builds, unpinned arm64 PGO is
68 / 68 identical to its own OFF build. That flag, however, moves today's arm64 output on 66 of 68 units, by −160 to
−172 dB on the float noise path. It is a one-time output change for the user to decide on, and it is not applied.

**Timing.** `bench --real` with `CE_REALREPS=3` (per-block minimum of 3 runs). All builds of one unit run side by
side, so they see the same machine load, and the start order rotates each round. Three rounds; the tables give the
mean over rounds. Columns: real % of a core / driven % / combined 64-frame p99 µs, then the change against OFF.

**9900X, campaign A** (3 rounds). OFF: real % / driven % / p99 µs; the others: change in each, %.

| Unit | OFF | ThinLTO | PGO | PGO + ThinLTO |
|---|---|---|---|---|
| 5150 | 90.3 / 94.2 / 2409 | -0.3 / -0.6 / -0.3 | -3.3 / -3.4 / -3.6 | -3.5 / -4.1 / -3.8 |
| Uberschall | 74.1 / 78.1 / 1693 | +0.2 / +0.1 / +0.4 | -0.0 / +0.4 / +1.6 | -2.8 / -1.9 / -2.4 |
| BE-100 | 70.3 / 75.0 / 1476 | -0.8 / -0.9 / -1.5 | -4.7 / -4.5 / -5.5 | -5.3 / -4.9 / -5.8 |
| SLO-100 | 68.4 / 79.3 / 1671 | -0.3 / -0.5 / -0.0 | -0.9 / -1.3 / -0.6 | -0.3 / -0.1 / +1.5 |
| SLO MkII | 60.8 / 63.9 / 1262 | +0.9 / +0.9 / +0.9 | -2.4 / -2.4 / -2.6 | -1.6 / -0.9 / +0.3 |
| JC-120 | 95.3 / 110.4 / 2530 | -1.0 / -0.4 / -0.5 | -5.4 / +0.2 / -1.0 | -6.4 / -0.8 / -0.0 |
| Dual Rectifier | 50.7 / 54.8 / 1473 | -0.6 / -0.2 / -0.7 | -5.4 / -4.3 / -6.0 | -5.0 / -4.3 / -4.8 |
| Plexi | 50.3 / 57.1 / 1495 | -1.0 / -0.2 / -1.0 | -4.1 / -3.9 / -4.5 | -2.2 / -2.0 / -2.5 |
| JCM800 | 46.7 / 57.5 / 1563 | +4.0 / +3.5 / +2.8 | -3.7 / -4.2 / -5.7 | -0.9 / -1.2 / -3.2 |
| Twin | 40.8 / 48.7 / 1074 | +1.0 / -0.1 / -1.0 | -4.1 / -4.1 / -5.1 | -1.6 / -1.0 / -0.0 |
| AC30 | 32.8 / 45.1 / 1265 | +3.7 / -1.5 / -1.5 | -2.0 / -4.0 / -4.2 | -0.4 / -2.9 / -3.4 |
| TS808 | 9.1 / 9.9 / 191 | -2.2 / -3.2 / -9.8 | -6.6 / -4.8 / -8.7 | -6.3 / -5.8 / -10.1 |
| Big Muff | 12.7 / 13.2 / 229 | -3.8 / -2.1 / +5.8 | -7.7 / -4.7 / -0.1 | -8.8 / -5.2 / +5.9 |
| HM-2 | 30.6 / 38.6 / 1049 | -3.8 / -1.3 / -0.7 | -4.9 / -3.6 / -4.2 | -4.0 / -2.9 / -4.8 |
| **geomean** | | **-0.3 / -0.5 / -0.6** | **-4.0 / -3.2 / -3.6** | **-3.5 / -2.7 / -2.4** |

ThinLTO: faster than OFF in 24 of 42 unit-rounds (real), median -0.2 %
PGO: faster than OFF in 39 of 42 unit-rounds (real), median -3.7 %
PGO + ThinLTO: faster than OFF in 38 of 42 unit-rounds (real), median -3.5 %

**M4 Pro** (3 rounds). OFF: real % / driven % / p99 µs; the others: change in each, %.

| Unit | OFF | ThinLTO | PGO (pinned) | PGO (pinned) + ThinLTO |
|---|---|---|---|---|
| 5150 | 89.3 / 93.9 / 2636 | -2.6 / -2.2 / -0.4 | -2.7 / -2.0 / -0.5 | -5.1 / -4.6 / -3.7 |
| Uberschall | 78.4 / 83.1 / 2211 | -3.3 / -3.2 / -2.8 | -0.8 / -0.7 / -0.5 | -3.8 / -3.6 / -3.4 |
| BE-100 | 72.4 / 77.4 / 1816 | -2.8 / -2.4 / -2.5 | -0.2 / -0.4 / -1.1 | -2.9 / -2.9 / -2.1 |
| SLO-100 | 68.7 / 80.3 / 2004 | -2.9 / -3.0 / -2.6 | -0.9 / -1.1 / -0.6 | -4.0 / -4.1 / -4.7 |
| SLO MkII | 58.3 / 60.9 / 1319 | -2.6 / -2.1 / -1.8 | -2.1 / -1.8 / -1.8 | -3.9 / -3.4 / -3.0 |
| JC-120 | 106.9 / 130.0 / 3820 | -8.0 / -7.3 / -4.4 | +0.2 / +0.5 / -0.0 | -11.1 / -9.7 / -6.0 |
| Dual Rectifier | 55.4 / 59.6 / 1935 | -2.1 / -2.1 / -3.0 | -1.7 / -1.7 / -1.6 | -3.6 / -3.6 / -2.7 |
| Plexi | 43.3 / 49.0 / 1395 | -4.6 / -4.6 / -4.7 | +0.3 / +0.0 / +0.1 | -4.5 / -4.4 / -5.9 |
| JCM800 | 39.1 / 49.2 / 1448 | -4.3 / -3.8 / -6.2 | +0.2 / -0.2 / -1.4 | -4.0 / -4.1 / -5.9 |
| Twin | 38.1 / 46.0 / 1190 | -1.9 / -2.9 / -2.3 | -1.6 / -1.7 / -1.8 | -4.2 / -4.8 / -4.2 |
| AC30 | 29.2 / 41.7 / 1321 | -5.7 / -4.8 / -2.8 | +0.9 / +0.5 / +0.9 | -5.1 / -4.4 / -2.1 |
| TS808 | 10.3 / 11.2 / 250 | -1.4 / -0.7 / -1.7 | +8.4 / +7.5 / +4.8 | +2.8 / +3.8 / +0.6 |
| HM-2 | 27.7 / 36.6 / 1143 | -3.1 / -2.1 / -1.0 | -1.8 / -1.4 / -1.4 | -5.4 / -3.7 / -2.5 |
| **geomean** | | **-3.5 / -3.2 / -2.8** | **-0.2 / -0.2 / -0.4** | **-4.2 / -3.8 / -3.5** |

ThinLTO: faster than OFF in 39 of 39 unit-rounds (real), median -3.0 %
PGO (pinned): faster than OFF in 24 of 39 unit-rounds (real), median -0.7 %
PGO (pinned) + ThinLTO: faster than OFF in 36 of 39 unit-rounds (real), median -4.4 %

**Other 9900X runs.**
- *Campaign A, splitting on top of PGO:* PGO + split-machine-functions −3.8 / −3.3 / −3.4, PGO + hot-cold-split
  −3.2 / −2.4 / −2.9. Neither beats plain PGO.
- *Campaign B* (3 more rounds, the same binaries plus the pinned build):

  | Build | Change vs OFF, real / driven / p99, % | Faster than OFF in |
  |---|---|---|
  | PGO | −3.4 / −3.4 / −4.1 | 37 of 42 unit-rounds |
  | PGO, pinned | −0.1 / −0.2 / −0.6 | 22 of 42 (noise) |

  On x86 the gain is the profile-driven inlining and vectorisation that the pin takes away, and unpinned PGO is
  already identical there.

**Unpinned arm64 PGO, for reference only** (it fails the identity gate): 1 round plus part of a second, geomean
change vs OFF, real / driven / p99, %.

| Build | Time change | Retired instructions |
|---|---|---|
| ThinLTO | −4.1 / −3.1 / −2.3 | −1.3 % |
| PGO | −4.9 / −4.1 / −3.4 | −5.0 % |
| PGO + ThinLTO | −6.7 / −5.9 / −5.1 | −5.6 % |

On arm64 the identity-safe part of the profile buys about nothing; ThinLTO is what pays there. The Mac p99 values are
inflated by machine load; the ratios between builds hold.

**Build cost and size.**

The six gate targets (`circuit_cli` and the five tests). Build times are wall / CPU seconds.

| | 9900X build | 9900X `circuit_cli` | M4 Pro build | M4 Pro `circuit_cli` |
|---|---|---|---|---|
| OFF | 218 / 833 (-j6) | 21.09 MB | 609 / 639 (-j5) | 14.80 MB |
| ThinLTO | 423 / 2397 (-j4) | 20.65 MB | 899 / 1353 (-j4) | 14.40 MB |
| PGO | 215 / 862 (-j6) | 20.94 MB | 436 / 601 (-j4, unpinned) | 14.19 MB |
| PGO, pinned | 163 / 765 (-j8) | 21.36 MB | 226 / 590 (-j4) | 15.03 MB |
| PGO + ThinLTO | 415 / 2466 (-j5) | 21.26 MB | 324 / 1220 (-j4, pinned) | 14.65 MB |

- **Build cost.** PGO costs nothing: under 5 % CPU. ThinLTO costs 2.9× CPU on the 9900X and about 2.1× on the Mac,
  because every test executable runs its own LTO back end.
- **Mac wall times are not comparable across rows:** the Mac's load average ranged from 25 to 77.
- **Headroom.app executable** (Mac): 21.33 MB OFF, 19.99 MB ThinLTO, 21.57 MB pinned PGO, 20.22 MB both.
- **Plugins** with pinned PGO + ThinLTO: AU 11.63 MB, VST3 11.67 MB, and `test_plugin_core` passes.
- **Training** takes 38 min on the 9900X (`JOBS=5`) and 2.3 h on the Mac under load.

**No gain.**
- Hot/cold splitting on top of x86 PGO: `-fsplit-machine-functions` with `-z keep-text-section-prefix`, and IR
  `-hot-cold-split`. The error, homotopy and rescue paths are already laid out cold by the profile.
- ThinLTO on x86: geomean −0.3 %, faster in only 24 of 42 unit-rounds, which is noise.
- Full LTO and CS-IRPGO were not tried. CS-IRPGO needs IR PGO, whose file-local names carry the full source path
  (`-static-func-full-module-prefix=false` would fix that), and the FE profile already leaves little on the table
  once pinned.

**Staleness.** The 9900X profile was applied to the tree as it stood 3.5 h after training. Meanwhile other work had
changed Engine.cpp (+175 lines) and Devices.hpp (+292).
- 16319 of 17457 records still matched, covering 92.6 % of the counts. Among the stale ones were the hottest
  functions: `Circuit::processGroup`, `NlDevice::evalLawAs` and `newtonKernel<…, DynSpec>`.
- Hashes were still 68 / 68 identical.
- The gain fell from −4.0 % to −2.4 % (one round, 11 amps).

Retrain after material engine changes. Then gate:
1. `train.sh`;
2. `hashes.sh` on an OFF build and on a USE build of the same tree, with `diff` empty on both architectures;
3. the five gate tests.

**Pitfalls met.**
- `bench --real` p99 on the Mac is inflated by the machine's load (average 25–77 during these runs); the ratios
  between builds hold.
- A leftover training process from a stopped run doubled about 40 units' runs in the arm64 profile. A weighted merge
  restored the intended shares.
- `circuit_codegen`'s `//` provenance comments vary between runs (thread order). The code is identical, and FE PGO
  ignores comments.
- The plugin build (`app/plugin`) still compiles the empty `CodegenStub.cpp`, so it runs the interpreter, not the
  §10.44 / §10.45 kernels. This predates this section. The profile covers the interpreter for that reason.

**Recommendation (original; items 2 and 3 are superseded by the update below).**
1. **x86-64 (Linux, and Windows when it builds with clang): PGO, unpinned.**
   - The default is `HEADROOM_PGO=AUTO`, which turns USE on for clang Release builds on x86-64 when
     `app/pgo/headroom-x86_64.profdata` exists and this clang reads it.
   - Result: about −3.5 % mean, driven and p99 on the heavy amps. It is identical on 68 / 68 units in five
     configurations and with a stale profile, and costs nothing to build.
   - ThinLTO adds nothing on x86. Hot/cold splitting adds nothing.
2. **arm64 (Mac): ThinLTO, not PGO.**
   - `-DHEADROOM_LTO=THIN` gives −3.5 / −3.2 / −2.8 % and was faster in 39 of 39 unit-rounds. It is 68 / 68
     identical and passes the gates, at about 2× build CPU.
   - Recommended for release builds: add it to `packaging/macos/build-release.sh`.
   - It stays opt-in because of the build cost on a busy dev machine, and because the universal build's x86_64 slice
     has not been through the hash gate (Linux x86 ThinLTO has, 68 / 68).
   - `AUTO` leaves PGO off on arm64. Pinned PGO is identical but worth −0.2 %. Unpinned PGO would be worth a further
     −3 % or so, but is not identical.
3. **To get unpinned PGO on arm64** (about −6.7 % with ThinLTO), the user would have to accept a one-time arm64
   output change of −160 to −172 dB from `-mllvm -force-ordered-reductions=false` in every arm64 build. That change
   makes the AArch64 vectoriser leave in-order FP reductions scalar, as x86 does. The alternative is an engine-side fix
   to the in-order reduction loops; there is a candidate in Engine.cpp's `updateNoise` and at least one more. Either
   way, re-gate afterwards.
4. **Maintenance.** Retrain after material engine changes and gate each new profile with the hash diff. The
   committed profiles were trained on the tree of 2026-11-14 21:00. They are stale against later Engine.cpp edits;
   stale is still identical, but the gain is smaller (see Staleness, above).

**Developer switches.**
- `HEADROOM_PGO=AUTO|OFF|GENERATE|USE`, `HEADROOM_PGO_PROFILE=<.profdata|.proftext>`, `HEADROOM_PGO_PIN=ON|OFF`.
- `HEADROOM_LTO=AUTO|OFF|PRODUCTS|THIN|FULL` (see the update below).
- Retraining: `app/tools/pgo/train.sh` (`JOBS`, `TRAIN_DI`, `CXX`, `CMAKE_ARGS`, `OUT`).
- Gates: `work/circuit-netlists/pgo/scripts/{hashes,gates,rbench}.sh`; arm64 bisection: `bisect.py`, `mix.sh`.
- Raw data: `work/circuit-netlists/pgo/` (`hash-*.tsv`, `bench-*.tsv`, `bisect-arm64.log`).

**Update: arm64 PGO unpinned, ThinLTO on the products, retrained profiles (2026-11-15).**

The one-time arm64 output change (item 3 above) was taken under the user's rule that non-lossy changes are fine: it
is rounding, not a change in the model.

*What changed (build side only; no engine source).*
- `HeadroomOptimise.cmake`:
  - Every clang build that targets arm64 passes `-mllvm -force-ordered-reductions=false` to the compiler, and to the
    linker when LTO is on (`-Wl,-mllvm,…`, because ThinLTO runs the loop vectoriser after the link). That covers the
    app, the plugins, the tests and the tools.
  - A universal build passes it to both slices. On x86_64 it is the default already.
  - The configure prints `Headroom LLVM options: -force-ordered-reductions=false`.
  - `HEADROOM_PGO=AUTO` now also turns USE on for clang Release builds for macOS arm64, unpinned.
    `HEADROOM_PGO_PIN` defaults to OFF everywhere and is kept as a diagnostic.
  - `HEADROOM_LTO` gains `AUTO` (the new default) and `PRODUCTS`.
    - `AUTO` means `PRODUCTS` for clang Release builds for macOS arm64, including the universal build (its first
      architecture is arm64), and `OFF` elsewhere.
    - `PRODUCTS` means ThinLTO on the shipped binaries only. `headroom_lto_products(TARGETS … LIBRARIES …)` gives each
      listed library a bitcode twin `<name>_lto`, with the same sources, include paths, definitions, options and links,
      compiled with `-flto=thin`. The listed targets link the twins.
    - `app/CMakeLists.txt` calls it for Headroom.app (twins of `rig_circuit_kernels` and `rig_engine`), and
      `app/plugin/CMakeLists.txt` for the AU and VST3 (twins of `rig_engine` and `headroom_plugin_core`).
    - The tests and tools keep the plain objects, so a dev build pays for one LTO back end (the app's) instead of one
      per test executable. NAM (`nam_core`) stays plain.
    - `THIN` / `FULL` still mean every target; the identity gate below builds `circuit_cli` and the tests that way.
- `packaging/macos/build-release.sh` configures the app and the plugins with `-DHEADROOM_PGO=AUTO
  -DHEADROOM_LTO=PRODUCTS`.
- Both profiles were retrained on the tree of 2026-11-15 03:47, which includes §10.51–§10.53:
  - `app/pgo/headroom-arm64.profdata`: M4 Pro, Apple clang 21, `JOBS=4`, about 97 min under load, 7.2 MB,
    17476 functions.
  - `app/pgo/headroom-x86_64.profdata`: 9900X, clang 20.1.2, `-march=x86-64-v3`, `JOBS=10`, 2003 s, 7.1 MB.
  - Each is 248 runs with no failures.

*Why one flag is enough.* AArch64 is the only target where the vectoriser turns a non-reassociable FP reduction into
an in-order vector reduction. That reduction is `fmul.4s` followed by scalar `fadd`s, so it leaves the `llvm.fmuladd`
unfused. With the flag such loops stay scalar `fmadd`, whatever inlining, peeling, trip-count estimates or LTO decide,
which is how x86-64 always compiles them.

The flag reaches more than the 1/f noise loop. In `Engine.cpp.o`, 370 of 1253 functions lose a vectorised in-order
reduction path, among them `la::solveInPlace`, `nk::spStep`, the `nk::wbStep<…>` / `wbFactor<…>` kernels,
`Circuit::linPorts` and `Circuit::ssSolve`. So the old-to-new change is not only in the noise: with `CE_NOISE=0`,
63 of 68 units still change.

**Identity: hashes of the 68 bound units, real + driven, against the plain build of the same tree.** All builds carry
the flag where they target arm64.

| Build | arm64 (M4 Pro) | x86-64-v3 (9900X) | x86_64 slice of the Mac build (Rosetta) |
|---|---|---|---|
| PGO, unpinned (AUTO) | 68 / 68 identical | 68 / 68 identical | — |
| PGO + ThinLTO (every target) | 68 / 68 identical | — | 68 / 68 identical |

- The x86_64 slice was built the way the universal build compiles it: x86_64-only builds with the arm64 profile, the
  flag and ThinLTO, against an x86_64 plain build with the flag. Both ran under Rosetta.
  - Its hashes differ from the arm64 ones, as expected. The slice is generic x86-64, without FMA, so its `fmuladd`
    never fuses.
  - The slice is now hash-gated. The universal binary itself was not run.
- **Gates.** test_circuit_engine, test_circuit_unit, test_effect_handoffs, test_aliasing and rig_tests pass on the
  plain build, PGO and PGO + ThinLTO (Mac), and on the plain build and PGO (9900X). Their output is line-identical to
  the plain build except for wall times, lane-scheduling counts and one hosted-AU timing line.
- **The products build.** The default configure builds Headroom.app from the twins (147 s wall / 521 s CPU at -j4; the
  plain `librig_engine.a` is not built). Its executable is 19.53 MB and signs.
  - The plugins build in 124 s / 408 s: AU 10.53 MB, VST3 10.59 MB.
  - `test_plugin_core`, which links the plain engine, passes.
  - Headroom.app was not launched, so its own output was not hashed. Its engine is compiled from the same bitcode as
    the `THIN` gate build's, but ThinLTO's cross-module choices depend on the executable. Identity there rests on the
    flag and on the `THIN` gates, including test_circuit_unit, which runs the units through `CircuitUnit` in
    `rig_engine`.

**The one-time change: the new arm64 output against the old.** The old build is today's tree with the old module,
without the flag. Both builds are plain (no PGO / LTO). `work/circuit-netlists/pgo/scripts/null.sh` dumps both
outputs (`CE_BENCH_DUMP`) and gives rms(new − old) / rms(old) per material.

| | Units |
|---|---|
| Bit-identical | 2 (40000, 174000); 166000's real material too |
| Null −160 dB or below (worse of the two materials) | 46 |
| −150 to −160 dB | 15 (among them 49000 JC-120, driven −150.9; 133000, real −151.5) |
| −140 to −150 dB | 3: 74000 Electric Mistress −146.8, 81000 Uberschall −149.3, 144000 Bi-Phase −144.4 |
| Worse | 2: 164000 Uni-Vibe −136.6 / −140.6 dB; 170000 EAE Surveyor −100.4 / −116.1 dB (real / driven) |

- The median is −167.8 dB over materials (−165.6 dB taking each unit's worse material). The rms nulls are per unit in `work/circuit-netlists/pgo/pgo2/null-arm64-old-vs-new.tsv`.
- **The Surveyor** is a 1/f-noise effect:
  - Changing its noise seed moves its output by −69.7 dB (real; −82.8 driven), so the build change sits 30 dB below
    the unit's own noise realisation.
  - With noise off (`CE_NOISE=0`), the old-to-new null is −142.0 / −146.4 dB.
  - The Uni-Vibe and the Bi-Phase are LFO-modulated circuits, where tiny differences carry forward in time in the
    same way.
- **The SPICE gate (test_circuit_engine).** Of its 655 NRMSE lines, 645 are unchanged to the printed digit, and every
  H1 and H2–H5 figure is unchanged.
  - The 10 changed lines are all 6G15 reverb rows, and they move in the last printed digit only.
  - One of them is an NRMSE: `g615_wet p3` 0.010 → 0.009 %. The other nine are max errors, for example `g615_drive p6`
    1.49e-02 → 1.50e-02 V and `g615_wet_drive p3` 4.90e-04 → 4.77e-04 V.
  - Also changed: the small-stone PTC worst-case Newton iterations (160 → 104 and 81 → 98, mean 2.19 / 2.14 it/sample
    unchanged), the 1176 idle drift (1.39e-12 → 5.83e-13 V at 30 s) and the speaker-coupling restore Δ (8.69e-09 →
    8.54e-09 dB).
  - These are deterministic (two runs of each build agree). They come from adaptive-step and Newton decisions that
    flip on the last bit.
  - All checks pass.

**CPU, with the retrained profiles.** `bench --real` (per-block minimum of 3 runs) on the 14 heavy units. All builds
of a unit run side by side, the start order rotates each round, and there are three rounds; the tables give the mean
over the rounds. The M4 Pro ran at a load average of about 7–28 from other work.

**M4 Pro** (3 rounds). plain: real % / driven % / p99 µs; the others: change in each, %.

| Unit | plain | PGO | PGO + ThinLTO |
|---|---|---|---|
| 5150 | 79.1 / 84.6 / 2181 | -2.9 / -3.2 / -2.7 | -5.5 / -5.9 / -5.6 |
| Uberschall | 67.9 / 71.7 / 1594 | -2.1 / -2.1 / -2.4 | -2.4 / -2.6 / -3.0 |
| BE-100 | 62.5 / 67.1 / 1302 | -1.2 / -1.4 / -2.2 | -3.5 / -3.7 / -4.4 |
| SLO-100 | 60.0 / 70.2 / 1498 | -2.0 / -2.1 / -2.4 | -3.4 / -3.7 / -3.8 |
| SLO MkII | 52.4 / 55.7 / 1098 | -2.2 / -2.3 / -1.8 | -2.7 / -2.9 / -2.8 |
| JC-120 | 92.0 / 106.1 / 2556 | -6.2 / -4.5 / -5.8 | -8.6 / -7.4 / -6.6 |
| Dual Rectifier | 48.3 / 53.8 / 1469 | -3.5 / -3.8 / -3.4 | -4.2 / -4.7 / -5.1 |
| Plexi | 41.3 / 46.8 / 1275 | -4.2 / -3.3 / -4.4 | -5.2 / -4.5 / -5.1 |
| JCM800 | 37.1 / 45.7 / 1255 | -4.7 / -4.9 / -6.2 | -5.7 / -6.0 / -6.7 |
| Twin | 33.7 / 39.9 / 904 | -1.3 / -1.9 / -2.8 | -4.5 / -5.4 / -6.3 |
| AC30 | 26.6 / 36.6 / 1057 | -6.5 / -6.7 / -7.4 | -9.1 / -8.5 / -9.3 |
| TS808 | 9.4 / 10.5 / 174 | +2.1 / +4.0 / +2.5 | -0.3 / -0.1 / -0.3 |
| Big Muff | 10.6 / 11.2 / 187 | -2.4 / -1.9 / -1.1 | -4.4 / -4.2 / -3.6 |
| HM-2 | 25.1 / 33.4 / 962 | -9.1 / -8.6 / -9.3 | -9.5 / -8.9 / -10.0 |
| **geomean** | | **-3.3 / -3.1 / -3.6** | **-5.0 / -4.9 / -5.2** |

- PGO: faster than plain in 39 of 42 unit-rounds (real), median -2.8 %
- PGO + ThinLTO: faster than plain in 40 of 42 unit-rounds (real), median -4.7 %

**9900X** (3 rounds). plain: real % / driven % / p99 µs; the others: change in each, %.

| Unit | plain | PGO |
|---|---|---|
| 5150 | 84.7 / 89.4 / 2252 | -2.0 / -1.8 / -1.4 |
| Uberschall | 69.2 / 73.3 / 1584 | -0.5 / -0.8 / -1.5 |
| BE-100 | 65.9 / 70.8 / 1353 | -2.3 / -2.3 / -2.1 |
| SLO-100 | 63.4 / 74.1 / 1557 | +0.1 / -0.7 / -1.0 |
| SLO MkII | 57.1 / 60.5 / 1165 | -2.6 / -2.7 / -2.7 |
| JC-120 | 79.9 / 98.1 / 2220 | -8.3 / -8.5 / -9.5 |
| Dual Rectifier | 47.3 / 51.0 / 1356 | -4.1 / -3.1 / -3.9 |
| Plexi | 45.9 / 52.7 / 1364 | -2.9 / -2.9 / -2.9 |
| JCM800 | 42.8 / 52.7 / 1399 | -2.5 / -2.4 / -2.3 |
| Twin | 38.0 / 45.4 / 984 | -3.3 / -3.3 / -2.7 |
| AC30 | 30.8 / 42.7 / 1203 | -0.6 / -3.1 / -3.8 |
| TS808 | 8.7 / 9.5 / 148 | -7.0 / -5.9 / -5.4 |
| Big Muff | 11.8 / 12.6 / 201 | -10.9 / -9.6 / -6.6 |
| HM-2 | 28.9 / 36.9 / 986 | -5.4 / -4.8 / -5.5 |
| **geomean** | | **-3.8 / -3.8 / -3.7** |

- PGO: faster than plain in 38 of 42 unit-rounds (real), median -2.9 %

- **arm64.** Unpinned PGO now pays −3.3 % on its own; pinned, it was −0.2 %. Together with ThinLTO it gives −5.0 %,
  against −4.2 % for pinned PGO + ThinLTO and −3.5 % for ThinLTO alone in the original runs.
  - The heavy amps gain 2–9 % (JC-120 −8.6 %, AC30 −9.1 %, 5150 −5.5 %).
  - TS808 is the one unit where PGO alone is slower (+2 %, about 0.2 % of a core), and ThinLTO cancels that out.
- **x86-64.** The retrained profile gives −3.8 %, against −4.0 / −3.4 % in the original campaigns. JC-120 gains −8.3 %.
- Raw rows: `work/circuit-netlists/pgo/pgo2/bench-arm64.tsv` and `bench-x86.tsv`.

**When to retrain, and the check.**
- Retrain both profiles after big engine changes: new device laws or Newton / solver paths in `Engine.cpp` /
  `Devices.hpp` / the generated kernels, or a new batch of bound units. The build does not warn about a stale profile
  (the `-Wno-profile-instr-*` flags silence it); a shrinking gain in `rbench.sh` is the sign.
  - A stale profile is still output-identical. It just loses gain, because the edited functions compile without a
    profile (see Staleness, above).
  - The command: `app/tools/pgo/train.sh` on the Mac for arm64, and on the 9900X for x86-64 (with
    `CXX=<clang-20> CMAKE_ARGS=-DCMAKE_CXX_FLAGS=-march=x86-64-v3 TRAIN_DI=<clips made on the Mac>`).
- **The check, on both architectures:**
  1. Build the same tree twice, `-DHEADROOM_PGO=OFF -DHEADROOM_LTO=OFF` and the shipped setting (`AUTO`, plus
     `-DHEADROOM_LTO=THIN` on arm64 so `circuit_cli` carries the LTO too).
  2. Run `work/circuit-netlists/pgo/scripts/hashes.sh <circuit_cli> <app> <out.tsv>` on each, then `diff` the two
     tables. The diff must be empty.
  3. Run the five gates (`gates.sh`).
- The same check applies to any new compiler, any new flag in `HeadroomOptimise.cmake`, and any new loop that could be
  vectorised (the flag removes the in-order hazard, but identity is still measured, not assumed).

**Settings to build with.**
- **Mac dev build (`app/build`):** `cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=Release`,
  with no PGO / LTO options. AUTO gives the arm64 profile unpinned, the flag, and ThinLTO on Headroom.app only.
  - Building everything (tests included) compiles the engine twice, once plain for the tests and tools and once as
    bitcode for the app, plus one LTO link (the app's). That replaces the one LTO back end per test executable that
    `THIN` costs.
  - An existing build directory keeps working. Its cached old defaults (`HEADROOM_PGO_PIN=ON` on arm64,
    `HEADROOM_LTO=OFF`) are recognised by their old help text and moved to `OFF` / `AUTO` at the next configure.
  - The configure should print `Headroom PGO: …/headroom-arm64.profdata` with no "(pinned…)", then `Headroom LLVM
    options: -force-ordered-reductions=false` and `Headroom LTO: ThinLTO on the shipped binaries`.
- **Release:** `packaging/macos/build-release.sh` (PGO=AUTO, LTO=PRODUCTS on the app and, with `--plugins`, on the
  AU / VST3).
- **Linux / Windows x86-64 (clang):** the defaults. PGO is USE, and AUTO leaves LTO off.
- **Identity gate builds:** `-DHEADROOM_LTO=THIN`, because `PRODUCTS` does not reach `circuit_cli`.
- Developer switches: `HEADROOM_LTO=AUTO|OFF|PRODUCTS|THIN|FULL`, and `HEADROOM_PGO_PIN=ON` (diagnostic). Data:
  `work/circuit-netlists/pgo/pgo2/` (`hash-*.tsv`, `null-arm64-old-vs-new.tsv`, `bench-*.tsv`).

### 10.55 Per-group Newton tolerance: non-tube ports of the five heaviest amps, checked against the truth (2026-11-15)

**The question.** §10.42 kept every non-tube port at 1e-6, because a uniform 1e-3 hurt the dbx detector, LFOs, zeners and
some pedals. That rule is per device class, for the whole library. §10.50 showed the JC-120 iterating at the junction
floor (two iterations a sample whatever the predictor). Can a looser tolerance be given per group, only where the
fine-step truth stays put, on the five units furthest over the §3.5 budget (5150, JC-120, Uberschall, BE-100, SLO-100)?

**Mechanism** (`Netlist.hpp`, `Netlist.cpp`, `Engine.cpp` "per-port Newton tolerance"):
- Netlist `"newton": {"groups": {"<part ref>": tol, ...}}`. Every non-tube port of the group holding that part converges at
  `tol` instead of `newtonTol` (1e-6). Tube ports keep `tubeNewtonTol` (1e-3). The group-level tests (predictor gate,
  chord gate, small-signal box, generic Newton) use the group's smallest port tolerance, as in §10.42, so a tube group's
  tests move from 1e-6 to the group value.
- DC operating points stay at 1e-6 (unchanged: `newton()` takes `min(r.tol, newtonTol)` in a DC solve).
- `CE_TOL` or a caller's `CompileOptions::newtonTol` still forces one tolerance on every port, and then the netlist's
  groups are ignored. `CompileOptions::newtonTolResolved` / `newtonTolForced` carry that decision into recompiles and
  settle helpers that copy a compiled circuit's options (without it, the unit path's second compile saw `newtonTol` set
  and ignored the groups).
- Developer A/B: `CE_TOLGROUPS="ref=x,ref=x"` replaces the netlist's list (`*=x`: every group; `none=1`: no group, which
  is the engine before this section byte for byte).
- Nothing else moves: equations, LTE control, the acceptance test and the codegen structures are unchanged, so the
  generated kernels bind as before.

**What landed** (originals in `work/circuit-netlists/cpunext/orig/`):

| Unit | `"newton": {"groups": ...}` | The ports it loosens |
|---|---|---|
| JC-120 (49000) | `D10`, `Q1`, `Q10`, `Q29_2`, `Q7`, `Q14_1`: 3e-5 | supply/regulators (g0), CH-2 preamp (g1), mixer (g6), right and left power amps (g8, g12), reverb driver (g9). Not the LFO + envelope detector (g13), LFO buffer (g14), BBD clock (g4) or the op-amp groups. |
| 5150 (134000) | `V5`: 1e-4 | the main group's flyback clamps CR200 / CR201, the OT, and (netlist v2) the Q7 clamp JFET and CR21 / CR22 |
| Uberschall (81000) | `V5A`: 1e-4 | the main group's CdS cells OC1 / OC2 / OC105, the OT |
| BE-100 (151000) | `V5`: 3e-5 | the main group's zeners Z1 / Z2, the JFETs Q1 / Q2, D5–D7, the OT |
| SLO-100 (122000) | `V6`: 1e-4 | the main group's CdS cells CELL1–4, the OT |

**Truth** (fine-step ngspice references; NRMSE % full band / 20 Hz–20 kHz; noise and hum off):
- **JC-120** (`jcspike/refs`, `jcspike/scripts/jctruth.py`; 48 node figures over four references). Every figure is within
  0.13 % of itself or better. The largest moves: `d5` Lead 0.1358 → 0.1360 (+0.13 %), `spkL` / `spkR` Lead 0.1427 /
  0.1497 → 0.1427 / 0.1497 (+0.03 / +0.02 % unrounded). Better: Rock driven `spkL` 1.0848 / 0.8959 → 1.0842 / 0.8954,
  `c15` 0.4621 → 0.4612, `tri` Lead 1.1537 → 1.1348.
  - 1e-5 is also lossless (largest move +0.05 %) for two thirds of the saving.
  - 1e-4 on all six is not: `spkL` Lead 0.1427 → 0.1433 (from the power amps), and `c84p` / `tri` driven
    0.2002 → 0.2021 / 2.3046 → 2.3113 (from the preamp, mixer and reverb groups, through the LFO's rail).
- **SLO-100** (`cutaudit/refs/slo_lead_d16`, `CE_SLOWSETTLE=0` as the references start from the old settle): spk
  0.2152 / 0.1271 → 0.2152 / 0.1270, pa 0.2345 / 0.1214 → 0.2344 / 0.1213, k3a 0.1106 → 0.1105, p4b unchanged. At 1e-3,
  k3a / p4b audio go 0.0689 / 0.0653 → 0.0691 / 0.0655: not taken.
- **BE-100** (`cutaudit/refs/be100_lead_d16`): unchanged to four digits at 3e-5, 1e-4 and 1e-3. The SPICE gate decided
  3e-5 (below).
- **5150.** A schematic revision replaced the netlist (v2) during this work, so the old `ros/refs` describe the old circuit.
  - On v1: both references unchanged to four digits at 1e-4 (Lead spk 3.0909 / 0.7839, Rock 6.0354 / 1.2651 →
    6.0352 / 1.2652; internal nodes ±0.0001), and `circuit_cli alias` −71.5 dB unchanged.
  - On v2, fresh references (`work/circuit-netlists/cpunext/refs/p5150v2_{lead,rock}_d32.npz`, T/32, from the engine's
    settled state; spk at the events' 8-sample lag): Lead spk 4.4772 / 1.2995 unchanged, Rock spk 7.9794 / 2.1056 →
    7.9796 / 2.1069; Rock `p5a` 0.2590 → 0.2603 and `k3b` 0.5119 → 0.5145, `p4a` 0.5981 → 0.5968.
  - **Those internal moves are the circuit's sensitivity, not the tolerance.** Any perturbation moves them by the same
    amount: the groups at 2e-6 (0.2600 / 0.5142), at 1e-5 or 3e-5 (0.2604 / 0.5145), or the tube ports at 0.9e-3 or
    1.1e-3 with no group change (0.2602 / 0.5143, 0.2605 / 0.5146). The renders at 2e-6 and at 1e-4 leave the shipped
    one within 50–260 samples at the same size (0.3 mV on `k3b`). The shipped figure is one draw from that spread, and
    1e-4 sits inside it. v1 showed no such spread.
  - Aliasing on v2: −65.9 dB both ways.
- **Uberschall**, a new reference (`work/circuit-netlists/cpunext/refs/uber_lead_d16.npz`: 2 s of the lead DI at T/16,
  ngspice 47, 83 min; nodes spk, pa, k3b, p2b; the deck now carries the CdS cells' internal states, which `--ic-settled`
  left out): spk 0.3069 / 0.1301 → 0.3066 / 0.1300, pa 0.3250 / 0.1288 → 0.3247 / 0.1287, k3b 0.0752 / 0.0555 →
  0.0753 / 0.0556 (the tube ports at 0.9e-3 give the same 0.0752 / 0.0556), p2b 0.0156 / 0.0152 unchanged.

**SPICE gate** (test_circuit_engine, the netlists with and without the groups, `CE_TOLGROUPS=none=1`): every NRMSE figure
is unchanged at three decimals except `jc120_clean spkR` 0.096 → 0.095 and `be100_sat ze`. The latter is the zener
node: 0.119 at 1e-6, 3e-5 and 1e-5, 0.121 at 1e-4, which is why the BE-100 takes 3e-5. Max-error columns move in the
third digit on `slo_od`, `p5150_clamp`, `uber_lead_small`, `uber_drive p3a`, `jc120_clean`, `jc120_dist vtop`.

**CPU.**
- **Mac (M4 Pro), retired instructions** (load-independent; `bench --table --real`, one run, noise and hum off; JC-120
  and 5150 v2 with `CE_NOLANES=1`, because a lane's spin-wait counts under load). G instructions per audio second
  real / driven, and the combined 64-frame block p99:

  | Unit | before | **after** | change real / driven / p99 | Newton it/sample real |
  |---|---|---|---|---|
  | JC-120 | 14.00 / 17.15, 40.7 M | **13.14 / 16.01, 37.9 M** | −6.1 / −6.6 / −6.8 % | 23.62 → 21.05 |
  | 5150 v2 | 14.16 / 15.10, 37.6 M | **13.32 / 14.17, 35.5 M** | −5.9 / −6.2 / −5.6 % | 5.11 → 4.86 |
  | Uberschall | 10.75 / 11.40, 24.7 M | **9.49 / 10.17, 21.6 M** | −11.7 / −10.8 / −12.4 % | 3.69 → 3.48 |
  | BE-100 | 10.04 / 10.77, 20.6 M | **9.59 / 10.40, 20.3 M** | −4.5 / −3.5 / −1.5 % | 5.01 → 5.01 |
  | SLO-100 | 10.36 / 12.11, 25.4 M | **8.34 / 9.77, 20.8 M** | −19.6 / −19.3 / −18.3 % | 3.93 → 3.41 |

  The BE-100's Newton count does not move: what it saves is refactorisations, as more second iterations pass the chord
  gate (which is in units of the group tolerance).
- **9900X** (`bench --table --real`, `CE_REALREPS=3`, noise and hum off; before and after from one
  build, run at once on pinned core pairs 4,6 / 8,10, swapped every round; three rounds; means, ranges in brackets).
  Real / driven % of one core, combined 64-frame p99 (µs and % of the 1333 µs period):

  | Unit | before | **after** | change real / driven / p99 |
  |---|---|---|---|
  | 5150 (v2 netlist) | 89.1 / 95.5 %, 2398 µs (180 %) | **84.3 / 90.3 %, 2287 µs (172 %)** | −5.4 / −5.5 / −4.6 % |
  | JC-120 | 66.1 / 82.0 %, 1897 µs (142 %) | **62.6 / 77.1 %, 1786 µs (134 %)** | −5.3 / −6.0 / −5.9 % |
  | Uberschall | 64.9 / 68.7 %, 1511 µs (113 %) | **60.8 / 65.1 %, 1381 µs (104 %)** | −6.2 / −5.3 / −8.6 % |
  | BE-100 (3e-5) | 60.9 / 65.7 %, 1278 µs (96 %) | **59.6 / 64.9 %, 1266 µs (95 %)** | −2.2 / −1.3 / −1.0 % |
  | SLO-100 | 59.6 / 69.6 %, 1478 µs (111 %) | **52.1 / 60.9 %, 1275 µs (96 %)** | −12.5 / −12.6 / −13.8 % |

  - Per round (`data/9900x/out*`): every unit saved in every round, except the BE-100's second round at 3e-5 (real −1.1,
    driven 0.0, p99 +0.5 %). `out/`'s 5150 rows are void: that copy of the v2 netlist ran without its generated kernels
    on a build made before v2 landed; `out2/` is the 5150 on a rebuilt tree.
  - The 5150's v2 netlist costs about 14 % more than v1 on its own (v1 on the same build: 78.0 / 82.5 %,
    p99 2117 µs). v1 with its group at 1e-4 measured −3.8 / −4.0 % instructions on the Mac.
  - The BE-100 at 1e-4 measured −2.0 / −1.6 / −1.1 % here; its Mac instruction saving does not translate into time.

**Identity.** `bench --real` hashes on the 9900X are identical with lanes off (`CE_NOLANES=1`) and with the generated
kernels off (`CE_NOCODEGEN=1`) on all five units. Every other netlist is untouched (the key is absent, so no group
changes; `CE_TOLGROUPS=none=1` reproduces the five before this section byte for byte).

**Measured and not taken.**
- **The 5150's event-local 16× (§10.48 / §10.49).** With `CE_LTEWHO` the events are the flyback: the OT's leakage
  states and winding capacitances, then the power tubes' and the inverter's states. 89 % of the event samples are
  continuations, and 90 % of those follow a sample in which a device changed regime: real nonlinear activity, chains of
  11–16 samples through each flyback.
  - LTE tolerance 24 / 32 (fewer events): alias −72.1 / −67.3 dB, but truth worse (v1 Lead spk audio 0.784 → 1.004 /
    1.010, `p4a` 0.137 → 0.143 / 0.169).
  - Continuing events only while a device changes regime (`CE_EVOS_REGIME=1`, the §10.48 variant, now on the 5150):
    every internal node worse (Rock `p5a` 0.1807 → 0.1817, `k3b` 0.2407 → 0.2416).
  - Without events at the tighter base tolerance: §10.49 measured it (tolerance 1, +8 % over events), not repeated.
  - The substep start and chord hints (§10.50's switches) on the 5150 v2: `CE_SUBPRED=26` −0.3 / −0.6 %, `CE_PREDQPORTS=4`
    and `CE_CHORDGATE=300` ±0.3 %. On the Uberschall, BE-100 and SLO-100 the chord gate 300 alone saves 5–8 % of
    instructions, but nothing on top of the group tolerance, which already scales that gate.
- **The supply groups** (rectifier bridges) at 1e-5 / 1e-4: their Newton count falls (BE-100 5.01 → 3.89 a sample) but
  the instructions do not (±0.4 %); left at 1e-6.
- **1e-3 on the tube amps' main groups**: a further −4 % (Uberschall, SLO-100) to −2 % (BE-100), but SLO-100 k3a / p4b
  and 5150 v1 spk audio move in the third digit.

**Not done here, for a later pass.** The same key could serve the other tube amps' mixed groups (Dual Rectifiers,
Ecstasy, SLO-100 MK II, JCM800, Plexi family). Each needs its own truth check: the LA-2A's and Fairchild's sidechains are
exactly the classes §10.42 kept tight.

**Files** (`work/circuit-netlists/cpunext/`): `scripts/` (`jcsweep.sh` / `jcone.sh` JC-120 truth + instructions,
`s5150.sh` / `t5150.sh` 5150 truth, alias and instructions, `tamp.sh`, `tref.py` truth against any reference, `tgamp.sh` /
`hint.sh` Mac instruction sweeps, `b9.sh` / `b9b.sh` / `ident9.sh` / `summ9.py` the 9900X A/B and identity, `mknew.py`),
`refs/` (the 5150 v2 and Uberschall references, with `refscripts/`), `data/` (every log and table above), `orig/` (the
five netlists before this section).

### 10.56 Series rectifier diodes: the OR120 and SLO-100 MK II start settled (2026-11-15)


**The finding.** §10.49's averaged-DC settle (`settleSlowRails`) fell back on two amps, so they still played their first
seconds at shifted rails: the Orange OR120 (80000) by 11.0 V (`d`, 340.1 → 329.1 V over 2.3 s) and the SLO-100 MK II
(163000) by 4.5 V (`n2`, 340.7 → 336.2 V over 1.5 s). §10.49 put this down to their "stacked" or "doubler" supplies, which
the averaged circuit did not describe. Neither supply is a doubler, and the stacked reservoirs (two capacitors in series
with balancing resistors) were not the cause either: the Dual Rectifier and Uberschall have them too, and they settle.
- **The cause: two diodes in series in each rectifier leg** (OR120 `D1A`/`D1B`, `D2A`/`D2B`; MK II `D1`/`D2`, `D3`/`D4`;
  2 × 1N400x on a centre-tapped winding).
- `mainsAveragedNetlist` takes the far terminal of a diode on the mains side as a rectifier output. Here that terminal is the
  chain's midpoint (`raw2`/`raw2b`, `d1m`/`d3m`), not the reservoir (`raw`, `n6`).
- The midpoint floats whenever its diodes are off. Its period mean is 205–265 V, about half the reservoir's 476–519 V.
- The averaged circuit was pinned there. Its DC point had the second diode reverse-biased and every rail near 0 V, so each
  step moved the states by 110–265 V (`CE_PSS_DEBUG`: 259, 265, 265, 264 V on the OR120). The steps never shrank, the
  settle undid them, and §10.52 memoised the failure.

**The fix** (`Engine.cpp`, `mainsAveragedNetlist`; `CE_DIODECHAIN=0` is the settle before, bit for bit).
- A rectifier output that only diodes touch (at least two of them, and not the circuit's input or output) is the inside
  of a series chain. It joins the mains side, and the rectifier output moves past the chain's next diode. This repeats for
  longer chains.
- The averaged circuit then pins `raw` (OR120) and `n6` (MK II) at their period means, as on every other amp. Nothing else
  in the method changes.
- A chain that leads only to ground or back to the mains side stays as it was. The JC-120's power LED (`D2PS` into `leda`,
  `LED1` to ground) is one such chain. Moving it would change the JC-120's watched rails and its state for no gain.
- **Both amps now converge in 2 steps.** The OR120's moves are 12.5 V, then 0.056 V (0.03 s); the MK II's are 16.1 V, then
  0.046 V (0.05 s).
  - At random settings (8 seeds × both channels × 4 units, `loadtime --mode rnd`, which covers the MK II's channel
    switches and the Rectifier's tube/silicon switch), 64 of 64 settles converge in 2 steps.
  - So §10.52's memo never sees a failure on these units.
- **The method needed no doubler or periodic-shooting extension.** Neither supply has pump capacitors. The series reservoirs'
  midpoints (`bm1`, `m6`) are fed only by their balancing resistors, so at DC they sit at half the rail, where the running
  circuit keeps them.
- **The only other averaged circuits it changes: both Dual Rectifiers.** Their silicon string (`D1`–`D4`, also two per
  leg) was pinned at its midpoints `d1m`/`d3m` next to the tube rectifier's `a`. It is now pinned at `sil` and `a`, which
  differ by 15 mV across the closed `SRECT` switch. The steps are the same (6.08 V, then 0.071 V). The renders null at
  −162 to −172 dB against `CE_DIODECHAIN=0`, and the drift is unchanged.

**Drift** (9900X; `work/circuit-netlists/qfix/scripts/drift.cpp`, now in `settle2/scripts/settle2.cpp`; every mains-fed
bound unit at its unit settings, then silence; per-period node means in 0.25 s windows; the figure is the largest DC node's
offset between the first window and the last, which is §10.49's figure. "Before" is the same binary with
`CE_DIODECHAIN=0`; 60 s rows are 60 s of silence):

| Unit | Before | After |
|---|---|---|
| OR120, 6 s | **11.03 V** (`d` 340.14 → 329.12, `p2b` 286.45 → 278.81, `c` 383.23 → 376.06, `p1b` 179.15 → 172.48) | **0.004 V** (`d` 328.94 → 328.94) |
| OR120, 60 s | 11.19 V | **0.005 V** |
| SLO-100 MK II, 6 s | **4.47 V** (`n2` 340.70 → 336.23, `p2b` 330.54 → 326.09, `n1`, `n3`) | **0.040 V** (`n2` 336.22 → 336.18) |
| SLO-100 MK II, 60 s | 4.52 V | **0.040 V** |
| Dual Rectifier / Rev C | 0.057 / 0.029 V | 0.057 / 0.029 V |
| BE-100 | 0.39 V (screens `s5`–`s8`, as §10.49) | unchanged |
| JCM800 / 1959RR / Plexi / variac | 0.043 / 0.052 / 0.034 / 0.016 V | unchanged |
| 6G15 / AC15 / Bassman / AC30 / Twin | 0.11 / 0.08 / 0.02 / 0.02 / 0.02 V | unchanged |
| the other 13 mains-fed amps and studio units | ≤ 0.006 V | unchanged |
| Supro 1624T | 3.26 V (`bpc`: its tremolo's load, expected) | unchanged |
| CE-1, Dimension D | 9.7 / 2.6 V (their LFO triangles, not rails) | unchanged |

**The state against 60 s of silence** (`settle2.cpp nullsettle`). The circuit at load is compared with the same circuit
after 60 s of silence. The node voltages are compared at the same mains phase (60 s is a whole number of periods), then
both render the same 3 s of plucked notes at 0.25 V (`loadtime`'s signal).

| Unit | Largest node difference, before → after | Null of the render, before → after (whole / worst 0.25 s window) |
|---|---|---|
| OR120 | `d` 12.46 V, `p2b` 8.72 V, `c` 8.36 V → `ch` 0.026 V | −51.5 / −48.0 → **−60.2 / −53.7 dB** |
| SLO-100 MK II | `n2` 4.97 V, `p2b` 4.94 V → `p5a` 0.069 V | −48.7 / −46.4 → **−68.5 / −68.9 dB** |

- The diode midpoints themselves (`raw2b`, `d1m`, `d3m`: 6–63 V) float with the junctions' state and are left out of that
  column.
- The null does not reach rounding level even for a settled amp. A high-gain amp amplifies the remaining millivolts, and
  its noise-free tube Newton paths diverge. The same measurement on amps that were already settled reads −42 (5150),
  −50.5 (JCM800), −51 to −54 (Plexi family), −64 (SLO-100, Uberschall), −66 to −72 (AC30, 5E3, Bassman, Dual Rectifier)
  and −88 to −93 dB (Fairchild, SVT). The OR120 and MK II now sit inside that range.

**Identity** (9900X; `loadtime --mode all`, 68 bindings × def / rnd / app = 204 3 s renders; one binary, default
against `CE_DIODECHAIN=0`; the default run twice was identical):
- 192 of 204 hashes are equal.
- The 12 that differ are the OR120, SLO-100 MK II, Dual Rectifier and Rev C, all three modes each: the four netlists whose
  averaged circuit changes (`circuit_cli avgnet` over the whole library).
- Against `CE_DIODECHAIN=0`, the OR120 nulls at −53 dB and the MK II at −37 to −38 dB (unit path, rnd / app settings
  included). That difference is the old start's rail error.

**Load time** (9900X; `loadtime` `prepare`, the defaults, both channels in parallel, `taskset -c 2,3`, `nice -n 10`; min of
3 reps; 3 rounds, before and after alternated, the same binary with and without `CE_DIODECHAIN=0`. "First" is the first
load in the process, before §10.52's memo knows the circuit.)

| Unit | Before | After | First load, before → after |
|---|---|---|---|
| OR120 | 0.139–0.142 s | 0.177–0.179 s | 0.233–0.240 → **0.177–0.182 s** |
| SLO-100 MK II | 0.229–0.237 s | 0.277–0.278 s | 0.340–0.349 → **0.277–0.287 s** |
| Dual Rectifier / Rev C | 0.158–0.166 / 0.296–0.302 s | 0.162 / 0.300–0.301 s | |
| all 68 bindings, summed | 7.24 s | 7.31 s | |

- **Later loads cost 0.04–0.05 s more.** Before, the memo skipped the attempt entirely, so a later load ran nothing but the
  450 ms supply run and step 2, and kept the unsettled rails. Now each load compiles the DC-only helper and takes its two
  steps (4 mains periods each).
- **The first load is 0.06 s faster on both amps,** because no failed attempt is paid for.

**Gates.**
- **9900X** (scratch build; the live tree, with other work in progress):
  - test_circuit_engine passes, including `or120_clean` / `_drive` / `_fac` / `_dimed` / `_ac` / `_ac_dimed` and
    `slo2_clean` / `_crunch` / `_od` / `_od_hot` / `_ac` / `_ac_crunch` / `_ac_od`.
  - test_circuit_unit and rig_tests pass.
  - test_effect_handoffs needs the NAM demo captures, which the scratch copy leaves out (`*.nam`); it ran on the Mac.
  - test_aliasing: 45 of 51. The Plexi, Plexi variac, JCM800 (both ids), AC30 and 1959RR fail identically with
    `CE_DIODECHAIN=0`, and those units are bit-identical between the two. The failures come from the tree, not from this
    change.
- **Mac** (`app/build-settle2`, the live tree at 10:15–10:40):
  - test_aliasing passes, 51 of 51 gated.
  - test_effect_handoffs and rig_tests pass.
  - test_circuit_engine passes every `or120_*` and `slo2_*` case. Its only failures are the `p5150_*` and `xtc_*` cases.
  - test_circuit_unit's only failures are "embedded netlist matches its JSON file" for the 5150 and the Ecstasy.
  - Both sets come from schematic edits to those two netlists (10:23), whose fixtures and
    `EmbeddedLibrary.hpp` it has not yet regenerated. Both units are bit-identical with and without this change.

**Switches.** `CE_DIODECHAIN=0`: rectifier outputs at a series chain's midpoint, as before (with it, the OR120 and MK II
fall back again).

**Files** (`work/circuit-netlists/settle2/`):
- `scripts/settle2.cpp`: §10.49's drift harness (`circuit_cli` + `drift`) plus `nullsettle` (the state and null against
  `QF_LONG` s of silence);
- `scripts/dan2.py`: the offset-at-load table from `QF_DUMP` dumps;
- `results/`: the surveys, nulls, identity hashes, timings and gate logs.

### 10.57 The 5150 netlist v2: where its accuracy and CPU went, and a band-limited truth for the output node (2026-11-15)

**The question.** The 5150's netlist v2 (the effects loop as drawn, `C_SN` removed, the Q7 / CR21 / CR22 clamp network
added; notes Addendum 9) measured about twice v1's error against fine-step ngspice (dimed sine: spk 1.75 → 2.6 %, plate
1.6 → 3.1 %; Lead / Rock spk 3.09 / 6.04 → 4.48 / 7.98 % full band) and about 14 % more CPU on the 9900X (§10.55,
78.0 → 89.1 % real). Was either an engine setting the new parts landed in, and can it be undone without losing quality?
The circuit is not up for change.

**Nothing moved in the engine's partition.** v1 and v2 are both two groups: the supply (11 unknowns, 2 ports) and the whole
rest of the amp (v1 93 unknowns / 29 ports / 7 inputs; v2 96 / 33 / 10). The clamp network joined a group that already
held everything from V1A to the speaker. Rate (192 kHz base, 16× events), LTE tolerance, start and Newton tolerance are
the same, and so are the sparse LU (cost 1750 → 1756) and the generated kernels (each bound in its own build).

**Accuracy, part 1: half of the speaker figure is the reference's.** The references are point samples, at 192 kHz, of a
waveform with content above 96 kHz. The engine's output node is a band-limited stream (§10.48), so the two differ by the
reference's folded content. Measured directly:
- New references (`cpunext/refs/p5150v2bl_{lead,rock}_d32.npz`): the same decks re-run with output at T/8, spk
  band-limited (Kaiser FIR, 91.2 kHz), the other nodes point samples. Their point samples equal the old references to
  1e-6 %.
- The point-sampled spk differs from the band-limited one by **5.50 / 1.26 %** (Lead) and **9.65 / 2.01 %** (Rock), full /
  audio. For v1 (engine at 8×, band-limited, as the stand-in) it was 3.95 / 0.93 and 7.50 / 1.20 %. Without `C_SN`, the
  power stage carries more content above 96 kHz.
- The engine against the band-limited spk: **Lead 2.10 / 0.31 %, Rock 3.62 / 0.67 %** (against the point samples:
  4.48 / 1.30, 7.98 / 2.11). v1 on the same footing: 1.70 / 0.37 and 3.10 / 0.47 %. The lead DI's audio band is better than v1;
  Rock's is 1.4 times v1.
- The engine at 16× without events, band-limited the same way, matches the band-limited ngspice to 0.036 / 0.013 % (Lead)
  and 0.066 / 0.025 % (Rock). That makes it a valid stand-in for cases without an ngspice run (the dimed sine below).
- `level/scripts/truth.py` has the two new cases (`p5150v2bl_*`; spk lag 8, nodes `pa` / `pb` added).

**Accuracy, part 2: the internal nodes are worse, and the circuit is why.** The internal nodes never pass through the
stream. Against ngspice, `p4a` (the inverter plate) reads Lead 0.158 / 0.137 → 0.262 / 0.220 % and Rock 0.371 / 0.322 →
0.597 / 0.505 % (v1 → v2). Each change was taken out on its own, and each variant was checked against its own 16× render
(audio band, NRMSE %; "dimed" is Lead, lead 1, post 1, 0.1 V 220 Hz, last 0.1 s of 0.25 s):

| Variant | Rock `p4a` / `pa` / `pb` | Lead `p4a` / `pa` / `pb` | Dimed `p4a` / `pa` / `pb` / spk (band-limited) |
|---|---|---|---|
| v1 | 0.331 / 0.558 / 0.560 | 0.143 / 0.294 / 0.271 | 0.142 / 0.379 / 0.251 / 0.269 |
| **v2** | **0.516 / 0.840 / 1.068** | **0.225 / 0.364 / 0.572** | **0.275 / 0.550 / 0.567 / 0.405** |
| v2 + `C_SN` | 0.505 / 0.752 / 0.867 | 0.216 / 0.332 / 0.440 | 0.244 / 0.434 / 0.434 / 0.343 |
| v2 − clamp network | 0.482 / 0.827 / 1.010 | 0.199 / 0.411 / 0.510 | 0.240 / 0.536 / 0.534 / 0.384 |
| v2 + `C_SN` − clamp | 0.469 / 0.702 / 0.820 | 0.192 / 0.337 / 0.392 | 0.198 / 0.344 / 0.406 / 0.310 |

- Every node up to the V3B / V3A grids is within a few per cent of v1. The growth starts at V3A's plate and the inverter
  grids, and is largest at the inverter and power plates.
- On guitar material the largest share is the loop path as drawn. It drives V3A, the inverter and the power stage about
  8 dB harder, so their edges are sharper. `C_SN` and the clamp network add the rest.
- On the dimed sine, `C_SN` and the clamp take most of it. With post at 1, Q7 and CR22 clip the CLAMP node and pass the
  edge on through C52.
- None of the three is a stiffness or convergence effect. Newton iterations are the same with and without the clamp
  network (4.86 a sample on real material).

**Where the error lives: inverter and power-stage knees at 768 kHz.** Against ngspice, `p4a` audio, Lead / Rock:

| Engine | `p4a` |
|---|---|
| shipped (192 kHz, 4-substep events) | 0.220 / 0.505 |
| LTE tolerance 12 / 8 / 4 (Rock `pb` against 16×: 1.010 / 0.974 / 0.941) | about the same |
| regime gate off (`CE_EVOS_NOGATE=1`) | unchanged |
| every sample at 4 substeps (`CE_EVOS_FORCE=1`) | 0.171 / 0.405 |
| fixed 768 kHz, no LTE (`--rate 768000`, `CE_LTE=0`) | **0.171 / 0.405**, identical |
| 768 kHz with its LTE substeps (up to 3 MHz) | 0.051 / 0.127 |
| fixed 1.536 MHz | 0.041 / 0.108 |

- The events already put 768 kHz everywhere it matters: substepping every sample gains little.
- What is left is the knees at 768 kHz. They need finer steps there.
- A tighter tolerance only adds more 4-substep events, because events are capped at M = 4. So it barely helps.
- So the engine is not mis-set for v2. The circuit as drawn has sharper knees, and the knees are where the error lives.

**Measured and not taken: 8 substeps on the sharpest events.** A 9900X-only experiment (`CE_EVOS_FINE=<r>`: an event whose
LTE ratio exceeds r takes 2M = 8 substeps on T/8 runtimes; every second substep feeds the stream). Not in the tree.
- **r = 4:** `p4a` 0.220 → 0.167 (Lead) and 0.505 → 0.414 (Rock); spk against the band-limited truth 0.308 → 0.293 and
  0.667 → 0.652. Dimed: `pa` 0.550 → 0.327, `pb` 0.567 → 0.376, `p4a` 0.275 → 0.179, spk 0.405 → 0.290 (about v1's level).
  - CPU (9900X, one binary at r = 0 and r = 4, 3 interleaved rounds): real 86.5 → 99.5 % (+15 %), driven 93.7 → 103.4 %
    (+10 %), p99 2371 → 2937 µs (+24 %).
  - Aliasing −65.9 → −63.2 dB: the mix of 4- and 8-substep events on one stream.
- **r = 16 and 64:** no change in accuracy, because few events exceed them.
- **r = 1** (every event at 8 substeps): +25 % real, −68.2 dB.
- So 8 substeps buy back the dimed figures, but on guitar material they mostly improve internal nodes, not the speaker's
  audio band, and the cost is high. If wanted later, it belongs in the netlist's `multirate` block as an opt-in.

**CPU: the clamp network's four ports.** 9900X, `bench --table 134000 --real`, `CE_REALREPS=3`, noise and hum off. Each
variant ran from its own tree with `EmbeddedLibrary.hpp` regenerated and `circuit_codegen --only 134000`, so every
variant has its kernels. Pairs ran at once on core pairs 4,6 / 8,10, swapped each round; three rounds; means:

| Variant | real / driven %, p99 µs | Newton it/sample real | against v2 |
|---|---|---|---|
| v1 | 78.6 / 83.3, 2130 | 4.67 | −7.2 / −7.6 % |
| **v2 (shipped)** | **84.7 / 90.2, 2269** | 4.86 | |
| v2 − clamp network | 79.7 / 85.0, 2141 | 4.86 | **−6.0 / −5.9 %** |
| v2 + `C_SN` | 88.7 / 94.3, 2407 | 4.92 | +4.8 / +4.3 % |

- v2 costs **+7.7 % real (+8.3 % driven, p99 +6.5 %)** over v1 at the same build. §10.55's 14 % compared two builds.
- **The clamp network is 6.0 % of that, at an identical iteration count.** The cost is per iteration. The 4 extra ports
  (Q7's two, CR21, CR22) grow every dense port-space product from 29² to 33² (`nkResidual` is about 9 % of a Mac `sample`
  profile, the Newton kernel's body about 26 %). They also add 10 columns to the linear update (W 122 → 132). The sparse
  LU hardly moves.
- The loop path as drawn costs about +6.5 % through iterations (4.67 → 4.86, the harder-driven stages). Removing `C_SN`
  saves about 5 %.
- No switch changes it. `CE_SPFULL` 2 / 8 run the same path (same hash); `CE_NOSPARSE=1` is +37 % instructions.
- **The exact lever, not built:** at idle and up to post ≈ 0.85 the four clamp ports are affine to rounding.
  - A junction more than about 1.8 V reverse (n = 1.906) has exp(v/nVt) below 1e-16, so its current is −Is (+ gmin·v) in
    floating point. CR21 and CR22 sit ≥ 9 V reverse up to post 0.8.
  - Q7 is pinched off (I_ds = 0) while CLAMP stays below about +15 V, and its gate junctions are ≥ 7 V reverse.
  - The flyback clamps CR200 / CR201 are the same except at flyback.
  - A reduced kernel without these "dormant affine" ports would remove up to the measured 6 % (more with CR200 / CR201),
    exact to rounding: the lazy-clamp scheme (§4) generalised from zero current to an affine law.
  - Each sample would solve the reduced system, check that the dormant ports' voltages stayed in their affine range, and
    fall back to the full system otherwise.
  - It needs the reduced structure in the sparse LU and the generated kernels (today lazy clamps are dense and limited to
    reduced systems of at most 12 ports). That is an engine project, not a netlist edit.

**What changed.** No netlist, engine or fixture: the 5150 renders, its kernels and `EmbeddedLibrary.hpp` are as shipped.
- `work/circuit-netlists/cpunext/refs/p5150v2bl_{lead,rock}_d32.npz` (+ README) and the two `truth.py` cases.
- The spk figure to quote for the 5150 is the band-limited one: **Lead 2.10 / 0.31 %, Rock 3.62 / 0.67 %**.
- Aliasing (`circuit_cli alias --table 134000`): −65.9 dB (ideal metric −66.5), unchanged.

**Files** (`work/circuit-netlists/p5150v2/`): `scripts/` (`cmp.py` engine at 192k·m against a reference's sample instants,
`nodes.py` / `dimed.py` per-node self-convergence against 16×, `bl.py` / `spkbl.py` / `mkbl.py` the band-limited
comparisons, `mkref8.py` the T/8-output decks, `ab9.sh` / `sum.py` the 9900X A/B, `fine.patch` the `CE_EVOS_FINE`
experiment), `data/` (the logs behind every table).

### 10.58 Idle ports: devices sitting where their laws are constant, left out of Newton exactly; measured, not landed (2026-11-15)

**The question.** §10.57 left an exact lever open: the 5150's clamp network and flyback diodes are affine to rounding
while idle, yet every Newton iteration pays for their ports. Generalised: whenever a device sits where its law is a
constant current (diodes and clamps far below conduction, a JFET pinched off, a zener below its knee, a tube cut off),
solve without it, fold its constant in, and re-admit it the moment its bound is crossed, with the result the full solve's
to rounding. How much is idle on real material across the 68 bound units, and what does leaving it out buy?

**The prize, counted** (`CE_IDLESTAT=1`: per accepted solve, which devices are idle by their law's own bounds; 9900X,
`bench --real` real + driven material). Idle ports per Newton iteration of the group, outside the existing lazy clamps:

| Unit | Group (ports) | Idle ports / iteration | Which |
|---|---|---|---|
| 5150 | 1 (33) | 7.5 (real) / 8.6 (driven) | Q7, CR21, CR22 100 %; CR200 / CR201 96–98 %; V5A 35–40 %, V4B 41–43 % |
| SSL 4000 G bus comp | 7 (52) | 17 | TR4 ×2, T15 (JFETs), five rail clamps ×2, D7 ×2: 100 % |
| dbx 160 | 20 (22) | 12 | OA6 clamps, Q9, Q10–Q13 (JFETs): 100 % |
| BOSS OC-2 | 2 (32) | 9 | four rail clamps 100 %, Q7 / Q8 + V1 / V2 50 % |
| Mu-Tron Bi-Phase | 22, 44 (28 each) | 8 each | rail clamps, 100 % |
| BOSS HM-2 | 0 (25) | 5.3 | five rail clamps 100 % |
| SSL 4000 E EQ | 6, 7 (19 each) | 4 each | F1 / F2, F4 / F5 (JFETs), 100 % |
| JC-120 | 1 (18), 6 (16) | 2, 3.4 | Q4; Q10, D7 100 %, D6 42 % |
| ~15 tube amps | supply (2) | 1.5–1.7 | rectifier diodes / vacuum diodes off 75–88 % of the time |
| Uberschall, SLO-100 (both), BE-100, Dual Rectifier (both), JCM800, Plexi, Twin | power / preamp groups | 0.1–1.2 | triodes cut off part of the cycle |

So the count is large on a handful of units. The cost it stands for is not (below).

**What was built** (behind `CompileOptions::idlePorts`; the tree no longer carries it, see the decision below):
- **The gmin split.** A junction's idle law is `c + gmin·v`, not a constant. `Circuit::compile` moves the gmin of every
  idle-capable junction (diode, zener, vacuum diode, BJT b-e / b-c, JFET g-s and g-d) into the linear network as a
  conductance across it and subtracts it from the law (`NlDevice::gShift`, the exact linear split §10.10 uses for floating islands;
  `gShiftX` carries the JFET's gate–drain term across its two ports). Not on a group below 6 ports, a WDF candidate, a
  device already split (a floating island's g0 would leave a slope) or a port at a cut node. Shot noise and the regime
  test put the gmin back, so they read the physical current and slope as before.
- **The idle region, from each law's own branches** (`NlDevice::idleAt`, `idleCurrent`): junctions below limexp's
  cut-off (`x < −40`: the exponential is exactly 0, the law exactly `c + gmin·v`); a JFET with both junctions there and
  `vgst <= 0` (level 1, `ksub = 0`); a zener on its forward branch below the cut-off; a BJT with both junctions (and its
  ISE / ISC terms) there and its ICBO tanh at −1 to the bit; a rail clamp inside its open band; a tube (tables) with the
  grid at or below the table's zero (−4 V) and z below the plate table's zero by 1e-6, or the plate at or below 0 V;
  a vacuum diode where `k·s^1.5` and its slope, times K's largest column entry, are below half an ulp of 1 V and 2^-53.
- **Idle solves** (`Circuit::idleStart`, `nk::IdleRun`, NewtonKernel.hpp). The idle set carries over from the last solve
  (checked at the start; others enter every 8th solve). The kernel then leaves idle devices unevaluated, holds their
  currents at the idle constant and their Jacobian rows at zero, and sums the residual over the other ports' columns
  (`p + K[:, idle]·c` folded once per solve). Every other operation is the full kernel's on all n rows, so the iterates
  and every convergence decision are the full solve's to rounding. Before it would evaluate the devices, each iteration
  checks every idle device is still in its region; the first one outside ends the attempt and the full solve runs from
  the same start (after the predictor correction the attempt already applied), exactly as it would have. An attempt that
  fails without leaving goes straight to the usual fallbacks (its iterates were the full solve's).
- **Checks.** `CE_IDLECHECK=1` evaluates every idle device anyway and reports any law output off its idle value beyond
  the shifted gmin's rounding: none on all 68 units (real + driven). It found one bug on the way: a tube carrying a split
  (the Bassman's V3A plate, g0 = 1e-9 S) is affine, not zero, when cut off (driven null −47.7 dB peak until excluded).
  With the residual summed over every column (`CE_IDLEFULLRES=1`) idle solves are bit-identical to the split engine
  (JC-120, Electric Mistress, Bassman).

**Why it does not pay.**
- The idle laws were already cheap. limexp returns 0 below −40 without an exponential, so a diode, JFET or BJT deep off
  evaluates in a few instructions; the tube tables exit early at cut-off; a rail clamp is a band test.
- Exactness keeps the rows. The idle ports' residual rows, step rows, limiters and step ratios decide when the full solve
  converges and whether it takes a chord; dropping them changes the result at the Newton tolerance, which on tube ports
  (1e-3) is far above rounding. So only their residual columns and evaluations go: `n·n_idle` of an `n²` residual.
- The bookkeeping (idle set, gathered columns, per-iteration box checks, a second residual routine) costs about what it
  saves. 5150 on the M4 Pro (`sample`, no PGO): residual 642 → 556 samples, triode-pair evaluations 409 → 296, but the
  residual check and the idle start add 400; Newton time on vs split-only +4 %.
- The new code has no PGO records (the profiles key on function and AST hash): in a PGO build the idle residual compiled
  cold and unvectorised. The A/B below uses `HEADROOM_PGO=OFF` builds.

**CPU, 9900X** (same binary, `CE_IDLEPORTS=1` vs off, `bench --real`, `CE_REALREPS=2`, noise and hum off, the two on
separate physical cores, swapped each round; 4 rounds; % of one core and combined block p99):

| Unit | real | driven | p99 |
|---|---|---|---|
| SSL 4000 E EQ | 18.30 → 16.24 (−11.2 %) | 18.27 → 16.40 (−10.2 %) | 298 → 253 µs (−15.1 %) |
| BOSS DS-1 | −7.3 % | −0.3 % | +1.6 % |
| JC-120 | −6.7 % | −6.5 % | −6.2 % |
| Cry Baby GCB95 | −5.5 % | −3.2 % | −5.2 % (no idle group: noise at 2.8 % of a core) |
| dbx 160 | −1.0 % | +1.1 % | +12.1 % |
| SSL 4000 G bus comp | −0.2 % | +0.7 % | −0.4 % |
| Hiwatt DR103 | +0.1 % | +0.2 % | +1.4 % |
| BOSS OC-2 | +0.4 % | +2.1 % | +2.2 % |
| 5150 | +0.4 % | −1.1 % | +1.1 % |
| Bi-Phase | +1.0 % | +2.0 % | −0.6 % |
| Leslie 122 | +3.1 % | +2.3 % | +7.6 % |
| HM-2 | +5.4 % | +2.6 % | +0.4 % |
| UREI 1176LN | +6.3 % | +1.7 % | +22.0 % (no idle solves: the split only) |

Round 0 over all 68 units (one round, noisier): real mean +1.1 %, from −13 % (SSL EQ) to +7.7 % (Leslie, before the
6-port floor). The SSL EQ's gain is the split, not idle solves: its JFET groups are lazy-clamp groups (no idle solves),
and with the gmin in the network the pinched-off JFETs' Jacobian rows are exactly zero, so their ports leave the
low-rank live set of the clamp-free Woodbury solve.

**Accuracy.**
- Fine-step truth (`level/scripts/truth.py`, all 28 cases, 108 nodes, `CE_IDLEPORTS=1` vs off): identical to 1e-6
  points everywhere (the 5150's band-limited `p5150v2bl_*` included).
- Nulls on vs off, `bench --real` (float output; one float LSB is about −137 dB against the peak): 14 of 68 identical,
  most at −140 to −220 dB rms with 1–2 LSB peaks. But the split and the reordered residual sums are rounding changes,
  and two circuits amplify them: JC-120 driven −98 dB rms / −48.7 dB peak (split alone −115 / −77, idle solves on top
  −106 / −55), Electric Mistress −112 / −76 (the split alone; its idle solves are bit-identical). Their solver decisions
  (LTE accept, chord gate) sit on thresholds that a last-bit change can cross.

**Decision: not landed.** The rule was: default off (the engine before this section, bit for bit), and a netlist opts in
with `"newton": {"idlePorts": true}` only where the 9900X shows real and p99 both at least 4 % better over the rounds and
the on / off null is at most −140 dB rms with no peak above about −120 dB. One unit qualified:
- **SSL 4000 E EQ (121000)**, final build, 3 rounds, opted in against `CE_NOIDLEPORTS=1`: real 16.86 → 15.25 % (−9.6 %),
  driven 17.03 → 15.40 % (−9.6 %), p99 251 → 228 µs (−9.1 %); null −164.6 dB rms / −141.4 dB peak (real), −165.1 /
  −133.0 (driven).
- Identity with that one opt-in: `bench --real` hashes against the binary without this section, all 68 units, noise and
  hum off: with `CE_NOIDLEPORTS=1` 68 of 68 identical; by default 67 of 68 (the SSL EQ differs, as opted).

But the code costs when it is off. The new binary with every unit off, against the binary without this section (9900X,
3 rounds, the two on separate physical cores, swapped per round):

| Unit | no PGO, real / driven / p99 | PGO (shipped profiles), real / driven / p99 |
|---|---|---|
| TS808 | +8.7 / +10.0 / +20.6 % | +8.4 / +5.9 / +7.0 % |
| DS-1 | +7.8 / +4.7 / +4.2 % | +10.7 / +14.1 / +16.7 % |
| JC-120 | +5.4 / +4.4 / +4.6 % | +8.0 / +7.5 / +9.9 % |
| 5150 | +2.2 / +2.5 / +1.5 % | −5.3 / −2.3 / −30.8 % |
| Uberschall | — | −10.3 / −4.4 / −23.1 % |
| JCM800, BE-100, Twin | — | +3.0, +1.1, +0.5 % real |

Without PGO the kernel's extra work when off (the idle branches and residual-check gathers in the hot loops, a larger
NewtonFn) costs 2–9 %. With PGO the changed kernel bodies and the renumbered generated kernels no longer match the
profiles' records, which moves units by −10 to +14 % until the profiles are retrained. A 10 % gain on one unit does not
pay for that, so the engine, the netlist and `EmbeddedLibrary.hpp` were put back exactly as they were (bit for bit the
files before this section). The implementation is kept as a patch against that tree.

Switches in the patch: `CE_NOIDLEPORTS=1` off everywhere; `CE_IDLEPORTS=1` on everywhere (developer A/B;
`CE_CG_IDLEALL=1` at build time gives every unit its kernels for that); `CE_IDLENOSOLVE=1` the split without idle solves;
`CE_IDLEFULLRES=1` idle solves on the full residual sums; `CE_IDLECHECK=1` the law check; `CE_IDLEMINPORTS` the group
floor (6); `CE_IDLESCAN` the entry scan interval (8); `CE_IDLESTAT=1` the census.

**Left open.**
- The SSL EQ's 10 % comes from the gmin split alone. A narrower change would carry it: the split on idle-capable junctions
  of lazy-clamp groups only, with no idle solves and no kernel change (so no off cost), behind the same opt-in. It would
  still need the PGO profiles retrained with it, and the null check per opted unit.
- A version that pays on the 5150 would have to drop the idle rows too, with their convergence decisions proven (a bound
  on the idle ports' steps from the active currents' change, `|d_idle| <= Σ|K_idle,A|·|Δi_A|`, settles most iterations
  without the products), and its own sparse-step and kernel structures. The split's rounding change rules out anything
  on by default for JC-120-like circuits without a bit-identical form.

**Files** (`work/circuit-netlists/idleports/`):
- `idleports.patch`: the implementation against the tree before this section. Engine.cpp ("idle ports", `idleStart`,
  `regatherIdle`, the compile pass after the partition, the census), NewtonKernel.hpp (`IdleRun`, `nkResidualIdle`),
  Devices.hpp (`idleAt`, `idleCurrent`, `gShiftX`), Codegen.hpp / circuit_codegen.cpp (the kernel argument, both
  variants), Netlist.* (`newton.idlePorts`), the SSL EQ opt-in. Apply it, then rerun `tools/circuit/embed_library.py`.
- `scripts/`: push, build, census, A/B, null, hash and law-check scripts for the 9900X test machine.
- `data/`: the census, every A/B and null table, the 68-unit hash run, the truth tables (`truth_idleon.tsv`,
  `truth_idleoff.tsv`).

### 10.59 Six pedals over budget: where the time goes, per-group Newton tolerance and one substep hint against new truth references (2026-11-15)

**The question.** The 2026-11-15 budget table (`cpu-9900x/budget-20261115.md`) has six pedals furthest over the pedal
limit (8.5 % of a 9900X core real, 13 % p99): OC-2, Bi-Phase, HM-2, GT2, Butler Tube Driver and Dyna Comp. What does each
spend its time on, and which netlist-level levers cut that without moving the true circuit's figures? The levers tried:
- §10.55's per-group Newton tolerance on non-tube groups;
- §10.50's Newton hints;
- rates, slow partitions and §10.48's events.

The engine source was not touched.

**Where the time goes** (M4 Pro, `CE_PROFILE=1`, `bench --table --real`, real DI, share of the unit's ns per sample):

| Unit (rate) | Groups | Where the time goes | Newton it/sample (sum over groups) |
|---|---|---|---|
| OC-2 (96 kHz) | 15 | The 55-unknown / 32-port octave group (IC1A … Q8, the IC3 / IC4 filters, the JFET choppers), 1.0 it a sample: 37 %. Nine 1–8-unknown groups are 57 %, at 200–330 ns each: the CMOS cells U3A / U3B / U5A (3 ports, 1.1–1.25 it), the IC2A / IC2B comparators (2.0 it), the IC1C / IC1D peak detectors (1.7 it). | 16.1 |
| Bi-Phase (96 kHz, slow partition 12 kHz) | 54 (37 slow) | The two 33-unknown / 28-port all-pass groups, 1.12 it: 44 %. They never take the small-signal box (0 of 81 tries), because their cds cells move with the LFO. The slow partition (LFOs, LED drivers, the 12 cell states): 29 %. The other 15 fast groups: 27 %, of which the PM1C / PM2C wet-path groups (1.9 it) and the supply group (1.5 it) are most. | 12.5 |
| HM-2 (192 kHz) | 1 | One group of 61 unknowns, 34 states and 25 ports, 1.89 it real / 2.19 driven; LTE substeps 0.003 / 0.026 events a sample. | 1.89 |
| GT2 (96 kHz, IC2B group 8× a 48 kHz host) | 3 | Group 0 (53 unknowns, 14 ports: the whole path to the Level amp, on the ladder at 384 kHz): 89 %, 1.85 it. | 9.3 |
| Tube Driver (768 kHz) | 4 | The tube group: 55 %. The input clamps and the two TL072 stages: 15 % each, mostly frozen. | 1.38 |
| Dyna Comp (384 kHz) | 2 | The 22-unknown / 19-port group (OTA, phase splitter, detector, Q5): 96 %, 1.16 it. | 1.16 |

**New fine-step references** (`work/circuit-netlists/pedalcpu/refs/`, ngspice 42 on the 9900X test machine, 0.5 s, `.op` start;
0.7–4 min each):
- `gt2_lead_d32`: out, o2b, o2a, o1b, o3b, o4a; T/32.
- `dyna_lead_d16`: out, env, abc, ob, q2e at 384 kHz.
- `hm2_rock_d16`: seven nodes. The old `hm2_lead_d16` has out only.
- `biph_rock_ped_d16`: Sweep A / B on Ped, so no LFO runs and the comparison is not an LFO phase race. Nodes ja, jb, j1_2, pa1o, j1_1, a9o; both inputs driven.
- `oc2_lead_d16`: out, mix, jout, bout, dout, d2.
  - Its deck needs the SPICE gate's OC-2 options (gmin 1e-10, abstol 1e-9, vntol 1e-5, gear 2, with reltol kept at 1e-5).
  - Without them, `.op` fails gmin stepping and lands on a non-physical point, with the 4.9 V reference at 90 µV.
  - With them it starts on the engine's operating point (ref 4.876 V, mix 3.364 V).
  - The flip-flop outputs bout and d2 align at a lag of one sample: the logic is evaluated after the sample.
- The GT2's ladder group reads through its half-band converter. `ptruth.py` therefore aligns its nodes at fractional lags:
  1.85 samples, and 4.4 on out / o4a.
- Engine against them, shipped, audio band: GT2 out 0.24 %, o2b 1.0 %; Dyna out 0.0038 %; HM-2 out 0.11 %; Bi-Phase ja / jb
  0.52 / 0.42 %; OC-2 out 0.21 %, jout 0.020 %, d2 2.5 %. The Tube Driver uses §10.42's `tubedriver_lead_d32` (out
  0.0022 %).

**Truth against the tolerance** (NRMSE % audio band, four significant digits; the largest move of any node and band):

| Unit, groups loosened | 1e-6 (shipped) → chosen | Largest move at the chosen value | The next step up |
|---|---|---|---|
| HM-2, its one group | `hm2_rock` c6 / n23 / o1b 0.01999 / 0.04133 / 0.0426 → 0.02 / 0.04134 / 0.04261 at **5e-5** | +0.02 % (o1b) | 1e-4: c6 +0.2 %; 1e-3: n23 0.0413 → 0.0514, c6 +17 % |
| GT2, the IC2B group | out 0.09354 / 0.2414 (full / audio) → 0.09364 / 0.2414 at **5e-5** | +0.11 % (out, full band) | 1e-4: out and o4a +0.28 % full band; 1e-3: +26 % |
| Dyna Comp, its NL group | out 0.00376 → 0.003759, env 0.002822 → 0.00282 at **3e-5** | −0.07 % | 5e-5 and 1e-4: abc +0.14 % |
| OC-2, every NL group | unchanged to four digits at **1e-4** (jout full 0.03582 → 0.03581) | 0.03 % | 1e-3: bout +0.2 %, jout full +0.3 % |
| Bi-Phase, 8 audio-path groups | unchanged to four digits at **1e-4** (and at 1e-3) | 0 | |
| Tube Driver, its non-tube groups | unchanged to four digits at **1e-4** (and at 1e-3) | 0 | |

- **The Bi-Phase's LFO is not touched.** The loosened groups are the followers Z8A / Z10A and Z9A / Z11A, the two all-pass
  groups (R4, R36) and the wet-path photocell groups (PM1C, PM2C). The sweep generators, LED drivers and the supply group
  keep 1e-6.
  - Over 19 s of the Rock DI at defaults (LFO on), tri1, tri2, lt_pm3 and lt_pm4 are bit-identical.
  - ja / jb null at −156 / −154 dB against the shipped unit.
  - Adding the supply group (CR6) perturbs the LFO (tri1 −243 dB, lt_pm3 −128 dB), and would save only 0.4 / 0.9 % more instructions. Not taken.
- **The HM-2's substep hint.** `"subPred": 26` (§10.46 / §10.50: the substep predictor without its grid-quadratic bit) is on
  top of the group tolerance. Every truth figure is identical to four digits to the tolerance alone, and aliasing is
  unchanged.
  - 26, 18 and 2 save 3–4 % of driven instructions; 26 is the most.
  - chordGate 300 and quadPorts 4 (the JC-120's other hints) measured within ±0.5 % on all six, so they were not taken.

**What landed** (one `"newton"` line per netlist after `"oversampling"`; the rest of each file is byte-identical; originals
in `work/circuit-netlists/pedalcpu/orig/`):

| Unit | `"newton"` |
|---|---|
| OC-2 (35000) | `{"groups": {"Q1", "IC1A", "IC1C", "IC1D", "IC2A", "IC2B", "U3A", "U3B", "U5A": 1e-4}}`: every NL group |
| Bi-Phase (144000) | `{"groups": {"R4", "R36", "Z8A", "Z10A", "PM1C", "PM2C", "Z9A", "Z11A": 1e-4}}` |
| HM-2 (165000) | `{"subPred": 26, "groups": {"IC1B": 5e-5}}` |
| GT2 (45000) | `{"groups": {"IC2B": 5e-5}}` |
| Tube Driver (111000) | `{"groups": {"D1", "IC1A", "IC1B": 1e-4}}`; the tube group keeps its tube ports at 1e-3 |
| Dyna Comp (48000) | `{"groups": {"Q1": 3e-5}}` |

**SPICE gate** (test_circuit_engine, each netlist old and new through `CE_GATE_FILE` / `CE_GATE_LIBDIR`): every figure is
identical except:
- gt2_hot out 1.588 → 1.586 %, max error 1.00e-2 → 9.88e-3 V;
- gt2_drive max error out 3.71 → 3.72e-3 V, o2b 4.99 → 4.98e-2 V;
- bp_drive jb worst H2–H5 0.02 → 0.01 dB.
- DC and AC cases are unchanged.

**Aliasing** (test_aliasing, `HEADROOM_ALIAS_REPORT=1`, old by `CE_TOLGROUPS=none=1 CE_NONEWTONHINTS=1`):
- HM-2 −64.9 / −64.9 dB, GT2 −61.9 / −61.9, OC-2 −99.9 / −99.9, Tube Driver −90.7 / −90.7.
- Dyna Comp −151.9 → −153.1.
- Bi-Phase −50.0 / −50.0 (report only).
- `circuit_cli alias` agrees.

**Identity.** The netlists give the same `bench --real` hashes as the A/B switch (`CE_TOLGROUPS`, `CE_SUBPRED`) on the
originals. They also match with the generated kernels off (`CE_NOCODEGEN=1`), and every kernel binds as before.

**CPU.**

| Unit | M4 retired instructions, G/s real / driven: before → after | Newton it/sample real / driven | 9900X real / driven % of one core, combined p99 µs (% of the period): before → after | change real / driven / p99 |
|---|---|---|---|---|
| OC-2 | 5.578 / 6.070 → 5.296 / 5.576 | 16.1 / 16.8 → 12.2 / 13.3 | 25.4 / 28.0, 547 (41.0 %) → **23.4 / 25.2, 499 (37.4 %)** | −7.7 / −10.1 / −8.8 % |
| Bi-Phase | 6.725 / 7.474 → 6.453 / 6.561 | 12.5 / 18.9 → 10.2 / 12.1 | 26.8 / 30.2, 485 (36.4 %) → **25.5 / 26.2, 430 (32.2 %)** | −4.8 / −13.0 / −11.3 % |
| HM-2 | 5.561 / 7.275 → 4.860 / 6.246 | 1.89 / 2.19 → 1.45 / 1.68 | 26.8 / 34.9, 963 (72.2 %) → **23.4 / 30.0, 798 (59.8 %)** | −12.4 / −14.1 / −17.1 % |
| GT2 | 5.689 / 7.063 → 4.990 / 5.902 | 9.27 / 10.56 → 6.99 / 8.07 | 24.0 / 30.4, 531 (39.9 %) → **21.1 / 25.9, 447 (33.5 %)** | −11.8 / −14.8 / −15.9 % |
| Tube Driver | 5.603 / 6.127 → 5.476 / 5.992 | 1.38 / 2.45 → 1.02 / 1.92 | 25.4 / 28.1, 437 (32.7 %) → **25.0 / 26.9, 418 (31.4 %)** | −1.9 / −4.2 / −4.2 % |
| Dyna Comp | 5.394 / 5.881 → 5.013 / 5.131 | 1.16 / 1.49 → 1.01 / 1.08 | 25.1 / 27.4, 429 (32.2 %) → **23.3 / 24.0, 375 (28.2 %)** | −7.2 / −12.5 / −12.5 % |

- **How measured.**
  - M4: `bench --table --real`, one run, noise and hum off.
  - 9900X: `CE_REALREPS=3`, noise and hum off. Old and new netlists ran at once on single pinned cores 2 and 6, swapped every round, over three rounds; means shown. Every unit saved in every round (real −1.3 … −12.5 %).
- **These 9900X figures are below the budget table's** (OC-2 37.2 / 37.0 %, Bi-Phase 34.2 / 38.4 %). That table pins a core pair at `CE_REALREPS=2` on a shared machine; the comparison here is within one run.
- **Not enough on its own.** By §3.5 (max of real / 8.5 % and p99 / 13 %) every unit is still over budget on these figures: GT2 2.6×, Dyna Comp 2.7×, OC-2 2.9×, Tube Driver 2.9×, Bi-Phase 3.0×, HM-2 4.6× (its p99). What is left is structural.

**Measured, not landed** (each trades quality for CPU, or fails a gate):
- **GT2 at a 96 kHz base with event-local 4×** (`CE_EVOS=1`): −39 / −42 % instructions. It fails aliasing at −57.9 dB (gate −60). LTE tolerance 0.5 / 1 / 4 (BDF2) gives −56.3 to −56.6. The LTE estimate misses the op-amp rail clipping (ENGINE_QUEUE).
- **Tube Driver at 192 kHz with event-local 16×** (`"minRate": 176400`, `"multirate": {"rates": {"V1A": 16}, "events": true}`):
  - −70 / −68 % instructions;
  - aliasing −66.6 dB, against −90.7 shipped; the gate passes;
  - truth out 0.0022 → 0.036 % audio band (p1 0.0010 → 0.0103 %).
  - At 384 kHz plain it fails the gate (−56.8 dB).
  - A decision for the user, not lossless.
- **Dyna Comp at 192 kHz** (`"minRate": 176400`): −47 / −46 % instructions, aliasing −109.0 dB. Truth (the reference's odd rows) out 0.0038 → 0.0057 %, env 0.0028 → 0.0056 %. A decision.
- **HM-2 at 96 kHz:** no cheaper (+9 % instructions: more iterations and substeps), and truth 0.11 → 0.48 %.
- **Bi-Phase slow partition at 6 / 3 kHz:**
  - −8 / −12 % instructions.
  - The LFO frequency against a 96 kHz slow partition moves 0.03 % (12 kHz) → 0.04 % (6 kHz) → 0.25 % (3 kHz).
  - The output's phase-drift null over 19 s goes −32 → −27 → −16 dB.
  - Not lossless.
- **Looser tolerances.** 1e-3 is lossless on the Bi-Phase's and Tube Driver's groups by these references. 1e-4 is kept as the op-amp class's ceiling: the Bi-Phase's error is all discretisation, which hides the tolerance's.

**Engine-level levers** (ENGINE_QUEUE "Pedal CPU"):
- the OC-2's tiny groups (57 % of its time at 1–2 iterations);
- the Bi-Phase's all-pass groups kept out of the small-signal box by their cds ports;
- its twelve duplicate photocell state groups;
- an event trigger for op-amp rail clipping (the GT2).

**Switches.** No new ones. A/B with `CE_TOLGROUPS=none=1` (no groups) and `CE_NONEWTONHINTS=1` (no hints): together they
reproduce the six netlists before this section, byte for byte.

**Files** (`work/circuit-netlists/pedalcpu/`):
- `refs/`: the five references, with their base decks in `refs/decks/`.
- `scripts/`:
  - `mk.sh`, `prep2.py`, `mkdeck2.py` (now drives VIN and VINR from the deck's own nodes), `fetch.sh`: decks and transfer;
  - `ptruth.py`: truth against any reference, at integer or fractional lags, `--lagscan`, `--net`;
  - `fraclag.py`, `half.py`, `lfocmp.py`;
  - `sw1.sh` / `swn.sh`: M4 instructions;
  - `b9.sh` + `summ9.py`: the 9900X A/B;
  - `mknew.py`: the netlist edits.
- `data/`: `truth/*.tsv`, `mac/` (profiles, benches, gate logs), `9900x/`.
- `orig/`, `new/`: the netlists before and after.

### 10.60 Pedal CPU, engine side: why events miss the GT2's clipping (not the detector), and unread groups skipped exactly (2026-11-15)

**The question.** §10.59 left two engine leads (ENGINE_QUEUE "Pedal CPU"): (1) event-local oversampling (§10.48) on the
SansAmp GT2 saves about 40 % but fails aliasing, blamed on the event detector missing op-amp rail clipping; (2) the
OC-2's nine tiny groups cost 200–330 ns a sample each, mostly fixed overhead.

**Lead 1: the detector is not what fails.** On the GT2 (`CE_EVOS=1`: group 0 at the 96 kHz base, 4 substeps on events)
the clamp crossings do go undetected: of 9280 clamp-band entries or exits in the gate's tone, 7356 had an LTE ratio
under 1 (`m = 1`), so no event. But firing on them changes nothing, and neither does substepping *every* sample
(`CE_EVOS_FORCE=1`), which no detector can beat: gate −56.4 dB at 0.5 FS against the ladder's −61.9. Three causes, found
in this order:
- **A backward-Euler restart every substepped sample.** `substepGroup` continues an event in plain trapezoid only with
  the BDF2 start (`cont` needs `r.bdf2`); with the default backward-Euler start every substepped sample begins on a BE
  substep. Every sample substepped therefore means a BE step every base sample: a base-rate-periodic error. On the GT2
  it is a 300 Hz line (96000 − 33·2900) at −35.3 dB on `o4a` (plain 96 kHz: −28.0; ladder: none above −72). The Plexi
  family is not affected: their netlists start on BDF2 (§10.48). With `CE_LTESTART=bdf2` the line is gone and `FORCE`
  gives −62.7 dB, better than the ladder. Note for the non-event LTE path: every continued substepped sample of a group
  on the BE start (most netlists without `"lte": {"start": "bdf2"}`) also restarts on BE (ENGINE_QUEUE).
- **The gate cannot see base-rate folds of a pedal.** `test_aliasing` / `circuit_cli alias` skip the mains-sideband bins
  around every harmonic (offsets of 1–12 × 50 or 60 Hz, ±10 Hz). A 2.9 kHz tone's folds at a 96 kHz base land 300 Hz (and
  multiples) from a harmonic, at 192 kHz 600 Hz: all skipped. That is why the line above read as "−56.4 dB at
  4100 Hz" (the 384 kHz fold of H131). Pedals have no mains; `scripts/al2.sh` reports the gate figure and the worst unmasked non-harmonic bin.
- **Seams at a 96 kHz base.** With the BDF2 start, events (not forced) on the GT2: gate −56.8 dB at 0.5 FS with a
  clamp-regime trigger, −59.5 without, −60.9 firing on any regime change; and at 0.25 FS an unmasked −45.9 dB line at
  100 Hz (a third of the 300 Hz beat between the event pattern and the base grid) in every event variant, against
  −56.4 dB for the ladder and for `FORCE`. A clamped op-amp output is a 3.8 ns mode (clamp conductance against the 30 pF
  compensation cap); between events it runs at the base step, where the trapezoid's ±1 chatter on it is undamped at
  48 kHz, and every event start and end re-excites it.

**What was built and measured** (patch `work/circuit-netlists/pedeng/evos-trigger.patch`, not landed; switches in it):
- `CE_EVOS_TRIG`: 1 a rail clamp's port changing regime starts an event whatever the LTE estimate; 2 any device port.
- `CE_EVOS_BND`: an event group's nodes that later groups read go to them through the band-limited stream (§10.48's
  output stream, generalised), not as base samples. Needed on the GT2, whose event group is not the output group.
- `CE_EVOS_UPIN`: an event group's inputs through the ladder's half-band up stages instead of linear interpolation
  across the substeps. It *worsens* truth: the IIR stages' phase (Tube Driver at 192 kHz, events, BDF2: out 0.036 → 0.108 %
  audio band even at its best fractional lag, 1.6 samples).

| GT2, 0.5 FS unless stated | gate dB | unmasked dB | M4 G instr/s real / driven |
|---|---|---|---|
| Shipped (IC2B group on the ladder, 384 kHz) | −61.9 | −61.9 (0.25 FS: −56.4) | 4.632 / 5.546 |
| Events, BE start (§10.59's figure) | −58.0 | (300 Hz line −35 dB, masked) | |
| Events, BDF2 start, clamp trigger | −56.8 | 0.25 FS: −45.9 at 100 Hz | 3.818 / 5.013 |
| Events, BDF2 start, no trigger | −59.5 | 0.25 FS: −45.9 at 100 Hz | 3.288 / 3.854 |
| Every sample substepped, BDF2 | −62.7 | 0.25 FS: −56.4 | 5.956 / 7.174 |

The other op-amp drives, as netlist variants (`scripts/mkvar.py`), truth by `scripts/etruth.py` (ptruth.py on the
reference's rows D−1::D, fractional lags):
- **Tube Driver** (shipped 768 kHz: out 0.0022 % audio band, gate −90.7, unmasked −79.1): events at 192 kHz, BE start
  0.036 % (§10.59's figure), BDF2 0.028 %, gate −66.0 / unmasked −57.8; events at 384 kHz, BDF2: 0.0066 %, −72.0 / −72.0.
  Every variant is several times further from the truth than shipped.
- **HM-2** at a 96 kHz base with events (its netlist's BDF2 start, tolerance 8): gate −69.3 / −65.4 / −61.7 at 0.25 / 0.5 / 1.0 FS, but unmasked −54.4 /
  −50.2 / −49.2 against shipped −62.4 / −64.9 / −57.2; the clamp trigger and `FORCE` are no better.
- **Dyna Comp** at 192 kHz with events: identical to plain 192 kHz (§10.59: a truth trade, 0.0038 → 0.0057 %); its group
  has no clamp.

Decision: nothing of lead 1 lands. Event-local oversampling suits the tube power stages it was built for (soft
regime changes, the output in the event group); op-amp clippers want their stiff clamp modes at the high rate all the
time.

**Lead 2: where a tiny group's time goes.** A SIGPROF sampler (`scripts/sampler.c`, `DYLD_INSERT_LIBRARIES`; symbolised
with `atos -i`, `scripts/sym.py`; `-gline-tables-only` build of the base tree) on the OC-2 bench (real + driven), share of
samples by innermost function: `processGroup` itself 12.4 %, the Newton kernel 10.9 % plus its lambdas 8.1 %,
`nkResidual` 6.1 %, the dense 3×3 / 4×4 LU (`luSmall`, `luSolveSmall`) 7.1 %, the octave group's sparse LU (`lu21_*`)
7.1 %, `newton()` 3.5 %, `lteSubsteps` 3.1 %, `limitPort` 2.8 %, the device laws 2.5 %, `pushLteHistory` 1.2 %,
`limitPredicted` 1.2 %. No line holds more than 1 % outside the kernels. The "fixed overhead" is diffuse bookkeeping
(inputs, predictor, linear rows, state update, LTE history: about 30 % of a tiny group's time); the rest is the solve
itself, one iteration of evaluations, residual, LU and limiter. A fused dispatch of tiny groups would save calls, not
this work. Two things stood out:
- **The 4013 cells never froze** (small-signal path, §10.37: 0 of 81 tries on U3A / U3B / U5A). Their output stage's
  drive port λ is a known node (its row of K is zero, so v = p exactly), but the box search halves the widest port
  for a cross term, λ included, until both ports' boxes are 2e-8 V, under the 2σ·tol room floor. Giving such ports a
  fixed box that holds their motion, never halved, and no room floor (`ss-knownport.patch`, `CE_SSKNOWN`) makes U3A's
  Q group freeze 63 % of samples, but each freeze costs 28 µs and U3B / U5A exit at once (their λ changes at every
  edge): M4 instructions +0.5 % on the OC-2. Not landed.
- **One of the nine does nothing.** U3A's Q pin is wired to nothing: its group (1 unknown, 3 ports) exports no node,
  and nothing reads it.

**What landed: unread groups are not solved** (`Engine.hpp` `Group::unread`, set in `makeGroupRt`; `runGroup`):
- A group that exports no node (no other group, logic cell, probe, rail draw or the output reads any node it solves),
  carries no cut term (draws no current from another group's cut node) and owns no cut node affects nothing outside
  itself: its states, port voltages, LTE history and noise generator are its own. `runGroup` skips its transient
  samples; its DC point is still solved at reset (so it starts where it would have).
- Exact by construction: the samples of every other group, and so every exported node, are the same operations on the
  same values. `bench --real` output hashes (real + driven material) of all 68 bound units are identical between the
  binary before this section, this one, and this one with `CE_NOUNREAD=1` (9900X, `scripts/hashes9.sh`).
- Which groups (bench / live configuration; `CE_UNREADINFO=1` lists them): the OC-2's U3A Q output; the dbx 160's
  meter movement (M2) and its indicator networks (R73 / CR6 / CR7 / CR10 with the CR6L / CR7L lamps, CR8 / CR11 / R68,
  R29 / C13); the Tremulus Lune's rate LED; the SVT's hum-balance network (FT / HUMBAL / FHUM); the SSL 4000 E dynamics'
  U1 / R75 / R76 and six T1 winding groups; the SSL 4000 G bus comp's R15 / D13 / RM; the LA-2A's NE-2 regulator lamp;
  the Bi-Phase's CR18 and CR19. The live unit adds only the LFO and GR readout probes (CircuitUnit), which these are not; an analysis instance
  (`exportAllNodes`, component-page probes, `circuit_cli info` / `render --probe`) exports them, and then they run.
- Diagnostics read differently (newton it/sample, LTE event counts leave out the skipped groups); audio does not.

| Unit | M4 retired instructions, G/s real / driven: before → after | change |
|---|---|---|
| dbx 160 | 3.379 / 3.416 → 2.983 / 3.015 | −11.7 / −11.7 % |
| Tremulus Lune | 0.775 / 0.787 → 0.716 / 0.728 | −7.6 / −7.5 % |
| OC-2 | 4.351 / 4.628 → 4.168 / 4.444 | −4.2 / −4.0 % |
| SVT | 8.369 / 9.243 → 8.085 / 8.959 | −3.4 / −3.1 % |
| SSL 4000 E dynamics | 3.949 / 4.296 → 3.878 / 4.224 | −1.8 / −1.7 % |
| SSL 4000 G bus comp | 3.773 / 4.210 → 3.751 / 4.188 | −0.6 / −0.5 % |
| Bi-Phase | 4.935 / 5.036 → 4.912 / 5.013 | −0.5 / −0.5 % |
| LA-2A | 1.086 / 1.175 → 1.082 / 1.170 | −0.4 / −0.4 % |

Off cost (M4, PGO with the shipped arm64 profile, new binary with `CE_NOUNREAD=1` or on units with no unread group,
against the binary before this section): +0.03 to +0.44 % instructions (TS808 +0.3, DS-1 +0.1, Twin +0.1, JCM800 +0.05,
5150 +0.03, HM-2 +0.1, GT2 +0.1, Tube Driver +0.4, Dyna Comp +0.2; JC-120 −0.8 / +0.05). An earlier placement, the test
at the top of `processGroup`, cost +0.5 to +1.1 %: it changed the hottest function's body, so its profile record no
longer matched (§10.58); in `runGroup` the profile of `processGroup` still applies.

**CPU, 9900X** (`scripts/b9.sh`: `bench --table --real`, `CE_REALREPS=3`, noise and hum off; the binary before this section
and this one, both PGO with the shipped x86 profile, at once on single cores 2 and 6, swapped every round; 3 rounds,
means; % of one core and the combined 64-frame block p99):

| Unit | before: real / driven %, p99 µs | after | change real / driven / p99 |
|---|---|---|---|
| dbx 160 | 16.7 / 16.9, 244 | **15.3 / 15.6, 225** | −8.0 / −7.5 / −7.9 % |
| OC-2 | 23.5 / 25.3, 500 | **22.7 / 24.6, 489** | −3.4 / −2.5 / −2.2 % |
| SVT | 51.1 / 56.9, 1026 | **49.5 / 55.1, 1006** | −3.1 / −3.1 / −2.0 % |
| Bi-Phase | 25.6 / 26.3, 432 | **24.9 / 25.6, 421** | −2.8 / −2.8 / −2.6 % |
| Tremulus Lune | 3.7 / 3.8, 55 | 3.6 / 3.7, 54 | −1.8 / −1.7 / −1.4 % |
| LA-2A | 6.6 / 7.2, 117 | 6.6 / 7.1, 117 | −0.3 / −0.8 / −0.1 % |
| SSL 4000 E dynamics | 20.9 / 23.7, 397 | 21.0 / 23.7, 399 | +0.5 / +0.3 / +0.5 % |
| SSL 4000 G bus comp | 18.7 / 21.1, 423 | 18.8 / 21.2, 425 | +0.6 / +0.6 / +0.5 % |
| *No unread group:* HM-2 | 23.8 / 30.6, 813 | 22.8 / 29.2, 776 | −4.4 / −4.5 / −4.6 % |
| Tube Driver | 25.1 / 27.0, 416 | 24.8 / 26.0, 396 | −1.5 / −3.9 / −4.7 % |
| Dyna Comp | 23.2 / 24.1, 375 | 22.5 / 23.4, 372 | −3.1 / −2.6 / −0.8 % |
| GT2 | 21.3 / 26.0, 446 | 21.7 / 26.0, 445 | +1.9 / 0.0 / −0.3 % |
| TS808 | 6.9 / 7.6, 122 | 7.0 / 7.7, 124 | +1.8 / +1.5 / +1.3 % |
| DS-1 | 14.0 / 17.1, 417 | 14.5 / 17.3, 417 | +3.7 / +1.6 / +0.2 % |
| JC-120 | 94.8 / 116.7, 2627 | 95.6 / 117.3, 2636 | +0.8 / +0.5 / +0.3 % |
| JCM800 | 39.8 / 49.6, 1339 | 39.8 / 49.8, 1340 | −0.1 / +0.4 / +0.1 % |
| Twin | 33.8 / 41.2, 916 | 33.9 / 41.1, 913 | +0.2 / −0.2 / −0.4 % |
| 5150 | 85.0 / 90.9, 2296 | 84.3 / 90.2, 2274 | −0.8 / −0.7 / −1.0 % |

- The units without an unread group move by −4.7 to +3.7 %, each consistently over the rounds. Most likely the changed
  `runGroup` lost its profile record (§10.58's effect, smaller here), so code layout and inlining around the group loop
  moved; the no-PGO A/B below and a retrained profile settle it.
- **Without PGO** (both trees `HEADROOM_PGO=OFF`, same protocol, `data/9900x/ab-nopgo`) the off cost is the branch and
  layout alone: TS808 +0.9 / −0.4 / −0.4 %, DS-1 +0.6 / +1.0 / +0.2 %, HM-2 +0.6 / +1.2 / +1.8 %, Tube Driver +0.3 / +0.7 /
  +1.0 %; and the gain: dbx 160 −12.1 / −12.3 / −13.3 %, OC-2 −3.5 / −3.8 / −2.8 %.
- Against §3.5's pedal budget (8.5 % real, 13 % p99 of the period) the six pedals stay over (OC-2 2.8×, Bi-Phase 2.9×,
  HM-2 4.5× (its p99), GT2 2.6×, Tube Driver 2.9×, Dyna Comp 2.6× on these figures); only the OC-2 and Bi-Phase had unread work.

**PGO.** `runGroup` and `makeGroupRt` changed, so their profile records no longer match until the profiles are
retrained (`app/tools/pgo/train.sh`); retrain with this section in.

**Verification.** Mac (a copy of this tree built under /tmp: during this session every new process that opened
the source folder itself stalled in `open()`, cmake included): test_circuit_engine, test_circuit_unit, test_effect_handoffs and
test_aliasing (51 of 51 gated) pass. rig_tests stalled the same way in its Audio Unit import (`describeAU`), so it ran on
The 9900X test machine (placeholder plugin host): all tests pass. The 68-unit hash identity above is on the 9900X. No
netlist changed (EmbeddedLibrary.hpp untouched).

**Left open** (ENGINE_QUEUE):
- The BE restart of every continued LTE substep sample on netlists without the BDF2 start.
- `test_aliasing`'s mains-sideband mask hides base-rate folds on units without mains: a pedal could skip only its exact
  harmonics.
- Op-amp clippers at a lower base rate need their clamp modes damped or always oversampled (an L-stable clamp
  companion, or the clamp ports alone at the high rate), not events.
- The 4013 cells' groups: an event-driven output stage (solve only near a λ edge or while the output settles) would
  need a bound on the settling tail, as the small-signal certificate gives, at less than 28 µs a freeze.

**Files** (`work/circuit-netlists/pedeng/`): `evos-trigger.patch`, `ss-knownport.patch` (measured, not landed; the second
also carries an earlier placement of the unread test), `unread.patch` (what landed), `base/` (the engine before this
section), `scripts/` (`push.sh` / `build9.sh` / `b9.sh` (`b9n.sh`: the no-PGO builds) / `hashes9.sh` for the 9900X test machine; `al2.sh` gate + unmasked
alias; `etruth.py` truth at a decimated rate with fractional lags; `mkvar.py` netlist variants; `hcmp.sh` M4 hashes and
instructions; `sampler.c` + `sym.py` the SIGPROF profile; `spec*.py`), `data/` (truth tables, the profile, the 9900X
A/B and hash tables).

### 10.61 The aliasing gate counts every bin on units without mains; the LTE start audit (2026-11-15)

**The question.** §10.60 left two items (ENGINE_QUEUE "Pedal CPU"). (A) `test_aliasing` skipped the bins it treats as
mains hum (within 10 Hz of a harmonic ± 1–12 × 50 or 60 Hz) on every unit. A 2.9 kHz tone's folds about a 96 / 192 kHz
base land 300 / 600 Hz from a harmonic, so on a pedal the mask hid exactly the base-rate folds. (B) LTE substeps restart
on a backward-Euler (BE) substep on every continued sample of a netlist without `"lte": {"start": "bdf2"}`.

**A. The gate change** (`tests/test_aliasing.cpp`, `circuit_cli alias` / `autorate`):
- A unit has mains hum when its netlist has a 45–65 Hz V source: the engine's own rule for heater hum (Engine.cpp, the
  largest such source is the hum reference), and the source of the supply ripple. Only those units skip the hum bins
  (both 50- and 60-Hz steps, as before). Every other unit counts every fold bin. No new flag was needed: all 30 mains
  units carry such a source, and no pedal or DC-supplied studio unit does.
- `HEADROOM_ALIAS_REPORT=1` now prints, per unit, the gate figure, the hum-masked figure (the old rule), an own-mains
  figure (only the unit's own 50 or 60 Hz steps skipped) and the every-bin figure. `HEADROOM_ALIAS_ONLY=<ids>` measures
  only those netlist or catalogue ids. `circuit_cli alias` follows the same rule; `CE_ALIAS_HUMMASK=1` / `0` forces the
  mask on / off for A/B.
- No exemption was added. Six gated units now fail, pending the decisions below.

**Re-measure of the 51 gated units** (9900X; dB re the tone; the gate level is 0.5 FS; 0.25 / 1.0 FS every-bin
figures to show the margin; after the netlist changes landed in B, which move only the six units marked *).
Units without mains, gate = every bin:

| Unit | 0.5 FS: hum-masked → **gate** (Hz) | 0.25 FS | 1.0 FS | Cause |
|---|---|---|---|---|
| MT-2 | −61.4 → **−36.6** (6100) | −39.0 | −35.8 | 96 kHz fold of the C027 / IC3A / IC3B clipper groups |
| JH-1 * | −65.3 → **−35.4** (300) | −35.9 | −27.1 | 96 kHz folds (C12 group at 8×, VR1B / C15 groups at 2×); BE start −31.6 |
| Rangemaster | −66.8 → **−52.4** (12200) | −63.0 | −44.5 | 192 kHz fold (one group) |
| Fuzz Face vintage Ge * | −68.2 → **−54.7** (8200) | −58.6 | −46.7 | 768 kHz fold (square-wave edges); BE start −52.8 |
| Fuzz Face Si * | −70.5 → **−55.9** (16900) | −57.6 | −50.1 | 768 kHz fold; BE start −54.5 |
| Fuzz Face Ge * | −74.5 → **−57.2** (19800) | −60.6 | −49.6 | 768 kHz fold; BE start −55.6 |
| DS-1 | −70.4 → **−60.6** (12200) | −66.5 | −56.3 | margin 9.8 → 0.6 dB: 192 kHz fold (2× rate: −79.1) |
| GT2 | −61.9 → −61.9 | −56.4 | −52.8 | unchanged at 0.5 FS (§10.60's 0.25 FS line is a fold too) |
| HM-2 | −64.9 → −64.9 | −62.4 | −57.2 | unchanged |
| RAT | −65.7 → −65.7 | −79.4 | −65.7 | unchanged |
| Big Muff | −74.3 → −67.3 | −71.5 | −64.2 | margin −7 dB |
| TS808 | −80.3 → −74.2 | −71.2 | −61.6 | margin −6 dB |
| Tube Driver | −90.7 → −79.1 | −82.1 | −67.4 | margin −12 dB |
| OC-2 | −99.9 → −79.1 | −93.4 | −50.3 | 100 Hz line, margin −21 dB |
| Bluesbreaker | −81.8 → −81.8 | −109.2 | −56.0 | unchanged |
| Harmonic Percolator | −129.9 → −81.3 | −97.5 | −79.7 | margin −49 dB |
| TS9DX | −100.2 → −89.1 | −150.0 | −76.1 | margin −11 dB |
| Klon | −153.1 → −96.7 | −145.1 | −73.5 | margin −56 dB |
| 1176 | −75.8 → −75.8 | −68.1 | −84.0 | unchanged |
| SSL G bus comp | −122.6 → −111.4 | −147.3 | −77.7 | |
| dbx 160, Crybaby ×2, Surveyor, Mu-Tron III, Dyna Comp, Micro Amp, SSL E dyn / EQ, V846 | below −130 either way | | | |

Mains units keep the mask; their gate figures are unchanged (SVT −101.0, Ecstasy −65.1, Fairchild −160.2, 5E3 −68.2,
Bassman −70.6, 6G15 * −83.5 (BE start −72.2), BE-100 −62.0, DR103 −75.2, Leslie −63.1, Plexi −75.1, variac −77.1,
1959RR −74.3, JCM800 −77.0, Rectifier −62.0, Rev C −66.9, OR120 −67.6, 5150 −65.9, SLO-100 −66.6, MK II −68.0,
AC30 −63.9).

**Causes** (`circuit_cli alias`, 0.5 FS; `scripts/exp1.sh`; noise off changes nothing, so every line is deterministic):

| Unit | shipped | LTE off | BDF2 start | 2× the rate | 2× rate, LTE off |
|---|---|---|---|---|---|
| MT-2 | −36.6 (6100) | −36.9 | −37.1 | −65.4 | −68.3 |
| JH-1 | −31.6 (300) | −33.2 | −35.4 | −51.2 (100) | −54.9 |
| Fuzz Face Ge | −55.6 | −50.4 | −57.2 | −66.1 | −69.3 |
| Fuzz Face vintage Ge | −52.8 | −49.6 | −54.7 | −64.2 | −65.8 |
| Fuzz Face Si | −54.5 | −54.5 | −55.9 | −71.2 | −67.1 |
| Rangemaster | −52.4 | −52.4 | −52.4 | −66.2 | −66.2 |
| DS-1 | −60.6 | −55.7 | −60.6 | −79.1 | −76.0 |

Every failure is a base-rate fold: the lines sit at the base rate's fold offsets (96 kHz: 300 Hz, 192 kHz: 600 Hz,
768 kHz: 500 Hz from a harmonic), and doubling the rate removes them. The LTE start moves them by under 2 dB (JH-1
3.8 dB). On the Fuzz Faces the edges are near square: harmonic 265 of a square wave is at −48 dB, so even 768 kHz folds.

**Fixes, measured; all need more CPU, so none is landed** (variants in `aliasgate/variants/`; truth = engine fine-step
reference, band-limited (below), audio-band NRMSE, Lead / Rock; CPU = 9900X, `bench --table --real`, 3 interleaved
rounds on cores 2 / 6, % change real / driven / p99 block):

| Unit | Fix | gate 0.25 / 0.5 / 1.0 FS (dB) | truth % (shipped → fix) | CPU |
|---|---|---|---|---|
| MT-2 | `"multirate": {"rates": {"IC3B": 4, "IC3A": 4, "C027": 4}}` (`autorate`) | −65.0 / −73.0 / −63.1 | 0.546 / 0.574 → 0.464 / 0.537 (ngspice out 0.568 → 0.476) | 14.7 → 18.7 %: +27.5 / +28.2 / +23.1 % |
| Fuzz Face Si | C1 at 32 with events (§10.48: 768 kHz base, 2 substeps on LTE events, BDF2 start) | −68.8 / −66.2 / −60.3 | 0.030 / 0.026 → 0.031 / 0.017 | +16.5 / +17.3 / +14.7 % |
| Fuzz Face Ge | the same | −74.4 / −69.8 / −60.0 | 0.0062 / 0.016 → 0.0035 / 0.011 | +11.8 / +12.2 / +11.0 % |
| Fuzz Face vintage Ge | the same | −67.1 / −68.3 / −57.8 | 0.012 / 0.022 → 0.0072 / 0.012 | +14.9 / +17.7 / +13.1 % |
| Rangemaster | C1 at 8 (ladder; or the whole unit at 384 kHz: same figures) | −79.9 / −66.2 / −57.9 | 0.076 / 0.158 → 0.111 / 0.225 | 2.5 → 4.5 %: +83 / +75 / +72 % |
| JH-1 | VR1B 4, C12 32, C15 4 (`autorate`) | −66.2 / −60.0 / −51.6 | not measured | 16.2 → 31.9 %: +96 / +90 / +69 % |

- Measured and worse: the Fuzz Faces with C1 at 32 on the ladder (gate −66.8 to −69.3, but +60–130 % CPU by the alias
  tool and truth 2–4× worse: the ladder's IIR converters' phase); events at a 384 kHz base (gate −56.9 to −59.9); the
  MT-2 with IC3A / IC3B only (−36.0) or with events (−37.1: its LTE does not fire on the fold); the JH-1 with events
  (−16.0) or C12 at 8 / 16 with VR1B / C15 at 4 (−54.9 / −56.7); the Rangemaster with events (no LTE events at the tone).
- The Rangemaster's truth is worse with either fix against this reference; its absolute cost is small (2.0 % of a core).

**B. The LTE start audit.** `CE_LTESTAT=1` on `bench --real` (real + driven, unit path) of all 68 bindings, with a
diagnostic counter of continued samples at the last sample's substep count (the samples a BE start restarts; patch
`aliasgate/ltestat-samem.patch`, not landed: it changes `processGroup`, so it would cost the PGO record):
- No LTE events at all: GT2, Dyna Comp, Micro Amp, dbx 160, SSL EQ, SSL bus comp.
- Already on the BDF2 start (the restart does not apply): the 24 netlists (25 bindings) with `"lte": {"start": "bdf2"}`.
- On the BE start with continued samples: 37 units (counts in `data/lte/be.sum`); the large ones are the Deluxe Reverb
  750 k, OC-2 707 k, Supro 481 k, SSL E dynamics 312 k, CE-1 288 k, JH-1 252 k, 6G15 195 k, MT-2 193 k, Fairchild 173 k,
  RE-201 133 k, Fuzz Face Ge 115 k, Mu-Tron III 114 k, Fuzz Face vintage Ge 104 k, Fuzz Face Si 73 k, TS808 55 k.

**The truth references.** ngspice references exist for eight of the 37 (MT-2, Fuzz Face Si, Big Muff, LA-2A ×2, Tube
Driver, OC-2, Bi-Phase; Phase 90 is LFO-phase-limited). For 32 of them (`scripts/ftruth.py`; not the two Crybabies, V846, Percolator and Klon, 6–76 continued samples each): the same netlist solved by
the engine at 8× the unit's circuit rate (LTE tolerance 0.25, Newton 1e-7 on every port), fed the piecewise-linear
interpolation of the unit-rate input (as the ngspice decks' PWL), noise and hum off, 0.5 s of the lead DI (from 2.0 s) and of
the rock DI at 0.35 V peak; engine sample i is fine row 8i + 7. The integrator is the only difference between a unit
run and its reference, so its error is not buried under device-model differences (the MT-2's ngspice floor is 0.61 %).
- Converged: the reference made with a BE start instead reads within 0.001 % on all but five (TS808 0.010 / 0.032,
  OC-2 0.022, Bi-Phase 0.043, Fairchild 0.28–0.49, Phase 90 0.21–0.46 %); those five are inconclusive below that.
- Point-sampled rows carry the content above R / 2 folded in, as a point-sampled engine does; a variant that removes
  aliasing (ladder, events) is compared against a band-limited reference (`refbl`: the fine output through a zero-phase
  Kaiser FIR, cutoff 0.45 R, before taking the rows; §10.57's point).
- Not comparable: units whose reference is a different time-varying system at 8× (the RE-201 tape, the 6G15 and Deluxe
  springs, the Memory Man's BBD read 14–50 % against it). The Deluxe was measured with its reverb off and the 6G15 at
  its tank drive (`tk`).

**Results** (audio-band NRMSE %, Lead / Rock, BE → BDF2; aliasing every-bin, or hum-masked on mains units, at
0.25 / 0.5 / 1.0 FS; CPU 9900X as above):

| Unit | truth | ngspice | aliasing | CPU | verdict |
|---|---|---|---|---|---|
| Fuzz Face Si | 0.023 / 0.064 → 0.0055 / 0.014 | out 0.026 → 0.012 | −58.6 / −54.5 / −48.9 → −57.6 / −55.9 / −50.1 | +0.3 / −0.5 / +0.9 % | **BDF2** |
| Fuzz Face Ge | 0.012 / 0.029 → 0.0039 / 0.0097 | | −63.2 / −55.6 / −48.4 → −60.6 / −57.2 / −49.6 | +0.3 / −1.7 / +1.5 % | **BDF2** |
| Fuzz Face vintage Ge | 0.016 / 0.041 → 0.0045 / 0.011 | | −59.9 / −52.8 / −46.2 → −58.6 / −54.7 / −46.7 | +0.6 / −0.4 / +1.5 % | **BDF2** |
| JH-1 | 0.190 / 0.419 → 0.173 / 0.344 | | −38.9 / −31.6 / −24.2 → −35.9 / −35.4 / −27.1 | −0.3 / +0.4 / −0.1 % | **BDF2** |
| Supro 1624T | 0.099 / 0.454 → 0.098 / 0.265 | | −81.6 / −76.1 / −75.1 → −86.3 / −80.2 / −82.1 | +0.2 / −1.0 / −2.0 % | **BDF2** |
| 6G15 (`tk`) | 0.692 / 1.298 → 0.693 / 0.885 | | −64.4 / −72.2 / −78.0 → −78.2 / −83.5 / −74.1; a 300 Hz line at −58.3 dB (96 kHz base fold under the hum mask) is gone | 0.0 / +0.3 / +2.1 % | **BDF2** |
| Deluxe Reverb (reverb off) | 0.151 / 0.411 → 0.111 / 0.364 | | −72.2 / −67.5 / −60.8 → −76.8 / −63.4 / −59.9; adds 192 kHz folds at −46 dB (3500, 6400, 9300 Hz, under the hum mask) | −0.1 / +3.3 / +2.7 % | BE (trade) |
| Bi-Phase | 0.090 / 0.347 → 0.065 / 0.330 (reference ±0.04) | equal | equal | +1.8 / +1.7 / +2.1 % | BE (inconclusive) |
| OC-2 | 0.686 / 0.659 → 0.728 / 0.701 | out 0.209 → 0.249 | −93.4 / −79.1 / −50.3 → −84.1 / −84.3 / −69.4 | | BE (worse) |
| Mu-Tron III | 0.105 / 0.381 → 0.100 / 0.538 | | below −135 | | BE (worse) |
| CE-1 | 0.456 / 0.643 → 0.204 / 0.836 | | −47.3 → −45.7 | | BE (mixed) |
| Memory Man | 14.0 / 17.0 → 82.8 / 96.7 (BBD; diverges) | | −30.6 → −22.9 | | BE |
| TS808 | 0.0116 / 0.0336 → 0.0115 / 0.0311 (reference ±0.03) | | −71.2 / −74.2 / −61.6 → −77.3 / −72.8 / −67.1 | | BE (inconclusive) |
| MT-2 | 0.565 / 0.585 → 0.563 / 0.584 | out 0.612 → 0.613 | −36.6 → −37.1 | | BE (neutral) |
| RAT | 0.0119 / 0.0186 → 0.0120 / 0.0187 | | equal | | BE (neutral) |
| Mistress | 0.468 / 0.999 → 0.456 / 0.980 | | equal | | BE (marginal) |
| Tube Driver | 0.0020 / 0.0066 → 0.0020 / 0.0064 | equal | −79.1 → −77.7 | | BE (neutral) |
| Big Muff, LA-2A, 1176, SSL E dynamics, Dimension D, Univibe, TS9DX, Surveyor, Bluesbreaker, Rangemaster, Small Stone, Tremulus, Crybaby ×2, Klon, Percolator, V846, Fairchild, Phase 90, RE-201 | identical to 4 digits, or not comparable | | | | BE |

**Landed: the BDF2 start on six netlists** (`"lte": {"start": "bdf2", "note": ...}`): Fuzz Face Si, Fuzz Face Ge, Fuzz
Face vintage Ge, JH-1, Supro 1624T, 6G15. EmbeddedLibrary.hpp regenerated; no ngspice fixture depends on the LTE start.

**Engine default: not proposed.** BDF2 is worse on the OC-2 (also against ngspice), the Mu-Tron III's Rock excerpt and
the Memory Man, and on the Deluxe Reverb it trades truth for a −46 dB base-rate fold; the BE start's damping is what
those units need. The Deluxe's fold is the GT2 mechanism in reverse (§10.60): trapezoid continuation leaves a power
stage's stiff mode undamped at a 192 kHz base.

**Mains units: what the mask still hides.** The base folds of 96 and 192 kHz land 300 / 600 Hz from a harmonic, which are
also 5 / 10 × 60 Hz, so on a 60 Hz unit a fold and a hum sideband share bins; the 6G15 (BE: a −58.3 dB fold at 300 Hz)
and the Deluxe with BDF2 (−46 dB at +600 Hz) are examples. The 50 Hz steps on 60 Hz units hide folds that cannot be hum:
with only the unit's own mains steps masked, the 5150 reads −55.5 dB (15000 Hz, harmonic 270 folded about its 768 kHz
event stream), Plexi −63.1, Plexi variac −62.1, 1959RR −68.4, JCM800 −73.2 (every other mains unit unchanged). The real
ripple sidebands are large (Deluxe, 5150, Supro: 2900 ± 120 Hz at −34 to −43 dB), so the mask itself is needed.

**Verification.** Mac (a /tmp copy of the tree: new processes opening the source folder still stall): test_circuit_engine,
test_circuit_unit and test_effect_handoffs pass; test_aliasing 45 of 51 gated pass, the six above fail (pending). The 9900X test machine:
test_circuit_engine, test_circuit_unit, rig_tests pass, test_aliasing identical to the Mac (test_effect_handoffs needs
the NAM demo capture, which the bundle does not carry). No engine source changed (the LTE diagnostic counter was used
for the audit only), so the PGO profiles stay valid.

**Open decisions** (ENGINE_QUEUE): the six gate failures (costs in the fix table); the own-mains mask (would fail the
5150 at −55.5 dB); a second gate tone for mains units whose base folds do not coincide with hum steps.

**Files** (`work/circuit-netlists/aliasgate/`): `variants/` (the six fixes as netlists, for the decision),
`ltestat-samem.patch`, `scripts/` (`push.sh` / `build.sh` / `send.sh` / `sendsrc.sh` for the 9900X test machine; `al.sh` +
`altab.py` the per-unit gate table; `lte.sh` + `ltesum.py` the LTE audit; `exp.sh` + `spec.py` alias experiments;
`ftruth.py` the engine fine-step references, `ntruth.py` the ngspice ones; `var.py` / `mkvars.sh` variants; `b9.sh` +
`summ9.py` the 9900X A/B), `data/` (all tables: `al/tab_*.txt`, `lte/*.sum`, `ft*/`, `nt/`, `b9a_summary.txt`).

### 10.62 The six aliasing failures fixed, the 5150's 768 kHz knee folds, and the gate's own-mains mask (2026-11-15)

**The question.** §10.61 left six gated units failing the every-bin gate (MT-2, JH-1, Rangemaster, the three Fuzz Faces),
fixes staged at +12 to +96 % CPU, and the own-mains mask open because the 5150 read −55.5 dB with it. Decision taken: fix
all six (−35 to −57 dB is audible). Bar for every fix: aliasing ≤ −63 dB (3 dB margin) at 0.25, 0.5 and 1.0 FS, truth
equal or better than shipped, then the cheapest.

**Method.** Aliasing: `circuit_cli alias` / test_aliasing at the three levels (every bin; own-mains mask on the 5150).
Truth: §10.61's engine fine-step references re-made (8× the unit's rate, LTE 0.25, Newton 1e-7, PWL input), compared
**against the band-limited `refbl`** (Kaiser FIR 0.45 R) as §10.61's fix table did; a variant at m× the unit's rate is
band-limited by the same FIR before its rows are taken (`ftruth.py` `stem@mb`; taking every m-th point sample instead
folds m× content into the band: the Rangemaster at 768 kHz reads 0.096 / 0.169 % that way and 0.0025 / 0.0075 %
band-limited). Audio-band NRMSE %, Lead / Rock. 5150: the band-limited ngspice v2 references (§10.57). CPU: 9900X,
`bench --table --real`, `CE_REALREPS=3`, noise / hum off, shipped and fixed netlist on one binary at once on cores 2 / 6,
swapped each round, 3 rounds, means; real / driven % of a core, combined 64-frame block p99 µs.

**Results** (gate dB at 0.25 / 0.5 / 1.0 FS, every bin; 5150 own-mains):

| Unit | landed | gate before → after | truth before → after | CPU before → after (real / driven %, p99 µs) |
|---|---|---|---|---|
| MT-2 | IC3B, IC3A, C027 at 4 (192 kHz), the staged fix | −39.0 / −36.6 / −35.8 → −65.0 / −73.0 / −63.1 | 0.546 / 0.574 → 0.464 / 0.537 (ngspice out 0.568 → 0.476) | 15.0 / 16.6, 259 → 19.4 / 21.7, 323 (+29.5 / +30.3 / +24.9 %) |
| Fuzz Face Si | C1 at 64 with events: 768 kHz, 4 substeps (3.07 MHz) per event | −57.6 / −55.9 / −50.1 → −83.9 / −76.0 / −69.7 | 0.030 / 0.026 → 0.030 / 0.009 | 15.0 / 12.9, 264 → 19.0 / 16.8, 330 (+27.0 / +30.4 / +25.2 %) |
| Fuzz Face Ge | the same | −60.6 / −57.2 / −49.6 → −85.7 / −74.9 / −71.2 | 0.0062 / 0.016 → 0.0030 / 0.0099 | 19.6 / 16.5, 358 → 23.7 / 20.6, 433 (+21.4 / +24.6 / +20.8 %) |
| Fuzz Face vintage Ge | the same | −58.6 / −54.7 / −46.7 → −80.8 / −74.1 / −65.7 | 0.012 / 0.022 → 0.0064 / 0.0084 | 16.2 / 13.3, 293 → 20.4 / 17.3, 363 (+25.6 / +29.5 / +23.8 %) |
| Rangemaster | the whole unit at 768 kHz (`minRate` 705600) | −63.0 / −52.4 / −44.5 → −103.1 / −80.4 / −71.0 | 0.076 / 0.158 → 0.0025 / 0.0075 | 2.5 / 2.9, 50 → 9.0 / 9.9, 157 (+261 / +244 / +216 %) |
| JH-1 | base 192 kHz (`oversampling` 4), VR1B at 8, C12 at 16 | −35.9 / −35.4 / −27.1 → −76.7 / −63.3 / −73.4 | 0.172 / 0.344 → 0.073 / 0.117 | 16.1 / 18.2, 311 → 34.5 / 38.2, 573 (+115 / +110 / +84 %) |
| 5150 | V5 at 32 with events: 8 substeps (1.536 MHz) per event | own-mains −57.5 / −55.5 / −54.6 → −66.3 / −68.1 / −67.6 | spk 0.308 / 0.667 → 0.274 / 0.602, p4a 0.220 / 0.491 → 0.107 / 0.262, pb 0.574 / 1.029 → 0.273 / 0.549 | 87.3 / 93.1, 2357 → 108.4 / 114.0, 3253 (+24.1 / +22.4 / +38.0 %) |

**What was tried first, cheaper** (each ≤ 45 min):
- MT-2: C027 alone at 4 −59.1 / −47.8 / −45.5; with IC3A −64.8 / −52.6 / −48.3; with IC3B −59.1 / −55.4 / −55.2; C027 with
  events (BDF2, tolerance 1 or 0.25) −39 / −37 / −36: the LTE never fires on an op-amp clamp's fold (§10.60). All three
  groups are needed; the whole unit at 192 kHz (−65.1 / −65.4 / −67.1) costs more.
- Fuzz Faces: the staged 2-substep events (C1 at 32) fail at 1.0 FS (Si −60.3, Ge −60.0, vintage −57.8; tolerance 1 / 0.5 /
  0.25 change < 3 dB), and so does a **fixed** 1.536 MHz with LTE substeps (Si −60.7, vintage −61.5): the folds at ±1000 Hz
  are 1.536 MHz folds of real edge content (a fixed 3.07 MHz with LTE: −68.1). Hence 4 substeps from the 768 kHz base:
  better truth than the staged fix (Si Rock 0.017 → 0.009) for about +10 points more CPU (staged: +12 to +17 %).
- Rangemaster: events never fire usefully on these edges (C1 at 8 / 16 with events, tolerance 1 / 0.25 / 0.05: at best
  −65.5 / −56.0 at 0.5 / 1.0 FS); 384 kHz reads −57.9 at 1.0 FS; C1 at 16 on the ladder passes (same figures) at 8.4 / 9.3 %,
  149 µs, but its IIR converters cost truth (0.065 / 0.152, against 0.0025 / 0.0075 at 768 kHz): 7 % more CPU for 20-30×
  closer is the trade taken. The absolute cost, 9 % of a core, is over §3.5's pedal budget (8.5 %).
- JH-1: which groups fold (0.5 FS): every op-amp group raised to 4 with C12 at 32 still reads −59.7 (192 kHz folds of
  IC1A / IC1B); the IC1A + IC1B pair (VR1B) at 8 is what removes them, IC1A or IC1B alone at 8 leaves −41.5 / −59.8; C12 needs
  768 kHz (16: −63.3 at 0.5 FS; 8: −52.2 at 1.0). On the 96 kHz base every such variant is **further from the truth**
  (VR1B 8 + C12 32: 0.216 / 0.453; the staged VR1B 4 / C12 32 / C15 4: 0.305 / 0.599): each half-band converter pair on the
  signal path adds its phase. On a 192 kHz base the same rates are closer than shipped. Measured: VR1B 8 + C12 32 on 192 kHz
  −78.7 / −76.3 / −76.3, 0.091 / 0.135 %, 42.4 / 46.1 %, 695 µs; the whole unit at 384 kHz with C12 at 32 −77.1 / −69.7 / −66.5,
  0.061 / 0.090 %, 57.1 / 61.5 %, 908 µs; the whole unit at 384 kHz alone 0.009 / 0.017 % but −60.4 / −50.7 at 0.5 / 1.0 FS
  (C12's folds). Landed the cheapest that passes, C12 at 16 (margin 3.3 dB at 0.5 FS, a 768 kHz fold at 500 Hz); C12 at 32 is
  13 dB of margin for +23 % CPU.

**The 5150's fold (own-mains −55.5 dB at 15 kHz) is integration error, not signal.** Lines at ±500 Hz from every harmonic
are folds about 768 kHz. At 0.5 FS, own-mains mask: shipped events −55.6; the ladder at 16× (`CE_EVOS=0`), every sample
substepped (`CE_EVOS_FORCE=1`), a tighter LTE (`CE_LTE=0.5`) and a fixed 768 kHz without LTE all −54.5 to −54.7; a fixed
768 kHz **with** its LTE substeps (to 3 MHz) −64.8, and a fixed 1.536 MHz −65.2. So the true waveform has no −55 dB content
near 768 kHz: the trapezoid's error at a knee depends on where the knee falls inside the 768 kHz step, a function of the
tone's phase sampled at 768 kHz, which folds (§10.57 measured the same knees as the 5150's truth error). Within an event
the 4 substeps are not error-controlled. Fix: 8 substeps per event (the stream at 1.536 MHz), which also halves the
inverter / power-plate error (table). Measured and not taken: tolerance 32 / 64 with 8 substeps (−69.9 / −60.9 dB, but spk
audio 0.428 / 0.844 and 0.867 / 1.710 %, worse than shipped). §10.57's thresholded 8 substeps (r = 4: +15 % real, +24 % p99)
mixed 4- and 8-substep events into one 768 kHz stream and was not re-measured on this mask.

**Engine change** (netlist opt-in; inert elsewhere):
- `Engine.cpp`: a group raised by the ladder takes M = 2^min(level, 3) substeps per event (was ≤ 4) when the netlist itself
  asks for events (`"multirate": {"events": true}`); such a group (`evos` 3) gets T/8 runtimes in the `[2]` sub-runtime slots
  (trapezoidal and BDF2 start; `buildGroupsRuntime`, `buildSubSparse`); the stream, interpolator and decimator were already
  sized for M = 8 (`EvosOut::kMaxM`, 81-tap decimator in the 128 ring). `CE_EVOS_MAX8=0` restores the cap of 4.
- `Netlist.cpp`: `multirate.rates` up to 64 with `"events": true` (32 otherwise); `CircuitUnit.cpp`, `circuit_cli`,
  `circuit_codegen`: an event netlist's level may go one past the oversampler's 32× (the level becomes substeps, never a
  ladder rate; `CE_EVOS=0` would run the ladder there). At a 96 kHz host the factors halve as before.
- Inert on every other netlist (none has events at level 3 or a rate over 32): `bench --real` output hashes of the Plexi,
  JCM800, AC30, 1959RR, Twin and OC-2 identical between the binary before and after; the new 5150 with `CE_EVOS_MAX8=0`
  identical to the shipped 5150. Every variant's groups bind generated kernels (`CE_CGDIAG`: 5150 50 of 50).
- **PGO:** the changed functions are compile / load time (`compile`'s ladder plan, `buildGroupsRuntime`, `buildSubSparse`,
  `buildChannel`); the hot path is unchanged, but the 5150 now runs `substepGroup` at m = 8, which the shipped profiles
  never saw. Retrain with this section in (not done here).

**The gate** (`tests/test_aliasing.cpp`, `circuit_cli alias`):
- **Own-mains mask landed.** A mains unit skips only the hum bins of its own frequency (±1–12 × 50 *or* 60 Hz); the other
  frequency's steps hid folds that cannot be hum. Re-measured all 30 mains units: at 0.5 FS every static one passes with
  it once the 5150 is fixed; the closest are the Plexi variac −62.1, Plexi −63.1, AC30 (50 Hz) −63.9, Ecstasy −65.1.
  The Leslie 122 read −30.1 dB at 3000 Hz: its rotor block's Doppler sidebands (tremolo 7.06 Hz), hidden before by the
  50 Hz steps. It is now gated with its rotors stopped (SF 0, TS 0, BS 0): −159.2 dB. `circuit_cli alias` follows
  (`CE_ALIAS_HUMMASK=1` both steps everywhere, `2` both steps on mains units as in §10.61, `0` none).
- **Second tone, 3130 Hz, reported, not gated** (`HEADROOM_ALIAS_TONE2=1` gates it; `CE_ALIAS_TONE=3130` in the CLI).
  2.9 kHz folds about 96 / 192 kHz land 300 / 600 Hz from a harmonic, a 60 Hz unit's 5th / 10th hum step. 3130 Hz is the
  tone nearest 2.9 kHz whose folds about every power-of-two rate from 48 kHz to 1.536 MHz (offsets 1050, 1030, 1070,
  990, 1150, 830 Hz) stay ≥ 20 Hz from any masked hum bin (j ≤ 12) of both mains frequencies and off every bin of
  j ≤ 20. With it six static mains units fail at 0.5 FS, all on base-rate folds the 2.9 kHz mask hides:

| Unit | 3130 Hz, 0.5 FS | fold | fix measured (alias only) |
|---|---|---|---|
| Dual Rectifier | −34.6 dB at 17750 | 96 kHz (its base, `minRate` 88200) | 192 kHz: −64.8 |
| BE-100 | −56.7 at 7330 | 192 kHz | 384 kHz: −69.1 |
| AC30 | −57.1 at 5110 | 768 kHz event stream (the 5150 mechanism) | VPA 32, 8 substeps: −79.9 |
| 5E3 | −58.6 at 10460 | 192 kHz (its 96 kHz base's 2-substep LTE) | 192 kHz: −68.4 |
| SLO-100 MK II | −58.7 at 17710 | 192 kHz | 384 kHz: −73.1 |
| Dual Rectifier Rev C | −58.9 at 19850 | 192 kHz | 384 kHz: −70.2 |

  Doubling a heavy amp's rate is roughly +40 to +100 % CPU (alias tool's thread time: Dual Rectifier 187 → 236 %, BE-100
  135 → 188 %, MK II 142 → 197 %, Rev C 226 → 280 %, 5E3 57 → 72 %, AC30 8 substeps 200 → 270 %), so none is landed: open
  decision. The Dual Rectifier's −35 dB fold has been under the hum mask since it was bound (2.9 kHz every bin: −35.0).

**Verification.** Mac (a /tmp copy of this tree; new processes opening the source folder still stall): test_circuit_engine,
test_circuit_unit (codegen 244 Lin + 105 Lu kernels), test_effect_handoffs and test_aliasing (51 of 51 gated) pass.
The 9900X test machine: test_circuit_engine, test_circuit_unit, rig_tests and test_aliasing (51 of 51) pass. EmbeddedLibrary.hpp
regenerated; no ngspice fixture depends on a netlist's rate (test_circuit_engine's cases run at their own rates, without
levels).

**Open decisions** (ENGINE_QUEUE): the six second-tone failures above (gate the 3130 Hz tone once they are fixed); the
5150's p99 (+38 %, 3253 µs, 2.4 periods) against a cheaper error-controlled event (substeps refined only at the knees); the
Rangemaster at 9 % of a core; the JH-1's C12 at 32 for margin; the Fuzz Faces and Rangemaster carry no transit time (`tf`)
in their germanium models (AC128 / OC44 fT ≈ 1-15 MHz), so their edges, and the content these rates chase, are
sharper than the real parts': a model question for the circuit owners.

**Files** (`work/circuit-netlists/aliasfix/`): `orig/` (the seven netlists and EmbeddedLibrary.hpp before), `base/` (the
engine, CLI, codegen and test sources before), `scripts/` (`push.sh` / `build.sh` / `send.sh` / `sendsrc.sh` for
the 9900X test machine; `mkv1-8.sh` variants; `al3.sh` + `exp.sh` + `spec.py` three-level alias; `e5150.sh` + `spec2.py` the gate's
masks on a dump; `ftruth.py` (`stem@mb`), `tr1.sh` / `tr2.sh` truth; `truth5150.py`; `bench2.sh` + `b9.sh` + `summ9.py` the
9900X A/B; `t2fix.sh` the second-tone fixes; `al.sh` + `altab.py` the 68-unit tables), `data/` (`al/` x = before, f / g =
after, per level; `ft/`, `tr*.txt`, `t5150*`, `b9b/`, `r*.txt`).

### 10.63 The second gate tone gated: six amps and the GT2 fixed (2026-11-16)

**The question.** §10.62 added a 3130 Hz second tone, reported only: six mains units failed it at 0.5 FS with base-rate
folds the 2.9 kHz own-mains mask hides (Dual Rectifier −34.6 dB, BE-100 −56.7, AC30 −57.1, 5E3 −58.6, SLO-100 MK II
−58.7, Rev C −58.9). Decision (2026-11-15): fix all six now, CPU second. Bar per unit: both tones ≤ −63 dB (3 dB margin)
at 0.25, 0.5 and 1.0 FS, truth equal or better than shipped, then the cheapest; then gate the second tone. Measuring the
second tone on every unit added one more failure, the GT2 (−59.2 dB, every bin, a 384 kHz fold); it is fixed the same way
(decision: fix, not exempt).

**Method.** Aliasing: `circuit_cli alias` (the gate's path, own-mains mask on mains units, every bin otherwise),
`CE_ALIAS_TONE=2900 / 3130`, `CE_ALIAS_LEVEL=0.25 / 0.5 / 1.0`; deterministic (hum and noise off change nothing). Truth:
§10.61's engine fine-step references (8× the unit's rate, 16× on the 96 kHz-base Rectifier and 5E3, 32× on the GT2; LTE
0.25, Newton 1e-7, PWL input), band-limited (`refbl`), 0.5 s of the lead DI and of the rock DI at 0.35 V peak, audio-band
NRMSE at each variant's own fractional lag; a variant at m× the base is band-limited before its rows are taken (§10.62).
The references are converged: T/8 against T/16 reads 0.002–0.007 % (BE-100, MK II, AC30). Cross-checks: the ngspice T/16
references of the AC30 (spk) and GT2 (out, T/32); the BE-100's (`cutaudit`) predates its netlist (6.2 % either way, not
used); the frozen SPICE gate of each netlist, old against new library (`CE_GATE_FILE`, `CE_GATE_LIBDIR`). CPU: 9900X,
`bench --table --real`, `CE_REALREPS=3`, noise and hum off, A (shipped) and B (fixed) at once on single cores, swapped
each round, 3 rounds, means (two streams on cores 2 / 6 and 10 / 14; round-to-round spread ≤ 3 %).

**Results** (dB re the tone, 0.25 / 0.5 / 1.0 FS; truth % audio band, Lead / Rock; CPU real / driven % of a core,
combined 64-frame block p99 µs):

| Unit | landed | 2900 Hz before → after | 3130 Hz before → after | truth before → after | CPU before → after |
|---|---|---|---|---|---|
| BE-100 | base 384 kHz (`minRate` 352800) | −71.0 / −62.0 / −57.5 → −71.2 / −73.7 / −68.0 | −61.0 / −56.7 / −50.8 → −69.5 / −69.1 / −66.5 | 0.064 / 0.157 → 0.018 / 0.041 | 62.1 / 68.4, 1371 → 117.3 / 117.9, 2023 (+89 / +72 / +48 %) |
| Dual Rectifier | events, V1A 16: 96 kHz base, 8 substeps (768 kHz) | −68.7 / −62.0 / −56.2 → −89.5 / −91.0 / −79.5 | −39.4 / −34.6 / −34.9 → −86.5 / −82.0 / −74.5 | 0.167 / 0.368 → 0.065 / 0.125 | 42.8 / 46.7, 1288 → 61.2 / 64.9, 2082 (+43 / +39 / +62 %) |
| Rev C | events, V1A 16: 192 kHz base, 4 substeps | −68.6 / −66.9 / −55.7 → −82.8 / −75.9 / −67.7 | −63.2 / −58.9 / −54.6 → −76.8 / −71.7 / −65.9 | 0.039 / 0.089 → 0.033 / 0.062 | 69.8 / 76.0, 1886 → 75.8 / 79.9, 2061 (+9 / +5 / +9 %) |
| SLO-100 MK II | events, V1A 16: 192 kHz base, 4 substeps | −70.4 / −68.0 / −61.9 → −74.5 / −73.3 / −72.2 | −63.6 / −58.7 / −61.2 → −63.6 / −64.5 / −68.3 | 0.083 / 0.105 → 0.046 / 0.080 | 53.0 / 56.2, 1113 → 56.5 / 60.3, 1253 (+7 / +7 / +13 %) |
| 5E3 | base 192 kHz (`oversampling` 4) | −78.6 / −68.2 / −65.4 → −86.0 / −77.8 / −68.5 | −63.8 / −58.6 / −56.6 → −78.3 / −68.4 / −67.3 | 0.164 / 0.538 → 0.040 / 0.138 | 10.1 / 13.7, 342 → 19.5 / 22.7, 462 (+93 / +66 / +35 %) |
| AC30 | base 384 kHz, VPA events 32 (4 substeps, 1.536 MHz), BE substep start | −66.7 / −63.9 / −57.1 → −67.0 / −64.6 / **−59.7** | −67.1 / −57.1 / −53.5 → −79.3 / −70.9 / **−61.4** | 0.063 / 0.141 → 0.019 / 0.053 (ngspice spk 0.083 → 0.058) | 29.5 / 41.5, 1202 → 55.0 / 66.4, 1643 (+86 / +60 / +37 %) |
| GT2 | base 384 kHz (`oversampling` 8), IC2B events 32 (4 substeps) | −56.4 / −61.8 / −52.8 → −72.2 / −70.4 / −65.6 | −62.8 / −59.2 / −52.5 → −72.9 / −72.5 / −67.2 | 0.128 / 0.401 → 0.035 / 0.114 (ngspice out 0.129 → 0.035) | 21.9 / 26.7, 485 → 31.5 / 36.0, 680 (+44 / +35 / +40 %) |

- Every unit passes both tones at 0.5 FS with ≥ 4.5 dB margin and is closer to the truth than shipped; every unit but the
  AC30 meets the 3 dB margin at all three levels (the MK II's closest point is 3.6 dB, at 0.25 FS on the 3130 Hz tone,
  unchanged from shipped: a 192 kHz fold too low for the LTE to fire).
- **Output level** unchanged: through the unit path (host oversampler + circuit) a 1 kHz tone at 0.1 FS and 220 Hz at
  0.5 FS move ≤ 0.002 dB on every unit; the DI excerpts at the circuit rate ≤ 0.02 dB.
- **The AC30 at 1.0 FS stays open.** At 1.0 FS every configuration tried folds at −47 to −62 dB, on a grid of 768 kHz
  multiples, at every tone from 2800 to 3300 Hz (shipped −49 to −57; 3000 Hz alone, a divisor of 768 kHz, reads −75):
  whole-unit fixed steps at 384 kHz, 768 kHz and 1.536 MHz (`CE_LTE=0`: −46.0, −57.3, −47.4 at 2900 Hz), every sample
  substepped (`CE_EVOS_FORCE=1`), tolerances 1–16, 8 substeps from a 384 kHz base (3.07 MHz). The level barely improves with
  the rate, so it is the knee-error mechanism of the 5150 (§10.62) rather than content a rate can resolve. Of the 20-odd
  variants tried, the landed one is the best at 1.0 FS among those that pass at 0.25 and 0.5 FS (−58.3 to −64.8 over
  2800–3300 Hz); the same with the BDF2 start is a
  little closer to the truth (0.017 / 0.045 %) but reads −47.9 dB at 2900 Hz 1.0 FS. Engine queue: an error-controlled
  event substep (the 5150's open item) is the likely fix.
- **The AC30's BE start moves its SPICE gate** (fixed 176.4 kHz without levels, a configuration the unit never runs):
  ac30_dimed spk 0.171 → 0.709 %, ac30_drive spk 0.030 → 0.089 % (gate 3 %). Every other gate case of the seven netlists
  is identical (rates and events do not reach it).
- **BE-100 over one core.** At 384 kHz it reads 117 % of a 9900X core (real and driven, single-threaded bench). It needs
  a lane split or engine work to run on one core; no cheaper variant passes (events at 8–32 on the 192 kHz base leave
  −56 dB base folds; LTE tol 16 or the BE start at 384 kHz fail at 1.0 FS).

**What was tried first, cheaper** (each ≤ 30 min, all measured on both tones and three levels; `data/r1.txt`, `r3.txt`):
- Rectifier: a 192 kHz base alone −55.6 at 1.0 FS (3130 Hz); events with 2 or 4 substeps fail at 1.0 FS; LTE tol 8 with
  8 substeps passes (+28 % CPU) but doubles the dimed SPICE-gate error (rect_dimed t8 1.14 → 2.21 %, pb 1.54 → 2.71 %);
  a 192 kHz base with 4 substeps is closest to the truth (0.023 / 0.059 %) for +59 % real.
- Rev C: tol 8 with events (+3 %) fails the dimed SPICE gate (revc_dimed pb 1.76 → 3.11 %, gate 3 %); 2 substeps fail at
  0.5 FS; a 384 kHz base passes at +58 % (alias tool).
- MK II: 2 substeps fail (−60.5 at 1.0 FS); BE start or tol 16 fail at 0.25 / 0.5 FS; 8 substeps −64.6 worst for about
  +60 %; a 384 kHz base −71.4 worst for about +35 % (alias tool).
- 5E3: events on the 96 kHz base (2, 4, 8 substeps, BDF2 or BE start) leave −56 to −59 dB at 1.0 FS on 3130 Hz (192 kHz
  folds); tol 32 at 192 kHz −61.7.
- GT2: IC2B at 768 kHz on the ladder passes (−67.8 worst, +86 % with a 192 kHz base) but is further from the truth (ngspice
  0.129 → 0.134 %, engine 0.425 % on Rock against 0.401: the converters' phase, §10.62's JH-1 lesson); events on the
  96 kHz base fold at −36 to −57 dB (300 Hz: 96 kHz folds); a 384 kHz base with 2 substeps −58.7 at 1.0 FS (+37 %).

**The gate** (`tests/test_aliasing.cpp`): the second tone runs on every unit and is gated by default
(`HEADROOM_ALIAS_TONE2=0` reports it only); time-varying and pending units get it with `HEADROOM_ALIAS_REPORT` only.
51 of 51 gated units pass both tones at 0.5 FS (Mac and the 9900X). Closest at 3130 Hz: DS-1 −60.6 (every bin;
2900 Hz −60.6 too), OC-2 −60.4 (a 20 Hz bin), Plexi −60.8, Plexi variac −61.5, OR120 −61.3, SLO-100 −61.9, Bassman −62.2
(own-mains mask): pass, under 3 dB of margin, not in this section's scope.
- The whole gate run at the other two levels (both tones, 9900X; for information, the gate level is 0.5 FS):
  0.25 FS 48 of 51 (SLO-100 −58.3, HM-2 −56.4, MT-2 −59.4); 1.0 FS 38 of 51 (Bluesbreaker −48.3, OC-2 −44.6 at 20 Hz,
  DS-1 −52.9, DR103 −53.1, TS808 −53.8, HM-2 −55.6, 5150 −55.8, SLO-100 −56.5, OR120 −57.7, Plexi variac −58.4, Plexi
  −58.6, Bassman −59.2, AC30 −59.7). None of these is one of the seven units of this section except the AC30 (above).

**PGO.** Netlist-only change; the AC30, BE-100, 5E3 and GT2 now run at other base rates and the Rectifiers and MK II run
`substepGroup` on events, which the shipped profiles did not see. Retrain with this section in.

**Verification.** Mac (a /tmp copy of the tree): test_circuit_engine, test_circuit_unit, test_effect_handoffs,
test_aliasing (51 of 51, both tones) and rig_tests pass. The 9900X test machine: test_circuit_engine, test_circuit_unit,
rig_tests and test_aliasing pass. EmbeddedLibrary.hpp regenerated; no ngspice fixture depends on a netlist's rate,
events or LTE start (the SPICE-gate cases compile at their own rate without levels), so none was regenerated.

**Files** (`work/circuit-netlists/tone2/`): `orig/` (the seven netlists, EmbeddedLibrary.hpp and test_aliasing.cpp
before), `scripts/` (`push.sh` / `build.sh` / `send.sh` for the 9900X test machine; `mk1.sh` … `mkg.sh` variants; `al6.sh` +
`exp.sh` + `e2.sh` + `spec.py` / `spec2.py` two-tone, three-level alias; `ftruth2.py` engine references (per-unit step,
`FOVR`), `refconv2.py` their convergence, `truthng.py` the ngspice cross-checks; `bench3.sh` / `bench4.sh` + `b9.sh` +
`summ9.py` the 9900X A/B; `lvl.sh` levels; `pull.sh` fetches results; `land.py` + `notes.json` the landed edits), `data/` (alias tables, truth,
bench rounds, gate logs).

### 10.64 Event substeps: where they go, why refining only the knees does not pay, and a looser tube-port Newton test on substeps (2026-11-16)

**The question.** §10.62 gave the 5150 8-substep events (V5's group at 32, 1.536 MHz on events) for a −55 dB knee fold
and half the inverter / power-plate error, at +24 / +22 % real / driven and +38 % block p99. Decision (2026-11-15): keep
the accuracy, win the CPU back with an engine method — refine only where it is needed (error-controlled substeps, knee
localisation, a better substep integrator, refinement limited to the ports that changed regime). Bar: the band-limited
ngspice truth (p5150v2bl, speaker and internal nodes) and aliasing on both tones at 0.25 / 0.5 / 1.0 FS no worse than
§10.62; per-netlist opt-in or bit-identical elsewhere; an env switch back; the off cost measured.

**Method.** Truth: `level/scripts/truth.py`'s p5150v2bl cases (audio-band NRMSE %, Lead / Rock; spk band-limited, lag 8);
for the Plexi family and JCM800 §10.61's engine fine-step references (8× the rate, LTE 0.25, Newton 1e-7, `refbl`).
Aliasing: `circuit_cli alias` (own-mains mask), 2900 and 3130 Hz, 0.25 / 0.5 / 1.0 FS. Event statistics: `CE_LTESTAT`
and a per-substep diagnostic (regime change per substep, the full step's LTE estimate on the substep grid; experiment
patch, not in the tree). CPU: 9900X `bench --table --real` (`CE_REALREPS=3`, noise / hum off, A and B at once on cores
2 / 6, swapped each round, 3 rounds), and M4 Pro retired instructions (`CE_BLOCKINSTR=1`: deterministic, which the
9900X was not while other 8-way renders ran: ±4 % round to round). Profile: an LD_PRELOAD SIGPROF sampler.

**Where the substeps go** (§10.62's 5150, 24 s of real DIs; driven within a few %):
- 14.7 % of base samples are event samples: 69.5 k detected on a rejected full step, 609 k continuing one (chains of
  10–16 samples). Each is 8 T/8 substeps at 1.84 Newton iterations plus a BDF2 start at 3.0: 14.9 iterations a sample,
  against 1.5 for a full step. **Events are 53 % of the unit's time.**
- Per substep: 47 % of event samples change no device's regime in any T/8 substep; the rest average 1.6 of their four
  T/4 slots with a regime change.
- **How many substeps a knee needs** (the full step's estimate on the T/8 grid, per T/4 slot): 56 % of slots are within
  tolerance at T/4, 33 % need T/8, 9 % T/16, 2 % T/32. So M8 is about right on average: it wastes a substep on half the
  slots and under-resolves one in nine.
- **The stream needs T/4 points, the integration T/8 at the knees:** T/8 integration with a 768 kHz stream (every other
  substep, the rest linear) reads −66.2 / −68.2 / −67.7 (3130 Hz −65.2 / −65.2 / −55.8), the 8-substep figures; a 384 kHz
  stream from the same substeps −62.5 / −61.3 / −61.2. The 3130 Hz 1.0 FS figure (−55.8 at 10460 Hz) is a 192 kHz base
  fold of harmonic 58, untouched by anything here.
- **The internal nodes' error is not in the events:** every sample at 8 substeps (`CE_EVOS_FORCE=1`) takes p4a Lead
  0.107 → 0.041 %; the rest is the base-rate full steps at tolerance 16.
- **Where the time goes** (profile, events and full steps): the group's sparse LU 29 % (factor 14, step 15), the Newton
  kernel 20 %, device sweeps 10 %, the linear update 14 %. A solve costs about its iterations (≈ 1.5 µs each on the 9900X).

**Error-controlled substeps, built and measured (not landed; `kneeref/variants/`).**
- *Slots:* four T/4 slots a sample (the stream's points), each one or two sub-steps by the slot-level estimate (third
  difference of pairwise means over the last five slot ends), a one-sub-step slot over a threshold rolled back and redone.
  Threshold 1: −27 % solves but −9 % iterations, alias −61.9 / −65.4 / −65.4 (knee onsets at T/4). Threshold 0.5 with
  rollback at 2: −5 % iterations, alias −66.2 / −67.9 / −67.5, truth within 1 %. Threshold 1 with rollback at 1: as many
  iterations as M8 (195 k rollbacks).
- *Variable step:* T/8, T/4 or T/2 on the dyadic grid from the estimate along the steps (third divided difference of the
  pairwise means on the actual grid), rollback, a cubic for a stream point a T/2 step passes. Threshold 1: −53.4 / −50.8 dB
  (T/2 steps on knee onsets). Threshold 0.25, rollback 1: alias −66.1 / −68.3 / −67.6 (3130 Hz −65.4 / −65.8 / −56.0), truth
  within 1 % of M8, −22 % solves, but its T/8 steps take 2.30 iterations (M8's 1.84): total iterations −1.4 %.
- *Ending an event once all four slots are within tolerance at T:* −2.6 % event samples (−8 % at 4× the tolerance, Rock
  pb +1.8 %). The continuation gate on regime (`CE_EVOS_REGIME=1`): −7.5 % event samples, truth ±1 %.
- **Why it cannot pay here:** M8's substeps on smooth slots are its cheap ones (1.48 iterations), on knee slots 2.36.
  Refinement keeps every knee solve and trades two cheap T/8 solves (2.96 iterations) for one T/4 solve (2.05); the
  onsets it misses must be rolled back. Ceiling about one iteration per smooth slot, under 8 % of the unit; measured
  1–5 %. Knee localisation (a step boundary on the knee) needs runtimes at arbitrary step sizes (a DK refresh per knee) or
  dyadic bisection (the same solves); a higher-order or L-stable step does not touch a kink's O(h²) error (§10.47).
- Also measured: the cross-sample chord (`CE_XCHORD=big`) does not engage on the 33-port sparse group (no change), chord
  gates 10 / 100 / 1000 cost +0.3 to +0.9 % instructions.

**What pays: the substeps' Newton test.** A substep's Newton starts a T/8 step away on the substep predictor, so its first
Newton step lands far inside the tolerance and the second iteration mostly confirms it. Scaling the substep runtimes'
tolerances (5150, M8; T/8 iterations; alias 2900 Hz; truth):

| Substep tolerance | T/8 it | 2900 Hz dB | p5150_clamp gate | truth |
|---|---|---|---|---|
| 1× (§10.62) | 1.84 | −66.3 / −68.2 / −67.6 | spk 2.81 % | spk 0.274 / 0.602, p4a 0.107 / 0.262 |
| all ports ×10 | 1.48 | −66.9 / −68.3 / −67.4 | **spk 6.56 %, clamp 16.6 % (> 3 %)** | equal or better |
| all ports ×30 / ×100 | 1.34 / 1.28 | −63.9 / −57.5 at 0.5 FS | | ×100: spk +54 % |
| **tube ports ×10** | **1.60** | **−66.8 / −68.1 / −67.3** | **spk 2.81 %** | **equal (4th digit)** |
| tube ports ×30 | 1.53 | −66.6 / −68.3 / −67.9 | 2.80 % | p4a +0.3 % |
| tube + speaker ×10 | 1.60 | as tube ×10 | | as tube ×10 |
| tube ×10 on the full step too | | −68.4 / −66.8 / −67.2 | | k3b +9 %, p5a +10 % |

- Junction ports cannot take it (§10.42: their law bends on n·Vt): the clamp network (Q7, CR21 / CR22) misses its SPICE
  gate at 1e-3. Tube ports converge at 1e-2 on substeps with every figure unchanged; the full step keeps 1e-3.
- Landed: **`"newton": {"substeps": k}`** scales the tube ports' tolerance of every LTE substep runtime (T/2, T/4, T/8 and
  their BDF2 / backward-Euler starts) by k; every other port and the full step keep theirs. k = 10 (×30 buys 1 % more for
  0.3 % on p4a and 0.1 dB on the second tone).

**Results, 5150** (shipped §10.62 → `newton.substeps` 10; pre-§10.62 = 4-substep events):

| | pre-§10.62 | §10.62 | §10.64 |
|---|---|---|---|
| 2900 Hz, 0.25 / 0.5 / 1.0 FS | −57.5 / −55.6 / −54.6 | −66.3 / −68.2 / −67.6 | −66.8 / −68.1 / −67.3 |
| 3130 Hz | −56.3 / −53.8 / −55.5 | −65.3 / −65.2 / −55.8 | −65.3 / −65.2 / −55.8 |
| truth Lead: spk / p4a / pa / pb / k3b | 0.308 / 0.220 / 0.364 / 0.574 / 0.109 | 0.274 / 0.1067 / 0.2223 / 0.2727 / 0.0856 | 0.274 / 0.1068 / 0.2222 / 0.2725 / 0.0856 |
| truth Rock | 0.667 / 0.491 / 0.808 / 1.029 / 0.251 | 0.602 / 0.2618 / 0.4854 / 0.5491 / 0.2016 | 0.602 / 0.2618 / 0.4854 / 0.5490 / 0.2015 |
| SPICE gate dimed / clamp spk | | 0.292 / 2.807 % | 0.293 / 2.806 % |
| event iterations a sample (substeps / start) | 10.1 (T/4 2.50 / 3.70) | 14.9 (T/8 1.84 / 2.99) | 12.9 (T/8 1.60 / 2.62) |
| M4 Pro instructions, block mean / p99 | | 22.67 M / 49.27 M | 21.97 M / 46.68 M (−3.1 / −5.3 %) |
| 9900X real / driven %, block p99 µs (this run) | 86.2 / 91.6, 2320 | 107.1 / 112.9, 3218 | 104.3 / 109.7, 3078 (−2.6 / −2.9 / −4.4 %) |

**Other units** (`CE_SUBTOL=10` against 1×; engine fine-step truth `refbl`, audio, Lead / Rock; alias both tones):

| Unit | 9900X real / driven / p99 | instructions mean / p99 | truth | alias, worst move | taken |
|---|---|---|---|---|---|
| Plexi | −3.6 / −4.8 / −8.4 % | −4.5 / −9.6 % | 0.0756 / 0.1257 → 0.0756 / 0.1259 | 0.1 dB (0.5 FS: −63.1 → −63.0, 3130 Hz −60.8 → −60.7) | yes |
| Plexi variac | −3.8 / −5.1 / −8.0 % | −4.1 / −9.1 % | 0.1184 / 0.1277 → 0.1184 / 0.1282 | 0.1 dB | yes |
| 1959RR | −1.3 / −2.6 / −6.4 % | −2.6 / −7.9 % | 0.0445 / 0.0891 → 0.0445 / 0.0891 | 0.2 dB (3130 Hz 0.5 FS −67.3 → −67.1) | yes |
| JCM800 | | −2.8 / −7.9 % | 0.1507 / 0.1035 → 0.1519 / 0.1044 | **3130 Hz 0.25 FS −77.4 → −73.4** | no |
| AC30 (tone2's) | | −0.7 / −3.7 % | (384 kHz base; not re-referenced) | 0.1 dB, 3130 Hz better | reported |
| Fuzz Faces, Rangemaster | | — | no tube ports: the factor changes nothing | | — |

- SPICE gate (T/8 ngspice fixtures, 1× → 10×): plexi_drive spk / pa 0.023 / 0.031 → 0.024 / 0.032 %, plexi_var0 and
  pvar_drive spk / pa 0.026 / 0.034 → 0.027 / 0.035 %, rr_dimed pa 0.979 → 0.980 %; every other case of the three equal.
- These moves (truth ≤ 0.4 % of itself, gate ≤ 0.001 % absolute, alias ≤ 0.2 dB) are of either sign across figures and
  the size any perturbation of the integration gives (the 5150's 768 kHz stream alone moves its alias ±0.1 dB); they are
  taken as equal. The JCM800's 4 dB and +0.8 % on both references are not.

**Lanes.** The 5150 compiles to two groups: the 2-port supply and one 33-port group holding
everything from V1A to the speaker (`CE_LANEDIAG`: est. 14.8 µs a sample, lane B empty). Nothing in it can be cut
losslessly: preamp, loop path, inverter, power stage and speaker are one linear network between the nonlinear ports, and
the only cuts that exist are the one-sample `kcf`-type cuts §10.48 removed for accuracy. Two cores would need parallelism
inside one Newton solve (device sweeps or the sparse factorisation split per iteration), with a handshake per iteration
against ≈ 1.5 µs of work: a separate project.

**The AC30's 1.0 FS folds (§10.63's open item).** Its folds "barely improve with the rate"; a knee's kink error scales
as h² (the 5150 gained 12.6 dB from 4 to 8 substeps), so a rate-independent floor points at something else. Tube
substeps at 10× change it by ≤ 0.3 dB, so it is not the substeps' Newton test either. Not pursued further here.

**Engine change** (`Netlist.hpp / .cpp`: `Netlist::newtonSubsteps`, read and written as `newton.substeps`; `Engine.cpp`:
`buildGroupsRuntime` scales the tube ports' entries of `GroupRt::ptol` (or `tol`, on an all-tube group) of `rtHalf_`,
`rtBe_[*]`, `rtTr_[*]`; `describeGroups` prints the factor and the T/8 runtimes' Newton). `CE_SUBTOL=<k>` replaces the
netlist's factor everywhere; `CE_SUBTOL=1` is the engine before. Compile-time only: the hot path is unchanged.
- **Identity:** `bench --real` hashes of the 5150 with `CE_SUBTOL=1` equal the shipped binary's; the JCM800, Plexi,
  variac, 1959RR (before their netlist edit), Fuzz Face Si, Twin, OC-2 and SLO-100 are identical between the two binaries.
- **Off cost:** the hot path is unchanged. M4 Pro instructions JCM800 +0.01 %, Plexi (1×) −0.02 % (identical hashes); 9900X
  JCM800 −0.3 / −0.3 % real / driven, p99 +0.7 % (noise).
- **PGO:** no new hot code; the 5150 and the Plexi family now take fewer substep iterations (branch weights shift). Retrain
  with this section in when the profiles are next retrained; not needed for correctness.

**Verification.** Mac (a /tmp copy): test_circuit_engine (the SPICE-gate moves above; the 5150's dimed / clamp spk 0.292 / 2.807 → 0.293 / 2.806 %),
test_circuit_unit, test_effect_handoffs, test_aliasing (51 of 51, both tones) and rig_tests pass. EmbeddedLibrary.hpp
regenerated; no fixture depends on a Newton tolerance.

**Open items.** The 5150 stays over one core (104 % real on the 9900X; §10.64 wins back 13 % of what §10.62 added): the lever left is the per-iteration cost of its 33-port group
(sparse LU 29 %, kernel 20 %), not the substep count. The JCM800 (no events; LTE substeps from a 192 kHz base) would take
−3 / −8 % but moves its second-tone figure 4 dB at 0.25 FS. The AC30's 1.0 FS floor is not a substep-count or
substep-tolerance effect.

**Files** (`work/circuit-netlists/kneeref/`): `base/` (Engine / Netlist sources before), `orig/` (the four netlists and
EmbeddedLibrary.hpp before), `variants/` (`Engine-experiments.cpp/.hpp` and their patches: the per-substep diagnostic
`CE_KNEESTAT`, the stream decimation test `CE_KR_ODD`, the slot and variable-step events `CE_KR*`, the tolerance
experiments `CE_KR_SUBTOL*`), `scripts/` (9900X `push.sh` / `build.sh` / `send.sh` / `sendsrc.sh`, `xp.sh`
alias + truth + stats, `al5.sh`, `tr.sh` / `tru2.sh` / `truth5150.py` (AUTOLAG), `ftruth.py` (+ Plexi family, JCM800),
`unit.sh`, `b9.sh` / `b9x.sh` / `summ9.py` A/B, `hashes.sh`, `lsampler.c` / `psym.py` / `prof.sh` the profiler,
`queue*.sh`), `data/` (logs).

### 10.65 BE-100 back under one core: its dead branches out of the main group, exactly (2026-11-16)

**The question.** §10.63 moved the BE-100 (151000) to a 384 kHz base to pass the 3130 Hz gate tone: 117.3 / 117.9 % of a
9900X core, p99 2023 µs, so it could not run single-threaded in real time. Target: at most 70 % real, both tones at
0.5 FS with ≥ 3 dB margin and no worse than §10.63 at 0.25 / 1.0 FS, and engine-truth equal or better than §10.63's
0.018 / 0.041 % (Lead / Rock, audio band). Netlist-level only (Engine.cpp was being changed by §10.64 meanwhile).

**Method.** As §10.63 (`circuit_cli alias` with `CE_ALIAS_TONE` / `CE_ALIAS_LEVEL`; truth = §10.61's engine T/8 references,
band-limited (`refbl`), re-rendered from the 192 kHz netlist because tone2's scratch was gone: they reproduce tone2's
figures to four digits, 0.0641 / 0.1568 % at 192 kHz and 0.0181 / 0.0411 % for the shipped 384 kHz netlist at `@2b`;
CPU `bench --table --real`, `CE_REALREPS=3`, noise and hum off, A and B at once on cores 10 / 14, swapped each round,
3 rounds, means). Every measurement ran on the §10.62 engine (`kneeref/base`), not the live Engine.cpp.

**What folds** (probe spectra, `render --sine 3130` at the circuit rate with `--probe`, worst non-harmonic 10 Hz bin
20 Hz–20 kHz, own-mains mask, dB re the tone at that node; 0.5 / 1.0 FS):

| Node | 192 kHz | 384 kHz |
|---|---|---|
| p1a, p2a (V1A, V2A plates) | no fold (−69 to −73: 20–40 Hz bins) | same |
| p2b (V2B plate) | −62.8 / −59.2 (7330, 16720 Hz) | −79 / −74.8 |
| p3a (V3A plate) | −57.0 / −52.1 (1070 Hz) | −69.1 / −65.5 |
| kcf, s7o (follower, master) | −56.1 / −50.9 | −69.6 / −64.9 |
| spk | −56.9 / −50.8 (7330, 80 Hz) | −69.2 / −66.5 |

The bins are harmonics 59–63 of the tone (184–197 kHz) folded about 192 kHz: the preamp's V2B and V3A clipping makes
them, and everything after only carries them. A preamp/power split at a different rate is out, though: every signal
cut between them is far from lossless at 384 kHz (truth `s7o` with C31 as load 0.175 / 0.361 %, `g4a` with V4A and R37
0.201 / 0.414 %, `kcf` 1.09 / 1.89 %), and §10.38 / §10.39 already ruled out the exact splits.

**Where the 384 kHz cost was.** One group of 112 unknowns, 91 states and 38 ports (28 tube ports). 13 of those ports
belong to branches that are dead at the catalogue settings (BE, loop bypassed, SAT off): V1A with the whole Clean
channel (V1B, its stack), the FX loop (Q1, Q2, D5–D7) and the SAT clamp (Z1, Z2). Five open contacts tie them to the
amp through their 1 GΩ `roff`: `K1_48` (v1o–k1c), `K2_139` (cj–s7c), `S7A_1` (s7c–snd), `S7B_4` (rlw–s7o) and `SW7`
(sat2–sat1). The compiler keeps such a contact (it removes one only when both ends see ≤ 1e-6 · roff to ground, and
never where an end would float: snd and sat2 sit behind capacitors), so the dead branches ride in the main group's
Newton every sample. The leaks are not negligible against this truth: raising the contacts' `roff` so they drop out
moves the output by the leak, truth 0.018 / 0.041 → 0.026 / 0.044 % (Clean contacts), 0.031 / 0.048 (FX loop),
0.019 / 0.042 (SAT), 0.043 / 0.055 (all). So the leak has to stay, as the same current.

**What landed** (`friedman-be100-151000.circuit.json`; `work/circuit-netlists/be100rt/scripts/land.py`).
- **Each of the five contacts keeps its 1 GΩ as a pair of grounded VCCS** (`<ref>_GA`, `<ref>_GB`, gain 1e-9 S, one
  stamped at each end, each controlled by both ends: together exactly the resistor), and its own `roff` goes to 1e13
  (1e19 with a 1 TΩ DC return, `S7A_1_RB` at snd and `SW7_RB` at sat2, where an end would float). The compiler now
  drops the contact when it is open; a closed contact (CLN, HBE, loop on, SAT on) is its 0.1 Ω as before, with the pair's
  1 GΩ beside it, as before.
- **Partitioning unites only a VCCS's output nodes** (Engine.cpp "Element ownership"), so the dead branch becomes its own
  group. The pair alone would still merge them: the two sides read each other, and Tarjan's SCC makes a cycle one group.
  So the dead side reads the live node a sample late, through a delay cut whose loads are only that side's VCCS
  (`k1c` ← K1_48_GA, `s7c` ← K2_139_GA and S7A_1_GB, `s7o` ← S7B_4_GA, `sat2` ← SW7_GB). No current is lagged: the
  owner carries nothing, and what is late is the dead side's view of a 1 GΩ leak.
- **The FX-loop and SAT groups run as 48 kHz slow partitions** (`"multirate": {"slow": {"rate": 48000, "nodes": ["snd",
  "sat1"]}}`). The Clean channel cannot: its group reads the input node (V1A's grid stopper), and a slow group never may.
- The main group is now 75 unknowns, 62 states, 25 ports (24 of them tube ports); beside it the Clean group (18
  unknowns, 4 ports), the FX loop (15, 7 ports, slow) and SAT (4, 2 ports, slow).
- `tests/test_aliasing.cpp`: the BE-100 joins the SLO-100 in `staticSlowPartition`. Its slow groups hold no oscillator
  or side chain; without the entry the gate would call the unit time-varying and only report it.

**Exactness.** DI nulls against §10.63's netlist, unit rate, 5 s of the lead DI at 0.35 V, noise and hum off, every
switch class: default −147.1 dB rms (peak −125.3), CLN −147.9, HBE −146.3, loop on −139.9, loop on + HBE −139.7, SAT
−149.0, SAT + CLN −148.0, FAT −147.5, C45 −146.6, voice −146.4, bright (CLN) −146.9, 4 Ω −147.6. **Output level
unchanged** (0.000 dB). Aliasing identical at five of the six points and within 0.1 dB at the sixth (3130 Hz,
0.5 FS: −69.2 dB on x86-64 with the landed part order); truth identical to four digits (0.0181 / 0.0411 %, full
band 0.0267 / 0.0583). The slow rate does not show in either: at 24, 48 and 96 kHz the truth reads the same four digits.

**Results** (9900X, 3 rounds; another 5150 bench ran on cores 2 / 6 throughout, which puts this session's
shipped figures 2–4 % under tone2's):

| | shipped (§10.63) | **§10.65** | change |
|---|---|---|---|
| real / driven % of a core | 112.5 / 115.2 | **88.6 / 91.0** | −21.2 / −21.0 % |
| combined 64-frame p99 | 1927 µs | **1453 µs** | −24.6 % |
| 2900 Hz, 0.25 / 0.5 / 1.0 FS | −71.2 / −73.7 / −68.0 dB | same | |
| 3130 Hz | −69.5 / −69.1 / −66.5 dB | same | |
| truth Lead / Rock (audio band) | 0.018 / 0.041 % | same | |

- The landed file's own build and kernels. An earlier A/B of the same structure (the bridge parts appended at the end of
  the part list rather than beside their contacts) read 113.7 / 117.7 → 86.6 / 88.5 %, p99 1950 → 1406 µs.
- **Short of the 70 % target.** Profiled (`CE_PROFILE`, bench path) the remaining sample is the main group 71 %
  (2350 ns at 384 kHz, Newton 1.0 iterations a sample), the Clean group 13 %, the supply groups 11 %, the slow groups 5 %.
- Without the slow partitions the bridges save only 7–9 %: the three dead groups cost about as much on their own (FX
  loop 731 ns, Clean 445 ns, SAT 187 ns a sample) as they did inside the main group.

**Measured, not taken.**
- **Event-local substeps on a 192 kHz base** (the bridged netlist, `"rates": {"V2B": 16}, "events": true`, 4 substeps,
  BDF2 start; alias 0.25 / 0.5 / 1.0 FS; 9900X A/B where marked, single runs otherwise):

  | LTE tol | 2900 Hz | 3130 Hz | truth Lead / Rock | CPU real / driven, p99 |
  |---|---|---|---|---|
  | 4 | −69.2 / −73.8 / −65.7 | −61.6 / −66.2 / −60.2 | | |
  | 3 | −86.4 / −76.6 / −67.9 | −78.7 / −68.5 / −63.3 | 0.056 / 0.125 | |
  | 2 | −90.1 / −81.2 / −74.1 | −86.5 / −74.0 / −67.2 | 0.051 / 0.108 | |
  | 1 | −91.5 / −82.6 / −75.1 | −89.5 / −79.5 / −72.7 | 0.044 / 0.083 | **52.9 / 59.3 %, 1360 µs** (A/B) |
  | 0.5 | | | 0.038 / 0.059 | 58.5 / 63.5 % |
  | 0.25 | −91.5 / −83.2 / −76.4 | −89.9 / −81.2 / −74.3 | 0.033 / 0.042 | 62.6 / 74.2 % |
  | 0.1 | same | same | 0.029 / 0.030 | **68.5 / 69.6 %, 1727 µs** (A/B) |

  Every aliasing figure from tol 2 down is better than §10.63's at every level, and tol 0.1 meets the CPU target with
  Rock's truth better than shipped, but the lead DI's stays at 0.029 % against 0.018. It does not converge there: with every
  sample substepped (`CE_EVOS_FORCE=1`) the lead DI reads 0.0235 / Rock 0.0234 % at 4 substeps (768 kHz steps) and 0.027 /
  0.047 % at 2 (384 kHz steps), against 0.018 / 0.041 % for a plain 384 kHz base. 8 substeps (`V2B: 32`) at tol 0.1:
  0.028 / 0.028 %. So the event path has its own floor above a fixed step of the same size (engine item below). 2
  substeps (`V1A: 8`, unbridged) reach −63.0 dB at 3130 Hz only at tol 0.5 (no margin), truth 0.041 / 0.076 %.
- LTE tolerance alone on the 192 kHz base (1 / 2 / 4): 3130 Hz at 0.5 FS −54.0 / −62.7 / −59.2 dB.
- The main group alone on the ladder at 384 kHz over a 192 kHz base: no LTE on raised groups, 3130 Hz −62.1 dB at
  0.5 FS, −65.2 at 2900 Hz.
- The supply groups as slow partitions too: a further 6–10 % at 48 kHz, but truth 0.104 / 0.103 %; 96 kHz 0.050 /
  0.060; 192 kHz (div 2) 0.027 / 0.045 % (the rectifier pair alone 0.022 / 0.043; the bias rectifier alone unchanged).
- Newton hints on the bridged netlist (`CE_CHORDGATE=300`, `CE_PREDQPORTS=4`, `CE_SUBPRED=26`) and §10.55 tolerances
  1e-4 on the supply diodes, the FX loop and SAT: within ±1.5 %, none taken.
- Lanes: the plan does not engage (unit-path compile: main group 9556 of 10591 ns estimated, two lanes 96–100 %), and
  the main group reads the supply rails' owners in the same sample.

**Engine queue** (not done here).
- **The event path's truth floor.** On a 192 kHz base, substeps on every sample still read 0.024–0.027 % (Lead)
  against 0.018 % for a fixed step of the same size. Candidates, in the order the data points to: the supply rails held
  over the base sample during substeps and the supply cut currents lagged a base sample (slow supply groups at 192 kHz
  under a 384 kHz main group reproduce the floor: 0.027 / 0.045 %); then the BDF2-start / trapezoid substep mix; then the
  stream reconstruction. If the floor drops to the fixed-step figure, tol 0.1–0.25 here would meet every bar at
  62–69 % (and leave aliasing far better than §10.63).
- **Open contacts across groups automatically.** A contact kept at roff only because its leak is above rounding, or an
  end would float, could be compiled as this section's VCCS pair with the dead side lagged: −140 dB from the netlist's
  circuit. The bleeders, the roff values and the delay cuts here would then go.
- **A gate flag for static slow partitions** instead of the id list in test_aliasing.cpp.

**Verification.** Mac (a /tmp copy: §10.62 engine, this netlist, EmbeddedLibrary.hpp regenerated): test_circuit_engine
(every be100 SPICE figure unchanged; one max-error column, be100_be_small p2b, 1.49e-4 → 1.48e-4 V), test_circuit_unit
(codegen 257 Lin + 111 Lu kernels, all bound), test_effect_handoffs, rig_tests, test_aliasing (51 of 51; the BE-100
gated, −69.1 dB at 3130 Hz, −73.7 at 2900). On the 9900X test machine, the landed file with its own generated kernels:
test_circuit_engine, test_circuit_unit, rig_tests and test_aliasing (51 of 51) pass, and the nulls, truth (0.0181 /
0.0411 %), alias grid and A/B above were measured on it. No ngspice fixture was regenerated (the gate cases compile the
netlist as before; their figures did not move).

**PGO.** Netlist and test only, but the BE-100's group structure changed: retrain the profiles with this section in.

**Files** (`work/circuit-netlists/be100rt/`): `orig/` (the netlist, EmbeddedLibrary.hpp and test_aliasing.cpp before),
`scripts/` (`land.py` the edit and `note.txt` its note; `patch_embed.py` rewrites only the BE-100's entry in
EmbeddedLibrary.hpp, as embed_library.py formats it, so other netlists in progress stay out; `push.sh` /
`build.sh` / `send.sh` / `pull.sh` for the 9900X test machine; `pspec.py` + `probe.sh` the fold localisation; `al.sh` the alias
grid; `ftruth2.py` + `tr.sh` truth; `nulls.sh` + `null.py` the mode nulls; `q.sh` single benches; `b9t.sh` + `summ9.py`
the 9900X A/B; `final.sh` the 9900X verification of the landed file; `vb.py` / `dead.py` / `dead2.py` / `mk*.sh` the
variants), `data/` (`probe_3130.txt`; alias tables `a1`–`a4.txt`; truth `ft/*.tsv`; A/B rounds `b1/`, `fin/b9/`; the
landed file's nulls, truth, alias and gate logs `fin/`; the Mac gate logs `mac/`).

### 10.66 Solver scheduling and generated sparse kernels (2026-11-16)

**Landed scope.** `Engine.cpp` uses elimination depth to break equal-fill Markowitz ties for groups of at least 20 nonlinear ports (`CE_SPDEPTH=0` restores the previous order). `NewtonKernel.hpp` filters the residual acceptance test with reassociated sums and conservative rounding bounds, falling back to the original in-order sum for undecided rows; it also vectorises independent port-ratio divisions. `tools/circuit/circuit_codegen.cpp` emits straight-line LU stamps/injections, prunes structurally unreachable forward-substitution work, removes zero-multiplier branches and defers pivot-failure reporting until the factor completes. Failed factors still use the existing dense fallback. No device laws, tolerances, sample rates, event/substep policy or supply-rail treatment changed.

**Recovered CPU evidence.** None of these runs were repeated after validation. Ryzen 9900X, x86-64-v3, `bench --real`, noise/hum disabled, two pinned cores swapped each round, three rounds and two DI repetitions per run. Real/driven columns are percent of one core; p99 is combined 64-sample block time in microseconds. They are workload measurements, not a guarantee that every amp meets its real-time deadline. In particular the 5150 and JC-120 still exceed one core in some cases. Small-unit regressions are retained and disclosed rather than traded against sound quality. The original final build showed a Fuzz Face Si performance regression (+10.7 % real, +6.0 % driven without PGO), with unchanged sound; it was not included in the seven-unit PGO batch. The later measured small-kernel recovery and its landing are recorded below; these historical table entries have not been retimed. Production CPU claims use only the measured PGO batch below: the 5150 is 106.5 / 112.0 % of one core, not an earlier estimate of 94 %.

**PGO off: all 16 units**

| Unit | Base real / driven % | Final real / driven % | Base → final p99 µs | Change real / driven / p99 |
|---|---:|---:|---:|---:|
| SLO-100 | 61.4 / 71.9 | 49.6 / 58.7 | 1606 → 1339 | -19.2 % / -18.3 % / -16.6 % |
| 5150 | 117.9 / 125.9 | 98.1 / 104.5 | 3695 → 3047 | -16.9 % / -17.0 % / -17.5 % |
| TS808 | 9.1 / 10.0 | 9.4 / 10.2 | 208 → 213 | +2.9 % / +2.0 % / +2.1 % |
| BE-100 | 108.1 / 106.9 | 93.3 / 96.0 | 2095 → 1839 | -13.6 % / -10.2 % / -12.2 % |
| Dual Rectifier | 74.5 / 75.3 | 63.0 / 63.7 | 2614 → 2181 | -15.5 % / -15.5 % / -16.6 % |
| SLO MkII | 64.8 / 69.5 | 50.5 / 54.5 | 1550 → 1222 | -22.1 % / -21.6 % / -21.1 % |
| HM-2 | 32.3 / 41.2 | 32.0 / 41.6 | 1203 → 1191 | -0.7 % / +1.0 % / -1.0 % |
| DS-1 | 18.4 / 23.0 | 18.8 / 23.1 | 623 → 627 | +1.9 % / +0.7 % / +0.7 % |
| Twin Reverb | 43.8 / 52.7 | 37.5 / 46.0 | 1258 → 1091 | -14.5 % / -12.7 % / -13.3 % |
| Plexi | 53.5 / 60.9 | 52.5 / 59.3 | 1705 → 1644 | -2.0 % / -2.6 % / -3.6 % |
| AC30 | 66.6 / 84.3 | 64.9 / 81.1 | 2178 → 2074 | -2.6 % / -3.9 % / -4.8 % |
| Fuzz Face Si | 24.2 / 23.0 | 26.8 / 24.4 | 521 → 527 | +10.7 % / +6.0 % / +1.2 % |
| JC-120 | 128.2 / 154.1 | 124.2 / 148.9 | 3684 → 3504 | -3.1 % / -3.4 % / -4.9 % |
| JCM800 | 50.0 / 63.0 | 49.5 / 62.1 | 1902 → 1862 | -0.9 % / -1.4 % / -2.1 % |
| Uberschall | 72.5 / 77.7 | 56.2 / 60.4 | 1790 → 1408 | -22.5 % / -22.3 % / -21.4 % |
| Rectifier Rev C | 88.9 / 98.0 | 75.1 / 82.9 | 2690 → 2246 | -15.5 % / -15.4 % / -16.5 % |

**Existing PGO profile: seven-unit confirmation (no retraining)**

| Unit | Base real / driven % | Final real / driven % | Base → final p99 µs | Change real / driven / p99 |
|---|---:|---:|---:|---:|
| 5150 | 125.2 / 131.5 | 106.5 / 112.0 | 3953 → 3346 | -15.0 % / -14.8 % / -15.4 % |
| TS808 | 8.5 / 9.4 | 8.7 / 9.5 | 181 → 185 | +2.6 % / +0.7 % / +2.3 % |
| BE-100 | 105.8 / 107.8 | 92.8 / 95.4 | 2053 → 1815 | -12.2 % / -11.5 % / -11.6 % |
| Dual Rectifier | 76.1 / 80.9 | 64.9 / 69.4 | 2795 → 2272 | -14.8 % / -14.2 % / -18.7 % |
| JC-120 | 123.5 / 148.9 | 123.0 / 147.4 | 3543 → 3473 | -0.4 % / -1.0 % / -2.0 % |
| JCM800 | 48.4 / 61.1 | 48.6 / 60.5 | 1808 → 1792 | +0.5 % / -1.0 % / -0.9 % |
| Rectifier Rev C | 93.1 / 97.8 | 79.9 / 83.4 | 2733 → 2279 | -14.1 % / -14.8 % / -16.6 % |

**Sound equivalence and SPICE.** All 68 real/driven unit hashes match the baseline with `CE_SPDEPTH=0`; the default order preserves 46 hashes and changes 22. All 22 changed units have recovered full-DI output nulls: −155.0 to −269.1 dB RMS relative to signal RMS. Worst peak is −137.1 dB relative to signal peak (SLO MkII); the 5150 is −165.5 / −138.2 dB RMS/peak. The null script also accidentally parsed the summary `SAME 46 DIFF 22` as a unit, producing a spurious `46 DIFF FileNotFoundError`; this is not a missing unit, and the valid null rows number 22.

The existing 5150 band-limited ngspice v2 comparison (`p5150v2bl`, Lead and Rock, seven nodes each) covers baseline, exact package, and depth-enabled package. Audio-band NRMSE is identical at all 14 case/node pairs to six decimal places in percent; speaker errors remain 0.273932 / 0.602086 %. The only full-band printed change is Lead `pb`, 1.247636 → 1.247635 %. This is evidence for rounding-level equivalence, not a claim of a new exhaustive SPICE sweep. The final 5150 sine/real/driven hashes also match the depth-enabled variant used by that SPICE comparison (`0e54d6ee274da946`, `6768777365121c39`, `d7cd350f9b383b6f`). All 138 recovered A/B logs have zero solver failures and stable hashes across rounds. The final engine and Newton kernel match the measured 9900X sources byte-for-byte; local codegen differs only by an explanatory comment. Earlier Mac instruction figures in the progress log are historical observations; their raw scratch logs were no longer present at recovery, so they are not used as new timing evidence.

**Validation.** 9900X baseline and final: `test_circuit_engine`, `test_circuit_unit`, `test_aliasing` (51/51) and `rig_tests` pass. Both handoff tests on the 9900X abort at the same missing NAM demo assertion, so Mac validation is required. Codegen emits 228 sparse-linear, 102 sparse-LU and 178 Newton structures. Mac Release build with the existing arm64 AUTO-PGO profile succeeds; all seven gates pass: `rig_tests` (42.30 s), `test_effect_handoffs` (11.15 s; NAM at 48 and 44.1 kHz), `test_circuit_engine` (59.21 s), `test_circuit_unit` (193.27 s), `test_aliasing` (166.74 s, 51/51 below −60 dB at both tones), `test_unit_param_mapping` (19.93 s), and `test_param_drag` (27.97 s, with `rtguard`). Total 520.57 s, zero failures. Generated boolean bitwise warnings are nonfatal; no source correction was needed. Source SHA256s remained unchanged through validation.

**Small-kernel recovery landing (2026-11-16).** The exact measured `work/circuit-netlists/iterc/data/fuzz-fix/candidate.patch` is now applied: old inline acceptance and limiter/ratio loops are restored only for compile-time `0 < NN < 16`, retaining the measured duplication deliberately. Dynamic/large algorithms and code-generator source remain unchanged. Full-file preimage SHA256 was `1e984d9c01d34675ed056ff2760d6bfb618105896593e6241e1bde4d6d5bcae9`; landed header is byte-identical to the measured candidate, SHA256 `526418a13eccae5d3ee23db8a60ab096379ce1e05319a814a68385fb6298da68`.

The earlier isolated x86 existing-PGO experiment recovered Si's regression: real/driven −2.50/−4.11% versus the previous final and −0.67/−0.87% versus baseline, with identical sound hashes/work across three paired rounds. Inspection of the compiled small kernels supports an outlining/inlining mechanism (up to four outlined residual-row calls removed). The sub-1% baseline advantage is not a robust general claim, and no new arm64 timing, non-PGO recovery or refreshed-profile claim is made. See [experiment](../../work/circuit-netlists/progress/fuzz-fix.md).

Local landing validation uses a preserved 09:03 pre-change engine archive relinked with the unchanged CLI object; the older 02:47 CLI was detected and excluded from comparisons. All 14 focused paired renders have zero serialized null and exact raw-double output hashes: five small units at 44.1 kHz and 705.6 kHz internal, 5150/BE-100/SLO-100 at 768 kHz, and hot test-zener at 44.1 kHz. Large-amp raw output hashes remain `d91f4d7a1b275151`, `657728b2e551f54e`, `9db423a7ba965dee`, respectively. All targeted solver failure counts are zero. Engine.cpp, circuit_codegen.cpp and stored PGO profiles have unchanged SHA256s; CMake regenerated derived shards, whose text hashes are not claimed identical. Release/AUTO-PGO build passed with two jobs after Ballast check. All seven normal landing gates passed (362.39 s total): factory/unit, effect handoffs, circuit-engine including existing limiter/floating-junction cases, circuit-unit, aliasing, parameter mapping and rtguard parameter dragging. No source correction was required. Results are recorded in [landing evidence](../../work/circuit-netlists/progress/fuzz-fix-land.md).

**Ship status.** The solver slice and exact small-kernel recovery are cleared for integration on the correctness and sound-equivalence evidence above. Performance sign-off remains conditional: the historical non-PGO regression has not been retimed after this recovery, and the measured PGO 5150 and JC-120 still exceed one core. No profile retraining was performed.

**Artifacts and boundaries.** `work/circuit-netlists/iterc/data/worker/out/` contains the recovered raw A/B rounds, gate logs, hash summaries, all changed-unit nulls and SPICE TSVs; `data/mac/` contains the local build and gate logs. The baseline snapshots and `iterc.patch` describe the three functional source changes; `src/circuit/Codegen.hpp` also has an existing explanatory comment added after the patch snapshot. No PGO retraining, rail interpolation experiment or new optimisation belongs to this slice. Section 10.65's held-rail explanation remains a candidate, not a confirmed root cause.

## 11. Sources and licences

**Valve curves (§4.1).** RCA, Philips datasheet plate characteristics as listed in §4.1 (values digitised; no
images reused; scans hosted by frank.pocnet.net). K. Dempwolf and U. Zölzer, "A physically-motivated triode model
for circuit simulations", DAFx-11, Paris 2011, Fig. 6 (measured 12AX7 points). Vox AC-15 factory drawing No. V-1-5
(el34world.com) for the EF86 stage voltages.

**Circuit data.**
- Pedal Parts Ltd, "Tube Screamer v2.0" build document: TS808 schematic and BOM (fetched as a PDF; values
  only, no text or graphics reused).
- R. G. Keen, "The Technology of the Tube Screamer", geofex.com.
- Barbarach BC, "What is a Tube Screamer?".
- Fender AB763 Twin Reverb service schematic (public).
- Yeh and Smith, DAFx 2006 (TMB topology).
- Rob Robinette: AB763 Twin Reverb layout (DIYLC) v2.18, "How the AB763 Deluxe Reverb Works", AB763 model
  differences, AB763 reverb and tremolo circuit diagrams (robrobinette.com). Values and topology only.
- Fender Electric Instrument Co., "Deluxe Reverb-Amp AB763" factory schematic with voltage chart (vibrato,
  roach driver and phase-inverter feedback wiring shared by the AB763 Twin).
- Music Electronics Forum thread on the AB763 opto tremolo (V5B cathode resistor, 10 MΩ neon bleed).
- Fender "Twin Reverb-Amp AB763" factory schematic (https://schematicheaven.net/fenderamps/twin_reverb_ab763_schem.pdf)
  and "Deluxe Reverb-Amp AB763" (https://schematicheaven.net/fenderamps/deluxe_reverb_ab763_schem.pdf): the roach
  driver's grid on the 0.01 µF / 1 MΩ junction, 2.2 MΩ to the bias supply, 3M-RA speed + 100 kΩ, 50K-RA intensity.
- Vox "AC-15 Amplifier" factory drawing No. V-1-5 (https://el34world.com/charts/Schematics/files/Vox/Vox_AC15.pdf)
  and S. Luckey's AC-15 redraw with voltages (https://sluckeyamps.com/VAC15/Vox_AC15.pdf): the Vibravox oscillator,
  LFO splitter, ECC82 modulator pair, HI / LO paths and output ladder.

All are cited in each netlist's `sources`.

**Device data.**
- TI CD4013B (SCHS023E), CD4011B (SCHS021D, Harris data), CD4069UB (SCHS054E) and CD4049UB datasheets: drive tables,
  transfer and sink-current figures, propagation delays, input capacitance. E. Brandt, "Hard Sync Without Aliasing"
  (ICMC 2001) for minBLEP.
- THAT Corporation datasheets and notes (public): 2180 Series doc 600029 rev 02, 2252 doc 600032 rev 03, 202XT/202XTC/202R
  modular VCAs (202data.pdf), Design Note 127 "Upgrading Modular VCAs" (dbx 202 Black Can pinout, application and
  control constant). (The Blackmer patent US 3,714,462 was not consulted: the 2180 Fig. 5 cell and DN127 sufficed.)
- 1N914 / 1N4148 / 1N4001 / 2N3904 / 2N3906 / 2N5088: manufacturer SPICE models (Fairchild/ON, NXP,
  Diodes Inc.).
- JRC4558, TL072, NE5532 and LM741 datasheets (New JRC, TI).
- N. Koren's triode parameter sets.
- RCA 12AX7, 12AT7 and 12AU7 datasheets (capacitances).
- The grid-conduction law comes from Headroom's own `TubeDevice.hpp`.
- Approximate entries are labelled as such (2SC1815 IS, 1N34A, BAT41, LEDs).

- Loudspeakers (§10.21): Celestion G12M Greenback and Vintage 30 sheets (celestion.com); Jensen P12Q and C12N
  specification sheets (jensentone.com); Eminence Legend 1258 datasheet; the measured G12M-25 set posted by "fyl" on the
  Music Electronics Forum ("2x12 closed back cab design", 2010-05-18). Large-signal criteria and parameter shapes after
  W. Klippel, "Loudspeaker nonlinearities: causes, parameters, symptoms", JAES 54(10), 2006, and IEC 62458; the thermal
  topology after W. Klippel, "Nonlinear modeling of the heat transfer in loudspeakers", JAES 52(1/2), 2004. Vector
  fitting: B. Gustavsen and A. Semlyen, IEEE Trans. Power Delivery 14(3), 1999.

**Cabinet IRs** (the acoustic paths in `cabs.blocks.json` are fitted to them; the IR files are not shipped by this
work): Jester's Emerald Pack 1.0 and Jester's Brutal Pack 1.0, Jester Dyne Productions (Bastian Karschewski), CC0 1.0
(licence text in the packs' handbooks; see `work/ir-library/SOURCES.md`).

**Papers and theses.** Listed in section 9.

**Software.**
- **ngspice 47** (Homebrew; BSD-3 core with some modules under other free licences). A dev-only external
  program: never linked, never shipped.
- **nlohmann/json** (MIT), already vendored under NeuralAmpModelerCore.
- `amp::Oversampler` and `amp::PowerAmp` are Headroom's own code.
- No code was copied from chowdsp_wdf (BSD-3), ACME.jl (MIT), LiveSPICE (MIT), Guitarix (GPL) or Faust
  libraries. They were studied as references only.

**Audio.** GuitarSet (Xi et al., ISMIR 2018, CC BY 4.0) for the A/B and SPICE DI proofs (see
`app/samples/di/SOURCES.md`).

**Legal line.** Nothing in this engine, its netlists or its tests was derived from proprietary third-party
software. Beyond the netlists, the only identifiers carried are the short parameter ids (`Dr`, `Tn`, `Vol`, `Bs`, `Br`, `Cl`, `Tr`, `Md`, `Sg`, `Vr`, `Bia`),
plus the Twin's `Br`, `RvL`, `RvOn`, `RvTm`, `RvSz`, `ViInt`, `ViSpd`, `ViOn`, `Rsp` and `Hz`, which appear solely
as compatibility aliases in the netlists' `params` lists. They are the single place to
change if they are ever renamed.

### Unit-to-unit variation (`serial`, 2026-11-10)
- **What it is.** The model JSON key `"serial"` (an integer) selects one particular unit off the production line.
  - `0`, the default, is the nominal ("golden") unit and is bit-identical to no variation.
  - Any other value draws every constant part value within its published `tolerance`, as a ±3σ normal.
  - It also gives each valve its own copy of its card with real tube spread: Koren `kg1` (cathode current) ±15 % and `µ` ±5 %, both 3σ.
- **Determinism.** The draw is deterministic per (serial, part ref), so a preset recalls the same unit.
- **Where.** `applyUnitVariation` in CircuitUnit.cpp, applied before compile. Tested in test_circuit_unit: serial 0 bit-identical; a repeated serial identical; different serials differ; level within ±3 dB (a varied Twin measured −0.63 dB).
- **Not yet modelled:** noise and hum (ENGINE_QUEUE 35).
