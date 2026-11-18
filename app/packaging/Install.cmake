# Release install layout + CPack for macOS, Windows and Linux (docs/PORTING.md, "Packaging").
#
# Included deferred from the end of app/CMakeLists.txt (cmake_language(DEFER ...)), so every target exists by
# the time this runs, wherever it was defined. Install rules never change what `ninja` builds; the one
# build-affecting switch, RIG_RELEASE_BUNDLE, is OFF by default.
#
# Data layout: every platform installs a "data root" that mirrors the source tree under app/, so the engine's
# repo-root-relative lookups work unchanged (RealtimeAudioServer: <root>/app/presets/Showcase,
# <root>/app/presets/Library, <root>/app/resources/models/guitar_midi.hgnn):
#   macOS    Headroom.app/Contents/Resources/ui/            (StudioApp.mm finds the UI here)
#            Headroom.app/Contents/Resources/app/...        data root = Contents; Contents/app -> Resources/app
#   Windows  <install dir>/Headroom.exe, <install dir>/app/{ui,presets,resources,...}   data root = exe dir
#   Linux    <prefix>/bin/headroom, <prefix>/share/headroom/app/{ui,presets,...}       data root = <prefix>/share/headroom
# The Linux/Windows shell must therefore look for the UI at <exe dir>/app/ui/index.html (Windows) and
# <exe dir>/../share/headroom/app/ui/index.html (Linux), and pass the data root as the server's repo root.
#
# Never installed: extracted/ (untracked local files), samples/di (DI recordings, not redistributable),
# samples/processed, work/. packaging/check-bundle.sh enforces that on the staged tree.

set(RIG_PACKAGE_VERSION "0.9.0" CACHE STRING "Version stamped on installers")

set(_rig_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_rig_pkg ${_rig_src}/packaging)

# A release must not compile the build machine's source path in: StudioApp.mm (and the portable shell)
# prefer RIG_APP_SOURCE_DIR/ui (the source checkout).
if(RIG_RELEASE_BUNDLE AND TARGET rig_studio)
    get_target_property(_defs rig_studio COMPILE_DEFINITIONS)
    if(_defs)
        list(FILTER _defs EXCLUDE REGEX "^RIG_APP_SOURCE_DIR=")
        set_target_properties(rig_studio PROPERTIES COMPILE_DEFINITIONS "${_defs}")
    endif()
endif()

# The shared data tree, installed under <dest>/app.
function(rig_install_data dest)
    install(DIRECTORY ${_rig_src}/presets/Showcase ${_rig_src}/presets/Library
            DESTINATION ${dest}/app/presets FILES_MATCHING PATTERN "*.rig")
    install(FILES ${_rig_src}/resources/models/guitar_midi.hgnn DESTINATION ${dest}/app/resources/models)
    install(DIRECTORY ${_rig_src}/resources/brand/ DESTINATION ${dest}/app/resources/brand
            FILES_MATCHING PATTERN "*.svg" PATTERN "*.png")
    # The three MIT demo captures NamComponent.cpp seeds on first run.
    set(_ex ${_rig_src}/third_party/NeuralAmpModelerCore/example_models)
    install(FILES ${_ex}/A2.nam ${_ex}/lstm.nam ${_ex}/my_model.nam
            DESTINATION ${dest}/app/third_party/NeuralAmpModelerCore/example_models OPTIONAL)
    # Licences
    install(FILES ${_rig_pkg}/THIRD_PARTY_NOTICES.txt DESTINATION ${dest}/app/licenses)
    foreach(_l
            "NeuralAmpModelerCore|NeuralAmpModelerCore/LICENSE"
            "Eigen|NeuralAmpModelerCore/Dependencies/eigen/COPYING.MPL2"
            "miniaudio|miniaudio/LICENSE"
            "signalsmith-stretch|signalsmith-stretch/LICENSE.txt"
            "signalsmith-linear|signalsmith-linear/LICENSE.txt"
            "webview|webview/LICENSE"
            "vst3sdk|vst3sdk/LICENSE.txt"
            "AudioUnitSDK|AudioUnitSDK/LICENSE.txt")
        string(REPLACE "|" ";" _p "${_l}")
        list(GET _p 0 _name)
        list(GET _p 1 _file)
        if(EXISTS ${_rig_src}/third_party/${_file})
            install(FILES ${_rig_src}/third_party/${_file} DESTINATION ${dest}/app/licenses/${_name})
        endif()
    endforeach()
