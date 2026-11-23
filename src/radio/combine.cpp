// Two-antenna video (maximal-ratio combining of path A and path B): see combine.h.
#include "combine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace arx {

namespace {
// The time offset between the paths starts at none: see combine.h for why not wifi-sdr's 146 ns.
// Measuring the offset: kSlices stretches of kSlice samples, one from the start of each process() (a bank from
// the radio: 13 ms in all at 40 MS/s, 26 ms at 20 MS/s, so most of a video field or more and every part of the
// picture gets a say), again every kLagEvery s.
constexpr size_t kSlice = Combiner::kBlock;
constexpr int kSlices = 64;
constexpr double kLagEvery = 0.5;
// A measurement counts when the paths are this coherent at the best lag (squared: 0.2 as an amplitude; two
// unrelated noises give about 0.001) ...
constexpr double kLagMinCoh2 = 0.04;
// ... and no lag kLagApart or more away from it comes within this of it. Video with its colour on a flat area
// repeats every subcarrier cycle (9 to 11 samples at 40 MS/s, half that at 20), and a bare carrier correlates
// the same at every lag: neither says which lag is the right one.
constexpr int kLagApart = 4;
constexpr double kLagClear = 0.8;
// Sums over a block are kept as this many partial sums side by side, one per position in a group of samples: the
// compiler can then do a group at a time without being allowed to reorder float additions (-ffast-math).
constexpr size_t kLanes = 8;

float sum(const float *s) {
  float t = 0;
  for (size_t j = 0; j < kLanes; j++) t += s[j];
  return t;
}
}  // namespace

Combiner::Combiner(double fs) : _fs(fs), _lag(0) {}

void Combiner::cut() { _cut = true; }

void Combiner::process(cf *a, const cf *b, size_t n) {
  if (n == 0) return;
  if (_cut) {
    // Nothing to delay the leading path with for the first few samples.
    //  - Where nothing has been combined yet (the start, reset()): its first sample held. Not zeros: the carrier
    //    has a constant envelope, and a step up from nothing reads as noise on that path (on a strong signal it
    //    lost a third of its weight for the first 10 ms). The weights start at A alone and come in over the
    //    first blocks, so the held sample itself, whose phase is wrong, carries next to no weight.
    //  - After a cut() on a running combiner: zeros. The weights are already at their values there, and a held
    //    sample would be added to the other path at full weight and the wrong phase (the carrier turns by up to
    //    36 degrees a sample at 40 MS/s, twice that at 20): the sum collapses, a click the demodulator turns
    //    into a damaged line. With zeros those few samples are the other path alone at its own weight and the
    //    right phase, and the estimates do not notice a step that short.
    std::fill(std::begin(_tail_a), std::end(_tail_a), _init ? cf{} : a[0]);
    std::fill(std::begin(_tail_b), std::end(_tail_b), _init ? cf{} : b[0]);
    _cut = false;
  }
  measure_lag(a, b, n);
  // the last kMaxLag samples of each path up to the end of this call, as they came in (a is overwritten below)
  cf ta[kMaxLag], tb[kMaxLag];
  for (size_t j = 0; j < static_cast<size_t>(kMaxLag); j++) {
    const size_t back = kMaxLag - j;  // this many samples before the end
    ta[j] = back <= n ? a[n - back] : _tail_a[j + n];
    tb[j] = back <= n ? b[n - back] : _tail_b[j + n];
  }
  const size_t m0 = std::min(n, kBlock);
  if (_lag > 0) {
    // B is early: combine A with B from _lag samples back. The first block's B starts with the end of the last
    // call's; the later blocks read straight from b, _lag samples behind A.
    const size_t lag = _lag, old = std::min(lag, m0);
    cf head[kBlock];
    std::copy(_tail_b + kMaxLag - lag, _tail_b + kMaxLag - lag + old, head);
    std::copy(b, b + (m0 - old), head + old);
    block(a, head, m0);
    for (size_t at = m0; at < n; at += kBlock) block(a + at, b + at - lag, std::min(n - at, kBlock));
  } else {
    if (_lag < 0) {
      // A is early (measured at 20 MS/s on a 0bda:b812 adapter: -1): delay A itself, in place
      const size_t lag = -_lag, old = std::min(lag, n);
      std::memmove(static_cast<void *>(a + old), a, (n - old) * sizeof(cf));
      std::copy(_tail_a + kMaxLag - lag, _tail_a + kMaxLag - lag + old, a);
    }
    for (size_t at = 0; at < n; at += kBlock) block(a + at, b + at, std::min(n - at, kBlock));
  }
  std::copy(ta, ta + kMaxLag, _tail_a);
  std::copy(tb, tb + kMaxLag, _tail_b);
}

