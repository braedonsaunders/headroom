# Headroom DAW plugins: Audio Unit v2 (.component) and VST3 (.vst3), plus their offline tests.
# Separate top-level project sharing the app's engine sources and circuit codegen:
#   cmake -S app/plugin -B app/build-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice ninja -C app/build-plugin -j2            # HeadroomAU, HeadroomVST3, tests
#   ninja -C app/build-plugin install-user        # copies both into ~/Library/Audio/Plug-Ins (no sudo)
cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target")
endif()
project(HeadroomPlugins VERSION 0.9.0 LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(APP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# The app's PGO / ThinLTO switches and profile (HEADROOM_PGO, HEADROOM_LTO; docs/CIRCUIT_ENGINE.md §10.54)
include(${APP_ROOT}/cmake/HeadroomOptimise.cmake)
get_filename_component(RESEARCH_ROOT "${APP_ROOT}/.." ABSOLUTE)
set(HEADROOM_VERSION_STR "${PROJECT_VERSION}")
math(EXPR HEADROOM_AU_VERSION "(${PROJECT_VERSION_MAJOR} << 16) | (${PROJECT_VERSION_MINOR} << 8) | ${PROJECT_VERSION_PATCH}")
# Objective-C classes are process-global: suffix them per format AND version so an AU and a VST3 (or two
# versions) of Headroom loaded into one host never collide.
string(REPLACE "." "_" _ver_us "${PROJECT_VERSION}")

# ── engine (nam_core + rig_engine), mirroring the app's CMakeLists ──
# The rig_engine source list is read from app/CMakeLists.txt so it never goes stale, but the app project
# itself is not included: its standalone/test targets change independently of the plugins.
set(NAM_ROOT ${APP_ROOT}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SRC_TOP ${NAM_ROOT}/NAM/*.cpp)
file(GLOB NAM_SRC_SUB ${NAM_ROOT}/NAM/*/*.cpp)
add_library(nam_core STATIC ${NAM_SRC_TOP} ${NAM_SRC_SUB})
target_include_directories(nam_core SYSTEM PUBLIC ${NAM_ROOT} ${NAM_ROOT}/NAM ${NAM_ROOT}/Dependencies/eigen ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST)
target_compile_options(nam_core PRIVATE -O3 -w)

file(READ ${APP_ROOT}/CMakeLists.txt _app_cmake)
string(REGEX MATCH "add_library\\(rig_engine STATIC([^)]*)\\)" _rig_match "${_app_cmake}")
if("${CMAKE_MATCH_1}" STREQUAL "")
    message(FATAL_ERROR "could not read the rig_engine source list from ${APP_ROOT}/CMakeLists.txt")
endif()
string(REGEX REPLACE "[ \t\r\n]+" ";" _rig_srcs "${CMAKE_MATCH_1}")
list(FILTER _rig_srcs EXCLUDE REGEX "^$")
list(TRANSFORM _rig_srcs PREPEND "${APP_ROOT}/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${APP_ROOT}/CMakeLists.txt)
add_library(rig_engine STATIC ${_rig_srcs})
target_include_directories(rig_engine PUBLIC ${APP_ROOT}/src)
target_link_libraries(rig_engine PUBLIC nam_core "-framework Accelerate" "-framework AudioToolbox" "-framework CoreFoundation")
target_link_options(rig_engine INTERFACE "-Wl,-force_load,$<TARGET_FILE:nam_core>") # NAM registers architectures statically
target_compile_definitions(rig_engine PRIVATE RIG_NAM_EXAMPLES_DIR="${NAM_ROOT}/example_models")
target_compile_options(rig_engine PRIVATE -O3)

# Share the app's generator, registry and 16 kernel shards (including HEADROOM_CODEGEN=OFF).
set(RIG_O2 -O2)
set(RIG_O3 -O3)
include(${APP_ROOT}/cmake/HeadroomCircuitCodegen.cmake)

# ── VST3 SDK (MIT since 3.8; app/third_party/vst3sdk/LICENSE.txt) ──
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "" FORCE) # validator
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)
set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_AUV2_BUILDS OFF CACHE BOOL "" FORCE)
# The SDK insists on an Xcode version; with only the Command Line Tools installed, declare a modern one
# (it only gates feature checks; we build with Ninja + clang from the CLT).
if(NOT XCODE_VERSION)
    set(ENV{XCODE_VERSION} "16.0")
    set(XCODE_VERSION "16.0")
endif()
add_subdirectory(${APP_ROOT}/third_party/vst3sdk vst3sdk EXCLUDE_FROM_ALL)

# ── format-agnostic core (one engine per plugin instance) ──
add_library(headroom_plugin_core STATIC
    core/HeadroomPluginCore.cpp
    ${APP_ROOT}/src/audio/AudioIO.mm   # the server owns an (unused in hosted mode) AudioIO member
    ${APP_ROOT}/src/midi/MidiOut.mm    # likewise MidiOut (never started in hosted mode)
)
target_include_directories(headroom_plugin_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${APP_ROOT}/src)
target_compile_definitions(headroom_plugin_core PRIVATE HEADROOM_DEFAULT_RESEARCH_ROOT="${RESEARCH_ROOT}")
target_link_libraries(headroom_plugin_core PUBLIC rig_engine
    "-framework Cocoa" "-framework WebKit" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
    "-framework CoreAudioKit" "-framework CoreMIDI" "-framework AVFoundation" "-framework Accelerate" "-framework CoreFoundation")
