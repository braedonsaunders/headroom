cmake_minimum_required(VERSION 3.22)
project(OpenRigDSP VERSION 0.1.0 LANGUAGES CXX)
option(RIG_RELEASE_BUNDLE "Relocatable release build: the app never prefers the live source tree" OFF)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toolchain-neutral flags (docs/PORTING.md). GCC/Clang keep the exact flags this project always used; MSVC
# gets the equivalents (its Release config already optimises with /O2).
if(MSVC)
    set(RIG_WARN /W3)
    set(RIG_PEDANTIC "")
    set(RIG_O1 "")
    set(RIG_O2 "")
    set(RIG_O3 "")
    set(RIG_NOWARN /w)
    add_compile_options(/utf-8 /bigobj /Zc:__cplusplus /permissive-)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    set(RIG_WARN -Wall -Wextra)
    set(RIG_PEDANTIC -Wpedantic)
    set(RIG_O1 -O1)
    set(RIG_O2 -O2)
    set(RIG_O3 -O3)
    set(RIG_NOWARN -w)
endif()

# Profile-guided optimisation and ThinLTO for every target (HEADROOM_PGO, HEADROOM_LTO; docs/CIRCUIT_ENGINE.md §10.54).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeadroomOptimise.cmake)

# Neural Amp Modeler core (MIT) — loads community .nam amp/pedal captures.
set(NAM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SRC_TOP ${NAM_ROOT}/NAM/*.cpp)
file(GLOB NAM_SRC_SUB ${NAM_ROOT}/NAM/*/*.cpp)
add_library(nam_core STATIC ${NAM_SRC_TOP} ${NAM_SRC_SUB})
target_include_directories(nam_core SYSTEM PUBLIC ${NAM_ROOT} ${NAM_ROOT}/NAM ${NAM_ROOT}/Dependencies/eigen ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)
target_compile_options(nam_core PRIVATE ${RIG_O3} ${RIG_NOWARN})

add_library(rig_engine STATIC
    src/dsp/ComponentFactory.cpp
    src/dsp/CabinetUnit.cpp
    src/dsp/LooperUnit.cpp
    src/dsp/LoopStation.cpp
    src/dsp/PitchDoublerUnits.cpp
    src/dsp/ToneMatchAndCalibration.cpp
    src/dsp/TubeAmpComponents.cpp
    src/dsp/ComponentModel.cpp
    src/circuit/Netlist.cpp
    src/circuit/Engine.cpp
    src/circuit/Wdf.cpp
    src/circuit/CodegenStub.cpp
    src/circuit/CircuitUnit.cpp
    src/library/Library.cpp
    src/library/Showcase.cpp
    src/midi/MidiComponents.cpp
    src/dsp/NamComponent.cpp
    src/plugins/PluginHost.cpp
    src/preset/PresetLoader.cpp
    src/audio/BackingTrackPlayer.cpp
    src/audio/BackingTracks.cpp
    src/platform/Paths.cpp
    src/platform/AudioDecoder.cpp
    src/platform/TimeStretch.cpp
)
# Plugin hosting: Audio Units on macOS. Elsewhere a placeholder host keeps presets that reference plugins
# loadable (pass-through units that preserve plugin state) until VST3 hosting lands (docs/PORTING.md).
# PluginHost.cpp stays in the list above because app/plugin/CMakeLists.txt reads that list.
if(NOT APPLE)
    set_source_files_properties(src/plugins/PluginHost.cpp PROPERTIES HEADER_FILE_ONLY ON)
    target_sources(rig_engine PRIVATE src/plugins/PluginHostNull.cpp)
endif()

target_include_directories(rig_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rig_engine PUBLIC nam_core)
# NAM registers its architectures (WaveNet, LSTM, …) via static initialisers: keep every object.
if(APPLE)
    target_link_options(rig_engine INTERFACE "-Wl,-force_load,$<TARGET_FILE:nam_core>")
elseif(MSVC)
    target_link_options(rig_engine INTERFACE "/WHOLEARCHIVE:$<TARGET_FILE:nam_core>")
else()
    target_link_options(rig_engine INTERFACE "LINKER:--whole-archive" "$<TARGET_FILE:nam_core>" "LINKER:--no-whole-archive")
