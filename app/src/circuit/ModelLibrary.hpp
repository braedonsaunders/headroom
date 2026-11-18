#pragma once
// Built-in device model library (docs/CIRCUIT_ENGINE.md §4). Every entry records where its numbers come from.
// Parameters are the public SPICE / datasheet / paper values; nothing here is derived from any commercial
// plug-in. A netlist can override or extend any entry with its own "models" section.
#include "Netlist.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace rig::ce {

struct LibraryEntry {
    const char* name;
    const char* kind;
    std::initializer_list<std::pair<const char*, double>> params;
    const char* provenance;
};

// Thermal voltage at SPICE's TNOM (27 °C) so engine and ngspice agree exactly.
inline constexpr double kVt = 0.025864186; // k·300.15 K / q

inline const std::map<std::string, DeviceModel>& builtinModels() {
    static const std::map<std::string, DeviceModel> lib = [] {
        const LibraryEntry entries[] = {
            // ── Diodes: Shockley + series R (SPICE IS / N / RS) ──
            {"1N914", "diode", {{"is", 2.52e-9}, {"n", 1.752}, {"rs", 0.568}},
             "Widely published 1N914 SPICE model (Fairchild/ON discrete library): IS=2.52n N=1.752 RS=0.568"},
            {"1N4148", "diode", {{"is", 4.352e-9}, {"n", 1.906}, {"rs", 0.6458}},
             "NXP/Philips 1N4148 SPICE model: IS=4.352n N=1.906 RS=0.6458"},
            {"1N4001", "diode", {{"is", 14.11e-9}, {"n", 1.984}, {"rs", 0.03389}},
             "Diodes Inc. 1N4001 SPICE model: IS=14.11n N=1.984 RS=33.89m"},
            {"1N34A", "diode", {{"is", 2.0e-7}, {"n", 1.3}, {"rs", 5.0}},
             "Generic germanium point-contact (approximate; Vf ≈ 0.3 V @ 1 mA). Replace with measured values when available"},
            {"BAT41", "diode", {{"is", 1.0e-7}, {"n", 1.08}, {"rs", 6.0}},
             "Generic small-signal Schottky (approximate; datasheet Vf 0.45 V @ 1 mA)"},
            {"LED_RED", "diode", {{"is", 1.0e-19}, {"n", 1.6}, {"rs", 5.0}},
             "Generic red LED fitted to Vf ≈ 1.5 V @ 1 mA, 1.8 V @ 20 mA (approximate)"},
            {"LED_GREEN", "diode", {{"is", 1.0e-22}, {"n", 1.7}, {"rs", 5.0}},
             "Generic green LED fitted to Vf ≈ 1.9 V @ 1 mA (approximate)"},
            // ── BJTs: Ebers–Moll transport (Gummel–Poon subset IS / BF / BR) ──
            {"2N3904", "npn", {{"is", 6.734e-15}, {"bf", 416.4}, {"br", 0.7371}},
             "Fairchild/ON 2N3904 SPICE model (IS=6.734f BF=416.4 BR=.7371), EM subset"},
            {"2N3906", "pnp", {{"is", 1.41e-15}, {"bf", 180.7}, {"br", 4.977}},
             "Fairchild/ON 2N3906 SPICE model (IS=1.41f BF=180.7 BR=4.977), EM subset"},
            {"2SC1815", "npn", {{"is", 2.0e-14}, {"bf", 300.0}, {"br", 3.0}},
             "Toshiba 2SC1815-GR (hFE 200-400 per datasheet; IS approximate), EM subset"},
            {"2N5088", "npn", {{"is", 5.911e-15}, {"bf", 1122.0}, {"br", 1.271}},
             "Fairchild 2N5088 SPICE model (IS=5.911f BF=1.122k BR=1.271), EM subset"},
            // ── Op-amps. level 0 = ideal (nullor); 1 = macro (tanh transconductance + dominant pole + rails) ──
            // swing_hi / swing_lo = output headroom from the supply pins (V); vhi / vlo = absolute rails when the
            // part has no supply pins.
            {"JRC4558", "opamp", {{"level", 1}, {"a0", 1e5}, {"gbw", 3e6}, {"slew", 1.0e6}, {"swing_hi", 1.5}, {"swing_lo", 1.5}, {"vhi", 13.5}, {"vlo", -13.5}},
             "NJM4558 datasheet (New JRC): Av 100 dB typ, GBW 3 MHz, SR 1 V/µs, ±12..13 V swing at ±15 V"},
            {"TL072", "opamp", {{"level", 1}, {"a0", 2e5}, {"gbw", 3e6}, {"slew", 13e6}, {"swing_hi", 1.5}, {"swing_lo", 1.5}, {"vhi", 13.5}, {"vlo", -13.5}},
             "TI TL072 datasheet: AVD 200 V/mV typ, GBW 3 MHz, SR 13 V/µs, ±13.5 V swing at ±15 V"},
            {"NE5532", "opamp", {{"level", 1}, {"a0", 1e5}, {"gbw", 10e6}, {"slew", 9e6}, {"swing_hi", 1.0}, {"swing_lo", 1.0}, {"vhi", 14.0}, {"vlo", -14.0}},
             "TI NE5532 datasheet: AVD 100 V/mV, GBW 10 MHz, SR 9 V/µs"},
            {"LM741", "opamp", {{"level", 1}, {"a0", 2e5}, {"gbw", 1e6}, {"slew", 0.5e6}, {"swing_hi", 1.5}, {"swing_lo", 1.5}, {"vhi", 13.5}, {"vlo", -13.5}},
             "TI LM741 datasheet: AVD 200 V/mV, GBW 1 MHz, SR 0.5 V/µs"},
            {"IDEAL_OPAMP", "opamp", {{"level", 0}},
             "Nullor (infinite gain, no slew, no rails): Eco level of detail"},
            // ── Triodes: Koren plate law with the contact-potential offset vgo (Devices.hpp); grid conduction: Headroom GridLaw.
            //    Fitted cards: tools/circuit/tube_fit.py on work/tube-curves/<tube> (docs/CIRCUIT_ENGINE.md §4.1) ──
            {"12AX7", "triode", {{"mu", 98.698}, {"ex", 1.4461}, {"kg1", 1965.9}, {"kp", 935.73}, {"kvb", 197.28}, {"vgo", 0.61033}, {"cgk", 1.6e-12}, {"cgp", 1.7e-12}, {"cpk", 0.46e-12}, {"gscale", 6.2e-4}, {"gknee", 0.06}},
             "Plate law fitted (Headroom 2026, tube_fit.py) to the RCA 12AX7-A 'Average Plate Characteristics' (92CM-6879, 10-60; 697 points) and the measured "
             "RSD 12AX7 of Dempwolf & Zoelzer, DAFx-11, Fig. 6 (positive grid, Va 20-300 V): RMS error, % of 4 mA, negative grid 5.3 -> 1.9, Va < 50 V 8.5 -> 0.9, "
             "positive grid 7.3 -> 7.0 against N. Koren's 1996 set; sheet points 250 V / -2 V 1.24 mA (1.2), gm 1.54 mA/V (1.6), rp 63.5k (62.5). "
             "Caps: RCA 12AX7 datasheet; grid law: Headroom TubeDevice.hpp (Ig of the RSD tube within 3.5 % of 1.9 mA)"},
            {"12AT7", "triode", {{"mu", 99.025}, {"ex", 1.8}, {"kg1", 445.84}, {"kp", 166.37}, {"kvb", 2939.2}, {"vgo", 0.43271}, {"cgk", 2.2e-12}, {"cgp", 1.5e-12}, {"cpk", 0.5e-12}, {"gscale", 6.2e-4}, {"gknee", 0.06}},
             "Plate law fitted (tube_fit.py) to the RCA 12AT7 'Average Plate Characteristics' (92CM-7056, 1954; 568 points): RMS error, % of 24 mA, 6.9 -> 2.2 "
             "(Va < 50 V 2.4 -> 1.4) against Koren's set; small-signal mu at 250 V / -2 V 62 (sheet 60). Caps: RCA 12AT7 datasheet"},
            {"12AY7", "triode", {{"mu", 43.379}, {"ex", 1.5279}, {"kg1", 2712.1}, {"kp", 240.66}, {"kvb", 36.177}, {"vgo", 0.72077}, {"cgk", 1.3e-12}, {"cgp", 1.3e-12}, {"cpk", 0.6e-12}, {"gscale", 6.2e-4}, {"gknee", 0.06}},
             "Plate law fitted (tube_fit.py) to the RCA 12AY7 'Average Plate Characteristics' (92CM-7861, 1952, incl. the Ec = +1 V curve; 628 points): "
             "RMS error, % of 10 mA, negative grid 4.6 -> 1.1, positive grid 4.4 -> 1.8, Va < 50 V 1.6 -> 0.5 against R. McLean's Koren-form set "
             "(previously netlist-local in the 5F6-A / 5E3); 250 V / -4 V gives 3.13 mA (sheet 3.0). Caps: RCA data"},
            {"12AU7", "triode", {{"mu", 21.5}, {"ex", 1.3}, {"kg1", 1180}, {"kp", 84}, {"kvb", 300}, {"cgk", 1.6e-12}, {"cgp", 1.5e-12}, {"cpk", 0.5e-12}, {"gscale", 6.2e-4}, {"gknee", 0.06}},
             "N. Koren (12AU7 set); caps: RCA 12AU7 datasheet"},
            // ── Pentodes / beam tetrodes: Koren plate law; screen = space current/kg2 + what the plate misses below its knee (Devices.hpp) + grid law ──
            {"6L6GC", "pentode", {{"mu", 14.571}, {"ex", 1.2918}, {"kg1", 677.94}, {"kg2", 9797.6}, {"kp", 26.115}, {"kvb", 17.247}, {"cg1k", 10e-12}, {"cg1p", 0.6e-12}, {"cpk", 6.5e-12}, {"gscale", 1.5e-3}, {"gknee", 0.08}},
             "Fitted (tube_fit.py) to the RCA 6L6GC plate families (Ec2 = 300 V, Ec1 0..-35 V with Ig2, 92CM-10133; Ec1 = 0 at Ec2 50..400 V, 92CM-10327) and the "
             "250 V / -14 V class-A point: RMS error, % of 372 mA, plate 2.4 -> 1.0, knee 20 -> 15, screen 0.8 -> 0.6; class-A point 68.7 / 4.7 mA (sheet 72 / 5). "
             "Previous: Koren's 6L6GC set. Caps from the RCA 6L6GC datasheet; grid conduction: Headroom law scaled for a power tube (approximate)"},
            {"EL34", "pentode", {{"mu", 13.972}, {"ex", 1.2334}, {"kg1", 347.46}, {"kg2", 1839}, {"kp", 48.096}, {"kvb", 29.752}, {"cg1k", 15e-12}, {"cg1p", 1.0e-12}, {"cpk", 8e-12}, {"gscale", 1.5e-3}, {"gknee", 0.08}},
             "Fitted (tube_fit.py) to the Philips EL34 data (1969): plate families at Vg2 = 250 and 360 V, Ia/Ig2 transfer at 250/250, 350/375, 400/425 V: "
             "RMS error, % of 459 mA, plate 5.5 -> 1.8, knee 8.1 -> 5.6, screen 0.3 -> 0.4 against Koren's set. The sheet's own table and curves disagree "
             "(250 V / -13.5 V: table 100 mA, curves 86 mA); the card follows the curves. Caps from the Philips EL34 datasheet; grid law approximate"},
            {"6V6GT", "pentode", {{"mu", 10.7}, {"ex", 1.31}, {"kg1", 1672}, {"kg2", 8000}, {"kp", 41.16}, {"kvb", 12.7}, {"cg1k", 9e-12}, {"cg1p", 0.7e-12}, {"cpk", 7.5e-12}, {"gscale", 1.79e-4}, {"gknee", 0.08}},
             "Koren-form 6V6GT set as widely circulated (Ia 46 mA at the 250 V / -12.5 V datasheet point, sheet 45); kg2 refitted for the screen law: Ig2 4.7 mA (sheet 4.5). "
             "Grid law gscale fitted (tube_fit.py) to the RCA 6V6-GTA Ic1 curves at Ec1 = +5 / +15 V (92CM-4807R2): grid-current error 312 % -> 12 % of 8.7 mA. "
             "The plate law is kept: a refit trades the plate and positive-grid regions for the chart's Ig2 (the law's Ig2/Ia ratio is fixed)"},
            {"EL84", "pentode", {{"mu", 22.537}, {"ex", 1.1861}, {"kg1", 320.18}, {"kg2", 2193.2}, {"kp", 128.43}, {"kvb", 21.731}, {"cg1k", 10.8e-12}, {"cg1p", 0.5e-12}, {"cpk", 6.5e-12}, {"gscale", 1.0e-3}, {"gknee", 0.08}},
             "Fitted (tube_fit.py) to the Philips EL84 data (1969): plate families at Vg2 = 250 and 300 V and the Ia/Ig2 transfer chart, anchored on "
             "Va = Vg2 = 250 V, Vg1 = -7.3 V: RMS error, % of 199 mA, plate 4.8 -> 2.2, knee 4.3 -> 4.7, screen 0.2 -> 0.15; class-A point 48.7 / 5.2 mA (sheet 48 / 5.5)"},
            {"6K6GT", "pentode", {{"mu", 7.2861}, {"ex", 1.3927}, {"kg1", 5141.3}, {"kg2", 12264}, {"kp", 31.538}, {"kvb", 9.8462}, {"cg1k", 5.5e-12}, {"cg1p", 0.5e-12}, {"cpk", 6e-12}, {"gscale", 1.1452e-4}, {"gknee", 0.08}},
             "Fitted (tube_fit.py, 2026-11-12) to the RCA 6K6-GT data: plate family at Vg2 = 250 V (92CM-4881R2, Ec1 0..-35 V, Ec1 = 0 Ic2), the Va = Vg2 sweep "
             "(92CM-5209R2, Ec1 0..-40 V, Vg2 to 400 V, Ec1 = 0 Ic2) and the three class A1 typical points (anchored): RMS error, % of 86 mA, plate 1.27, knee 4.5, "
             "screen 0.80, table 0.37 (100/100/-7 V: 8.75 / 1.83 mA, sheet 9 / 1.6; 250/250/-18 V: 31.5 / 5.12, sheet 32 / 5.5; 315/250/-21 V: 25.2 / 3.95, sheet 25.5 / 4); "
             "gm 1.54 / 2.25 / 2.02 mA/V (sheet 1.5 / 2.3 / 2.1). The law's atan plate has no linear slope, so rp is high (309k at 250 V, sheet 90k). Grid law gscale "
             "fitted to the Ic1 plateaus of 92CM-6311R1 (Ec1 +10..+40 V): 4.7 % of 29 mA (reads 3.6 mA at +10 V, sheet ~5.6). Caps: RCA data (Cg1p 0.5, in 5.5, out 6 pF). "
             "Data: work/tube-curves/6K6GT. Used by the Fender 6G15 reverb driver"},
            {"KT88", "pentode", {{"mu", 8.8}, {"ex", 1.35}, {"kg1", 730}, {"kg2", 4200}, {"kp", 32}, {"kvb", 16}, {"cg1k", 16e-12}, {"cg1p", 1.2e-12}, {"cpk", 12e-12}, {"gscale", 1.5e-3}, {"gknee", 0.08}},
             "N. Koren (KT88 set); caps approximate"},
            {"6550", "pentode", {{"mu", 7.9}, {"ex", 1.35}, {"kg1", 890}, {"kg2", 4200}, {"kp", 60}, {"kvb", 24}, {"cg1k", 14e-12}, {"cg1p", 0.85e-12}, {"cpk", 12e-12}, {"gscale", 1.5e-3}, {"gknee", 0.08}},
             "Koren-form 6550 set (approximate)"},
            // ── Opto: neon lamp + LDR ("roach") ──
            {"FENDER_ROACH", "ldr", {{"rdark", 10e6}, {"ron", 3e3}, {"gamma", 0.9}, {"vstrike", 65}, {"rlamp", 30e3}, {"smooth", 2.0}, {"drive", 28000.0}, {"release", 0.035}},
             "Neon lamp + CdS LDR as used in Fender AB763 vibrato channels (approximate: strike ≈65 V, ≈3 kΩ at ~1 mA of lamp current, "
             "megohms dark, ≈35 ms response like VTL5C-class cells; calibrated so an AB763 vibrato at full Intensity modulates "
             "10-16 dB from fast to slow Speed); light = capacitor charged by the lamp current, discharged through the release time constant"},
            // ── Spring reverb tanks (behavioural block with electrical transducers) ──
            {"4AB3C1B", "springtank", {{"rin", 8.0}, {"lin", 1.0e-3}, {"rout", 2575.0}, {"delay1", 0.0334}, {"delay2", 0.0392}, {"t60", 2.9}, {"disp", 0.62}, {"stages", 36}, {"lp", 4300}, {"hp", 90}, {"gain", 0.08}},
             "Accutronics 4AB3C1B long two-spring tank as fitted to AB763 reverb amps: 8 Ω input, 2575 Ω output, 2.75-4 s decay (Accutronics data); "
             "flight delays and dispersion approximate"},
            // ── Vacuum rectifiers: Child–Langmuir perveance fitted to the datasheet drop at rated current ──
            {"GZ34", "vacdiode", {{"k", 3.6e-3}, {"knee", 0.5}}, "GZ34/5AR4: ≈17 V drop at 250 mA per plate (Mullard data) -> K = 0.25/17^1.5"},
            {"5AR4", "vacdiode", {{"k", 3.6e-3}, {"knee", 0.5}}, "same as GZ34"},
            {"5U4GB", "vacdiode", {{"k", 9.4e-4}, {"knee", 0.5}}, "5U4GB: ≈44 V drop at 275 mA (RCA data) -> K = 0.275/44^1.5"},
            {"5Y3GT", "vacdiode", {{"k", 2.7e-4}, {"knee", 0.5}}, "5Y3GT: ≈60 V drop at 125 mA (RCA data)"},
            // ── JFETs: SPICE level 1 (Shichman-Hodges). Research: work/circuit-netlists/wave-b/JFET.md ──
            {"J201", "njf", {{"vto", -0.691}, {"beta", 787e-6}, {"lambda", 4.46e-3}, {"is", 30.7e-15}, {"n", 1.0}, {"cgs", 2.5e-12}, {"cgd", 2.5e-12}},
             "InterFET J201 SPICE card (interfet-spice-models.xlsx, 2026-11-09): IDSS 0.38 mA, Vgs(off) -0.69 V (datasheet typ -0.684 V). "
             "The onsemi/Fairchild J201.LIB card (VTO -3.95 V, IDSS 31 mA) contradicts the J201 datasheet (Vgs(off) -0.3 to -1.5 V) and is NOT used"},
            {"2N5457", "njf", {{"vto", -1.25}, {"beta", 1.04e-3}, {"lambda", 30.8e-3}, {"is", 95.6e-15}, {"n", 1.0}, {"cgs", 3.58e-12}, {"cgd", 3.58e-12}},
             "InterFET 2N5457 SPICE card: IDSS 1.63 mA (onsemi datasheet 1-5 mA)"},
            {"2N5458", "njf", {{"vto", -3.0}, {"beta", 0.667e-3}, {"lambda", 10e-3}, {"is", 10e-15}, {"n", 1.0}, {"cgs", 4.5e-12}, {"cgd", 1.5e-12}},
             "derived from the onsemi 2N5457/D datasheet typicals: IDSS 6 mA, |yfs| 4 mS -> |VTO| = 2 IDSS/gm0 = 3 V, BETA = IDSS/VTO^2; "
             "lambda, IS and capacitances assumed (no vendor card found)"},
            {"MPF102", "njf", {{"vto", -3.41}, {"beta", 1.04e-3}, {"lambda", 2e-3}, {"is", 33.57e-15}, {"n", 1.0}, {"cgs", 2.414e-12}, {"cgd", 1.6e-12}},
             "National MPF102 SPICE card as circulated (IDSS 12.1 mA); licence not stated"},
            {"2SK30A", "njf", {{"vto", -1.5}, {"beta", 1.78e-3}, {"lambda", 10e-3}, {"is", 1e-14}, {"n", 1.0}, {"cgs", 5.6e-12}, {"cgd", 2.6e-12}},
             "Toshiba 2SK30ATM GR rank, derived/assumed: IDSS 4 mA, VTO assumed -1.5 V; Ciss 8.2 pF, Crss 2.6 pF (datasheet, at VGD 10 V)"},
            {"J113", "njf", {{"vto", -1.2}, {"beta", 4.2e-3}, {"lambda", 6.49e-3}, {"is", 22.1e-15}, {"n", 1.0}, {"cgs", 10.9e-12}, {"cgd", 10.9e-12}},
             "datasheet-centred J113 (derived: rDS(on) = 1/(2 BETA |VTO|) ~ 99 ohm, IDSS 6 mA); lambda, IS, caps from InterFET J113A-H"},
            {"2N5952", "njf", {{"vto", -2.2}, {"beta", 1.24e-3}, {"lambda", 10e-3}, {"is", 1e-14}, {"n", 1.0}, {"cgs", 3e-12}, {"cgd", 1.5e-12}},
             "derived from the Central Semiconductor 2N5949-series datasheet: IDSS 6 mA, VTO near the Vgs(off) range centre; lambda from gos <= 75 uS; caps assumed"},
            // ── OTAs (one amplifier; DEVICES.md §1). abc junction law (is, n) × junctions; output compliance usat_hi / usat_lo
            //    from V+ / V−, smoothing w; input bf (base current), Early-like va (0 = off), output mirror ratio ──
            {"CA3080", "ota", {{"is", 5.9e-16}, {"n", 1.0}, {"junctions", 1}, {"usat_hi", 1.5}, {"usat_lo", 0.6}, {"w", 0.1}, {"bf", 125}, {"va", 7500}, {"mirror", 1.0}},
             "Intersil CA3080 FN475.5 / AN6668: gm = 19.2·IABC (= IABC/2Vt), bias pin 0.71 V above V− at 500 µA (one junction -> IS 5.9e-16, D), "
             "swing +13.5/−14.4 V at ±15 V (usat 1.5/0.6, D), RO ≈ 7.5 MΩ·mA/IABC (va 7500 V, D), IB 2 µA at IABC 500 µA (bf 125, D); w 0.1 V assumed"},
            {"CA3094", "ota", {{"is", 5.9e-16}, {"n", 1.0}, {"junctions", 1}, {"usat_hi", 1.5}, {"usat_lo", 0.6}, {"w", 0.1}, {"bf", 125}, {"va", 7500}, {"mirror", 1.0}},
             "CA3094 OTA section: CA3080-type OTA (Intersil CA3094 datasheet; values as CA3080, unverified for the 3094 die). The on-chip "
             "output Darlington is placed in the netlist as npn parts (model CA3094_Q)"},
            {"LM13700", "ota", {{"is", 1.0e-15}, {"n", 1.05}, {"junctions", 2}, {"usat_hi", 0.8}, {"usat_lo", 0.6}, {"w", 0.1}, {"bf", 625}, {"va", 0}, {"mirror", 1.0}},
             "TI LM13700 SNOSBW2F: gm ≈ 19.2·IABC, VBIAS = 2·VBE + V− (Fig. 10: 1.0 V at 0.1 µA, 1.5 V at 1 mA -> n 1.05, IS 1e-15 per junction, D), "
             "swing +14.2/−14.4 V (usat 0.8/0.6, D), IB 0.4 µA at 500 µA (bf 625, D); w assumed"},
            {"LM13600", "ota", {{"is", 1.0e-15}, {"n", 1.05}, {"junctions", 2}, {"usat_hi", 0.8}, {"usat_lo", 0.6}, {"w", 0.1}, {"bf", 625}, {"va", 0}, {"mirror", 1.0}},
             "LM13600: the LM13700 amplifier (National DS007981); its buffer bias tied to IABC is not modelled (v1)"},
            // ── Zeners (diode bv / ibv / nbv: the ngspice breakdown law). bv = Vz nominal at the test current ibv = Izt; nbv from
            //    the zener impedance, Zzt = nbv·Vt/Izt (rs 0); forward IS / N generic silicon (A) ──
            {"1N4731A", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 7.2}, {"bv", 3.8824}, {"ibv", 58e-3}, {"nbv", 4.0}},
             "1N4728A–1N4764A series (Vishay / Microsemi): 4.3 V at Izt 58 mA, Zzt 9 Ω (S). nbv 4 (A) keeps the SPICE knee "
             "voltage positive (nbv ≤ 5.6 here), rs = Zzt − nbv·Vt/Izt (D) and bv = 4.3 V − Izt·rs at the junction, so the terminals read "
             "4.3 V at Izt (D); the 500 Ω Zzk at 1 mA is not reached (110 Ω)"},
            {"1N5235B", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0.0}, {"bv", 6.8}, {"ibv", 20e-3}, {"nbv", 3.87}},
             "1N5221B–1N5267B series (onsemi / Microsemi): 6.8 V at Izt 20 mA, Zzt 5 Ω (S); nbv = Zzt·Izt/Vt (D)"},
            {"1N961B", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0.0}, {"bv", 10.0}, {"ibv", 12.5e-3}, {"nbv", 4.11}},
             "1N957B–1N992B series (Microsemi): 10 V at Izt 12.5 mA, Zzt 8.5 Ω (S); nbv = Zzt·Izt/Vt (D)"},
            {"1N4744A", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0.0}, {"bv", 15.0}, {"ibv", 17e-3}, {"nbv", 9.2}},
             "1N4728A–1N4764A series: 15 V at Izt 17 mA, Zzt 14 Ω (S); nbv = Zzt·Izt/Vt (D)"},
            {"1N5245B", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0.0}, {"bv", 15.0}, {"ibv", 8.5e-3}, {"nbv", 5.26}},
             "1N5221B–1N5267B series: 15 V at Izt 8.5 mA, Zzt 16 Ω (S); nbv = Zzt·Izt/Vt (D)"},
            {"LM13700_DIODE", "diode", {{"is", 1.0e-15}, {"n", 1.05}, {"rs", 0.0}},
             "LM13700 linearising diode: the same junction law as the abc junctions (derived, D); IS sets only the absolute voltage"},
            {"CA3094_Q", "npn", {{"is", 1.0e-14}, {"bf", 100}, {"br", 1}},
             "CA3094 output Darlington transistor (approximate: generic IC npn, hFE ≈ 100; the datasheet gives the pair's 100 mA peak rating)"},
            {"LM13700_Q", "npn", {{"is", 1.0e-14}, {"bf", 100}, {"br", 1}},
             "LM13700 Darlington buffer transistor (approximate: generic IC npn)"},
            // ── Bucket-brigade delays (Blocks.hpp BbdLine; DEVICES.md §2.1). eps = N·ε total transfer loss, a2 = H2 term
            //    (1/V) about the window centre, vlo/vhi = input window (absolute volts in a −15 V VDD / 0 V ground
            //    circuit; units with other references override), noise = rms per bucket sample (V), rout = output ──
            {"MN3002", "bbd", {{"stages", 512}, {"eps", 0.05}, {"a2", 0.02}, {"vlo", -11.5}, {"vhi", -4.5}, {"vshift", 0}, {"noise", 0.25e-3}, {"rout", 2e3}, {"cin", 0}},
             "Panasonic MN3002: 512 stages (S, djjondent / madbean CE-1 pin map); THD 0.4 %, S/N 70 dB (U, snippet); eps, a2, window, "
             "rout assumed (A)"},
            {"MN3005", "bbd", {{"stages", 4096}, {"eps", 0.1}, {"a2", 0.045}, {"vlo", -11.0}, {"vhi", -4.6}, {"vshift", 0}, {"noise", 0.8e-3}, {"rout", 2e3}, {"cin", 0}},
             "Panasonic MN3005 (Xvive reissue sheet): 4096 stages, input bias −5…−10 V (curves at −7.8 V), 0.9 Vrms at 2.5 % THD (a2 0.045/V, D), "
             "0.8 mVrms A-weighted noise (S); eps 0.1 (A: insertion loss 0-4 dB at fcp/2 per Raffel & Smith, D); rout assumed"},
            {"MN3007", "bbd", {{"stages", 1024}, {"eps", 0.1}, {"a2", 0.0091}, {"vlo", -11.0}, {"vhi", -4.6}, {"vshift", 0}, {"noise", 0.3e-3}, {"rout", 2e3}, {"cin", 0}},
             "Panasonic MN3007 (Xvive scan): 1024 stages, 1.5 Vrms at 2.5 % THD, 0.5 % THD at 0.78 Vrms (a2 0.0091/V, D), 0.3 mVrms noise (S); "
             "eps, rout assumed (A)"},
            {"MN3207", "bbd", {{"stages", 1024}, {"eps", 0.1}, {"a2", 0.023}, {"vlo", 2.0}, {"vhi", 6.0}, {"vshift", 0}, {"noise", 0.25e-3}, {"rout", 2e3}, {"cin", 0}},
             "Panasonic MN3207: 1024 stages, +5…+10 V supply, 0.4 % THD at 0.25 Vrms (a2 0.023/V, D), 0.25 mVrms noise (S); window, eps, rout assumed"},
            {"SAD1024", "bbd", {{"stages", 512}, {"eps", 0.15}, {"a2", 0.03}, {"vlo", 2.0}, {"vhi", 12.0}, {"vshift", 0}, {"noise", 0.5e-3}, {"rout", 2e3}, {"cin", 0}},
             "Reticon SAD1024 (one of its two 512-stage sections): values approximate (A); netlists set the window to their bias"},
            // ── Tape (Blocks.hpp TapeLoop; DEVICES.md §6) ──
            {"RE201", "tape", {{"d1", 0.069}, {"d2", 0.131}, {"d3", 0.189}, {"range", 2.57}, {"ms", 3.5e5}, {"a", 22e3}, {"alpha", 1.6e-3},
                               {"drive", 4e8}, {"gain", 5e-7}, {"v0", 0.19}, {"gap", 5}, {"spacing", 3}, {"thickness", 10}, {"wow", 1e-3},
                               {"flutter", 5e-4}, {"servo", 0.25}, {"rrec", 100}, {"lrec", 10e-3}, {"rpb", 300}, {"lpb", 0.1}},
             "Roland RE-201: head delays 69 / 131 / 189 ms at the fastest Repeat Rate, 2.57× range (UA RE-201 manual, S/D); tape "
             "magnetisation Ms 3.5e5 A/m, a 22 kA/m, α 1.6e-3 (Chowdhury DAFx-19, S); tape speed, head R/L, gap/spacing/coating, record "
             "drive, playback gain, wow/flutter amplitudes and servo time are NOT sourced (A): see DEVICES.md §6.4"},
            {"RE201_SPRING", "springtank", {{"rin", 10.0}, {"lin", 0.4e-3}, {"rout", 2575.0}, {"delay1", 0.021}, {"delay2", 0.025}, {"delay3", 0.029}, {"t60", 2.2}, {"disp", 0.6}, {"stages", 30}, {"lp", 4500}, {"hp", 100}, {"gain", 0.08}},
             "RE-201 3-spring tank (Roland spec 'REVERB UNIT: (3 springs system)'); electrical data of the usual replacement Accutronics "
             "8AB2D1A (10 Ω in, 2575 Ω out, medium decay, S); flight delays, decay time and input inductance assumed (A)"},
            // ── Compander (lib:ne570; Philips NE570 product data 2003, Figs. 2, 6, 9, 10) ──
            {"NE570", "compander", {{"r1", 10e3}, {"r2", 20e3}, {"r5", 10e3}, {"i1", 140e-6}, {"delta", 1e-3}},
             "Philips NE570 (2003): R1 10 kΩ, R2 20 kΩ, R5 10 kΩ (Fig. 9), IB = I1 = 140 µA, IG = 2·VIN(avg)/R1 (S); the gain cell's "
             "tanh limit at |IIN| = I1 and the rectifier rounding δ 1 mV are assumed (A)"},
            {"NE570_OPAMP", "opamp", {{"level", 1}, {"a0", 1e5}, {"gbw", 1e6}, {"slew", 0.5e6}, {"swing_hi", 1.5}, {"swing_lo", 1.0}, {"vhi", 13.5}, {"vlo", -13.5}, {"iq", 1.6e-3}},
             "NE570 internal op-amp: 1 MHz, ±0.5 V/µs, ±20 mA (S); gain and output swing assumed 741-like (A); iq = half the 3.2 mA chip ICC"},
            {"NE570_Q8", "diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0}},
             "NE570 rectifier mirror input (Q8, diode-connected; generic IC npn junction, A)"},
            // ── Blackmer VCAs (part "vca"; docs/CIRCUIT_ENGINE.md "vca and rms"). mvdb = gain constant (mV/dB at the chip
            //    temperature: 2VT·ln10/20); iq = idle current per core path; vsym / rs / rd = fitted symmetry offset (V),
            //    ohmic drop per path (Ω) and output-path ohmic asymmetry (Ω); rsym = SYM pin to Ec+ (Ω); rc = control
            //    port to GND (Ω, 0 = high impedance); icc = quiescent supply current V+ → V− (A). Fits: tools in
            //    work/vca-rms/fit (THD+N with the datasheet noise floor added, Rin = Rout = 20 kΩ, 1 kHz) ──
            {"THAT2180A", "vca", {{"mvdb", 6.1}, {"iq", 20e-6}, {"vsym", 3.09e-5}, {"rs", 0.383}, {"rd", 0.108}, {"rsym", 25}, {"rc", 0}, {"icc", 0}, {"v5", 2.85}, {"r5", 20}},
             "THAT 2180A (doc 600029 rev 02): 6.1 mV/dB at TA 25 °C (Tchip ≈ 35 °C), 20 µA idle, SYM 25 Ω to Ec+ (Fig. 5), pin 5 at −2.85 V "
             "(its resistor to VEE sets the 2.4 mA supply current, S; r5 20 Ω assumed, A); "
             "vsym/rs/rd fitted (D) to THD 0.005 % (0 dBV, 0 dB), 0.024 % at 10 Vrms (Fig. 10), 0.02 % at +10 dBV/−15 dB and −5 dBV/+15 dB"},
            {"THAT2180B", "vca", {{"mvdb", 6.1}, {"iq", 20e-6}, {"vsym", -2.898e-5}, {"rs", 0.5417}, {"rd", 0.0616}, {"rsym", 25}, {"rc", 0}, {"icc", 0}, {"v5", 2.85}, {"r5", 20}},
             "THAT 2180B (doc 600029): as 2180A (S); vsym/rs/rd fitted (D) to THD 0.010 % (0 dBV, 0 dB; Fig. 13 H2 −80.1 dBc, H4 −104 dBc), "
             "0.032 % at 10 Vrms (Fig. 10), 0.03 % at ±15 dB"},
            {"THAT2180C", "vca", {{"mvdb", 6.1}, {"iq", 20e-6}, {"vsym", -1.041e-4}, {"rs", 0.3762}, {"rd", 0.0604}, {"rsym", 25}, {"rc", 0}, {"icc", 0}, {"v5", 2.85}, {"r5", 20}},
             "THAT 2180C (doc 600029): as 2180A (S); vsym/rs/rd fitted (D) to THD 0.030 % (0 dBV, 0 dB), 0.063 % at 10 Vrms (Fig. 10), 0.04 % at ±15 dB"},
            {"DBX2151", "vca", {{"mvdb", 6.1}, {"iq", 20e-6}, {"vsym", 1.19e-5}, {"rs", 0.515}, {"rd", 0.0}, {"rsym", 0}, {"rc", 0}, {"icc", 0}, {"vczero", 0},
                                {"v5", 2.76}, {"r5", 20}, {"bf", 0}},
             "dbx / THAT 2151 (THAT 2150 Series data sheet rev. 10/25/96; dbx AN103): 6.1 mV/dB at Ec− pin 3 and Ec+ pins 2 / 4, gain at 0 V "
             "control 0 ±0.1 dB (vczero 0 typ), 20 µA idle; control pins are transistor bases (high impedance, \"sourced from a low "
             "impedance\": rc 0, bf 0 = ideal), SYM pin 4 is a separate base (rsym 0: wire it, 51 Ω to GND in Fig. 3); pin 5 held below "
             "GND by the bias network (AN103: four junction drops; Fig. 3's 5.1 kΩ to −15 V at ISET 2.4 mA puts it at −2.76 V, D), r5 "
             "assumed (A); vsym / rs fitted (D) to THD 0.004 % (0 dB) and 0.025 % (±15 dB) at IIN+IOUT 180 µA, 1 kHz (S)"},
            {"DBX202", "vca", {{"mvdb", 6.0}, {"iq", 20e-6}, {"vsym", 7.4e-6}, {"rs", 0.0761}, {"rd", 0.0}, {"rsym", 51}, {"rc", 203}, {"icc", 0}},
             "dbx 202 'Black Can' (THAT DN127 Fig. 2: In 1, Ec 2, VEE 3, Sym 4, Gnd 5, Out 6, VCC 7; −6 mV/dB, 100 kΩ in/out, Sym via 1 MΩ "
             "from a 50 kΩ trim, S). No Black Can THD/port data is published: its documented drop-in, the THAT 202R (202data.pdf), gives "
             "6.0 mV/dB, control port 203 Ω, symmetry port 51 Ω (S) and THD 0.003 % at 0 dB / 0.02 % at ±20 dB, IIN+IOUT 700 µA, which "
             "vsym/rs are fitted to (D, rd 0); iq assumed as the 2180 (A); the 202R's paralleled-IC 1.25 kΩ input is not modelled; "
             "icc unpublished (0: not drawn). Use Ec−, Ec+ to GND"},
            // ── CMOS 4000B logic (parts dff / nand / inv; docs/CIRCUIT_ENGINE.md §10.14). Channel law β / vt / theta / lambda /
            //    kappa fitted (D) to the TI IOL / IOH tables (identical for 4013B, 4011B, 4069UB) and the CD4069UB Fig. 3 / 4
            //    curves; vth = switching threshold / (VDD − VSS); tpd5 / 10 / 15 = typical propagation delay (s); cin (F);
            //    iddq = quiescent current per output; ub = unbuffered inverter (the channel pair itself, no logic cell) ──
            {"CD4013B", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"vth", 0.5},
                                 {"tpd5", 150e-9}, {"tpd10", 65e-9}, {"tpd15", 45e-9}, {"cin", 5e-12}, {"iddq", 5e-9}, {"ub", 0}},
             "TI CD4013B (SCHS023E): clock to Q 150 / 65 / 45 ns at 5 / 10 / 15 V, CIN 5 pF, IOL 1 / 2.6 / 6.8 mA at VO 0.4 / 0.5 / "
             "1.5 V, IOH −3.2 mA at 2.5 V (VDD 5), VIL / VIH 1.5 / 3.5 V at 5 V (S); channel law fitted (D); threshold VDD/2 (A, "
             "within the VIL–VIH window); set / reset to Q delays use the clock figure (A)"},
            {"CD4027B", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"vth", 0.5},
                                 {"tpd5", 150e-9}, {"tpd10", 65e-9}, {"tpd15", 45e-9}, {"cin", 5e-12}, {"iddq", 5e-9}, {"ub", 0}},
             "TI CD4027B (SCHS032D): clock to Q 150 / 65 / 45 ns at 5 / 10 / 15 V, CIN 5 pF, IOL 1 / 2.6 / 6.8 mA (as the 4013B), truth table "
             "§8.2 (J·Q̄ + K̄·Q on the rising clock, set / reset asynchronous, both high: Q = Q̄ = 1) (S); channel law as CD4013B (D)"},
            {"CD4011B", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"vth", 0.5},
                                 {"tpd5", 125e-9}, {"tpd10", 60e-9}, {"tpd15", 45e-9}, {"cin", 5e-12}, {"iddq", 5e-9}, {"ub", 0}},
             "TI / Harris CD4011B (SCHS021D): tPHL, tPLH 125 / 60 / 45 ns, CIN 5 pF, same IOL / IOH table as the 4013B (S); "
             "channel law as CD4013B (D)"},
            {"CMOS_INV", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"vth", 0.5},
                                  {"tpd5", 125e-9}, {"tpd10", 60e-9}, {"tpd15", 45e-9}, {"cin", 5e-12}, {"iddq", 5e-9}, {"ub", 0}},
             "buffered 4000B inverter (a 4011B input stage with tied inputs: the 4011B delays and drive, S/D)"},
            {"CD4069UB", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"betan", 1.215e-3},
                                  {"betap", 1.215e-3}, {"cin", 10e-12}, {"ub", 1}},
             "TI CD4069UB (SCHS054E): unbuffered; IOL / IOH table as the 4013B, Fig. 4 sink curves (VGS 5 / 10 / 15 V saturating near "
             "4.4 / 14.2 / 27 mA), Fig. 3 crowbar current 4.8 / 11 mA at VDD 10 / 15 V, CIN 10 pF (S); channel law fitted (D): "
             "transfer threshold VDD/2, midpoint gain 150 / 34 / 18 at 5 / 10 / 15 V, crowbar 0.38 mA at 5 V (Fig. 3 shape)"},
            {"CD4049UB", "cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"betan", 7.90e-3},
                                  {"betap", 1.46e-3}, {"cin", 15e-12}, {"ub", 1}},
             "TI CD4049UB: unbuffered inverting buffer, IOL 6.4 / 16 / 48 mA (6.2–7× the 4069UB: betan ×6.5, D), IOH 1.2 / 3.9 / 3 mA "
             "(×1.2: betap, D), CIN 15 pF (S); channel law otherwise as CD4069UB (D); its threshold sits below VDD/2 as a result"},
            // ── RMS detectors (part "rms"). Output = 2VT·ln(Irms/Iin0) at mvdb, Iin0 = √(IBIAS·IT)/kgeo, τ = CT·VT/IT with the
            //    external CT and timing current; IBIAS = pin-2 current through nb junctions (vbias at ibref); vsym = SYM
            //    voltage for symmetry (Q1/Q2 mirror); iflr = input-bias floor (rms-added); vofs = CAP level at 0 V out ──
            {"AD536AJ", "ad536", {{"r1", 25e3}, {"r2", 25e3}, {"r4", 50e3}, {"rin", 16.67e3}, {"mvdb", 3.0}, {"iref", 20e-6}, {"vfloor", 1e-3}, {"i4max", 0.049}, {"icc", 1.2e-3}},
             "Analog Devices AD536A rev. G (D-14): Fig. 6 internals R1 25 kΩ (25 ms/µF), R2 25 kΩ, R4 50 kΩ (IOUT 40 µA/V rms), input "
             "16.67 kΩ, dB −3 mV/dB with 0 dB at IREF 20 µA = 1 V rms, 1.2 mA quiescent (S); i4max fitted (D) so a 1 V rms pulse train "
             "of crest factor 7 reads −1.0 % (spec); input floor 1 mV rms (A, the 7 mV dB-error limit)"},
            {"THAT2252", "rms", {{"mvdb", 6.1}, {"kgeo", 2.9}, {"nb", 3}, {"vbias", 2.1}, {"ibref", 24e-6}, {"vsym", 0.0}, {"iflr", 16e-9}, {"vofs", 1.4}, {"icc", 1e-3}},
             "THAT 2252 (doc 600032 rev 03): 6.1 mV/dB, Iin0 = √(IBIAS·IT)/2.9, τ = CT·VT/IT, pin 2 = VEE + 2.1 V through three diodes "
             "at 24 µA, CAP at +2 VBE, ICC 1 mA (S); vsym 0 = trimmed (the untrimmed part needs +8 mV typ, S); iflr 16 nA from the 1 dB "
             "linearity error at 31.6 nA (D)"},
            // ── Optical (DEVICES.md §4.2): emitters drive a normalised light node (λ = 1 at full drive); cds cells read it.
            //    cds: G = 1/rdark + (couple·λ)^gamma/ron at rest; dynamics kr (attack ≈ 1/(2·kr) at full light), tau_l (dark
            //    leak), tau_h / mh / mr (history; 0 = off) ──
            {"VTL5C3_CELL", "cds", {{"rdark", 10e6}, {"ron", 1e3}, {"gamma", 1.0}, {"kr", 230}, {"tau_l", 2.0}, {"tau_h", 0}, {"mh", 0}, {"mr", 0}},
             "PerkinElmer/CoolAudio VTL5C3: 10 kΩ / 1 kΩ / 500 Ω at 1 / 10 / 40 mA, dark 10 MΩ (S); with iref 10 mA, gamma 1 fits 1-10 mA "
             "(D; predicts 250 Ω at 40 mA, unverified); turn-on 2.5 ms, turn-off 35 ms to 100 kΩ -> kr 230 (2.2 ms / 39 ms, D); tau_l assumed"},
            {"VTL5C3_LED", "led", {{"is", 2.7551e-9}, {"n", 3.9696}, {"rs", 0}, {"iref", 0.01}, {"tau_light", 1e-4}},
             "VTL5C3/2 LED as fitted by Parker & D'Angelo, DAFx-13 (IS 2.7551 nA, n 3.9696, S); iref 10 mA = full light (λ = 1)"},
            {"NSL32_CELL", "cds", {{"rdark", 500e3}, {"ron", 500}, {"gamma", 1.0}, {"kr", 300}, {"tau_l", 2.0}, {"tau_h", 0}, {"mh", 0}, {"mr", 0}},
             "Silonex NSL-32: 500 Ω at 20 mA, dark 500 kΩ, rise 3.5 ms, decay 500 ms to 100 kΩ (S); gamma assumed 1; kr 300 compromises "
             "rise (1.7 ms) and decay (0.8 s) (A)"},
            {"NSL32_LED", "led", {{"is", 1.0e-19}, {"n", 1.6}, {"rs", 5.0}, {"iref", 0.02}, {"tau_light", 1e-4}},
             "NSL-32 LED: generic red LED fit (Vf ≈ 1.5 V at 1 mA, approximate); iref 20 mA = full light"},
            {"UNIVIBE_LDR", "cds", {{"rdark", 20e6}, {"ron", 5e3}, {"gamma", 0.7}, {"kr", 100}, {"tau_l", 2.0}, {"tau_h", 0}, {"mh", 0}, {"mr", 0}},
             "Uni-Vibe CdS cell starting point: DAFx-19 (Darabundit et al.) measured 6.2-12.7 kΩ min and 2.6-4.2 MΩ max under modulation (S); "
             "rdark, ron, gamma 0.7 (CdS 0.6-0.9) and kr assumed, to be fitted per stage"},
            {"UNIVIBE_LAMP", "lamp", {{"vrated", 28}, {"irated", 0.04}, {"rcold_ratio", 1.0 / 13.5}, {"tau", 0.02}, {"tlambda", 20250}, {"thot", 2500}, {"alpha", 1.214}, {"tau_light", 1e-4}},
             "Uni-Vibe 28 V / 40 mA bulb (R. G. Keen, GeoFex, S); R ∝ T^1.214 (Agrawal 2017, S); T_hot 2500 K and tau 20 ms assumed; "
             "tlambda from light ∝ V^3.4, T ∝ V^0.42 (D, DEVICES.md §4.2)"},
            {"T4B_EL", "elpanel", {{"cp", 3e-9}, {"rs", 1e4}, {"b", 20.0}, {"veps", 0.5}, {"vref", 60.0}, {"fref", 1000.0}, {"tau_light", 1e-3}},
             "T4B electroluminescent panel (Teletronix LA-2A): Cp 2-3.5 nF from 2-3.5 nF/in² (Micrel AN-36, A); Rs, b (Alfrey-Taylor field "
             "activation), the 60 Vrms / 1 kHz reference and the 1 ms phosphor decay are assumed (no published T4B L(V, f) data): fit to "
             "LA-2A gain-reduction measurements"},
            {"NE2", "neon", {{"vstrike", 65}, {"rlamp", 30e3}, {"smooth", 2.0}, {"iref", 1e-3}, {"tau_light", 1e-4}},
             "NE-2 neon lamp: strike ≈ 65 V as FENDER_ROACH (approximate); 1 mA = full light (A)"},
            // ── Germanium BJTs (ICBO collector-base leakage, Gummel-Poon ISE/NE low-current beta droop, VAF Early) ──
            {"AC128", "pnp", {{"is", 85.8e-9}, {"bf", 85}, {"br", 20}, {"ise", 0.435e-9}, {"ne", 1.2}, {"vaf", 102.2}, {"cje", 6e-12}, {"cjc", 3.75e-12}},
             "AC128 SPICE card as circulated (IS 85.8n BF 85 BR 20 ISE .435n NE 1.2 VAF 102.2 CJE 6p CJC 3.75p); RB/RE/RC and TF omitted; "
             "the card carries no ICBO term"},
            {"NKT275", "pnp", {{"is", 0.1e-6}, {"bf", 75}, {"br", 20}, {"icbo", 2e-6}, {"vleak", 0.2}},
             "derived: IS from ElectroSmash's Fuzz Face bias (Vbe ~0.2 V at 0.25 mA); BF a typical selected Q1 value; BR borrowed from AC128; "
             "ICBO 2 uA assumed (fuzz-grade parts 1-3 uA, ICEO ~ BF x ICBO)"},
            // ── Loudspeakers (type "speaker", docs §4 / §10.21): T/S small-signal data from the makers' sheets (S) or a measured
            // set (M); D = derived; A = assumed large-signal shapes (Klippel-typical) and thermal network. x in mm in b*/k*/lx*.
            {"CELESTION_G12M25_8", "speaker", {{"re", 6.7}, {"le", 0.000137337}, {"l2", 0.000274673}, {"r2", 20.1}, {"bl", 12.14}, {"mms", 0.02922}, {"kms", 6488.77}, {"rms", 1.52996}, {"sd", 0.05251}, {"b1", -0.0138412}, {"b2", -0.0270836}, {"b3", 0.000147069}, {"b4", 0.000271651}, {"k1", 0.0277778}, {"k2", 0.0341435}, {"k4", 0.0208976}, {"lx1", -0.125}, {"lx2", 0.0138889}, {"alpha", 0.00393}, {"rtv", 3.08}, {"ctv", 2.5974}, {"rtm", 1.32}, {"ctm", 681.818}},
             "Celestion G12M-25 Greenback 8 ohm. S: Re 6.7, Fs 75 Hz (Celestion datasheet). M: Mms 29.22 g, Bl 12.14 T*m, Qms 9, Le 0.41 mH, Sd 525.1 cm2, Xmax 1.2 mm (measured set posted by 'fyl', Music Electronics Forum 2010-05-18, which read Fs 85 Hz). D: Kms refit to the datasheet Fs; LR-2 coil split L2 = 2 Le, R2 = 3 Re fitted to Le(1 kHz). A: Bl(x) from a 6 mm gap / 8.4 mm coil with exponential fringes (front 0.25, back 0.4 gap); Kms(x) 133 % at 1.5 Xmax, 300 % at 2.5 Xmax, +5 % asymmetry; Le(x) -15 %/Xmax (Klippel-typical shapes); thermal 110 K at rated 25 W (70 % coil, 30 % magnet), tau 8 s / 15 min"},
            {"CELESTION_G12M25_16", "speaker", {{"re", 13.1}, {"le", 0.000268524}, {"l2", 0.000537048}, {"r2", 39.3}, {"bl", 16.9753}, {"mms", 0.02922}, {"kms", 6488.77}, {"rms", 1.52996}, {"sd", 0.05251}, {"b1", -0.0138412}, {"b2", -0.0270836}, {"b3", 0.000147069}, {"b4", 0.000271651}, {"k1", 0.0277778}, {"k2", 0.0341435}, {"k4", 0.0208976}, {"lx1", -0.125}, {"lx2", 0.0138889}, {"alpha", 0.00393}, {"rtv", 3.08}, {"ctv", 2.5974}, {"rtm", 1.32}, {"ctm", 681.818}},
             "Celestion G12M-25 Greenback 16 ohm. S: Re 13.1, Fs 75 Hz (Celestion datasheet). D: the 8 ohm card's cone and motor with the coil scaled to Re 13.1 (Bl x sqrt(13.1/6.7), Le x 13.1/6.7, so Qes is unchanged). Otherwise as CELESTION_G12M25_8"},
            {"CELESTION_V30_16", "speaker", {{"re", 12.9}, {"le", 0.00026801}, {"l2", 0.000536019}, {"r2", 38.7}, {"bl", 16.4}, {"mms", 0.029}, {"kms", 6439.92}, {"rms", 1.48543}, {"sd", 0.05251}, {"b1", -0.0138412}, {"b2", -0.0270836}, {"b3", 0.000147069}, {"b4", 0.000271651}, {"k1", 0.0277778}, {"k2", 0.0341435}, {"k4", 0.0208976}, {"lx1", -0.125}, {"lx2", 0.0138889}, {"alpha", 0.00393}, {"rtv", 1.28333}, {"ctv", 6.23377}, {"rtm", 0.55}, {"ctm", 1636.36}},
             "Celestion Vintage 30 16 ohm. S: Re 12.9, Fs 75 Hz (Celestion datasheet). M: Mms 29 g, Bl 16.4 T*m, Qms 9.2 (a forum measurement attributed to the V30 only by its Re of 12.91 ohm). A: Le(1 kHz) 0.8 mH, Sd, motor geometry and nonlinear shapes as the G12M (same 44 mm coil family); thermal 110 K at rated 60 W"},
            {"JENSEN_P12Q_8", "speaker", {{"re", 5.6}, {"le", 0.000227608}, {"l2", 0.000455215}, {"r2", 16.8}, {"bl", 5.88}, {"mms", 0.0268}, {"kms", 8665.46}, {"rms", 1.31713}, {"sd", 0.04909}, {"b1", -0.0135958}, {"b2", -0.0284117}, {"b3", 0.000138947}, {"b4", 0.000302024}, {"k1", 0.0333333}, {"k2", 0.0491667}, {"k4", 0.0433333}, {"lx1", -0.15}, {"lx2", 0.02}, {"alpha", 0.00393}, {"rtv", 1.925}, {"ctv", 4.15584}, {"rtm", 0.825}, {"ctm", 1090.91}},
             "Jensen P12Q 8 ohm (Jensen specification sheet, S): Re 5.6, Fs 90.5 Hz, Qms 11.57, Mms 26.8 g, Bl 5.88 T*m, Le 0.67 mH, Sd 490.9 cm2, Xmax 1 mm, winding depth 8 mm, gap depth 6 mm (the sheet's Qes 3.33 is inconsistent with its Qms/Qts; this card's Qes is 2.47 from Bl, Mms, Re). A: shapes as CELESTION_G12M25_8 from that geometry; thermal 110 K at rated 40 W"},
            {"JENSEN_C12N_8", "speaker", {{"re", 6.05}, {"le", 0.000309017}, {"l2", 0.000618035}, {"r2", 18.15}, {"bl", 10.46}, {"mms", 0.0299}, {"kms", 15072.6}, {"rms", 2.82301}, {"sd", 0.04909}, {"b1", -0.00986854}, {"b2", -0.0168074}, {"b3", 5.1341e-05}, {"b4", 0.000106939}, {"k1", 0.0333333}, {"k2", 0.0491667}, {"k4", 0.0433333}, {"lx1", -0.15}, {"lx2", 0.02}, {"alpha", 0.00393}, {"rtv", 1.54}, {"ctv", 5.19481}, {"rtm", 0.66}, {"ctm", 1363.64}},
             "Jensen C12N 8 ohm (Jensen specification sheet, S): Re 6.05, Fs 113 Hz, Qms 7.52, Mms 29.9 g, Bl 10.46 T*m, Le 0.9 mH, Sd 490.9 cm2, Xmax 1 mm, winding depth 10 mm, gap depth 8 mm. A: shapes from that geometry; thermal 110 K at rated 50 W"},
            {"EMINENCE_LEGEND1258_8", "speaker", {{"re", 7.44}, {"le", 0.000236065}, {"l2", 0.00047213}, {"r2", 22.32}, {"bl", 10.9}, {"mms", 0.032}, {"kms", 11162.6}, {"rms", 3.07314}, {"sd", 0.05067}, {"b1", -0.00924796}, {"b2", -0.0179284}, {"b3", 3.31187e-05}, {"b4", 0.000122929}, {"k1", 0.0666667}, {"k2", 0.196667}, {"k4", 0.693333}, {"lx1", -0.3}, {"lx2", 0.08}, {"alpha", 0.00393}, {"rtv", 1.02667}, {"ctv", 7.79221}, {"rtm", 0.44}, {"ctm", 2045.45}},
             "Eminence Legend 1258 8 ohm (Eminence datasheet, S): Re 7.44, Fs 94 Hz, Qms 6.15, Mms 32 g, Bl 10.9 T*m, Le 0.70 mH, Sd 506.7 cm2, Xmax 0.5 mm, gap height 7.92 mm. A: shapes from that geometry; thermal 110 K at rated 75 W"},
        };
        std::map<std::string, DeviceModel> m;
        for (const auto& e : entries) {
            DeviceModel d;
            d.name = e.name;
            d.kind = e.kind;
            d.note = e.provenance;
            for (const auto& [k, v] : e.params) d.p[k] = v;
            m.emplace(d.name, std::move(d));
        }
        return m;
    }();
    return lib;
}

