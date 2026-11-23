// Two-antenna video: path A and path B of the 8812BU combined into one stream before FM demodulation
// (maximal-ratio combining). (From Auburn/fpvDROID, which took this from here and changed it: the two paths are
// lined up in time first, a hole in the stream does not carry samples across it, and the per-sample loops are
// plain float arithmetic.)
//
// One chip and one clock, but the two paths need not come out of it in step: wifi-sdr FINDINGS.md ("8822B
// dual-path tap") has path B leading path A by ~146 ns (6 samples at 40 MS/s), from Wi-Fi frames. A time offset
// is a phase that grows with frequency, which the one phase per block below cannot undo: added as they come, the
// paths would cancel 3.4 MHz either side of the carrier, where the colour is. So the leading path is delayed by a
// whole number of samples first. That number is measured from the paths' own correlation at each lag (once at
// the start, then again every half second), and used when one lag stands out clearly. Until then, and whenever
// nothing stands out, the paths are taken as they come (no delay, as before this was added): on a Realtek
// 0bda:b812 adapter on video the measurement found 0 samples at 40 MS/s and -1 at 20 MS/s, not 6 and 3, so the
// figure above is no safe guess. Starting from it there, the coherence of the paths fell from 0.97 to 0.75 at
// 40 MS/s, and a picture with little in it (a flat grey screen with an OSD) may never let a lag stand out.
//
// Then, every block of kBlock samples: each path's signal and noise power (the M2M4 estimator: the FM carrier has
// a constant envelope, so  S = sqrt(2 M2^2 - M4),  N = M2 - S), and the phase of B relative to A (from their
// cross-correlation). B is turned into step with A, and each path is weighted by its amplitude / noise
// (sqrt(S) / N), so a faded or missing antenna drops out by itself and two good ones add up (~3 dB more SNR,
// and far fewer of the deep fades that make sparkles). Weights move smoothly from block to block.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>

namespace arx {

using cf = std::complex<float>;

class Combiner {
public:
  static constexpr size_t kBlock = 1024;
  static constexpr int kMaxLag = 12;  // the largest time offset between the paths looked for, in samples

  // fs: the stream's sample rate (the paths are combined without a time offset until one has been measured).
  explicit Combiner(double fs = 40e6);
  // Start afresh: a new carrier, another sample rate, or combining switched on.
  void reset(double fs) { *this = Combiner(fs); }
  // The samples of the next process() do not follow on from the last one's (banks lost in between, or the stream
  // restarted): what was kept of the last ones for the delay is dropped. Everything else stays.
  void cut();
  // a: path A, replaced by the combined stream; b: path B (DSP thread only, like take_stats). Call it with a
  // bank at a time: the time offset is measured on the start of each call, and only on calls of at least
  // kBlock + 2 * kMaxLag samples.
  void process(cf *a, const cf *b, size_t n);  // in blocks of kBlock (a shorter last block is fine)

  struct Stats {
    float snr_a_db = -99, snr_b_db = -99, snr_c_db = -99;  // per path and combined (sum), wideband, dB
    float b_share = 0;      // B's part of the combined signal-to-noise (0 = A only, 0.5 = equal, 1 = B only)
    float a_fade = 0;       // fraction of the time A alone was below kFadeDb ...
    float rescued = 0;      // ... of which the combined stream was back above it
    float coherence = 0;    // |correlation| of A and B (1 = the same signal, 0 = no relation)
    int lag = 0;            // samples path B leads path A by, as now compensated (B delayed; negative: A is)
    bool lag_measured = false;  // true: measured on this signal; false: none measured yet (lag is 0)
  };
  // averages since the last call (call ~1/s)
  Stats take_stats();
  static constexpr float kFadeDb = 6;  // wideband SNR below which the picture starts to sparkle (roughly)

private:
  static constexpr int kLags = 2 * kMaxLag + 1;
  void block(cf *a, const cf *b, size_t n);
  void measure_lag(const cf *a, const cf *b, size_t n);
  void pick_lag();
  bool _init = false;
  float _dcar = 0, _dcai = 0, _dcbr = 0, _dcbi = 0;  // each path's DC
  double _m2a = 0, _m4a = 0, _m2b = 0, _m4b = 0;
  std::complex<double> _c{};
  double _noise_a = 0, _noise_b = 0;  // noise power of each path, averaged slowly
  // the weights in effect at the end of the last block (A's is real: B is the one that is turned)
  float _wa = 1, _wbr = 0, _wbi = 0;
  // time offset between the paths
  double _fs;
  int _lag;                    // B leads A by this many samples
  bool _lag_measured = false;
  cf _tail_a[kMaxLag] = {}, _tail_b[kMaxLag] = {};  // each path's last samples of the previous process()
  bool _cut = true;            // there was no previous one to follow on from (the start, or cut())
  double _q[kLags] = {};       // the measurement under way: |correlation|^2 at each lag, summed over its slices,
  double _qp = 0;              // and (power of A) * (power of B) likewise: the two paths' coherence is _q / _qp
  int _slices = 0;
  int64_t _lag_wait = 0;       // samples until the next measurement starts
  // stats
  double _sa = 0, _sb = 0, _sc = 0, _share = 0, _coh = 0;
  size_t _nb = 0, _fade = 0, _resc = 0;
};

}  // namespace arx