endif()
if(NOT APPLE AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(rig_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
# Off macOS: miniaudio (public domain / MIT-0) decodes backing tracks (src/platform/AudioDecoder.cpp) and is the
# device-I/O layer. Its implementation is compiled exactly once, in this `miniaudio` target, which rig_engine
# links publicly: other targets (AudioIO, the app shell) should link rig_engine or `miniaudio`, never compile
# third_party/miniaudio/miniaudio.c again. Signalsmith Stretch (MIT, header-only) does speed / transpose.
if(NOT APPLE)
    enable_language(C)
    add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)
    target_include_directories(miniaudio SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
    target_compile_options(miniaudio PRIVATE ${RIG_O2} ${RIG_NOWARN})
    if(NOT WIN32)
        target_link_libraries(miniaudio PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
    endif()
    target_link_libraries(rig_engine PUBLIC miniaudio)
    target_include_directories(rig_engine SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include)
endif()
target_compile_definitions(rig_engine PRIVATE RIG_NAM_EXAMPLES_DIR="${NAM_ROOT}/example_models")

if(APPLE)
    target_link_libraries(rig_engine PUBLIC "-framework Accelerate" "-framework AudioToolbox" "-framework CoreFoundation")
endif()

target_compile_options(rig_engine PRIVATE
    ${RIG_WARN} ${RIG_PEDANTIC} ${RIG_O3}
)

add_executable(rig_cli
    src/cli/main.cpp
)
target_link_libraries(rig_cli PRIVATE rig_engine)
target_compile_options(rig_cli PRIVATE ${RIG_WARN} ${RIG_O3})

if(APPLE)
    set(HEADROOM_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Headroom-Info.plist)
    file(WRITE ${HEADROOM_PLIST} "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleName</key><string>Headroom</string>
<key>CFBundleDisplayName</key><string>Headroom</string>
<key>CFBundleIdentifier</key><string>com.headroom.studio</string>
<key>CFBundleExecutable</key><string>Headroom</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.9</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSMicrophoneUsageDescription</key><string>Headroom processes the guitar or bass plugged into your audio interface.</string>
<key>CFBundleIconFile</key><string>Headroom</string>
</dict></plist>
")
    set(HEADROOM_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/Headroom.icns)
    set_source_files_properties(${HEADROOM_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(rig_studio MACOSX_BUNDLE
        src/gui/StudioApp.mm
        src/audio/AudioIO.mm
        src/midi/MidiOut.mm
        src/plugins/PluginEditor.mm
        ${HEADROOM_ICON}
    )
    set_target_properties(rig_studio PROPERTIES
        OUTPUT_NAME Headroom
        MACOSX_BUNDLE_INFO_PLIST ${HEADROOM_PLIST}
    )
    target_compile_definitions(rig_studio PRIVATE RIG_APP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(rig_studio PRIVATE
        rig_engine
        "-framework Cocoa"
        "-framework WebKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework AVFoundation"
        "-framework CoreMIDI"
        "-framework CoreAudioKit"
        "-framework Security"
        "-framework UniformTypeIdentifiers"
    )
    target_compile_options(rig_studio PRIVATE -fobjc-arc -O2)
    # Sign with a stable identity when one is available so macOS privacy grants (microphone, Documents
    # access) survive rebuilds; ad-hoc signatures change every build and make macOS ask again.
    if(NOT DEFINED HEADROOM_SIGN_IDENTITY)
        execute_process(COMMAND security find-identity -v -p codesigning
                        OUTPUT_VARIABLE _ids ERROR_QUIET)
        string(REGEX MATCH "[0-9A-F]+ \"Apple Development[^\"]*\"" _dev "${_ids}")
        string(REGEX MATCH "^[0-9A-F]+" _hash "${_dev}")
        if(_hash)
            set(HEADROOM_SIGN_IDENTITY "${_hash}" CACHE STRING "codesign identity for Headroom.app")
        else()
            set(HEADROOM_SIGN_IDENTITY "-" CACHE STRING "codesign identity for Headroom.app")
        endif()
    endif()
    add_custom_command(TARGET rig_studio POST_BUILD
        COMMAND codesign --force --sign ${HEADROOM_SIGN_IDENTITY} --deep $<TARGET_BUNDLE_DIR:rig_studio> || codesign --force --sign - --deep $<TARGET_BUNDLE_DIR:rig_studio>)
endif()

# Windows / Linux desktop app: the same engine and app/ui in a native web view (src/gui/shell, webview/webview
# MIT: WebKitGTK 4.1 on Linux, WebView2 on Windows), miniaudio device I/O, ALSA-sequencer MIDI out, libsecret /
# Credential Manager for keys. Off by default when the GUI dependencies are missing, so headless engine/CI builds
# need nothing beyond a compiler.
if(NOT APPLE)
    set(_rig_app_default OFF)
    if(WIN32)
        # WebView2 SDK headers (NuGet Microsoft.Web.WebView2, build/native/include): -DRIG_WEBVIEW2_SDK=<package dir>.
        find_path(RIG_WEBVIEW2_INCLUDE WebView2.h
                  HINTS ${RIG_WEBVIEW2_SDK}/build/native/include $ENV{RIG_WEBVIEW2_SDK}/build/native/include)
        if(RIG_WEBVIEW2_INCLUDE)
            set(_rig_app_default ON)
        endif()
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(RIG_GTK QUIET IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
            pkg_check_modules(RIG_ALSA QUIET IMPORTED_TARGET alsa)
            pkg_check_modules(RIG_SECRET QUIET IMPORTED_TARGET libsecret-1)
        endif()
        if(RIG_GTK_FOUND)
            set(_rig_app_default ON)
        endif()
    endif()
    option(RIG_BUILD_APP "Build the Headroom desktop app (needs WebKitGTK 4.1 on Linux, the WebView2 SDK on Windows)" ${_rig_app_default})
    if(WIN32)
        option(RIG_WITH_ASIO "Build the Windows standalone with Steinberg ASIO driver support" ${RIG_RELEASE_BUNDLE})
        set(RIG_ASIO_SDK_DIR "$ENV{RIG_ASIO_SDK_DIR}" CACHE PATH "Steinberg ASIO SDK root containing common/asio.h")
        if(RIG_RELEASE_BUNDLE AND (NOT RIG_BUILD_APP OR NOT RIG_WITH_ASIO))
            message(FATAL_ERROR "The Windows desktop release requires the WebView2 app target and ASIO. Set RIG_BUILD_APP=ON, RIG_WITH_ASIO=ON and provide both SDKs.")
        endif()
    endif()
    if(RIG_BUILD_APP)
        add_executable(rig_studio
            src/gui/shell/ShellMain.cpp
            src/gui/shell/Bridge.cpp
            $<IF:$<BOOL:${WIN32}>,src/gui/shell/ShellWin.cpp,src/gui/shell/ShellLinux.cpp>
            src/audio/AudioIOMiniaudio.cpp
            src/midi/MidiOutPortable.cpp
            src/plugins/PluginEditorNull.cpp
            src/platform/Secrets.cpp
        )
        set_target_properties(rig_studio PROPERTIES OUTPUT_NAME Headroom)
        target_include_directories(rig_studio SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview)
        target_link_libraries(rig_studio PRIVATE rig_engine)
        target_compile_options(rig_studio PRIVATE ${RIG_O2})
        # Dev builds prefer the live source tree's app/ui (as the Mac app does); packages ship their own copy.
        option(RIG_DEV_SOURCE_UI "Let the app load app/ui from this source tree when it exists" ON)
        if(RIG_DEV_SOURCE_UI)
            target_compile_definitions(rig_studio PRIVATE RIG_APP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        if(WIN32)
            if(RIG_WITH_ASIO)
                if(NOT EXISTS "${RIG_ASIO_SDK_DIR}/common/asio.h" OR NOT EXISTS "${RIG_ASIO_SDK_DIR}/common/iasiodrv.h")
                    message(FATAL_ERROR "Set RIG_ASIO_SDK_DIR to the extracted Steinberg ASIO SDK directory containing common/asio.h and common/iasiodrv.h.")
                endif()
                target_sources(rig_studio PRIVATE src/audio/AsioDevice.cpp)
                target_include_directories(rig_studio SYSTEM PRIVATE "${RIG_ASIO_SDK_DIR}")
                target_compile_definitions(rig_studio PRIVATE RIG_WITH_ASIO=1)
            endif()
            target_include_directories(rig_studio SYSTEM PRIVATE ${RIG_WEBVIEW2_INCLUDE})
            set_target_properties(rig_studio PROPERTIES WIN32_EXECUTABLE TRUE)
            target_link_options(rig_studio PRIVATE /ENTRY:mainCRTStartup)
            target_link_libraries(rig_studio PRIVATE advapi32 ole32 shell32 shlwapi user32 version)
        else()
            target_link_libraries(rig_studio PRIVATE PkgConfig::RIG_GTK)
            if(RIG_ALSA_FOUND)
                target_compile_definitions(rig_studio PRIVATE RIG_HAVE_ALSA_SEQ=1)
                target_link_libraries(rig_studio PRIVATE PkgConfig::RIG_ALSA)
            endif()
            if(RIG_SECRET_FOUND)
                target_compile_definitions(rig_studio PRIVATE RIG_HAVE_LIBSECRET=1)
                target_link_libraries(rig_studio PRIVATE PkgConfig::RIG_SECRET)
            endif()
        endif()
    endif()
endif()

enable_testing()
if(APPLE)
    set(RIG_PLUGIN_TESTS tests/test_plugins.cpp)        # hosted Audio Units
else()
    set(RIG_PLUGIN_TESTS tests/test_plugins_null.cpp)   # placeholder host: presets with plugins still load
endif()
add_executable(rig_tests
    tests/test_main.cpp
    ${RIG_PLUGIN_TESTS}
)
target_link_libraries(rig_tests PRIVATE rig_engine)
target_compile_options(rig_tests PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME rig_unit_and_factory_tests COMMAND rig_tests)

# Small deterministic ownership/utility regressions, independent of audio hardware.
foreach(check graph_exchange graph_runtime rack_snapshots amp_utilities)
    add_executable(test_${check} tests/test_${check}.cpp)
    target_compile_options(test_${check} PRIVATE ${RIG_WARN} ${RIG_O1})
    add_test(NAME ${check} COMMAND test_${check})
endforeach()
# rack_snapshots instantiates units through the factory (GCC emits the inline factory call; clang drops it).
target_link_libraries(test_rack_snapshots PRIVATE rig_engine)
add_executable(test_midi_handoffs tests/test_midi_handoffs.cpp)
target_link_libraries(test_midi_handoffs PRIVATE rig_engine)
target_compile_options(test_midi_handoffs PRIVATE ${RIG_WARN} ${RIG_O1})
add_test(NAME midi_handoffs COMMAND test_midi_handoffs)

# Bloom (900006): harmony, key, groove, bend PLLs, string capture and the trigger logic, on synthetic guitar.
add_executable(test_bloom tests/test_bloom.cpp)
target_link_libraries(test_bloom PRIVATE rig_engine)
target_compile_options(test_bloom PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME bloom COMMAND test_bloom)
add_executable(bloom_probe tests/bloom_probe.cpp)  # a real DI through Pitch → MIDI → Bloom, with a timeline (not a test)
target_link_libraries(bloom_probe PRIVATE rig_engine)
target_compile_options(bloom_probe PRIVATE ${RIG_O2})

add_executable(test_effect_handoffs tests/test_effect_handoffs.cpp)
target_link_libraries(test_effect_handoffs PRIVATE rig_engine)
target_compile_options(test_effect_handoffs PRIVATE ${RIG_WARN} ${RIG_O1})
add_test(NAME effect_handoffs COMMAND test_effect_handoffs)

# Cabinet unit (cab / mic / position content, IR handoff) and rig tempo (sync, persistence, RT safety).
add_executable(test_cabinet_tempo tests/test_cabinet_tempo.cpp)
target_link_libraries(test_cabinet_tempo PRIVATE rig_engine)
target_compile_options(test_cabinet_tempo PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME cabinet_tempo COMMAND test_cabinet_tempo)

# SSL 4000 E/G channel EQ: per-band response and the illustrated face's curve (index.json).
add_executable(test_studio_eq tests/test_studio_eq.cpp)
target_link_libraries(test_studio_eq PRIVATE rig_engine)
target_compile_options(test_studio_eq PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME studio_eq COMMAND test_studio_eq)

# Amp-Matched Cab (156000) volume range / A-B / room, and bass-register loudness calibration.
add_executable(test_level_staging tests/test_level_staging.cpp)
target_link_libraries(test_level_staging PRIVATE rig_engine)
target_compile_options(test_level_staging PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME level_staging COMMAND test_level_staging)

# Level folding (core/LevelFold.hpp): the level knobs' dB maps against the DSP, the knob search, the .rig edit.
add_executable(test_level_fold tests/test_level_fold.cpp)
target_link_libraries(test_level_fold PRIVATE rig_engine)
target_compile_options(test_level_fold PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME level_fold COMMAND test_level_fold)

add_executable(test_circuit_integration tests/test_circuit_integration.cpp)
target_link_libraries(test_circuit_integration PRIVATE rig_engine)
target_compile_options(test_circuit_integration PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME circuit_integration COMMAND test_circuit_integration)

# Manual deadline measurement: correctness and measured budget are reported separately.
add_executable(test_circuit_budget tests/test_circuit_budget.cpp)
target_link_libraries(test_circuit_budget PRIVATE rig_engine)
target_compile_options(test_circuit_budget PRIVATE ${RIG_WARN} ${RIG_O2})

# NAM Capture wrapper: rate bridge aliasing/latency/continuity, post-model gate, dBu calibration.
add_executable(test_nam_bridge tests/test_nam_bridge.cpp)
target_link_libraries(test_nam_bridge PRIVATE rig_engine)
target_compile_options(test_nam_bridge PRIVATE ${RIG_WARN} ${RIG_O2})
target_compile_definitions(test_nam_bridge PRIVATE RIG_NAM_EXAMPLES="${NAM_ROOT}/example_models")
add_test(NAME nam_bridge COMMAND test_nam_bridge)

# DSP quality: pedal dry/wet alignment, triode grid coupling, amp↔speaker coupling, partitioned convolver.
add_executable(test_dsp_quality tests/test_dsp_quality.cpp)
target_link_libraries(test_dsp_quality PRIVATE rig_engine)
target_compile_options(test_dsp_quality PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME dsp_quality COMMAND test_dsp_quality)

# Neural guitar → MIDI decoder: onset confirmation + octave/fifth ghost suppression on synthetic probabilities.
add_executable(test_guitar_midi_decoder tests/test_guitar_midi_decoder.cpp)
target_link_libraries(test_guitar_midi_decoder PRIVATE rig_engine)
target_compile_options(test_guitar_midi_decoder PRIVATE ${RIG_WARN} ${RIG_O1})
target_compile_definitions(test_guitar_midi_decoder PRIVATE RIG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME guitar_midi_decoder COMMAND test_guitar_midi_decoder)
set_tests_properties(guitar_midi_decoder PROPERTIES SKIP_RETURN_CODE 77)

# Practice backing tracks: decode (ExtAudioFile on macOS, miniaudio elsewhere), seek, loop A–B, mix level, time-pitch, library, and no heap
# use on the audio thread (malloc/free counted through an interposing dylib loaded only into this test).
add_executable(test_backing_track tests/test_backing_track.cpp)
target_link_libraries(test_backing_track PRIVATE rig_engine)
target_compile_options(test_backing_track PRIVATE ${RIG_WARN} ${RIG_O2})
target_compile_definitions(test_backing_track PRIVATE RIG_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(NOT WIN32) # no malloc interposition on Windows: the test skips its heap check there
    add_library(rtguard SHARED tests/rtguard.cpp)
    target_compile_options(rtguard PRIVATE ${RIG_O2})
    target_link_libraries(rtguard PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_backing_track rtguard)
endif()
add_test(NAME backing_track COMMAND test_backing_track)
if(APPLE)
    set_tests_properties(backing_track PROPERTIES ENVIRONMENT "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:rtguard>")
elseif(NOT WIN32)
    set_tests_properties(backing_track PROPERTIES ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:rtguard>")
endif()

# Global multitrack looper (LoopStation): record/overdub/undo/redo, tempo/loop sync, speed/reverse, stop modes,
# export, and no allocation on the audio path.
add_executable(test_loop_station tests/test_loop_station.cpp)
target_link_libraries(test_loop_station PRIVATE rig_engine)
target_compile_options(test_loop_station PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME loop_station COMMAND test_loop_station)

# Per-unit face meters (levels, GR, LFO phase, spectrum, pitch) for the illustrated hardware faces; RT-safe.
add_executable(test_unit_meters tests/test_unit_meters.cpp)
target_link_libraries(test_unit_meters PRIVATE rig_engine)
target_compile_options(test_unit_meters PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME unit_meters COMMAND test_unit_meters)

# Portable vDSP subset (src/platform/VDsp.hpp): layout/scaling vs a reference DFT everywhere, vs Accelerate on macOS.
add_executable(test_vdsp_portable tests/test_vdsp_portable.cpp)
target_compile_options(test_vdsp_portable PRIVATE ${RIG_WARN} ${RIG_O2})
if(APPLE)
    target_link_libraries(test_vdsp_portable PRIVATE "-framework Accelerate")
endif()
add_test(NAME vdsp_portable COMMAND test_vdsp_portable)

# Unit fidelity audit: per-unit DSP class + per-parameter min/mid/max renders.
# Circuit engine (docs/CIRCUIT_ENGINE.md): developer CLI + SPICE-validated tests.
# The solver core has no catalog/engine dependency; tools link it alone (its sources are also part of rig_engine).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeadroomCircuitCodegen.cmake)
add_library(rig_circuit_core STATIC $<TARGET_OBJECTS:rig_circuit_obj> $<TARGET_OBJECTS:rig_circuit_kernels>)
target_include_directories(rig_circuit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_options(rig_circuit_core PRIVATE ${RIG_WARN} ${RIG_PEDANTIC} ${RIG_O3})
add_executable(circuit_cli tools/circuit/circuit_cli.cpp)
target_link_libraries(circuit_cli PRIVATE rig_circuit_core)
target_include_directories(circuit_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/circuit)
target_compile_options(circuit_cli PRIVATE ${RIG_WARN} ${RIG_O3})
# circuit_ab collects macOS libproc hardware counters.
if(APPLE)
    add_executable(circuit_ab tools/circuit/circuit_ab.cpp)
    target_link_libraries(circuit_ab PRIVATE rig_engine)
    target_include_directories(circuit_ab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/circuit)
    target_compile_options(circuit_ab PRIVATE ${RIG_WARN} ${RIG_O3})
endif()
# Real-time preset timing (docs: work/circuit-netlists/PRESET_REALTIME.md): a rig on a time-constraint caller in an
# audio work interval, per-callback time against the deadline, per-unit time. macOS only (work interval API).
if(APPLE)
    add_executable(preset_rt tools/preset_rt/preset_rt.cpp)
    target_link_libraries(preset_rt PRIVATE rig_engine)
    target_compile_options(preset_rt PRIVATE ${RIG_O3})
endif()
# Offline levelling of the shipped library into the rigs' own level knobs (core/LevelFold.hpp; docs/ENGINE_API.md
# "Levels"). Run by hand when rigs or levels change; the engine itself never measures or re-levels a rig.
add_executable(rig_level tools/rig_level/rig_level.cpp)
target_link_libraries(rig_level PRIVATE rig_engine)
target_compile_options(rig_level PRIVATE ${RIG_WARN} ${RIG_O3})
add_executable(test_circuit_engine tests/test_circuit_engine.cpp)
target_link_libraries(test_circuit_engine PRIVATE rig_circuit_core)
target_compile_options(test_circuit_engine PRIVATE ${RIG_WARN} ${RIG_O2})
target_compile_definitions(test_circuit_engine PRIVATE
    HEADROOM_CIRCUIT_LIBRARY="${CMAKE_CURRENT_SOURCE_DIR}/src/circuit/library"
    HEADROOM_CIRCUIT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/circuit")
add_test(NAME circuit_engine COMMAND test_circuit_engine)
add_executable(test_circuit_noise tests/test_circuit_noise.cpp)
target_link_libraries(test_circuit_noise PRIVATE rig_engine)
add_test(NAME circuit_noise COMMAND test_circuit_noise)

add_executable(test_circuit_unit tests/test_circuit_unit.cpp)
target_link_libraries(test_circuit_unit PRIVATE rig_engine)
target_compile_options(test_circuit_unit PRIVATE ${RIG_WARN} ${RIG_O2})
target_compile_definitions(test_circuit_unit PRIVATE HEADROOM_CIRCUIT_LIBRARY="${CMAKE_CURRENT_SOURCE_DIR}/src/circuit/library")
add_test(NAME circuit_unit COMMAND test_circuit_unit)
add_executable(test_aliasing tests/test_aliasing.cpp)
target_link_libraries(test_aliasing PRIVATE rig_engine)
target_compile_options(test_aliasing PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME aliasing COMMAND test_aliasing)
add_executable(unit_audit tools/unit_audit/unit_audit.cpp)
target_link_libraries(unit_audit PRIVATE rig_engine)
target_compile_options(unit_audit PRIVATE ${RIG_O2})
add_test(NAME unit_audit_smoke COMMAND unit_audit --quick --units 25000,13000 --out unit_audit_smoke.json)
    set_tests_properties(unit_audit_smoke PROPERTIES ENVIRONMENT "HEADROOM_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/unit_audit_smoke_data") # never the real data folder
add_executable(test_unit_param_mapping tests/test_unit_param_mapping.cpp)
target_link_libraries(test_unit_param_mapping PRIVATE rig_engine)
target_compile_options(test_unit_param_mapping PRIVATE ${RIG_WARN} ${RIG_O2})
add_test(NAME unit_param_mapping COMMAND test_unit_param_mapping)

# Control drags while rendering: the real HTTP routes hammered at UI rate against a paced real-time render thread
# (hosted engine mode, ephemeral port, temp data dir). No clicks, dropouts, deadline misses, heap or locks on audio.
if(APPLE)
    get_filename_component(RIG_RESEARCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_executable(test_param_drag tests/test_param_drag.cpp src/audio/AudioIO.mm src/midi/MidiOut.mm)
    target_link_libraries(test_param_drag PRIVATE rig_engine "-framework CoreAudio" "-framework AVFoundation"
                          "-framework CoreMIDI" "-framework Foundation")
    target_compile_options(test_param_drag PRIVATE ${RIG_WARN} ${RIG_O2} $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(test_param_drag PRIVATE RIG_RESEARCH_ROOT="${RIG_RESEARCH_ROOT}")
    add_dependencies(test_param_drag rtguard)
    add_test(NAME param_drag COMMAND test_param_drag)
    set_tests_properties(param_drag PROPERTIES ENVIRONMENT "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:rtguard>" SKIP_RETURN_CODE 77
                         RUN_SERIAL TRUE)
endif()

# Rig levels through the real engine (hosted mode, ephemeral port, temp data dir): a rig plays at its own level
# with no automatic levelling, edits change only what they change, save / load, the one-time user-rig migration.
if(APPLE)
    add_executable(test_rig_levels tests/test_rig_levels.cpp src/audio/AudioIO.mm src/midi/MidiOut.mm)
    target_link_libraries(test_rig_levels PRIVATE rig_engine "-framework CoreAudio" "-framework AVFoundation"
                          "-framework CoreMIDI" "-framework Foundation")
    target_compile_options(test_rig_levels PRIVATE ${RIG_WARN} ${RIG_O2} $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(test_rig_levels PRIVATE RIG_RESEARCH_ROOT="${RIG_RESEARCH_ROOT}")
    add_test(NAME rig_levels COMMAND test_rig_levels)
    set_tests_properties(rig_levels PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endif()

# ThinLTO on the shipped binary only (HEADROOM_LTO=PRODUCTS, the default for macOS arm64 Release builds; docs
# CIRCUIT_ENGINE.md §10.54): Headroom.app links bitcode twins of the engine, the tests and tools keep the plain objects.
if(TARGET rig_studio)
    if(HEADROOM_CODEGEN)
        headroom_lto_products(TARGETS rig_studio LIBRARIES rig_circuit_kernels rig_engine)
    else()
        headroom_lto_products(TARGETS rig_studio LIBRARIES rig_engine)
    endif()
endif()

# Windows: embed resources/windows/utf8.manifest (UTF-8 active code page, long paths) in every executable of
# this directory. Deferred to the end of the file so targets added anywhere above (or later) are covered.
if(MSVC)
    function(rig_embed_windows_manifest)
        get_property(_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _targets)
            get_target_property(_type ${_t} TYPE)
            if(_type STREQUAL "EXECUTABLE")
                target_sources(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/utf8.manifest)
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER CALL rig_embed_windows_manifest)
endif()

# Tests never touch the user's real data folder: every test executable (rig_tests, test_*) links
# tests/isolated_data_dir.cpp, which points HEADROOM_DATA_DIR at a temp folder before main() unless one is set.
# Deferred, like the manifest above, so test targets added anywhere in this file are covered.
function(rig_isolate_test_data)
    get_property(_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE" AND (_t STREQUAL "rig_tests" OR _t MATCHES "^test_"))
            target_sources(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/isolated_data_dir.cpp)
        endif()
    endforeach()
endfunction()
cmake_language(DEFER CALL rig_isolate_test_data)

# ── Packaging: install layout + CPack for all three OSes (packaging/Install.cmake, docs/PORTING.md) ──
# Deferred so it sees every target regardless of where it is defined. Install rules don't change the build.
cmake_language(DEFER CALL include ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Install.cmake)
