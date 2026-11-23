#include "radio.h"
#include "mmcss.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "log.h"
#include "receiver.h"
#include "spectrum.h"
#include "stream88xx.h"

#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace kp {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {
constexpr double kFs = 40e6;
constexpr double kEdge = 16.5e6;     // usable +- span at 40 MS/s (BW40 DFIR is flat to ~17 MHz)
constexpr double kStep = 20e6;       // a hump up to ~13 MHz wide is fully inside some step's span
constexpr double kCapture = 0.09;    // s of IQ per scan step (60 ms acquisition + ~300 lines)
constexpr double kPsdSpan = 0.012;   // s of it for the spectrum
constexpr int kSettleBanks = 40;     // ~8 ms after a retune (chip AGC)
constexpr int kCapRestarts = 8;      // times a scan step's capture starts over after a gap; then gaps stay in

// Raise this thread's priority (the USB stream thread must never miss a chip deadline; the DSP thread
// must keep up). Best effort: logs what the OS allowed.
void boost_thread(const char *what) {
#ifdef __APPLE__
  // macOS has no per-thread nice: the highest quality-of-service class instead (Apple silicon also keeps such a
  // thread on the performance cores)
  const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  KP_LOGI("radio: %s thread QoS user-interactive: %s", what, rc == 0 ? "ok" : "refused");
#else
  const int tid = static_cast<int>(gettid());
  const int rc = setpriority(PRIO_PROCESS, tid, -10);
  KP_LOGI("radio: %s thread %d nice -10: %s", what, tid, rc == 0 ? "ok" : "refused");
#endif
}

std::vector<double> scan_plan() {
  std::vector<double> p;
  // the common analog bands first (A, B, E, F, R: 5645..5945), then the low band (L: 5362..5621)
  for (double c = 5652e6; c <= 5940e6; c += kStep) p.push_back(c);
  for (double c = 5370e6; c <= 5630e6; c += kStep) p.push_back(c);
  return p;
}
}  // namespace

Radio::Radio(FrameSink frames, LineSink lines) : _frames(std::move(frames)), _lines(std::move(lines)) {}

Radio::~Radio() { close(); }

bool Radio::is_idle() {
  std::lock_guard<std::mutex> l(_m);
  return _mode == Mode::Idle;
}

void Radio::lowlat_thread() {
  thread_local int was = 0;
  const int on = _lowlat.load(std::memory_order_relaxed) ? 1 : 0;
  if (on == was) return;
  was = on;
  arx::mmcss_thread(on);
}

bool Radio::is_open() const {
  std::lock_guard<std::mutex> l(_m);
  return _open;
}

bool Radio::superspeed() const {
  std::lock_guard<std::mutex> l(_m);
  return _superspeed;
}

void Radio::open(int bus, const std::string &port) {
  close();
  auto log = std::make_shared<Logger>();
  auto rx = std::make_unique<wsdr::Receiver>(log);
  wsdr::RxSettings s;
  s.freq = 5769e6;
  s.rate_sel = 1;  // 40 MS/s
  s.wide = true;
  s.agc = true;
  KP_LOGI("radio: opening adapter %s", bus < 0 ? "(first found)" : (std::to_string(bus) + "-" + port).c_str());
  rx->open(wsdr::kPid8812bu, s, bus, port, -1);
  const auto caps = rx->caps();
  KP_LOGI("radio: %s, USB %s, fastest continuous rate %.1f MS/s", caps.chip.c_str(), caps.superspeed ? "3" : "2",
          wsdr::Receiver::rate_of(caps.min_rate_sel) / 1e6);
  if (!caps.superspeed) {
    // 40 MS/s does not stream on USB 2; open at a rate it can, so the stream still comes up for diagnostics
    s.rate_sel = caps.min_rate_sel;
    rx->set(s);
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _superspeed = caps.superspeed;
    _chip = caps.chip;
    _open = true;
    _mode = Mode::Idle;
    _mode_epoch++;
    _req_hz = s.freq;
    _req_rate = s.rate_sel;
  }
  _banks = 0;
  _dropped = 0;
  _stop = false;
  {
    std::lock_guard<std::mutex> l(_rxm);
    _rx = std::move(rx);
  }
  _ddropped = 0;
  _started = false;
  _dsp = std::thread([this] { dsp_loop(); });
  _decoder = std::thread([this] { decode_loop(); });
  _rx->start([this](const uint8_t *b, size_t n, const wsdr::BankMeta &m) {
    on_bank(b, n, m.seq, m.gen, m.fs, m.centre_hz, m.shift_hz);
  });
  _started = true;
}

