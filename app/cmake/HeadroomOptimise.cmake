# Compiler-level optimisation of the whole build: profile-guided optimisation and ThinLTO (docs/CIRCUIT_ENGINE.md §10.54).
# Included by app/CMakeLists.txt and app/plugin/CMakeLists.txt before any target, so the app, the plugins, the engine
# library, circuit_cli and the tests are all built the same way.
#
#   HEADROOM_PGO = AUTO      (default) USE for clang Release builds of x86-64 and of macOS arm64 (identical output on
#                            both, §10.54), when the profile is there and this clang reads it; else OFF
#                  OFF       no profile (the build as before §10.54)
#                  GENERATE  instrumented build for training (app/tools/pgo/train.sh); never ship it
#                  USE       optimise with HEADROOM_PGO_PROFILE (default app/pgo/headroom-<arch>.profdata)
#   HEADROOM_LTO = AUTO      (default) PRODUCTS for clang Release builds for macOS arm64 (incl. universal), else OFF
#                  OFF
#                  PRODUCTS  ThinLTO on the shipped binaries only (Headroom.app, the AU / VST3), via headroom_lto_products()
#                            below: the engine is compiled a second time as bitcode for them, and the tests and tools
#                            keep their ordinary objects (one LTO back end per product instead of one per test)
#                  THIN      ThinLTO on every target (the identity gate builds circuit_cli and the tests this way)
#                  FULL      full LTO on every target (not measured)
#   HEADROOM_PGO_PIN = OFF   (USE only; diagnostic) keep the profile out of the decisions that can change FP results
#
# The FP model is untouched: no -ffast-math / -ffp-contract / -fno-math-errno flag is added or removed, so it stays
# clang's default -ffp-contract=on (a*b+c within one expression becomes llvm.fmuladd). But fmuladd only *permits*
# fusion, and optimisation decisions pick per instance: on AArch64 the loop vectoriser vectorises a non-reassociable
# FP reduction *in order* and splits its fmuladd into fmul + fadd, while the scalar loop keeps fmadd. Whether such a
# loop is vectorised depends on inlining, peeling and trip-count estimates, all of which a profile (or LTO) moves, so
# arm64 PGO changed 57 of 68 units' output. Every clang build that targets arm64 therefore passes
# -mllvm -force-ordered-reductions=false (also to the LTO back end at link time): the AArch64 vectoriser leaves
# in-order FP reductions scalar, as x86-64 always does, and the output no longer depends on those decisions (arm64 PGO
# and PGO + ThinLTO are 68 / 68 identical to the plain build of the same tree). That flag moved the arm64 output once,
# by rounding (-160 dB and below, §10.54). Identity stays a measured gate, not a construction: every new profile or LTO
# change is checked with work/circuit-netlists/pgo/scripts/hashes.sh (plain build against the other).
#
# Front-end instrumentation (-fprofile-instr-generate / -fprofile-instr-use) is used rather than IR PGO: its records
# are keyed on function name + AST hash, with file-local functions prefixed by the source's *basename*, so one profile
# matches any checkout path, any build directory and both the app and the plugin builds. A function whose source has
# changed since training simply has no profile (it compiles as with OFF); retrain when the engine changes materially.
set(HEADROOM_PGO "AUTO" CACHE STRING "Profile-guided optimisation: AUTO, OFF, GENERATE (instrumented training build) or USE")
set_property(CACHE HEADROOM_PGO PROPERTY STRINGS AUTO OFF GENERATE USE)
set(HEADROOM_PGO_PROFILE "" CACHE FILEPATH "Profile for HEADROOM_PGO=USE: .profdata (indexed) or .proftext (merged at configure)")
# A build directory configured before the update to §10.54 caches the old defaults (HEADROOM_PGO_PIN=ON on arm64,
# HEADROOM_LTO=OFF), which set() / option() would keep. Entries still carrying the old help text were never set with
# this module: move them to the new defaults once (an explicit choice made since then carries the new help text).
get_property(_hr_help CACHE HEADROOM_PGO_PIN PROPERTY HELPSTRING)
if(_hr_help MATCHES "needed for FP identity on arm64")
    set_property(CACHE HEADROOM_PGO_PIN PROPERTY VALUE OFF)