// One more slice of the measurement of the time offset between the paths; after the last, the verdict.
void Combiner::measure_lag(const cf *a, const cf *b, size_t n) {
  if (_lag_wait > 0) {
    _lag_wait -= static_cast<int64_t>(n);
    return;
  }
  constexpr size_t kSpan = kSlice + 2 * kMaxLag;  // of A, so that every lag has kSlice samples of B to meet
  if (n < kSpan) return;
  // Each path without its mean over the slice: the adapter's DC line is on both and would correlate at every lag.
  float xr[kSpan], xi[kSpan], yr[kSlice], yi[kSlice];
  cf ma{}, mb{};
  for (size_t i = 0; i < kSpan; i++) ma += a[i];
  for (size_t i = 0; i < kSlice; i++) mb += b[kMaxLag + i];
  ma /= static_cast<float>(kSpan);
  mb /= static_cast<float>(kSlice);
  for (size_t i = 0; i < kSpan; i++) xr[i] = a[i].real() - ma.real(), xi[i] = a[i].imag() - ma.imag();
  float pa = 0, pb = 0;
  for (size_t i = 0; i < kSlice; i++) {
    yr[i] = b[kMaxLag + i].real() - mb.real(), yi[i] = b[kMaxLag + i].imag() - mb.imag();
    pa += xr[kMaxLag + i] * xr[kMaxLag + i] + xi[kMaxLag + i] * xi[kMaxLag + i];
    pb += yr[i] * yr[i] + yi[i] * yi[i];
  }
  // sum of a[i + lag] * conj(b[i]) for every lag at once: where B leads A by L samples it peaks at lag L
  float cr[kLags] = {}, ci[kLags] = {};
  for (size_t i = 0; i < kSlice; i++) {
    const float br = yr[i], bi = yi[i];
    const float *ar = xr + i, *ai = xi + i;  // lag k - kMaxLag is at index k
    for (int k = 0; k < kLags; k++) {
      cr[k] += ar[k] * br + ai[k] * bi;
      ci[k] += ai[k] * br - ar[k] * bi;
    }
  }
  // Slices add up as powers: the phase between the antennas may turn from one slice to the next (a moving
  // transmitter), the lag does not.
  for (int k = 0; k < kLags; k++) _q[k] += static_cast<double>(cr[k]) * cr[k] + static_cast<double>(ci[k]) * ci[k];
  _qp += static_cast<double>(pa) * pb;
  if (++_slices < kSlices) return;
  pick_lag();
  std::fill(std::begin(_q), std::end(_q), 0.0);
  _qp = 0;
  _slices = 0;
  _lag_wait = static_cast<int64_t>(kLagEvery * _fs);
}

void Combiner::pick_lag() {
  const int best = static_cast<int>(std::max_element(_q, _q + kLags) - _q);
  // Not at the end of the range (the true peak may lie beyond it), on a signal both paths have, and alone.
  bool clear = best > 0 && best < kLags - 1 && _q[best] > kLagMinCoh2 * _qp;
  for (int k = 0; k < kLags && clear; k++)
    if (std::abs(k - best) >= kLagApart && _q[k] > kLagClear * _q[best]) clear = false;
  if (!clear) return;  // nothing to go by this time: the offset stays what it was
  int lag = best - kMaxLag;
  // A true offset half way between two samples would have the two take turns: stay with the one in use while it
  // is next to the best and about as good.
  if (std::abs(lag - _lag) == 1 && _q[_lag + kMaxLag] > 0.95 * _q[best]) lag = _lag;
  _lag = lag;
  _lag_measured = true;
}