void Radio::close() {
  _stop = true;
  _qcv.notify_all();
  if (_rx) _rx->stop();
  if (_dsp.joinable()) _dsp.join();
  _dqcv.notify_all();
  if (_decoder.joinable()) _decoder.join();
  {
    std::lock_guard<std::mutex> l(_dqm);
    _dq.clear();
  }
  _fe.reset();
  // out of status_json()'s sight first, then closed: closing can take a while, and a status poll (the UI's
  // thread) must not wait for it
  std::unique_ptr<wsdr::Receiver> rx;
  {
    std::lock_guard<std::mutex> l(_rxm);
    rx = std::move(_rx);
  }
  if (rx) {
    rx->close();
    rx.reset();
    KP_LOGI("radio: closed");
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _open = false;
    _mode = Mode::Idle;
  }
  std::lock_guard<std::mutex> q(_qm);
  _q.clear();
}

// Receiver stream thread: copy only.
void Radio::on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift) {
  thread_local bool boosted = false;
  if (!boosted) {
    boosted = true;
    boost_thread("usb stream");
  }
  lowlat_thread();
  std::vector<uint8_t> v;
  {
    std::lock_guard<std::mutex> l(_qm);
    if (!_pool.empty()) {
      v = std::move(_pool.back());
      _pool.pop_back();
    }
  }
  v.assign(b, b + n);
  {
    std::lock_guard<std::mutex> l(_qm);
    const double t_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // full (100 ms); low latency: also banks over 8 ms old (a backlog: dropped, not shown late; a burst is kept)
    while (!_q.empty() && (_q.size() >= _qcap || (_lowlat && t_s - _q.front().t_s > 0.008))) {
      _pool.push_back(std::move(_q.front().data));
      _q.pop_front();
      _dropped++;
    }
    _q.push_back(Bank{std::move(v), seq, gen, fs, centre, shift, t_s});
  }
  _banks++;
  _qcv.notify_one();
}

// Watching: the hardware centre on the 5 MHz grid whose offset from the pilot is nearest kAim (either side), so
// the video's swing (about +-2.5 MHz around the carrier) stays clear of the chip's DC point and inside the band.
static double offset_centre(double hz, double fs) {
  const double aim = fs >= 40e6 ? 6e6 : 4e6;
  const double c0 = std::round(hz / 5e6) * 5e6;
  double best = c0, err = 1e18;
  for (int k = -3; k <= 3; k++) {
    const double c = c0 + k * 5e6, e = std::fabs(std::fabs(hz - c) - aim);
    if (e < err) err = e, best = c;
  }
  return best;
}

bool Radio::set_offset_tuning(bool on) {
  std::lock_guard<std::mutex> l(_m);
  if (_offset == on) return false;
  _offset = on;
  return true;
}

void Radio::tune(double hz, int rate_sel, bool watching) {
  wsdr::RxSettings s = _rx->settings();
  s.freq = hz;
  {
    std::lock_guard<std::mutex> l(_m);
    s.centre = watching && _offset ? offset_centre(hz, wsdr::Receiver::rate_of(rate_sel)) : 0;
  }
  if (s.centre > 0) KP_LOGI("radio: hardware centre %.0f MHz (pilot %+.1f MHz beside it, off the DC point)", s.centre / 1e6, (hz - s.centre) / 1e6);
  s.rate_sel = rate_sel;
  s.wide = true;
  s.allow_gapped = rate_sel == 0;  // 80 MS/s only gapped (timing only: a wide view of several channels)
  {
    std::lock_guard<std::mutex> l(_m);
    s.agc = _gain_db < 0;
    if (_gain_db >= 0) s.gain_db = _gain_db;
  }
  _rx->set(s);
  std::lock_guard<std::mutex> l(_m);
  _req_hz = hz;
  _req_rate = rate_sel;
}

// The mode calls do nothing on a radio that is not open, and that is decided under the lock open() and close()
// set _open under: a call that races a close can no longer leave the closed radio in its mode.
void Radio::scan() {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _resume_hz = _mode == Mode::Watch ? _watch_hz : 0;
  _mode = Mode::Scan;
  _mode_epoch++;
  // the status says "scan" from here on: its progress with it, not what the scan before left (the DSP thread
  // sets both again when it takes the scan up, and moves the step on only for the scan it is working on)
  _step = 0;
  _plan_n = scan_plan().size();
}

void Radio::time_channels(const std::vector<double> &hz) {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _time_req_hz.assign(hz.begin(), hz.begin() + std::min<size_t>(hz.size(), kMaxTimed));
  _mode = Mode::Timing;
  _mode_epoch++;
}

void Radio::watch(double hz) {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _watch_hz = hz;
  _mode = Mode::Watch;
  _mode_epoch++;
}

void Radio::idle() {
  std::lock_guard<std::mutex> l(_m);
  if (!_open) return;
  _mode = Mode::Idle;
  _mode_epoch++;
}

void Radio::set_watch_rate(int rate_sel) {
  std::lock_guard<std::mutex> l(_m);
  _watch_rate = rate_sel;
}

void Radio::set_gain(double db) {
  std::lock_guard<std::mutex> l(_m);
  _gain_db = db;
}

void Radio::set_combine(bool on) { _combine = on; }


std::vector<Pilot> Radio::pilots() {
  std::lock_guard<std::mutex> l(_m);
  return _pilots;
}