endif()
get_property(_hr_help CACHE HEADROOM_LTO PROPERTY HELPSTRING)
if(_hr_help STREQUAL "Link-time optimisation: OFF, THIN or FULL" AND HEADROOM_LTO STREQUAL "OFF")
    set_property(CACHE HEADROOM_LTO PROPERTY VALUE AUTO)
endif()
# The pin was needed on arm64 before -force-ordered-reductions=false; it costs the profile its gain (x86: PGO -3.4 %
# unpinned, +-0 pinned) and is kept as a diagnostic only.
option(HEADROOM_PGO_PIN "With HEADROOM_PGO=USE: keep the profile out of inlining, vectorisation and peeling (diagnostic)" OFF)
set(HEADROOM_LTO "AUTO" CACHE STRING "Link-time optimisation: AUTO, OFF, PRODUCTS (ThinLTO on the app and plugins only), THIN or FULL")
set_property(CACHE HEADROOM_LTO PROPERTY STRINGS AUTO OFF PRODUCTS THIN FULL)

# Compile flags go to clang-compiled sources only: on Linux the C sources (miniaudio) may be built by GCC.
function(_hr_clang_compile_options)
    foreach(_f IN LISTS ARGN)
        add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:${_f}>" "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:${_f}>"
                            "$<$<COMPILE_LANG_AND_ID:OBJC,Clang,AppleClang>:${_f}>" "$<$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>:${_f}>")
    endforeach()
endfunction()

get_filename_component(_hr_opt_app_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_hr_opt_ok TRUE)
if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_hr_opt_ok FALSE)
endif()

set(_hr_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 _hr_arch)
endif()
if(_hr_arch STREQUAL "aarch64")
    set(_hr_arch arm64)
elseif(_hr_arch MATCHES "^(AMD64|amd64)$")
    set(_hr_arch x86_64)
endif()
# Does any slice target arm64? (A universal macOS build lists both; its first architecture picks the profile.)
set(_hr_any_arm64 FALSE)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES OR "arm64e" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_hr_any_arm64 TRUE)
    endif()
elseif(_hr_arch STREQUAL "arm64")
    set(_hr_any_arm64 TRUE)
endif()
get_property(_hr_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_hr_release FALSE)
if(_hr_opt_ok AND NOT _hr_multi AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_hr_release TRUE)
endif()

# LLVM options that must reach every code generator: the compiler, and the LTO back end at link time (ThinLTO runs the
# loop vectoriser after the link, so a compile-only flag would not reach it).
set(_hr_mllvm "")
if(_hr_opt_ok AND _hr_any_arm64)
    # §10.54: leave in-order FP reductions scalar on AArch64 (as on x86-64), so that inlining / vectorisation decisions
    # driven by a profile or by LTO cannot split an llvm.fmuladd into fmul + fadd. In a universal build it reaches the
    # x86_64 slice too, where it is the default already (x86 never vectorises in-order reductions).
    list(APPEND _hr_mllvm -force-ordered-reductions=false)
endif()

# LTO mode. AUTO: ThinLTO on the shipped binaries for clang Release builds for macOS arm64 (§10.54: -3.5 %, 68 / 68
# identical), nothing elsewhere (x86-64: no gain measured).
set(_hr_lto "${HEADROOM_LTO}")
if(_hr_lto STREQUAL "AUTO")
    set(_hr_lto OFF)
    if(_hr_release AND APPLE AND _hr_arch STREQUAL "arm64")
        set(_hr_lto PRODUCTS)
    endif()
endif()
if(NOT _hr_lto MATCHES "^(OFF|PRODUCTS|THIN|FULL)$")
    message(FATAL_ERROR "HEADROOM_LTO must be AUTO, OFF, PRODUCTS, THIN or FULL (got ${HEADROOM_LTO})")
endif()
if(NOT _hr_lto STREQUAL "OFF" AND NOT _hr_opt_ok)
    message(WARNING "HEADROOM_LTO=${HEADROOM_LTO} is wired for clang only; building without LTO")
    set(_hr_lto OFF)
endif()

if(HEADROOM_PGO_PROFILE STREQUAL "")
    set(_hr_prof "${_hr_opt_app_root}/pgo/headroom-${_hr_arch}.profdata")
else()
    set(_hr_prof "${HEADROOM_PGO_PROFILE}")