void Combiner::block(cf *a, const cf *b, size_t n) {
  if (n == 0) return;
  // The samples as plain floats (real, imaginary, real, ...), and every sum over the block in float, made double
  // once per block: this runs on every sample of both paths.
  float *x = reinterpret_cast<float *>(a);
  const float *y = reinterpret_cast<const float *>(b);
  const size_t whole = n - n % kLanes;
  // DC (the adapter's DC line), smoothed across blocks like the demodulator does
  {
    float sar[kLanes] = {}, sai[kLanes] = {}, sbr[kLanes] = {}, sbi[kLanes] = {};
    for (size_t i = 0; i < whole; i += kLanes)
      for (size_t j = 0; j < kLanes; j++) {
        sar[j] += x[2 * (i + j)];
        sai[j] += x[2 * (i + j) + 1];
        sbr[j] += y[2 * (i + j)];
        sbi[j] += y[2 * (i + j) + 1];
      }
    for (size_t i = whole; i < n; i++)
      sar[0] += x[2 * i], sai[0] += x[2 * i + 1], sbr[0] += y[2 * i], sbi[0] += y[2 * i + 1];
    const float mar = sum(sar) / n, mai = sum(sai) / n, mbr = sum(sbr) / n, mbi = sum(sbi) / n;
    if (!_init) _dcar = mar, _dcai = mai, _dcbr = mbr, _dcbi = mbi;
    _dcar += (mar - _dcar) * 0.3f;
    _dcai += (mai - _dcai) * 0.3f;
    _dcbr += (mbr - _dcbr) * 0.3f;
    _dcbi += (mbi - _dcbi) * 0.3f;
  }
  const float dcar = _dcar, dcai = _dcai, dcbr = _dcbr, dcbi = _dcbi;
  // moments and the cross-correlation
  double m2a, m4a, m2b, m4b, cr, ci;
  {
    float s2a[kLanes] = {}, s4a[kLanes] = {}, s2b[kLanes] = {}, s4b[kLanes] = {}, scr[kLanes] = {}, sci[kLanes] = {};
    const auto add = [&](size_t i, size_t j) {
      const float xr = x[2 * i] - dcar, xi = x[2 * i + 1] - dcai;
      const float yr = y[2 * i] - dcbr, yi = y[2 * i + 1] - dcbi;
      const float pa = xr * xr + xi * xi, pb = yr * yr + yi * yi;
      s2a[j] += pa;
      s4a[j] += pa * pa;
      s2b[j] += pb;
      s4b[j] += pb * pb;
      scr[j] += xr * yr + xi * yi;  // a * conj(b)
      sci[j] += xi * yr - xr * yi;
    };
    for (size_t i = 0; i < whole; i += kLanes)
      for (size_t j = 0; j < kLanes; j++) add(i + j, j);
    for (size_t i = whole; i < n; i++) add(i, 0);
    m2a = sum(s2a), m4a = sum(s4a), m2b = sum(s2b), m4b = sum(s4b), cr = sum(scr), ci = sum(sci);
  }
  m2a /= n, m4a /= n, m2b /= n, m4b /= n, cr /= n, ci /= n;
  const double k = _init ? 0.3 : 1.0;  // ~3 blocks (~150 us at 20 MS/s): far quicker than a fade
  _m2a += (m2a - _m2a) * k;
  _m4a += (m4a - _m4a) * k;
  _m2b += (m2b - _m2b) * k;
  _m4b += (m4b - _m4b) * k;
  _c += (std::complex<double>(cr, ci) - _c) * k;
  // signal and noise of each path (M2M4); noise floored at the 12-bit quantisation and 40 dB SNR
  auto split = [](double m2, double m4, double &s, double &nz) {
    s = std::sqrt(std::max(0.0, 2 * m2 * m2 - m4));
    s = std::min(s, m2);
    nz = std::max({m2 - s, 1e-4 * m2, 0.5});
  };
  double sa, na, sb, nb;
  split(_m2a, _m4a, sa, na);
  split(_m2b, _m4b, sb, nb);
  // the receiver noise hardly changes at a fixed gain, while each block's estimate of it is jumpy when the signal is
  // strong (a small difference of two big numbers): average it over ~5 ms; the signal itself is tracked fast
  const double kn = _init ? 0.01 : 1.0;
  _noise_a += (na - _noise_a) * kn;
  _noise_b += (nb - _noise_b) * kn;
  na = _noise_a;
  nb = _noise_b;
  _init = true;
  const double ca = std::abs(_c);
  const std::complex<double> rot = ca > 0 ? _c / ca : std::complex<double>(1, 0);  // turns B into step with A
  double wa = std::sqrt(sa) / na, wb = std::sqrt(sb) / nb;
  if (wa + wb <= 0) wa = 1, wb = 0;  // nothing on either: A as it is
  const double tot = wa + wb;
  const float wa_new = static_cast<float>(wa / tot);
  const float wbr_new = static_cast<float>(wb / tot * rot.real()), wbi_new = static_cast<float>(wb / tot * rot.imag());
  // combine, the weights moving linearly from the last block's to this one's
  {
    const float wa0 = _wa, wbr0 = _wbr, wbi0 = _wbi;
    const float dwa = (wa_new - wa0) / n, dwbr = (wbr_new - wbr0) / n, dwbi = (wbi_new - wbi0) / n;
    const int m = static_cast<int>(n);
    for (int i = 0; i < m; i++) {
      const float t = static_cast<float>(i + 1);
      const float ua = wa0 + t * dwa, ubr = wbr0 + t * dwbr, ubi = wbi0 + t * dwbi;
      const float xr = x[2 * i] - dcar, xi = x[2 * i + 1] - dcai;
      const float yr = y[2 * i] - dcbr, yi = y[2 * i + 1] - dcbi;
      x[2 * i] = ua * xr + ubr * yr - ubi * yi;  // ua * x + ub * y
      x[2 * i + 1] = ua * xi + ubr * yi + ubi * yr;
    }
  }
  _wa = wa_new;
  _wbr = wbr_new;
  _wbi = wbi_new;
  // stats
  const double snra = sa / na, snrb = sb / nb, snrc = snra + snrb;
  _sa += snra;
  _sb += snrb;
  _sc += snrc;
  _share += snrc > 0 ? snrb / snrc : 0;
  _coh += std::sqrt(_m2a * _m2b) > 0 ? ca / std::sqrt(_m2a * _m2b) : 0;
  const double thr = std::pow(10.0, kFadeDb / 10);
  if (snra < thr) {
    _fade++;
    if (snrc >= thr) _resc++;
  }
  _nb++;
}

Combiner::Stats Combiner::take_stats() {
  Stats s;
  s.lag = _lag;
  s.lag_measured = _lag_measured;
  if (_nb == 0) return s;
  auto db = [](double v) { return static_cast<float>(10 * std::log10(std::max(v, 1e-10))); };
  s.snr_a_db = db(_sa / _nb);
  s.snr_b_db = db(_sb / _nb);
  s.snr_c_db = db(_sc / _nb);
  s.b_share = static_cast<float>(_share / _nb);
  s.coherence = static_cast<float>(_coh / _nb);
  s.a_fade = static_cast<float>(static_cast<double>(_fade) / _nb);
  s.rescued = _fade ? static_cast<float>(static_cast<double>(_resc) / _fade) : 0.f;
  _sa = _sb = _sc = _share = _coh = 0;
  _nb = _fade = _resc = 0;
  return s;
}

}  // namespace arx