void Radio::unpack(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    out[i] = cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w)));
  }
}

void Radio::unpack_b(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    // path B: I [35:24], Q [47:36] (kDmaBothPaths8822b), sign-extended like la_i / la_q
    out[i] = cf(static_cast<float>(static_cast<int64_t>(w << 28) >> 52), static_cast<float>(static_cast<int64_t>(w << 16) >> 52));
  }
}

void Radio::dsp_loop() {
  boost_thread("dsp");
  Bank b;
  _load_t0_ms = now_ms();
  while (!_stop) {
    bool none;
    {
      std::unique_lock<std::mutex> l(_qm);
      _qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_q.empty() || _stop; });
      if (_stop) break;  // close(): stops the receiver too, which is not a death
      none = _q.empty();
      if (!none) {
        b = std::move(_q.front());
        _q.pop_front();
      }
    }
    if (none) {
      // Outside the queue's lock (the receiver's own lock is never taken while holding it). And only once open()
      // has the receiver started: this thread runs before that call, and a receiver not yet running is not one
      // that stopped.
      if (_started && _rx) {
        const wsdr::RxStatus st = _rx->status();
        if (_stop) break;  // close() got in between: it stopped the receiver itself
        if (!st.running) {
          KP_LOGE("radio: receiver stopped: %s", st.error.c_str());
          break;
        }
      }
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Mode mode;
    uint64_t epoch;
    {
      std::lock_guard<std::mutex> l(_m);
      mode = _mode;
      epoch = _mode_epoch;
    }
    const bool fresh = epoch != _epoch_seen;
    _epoch_seen = epoch;
    if (fresh) {
      // a mode change: set the radio up for it (a new watch builds a new decoder)
      _fe.reset();
      _fe_gen = ~0u;
      if (mode == Mode::Watch) {
        double hz;
        {
          std::lock_guard<std::mutex> l(_m);
          hz = _watch_hz;
        }
        // Watch at 40 MS/s with the chip's AGC. At 20 MS/s (20 MHz channel) the X200 Pro's adapter streams
        // runs of 120 frozen samples (~3900/s with AGC: grey streaks with a coloured tail); fixed gain avoids
        // them only below saturation. 40 MS/s: none, and 60 s with colour decoding: 0 bank-deadline restarts.
        KP_LOGI("radio: watch %.3f MHz", hz / 1e6);
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _watch_rate;
        }
        tune(hz, rate, true);
      } else if (mode == Mode::Scan) {
        _plan = scan_plan();
        _plan_n = _plan.size();
        _at = 0;
        _step = 0;
        _cap.clear();
        _settle = -1;
        KP_LOGI("radio: scan, %zu steps", _plan.size());
        tune(_plan[0], 1);
      } else if (mode == Mode::Timing) {
        {
          std::lock_guard<std::mutex> l(_m);
          _time_hz = _time_req_hz;
          _vstats = DecoderStats{};  // no video in this mode: no stale picture numbers in the status
          _fps = 0;
        }
        if (!_time_hz.empty()) {
          const auto [lo, hi] = std::minmax_element(_time_hz.begin(), _time_hz.end());
          const double span = *hi - *lo;
          _time_centre = (*lo + *hi) / 2;
          _time_rate = _time_hz.size() == 1 ? 2 : span <= 20e6 ? 1 : 0;  // 20 MS/s, 40 MS/s, 80 MS/s gapped
          _time_settle = -1;
          _tl_n = 0;
          _tl_last = 0;
          std::string list;
          for (double h : _time_hz) list += (list.empty() ? "" : ", ") + std::to_string(static_cast<int>(h / 1e6));
          KP_LOGI("radio: timing only, %s MHz at %s", list.c_str(),
                  _time_rate == 2 ? "20 MS/s" : _time_rate == 1 ? "40 MS/s" : "80 MS/s gapped");
          // one channel: beside it (off the DC point) like watch; several: the middle (between the carriers)
          tune(_time_hz.size() == 1 ? _time_hz[0] : _time_centre, _time_rate, _time_hz.size() == 1);
        }
      }
    }
    switch (mode) {
    case Mode::Watch: handle_watch(b); break;
    case Mode::Scan: handle_scan(b); break;
    case Mode::Timing: handle_timing(b); break;
    case Mode::Idle: break;
    }
    {
      std::lock_guard<std::mutex> l(_qm);
      if (_pool.size() < _qcap) _pool.push_back(std::move(b.data));
    }
    _busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int64_t t = now_ms();
    if (t - _load_t0_ms >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _load = _busy_s / ((t - _load_t0_ms) / 1000.0);
      _busy_s = 0;
      _load_t0_ms = t;
      if (++_status_ticks % 5 == 0 && _mode == Mode::Watch)
        KP_LOGI("status: watch %.1f MHz, cpu demod %.0f%% decode %.0f%%, dropped stream %llu usb %llu decode %llu, "
                "restarts %llu, %s %s lock %.2f fps %.0f, acq %llu lost %llu rephase %llu", _watch_hz / 1e6,
                _load * 100, _dload * 100, (unsigned long long)(_rx ? _rx->status().mid_drops : 0),
                (unsigned long long)_dropped.load(), (unsigned long long)_ddropped.load(),
                (unsigned long long)(_rx ? _rx->status().restarts : 0), _vstats.std_name,
                _vstats.colour ? "colour" : "grey", _vstats.lock_recent, _fps,
                (unsigned long long)_vstats.acquisitions, (unsigned long long)_vstats.losses,
                (unsigned long long)_vstats.rephases);
    }
  }
}