endif()

# AUTO: USE for clang Release builds on the platforms where the profile measurably helps at identical output
# (§10.54): x86-64, and macOS arm64 (with -force-ordered-reductions=false; the Linux aarch64 build is not gated), when
# the profile is there and this clang reads it; OFF otherwise (Debug, MSVC, GCC, other architectures).
set(_hr_pgo_auto_archs x86_64)
if(APPLE)
    list(APPEND _hr_pgo_auto_archs arm64)
endif()
set(_hr_mode "${HEADROOM_PGO}")
set(_hr_auto FALSE)
if(_hr_mode STREQUAL "AUTO")
    set(_hr_auto TRUE)
    set(_hr_mode OFF)
    if(_hr_release AND _hr_arch IN_LIST _hr_pgo_auto_archs AND EXISTS "${_hr_prof}")
        set(_hr_mode USE)
    endif()
endif()

if(NOT _hr_mode STREQUAL "OFF")
    if(NOT _hr_opt_ok)
        message(WARNING "HEADROOM_PGO=${HEADROOM_PGO} needs clang (not MSVC / GCC); building without a profile")
    elseif(_hr_mode STREQUAL "GENERATE")
        # Raw profiles go where LLVM_PROFILE_FILE says (train.sh sets it); the build itself runs circuit_codegen, whose
        # default.profraw in the build directory is ignored.
        _hr_clang_compile_options(-fprofile-instr-generate)
        add_link_options(-fprofile-instr-generate)
        message(STATUS "Headroom PGO: instrumented build (training only)")
    elseif(_hr_mode STREQUAL "USE")
        if(NOT EXISTS "${_hr_prof}")
            message(FATAL_ERROR "HEADROOM_PGO=USE: no profile at ${_hr_prof} (app/tools/pgo/train.sh writes one)")
        endif()
        if(_hr_prof MATCHES "\\.proftext$")
            # Text profiles are version-independent: index them with this toolchain's llvm-profdata.
            if(APPLE)
                execute_process(COMMAND xcrun -f llvm-profdata OUTPUT_VARIABLE _hr_profdata OUTPUT_STRIP_TRAILING_WHITESPACE)
            else()
                get_filename_component(_hr_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
                find_program(_hr_profdata NAMES llvm-profdata llvm-profdata-${CMAKE_CXX_COMPILER_VERSION_MAJOR} HINTS ${_hr_cxx_dir})
            endif()
            set(_hr_indexed "${CMAKE_BINARY_DIR}/headroom.profdata")
            execute_process(COMMAND ${_hr_profdata} merge -o ${_hr_indexed} ${_hr_prof} RESULT_VARIABLE _hr_rc)
            if(NOT _hr_rc EQUAL 0)
                message(FATAL_ERROR "llvm-profdata could not index ${_hr_prof}")
            endif()
            set(_hr_prof "${_hr_indexed}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hr_prof}")
        # An indexed profile written by a newer llvm-profdata than this clang reads is an error per file: check once.
        file(WRITE "${CMAKE_BINARY_DIR}/hr_pgo_check.cpp" "int main() { return 0; }\n")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} "-fprofile-instr-use=${_hr_prof}" -c "${CMAKE_BINARY_DIR}/hr_pgo_check.cpp"
                                -o "${CMAKE_BINARY_DIR}/hr_pgo_check.o"
                        RESULT_VARIABLE _hr_rc OUTPUT_QUIET ERROR_VARIABLE _hr_err)
        if(NOT _hr_rc EQUAL 0 AND _hr_auto)
            message(STATUS "Headroom PGO: off (AUTO; this compiler cannot read ${_hr_prof})")
            set(_hr_mode OFF)
        elseif(NOT _hr_rc EQUAL 0)
            message(FATAL_ERROR "this compiler cannot read ${_hr_prof} (retrain with app/tools/pgo/train.sh, or pass a "
                                ".proftext as HEADROOM_PGO_PROFILE, or -DHEADROOM_PGO=OFF):\n${_hr_err}")
        endif()
    endif()
    if(_hr_opt_ok AND _hr_mode STREQUAL "USE")
        # A stale profile is expected after engine edits (those functions just get none): not a warning per function.
        _hr_clang_compile_options("-fprofile-instr-use=${_hr_prof}" -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled
                            -Wno-profile-instr-missing -Wno-backend-plugin)
        if(HEADROOM_PGO_PIN)
            # Diagnostic (§10.54; superseded by -force-ordered-reductions=false). Keep the profile away from every
            # decision that can change floating-point results: the
            # inliner's hot / cold thresholds (all at the default 225, no cost-benefit mode), indirect-call promotion,
            # the loop vectoriser's profile trip counts and cold-region limits, profile-guided size optimisation and
            # profile-driven loop peeling. Why: under -ffp-contract=on a*b+c is llvm.fmuladd, which *may* fuse; on
            # AArch64 LoopVectorize vectorises a non-reassociable reduction in order and splits the fmuladd into fmul +
            # in-order fadd, while the scalar loop keeps fmadd. A profile that changes whether such a loop is inlined,
            # peeled or vectorised changes the output (Circuit::updateNoise's 1/f-bank loop did, on 57 of 68 units).
            # What the profile still drives: branch probabilities, block placement and hot/cold layout, register
            # allocation, if-conversion and switch lowering.
            set(_hr_pin -hot-callsite-threshold=225 -inline-cold-callsite-threshold=225 -locally-hot-callsite-threshold=225
                        -inline-enable-cost-benefit-analysis=false -disable-icp -loop-vectorize-with-block-frequency=false
                        -pgso=false -unroll-peel-max-count=0)
            list(APPEND _hr_mllvm ${_hr_pin})
            set(_hr_pin_msg " (pinned: HEADROOM_PGO_PIN)")
        endif()
        message(STATUS "Headroom PGO: ${_hr_prof}${_hr_pin_msg}")
    elseif(_hr_opt_ok AND NOT _hr_mode MATCHES "^(OFF|GENERATE)$")
        message(FATAL_ERROR "HEADROOM_PGO must be AUTO, OFF, GENERATE or USE (got ${HEADROOM_PGO})")
    endif()