endfunction()

function(rig_install_ui dest)
    install(DIRECTORY ${_rig_src}/ui/ DESTINATION ${dest}
            PATTERN ".playwright-cli" EXCLUDE PATTERN ".DS_Store" EXCLUDE PATTERN "*.map" EXCLUDE)
endfunction()

if(TARGET rig_studio)
    if(APPLE)
        install(TARGETS rig_studio BUNDLE DESTINATION . COMPONENT app)
        rig_install_ui(Headroom.app/Contents/Resources/ui)
        rig_install_data(Headroom.app/Contents/Resources)
        install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink Resources/app
                      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/Headroom.app/Contents/app\")")
    elseif(WIN32)
        install(TARGETS rig_studio RUNTIME DESTINATION . COMPONENT app)
        rig_install_ui(app/ui)
        if(RIG_WITH_ASIO)
            install(FILES "${RIG_ASIO_SDK_DIR}/Steinberg ASIO Logo Artwork/ASIO-compatible-logo-Steinberg-TM-BW.png"
                    DESTINATION app/ui/assets RENAME asio-compatible.png)
        endif()
        rig_install_data(.)
        # WebView2Loader.dll only when the shell links the loader dynamically (Fork C decides; static is preferred).
        if(DEFINED RIG_WEBVIEW2_LOADER_DLL AND EXISTS "${RIG_WEBVIEW2_LOADER_DLL}")
            install(FILES "${RIG_WEBVIEW2_LOADER_DLL}" DESTINATION .)
        endif()
    else()
        set(CMAKE_INSTALL_DOCDIR share/doc/headroom)
        include(GNUInstallDirs)
        install(PROGRAMS $<TARGET_FILE:rig_studio> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME headroom COMPONENT app)
        rig_install_ui(${CMAKE_INSTALL_DATADIR}/headroom/app/ui)
        rig_install_data(${CMAKE_INSTALL_DATADIR}/headroom)
        install(FILES ${_rig_pkg}/linux/headroom.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES ${_rig_pkg}/linux/com.headroom.studio.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
        install(FILES ${_rig_pkg}/linux/icons/headroom-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME headroom.png)
        install(FILES ${_rig_pkg}/linux/icons/headroom-512.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps RENAME headroom.png)
        install(FILES ${_rig_src}/resources/brand/headroom-app-icon.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME headroom.svg)
        install(FILES ${_rig_pkg}/THIRD_PARTY_NOTICES.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})
    endif()
endif()

# rig_cli ships as a developer tool only on Linux/Windows archives (component "cli", not in the default installers).
if(TARGET rig_cli AND NOT APPLE)
    install(TARGETS rig_cli RUNTIME DESTINATION bin COMPONENT cli EXCLUDE_FROM_ALL)
endif()

# ── CPack ──
set(CPACK_PACKAGE_NAME "Headroom")
set(CPACK_PACKAGE_VENDOR "Headroom")
set(CPACK_PACKAGE_VERSION "${RIG_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Guitar and bass amp simulator")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/braedonsaunders/headroom")
set(CPACK_PACKAGE_CONTACT "Headroom <https://github.com/braedonsaunders/headroom/issues>")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Headroom")
set(CPACK_STRIP_FILES ON)
if(APPLE)
    # packaging/macos/build-release.sh signs the staged bundle and builds the .dmg itself (CPack can't sign
    # nested code in the right order); this TGZ is only a debugging aid.
    set(CPACK_GENERATOR "TGZ")
elseif(WIN32)
    # packaging/windows/headroom.iss (Inno Setup) builds the installer from `cmake --install` output; ZIP = portable.
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_NAME "headroom")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)       # dpkg-shlibdeps derives WebKitGTK/GTK/ALSA/JACK deps
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire-jack | jackd2")
endif()
include(CPack)