void Radio::handle_watch(Bank &b) {
  lowlat_thread();  // the DSP thread
  double req;
  {
    std::lock_guard<std::mutex> l(_m);
    req = _req_hz;
  }
  // ignore banks from before the retune
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3) return;
  if (!_fe || b.fs != _fe_fs || b.shift_hz != _fe_shift) {
    DecoderOpts o;  // AnalogRX low latency: every line to _lines
    if (_lines) o.lines = [this](const Line &l) {  // stamped with when its samples came in from USB
      Line c = l;
      c.t_s = _dec_t;
      _lines(c);
    };
    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _frames, o);
    _fe_fs = b.fs;
    _fe_shift = b.shift_hz;
    // a new carrier or rate: no weights or noise estimates from the last one, and the paths' time offset is
    // measured again (it is a number of samples, so it depends on the rate)
    _comb.reset(b.fs);
  } else if (b.gen != _fe_gen) {
    // same settings: keep the decoder; it re-phases onto the restarted stream within a few lines
    std::string why;
    if (_rx) why = _rx->status().last_fault;
    KP_LOGW("radio: stream restarted (gen %u, %s), decoder keeps going", b.gen, why.c_str());
  }
  // After a restart, or banks lost on the way here (dropped by the stream: a step in seq), this bank does not
  // follow on from the last one: the combiner must not delay path B with samples from before the hole.
  if (b.gen != _fe_gen || b.seq != _watch_seq + 1) _comb.cut();
  _fe_gen = b.gen;
  _watch_seq = b.seq;
  unpack(b, _iq);
  const bool comb = _combine.load();
  unpack_b(b, _iqb);  // always: cheap
  measure(b);
  if (comb != _combine_was) {
    _comb.reset(b.fs);
    _combine_was = comb;
    KP_LOGI("radio: video antennas %s", comb ? "A + B combined" : "A only");
  }
  // two-antenna video: B lined up and turned into step with A and both weighted by their SNR, in place of A
  if (comb) _comb.process(_iq.data(), _iqb.data(), std::min(_iq.size(), _iqb.size()));
  const int64_t tnow = now_ms();
  if (tnow - _comb_t0_ms >= 1000) {
    _comb_t0_ms = tnow;
    ComboStatus cs;
    cs.on = comb;
    if (comb) cs.s = _comb.take_stats();
    bool news;
    {
      std::lock_guard<std::mutex> l(_m);
      news = comb && (!_comb_status.on || cs.s.lag != _comb_status.s.lag ||
                      cs.s.lag_measured != _comb_status.s.lag_measured);
      _comb_status = cs;
    }
    if (news)
      KP_LOGI("radio: antenna B taken as %d samples (%.0f ns) ahead of A, %s", cs.s.lag, cs.s.lag / b.fs * 1e9,
              cs.s.lag_measured ? "measured" : "none until measured");
  }
  std::vector<float> d;
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (!_dpool.empty()) {
      d = std::move(_dpool.back());
      _dpool.pop_back();
    }
  }
  std::vector<uint8_t> bad;
  _fe->demod(_iq.data(), _iq.size(), d, &bad);
  {
    std::lock_guard<std::mutex> l(_dqm);
    // ~26 ms behind (low latency: over 8 ms since the oldest arrived): drop the oldest (the decoder re-phases)
    const double tq = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    while (!_dq.empty() && (_dq.size() >= 128 || (_lowlat && tq - _dq.front().t_s > 0.008))) {
      _dpool.push_back(std::move(_dq.front().d));
      _dq.pop_front();
      _ddropped++;
    }
    _dq.push_back(DBlock{_fe, std::move(d), std::move(bad), b.t_s});
  }
  _dqcv.notify_one();
}

