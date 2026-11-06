#include "synth.h"

#include <algorithm>
#include <cmath>

namespace synth {

namespace {
constexpr double kPi = 3.14159265358979323846;

double wrap(double s, double L) {
  s = std::fmod(s, L);
  return s < 0.0 ? s + L : s;
}
}  // namespace

double Rng::uniform() {
  s_ ^= s_ >> 12;
  s_ ^= s_ << 25;
  s_ ^= s_ >> 27;
  const uint64_t v = s_ * 2685821657736338717ULL;
  return static_cast<double>(v >> 11) * (1.0 / 9007199254740992.0);
}

double Rng::normal() {
  if (haveSpare_) {
    haveSpare_ = false;
    return spare_;
  }
  double u1 = 0.0;
  do {
    u1 = uniform();
  } while (u1 <= 0.0);
  const double u2 = uniform();
  const double r = std::sqrt(-2.0 * std::log(u1));
  spare_ = r * std::sin(2.0 * kPi * u2);
  haveSpare_ = true;
  return r * std::cos(2.0 * kPi * u2);
}

double Track::length() const { return 2.0 * straight + 2.0 * kPi * radius; }

Pt Track::at(double s) const {
  const double S = straight;
  const double R = radius;
  s = wrap(s, length());
  if (s < S) return {s - S / 2.0, -R};
  s -= S;
  if (s < kPi * R) {
    const double a = -kPi / 2.0 + s / R;
    return {S / 2.0 + R * std::cos(a), R * std::sin(a)};
  }
  s -= kPi * R;
  if (s < S) return {S / 2.0 - s, R};
  s -= S;
  const double a = kPi / 2.0 + s / R;
  return {-S / 2.0 + R * std::cos(a), R * std::sin(a)};
}

double Track::headingAt(double s) const {
  const Pt a = at(s - 0.05);
  const Pt b = at(s + 0.05);
  double h = std::atan2(b.x - a.x, b.y - a.y) * 180.0 / kPi;
  if (h < 0.0) h += 360.0;
  return h;
}

bool Track::inCorner(double s) const {
  s = wrap(s, length());
  const double c1 = straight;
  const double c2 = 2.0 * straight + kPi * radius;
  return (s >= c1 && s < c1 + kPi * radius) || s >= c2;
}

double Track::speedAt(double s) const {
  if (inCorner(s)) return cornerSpeed;
  double m = wrap(s, length());
  if (m >= straight) m -= straight + kPi * radius;  // second straight
  return straightLo + (straightHi - straightLo) * std::sin(kPi * m / straight);
}

double Run::distAt(double t) const {
  if (t <= 0.0) return S.front();
  const double x = t / dt;
  const size_t i = static_cast<size_t>(x);
  if (i + 1 >= S.size()) return S.back();
  const double f = x - static_cast<double>(i);
  return S[i] + f * (S[i + 1] - S[i]);
}

double Run::timeAt(double s) const {
  const auto it = std::lower_bound(S.begin(), S.end(), s);
  if (it == S.begin()) return 0.0;
  if (it == S.end()) return static_cast<double>(S.size() - 1) * dt;
  const size_t i = static_cast<size_t>(it - S.begin());
  const double f = (s - S[i - 1]) / (S[i] - S[i - 1]);
  return (static_cast<double>(i - 1) + f) * dt;
}

int64_t Run::timeAtUs(double s) const { return t0Us + std::llround(timeAt(s) * 1e6); }

Run makeRun(const Track& tr, const lt::Plane& pl, const RunConfig& cfg) {
  Run r;
  r.sfS = cfg.sfS;
  r.L = tr.length();
  r.t0Us = cfg.t0Us;
  Rng rng(cfg.seed);

  // Lap k (between S/F passes k-1 and k) is driven with scale[k]; scale[0] before pass 0.
  std::vector<double> scale(static_cast<size_t>(cfg.laps) + 3);
  for (double& k : scale) k = cfg.lapScaleMin + (cfg.lapScaleMax - cfg.lapScaleMin) * rng.uniform();

  const double endS = r.passS(cfg.laps) + 30.0;
  double s = 0.0;
  r.S.push_back(s);
  while (s < endS) {
    const size_t lap = s < cfg.sfS ? 0 : static_cast<size_t>((s - cfg.sfS) / r.L) + 1;
    s += tr.speedAt(s) / scale[std::min(lap, scale.size() - 1)] * r.dt;
    r.S.push_back(s);
  }

  const double step = 1.0 / cfg.rateHz;
  const double phase = rng.uniform() * step;
  const double rho = std::exp(-step / 0.4);
  const double drive = cfg.noiseM * std::sqrt(1.0 - rho * rho);
  double ex = cfg.noiseM * rng.normal();
  double ey = cfg.noiseM * rng.normal();
  const double tEnd = static_cast<double>(r.S.size() - 2) * r.dt;
  for (int k = 0;; ++k) {
    const double ts = phase + k * step;
    if (ts >= tEnd) break;
    if (k > 0) {
      ex = rho * ex + drive * rng.normal();
      ey = rho * ey + drive * rng.normal();
    }
    const double sTrue = r.distAt(ts);
    const Pt p = tr.at(sTrue);
    lt::Fix f{};
    pl.toGlobal({static_cast<float>(p.x + ex), static_cast<float>(p.y + ey)}, &f.latE7, &f.lonE7);
    f.tUs = cfg.t0Us + std::llround(ts * 1e6);
    f.speedMps = static_cast<float>((r.distAt(ts + 0.0005) - r.distAt(ts - 0.0005)) / 0.001);
    f.valid = true;
    r.fixes.push_back(f);
    r.fixS.push_back(sTrue);
  }
  return r;
}

lt::GateDef gateAt(const Track& tr, const lt::Plane& pl, double s, float widthM) {
  const Pt p = tr.at(s);
  lt::GateDef g{};
  pl.toGlobal({static_cast<float>(p.x), static_cast<float>(p.y)}, &g.latE7, &g.lonE7);
  g.headingDeg = static_cast<float>(tr.headingAt(s));
  g.widthM = widthM;
  return g;
}

}  // namespace synth
