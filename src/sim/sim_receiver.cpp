// A stand-in for wsdr::Receiver (analogrx_sim only): several virtual 8812BU adapters, each hearing every
// virtual transmitter (a .cs16 recording, e.g. from gen_testsignal) that falls inside its tuned band.
// Based on kepyur's tools/sim_receiver.cpp (one adapter, one recording; GPL-2.0), made multi-instance.
//
// arx::sim::configure() first: how many adapters, which recordings. Each recording's .json gives its centre
// frequency (centre_hz); the transmitter in it stays there, so tuning elsewhere moves it in the band and a
// step far from it shows only noise. Bank pacing is real time.
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "adapters.h"
#include "receiver.h"
#include "sim.h"
#include "stream88xx.h"

namespace arx::sim {
namespace {
struct Recording {
  std::string path;
  std::vector<int16_t> iq;  // I,Q (value << 4)
  double centre = 5770e6;
  double rms = 1;  // of the samples (value units, after >> 4)
};
struct State {
  std::mutex m;
  int adapters = 0;
  std::vector<std::shared_ptr<const Recording>> recs;
  std::set<int> busy;  // virtual adapter numbers in use
  std::map<const void *, int> owner;  // Receiver -> its adapter
  bool two_antennas = false;
  double snr_db = 12;
};
State &st() {
  static State s;
  return s;
}

double json_number(const std::string &txt, const char *key, double def) {
  const std::string k = std::string("\"") + key + "\"";
  size_t p = txt.find(k);
  if (p == std::string::npos) return def;
  p = txt.find(':', p);
  return p == std::string::npos ? def : std::strtod(txt.c_str() + p + 1, nullptr);
}
}  // namespace

constexpr int kSimBus = 100;  // virtual adapters live on "bus 100", ports 1..N

void configure(int adapters, const std::vector<std::string> &recordings) {
  std::vector<std::shared_ptr<const Recording>> recs;
  for (const auto &path : recordings) {
    auto r = std::make_shared<Recording>();
    r->path = path;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("cannot open " + path);
    std::fseek(fp, 0, SEEK_END);
    const long bytes = std::min<long>(std::ftell(fp), 40000000L * 4 * 2);  // up to 2 s
    std::fseek(fp, 0, SEEK_SET);
    r->iq.resize(bytes / 2);
    const size_t got = std::fread(r->iq.data(), 2, r->iq.size(), fp);
    std::fclose(fp);
    if (got != r->iq.size()) throw std::runtime_error("short read: " + path);
    std::string base = path;
    if (base.size() > 5 && base.compare(base.size() - 5, 5, ".cs16") == 0) base.resize(base.size() - 5);
    std::ifstream j(base + ".json");
    std::stringstream ss;
    ss << j.rdbuf();
    r->centre = json_number(ss.str(), "centre_hz", 5770e6);
    double p = 0;
    const size_t n = std::min<size_t>(r->iq.size(), 4000000);
    for (size_t i = 0; i < n; i++) p += (r->iq[i] / 16.0) * (r->iq[i] / 16.0);
    r->rms = n ? std::max(1.0, std::sqrt(2 * p / n)) : 1;
    recs.push_back(r);
  }
  std::lock_guard<std::mutex> l(st().m);
  st().adapters = adapters;
  st().recs = std::move(recs);
}

void set_two_antennas(bool on, double snr_db) {
  std::lock_guard<std::mutex> l(st().m);
  st().two_antennas = on;
  st().snr_db = snr_db;
}

std::vector<AdapterInfo> list_adapters() {
  std::lock_guard<std::mutex> l(st().m);
  std::vector<AdapterInfo> v;
  for (int i = 1; i <= st().adapters; i++) {
    AdapterInfo a;
    a.bus = kSimBus;
    a.port = std::to_string(i);
    a.vid = 0x0bda;
    a.pid = wsdr::kPid8812bu;
    a.superspeed = true;
    a.name = "Simulated adapter " + std::to_string(i);
    v.push_back(a);
  }
  return v;
}

}  // namespace arx::sim