// AnalogRX: the power on each antenna path (~1 ms points) and an averaged spectrum of path A (~10/s).
// Total power of the stream, i.e. of the ~35 MHz the chip passes around the pilot's channel; the next
// raceband channel (37 MHz away) is mostly outside it.
void Radio::measure(const Bank &b) {
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  double pa = 0, pb = 0;
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    const int ai = wsdr::la_i(w), aq = wsdr::la_q(w);
    // path B: I [35:24], Q [47:36] (kDmaBothPaths8822b), sign-extended like la_i / la_q
    const int bi = static_cast<int>(static_cast<int64_t>(w << 28) >> 52);
    const int bq = static_cast<int>(static_cast<int64_t>(w << 16) >> 52);
    pa += ai * ai + aq * aq;
    pb += bi * bi + bq * bq;
  }
  if (_lv_n == 0) _lv_t = b.t_s;
  _lv_acc_a += pa;
  _lv_acc_b += pb;
  _lv_n += ns;
  if (++_lv_banks >= 5) {  // 5 banks of 8192 at 40 MS/s ~ 1 ms
    constexpr double fs2 = 2048.0 * 2048.0;  // 12-bit full scale
    auto db = [](double pw) { return static_cast<float>(10 * std::log10(pw / fs2 + 1e-12)); };
    const Level lv{_lv_t, db(_lv_acc_a / _lv_n), db(_lv_acc_b / _lv_n)};
    std::lock_guard<std::mutex> l(_lm);
    _levels.push_back(lv);
    while (_levels.size() > 30000) _levels.pop_front();
    _lv_acc_a = _lv_acc_b = 0;
    _lv_n = 0;
    _lv_banks = 0;
  }
  // spectrum: a Hann-windowed 1024-point FFT of the start of every 4th bank, averaged, published every ~100 ms
  constexpr int kN = 1024;
  if ((_psd_banks++ & 3) == 0 && _iq.size() >= static_cast<size_t>(kN)) {
    if (_sp_win.empty()) {
      _sp_win.resize(kN);
      _sp_acc.assign(kN, 0.0);
      for (int i = 0; i < kN; i++) _sp_win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
    }
    _sp_tmp.resize(kN);
    for (int i = 0; i < kN; i++) _sp_tmp[i] = _iq[i] * _sp_win[i];
    fft_inplace(_sp_tmp.data(), kN);
    for (int i = 0; i < kN; i++) _sp_acc[i] += std::norm(_sp_tmp[i]);
    _sp_frames++;
  }
  const int64_t t = now_ms();
  if (t - _psd_t0_ms >= 100 && _sp_frames > 0) {
    // dBFS: a full-scale carrier (amplitude 2048) in one bin reads 0 dB (Hann coherent gain: sum = N/2)
    const double ref = std::pow(2048.0 * kN / 2, 2) * _sp_frames;
    std::vector<float> db(kN);
    for (int k = 0; k < kN; k++)
      db[k] = static_cast<float>(10 * std::log10(_sp_acc[(k + kN / 2) % kN] / ref + 1e-15));  // fftshift
    std::fill(_sp_acc.begin(), _sp_acc.end(), 0.0);
    _sp_frames = 0;
    _psd_t0_ms = t;
    std::lock_guard<std::mutex> l(_lm);
    _spectrum.centre_hz = b.centre_hz;
    _spectrum.fs = b.fs;
    _spectrum.db = std::move(db);
    _spectrum.seq++;
  }
}

std::vector<Radio::Level> Radio::levels_since(double after_s) {
  std::lock_guard<std::mutex> l(_lm);
  std::vector<Level> out;
  auto it = std::upper_bound(_levels.begin(), _levels.end(), after_s,
                             [](double t, const Level &lv) { return t < lv.t_s; });
  out.assign(it, _levels.end());
  return out;
}

std::vector<Radio::Level> Radio::levels_since(double after_s, int ch) {
  std::lock_guard<std::mutex> l(_lm);
  std::vector<Level> out;
  if (ch < 0 || ch >= kMaxTimed) return out;
  const auto &q = _tlevels[ch];
  auto it = std::upper_bound(q.begin(), q.end(), after_s, [](double t, const Level &lv) { return t < lv.t_s; });
  out.assign(it, q.end());
  return out;
}

