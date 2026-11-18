#pragma once
#include "TubeDevice.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace rig::circuit {
class PreampCircuit {
public:
  struct Config {
    bool directTone = true;
    double supply = 300, inputC = 22e-9, gridR = 68000, gridLeakR = 1000000,
           plateR = 100000, cathodeR = 1500, followerR = 100000,
           cathodeC = 0.68e-6, couplingC = 22e-9, slopeR = 33000,
           trebleC = 470e-12, bassC = 22e-9, midC = 22e-9, trebleR = 250000,
           bassR = 1000000, midR = 25000, loadR = 1000000;
  };
  // These configure the coupled pair and tone stack, not an entire amplifier.
  static Config marshall2204Pair() noexcept {
    Config c;
    c.supply = 348;
    c.cathodeR = 820;
    c.cathodeC = 0;
    c.gridLeakR = 470000;
    c.trebleR = 220000;
    c.midR = 22000;
    return c;
  }
  static Config bassman5f6aToneStack() noexcept {
    Config c;
    c.slopeR = 100000;
    c.trebleC = 250e-12;
    c.bassC = 100e-9;
    c.midC = 20e-9;
    return c;
  }
  struct Diagnostics {
    double plate = 0, cathode = 0, follower = 0, grid = 0, output = 0,
           residual = 0;
    unsigned iterations = 0, failures = 0, retries = 0;
  };
  void prepare(double rate) noexcept {
    rate_ = std::clamp(std::isfinite(rate) ? rate : 48000., 8000., 384000.);
    reset();
  }
  void setConfig(const Config &c) noexcept {
    cfg_ = c;
    sanitize();
    reset();
  }
  void setControls(double bass, double mid, double treble) noexcept {
    bass = control(bass);
    mid = control(mid);
    treble = control(treble);
    if (bass != bass_ || mid != mid_ || treble != treble_) {
      bass_ = bass;
      mid_ = mid;
      treble_ = treble;
      dirty_ = true;
    }
  }
  void setSupplyVolts(double volts) noexcept {
    if (std::isfinite(volts))
      cfg_.supply = std::clamp(volts, 50.0, 600.0);
  }
  double supplyCurrentAmps() const noexcept {
    return (cfg_.supply - x_[0]) / cfg_.plateR +
           TubeDevice::plate(x_[0] - x_[2], cfg_.supply - x_[2]);
  }
  // Audio-thread handoff only. Keep the replacement's electrical values and
  // controls while carrying capacitor charge and the last accepted solution.
  void adoptAudioStateFrom(const PreampCircuit& previous) noexcept {
    if (rate_ != previous.rate_ || cfg_.directTone != previous.cfg_.directTone) return;
    std::copy(previous.x_, previous.x_ + N, x_);
    std::copy(previous.capV_, previous.capV_ + 5, capV_);
    std::copy(previous.capI_, previous.capI_ + 5, capI_);
    inputCapV_ = previous.inputCapV_;
    inputCapI_ = previous.inputCapI_;
    previousInput_ = previous.previousInput_;
    diag_ = previous.diag_;
    dirty_ = true;
  }
  const Config &config() const noexcept { return cfg_; }
  const Diagnostics &diagnostics() const noexcept { return diag_; }
  void reset() noexcept {
    dirty_ = true;
    bdf1_ = false;
    inputCapV_ = inputCapI_ = previousInput_ = 0;
    std::memset(x_, 0, sizeof(x_));
    std::memset(capV_, 0, sizeof(capV_));
    std::memset(capI_, 0, sizeof(capI_));
    diag_ = {};
    dc_ = true;
    if (!solve(0))
      ++diag_.failures;
    dc_ = false;
    dirty_ = true;
    inputCapV_ = -x_[10];
    for (int j = 0; j < 5; ++j)
      capV_[j] = voltage(x_, ca_[j], cb_[j]);
  }
  double process(double inputVolts) noexcept {
    if (!std::isfinite(inputVolts))
      inputVolts = 0;
    inputVolts = std::clamp(inputVolts, -100., 100.);
    if (!solve(inputVolts)) {
      ++diag_.retries;
      // Retry a difficult transient with bounded L-stable Backward-Euler (BDF1)
      // homotopy substeps so stiff grid-conduction steps damp without trapezoidal
      // ringing. Snapshot all dynamic state so an unsuccessful retry cannot
      // partially advance time.
      double oldX[N], oldV[5], oldI[5];
      std::memcpy(oldX, x_, sizeof x_);
      std::memcpy(oldV, capV_, sizeof capV_);
      std::memcpy(oldI, capI_, sizeof capI_);
      const double oldInputV = inputCapV_, oldInputI = inputCapI_;
      const double originalRate = rate_;
      bool success = false;
      for (int steps : {4, 8}) {
        std::memcpy(x_, oldX, sizeof x_);
        std::memcpy(capV_, oldV, sizeof capV_);
        std::memcpy(capI_, oldI, sizeof capI_);
        inputCapV_ = oldInputV;
        inputCapI_ = oldInputI;
        rate_ = originalRate * steps;
        bdf1_ = true;
        dirty_ = true;
        success = true;
        for (int i = 1; i <= steps; ++i) {
          const double subInput =
              previousInput_ + (inputVolts - previousInput_) * i / steps;
          if (!solve(subInput)) {
            success = false;
            break;
          }
        }
        if (success)
          break;
      }
      bdf1_ = false;
      rate_ = originalRate;
      dirty_ = true;
      if (!success) {
        std::memcpy(x_, oldX, sizeof x_);
        std::memcpy(capV_, oldV, sizeof capV_);
        std::memcpy(capI_, oldI, sizeof capI_);
        inputCapV_ = oldInputV;
        inputCapI_ = oldInputI;
        ++diag_.failures;
        diag_.plate = x_[0];
        diag_.cathode = x_[1];
        diag_.follower = x_[2];
        diag_.grid = x_[9];
        diag_.output = x_[8];
      }
    }
    previousInput_ = inputVolts;
    return x_[8];
  }

private:
  // Node order: plate, cathode, follower cathode, stack input, slope node,
  // treble top, bass top, mid node, output, input grid, input capacitor,
  // mid-pot top. Ground is -1.
  static constexpr int N = 12;
  Config cfg_{};
  double rate_ = 48000, bass_ = 0.5, mid_ = 0.5, treble_ = 0.5, x_[N]{},
         capV_[5]{}, capI_[5]{}, inputCapV_ = 0, inputCapI_ = 0,
         previousInput_ = 0;
  Diagnostics diag_{};
  bool dc_ = false, dirty_ = true, bdf1_ = false;
  double linear_[N][N]{};
  // Only tube-connected nodes are nonlinear. Eliminate the passive network
  // exactly once per matrix change; Newton still tests all twelve KCL rows.
  static constexpr int active_[4] = {0, 1, 2, 9};
  static constexpr int passive_[8] = {3, 4, 5, 6, 7, 8, 10, 11};
  double passiveInverse_[8][8]{}, passiveResponse_[8][4]{}, schur_[4][4]{};
  bool reductionValid_ = false;
  void prepareReduction() noexcept {
    double matrix[8][16]{};
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j)
        matrix[i][j] = linear_[passive_[i]][passive_[j]];
      matrix[i][8 + i] = 1;
    }
    reductionValid_ = false;
    for (int k = 0; k < 8; ++k) {
      int pivot = k;
      for (int i = k + 1; i < 8; ++i)
        if (std::abs(matrix[i][k]) > std::abs(matrix[pivot][k])) pivot = i;
      if (std::abs(matrix[pivot][k]) < 1e-14) return;
      for (int j = 0; j < 16; ++j) std::swap(matrix[k][j], matrix[pivot][j]);
      const double divisor = matrix[k][k];
      for (int j = 0; j < 16; ++j) matrix[k][j] /= divisor;
      for (int i = 0; i < 8; ++i) if (i != k) {
        const double factor = matrix[i][k];
        for (int j = 0; j < 16; ++j) matrix[i][j] -= factor * matrix[k][j];
      }
    }
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) passiveInverse_[i][j] = matrix[i][8 + j];
      for (int j = 0; j < 4; ++j) {
        passiveResponse_[i][j] = 0;
        for (int k = 0; k < 8; ++k)
          passiveResponse_[i][j] += passiveInverse_[i][k] * linear_[passive_[k]][active_[j]];
      }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
      schur_[i][j] = 0;
      for (int k = 0; k < 8; ++k)
        schur_[i][j] += linear_[active_[i]][passive_[k]] * passiveResponse_[k][j];
    }
    reductionValid_ = true;
  }
  bool reducedStep(double (&a)[N][N + 1]) const noexcept {
    if (!reductionValid_) return false;
    double passiveRhs[8]{};
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 8; ++j)
      passiveRhs[i] += passiveInverse_[i][j] * a[passive_[j]][N];
    double small[4][5];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j)
        small[i][j] = a[active_[i]][active_[j]] - schur_[i][j];
      small[i][4] = a[active_[i]][N];
      for (int j = 0; j < 8; ++j)
        small[i][4] -= linear_[active_[i]][passive_[j]] * passiveRhs[j];
    }
    for (int k = 0; k < 4; ++k) {
      int pivot = k;
      for (int i = k + 1; i < 4; ++i)
        if (std::abs(small[i][k]) > std::abs(small[pivot][k])) pivot = i;
      if (std::abs(small[pivot][k]) < 1e-14) return false;
      for (int j = k; j < 5; ++j) std::swap(small[k][j], small[pivot][j]);
      const double divisor = small[k][k];
      for (int j = k; j < 5; ++j) small[k][j] /= divisor;
      for (int i = 0; i < 4; ++i) if (i != k) {
        const double factor = small[i][k];
        for (int j = k; j < 5; ++j) small[i][j] -= factor * small[k][j];
      }
    }
    for (int i = 0; i < 4; ++i) a[active_[i]][N] = small[i][4];
    for (int i = 0; i < 8; ++i) {
      a[passive_[i]][N] = passiveRhs[i];
      for (int j = 0; j < 4; ++j)
        a[passive_[i]][N] -= passiveResponse_[i][j] * small[j][4];
    }
    return true;
  }
  static constexpr int ca_[5] = {1, 2, 3, 4, 4}, cb_[5] = {-1, 3, 5, 6, 7};
  static double control(double x) noexcept {
    return std::clamp(std::isfinite(x) ? x : 0.5, 0.0, 1.0);
  }
  static double voltage(const double *x, int a, int b) noexcept {
    return x[a] - (b < 0 ? 0 : x[b]);
  }
  void sanitize() noexcept {
    Config d{};
    auto fix = [](double &v, double fallback, double lo, double hi) {
      v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    fix(cfg_.supply, d.supply, 50, 600);
    double *rs[] = {&cfg_.gridR,    &cfg_.gridLeakR, &cfg_.plateR,
                    &cfg_.cathodeR, &cfg_.followerR, &cfg_.slopeR,
                    &cfg_.trebleR,  &cfg_.bassR,     &cfg_.midR,
                    &cfg_.loadR};
    const double rd[] = {d.gridR,     d.gridLeakR, d.plateR,  d.cathodeR,
                         d.followerR, d.slopeR,    d.trebleR, d.bassR,
                         d.midR,      d.loadR};
    for (int i = 0; i < 10; ++i)
      fix(*rs[i], rd[i], 10, 1e8);
    double *cs[] = {&cfg_.inputC,  &cfg_.cathodeC, &cfg_.couplingC,
                    &cfg_.trebleC, &cfg_.bassC,    &cfg_.midC};
    const double cd[] = {d.inputC,  d.cathodeC, d.couplingC,
                         d.trebleC, d.bassC,    d.midC};
    for (int i = 0; i < 6; ++i)
      fix(*cs[i], cd[i], i == 1 ? 0.0 : 1e-12, 1e-3);
  }
  void residual(const double *v, double in, double *r,
                bool tubes = true) const noexcept {
    std::fill(r, r + N, 0.0);
    auto branch = [&](int a, int b, double i) {
      if (a >= 0)
        r[a] += i;
      if (b >= 0)
        r[b] -= i;
    };
    auto res = [&](int a, int b, double resistance) {
      branch(a, b, voltage(v, a, b) / std::max(1.0, resistance));
    };
    const double capScale = bdf1_ ? rate_ : (2.0 * rate_);
    const double histSign = bdf1_ ? 0.0 : 1.0;
    res(10, 9, cfg_.gridR);
    res(9, -1, cfg_.gridLeakR);
    if (!dc_)
      branch(-1, 10,
             capScale * cfg_.inputC * (in - v[10] - inputCapV_) -
                 histSign * inputCapI_);
    branch(0, -1, (v[0] - cfg_.supply) / cfg_.plateR);
    res(1, -1, cfg_.cathodeR);
    res(2, -1, cfg_.followerR);
    if (tubes) {
      branch(0, 1, TubeDevice::plate(v[9] - v[1], v[0] - v[1]));
      branch(9, 1, TubeDevice::grid(v[9] - v[1]));
      branch(-1, 2, TubeDevice::plate(v[0] - v[2], cfg_.supply - v[2]));
      branch(0, 2, TubeDevice::grid(v[0] - v[2]));
    }
    branch(3, -1, v[3] * 1e-10);
    branch(4, -1, v[4] * 1e-10);
    res(3, 4, cfg_.slopeR);
    res(5, 8, std::max(10., cfg_.trebleR * (1 - treble_)));
    res(8, 6, std::max(10., cfg_.trebleR * treble_));
    res(6, 11, std::max(10., cfg_.bassR * (1 - bass_)));
    res(11, 7, std::max(10., cfg_.midR * (1 - mid_)));
    res(7, -1, std::max(10., cfg_.midR * mid_));
    res(8, -1, cfg_.loadR);
    if (cfg_.directTone)
      res(2, 3, 10.0);
    const double cs[5] = {cfg_.cathodeC, cfg_.couplingC, cfg_.trebleC,
                          cfg_.bassC, cfg_.midC};
    if (!dc_)
      for (int j = 0; j < 5; ++j)
        if (j != 1 || !cfg_.directTone)
          branch(ca_[j], cb_[j],
                 capScale * cs[j] * (voltage(v, ca_[j], cb_[j]) - capV_[j]) -
                     histSign * capI_[j]);
  }
  bool solve(double in) noexcept {
    double v[N];
    std::memcpy(v, x_, sizeof v);
    if (dc_) {
      v[0] = 150;
      v[1] = 1.5;
      v[2] = 150;
    }
    if (dirty_) {
      double base[N], zero[N]{};
      residual(zero, in, base, false);
      for (int col = 0; col < N; ++col) {
        double w[N]{}, rr[N];
        w[col] = 1;
        residual(w, in, rr, false);
        for (int row = 0; row < N; ++row)
          linear_[row][col] = rr[row] - base[row];
      }
      prepareReduction();
      dirty_ = false;
    }
    double err = 0;
    unsigned it = 0;
    bool good = false;
    for (; it < 20; ++it) {
      double r[N], a[N][N + 1];
      residual(v, in, r);
      err = 0;
      for (double z : r)
        err = std::max(err, std::abs(z));
      if (err < 1e-10) {
        good = true;
        break;
      }
      for (int row = 0; row < N; ++row)
        for (int col = 0; col < N; ++col)
          a[row][col] = linear_[row][col];
      const auto first = TubeDevice::plateDerivatives(v[9] - v[1], v[0] - v[1]);
      const auto follower =
          TubeDevice::plateDerivatives(v[0] - v[2], cfg_.supply - v[2]);
      const double gridFirst = TubeDevice::gridDerivative(v[9] - v[1]);
      const double gridFollower = TubeDevice::gridDerivative(v[0] - v[2]);
      auto stamp = [&](int row, int col, double value) {
        a[row][col] += value;
      };
      // First plate current leaves node 0 and enters node 1.
      for (int row : {0, 1}) {
        const double sign = row == 0 ? 1.0 : -1.0;
        stamp(row, 9, sign * first.grid);
        stamp(row, 0, sign * first.plate);
        stamp(row, 1, -sign * (first.grid + first.plate));
      }
      stamp(9, 9, gridFirst);
      stamp(9, 1, -gridFirst);
      stamp(1, 9, -gridFirst);
      stamp(1, 1, gridFirst);
      stamp(2, 0, -follower.grid);
      stamp(2, 2, follower.grid + follower.plate);
      stamp(0, 0, gridFollower);
      stamp(0, 2, -gridFollower);
      stamp(2, 0, -gridFollower);
      stamp(2, 2, gridFollower);
      for (int row = 0; row < N; ++row)
        a[row][N] = -r[row];
      if (!reducedStep(a)) break;
      double scale = 1;
      for (int j = 0; j < N; ++j)
        if (std::abs(a[j][N]) > 30)
          scale = std::min(scale, 30 / std::abs(a[j][N]));
      for (int j = 0; j < N; ++j)
        v[j] += scale * a[j][N];
    }
    if (good) {
      if (!dc_) {
        const double capScale = bdf1_ ? rate_ : (2.0 * rate_);
        const double histSign = bdf1_ ? 0.0 : 1.0;
        double nvInput = in - v[10];
        inputCapI_ =
            capScale * cfg_.inputC * (nvInput - inputCapV_) - histSign * inputCapI_;
        inputCapV_ = nvInput;
        const double cs[5] = {cfg_.cathodeC, cfg_.couplingC, cfg_.trebleC,
                              cfg_.bassC, cfg_.midC};
        for (int j = 0; j < 5; ++j) {
          if (j == 1 && cfg_.directTone)
            continue;
          double nv = voltage(v, ca_[j], cb_[j]);
          capI_[j] = capScale * cs[j] * (nv - capV_[j]) - histSign * capI_[j];
          capV_[j] = nv;
        }
      }
      std::memcpy(x_, v, sizeof v);
    }
    diag_.plate = x_[0];
    diag_.cathode = x_[1];
    diag_.follower = x_[2];
    diag_.grid = x_[9];
    diag_.output = x_[8];
    diag_.residual = err;
    diag_.iterations = it;
    return good;
  }
};
} // namespace rig::circuit