// Parameters each kind accepts (validation) with defaults used when a local model omits them.
inline const std::map<std::string, double>& modelDefaults(const std::string& kind) {
    static const std::map<std::string, std::map<std::string, double>> d = {
        {"diode", {{"is", 1e-14}, {"n", 1.0}, {"rs", 0.0}, {"bv", 0.0}, {"ibv", 1e-3}, {"nbv", 0.0}}},
        {"npn", {{"is", 1e-14}, {"bf", 100.0}, {"br", 1.0}, {"ise", 0.0}, {"ne", 1.5}, {"isc", 0.0}, {"nc", 2.0}, {"vaf", 0.0}, {"cje", 0.0}, {"cjc", 0.0}, {"rb", 0.0}, {"icbo", 0.0}, {"vleak", 0.2}}},
        {"pnp", {{"is", 1e-14}, {"bf", 100.0}, {"br", 1.0}, {"ise", 0.0}, {"ne", 1.5}, {"isc", 0.0}, {"nc", 2.0}, {"vaf", 0.0}, {"cje", 0.0}, {"cjc", 0.0}, {"rb", 0.0}, {"icbo", 0.0}, {"vleak", 0.2}}},
        {"njf", {{"vto", -2.0}, {"beta", 1e-4}, {"lambda", 0.0}, {"is", 1e-14}, {"n", 1.0}, {"cgs", 0.0}, {"cgd", 0.0}, {"ksub", 0.0}}},
        {"pjf", {{"vto", -2.0}, {"beta", 1e-4}, {"lambda", 0.0}, {"is", 1e-14}, {"n", 1.0}, {"cgs", 0.0}, {"cgd", 0.0}, {"ksub", 0.0}}},
        {"ota", {{"is", 5.9e-16}, {"n", 1.0}, {"junctions", 1}, {"usat_hi", 1.5}, {"usat_lo", 0.6}, {"w", 0.1}, {"bf", 125}, {"va", 0}, {"mirror", 1.0}}},
        {"tape", {{"d1", 0.069}, {"d2", 0.131}, {"d3", 0.189}, {"range", 2.57}, {"ms", 3.5e5}, {"a", 22e3}, {"alpha", 1.6e-3}, {"drive", 4e8}, {"gain", 5e-7}, {"v0", 0.19},
                  {"gap", 5}, {"spacing", 3}, {"thickness", 10}, {"wow", 1e-3}, {"flutter", 5e-4}, {"servo", 0.25}, {"rrec", 100}, {"lrec", 10e-3}, {"rpb", 300}, {"lpb", 0.1}}},
        {"bbd", {{"stages", 1024}, {"eps", 0.1}, {"a2", 0.0}, {"vlo", -1e9}, {"vhi", 1e9}, {"vshift", 0}, {"noise", 0}, {"rout", 2e3}, {"cin", 0}}},
        {"compander", {{"r1", 10e3}, {"r2", 20e3}, {"r5", 10e3}, {"i1", 140e-6}, {"delta", 1e-3}}},
        {"cmos", {{"beta", 1.215e-3}, {"vt", 1.659}, {"theta", 0.2016}, {"lambda", 0.01527}, {"kappa", 0.05}, {"betan", 1.215e-3}, {"betap", 1.215e-3}, {"vth", 0.5},
                  {"tpd5", 125e-9}, {"tpd10", 60e-9}, {"tpd15", 45e-9}, {"cin", 5e-12}, {"iddq", 5e-9}, {"ub", 0}, {"blep", 1}, {"q0", 0}}},
        {"vca", {{"mvdb", 6.1}, {"iq", 20e-6}, {"vsym", 0.0}, {"rs", 0.0}, {"rd", 0.0}, {"rsym", 25}, {"rc", 0}, {"icc", 0}, {"vczero", 0}, {"v5", 0}, {"r5", 20}, {"bf", 0}, {"isc", 0}}},
        {"ad536", {{"r1", 25e3}, {"r2", 25e3}, {"r4", 50e3}, {"rin", 16.67e3}, {"mvdb", 3.0}, {"iref", 20e-6}, {"vfloor", 1e-3}, {"i4max", 0}, {"icc", 0}}},
        {"rms", {{"mvdb", 6.1}, {"kgeo", 2.9}, {"nb", 3}, {"vbias", 2.1}, {"ibref", 24e-6}, {"vsym", 0.0}, {"iflr", 16e-9}, {"vofs", 1.4}, {"icc", 0}, {"vtj", 0}}},
        {"cds", {{"rdark", 10e6}, {"ron", 1e3}, {"gamma", 1.0}, {"kr", 200}, {"tau_l", 2.0}, {"tau_h", 0}, {"mh", 0}, {"mr", 0}}},
        {"led", {{"is", 1e-19}, {"n", 1.6}, {"rs", 5.0}, {"iref", 0.01}, {"tau_light", 1e-4}}},
        {"lamp", {{"vrated", 28}, {"irated", 0.04}, {"rcold_ratio", 1.0 / 13.5}, {"tau", 0.02}, {"tlambda", 20250}, {"thot", 2500}, {"alpha", 1.214}, {"tau_light", 1e-4}}},
        {"elpanel", {{"cp", 3e-9}, {"rs", 1e4}, {"b", 20.0}, {"veps", 0.5}, {"vref", 60.0}, {"fref", 1000.0}, {"tau_light", 1e-3}}},
        {"neon", {{"vstrike", 65}, {"rlamp", 30e3}, {"smooth", 2.0}, {"iref", 1e-3}, {"tau_light", 1e-4}}},
        {"opamp", {{"level", 1}, {"a0", 1e5}, {"gbw", 1e6}, {"slew", 1e6}, {"swing_hi", 1.5}, {"swing_lo", 1.5}, {"vhi", 13.5}, {"vlo", -13.5}, {"cdom", 30e-12}, {"be", 0}, {"rout", 100.0}, {"iq", 1e-3}, {"ilim", 0}, {"vos", 0}, {"ib", 0}, {"ios", 0}}},
        {"pentode", {{"mu", 8.7}, {"ex", 1.35}, {"kg1", 1460}, {"kg2", 12000}, {"kp", 48}, {"kvb", 12}, {"cg1k", 0}, {"cg1p", 0}, {"cpk", 0}, {"gscale", 1.5e-3}, {"gknee", 0.08}}},
        {"vacdiode", {{"k", 3.6e-3}, {"knee", 0.5}}},
        {"springtank", {{"rin", 8.0}, {"lin", 1.0e-3}, {"rout", 2575.0}, {"delay1", 0.0334}, {"delay2", 0.0392}, {"delay3", 0.0}, {"t60", 2.9}, {"disp", 0.62}, {"stages", 36}, {"lp", 4300}, {"hp", 90}, {"gain", 0.08}}},
        {"ldr", {{"rdark", 10e6}, {"ron", 8e3}, {"gamma", 0.9}, {"vstrike", 65}, {"rlamp", 30e3}, {"smooth", 2.0}, {"drive", 150.0}, {"release", 0.08}}},
        {"speaker", {{"re", 6.0}, {"le", 2e-4}, {"l2", 4e-4}, {"r2", 18.0}, {"bl", 10.0}, {"mms", 0.03}, {"kms", 6000.0}, {"rms", 1.5}, {"sd", 0.05},
                     {"b1", 0}, {"b2", 0}, {"b3", 0}, {"b4", 0}, {"k1", 0}, {"k2", 0}, {"k4", 0}, {"lx1", 0}, {"lx2", 0},
                     {"alpha", 0.00393}, {"rtv", 0}, {"ctv", 1.0}, {"rtm", 0}, {"ctm", 100.0}}},
        {"triode", {{"mu", 100}, {"ex", 1.4}, {"kg1", 1060}, {"kp", 600}, {"kvb", 300}, {"cgk", 0}, {"cgp", 0}, {"cpk", 0}, {"gscale", 6.2e-4}, {"gknee", 0.06}, {"lut", 1}, {"vgo", 0}}},
    };
    static const std::map<std::string, double> none;
    auto it = d.find(kind);
    return it == d.end() ? none : it->second;
}