// Timing only: each channel's power on both antenna paths, about one point per millisecond (gapped: one per
// capture, ~1.6 ms apart). One channel: the whole band (no FFT); several: an FFT of part of each bank, the power
// within +-kHalf of each carrier. dBFS like measure(): the mean power per sample in that band.
void Radio::handle_timing(Bank &b) {
  if (_time_hz.empty()) return;
  const double req = _time_hz.size() == 1 ? _time_hz[0] : _time_centre;
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3) return;  // still the previous tuning
  if (_time_settle < 0) _time_settle = 20;  // the tuner and filters settle
  if (_time_settle > 0) {
    _time_settle--;
    return;
  }
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  const size_t nch = _time_hz.size();
  auto sx = [](uint64_t w, int sh) { return static_cast<int>(static_cast<int64_t>(w << sh) >> 52); };
  if (nch == 1) {
    double pa = 0, pb = 0;
    for (size_t i = 0; i < ns; i++) {
      uint64_t w;
      std::memcpy(&w, p + 8 * i, 8);
      const int ai = wsdr::la_i(w), aq = wsdr::la_q(w), bi = sx(w, 28), bq = sx(w, 16);
      pa += ai * ai + aq * aq;
      pb += bi * bi + bq * bq;
    }
    if (_tl_n == 0) _tl_t = b.t_s;
    _tl_acc_a[0] += pa / std::max<size_t>(ns, 1);
    _tl_acc_b[0] += pb / std::max<size_t>(ns, 1);
  } else {
    // one FFT look per ~1 ms is plenty for timing: a continuous 40 MS/s stream (~5000 banks a second) is thinned
    // to that, so the load stays small (gapped banks are ~1.6 ms apart: all of them)
    if (b.t_s - _tl_last < 0.0009) return;
    _tl_last = b.t_s;
    constexpr int kN = 1024;
    if (_tl_win.empty()) {
      _tl_win.resize(kN);
      for (int i = 0; i < kN; i++) _tl_win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
    }
    _tl_fa.resize(kN);
    _tl_fb.resize(kN);
    double win2 = 0;
    for (float v : _tl_win) win2 += static_cast<double>(v) * v;
    const double fs = b.fs, bin_hz = fs / kN;
    // each carrier's band: +-9 MHz holds most of an analog FM video signal, so its power hardly depends on the
    // picture; narrower when channels sit closer (never past halfway to the neighbour)
    double kHalf = 9e6;
    for (size_t c1 = 0; c1 < nch; c1++)
      for (size_t c2 = c1 + 1; c2 < nch; c2++) kHalf = std::min(kHalf, 0.45 * std::fabs(_time_hz[c1] - _time_hz[c2]));
    const int segs = static_cast<int>(std::max<size_t>(1, ns / kN));  // the whole bank: a short capture is noisy
    double ea[kMaxTimed] = {}, eb[kMaxTimed] = {};
    int done = 0;
    for (int sgi = 0; sgi < segs && static_cast<size_t>(sgi + 1) * kN <= ns; sgi++, done++) {
      const size_t off = static_cast<size_t>(sgi) * kN;
      for (int i = 0; i < kN && off + i < ns; i++) {
        uint64_t w;
        std::memcpy(&w, p + 8 * (off + i), 8);
        _tl_fa[i] = cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w))) * _tl_win[i];
        _tl_fb[i] = cf(static_cast<float>(sx(w, 28)), static_cast<float>(sx(w, 16))) * _tl_win[i];
      }
      fft_inplace(_tl_fa.data(), kN);
      fft_inplace(_tl_fb.data(), kN);
      for (size_t c = 0; c < nch; c++) {
        const double off_hz = _time_hz[c] - b.centre_hz;
        const int k0 = static_cast<int>(std::floor((off_hz - kHalf) / bin_hz)), k1 = static_cast<int>(std::ceil((off_hz + kHalf) / bin_hz));
        for (int k = k0; k <= k1; k++) {
          if (k <= -kN / 2 || k >= kN / 2) continue;  // outside the capture
          const int bin = (k + kN) % kN;
          ea[c] += std::norm(_tl_fa[bin]);
          eb[c] += std::norm(_tl_fb[bin]);
        }
      }
    }
    if (done == 0) return;
    if (_tl_n == 0) _tl_t = b.t_s;
    // Parseval with the window: band energy / (N * sum w^2) = the band's mean power per sample
    for (size_t c = 0; c < nch; c++) {
      _tl_acc_a[c] += ea[c] / (kN * win2 * done);
      _tl_acc_b[c] += eb[c] / (kN * win2 * done);
    }
  }
  _tl_n++;
  // a point per ~1 ms of arrivals (gapped banks are further apart: one each)
  // one point per ~5 ms (several captures averaged: an analog picture's power wanders line by line); one
  // channel (the whole band, every sample): per ~1 ms like watch
  if (b.t_s - _tl_t < (nch == 1 ? 0.001 : 0.005)) return;
  constexpr double fs2 = 2048.0 * 2048.0;  // 12-bit full scale
  auto db = [](double pw) { return static_cast<float>(10 * std::log10(pw / fs2 + 1e-12)); };
  {
    std::lock_guard<std::mutex> l(_lm);
    for (size_t c = 0; c < nch; c++) {
      _tlevels[c].push_back({_tl_t, db(_tl_acc_a[c] / _tl_n), db(_tl_acc_b[c] / _tl_n)});
      while (_tlevels[c].size() > 30000) _tlevels[c].pop_front();
    }
  }
  for (size_t c = 0; c < nch; c++) _tl_acc_a[c] = _tl_acc_b[c] = 0;
  _tl_n = 0;
}

Radio::Spectrum Radio::spectrum() {
  std::lock_guard<std::mutex> l(_lm);
  return _spectrum;
}

double Radio::gain_db() {
  std::lock_guard<std::mutex> l(_m);
  return _gain_db;
}


