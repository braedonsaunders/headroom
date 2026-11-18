# Shared circuit generator and kernel shards for the standalone and plugin projects.
# Include after rig_engine and NAM_ROOT are defined; inherit the caller's optimisation flags.
get_filename_component(_hr_circuit_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(rig_circuit_obj OBJECT ${_hr_circuit_root}/src/circuit/Netlist.cpp ${_hr_circuit_root}/src/circuit/Engine.cpp ${_hr_circuit_root}/src/circuit/Wdf.cpp)
target_include_directories(rig_circuit_obj PUBLIC ${_hr_circuit_root}/src ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_options(rig_circuit_obj PRIVATE ${RIG_WARN} ${RIG_PEDANTIC} ${RIG_O3})
# Ahead-of-time specialised kernels for the shipped netlists (docs/CIRCUIT_ENGINE.md §10.44): circuit_codegen, linked
# against the core with an empty kernel registry, compiles every bound unit as CircuitUnit does and writes straight-line
# kernels for the structures it finds; rig_circuit_kernels compiles them once for both rig_circuit_core and rig_engine.
# Bit-identical to the interpreter (CE_NOCODEGEN=1 at run time switches them off). HEADROOM_CODEGEN=OFF: interpreter only.
option(HEADROOM_CODEGEN "Generate specialised circuit kernels for the shipped netlists" ON)
set(HEADROOM_CODEGEN_ARGS "" CACHE STRING "Extra circuit_codegen arguments (developer: --only ids, --hosts, --sweep)")
add_executable(circuit_codegen ${_hr_circuit_root}/tools/circuit/circuit_codegen.cpp $<TARGET_OBJECTS:rig_circuit_obj>) # defines the empty registry itself
target_include_directories(circuit_codegen PRIVATE ${_hr_circuit_root}/src ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_options(circuit_codegen PRIVATE ${RIG_WARN} ${RIG_O2})
if(NOT APPLE AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(circuit_codegen PRIVATE Threads::Threads)
endif()
if(HEADROOM_CODEGEN)
    set(CG_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_kernels)
    set(CG_SHARDS 16)
    set(CG_SRC ${CG_DIR}/circuit_kernels.cpp)
    math(EXPR _cg_last "${CG_SHARDS} - 1")
    foreach(_i RANGE ${_cg_last})
        list(APPEND CG_SRC ${CG_DIR}/circuit_kernels_${_i}.cpp)
    endforeach()
    add_custom_command(OUTPUT ${CG_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CG_DIR}
        COMMAND circuit_codegen ${CG_DIR} ${_hr_circuit_root}/src/circuit/CircuitUnit.cpp --shards ${CG_SHARDS} ${HEADROOM_CODEGEN_ARGS}
        DEPENDS circuit_codegen ${_hr_circuit_root}/src/circuit/CircuitUnit.cpp ${_hr_circuit_root}/src/circuit/library/EmbeddedLibrary.hpp
        COMMENT "Specialising the shipped circuit netlists (circuit_codegen)"
        VERBATIM)
    # Both projects keep the empty registry in rig_engine's source list and swap it here.
    set_source_files_properties(${_hr_circuit_root}/src/circuit/CodegenStub.cpp PROPERTIES HEADER_FILE_ONLY ON)
else()
    set(CG_SRC ${_hr_circuit_root}/src/circuit/CodegenStub.cpp)
endif()
add_library(rig_circuit_kernels OBJECT ${CG_SRC})
target_include_directories(rig_circuit_kernels PRIVATE ${_hr_circuit_root}/src ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_options(rig_circuit_kernels PRIVATE ${RIG_WARN} ${RIG_PEDANTIC} ${RIG_O3})
if(HEADROOM_CODEGEN)
    target_sources(rig_engine PRIVATE $<TARGET_OBJECTS:rig_circuit_kernels>)
endif()
