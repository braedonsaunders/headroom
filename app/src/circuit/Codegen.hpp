#pragma once
// Ahead-of-time specialised kernels for the shipped netlists (docs/CIRCUIT_ENGINE.md §10.44).
//
// At build time tools/circuit/circuit_codegen compiles every bound library netlist the way its unit does, walks each
// group runtime (the full step and the LTE / step-halving substep runtimes) and writes straight-line C++ for the fixed
// structures it finds:
//   1. SparseLin (linPorts / linOut): R's column pattern, L's and U's fill, the back-substitution rows, and the port /
//      output difference maps with every index baked in.
//   2. SparseLu (spFactor / spStep): the sparse nodal Newton factorisation and step on the static / live split, with
//      device stamp positions, live column scalings, live elimination updates, forward solve, back-substitution rows
//      and port-step gathers baked in.
//   3. The dense Newton kernel (docs §10.45): the shared template nk::newtonKernel instantiated on the group's structure
//      (device laws, port counts, limiter kinds, current-convergence ports), so its device and port loops are straight-
//      line code with each law and limiter inlined; the same source as the interpreter's instantiation.
// The registries are keyed by 128-bit signatures of those structures; Circuit::buildSparseLin and Circuit::buildSparse
// look the signature up at reset() and bind the kernel when it is there. A structure the build did not see (a user-
// edited circuit, or an unseen Markowitz tie-break) has no entry: the interpreter runs it as before.
//
// Bit identity by construction: each kernel performs the interpreter's floating-point operations on the same operands
// in the same order (the same expression shapes, so the same FMA contractions); only index loads, loop control and
// stores of values nothing reads go. R entries that were exactly ±1 at generation are baked as an add / subtract of the
// input (exact: fma(±1, w, b) = b ± w); SparseLin::pack checks them against the live values after every refresh and
// keeps the interpreter for any refresh where one differs. Since §10.66 the kernels also drop the interpreter's zero
// tests (an update, column or substitution term by an exact zero changes no value; columns no input reaches are left
// out) and the factorisation's early pivot returns (one flag at the end): one branch-free block, the same values
// (68 / 68 units hash equal; CE_CG_OLD=1 at generation emits the kernels as before).
//
// CE_NOCODEGEN=1: never bind a kernel (the interpreter everywhere; developer A/B).
#include <cstdint>
#include <string>
#include <vector>

namespace rig::ce {
struct NlDevice;
namespace nk {
struct KaCache;
struct KernelState;
struct SparseLu;
} // namespace nk
} // namespace rig::ce

namespace rig::ce::cg {

// y = the solve's port (linPorts) or output rows (linOut); see SparseLin. bias: the held noise offsets (null: noise off).
// bPart: linPorts writes the right-hand side after the state and input columns there (null: don't); linOut starts from it
// (null: from zero, summing every column).
using LinFn = void (*)(const double* __restrict lv, const double* __restrict uv, const double* __restrict rv, const double* __restrict inv,
                       const double* __restrict w, double* __restrict y, const double* __restrict bias, double* __restrict bPart) noexcept;

struct LinKernel {
    std::uint64_t h1, h2; // structure signature (linSignature in Engine.cpp)
    LinFn ports, out;
    int nBaked;
    const int* bakedIdx;    // rVal positions baked as constants
    const double* bakedVal; // their values (±1)
};

// Sparse nodal Newton factorisation and step (see spFactor / spStep in Engine.cpp).
// snap: static values (nL + nU + n); cmaxS: static column maxima (n); Jd: device Jacobians [port][3];
// live: active port indices (null: all ports); na: active port count; v: packed L|U|D workspace; inv: pivot inverses (n).
using LuFactorFn = bool (*)(const double* __restrict snap, const double* __restrict cmaxS, const double (*__restrict Jd)[3],
                            const int* __restrict live, int na, double* __restrict v, double* __restrict inv) noexcept;
using LuStepFn = void (*)(const double* __restrict v, const double* __restrict inv, const double (*__restrict Jd)[3],
                          const int* __restrict live, int na, const double* __restrict F, double* __restrict d) noexcept;

struct LuKernel {
    std::uint64_t h1, h2; // structure signature (luSignature in Engine.cpp)
    LuFactorFn factor;
    LuStepFn step;
};

// The dense port-space Newton kernel specialised to one group structure (docs §10.45): nk::newtonKernel<NN,
// nk::StaticSpec<G>> with the group's devices (kinds, port counts, exact-split shifts), per-port limiter kinds and
// current-convergence ports as constants. The arguments are newtonKernel's (NewtonKernel.hpp).
using NewtonFn = bool (*)(const NlDevice* const* devs, int ndev, const int* bs, const int* bn, const double* __restrict K, const double* __restrict p,
                          double* __restrict vpOut, double* __restrict curOut, const std::uint8_t* lim, const double* limA, const double* limB,
                          const double* limC, int maxIt, double tol, int& itersOut, double& residualOut, const double* kcol, bool chordOn,
                          nk::KaCache* kac, int nDyn, nk::KernelState* ks, const double* dpPred, double* luScratch, nk::SparseLu* sp,
                          const double* ptol) noexcept;

struct NewtonKernel {
    std::uint64_t h1, h2; // structure signature (newtonSignature in Engine.cpp)
    NewtonFn fn;
};

// The generated registries (circuit_kernels.cpp in the build tree; CodegenStub.cpp when the build has none).
const LinKernel* findLin(std::uint64_t h1, std::uint64_t h2) noexcept;
int linKernelCount() noexcept;
const LuKernel* findLu(std::uint64_t h1, std::uint64_t h2) noexcept;
int luKernelCount() noexcept;
const NewtonKernel* findNewton(std::uint64_t h1, std::uint64_t h2) noexcept;
int newtonKernelCount() noexcept;

// One SparseLin's structure as the generator reads it (Circuit::codegenLinPatterns).
struct LinPattern {
    std::uint64_t h1 = 0, h2 = 0;
    int n = 0, ns = 0, nu = 0, nn = 0, nuc = 0, ny = 0, W = 0;
    std::vector<int> lOff, lIdx, uOff, uIdx, cOff, cRow, bPorts, bOut;
    std::vector<int> pA, pB, pJA, pJB, oA, oB, oJA, oJB;
    std::vector<double> rVal; // R's packed values at generation (entries exactly ±1 are candidates for baking)
    std::string where;        // group / runtime, for the generated file's comments
};

// One SparseLu's structure as the generator reads it (Circuit::codegenLuPatterns).
struct LuPattern {
    std::uint64_t h1 = 0, h2 = 0;
    int n = 0, nn = 0;
    std::vector<int> bs, bn;
    std::vector<int> lOff, lIdx, uOff, uIdx;
    std::vector<int> stOff, stPos, stSign; // stSign: +1 or -1
    std::vector<std::uint8_t> kFl;
    std::vector<int> dOff, dIdx, rOff, rE, oOff, oQ, oU;
    std::vector<int> bRows, pFrom, pTo, xP, xN;
    std::string where;
};

// One Newton kernel structure as the generator reads it (Circuit::codegenNewtonPatterns): per device its law (NlKind), port
// count and exact-split flag; per port its limiter kind, its device, and the current-convergence flags (ccA: a floating
// zener junction; ccB: a diode or zener junction, on its current in groups with per-port tolerances).
struct NewtonPattern {
    std::uint64_t h1 = 0, h2 = 0;
    int n = 0;
    std::vector<int> kind, np, off, shift; // per device
    std::vector<int> lim, dev, ccA, ccB;   // per port
    std::string where;
};

} // namespace rig::ce::cg