// Electro-mechanical loudspeaker ("speaker" part, docs §10.21): one model card per driver, `series` x `parallel` identical
// drivers wired as one cab load. The wiring folds into an equivalent single driver (exact for identical drivers): electrical
// R, L x s/p, Bl x s, mechanical Mms, Kms, Rms x s·p, the same cone velocity and excursion. The engine and the SPICE export
// both build from this, so they solve the same equations.
//   Bl(x) = Bl0·(1 + b1 x + b2 x² + b3 x³ + b4 x⁴),  Kms(x) = Kms0·(1 + k1 x + k2 x² + k4 x⁴),  Le(x) = Le0·(1 + lx1 x + lx2 x²)
// with x in mm (positive = cone outward for positive current into pin 1). Bl and Le are evaluated at x clamped to
// [xlo, xhi], the stationary points of the Bl polynomial (its minimum on each side; Bl is then C¹ there), or where Bl falls
// to 20 % of Bl0 if the polynomial has no minimum first. Kms is not clamped (it stiffens without bound, as a suspension does).
struct SpeakerDerived {
    double re = 0, le = 0, l2 = 0, r2 = 0, bl = 0, mms = 0, kms = 0, rms = 0, sd = 0;
    double b[4] = {}, k1 = 0, k2 = 0, k4 = 0, lx1 = 0, lx2 = 0, xlo = -1e3, xhi = 1e3;
    double alpha = 0, rtv = 0, ctv = 1, rtm = 0, ctm = 1;
    int coils = 1;     // series · parallel drivers sharing the heat
    bool thermal = false;
};
inline double speakerBlRatio(const double* b, double x) noexcept { return 1.0 + x * (b[0] + x * (b[1] + x * (b[2] + x * b[3]))); }
inline double speakerBlSlope(const double* b, double x) noexcept { return b[0] + x * (2.0 * b[1] + x * (3.0 * b[2] + x * 4.0 * b[3])); }
// The clamp on one side (dir = +1 or -1): walk outward in 0.01 mm steps to the first local minimum of Bl past where it starts
// falling (bisected), or to the 20 % floor; 60 mm at most.
inline double speakerClamp(const double* b, double dir) noexcept {
    const double h = 0.01;
    double x = 0.0;
    bool fell = false; // Bl may first rise outward (an offset coil); the clamp is the minimum past the maximum
    for (int i = 0; i < 6000; ++i) {
        const double xn = x + dir * h;
        if (dir * speakerBlSlope(b, xn) < 0.0) fell = true;
        else if (fell) { // Bl stopped falling outward between x and xn
            double lo = x, hi = xn;
            for (int k = 0; k < 60; ++k) {
                const double mid = 0.5 * (lo + hi);
                (dir * speakerBlSlope(b, mid) >= 0.0 ? hi : lo) = mid;
            }
            return 0.5 * (lo + hi);
        }
        if (speakerBlRatio(b, xn) <= 0.2) return xn;
        x = xn;
    }
    return x;
}
inline SpeakerDerived speakerDerived(const DeviceModel& m, int series, int parallel) {
    SpeakerDerived d;
    const double s = std::max(series, 1), p = std::max(parallel, 1), e = s / p, mech = s * p;
    d.re = m.get("re", 6.0) * e;
    d.le = m.get("le", 2e-4) * e;
    d.l2 = m.get("l2", 0.0) * e;
    d.r2 = m.get("r2", 0.0) * e;
    d.bl = m.get("bl", 10.0) * s;
    d.mms = m.get("mms", 0.03) * mech;
    d.kms = m.get("kms", 6000.0) * mech;
    d.rms = m.get("rms", 1.5) * mech;
    d.sd = m.get("sd", 0.05) * mech;
    const char* bk[4] = {"b1", "b2", "b3", "b4"};
    for (int i = 0; i < 4; ++i) d.b[i] = m.get(bk[i], 0.0);
    d.k1 = m.get("k1", 0.0);
    d.k2 = m.get("k2", 0.0);
    d.k4 = m.get("k4", 0.0);
    d.lx1 = m.get("lx1", 0.0);
    d.lx2 = m.get("lx2", 0.0);
    d.xlo = speakerClamp(d.b, -1.0);
    d.xhi = speakerClamp(d.b, 1.0);
    d.alpha = m.get("alpha", 0.00393);
    d.rtv = m.get("rtv", 0.0);
    d.ctv = std::max(m.get("ctv", 1.0), 1e-9);
    d.rtm = m.get("rtm", 0.0);
    d.ctm = std::max(m.get("ctm", 100.0), 1e-9);
    d.coils = static_cast<int>(mech);
    d.thermal = d.rtv > 0 && d.alpha != 0;
    return d;
}