void Radio::decode_loop() {
  boost_thread("decode");
  std::shared_ptr<FrontEnd> cur;
  double busy = 0;
  int64_t t0 = now_ms();
  while (!_stop) {
    DBlock blk;
    {
      std::unique_lock<std::mutex> l(_dqm);
      _dqcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_dq.empty() || _stop; });
      if (_dq.empty()) continue;
      blk = std::move(_dq.front());
      _dq.pop_front();
    }
    lowlat_thread();
    const auto ta = std::chrono::steady_clock::now();
    if (blk.fe != cur) {
      cur = blk.fe;
      _frames_at_last = 0;
      _fps_t0_ms = now_ms();
    }
    cur->set_async_frames(_lowlat.load(std::memory_order_relaxed));
    _dec_t = blk.t_s;
    cur->decode(blk.d.data(), blk.d.size(), blk.bad.size() == blk.d.size() ? blk.bad.data() : nullptr);
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
    {
      std::lock_guard<std::mutex> l(_dqm);
      if (_dpool.size() < 256) _dpool.push_back(std::move(blk.d));
    }
    const int64_t t = now_ms();
    if (t - _fps_t0_ms >= 1000) {
      const auto s = cur->stats();
      std::lock_guard<std::mutex> l(_m);
      _vstats = s;
      _fps = static_cast<float>((s.frames - _frames_at_last) * 1000.0 / (t - _fps_t0_ms));
      _frames_at_last = s.frames;
      _fps_t0_ms = t;
    }
    if (t - t0 >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _dload = busy / ((t - t0) / 1000.0);
      busy = 0;
      t0 = t;
    }
  }
}

void Radio::handle_scan(Bank &b) {
  if (_at >= _plan.size()) return;
  const double req = _plan[_at];
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != kFs) return;
  if (_settle < 0) {
    _settle = kSettleBanks;
    _cap_restarts = 0;
    _cap.clear();
    _cap_need = static_cast<size_t>(kPsdSpan * kFs);  // first just enough for the spectrum
    _humps.clear();
  }
  if (_settle > 0) {
    _settle--;
    _cap_last_seq = b.seq;
    _cap_gen = b.gen;
    return;
  }
  // A gap (banks dropped on the way, or the stream restarted): start the capture over. Not for ever: on a link
  // that loses a bank every few ms no capture would ever be whole and the scan would stay on this step, so after
  // kCapRestarts tries it is taken with its gaps (the sync check loses a few lines at each).
  const bool gap = b.seq != _cap_last_seq + 1 || b.gen != _cap_gen;
  if (!_cap.empty() && gap && _cap_restarts++ < kCapRestarts) _cap.clear();
  _cap_last_seq = b.seq;
  _cap_gen = b.gen;
  _cap_centre = b.centre_hz;
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns && _cap.size() < _cap_need; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    _cap.emplace_back(static_cast<int16_t>(wsdr::la_i(w)), static_cast<int16_t>(wsdr::la_q(w)));
  }
  if (_cap.size() < _cap_need) return;
  if (_cap_need < static_cast<size_t>(kCapture * kFs)) {
    // spectrum first: a step with no candidate is done now; otherwise keep capturing for the sync check
    std::vector<cf> x(_cap.size());
    for (size_t i = 0; i < _cap.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
    Psd psd(1024);
    psd.add(x.data(), x.size());
    _humps = find_humps(psd.db(), kFs, kEdge);
    if (!_humps.empty()) {
      _cap_need = static_cast<size_t>(kCapture * kFs);
      return;
    }
  }
  scan_step_done();
}

