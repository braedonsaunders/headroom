// hw_nam_render: render a WAV through a NAM capture (.nam) for the hardware comparison tier (tools/circuit/hw_compare.py).
//   hw_nam_render <model.nam> <in.wav> <out.wav> [--slim s]
// Input is used as-is (digital full scale); the capture's calibrated input level, if any, is printed so the caller
// can convert volts to the capture's digital scale: "input_level_dbu <dBu>" (dBu of a full-scale sine).
// Build: c++ -std=c++20 -O3 -DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -I<NAM> -I<NAM>/NAM -I<NAM>/Dependencies/eigen
//        -I<NAM>/Dependencies/nlohmann -Itools/circuit tools/circuit/hw_nam_render.cpp -Wl,-force_load,<build>/libnam_core.a
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "WavIO.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: hw_nam_render <model.nam> <in.wav> <out.wav> [--slim s]\n");
        return 1;
    }
    auto model = nam::get_dsp(std::filesystem::path(argv[1]));
    if (!model) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    for (int i = 4; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--slim")
            if (auto* s = dynamic_cast<nam::SlimmableModel*>(model.get())) s->SetSlimmableSize(std::atof(argv[i + 1]));
    std::vector<double> in;
    double rate = 0;
    if (!ce_tools::readWav(argv[2], in, rate)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    const double expected = model->GetExpectedSampleRate();
    if (expected > 0 && std::abs(expected - rate) > 0.5) {
        std::fprintf(stderr, "input rate %.0f != model rate %.0f\n", rate, expected);
        return 1;
    }
    std::printf("rate %.0f\n", rate);
    if (model->HasInputLevel()) std::printf("input_level_dbu %.4f\n", model->GetInputLevel());
    const int bs = 64;
    model->Reset(rate, bs);
    std::vector<NAM_SAMPLE> ib(bs), ob(bs);
    NAM_SAMPLE* ip[1] = {ib.data()};
    NAM_SAMPLE* op[1] = {ob.data()};
    std::vector<double> out;
    out.reserve(in.size());
    for (std::size_t p = 0; p < in.size(); p += bs) {
        const int n = static_cast<int>(std::min<std::size_t>(bs, in.size() - p));
        for (int i = 0; i < bs; ++i) ib[static_cast<std::size_t>(i)] = i < n ? static_cast<NAM_SAMPLE>(in[p + static_cast<std::size_t>(i)]) : 0;
        model->process(ip, op, n);
        for (int i = 0; i < n; ++i) out.push_back(ob[static_cast<std::size_t>(i)]);
    }
    ce_tools::writeWav(argv[3], out, rate);
    return 0;
}