endif()

foreach(_f IN LISTS _hr_mllvm)
    _hr_clang_compile_options("-mllvm=${_f}")
endforeach()
if(_hr_mllvm)
    message(STATUS "Headroom LLVM options: ${_hr_mllvm}")
endif()

# LTO compile and link flags. The link carries the LLVM options again: the LTO back end optimises once more there.
set(_hr_lto_cflags "")
set(_hr_lto_lflags "")
if(NOT _hr_lto STREQUAL "OFF")
    if(_hr_lto STREQUAL "FULL")
        set(_hr_lto_cflags -flto=full)
    else()
        set(_hr_lto_cflags -flto=thin)
    endif()
    set(_hr_lto_lflags ${_hr_lto_cflags})
    foreach(_f IN LISTS _hr_mllvm)
        list(APPEND _hr_lto_lflags "LINKER:-mllvm,${_f}")
    endforeach()
    if(APPLE)
        list(APPEND _hr_lto_lflags "-Wl,-cache_path_lto,${CMAKE_BINARY_DIR}/lto.cache")
    else()
        # GNU ld cannot read LLVM bitcode without the gold plugin: link with lld, archive with llvm-ar.
        list(APPEND _hr_lto_lflags -fuse-ld=lld "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto.cache")
        get_filename_component(_hr_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(HEADROOM_LLVM_AR NAMES llvm-ar llvm-ar-${CMAKE_CXX_COMPILER_VERSION_MAJOR} HINTS ${_hr_cxx_dir})
        find_program(HEADROOM_LLVM_RANLIB NAMES llvm-ranlib llvm-ranlib-${CMAKE_CXX_COMPILER_VERSION_MAJOR} HINTS ${_hr_cxx_dir})
        if(HEADROOM_LLVM_AR AND HEADROOM_LLVM_RANLIB)
            set(CMAKE_AR "${HEADROOM_LLVM_AR}" CACHE FILEPATH "" FORCE)
            set(CMAKE_RANLIB "${HEADROOM_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
        endif()
    endif()
    if(_hr_lto STREQUAL "PRODUCTS")
        message(STATUS "Headroom LTO: ThinLTO on the shipped binaries (headroom_lto_products)")
    else()
        _hr_clang_compile_options(${_hr_lto_cflags})
        add_link_options(${_hr_lto_lflags})
        message(STATUS "Headroom LTO: ${_hr_lto} (every target)")
    endif()
endif()

# headroom_lto_products(TARGETS <executable / module>... LIBRARIES <library>...)
# With HEADROOM_LTO=PRODUCTS (AUTO on macOS arm64 Release): every LIBRARY gets a bitcode twin <name>_lto (same sources,
# include paths, definitions, options and links, compiled with -flto=thin), and the TARGETS link the twins instead.
# The tests and tools keep linking the ordinary libraries, so they stay as fast to build as without LTO. List the
# libraries dependencies first (a twin's sources and links are mapped onto the twins made before it, e.g.
# $<TARGET_OBJECTS:rig_circuit_kernels>). Call it once, at the end of the CMakeLists, after every target_sources().
# Any other HEADROOM_LTO value: nothing to do (THIN / FULL already cover every target).
function(_hr_lto_map out)
    set(_r "")
    foreach(_e IN LISTS ARGN)
        foreach(_m IN LISTS _hr_lto_twins)
            if(_e STREQUAL _m)
                set(_e ${_m}_lto)
            else()
                string(REPLACE ":${_m}>" ":${_m}_lto>" _e "${_e}")
            endif()
        endforeach()
        list(APPEND _r "${_e}")
    endforeach()
    set(${out} "${_r}" PARENT_SCOPE)
endfunction()
function(headroom_lto_products)
    if(NOT _hr_lto STREQUAL "PRODUCTS")
        return()
    endif()
    cmake_parse_arguments(A "" "" "TARGETS;LIBRARIES" ${ARGN})
    set(_hr_lto_twins "")
    foreach(_l IN LISTS A_LIBRARIES)
        get_target_property(_type ${_l} TYPE)
        string(REPLACE "_LIBRARY" "" _kind "${_type}") # STATIC / OBJECT / SHARED
        get_target_property(_srcs ${_l} SOURCES)
        _hr_lto_map(_srcs ${_srcs})
        add_library(${_l}_lto ${_kind} ${_srcs})
        foreach(_p INCLUDE_DIRECTORIES SYSTEM_INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
                   LINK_LIBRARIES LINK_OPTIONS INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                   INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES
                   INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
            get_target_property(_v ${_l} ${_p})
            if(_v)
                _hr_lto_map(_v ${_v})
                set_property(TARGET ${_l}_lto PROPERTY ${_p} "${_v}")
            else()
                set_property(TARGET ${_l}_lto PROPERTY ${_p} "")
            endif()
        endforeach()
        foreach(_p C_STANDARD CXX_STANDARD OBJC_STANDARD OBJCXX_STANDARD CXX_EXTENSIONS POSITION_INDEPENDENT_CODE)
            get_target_property(_v ${_l} ${_p})
            if(NOT _v STREQUAL "_v-NOTFOUND")
                set_property(TARGET ${_l}_lto PROPERTY ${_p} "${_v}")
            endif()
        endforeach()
        target_compile_options(${_l}_lto PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:${_hr_lto_cflags}>"
            "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:${_hr_lto_cflags}>" "$<$<COMPILE_LANG_AND_ID:OBJC,Clang,AppleClang>:${_hr_lto_cflags}>"
            "$<$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>:${_hr_lto_cflags}>")
        if(NOT CMAKE_GENERATOR MATCHES "Ninja")
            add_dependencies(${_l}_lto ${_l}) # generated sources: let the original's custom commands run first
        endif()
        list(APPEND _hr_lto_twins ${_l})
    endforeach()
    foreach(_t IN LISTS A_TARGETS)
        foreach(_p LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_v ${_t} ${_p})
            if(_v)
                _hr_lto_map(_v ${_v})
                set_property(TARGET ${_t} PROPERTY ${_p} "${_v}")
            endif()
        endforeach()
        target_compile_options(${_t} PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:${_hr_lto_cflags}>"
            "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:${_hr_lto_cflags}>" "$<$<COMPILE_LANG_AND_ID:OBJC,Clang,AppleClang>:${_hr_lto_cflags}>"
            "$<$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>:${_hr_lto_cflags}>")
        target_link_options(${_t} PRIVATE ${_hr_lto_lflags})
    endforeach()
endfunction()