void Radio::scan_step_done() {
  const double centre = _cap_centre;  // the samples are centred on the hardware centre, not the request
  // start the next retune now; the chip settles while this step is analysed
  _at++;
  if (_at < _plan.size()) {
    tune(_plan[_at], 1);
    _settle = -1;
  }
  std::vector<cf> x(_humps.empty() ? 0 : _cap.size());
  for (size_t i = 0; i < x.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
  const auto humps = _humps;
  std::vector<Pilot> found;
  for (const auto &h : humps) {
    const auto vc = check_video(x.data(), x.size(), kFs, h.offset_hz);
    const double mhz = (centre + h.offset_hz) / 1e6;
    KP_LOGI("scan: %.1f MHz hump %+.2f MHz (%.1f MHz wide, %.1f dB) -> %s %s lock %.2f", centre / 1e6,
            h.offset_hz / 1e6, h.width_hz / 1e6, h.snr_db, vc.video ? "VIDEO" : "no video", vc.std_name, vc.lock);
    if (!vc.video) continue;
    Pilot pl;
    pl.mhz = mhz;
    const auto *ch = nearest_channel(mhz);
    pl.name = ch ? ch->name : "";
    pl.std_name = vc.std_name;
    pl.snr_db = h.snr_db;
    pl.lock = vc.lock;
    pl.seen_ms = now_ms();
    found.push_back(pl);
  }
  const bool done = _at >= _plan.size();
  double next_watch = 0;
  {
    std::lock_guard<std::mutex> l(_m);
    // A mode call made since this bank's mode was read (from another thread, while this step was analysed)
    // has the last word: the status shows its progress (a scan called again starts at 0), and the end of
    // this scan does not put a watch of its own choosing over it. The next bank takes that call up.
    const bool current = _mode_epoch == _epoch_seen;
    if (current) _step = _at;
    for (const auto &f : found) {
      auto it = std::find_if(_pilots.begin(), _pilots.end(), [&](const Pilot &p) { return std::fabs(p.mhz - f.mhz) < 4; });
      if (it == _pilots.end()) _pilots.push_back(f);
      else if (f.snr_db >= it->snr_db - 3) *it = f;  // prefer the newer sighting unless much weaker
      else it->seen_ms = f.seen_ms;
    }
    std::sort(_pilots.begin(), _pilots.end(), [](const Pilot &a, const Pilot &b) { return a.snr_db > b.snr_db; });
    if (done && current) {
      // after a scan: the pilot watched before, if still there, else the strongest seen in this scan
      const int64_t scan_start = now_ms() - 60000;
      for (const auto &p : _pilots)
        if (_resume_hz > 0 && std::fabs(p.mhz * 1e6 - _resume_hz) < 4e6) next_watch = p.mhz * 1e6;
      if (next_watch == 0)
        for (const auto &p : _pilots)
          if (p.seen_ms >= scan_start) {
            next_watch = p.mhz * 1e6;
            break;
          }
      if (next_watch == 0 && _resume_hz > 0) next_watch = _resume_hz;
      KP_LOGI("scan: done, %zu pilot(s)%s", _pilots.size(), next_watch > 0 ? ", watching" : "");
      if (next_watch > 0) {
        _watch_hz = next_watch;
        _mode = Mode::Watch;
      } else {
        _mode = Mode::Idle;
      }
      _mode_epoch++;
    }
  }
  _cap.clear();
}

static std::string jstr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

std::string Radio::status_json() {
  std::ostringstream o;
  wsdr::RxStatus rs;
  {
    std::lock_guard<std::mutex> l(_rxm);
    if (_rx) rs = _rx->status();
  }
  std::lock_guard<std::mutex> l(_m);
  // A JSON number or nothing: a nan or inf printed as it comes is not JSON, and a reader may choke on it or,
  // worse, take it for a string.
  const auto num = [](double v) { return std::isfinite(v) ? v : 0.0; };
  static const char *modes[] = {"idle", "scan", "watch", "test", "timing"};
  o << "{\"open\":" << (_open ? "true" : "false") << ",\"usb3\":" << (_superspeed ? "true" : "false")
    << ",\"chip\":" << jstr(_chip) << ",\"rx_state\":" << jstr(wsdr::rx_state_name(rs.state))
    << ",\"rx_error\":" << jstr(rs.error) << ",\"last_fault\":" << jstr(rs.last_fault)
    << ",\"mode\":\"" << modes[static_cast<int>(_mode)] << "\",\"freq_hz\":" << num(_req_hz)
    << ",\"watch_hz\":" << num(_watch_hz) << ",\"fs\":" << num(rs.fs) << ",\"banks\":" << _banks.load()
    << ",\"dropped\":" << _dropped.load() << ",\"decode_dropped\":" << _ddropped.load()
    << ",\"stream_dropped\":" << rs.mid_drops
    << ",\"restarts\":" << rs.restarts << ",\"dsp_load\":" << num(_load) << ",\"decode_load\":" << num(_dload);
  if (_mode == Mode::Scan) o << ",\"scan_step\":" << _step.load() << ",\"scan_steps\":" << _plan_n.load();
  o << ",\"video\":{\"locked\":" << (!_vstats.acquiring && _vstats.lock_recent > 0.5f ? "true" : "false")
    << ",\"lock\":" << num(_vstats.lock_recent) << ",\"std\":" << jstr(_vstats.std_name) << ",\"fps\":" << num(_fps)
    << ",\"acquisitions\":" << _vstats.acquisitions << ",\"losses\":" << _vstats.losses
    << ",\"rephases\":" << _vstats.rephases << ",\"clicks_repaired\":" << _vstats.clicks_repaired
    << ",\"lines_kept\":" << _vstats.lines_kept << ",\"colour\":" << (_vstats.colour ? "true" : "false") << "}";
  {
    const auto &d = _comb_status.s;
    o << ",\"antennas\":{\"combine\":" << (_comb_status.on ? "true" : "false") << ",\"snr_a\":" << num(d.snr_a_db)
      << ",\"snr_b\":" << num(d.snr_b_db) << ",\"snr_c\":" << num(d.snr_c_db) << ",\"b_share\":" << num(d.b_share)
      << ",\"a_fade\":" << num(d.a_fade) << ",\"rescued\":" << num(d.rescued) << ",\"coherence\":" << num(d.coherence)
      << ",\"lag\":" << d.lag << ",\"lag_measured\":" << (d.lag_measured ? "true" : "false") << "}";
  }
  o << ",\"pilots\":[";
  const int64_t t = now_ms();
  for (size_t i = 0; i < _pilots.size(); i++) {
    const auto &p = _pilots[i];
    o << (i ? "," : "") << "{\"mhz\":" << num(p.mhz) << ",\"name\":" << jstr(p.name) << ",\"std\":" << jstr(p.std_name)
      << ",\"snr\":" << num(p.snr_db) << ",\"age_s\":" << (t - p.seen_ms) / 1000 << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace kp