namespace wsdr {

// USB 3 recovery: nothing to recover in the simulation; kept so the switch reads back
static std::atomic<bool> g_usb3_recovery{true};
void set_usb3_recovery(bool on) { g_usb3_recovery = on; }
bool usb3_recovery() { return g_usb3_recovery.load(); }

const char *rx_state_name(RxState s) {
  switch (s) {
  case RxState::Closed: return "closed";
  case RxState::Opening: return "opening";
  case RxState::Starting: return "starting";
  case RxState::Streaming: return "streaming";
  case RxState::Lost: return "lost";
  case RxState::Error: return "error";
  }
  return "?";
}

Receiver::Receiver(Logger_t log) : _log(std::move(log)) {}
Receiver::~Receiver() { close(); }

void Receiver::set_state(RxState s, const std::string &err) {
  std::lock_guard<std::mutex> l(_m);
  _st.state = s;
  _st.error = err;
}

void Receiver::open(uint16_t pid, const RxSettings &initial, int bus, const std::string &port, int) {
  auto &S = arx::sim::st();
  {
    std::lock_guard<std::mutex> l(S.m);
    int n = -1;
    if (bus < 0) {
      for (int i = 1; i <= S.adapters && n < 0; i++)
        if (!S.busy.count(i)) n = i;
    } else if (bus == arx::sim::kSimBus) {
      n = std::atoi(port.c_str());
      if (n < 1 || n > S.adapters) n = -1;
    }
    if (n < 0) throw std::runtime_error("adapter not found");
    if (S.busy.count(n)) throw std::runtime_error("adapter busy");
    S.busy.insert(n);
    S.owner[this] = n;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // a little of the real bring-up delay
  _set = initial;
  _caps = {};
  _caps.pid = pid;
  _caps.superspeed = true;
  _caps.chip = "SIM";
  _caps.min_rate_sel = 1;
  _open = true;
  set_state(RxState::Starting);
}

void Receiver::close() {
  stop();
  _open = false;
  auto &S = arx::sim::st();
  std::lock_guard<std::mutex> l(S.m);
  auto it = S.owner.find(this);
  if (it != S.owner.end()) {
    S.busy.erase(it->second);
    S.owner.erase(it);
  }
}

void Receiver::start(BankSink sink) {
  _sink = std::move(sink);
  _active = true;
  _worker = std::thread([this] { worker(); });
}

void Receiver::stop() {
  _active = false;
  if (_worker.joinable()) _worker.join();
}

void Receiver::set(const RxSettings &s) {
  std::lock_guard<std::mutex> l(_m);
  _set = s;
  _restart = true;
}

RxSettings Receiver::settings() const {
  std::lock_guard<std::mutex> l(_m);
  return _set;
}

RxStatus Receiver::status() const {
  std::lock_guard<std::mutex> l(_m);
  RxStatus s = _st;
  s.running = _active;
  return s;
}

void Receiver::worker() {
  std::vector<std::shared_ptr<const arx::sim::Recording>> recs;
  int me = 0;
  bool two = false;
  double snr_db = 12;
  {
    auto &S = arx::sim::st();
    std::lock_guard<std::mutex> l(S.m);
    recs = S.recs;
    two = S.two_antennas;
    snr_db = S.snr_db;
    me = S.owner.count(this) ? S.owner[this] : 0;
  }
  // a little noise so empty steps have a floor: a precomputed Gaussian table walked from a random start
  // each bank (drawing 80 M normals a second would cost more than a core per adapter)
  std::mt19937 rng(7 + me);
  static const std::vector<float> noise = [] {
    std::mt19937 g(1);
    std::normal_distribution<float> nd(0, 3);
    std::vector<float> v(1 << 18);
    for (auto &x : v) x = nd(g);
    return v;
  }();
  const size_t N = Stream8822b::kBankSamples;
  std::vector<uint8_t> bank(N * 8);
  // each adapter starts the recordings at its own place, so two adapters on one channel are not identical
  std::vector<size_t> pos(recs.size());
  std::vector<double> ph(recs.size(), 0);
  for (size_t r = 0; r < recs.size(); r++) pos[r] = (static_cast<size_t>(me) * 977777 * 2) % recs[r]->iq.size();
  std::vector<std::complex<float>> acc(N);
  // two video antennas: each path a Rayleigh fading channel (Jakes: 8 rays, ~4 Hz Doppler, a quad at speed),
  // independent of the other; the signal at amplitude kAmp, noise set by snr_db
  struct Ray { double f, ph; };
  std::vector<Ray> rays_a, rays_b;
  {
    std::mt19937 g(1234 + 17 * me);
    std::uniform_real_distribution<double> u(0, 2 * M_PI);
    for (int r = 0; r < 8; r++) rays_a.push_back({4.0 * std::cos(u(g)), u(g)});
    for (int r = 0; r < 8; r++) rays_b.push_back({4.0 * std::cos(u(g)), u(g)});
  }
  auto fade = [](const std::vector<Ray> &rays, double t) {
    std::complex<double> h{};
    for (const auto &r : rays) h += std::polar(1.0, 2 * M_PI * r.f * t + r.ph);
    return std::complex<float>(h / std::sqrt(static_cast<double>(rays.size())));
  };
  constexpr float kAmp = 250;  // of 2048 full scale (away from the gate; ~3x at the gate, Rayleigh peaks ~2x)
  const float sig_scale = recs.empty() ? 1.f : static_cast<float>(kAmp / recs[0]->rms);
  const float noise_scale = static_cast<float>(kAmp / std::sqrt(2.0 * std::pow(10.0, snr_db / 10)) / 3.0);  // table sigma 3
  if (two) std::fprintf(stderr, "sim adapter %d: two video antennas, mean SNR %.0f dB\n", me, snr_db);
  uint64_t seq = 0;
  auto t_next = std::chrono::steady_clock::now();
  RxSettings s;
  BankMeta m;
  bool first = true;
  while (_active) {
    bool restart;
    {
      std::lock_guard<std::mutex> l(_m);
      restart = _restart || first;
      _restart = false;
      s = _set;
    }
    if (restart) {
      if (!first) std::this_thread::sleep_for(std::chrono::milliseconds(150));  // retune + stream restart
      first = false;
      m = {};
      m.gen = ++_gen;
      m.fs = s.rate_sel == 0 ? 40e6 : rate_of(s.rate_sel);  // 80 MS/s (gapped, timing only): the sim tops out at 40
      m.centre_hz = std::lround((s.centre > 0 ? s.centre : s.freq) / 5e6) * 5e6;
      m.shift_hz = s.freq - m.centre_hz;
      seq = 0;
      t_next = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> l(_m);
      _st.state = RxState::Streaming;
      _st.fs = m.fs;
      _st.centre_hz = m.centre_hz;
      _st.gen = m.gen;
    }
    // every recording at 40 MS/s, moved by (its centre - hw centre); decimated naively for lower rates
    const int dec = static_cast<int>(std::lround(40e6 / m.fs));
    std::fill(acc.begin(), acc.end(), std::complex<float>{});
    for (size_t r = 0; r < recs.size(); r++) {
      const auto &rec = *recs[r];
      const double move = rec.centre - m.centre_hz;
      const size_t len = rec.iq.size();
      if (std::fabs(move) >= 30e6) {
        pos[r] = (pos[r] + 2 * N * dec) % len;  // keep time running while out of band
        continue;
      }
      const std::complex<float> rot(static_cast<float>(std::cos(2 * M_PI * move / 40e6)),
                                    static_cast<float>(std::sin(2 * M_PI * move / 40e6)));
      std::complex<float> lo(static_cast<float>(std::cos(ph[r])), static_cast<float>(std::sin(ph[r])));
      size_t p = pos[r];
      for (size_t i = 0; i < N; i++) {
        std::complex<float> sum{};
        for (int k = 0; k < dec; k++) {
          sum += std::complex<float>(rec.iq[p] / 16.f, rec.iq[p + 1] / 16.f) * lo;
          p += 2;
          if (p >= len) p = 0;
          lo *= rot;
        }
        acc[i] += sum / static_cast<float>(dec);
      }
      lo /= std::abs(lo);
      ph[r] = std::arg(lo);
      pos[r] = p;
    }
    // path B is a gate antenna: the same signal ~26 dB down, rising to full strength for ~0.3 s as the quad
    // flies through the gate once a lap. Each virtual adapter's pilot laps at their own pace, +-1 s per lap.
    const double t_now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double lap = 14 + 2.3 * me, off = 1.7 * me;
    const double k = std::floor((t_now + off) / lap);
    const uint32_t h = static_cast<uint32_t>(k) * 2654435761u ^ static_cast<uint32_t>(me) * 40503u;
    const double jitter = ((h >> 8) % 2001) / 1000.0 - 1.0;
    double dt = std::fmod(t_now + off, lap) - (lap / 2 + jitter);
    // about one lap in four the pilot clips past the gate and comes back through 2.5 s later (a near miss the
    // gate counts, then the real pass inside the minimum lap time): the second pass is the nearer one
    if ((h >> 4) % 4 == 0 && std::abs(dt - 2.5) < std::abs(dt)) dt -= 2.5;
    const float gate = static_cast<float>(0.05 + 0.95 * std::exp(-(dt / 0.12) * (dt / 0.12)));
    const float gate_a = static_cast<float>(0.14 + 0.86 * std::exp(-(dt / 0.25) * (dt / 0.25)));
    size_t np = rng() & (noise.size() - 1) & ~size_t(1);
    auto q12 = [](float v) {
      const int x = std::max(-2048, std::min(2047, static_cast<int>(std::lround(v))));
      return static_cast<uint64_t>(x & 0xFFF);
    };
    if (two) {
      const double tb = static_cast<double>(seq) * N / m.fs;
      // both video antennas rise ~10 dB as the quad passes the gate (so timing on Best of A/B still works)
      const float rise = static_cast<float>(1 + 2 * std::exp(-(dt / 0.25) * (dt / 0.25)));
      const std::complex<float> ha = fade(rays_a, tb) * rise * sig_scale, hb = fade(rays_b, tb) * rise * sig_scale;
      for (size_t i = 0; i < N; i++) {
        const float na = noise[np] * noise_scale, nb = noise[np + 1] * noise_scale;
        np = (np + 2) & (noise.size() - 1);
        const float nc = noise[np] * noise_scale, nd = noise[np + 1] * noise_scale;
        const std::complex<float> xa = acc[i] * ha, xb = acc[i] * hb;
        const uint64_t w = q12(xa.real() + na) | (q12(xa.imag() + nb) << 12) | (q12(xb.real() + nc) << 24) | (q12(xb.imag() + nd) << 36);
        std::memcpy(bank.data() + 8 * i, &w, 8);
      }
    } else
    for (size_t i = 0; i < N; i++) {
      const float na = noise[np], nb = noise[np + 1];
      np = (np + 2) & (noise.size() - 1);
      const float nc = noise[np], nd = noise[np + 1];
      // path A (video antenna) also rises at the gate, less: ~-17 dB below its peak elsewhere on the lap
      const uint64_t w = q12(acc[i].real() * gate_a + na) | (q12(acc[i].imag() * gate_a + nb) << 12) |
                         (q12(acc[i].real() * gate + nc) << 24) | (q12(acc[i].imag() * gate + nd) << 36);
      std::memcpy(bank.data() + 8 * i, &w, 8);
    }
    m.seq = seq++;
    _sink(bank.data(), bank.size(), m);
    {
      std::lock_guard<std::mutex> l(_m);
      _st.banks++;
    }
    t_next += std::chrono::nanoseconds(static_cast<int64_t>(N / m.fs * 1e9));
    std::this_thread::sleep_until(t_next);
  }
}

}  // namespace wsdr