target_compile_options(headroom_plugin_core PRIVATE -O3 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# codesign identity (same rule as Headroom.app: stable Apple Development identity when installed)
if(NOT DEFINED HEADROOM_SIGN_IDENTITY)
    execute_process(COMMAND security find-identity -v -p codesigning OUTPUT_VARIABLE _ids ERROR_QUIET)
    string(REGEX MATCH "[0-9A-F]+ \"Apple Development[^\"]*\"" _dev "${_ids}")
    string(REGEX MATCH "^[0-9A-F]+" _hash "${_dev}")
    if(_hash)
        set(HEADROOM_SIGN_IDENTITY "${_hash}" CACHE STRING "codesign identity for the plugin bundles")
    else()
        set(HEADROOM_SIGN_IDENTITY "-" CACHE STRING "codesign identity for the plugin bundles")
    endif()
endif()

function(headroom_bundle target ext plist bundle_id suffix)
    set_target_properties(${target} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION ${ext}
        OUTPUT_NAME Headroom
        MACOSX_BUNDLE_INFO_PLIST ${plist}
        XCODE_ATTRIBUTE_GENERATE_PKGINFO_FILE YES)
    target_compile_definitions(${target} PRIVATE
        HEADROOM_OBJC_SUFFIX=${suffix} HEADROOM_OBJC_SUFFIX_STR="${suffix}" HEADROOM_VERSION_STR="${HEADROOM_VERSION_STR}")
    target_compile_options(${target} PRIVATE -O2 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(${target} PRIVATE headroom_plugin_core)
    # The UI ships inside the bundle (Resources/ui), never loaded from the source tree.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP_ROOT}/ui $<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/ui
        COMMAND ${CMAKE_COMMAND} -E copy ${APP_ROOT}/resources/Headroom.icns $<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/Headroom.icns
        COMMAND sh -c "printf 'BNDL????' > \"$1/PkgInfo\"" sh $<TARGET_BUNDLE_CONTENT_DIR:${target}>
        COMMAND sh -c "codesign --force --sign \"$1\" \"$2\" || codesign --force --sign - \"$2\"" sh ${HEADROOM_SIGN_IDENTITY} $<TARGET_BUNDLE_DIR:${target}>
        VERBATIM)
endfunction()

# ── Audio Unit v2 (Apple AudioUnitSDK, Apache-2.0: app/third_party/AudioUnitSDK) ──
set(AUSDK ${APP_ROOT}/third_party/AudioUnitSDK)
file(GLOB AUSDK_SRC ${AUSDK}/src/AudioUnitSDK/*.cpp)
add_library(ausdk STATIC ${AUSDK_SRC})
target_include_directories(ausdk PUBLIC ${AUSDK}/include)
target_compile_options(ausdk PRIVATE -O2 -w)

set(HEADROOM_AU_BUNDLE_ID com.headroom.plugin.au)
set(AU_PLIST ${CMAKE_CURRENT_BINARY_DIR}/HeadroomAU-Info.plist)
file(WRITE ${AU_PLIST} "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleDevelopmentRegion</key><string>English</string>
<key>CFBundleExecutable</key><string>Headroom</string>
<key>CFBundleIdentifier</key><string>${HEADROOM_AU_BUNDLE_ID}</string>
<key>CFBundleName</key><string>Headroom</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleShortVersionString</key><string>${HEADROOM_VERSION_STR}</string>
<key>CFBundleVersion</key><string>${HEADROOM_VERSION_STR}</string>
<key>CFBundleIconFile</key><string>Headroom</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>AudioComponents</key><array><dict>
  <key>type</key><string>aufx</string>
  <key>subtype</key><string>Hdrm</string>
  <key>manufacturer</key><string>Hdrm</string>
  <key>name</key><string>Headroom: Headroom</string>
  <key>description</key><string>Headroom guitar amp and effects rack</string>
  <key>version</key><integer>${HEADROOM_AU_VERSION}</integer>
  <key>factoryFunction</key><string>HeadroomAUFactory</string>
  <key>tags</key><array><string>Effects</string><string>Distortion</string></array>
</dict></array>
</dict></plist>
")
add_library(HeadroomAU MODULE au/HeadroomAU.mm editor/HeadroomEditor.mm)
headroom_bundle(HeadroomAU component ${AU_PLIST} ${HEADROOM_AU_BUNDLE_ID} AU_${_ver_us})
target_compile_definitions(HeadroomAU PRIVATE HEADROOM_AU_BUNDLE_ID="${HEADROOM_AU_BUNDLE_ID}")
target_link_libraries(HeadroomAU PRIVATE ausdk)
# AudioUnitSDK uses std::expected (C++23).
set_target_properties(ausdk HeadroomAU PROPERTIES CXX_STANDARD 23 OBJCXX_STANDARD 23)

# ── VST3 ──
set(VST3SDK ${APP_ROOT}/third_party/vst3sdk)
set(HEADROOM_VST3_BUNDLE_ID com.headroom.plugin.vst3)
set(VST3_PLIST ${CMAKE_CURRENT_BINARY_DIR}/HeadroomVST3-Info.plist)
file(WRITE ${VST3_PLIST} "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>CFBundleDevelopmentRegion</key><string>English</string>
<key>CFBundleExecutable</key><string>Headroom</string>
<key>CFBundleIdentifier</key><string>${HEADROOM_VST3_BUNDLE_ID}</string>
<key>CFBundleName</key><string>Headroom</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleSignature</key><string>????</string>
<key>CFBundleShortVersionString</key><string>${HEADROOM_VERSION_STR}</string>
<key>CFBundleVersion</key><string>${HEADROOM_VERSION_STR}</string>
<key>CFBundleIconFile</key><string>Headroom</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
")
add_library(HeadroomVST3 MODULE
    vst3/HeadroomVST3.mm
    editor/HeadroomEditor.mm
    ${VST3SDK}/public.sdk/source/main/macmain.cpp
    ${VST3SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
headroom_bundle(HeadroomVST3 vst3 ${VST3_PLIST} ${HEADROOM_VST3_BUNDLE_ID} VST3_${_ver_us})
target_compile_definitions(HeadroomVST3 PRIVATE HEADROOM_VST3_BUNDLE_ID="${HEADROOM_VST3_BUNDLE_ID}" $<$<CONFIG:Debug>:DEVELOPMENT=1> $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>)
target_include_directories(HeadroomVST3 PRIVATE ${VST3SDK})
target_link_libraries(HeadroomVST3 PRIVATE sdk)

# ThinLTO on the two plugin bundles only (HEADROOM_LTO=PRODUCTS, the default for macOS arm64 Release builds; docs
# CIRCUIT_ENGINE.md §10.54); the tests below link the plain engine. Called here, after both bundles exist.
headroom_lto_products(TARGETS HeadroomAU HeadroomVST3 LIBRARIES rig_circuit_kernels rig_engine headroom_plugin_core)

# ── tests ──
enable_testing()
# Engine-level hosted-mode regression (no AU/VST3 shell): buffers, allocation guard, state, MIDI, lifecycle.
add_executable(test_plugin_core tests/test_plugin_core.cpp ${APP_ROOT}/tests/isolated_data_dir.cpp) # temp HEADROOM_DATA_DIR: never the real data folder
target_link_libraries(test_plugin_core PRIVATE headroom_plugin_core)
target_compile_definitions(test_plugin_core PRIVATE HEADROOM_TEST_RESEARCH_ROOT="${RESEARCH_ROOT}" HEADROOM_TEST_UI_DIR="${APP_ROOT}/ui")
target_compile_options(test_plugin_core PRIVATE -O2)
add_test(NAME plugin_core COMMAND test_plugin_core)

add_executable(test_plugin_latency tests/test_plugin_latency.cpp ${APP_ROOT}/tests/isolated_data_dir.cpp)
target_link_libraries(test_plugin_latency PRIVATE headroom_plugin_core)
# Offline AudioToolbox host for the installed AU: render, state round-trip, determinism, two instances, RSS.
add_executable(hr_host tests/hr_host.mm ${APP_ROOT}/tests/isolated_data_dir.cpp) # the hosted AU runs in-process: same temp data folder
target_link_libraries(hr_host PRIVATE "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation" "-framework Foundation"
    "-framework Cocoa" "-framework WebKit")
target_compile_options(hr_host PRIVATE -O2 -fobjc-arc)
target_compile_definitions(hr_host PRIVATE HR_DEFAULT_DI="${APP_ROOT}/samples/di/Rock DI.wav" HR_APP_ROOT="${APP_ROOT}")

add_custom_target(plugins ALL DEPENDS HeadroomAU HeadroomVST3)
set(_comp_dst "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
set(_vst3_dst "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
add_custom_target(install-user
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_comp_dst} ${_vst3_dst}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${_comp_dst}/Headroom.component ${_vst3_dst}/Headroom.vst3
    COMMAND ditto $<TARGET_BUNDLE_DIR:HeadroomAU> ${_comp_dst}/Headroom.component
    COMMAND ditto $<TARGET_BUNDLE_DIR:HeadroomVST3> ${_vst3_dst}/Headroom.vst3
    COMMAND sh -c "killall -9 AudioComponentRegistrar 2>/dev/null || true"
    DEPENDS HeadroomAU HeadroomVST3
    COMMENT "Installing Headroom.component and Headroom.vst3 for this user"
    VERBATIM)
add_custom_target(uninstall-user
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${_comp_dst}/Headroom.component ${_vst3_dst}/Headroom.vst3
    COMMAND sh -c "killall -9 AudioComponentRegistrar 2>/dev/null || true"
    VERBATIM)
