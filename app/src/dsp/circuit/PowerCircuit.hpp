#pragma once
#include "PowerDevices.hpp"
#include "TubeDevice.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rig::circuit {
struct PowerConfig {
  double reservoirSourceVolts = 475.0, reservoirResistanceOhms = 200.0,
         reservoirCapacitanceFarads = 100e-6;
  double screenResistanceOhms = 1000.0, screenCapacitanceFarads = 22e-6;
  double preampResistanceOhms = 47000.0, preampCapacitanceFarads = 22e-6;
  double phasePlateAOhms = 82000.0, phasePlateBOhms = 100000.0;
  double phaseCathodeOhms = 470.0, phaseTailOhms = 10000.0,
         phaseTailReferenceVolts = 13.5;
  double outputGridLeakOhms = 150000.0, outputCouplingFarads = 22e-9;
  double outputBiasVolts = -37.0;
};
class PowerCircuit {
  PowerConfig config_{};
  double fs_ = 48000, feedback_ = 0.4, presence_ = 0.5, depth_ = 0.5,
         loadR_ = 1000, loadL_ = 1.0, preampLoad_ = 0.002, plateA_ = 0,
         plateB_ = 0, kcl_ = 0;
  double supply_ = 450, screen_ = 400, preamp_ = 300, ga_ = -37, gb_ = -37;
  double va0_ = 180, vb0_ = 180, cathode_ = 1.5, il_ = 0, im_ = 0, cm_ = 0,
         fbLow_ = 0, fbHigh_ = 0, out_ = 0, ia_ = 0, ib_ = 0, ig_ = 0;
  std::uint64_t failures_ = 0, inputClamps_ = 0;
  static double clamp(double x, double a, double b) noexcept {
    return std::max(a, std::min(b, x));
  }
  // Backward-Euler grid coupling and tube grid conduction. The source plate
  // current includes capacitor current, so positive grid current loads the PI.
  double coupledGrid(double plate, double oldPlate, double oldGrid,
                     double dt) const noexcept {
    const double c = config_.outputCouplingFarads,
                 leak = config_.outputGridLeakOhms;
    const double rhs =
        oldGrid + plate - oldPlate - dt / c * (-config_.outputBiasVolts / leak);
    const double d = 1.0 + dt / (c * leak);
    const double unloaded = rhs / d;
    if (unloaded <= 0)
      return unloaded;
    const double a = d + dt / (c * 1500.0), b = 0.5 * d - rhs;
    return rhs / (std::sqrt(b * b + 2 * a * rhs) + b);
  }
  using GridEvaluation = TubeDevice::GridEvaluation;
  static GridEvaluation evaluateGrid(double voltage) noexcept {
    return TubeDevice::gridWithDerivative(voltage);
  }
  static double gridSourceVoltage(double raw, GridEvaluation* result = nullptr,
                                  double initial = 1.0) noexcept {
    auto grid = evaluateGrid(raw);
    if (68000.0 * grid.current < 1e-10) {
      if (result)
        *result = grid;
      return raw;
    }
    double lo = raw - 68000.0 * grid.current, hi = raw;
    double value = std::clamp(initial, lo, hi);
    for (int i = 0; i < 16; ++i) {
      grid = evaluateGrid(value);
      const double f = value + 68000.0 * grid.current - raw;
      if (std::abs(f) < 1e-10) {
        if (result)
          *result = grid;
        return value;
      }
      if (f > 0)
        hi = value;
      else
        lo = value;
      const double next = value - f / (1 + 68000.0 * grid.slope);
      value = (next > lo && next < hi) ? next : (lo + hi) * 0.5;
    }
    if (result)
      *result = evaluateGrid(value);
    return value;
  }
  bool phase(double input, double feedback, double dt, double &a,
             double &b) noexcept {
    double x[3] = {a, b, cathode_};
    // The 470-ohm cathode resistor returns to the elevated grid-bias junction.
    // The 10k tail ends at the reference/feedback node. A grid-source
    // resistance limits positive grid current, approximating the PI input
    // coupling network.
    double cachedGridA = 0, cachedGridB = 0;
    TubeDevice::Derivatives cachedDa{}, cachedDb{};
    GridEvaluation cachedSourceA{}, cachedSourceB{};
    double sourceGuessA = 1, sourceGuessB = 1;
    auto tube = [](double vg, double vp,
                   TubeDevice::Derivatives &derivative) noexcept {
      return TubeDevice::plateWithDerivatives(vg, vp, derivative);
    };
    auto residual = [&](const double *z, double *f) noexcept {
      const double tail = config_.phaseTailReferenceVolts;
      const double totalR = config_.phaseTailOhms + config_.phaseCathodeOhms;
      const double junction =
          (config_.phaseTailOhms * z[2] + config_.phaseCathodeOhms * tail) /
          totalR;
      const double va = sourceGuessA =
          gridSourceVoltage(junction + input - z[2], &cachedSourceA, sourceGuessA);
      const double vb = sourceGuessB =
          gridSourceVoltage(junction - feedback - z[2], &cachedSourceB, sourceGuessB);
      const double ia = tube(va, z[0] - z[2], cachedDa);
      const double ib = tube(vb, z[1] - z[2], cachedDb);
      const double gridA = cachedGridA = coupledGrid(z[0], va0_, ga_, dt);
      const double gridB = cachedGridB = coupledGrid(z[1], vb0_, gb_, dt);
      const double ca =
          config_.outputCouplingFarads / dt * (z[0] - va0_ - gridA + ga_);
      const double cb =
          config_.outputCouplingFarads / dt * (z[1] - vb0_ - gridB + gb_);
      f[0] = preamp_ - z[0] - config_.phasePlateAOhms * (ia + ca);
      f[1] = preamp_ - z[1] - config_.phasePlateBOhms * (ib + cb);
      f[2] = z[2] - tail -
             totalR * (ia + ib + cachedSourceA.current + cachedSourceB.current);
    };
    double f[3];
    residual(x, f);
    for (int n = 0; n < 100; ++n) {
      double norm = std::max({std::abs(f[0]), std::abs(f[1]), std::abs(f[2])});
      if (norm < 0.00001) {
        a = x[0];
        b = x[1];
        cathode_ = x[2];
        return true;
      }
      // Analytic derivatives of the tube law and each companion current.
      const double totalR = config_.phaseTailOhms + config_.phaseCathodeOhms;
      auto capSlope = [&](double grid) noexcept {
        const double c = config_.outputCouplingFarads,
                     leak = config_.outputGridLeakOhms;
        const double den =
            1 + dt / (c * leak) + dt / c * PowerDevices::gridDerivative(grid);
        return c / dt * (1 - 1 / den);
      };
      const auto dA = cachedDa;
      const auto dB = cachedDb;
      const double dpA = dA.plate, dgA = dA.grid, dpB = dB.plate, dgB = dB.grid;
      const double ratio = config_.phaseCathodeOhms / totalR;
      const double ka = dpA + ratio * (1 / (1 + 68000.0 * cachedSourceA.slope)) * dgA;
      const double kb = dpB + ratio * (1 / (1 + 68000.0 * cachedSourceB.slope)) * dgB;
      const double gridK = ratio * ((1 / (1 + 68000.0 * cachedSourceA.slope)) * cachedSourceA.slope +
                                    (1 / (1 + 68000.0 * cachedSourceB.slope)) * cachedSourceB.slope);
      double m[3][4] = {
          {-1 - config_.phasePlateAOhms * (dpA + capSlope(cachedGridA)), 0,
           config_.phasePlateAOhms * ka, 0},
          {0, -1 - config_.phasePlateBOhms * (dpB + capSlope(cachedGridB)),
           config_.phasePlateBOhms * kb, 0},
          {-totalR * dpA, -totalR * dpB, 1 + totalR * (ka + kb + gridK), 0}};
      // The PI Jacobian is arrow-shaped: each plate couples only to
      // the common cathode. Eliminate the two plate rows analytically;
      // no dense pivoting/fill is required for this passive diagonal block.
      const double schur = m[2][2] - m[2][0] * m[0][2] / m[0][0]
                                      - m[2][1] * m[1][2] / m[1][1];
      if (!std::isfinite(schur) || std::abs(schur) < 1e-12) break;
      m[2][3] = (-f[2] + m[2][0] * f[0] / m[0][0]
                        + m[2][1] * f[1] / m[1][1]) / schur;
      m[0][3] = (-f[0] - m[0][2] * m[2][3]) / m[0][0];
      m[1][3] = (-f[1] - m[1][2] * m[2][3]) / m[1][1];
      bool accepted = false;
      for (double alpha = 1; alpha >= 0.000000059604644775390625;
           alpha *= 0.5) {
        double z[3], g[3];
        for (int i = 0; i < 3; ++i)
          z[i] = clamp(x[i] + alpha * m[i][3], 0,
                       i == 2 ? 500 : std::max(0.0, preamp_));
        residual(z, g);
        double nn = std::max({std::abs(g[0]), std::abs(g[1]), std::abs(g[2])});
        if (nn < norm) {
          // The accepted line-search evaluation already holds the residual
          // and exact tube/grid Jacobian terms for the next Newton iteration.
          for (int i = 0; i < 3; ++i) {
            x[i] = z[i];
            f[i] = g[i];
          }
          accepted = true;
          break;
        }
      }
      if (!accepted)
        break;
    }
    return false;
  }
  void step(double input, double dt) noexcept {
    // Solve the same-time feedback loop by outer fixed-point iteration. Each
    // trial starts from the previously accepted capacitor/cathode state; no
    // reactive or filter state advances until the loop has converged.
    const double gl = dt / loadL_,
                 gm = 1.0 / (6000.0 + 0.6 / dt + dt / 0.0000066);
    const double lowAlpha = dt / (0.003 + dt), highAlpha = dt / (0.00012 + dt);
    const double oldCathode = cathode_;
    double pa = va0_, pb = vb0_, v = 0, trial = out_, newLow = fbLow_,
           newHigh = fbHigh_;
    double nextGa = ga_, nextGb = gb_;
    double guessCathode = oldCathode;
    bool converged = false;
    double previousTrial = 0, previousError = 0;
    for (int iteration = 0; iteration < 10; ++iteration) {
      newLow = fbLow_ + lowAlpha * (trial - fbLow_);
      newHigh = fbHigh_ + highAlpha * (trial - fbHigh_);
      const double feedback =
          feedback_ * 0.16 *
          (trial - depth_ * 0.7 * newLow - presence_ * 0.7 * (trial - newHigh));
      cathode_ = guessCathode;
      if (!phase(input, feedback, dt, pa, pb)) {

        break;
      }
      guessCathode = cathode_;
      nextGa = coupledGrid(pa, va0_, ga_, dt);
      nextGb = coupledGrid(pb, vb0_, gb_, dt);
      const double factorA = PowerDevices::plateFactor(nextGa, screen_);
      const double factorB = PowerDevices::plateFactor(nextGb, screen_);
      auto residual = [&](double q) noexcept {
        const double a = factorA * std::atan((supply_ - q) / 55.0);
        const double b = factorB * std::atan((supply_ + q) / 55.0);
        return q / loadR_ + il_ + gl * q + gm * (q + 0.6 / dt * im_ - cm_) -
               (a - b);
      };
      double lo = -std::max(1.0, supply_), hi = std::max(1.0, supply_);
      if (residual(lo) > 0 || residual(hi) < 0) {
        break;
      }
      v = clamp(trial * std::sqrt(4000.0 / 8.0) / 2, lo, hi);
      bool loadConverged = false;
      for (int i = 0; i < 20; ++i) {
        const double f = residual(v);
        if (std::abs(f) < 1e-9) {
          loadConverged = true;
          break;
        }
        if (f > 0)
          hi = v;
        else
          lo = v;
        const double x = (supply_ - v) / 55.0, y = (supply_ + v) / 55.0;
        const double slope = 1 / loadR_ + gl + gm +
                             factorA / (55 * (1 + x * x)) +
                             factorB / (55 * (1 + y * y));
        const double next = v - f / slope;
        v = (next > lo && next < hi) ? next : 0.5 * (lo + hi);
      }
      if (!loadConverged) {
        break;
      }
      const double next = 2 * v / std::sqrt(4000.0 / 8.0);
      kcl_ = residual(v);

      if (std::abs(next - trial) < 0.0001) {
        trial = next;
        converged = true;
        break;
      }
      const double error = next - trial;
      double nextTrial = next;
      if (iteration > 0 && std::abs(error - previousError) > 1e-9)
        nextTrial =
            trial - error * (trial - previousTrial) / (error - previousError);
      previousTrial = trial;
      previousError = error;
      trial = clamp(nextTrial, -45, 45);
    }
    if (!converged) {

      cathode_ = oldCathode;
      ++failures_;
      return;
    }
    fbLow_ = fbLow_ + lowAlpha * (trial - fbLow_);
    fbHigh_ = fbHigh_ + highAlpha * (trial - fbHigh_);
    ga_ = nextGa;
    gb_ = nextGb;
    va0_ = pa;
    vb0_ = pb;
    plateA_ = supply_ - v;
    plateB_ = supply_ + v;
    il_ += gl * v;
    im_ = gm * (v + 0.6 / dt * im_ - cm_);
    cm_ += dt / 0.0000066 * im_;
    ia_ = PowerDevices::plate(ga_, supply_ - v, screen_);
    ib_ = PowerDevices::plate(gb_, supply_ + v, screen_);
    ig_ = PowerDevices::grid(ga_) + PowerDevices::grid(gb_);
    out_ = 2 * v / std::sqrt(4000.0 / 8.0);
    // Screen current uses the plate currents just evaluated at this state.
    const double is = ia_ * (0.06 + 0.16 * std::exp(-std::max(0.0, supply_ - v) / 60.0)) +
                      ib_ * (0.06 + 0.16 * std::exp(-std::max(0.0, supply_ + v) / 60.0));
    // Backward-Euler nodal KCL of the three coupled supply capacitors.
    // Branch currents flow through reservoir->screen->preamp resistors.
    const double g0 = 1 / config_.reservoirResistanceOhms;
    const double g1 = 1 / config_.screenResistanceOhms;
    const double g2 = 1 / config_.preampResistanceOhms;
    const double c0 = config_.reservoirCapacitanceFarads / dt;
    const double c1 = config_.screenCapacitanceFarads / dt;
    const double c2 = config_.preampCapacitanceFarads / dt;
    const double piCurrent = (preamp_ - pa) / config_.phasePlateAOhms +
                             (preamp_ - pb) / config_.phasePlateBOhms;
    double d0 = c0 + g0 + g1, d1 = c1 + g1 + g2, d2 = c2 + g2;
    double r0 = c0 * supply_ + g0 * config_.reservoirSourceVolts - ia_ - ib_;
    double r1 = c1 * screen_ - is;
    double r2 = c2 * preamp_ - preampLoad_ - piCurrent;
    const double f1 = -g1 / d0;
    d1 -= f1 * (-g1);
    r1 -= f1 * r0;
    const double f2 = -g2 / d1;
    d2 -= f2 * (-g2);
    r2 -= f2 * r1;
    const double nextPreamp = r2 / d2;
    const double nextScreen = (r1 + g2 * nextPreamp) / d1;
    const double nextSupply = (r0 + g1 * nextScreen) / d0;
    supply_ = nextSupply;
    screen_ = nextScreen;
    preamp_ = nextPreamp;
  }

public:
  static double phaseGridSourceVoltage(double raw) noexcept {
    return gridSourceVoltage(raw);
  }
  void prepare(double sr) noexcept {
    fs_ = clamp(std::isfinite(sr) ? sr : 48000, 8000, 384000);
    reset();
  }
  // Solve the zero-input DC operating point with open coupling capacitors.
  // The ideal transformer shunts differential DC, so both plate voltages equal
  // the reservoir at idle. No simulation-length warmup runs in an audio call.
  void reset() noexcept {
    supply_ = 450;
    screen_ = 400;
    preamp_ = 300;
    ga_ = gb_ = config_.outputBiasVolts;
    il_ = im_ = cm_ = fbLow_ = fbHigh_ = out_ = ia_ = ib_ = ig_ = 0;
    failures_ = inputClamps_ = 0;
    va0_ = vb0_ = 200;
    cathode_ = 40;
    bool settled = false;
    for (int i = 0; i < 200; ++i) {
      double a = va0_, b = vb0_;
      if (!phase(0, 0, 1e6, a, b)) {
        ++failures_;
        return;
      }
      va0_ = a;
      vb0_ = b;
      const double plate = PowerDevices::plate(ga_, supply_, screen_);
      const double screen = PowerDevices::screen(ga_, supply_, screen_);
      const double pi = (preamp_ - a) / config_.phasePlateAOhms +
                        (preamp_ - b) / config_.phasePlateBOhms;
      const double nextSupply = config_.reservoirSourceVolts -
                                config_.reservoirResistanceOhms *
                                    (2 * plate + 2 * screen + pi + preampLoad_);
      const double nextScreen =
          nextSupply -
          config_.screenResistanceOhms * (2 * screen + pi + preampLoad_);
      const double nextPreamp =
          nextScreen - config_.preampResistanceOhms * (pi + preampLoad_);
      const double error = std::max({std::abs(nextSupply - supply_),
                                     std::abs(nextScreen - screen_),
                                     std::abs(nextPreamp - preamp_)});
      supply_ += 0.25 * (nextSupply - supply_);
      screen_ += 0.25 * (nextScreen - screen_);
      preamp_ += 0.25 * (nextPreamp - preamp_);
      if (error < 1e-5) {
        settled = true;
        break;
      }
    }
    if (!settled)
      ++failures_;
    plateA_ = plateB_ = supply_;
    ia_ = ib_ = PowerDevices::plate(ga_, supply_, screen_);
    ig_ = 2 * PowerDevices::grid(ga_);
    kcl_ = 0;
  }
  double process(double inputVolts) noexcept {
    if (!std::isfinite(inputVolts)) {
      ++failures_;
      inputVolts = 0;
    }
    if (std::abs(inputVolts) > 10000) {
      ++inputClamps_;
      inputVolts = clamp(inputVolts, -10000, 10000);
    }
    PowerCircuit previous = *this;
    int n = std::max(1, static_cast<int>(std::ceil(96000.0 / fs_)));
    double dt = 1.0 / (fs_ * n);
    for (int i = 0; i < n; ++i)
      step(inputVolts, dt);
    if (!std::isfinite(out_)) {
      auto count = failures_ + 1;
      *this = previous;
      failures_ = count;
      return out_;
    }
    return out_;
  }
  // Preserve destination configuration while transferring circuit history.
  void adoptAudioStateFrom(const PowerCircuit &source) noexcept {
    const auto config = config_;
    const double fs = fs_, feedback = feedback_, presence = presence_;
    const double depth = depth_, loadR = loadR_, loadL = loadL_;
    const double preampLoad = preampLoad_;
    *this = source;
    config_ = config;
    fs_ = fs;
    feedback_ = feedback;
    presence_ = presence;
    depth_ = depth;
    loadR_ = loadR;
    loadL_ = loadL;
    preampLoad_ = preampLoad;
  }
  void setControls(double feedback, double presence, double depth) noexcept {
    feedback_ = clamp(std::isfinite(feedback) ? feedback : 0, 0, 1);
    presence_ = clamp(std::isfinite(presence) ? presence : 0, 0, 1);
    depth_ = clamp(std::isfinite(depth) ? depth : 0, 0, 1);
  }
  void setConfig(const PowerConfig &c) noexcept {
    auto sane = [](double v, double fallback, double lo, double hi) {
      return std::isfinite(v) ? clamp(v, lo, hi) : fallback;
    };
    config_.reservoirSourceVolts = sane(c.reservoirSourceVolts, 475, 100, 600);
    config_.reservoirResistanceOhms =
        sane(c.reservoirResistanceOhms, 200, 1, 2000);
    config_.reservoirCapacitanceFarads =
        sane(c.reservoirCapacitanceFarads, 100e-6, 1e-6, 0.01);
    config_.screenResistanceOhms = sane(c.screenResistanceOhms, 1000, 1, 10000);
    config_.screenCapacitanceFarads =
        sane(c.screenCapacitanceFarads, 22e-6, 1e-6, 0.01);
    config_.preampResistanceOhms =
        sane(c.preampResistanceOhms, 47000, 100, 200000);
    config_.preampCapacitanceFarads =
        sane(c.preampCapacitanceFarads, 22e-6, 1e-6, 0.01);
    config_.phasePlateAOhms = sane(c.phasePlateAOhms, 82000, 1000, 500000);
    config_.phasePlateBOhms = sane(c.phasePlateBOhms, 100000, 1000, 500000);
    config_.phaseCathodeOhms = sane(c.phaseCathodeOhms, 470, 10, 10000);
    config_.phaseTailOhms = sane(c.phaseTailOhms, 10000, 100, 100000);
    config_.phaseTailReferenceVolts =
        sane(c.phaseTailReferenceVolts, 13.5, 0, 80);
    config_.outputGridLeakOhms =
        sane(c.outputGridLeakOhms, 150000, 10000, 1000000);
    config_.outputCouplingFarads =
        sane(c.outputCouplingFarads, 22e-9, 1e-9, 1e-6);
    config_.outputBiasVolts = sane(c.outputBiasVolts, -37, -100, -5);
  }
  const PowerConfig &config() const noexcept { return config_; }
  void setPreampLoadAmps(double amps) noexcept {
    preampLoad_ = clamp(std::isfinite(amps) ? amps : 0.002, 0.0, 0.02);
  }
  void setLoad(double reflectedHalfPrimaryOhms,
               double inductanceHenries) noexcept {
    loadR_ =
        clamp(std::isfinite(reflectedHalfPrimaryOhms) ? reflectedHalfPrimaryOhms
                                                      : 1000,
              100, 10000);
    loadL_ = clamp(std::isfinite(inductanceHenries) ? inductanceHenries : 1,
                   0.05, 10);
  }
  double phasePlateAVolts() const noexcept { return va0_; }
  double phasePlateBVolts() const noexcept { return vb0_; }
  double phaseCathodeVolts() const noexcept { return cathode_; }
  double outputGridAVolts() const noexcept { return ga_; }
  double outputGridBVolts() const noexcept { return gb_; }
  double screenCurrent() const noexcept {
    return PowerDevices::screen(ga_, plateA_, screen_) +
           PowerDevices::screen(gb_, plateB_, screen_);
  }
  double phaseSupplyCurrent() const noexcept {
    return (preamp_ - va0_) / config_.phasePlateAOhms +
           (preamp_ - vb0_) / config_.phasePlateBOhms;
  }
  double plateAVolts() const noexcept { return plateA_; }
  double plateBVolts() const noexcept { return plateB_; }
  double plateACurrent() const noexcept { return ia_; }
  double plateBCurrent() const noexcept { return ib_; }
  double loadResidualAmps() const noexcept { return kcl_; }
  double supplyVolts() const noexcept { return supply_; }
  double preampSupplyVolts() const noexcept { return preamp_; }
  double screenVolts() const noexcept { return screen_; }
  double outputCurrent() const noexcept { return ia_ + ib_; }
  double gridCurrent() const noexcept { return ig_; }
  std::uint64_t failureCount() const noexcept { return failures_; }
  std::uint64_t inputClampCount() const noexcept { return inputClamps_; }
};
} // namespace rig::circuit