// Op-amp input noise (CompileOptions::noise, docs §3.9): white en (V/√Hz) and in (A/√Hz) at 1 kHz, and their 1/f
// corners fce / fci (Hz; e² = en²·(1 + fce/f)). A model card's "en", "in", "fce", "fci" override these per-type
// defaults, which match the part name (model string) by prefix. (S) datasheet typical, (D) derived from two datasheet
// points, (A) assumed: the class value where the datasheet gives none.
struct OpampNoise {
    double en, fce, in, fci;
};
inline OpampNoise opampNoiseDefaults(const std::string& model) {
    struct Row {
        const char* prefix;
        OpampNoise n;
    };
    static const Row rows[] = {
        // TI TL07x (SLOS080): 18 nV/√Hz, 0.01 pA/√Hz at 1 kHz (S); corner 100 Hz (A, JFET input class)
        {"TL07", {18e-9, 100.0, 0.01e-12, 0.0}},
        // TI TL06x low-power BiFET: 42 nV/√Hz at 1 kHz (S); in as TL07x (A)
        {"TL06", {42e-9, 100.0, 0.01e-12, 0.0}},
        // TI LF353: 16 nV/√Hz, 0.01 pA/√Hz at 1 kHz (S); corner (A)
        {"LF353", {16e-9, 100.0, 0.01e-12, 0.0}},
        // TI NE5534A: 3.5 nV/√Hz and 0.4 pA/√Hz at 1 kHz (S); 5.5 nV and 1.5 pA at 30 Hz give fce 45 Hz, fci 450 Hz (D)
        {"NE5534", {3.5e-9, 45.0, 0.4e-12, 450.0}},
        // TI NE5532: 5 nV/√Hz, 0.7 pA/√Hz at 1 kHz (S); corners as NE5534 (A)
        {"NE5532", {5e-9, 45.0, 0.7e-12, 450.0}},
        // TI RC4558: 8 nV/√Hz at 1 kHz (S; 7 in the latest revision), 0.15 pA/√Hz (S, latest revision); corners (A)
        {"JRC4558", {8e-9, 50.0, 0.15e-12, 300.0}},
        {"NJM4558", {8e-9, 50.0, 0.15e-12, 300.0}},
        {"RC4558", {8e-9, 50.0, 0.15e-12, 300.0}},
        {"TA75558", {8e-9, 50.0, 0.15e-12, 300.0}}, // 4558-class (A)
        {"M5218", {8e-9, 50.0, 0.15e-12, 300.0}},   // 4558-class low-noise dual (A)
        // TI LM324 / LM2904: 35 nV/√Hz at 1 kHz (S; ST 40); in, corners (A)
        {"LM324", {35e-9, 100.0, 0.1e-12, 500.0}},
        {"LM2904", {35e-9, 100.0, 0.1e-12, 500.0}},
        {"NJM2904", {35e-9, 100.0, 0.1e-12, 500.0}},
        {"LM358", {35e-9, 100.0, 0.1e-12, 500.0}},
        // LM308 (super-beta input, Ib ~1.5 nA): 741-class en (A), in from shot noise of Ib (A)
        {"LM308", {20e-9, 200.0, 0.03e-12, 500.0}},
    };
    for (const auto& r : rows)
        if (model.compare(0, std::char_traits<char>::length(r.prefix), r.prefix) == 0) return r.n;
    // 741 / 1458 / unknown bipolar: 20 nV/√Hz, 0.5 pA/√Hz, corners 200 Hz / 2 kHz (A, general-purpose bipolar class)
    return {20e-9, 200.0, 0.5e-12, 2000.0};
}

} // namespace rig::ce
