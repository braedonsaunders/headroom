// The empty kernel registry (docs/CIRCUIT_ENGINE.md §10.44): linked into the bootstrap engine the generator runs on, and
// into builds configured with HEADROOM_CODEGEN=OFF. Every group runs on the interpreter.
#include "Codegen.hpp"

namespace rig::ce::cg {
const LinKernel* findLin(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int linKernelCount() noexcept { return 0; }
const LuKernel* findLu(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int luKernelCount() noexcept { return 0; }
const NewtonKernel* findNewton(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int newtonKernelCount() noexcept { return 0; }
} // namespace rig::ce::cg
