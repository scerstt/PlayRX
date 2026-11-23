// AnalogRX: kepyur's radio (third_party/kepyur/native/radio, GPL-2.0), copied to pick one of several adapters
// by USB bus/port (open(bus, port)) instead of an Android usbfs fd. Otherwise unchanged.
// Since brought up to date with fixes from Auburn/fpvDROID (the Android app that carries Play RX's changes): a
// bank the stream drops is seen as a hole (status "stream_dropped"; the combiner does not carry samples across
// it), a scan step's capture does not start over for ever on a lossy link, a mode call that lands as a scan ends
// is not overridden by that end, and what other threads read while open(), close() and the DSP thread run (the
// receiver, the scan's progress) is no longer a data race.
//
// kepyur radio: one RTL8812BU (wifi-sdr Receiver) + a DSP thread. Modes:
//   scan   - sweep the analog FPV bands at 40 MS/s, list pilots (humps that pass a sync check)
//   watch  - tune to one pilot and decode its video (frames to the FrameSink)
//   timing - no video: the signal level of up to 3 neighbouring channels, for lap timing (AnalogRX)
// The Receiver's sink only copies banks into a queue (wifi-sdr rule); everything else runs on the DSP thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "combine.h"
#include "spectrum.h"
#include "video.h"

namespace wsdr {
class Receiver;
}

namespace kp {

struct Pilot {
  double mhz = 0;        // measured carrier
  std::string name;      // nearest standard channel ("R4"), "" if none
  std::string std_name;  // NTSC / PAL
  float snr_db = 0;
  float lock = 0;
  int64_t seen_ms = 0;   // steady-clock ms of the last scan that saw it
};

class Radio {
public:
  // lines (AnalogRX low latency): every picture line as it is drawn, on the decoding thread (empty = none)
  explicit Radio(FrameSink frames, LineSink lines = {});
  ~Radio();
  Radio(const Radio &) = delete;
  Radio &operator=(const Radio &) = delete;

  // The 8812BU at this USB bus / port path ("3" or "3.1"); bus -1 = the first one found.
  // Blocking (~5 s chip bring-up). Throws std::exception with a user-presentable message.
  void open(int bus = -1, const std::string &port = {});
  void close();
  bool is_open() const;
  bool superspeed() const;

  void scan();                 // then watch the previous pilot again, or the strongest found
  void watch(double hz);
  void idle();
  // AnalogRX timing only: no video; the signal level of each of these channels (up to kMaxTimed, all within
  // ~64 MHz of each other) for lap timing. One channel: 20 MS/s, the whole band's power (like watch). Several:
  // one wide capture holding them all (40 MS/s when they are within ~20 MHz, else 80 MS/s gapped: ~77 MHz),
  // each channel's power from an FFT. Their levels: levels_since(t, channel index).
  static constexpr int kMaxTimed = 4;
  static constexpr double kMaxTimedSpanHz = 64e6;
  void time_channels(const std::vector<double> &hz);
  // Debug: the rate watch uses (1 = 40 MS/s, the default; 2 = 20 MS/s); takes effect on the next watch().
  void set_watch_rate(int rate_sel);
  // Debug: gain for the next tune: < 0 = the chip's AGC, else fixed gain in dB (CCA off, IGI pinned).
  void set_gain(double db);
  // AnalogRX: watch with the hardware centre ~4 MHz (20 MS/s) / ~6 MHz (40 MS/s) to one side of the pilot, so the
  // video never sits on the chip's DC point: its DC filter eats whatever brightness lands there and rings when
  // the picture moves off it (stripes at the left edge, ghost outlines). Returns true when it changed (retune).
  bool set_offset_tuning(bool on);
  // AnalogRX: two-antenna video: true = paths A and B combined before demodulation (combine.h), false = A only.
  // Takes effect at once (no retune).
  void set_combine(bool on);
  // AnalogRX low latency (this radio feeds the low latency window): whole pictures built on a thread of their own,
  // the bank and decode queues kept short (a backlog is dropped, not drawn late) and the USB, DSP and decode threads
  // in the "Pro Audio" class (Windows). Takes effect at once.
  void set_low_latency(bool on) { _lowlat = on; }
  bool is_idle();  // idle() and nothing since
  struct ComboStatus {
    bool on = false;
    arx::Combiner::Stats s;  // the last second; the time offset between the paths as of its end
  };

  std::string status_json();   // for the UI (polled)
  std::vector<Pilot> pilots();

  // ---- AnalogRX additions: signal levels and spectrum while watching ----
  // Power of each antenna path (A: the video input, B: the second input, e.g. a gate antenna), in dBFS,
  // one point per ~1 ms of IQ. `t_s` is the steady-clock time (s) the samples arrived from USB.
  struct Level {
    double t_s;
    float a_db, b_db;
  };
  // Points newer than `after_s` (at most the last ~30 s).
  std::vector<Level> levels_since(double after_s);
  // timing only: channel ch of the time_channels() list
  std::vector<Level> levels_since(double after_s, int ch);
  struct Spectrum {
    double centre_hz = 0, fs = 0;
    std::vector<float> db;  // fftshifted: bin 0 = centre - fs/2
    uint64_t seq = 0;       // bumps on every new spectrum (~10 per second)
  };
  Spectrum spectrum();
  double gain_db();  // < 0: the chip's AGC

private:
  enum class Mode { Idle, Scan, Watch, Timing };
  struct Bank {
    std::vector<uint8_t> data;
    uint64_t seq;
    uint32_t gen;
    double fs, centre_hz, shift_hz;
    double t_s;  // arrival (steady clock, s)
  };
  void measure(const Bank &b);  // levels + spectrum (DSP thread, watch mode)
  void dsp_loop();
  void decode_loop();
  void on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift);
  void tune(double hz, int rate_sel, bool watching = false);
  void handle_watch(Bank &b);
  void handle_scan(Bank &b);
  void handle_timing(Bank &b);
  void scan_step_done();
  void unpack(const Bank &b, std::vector<cf> &out);
  void unpack_b(const Bank &b, std::vector<cf> &out);  // path B

  FrameSink _frames;
  LineSink _lines;
  std::atomic<bool> _lowlat{false};
  double _dec_t = 0;  // decode thread: arrival of the block being decoded (stamped on its lines)
  void lowlat_thread();  // the calling thread into / out of the Pro Audio class as _lowlat changes
  std::unique_ptr<wsdr::Receiver> _rx;
  std::mutex _rxm;  // _rx itself (open and close replace it; status_json reads it from another thread)
  mutable std::mutex _m;  // mode, pilots, status fields below
  Mode _mode = Mode::Idle;
  uint64_t _mode_epoch = 0;  // bumped on every mode change; the DSP thread restarts its state
  double _watch_hz = 0, _req_hz = 0;
  int _req_rate = 1;
  int _watch_rate = 1;
  double _gain_db = -1;
  bool _offset = true;
  std::vector<Pilot> _pilots;
  bool _superspeed = false, _open = false;
  std::string _chip;

  // bank queue
  std::mutex _qm;
  std::condition_variable _qcv;
  std::deque<Bank> _q;
  std::vector<std::vector<uint8_t>> _pool;
  size_t _qcap = 512;  // 100 ms at 40 MS/s
  std::atomic<uint64_t> _banks{0}, _dropped{0};

  std::thread _dsp;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _started{false};  // open() has the receiver started: only then can it be found stopped

  // decode thread: watch mode runs demod on the DSP thread and the video decoder here (two cores)
  struct DBlock {
    std::shared_ptr<FrontEnd> fe;
    std::vector<float> d;
    std::vector<uint8_t> bad;  // sparkle repair marks (empty when it is off)
    double t_s = 0;            // when its bank arrived from USB (steady clock, s)
  };
  std::thread _decoder;
  std::mutex _dqm;
  std::condition_variable _dqcv;
  std::deque<DBlock> _dq;
  std::vector<std::vector<float>> _dpool;
  std::atomic<uint64_t> _ddropped{0};
  double _dload = 0;

  // DSP-thread state
  uint64_t _epoch_seen = ~0ull;
  std::shared_ptr<FrontEnd> _fe;
  uint32_t _fe_gen = ~0u;
  uint64_t _watch_seq = 0;  // the last bank this watch took (with _fe_gen: does the next one follow on from it?)
  double _fe_fs = 0, _fe_shift = 0;
  std::vector<cf> _iq, _iqb;
  // two-antenna video
  std::atomic<bool> _combine{false};
  bool _combine_was = false;  // DSP thread: what the last bank did (reset the combiner on a change)
  arx::Combiner _comb;
  int64_t _comb_t0_ms = 0;
  ComboStatus _comb_status;  // under _m
  // scan
  std::vector<double> _plan;
  size_t _at = 0;  // the step the scan is on
  // The scan's progress as status_json() shows it (_step: _at, _plan_n: _plan.size()). Atomic: read there while
  // the DSP thread moves it on, and set back to the start by scan() for the status to show until that thread
  // takes the new scan up.
  std::atomic<size_t> _step{0}, _plan_n{0};
  int _settle = 0;
  uint64_t _cap_last_seq = 0;
  uint32_t _cap_gen = 0;
  int _cap_restarts = 0;  // of this step's capture, after a gap
  std::vector<std::complex<int16_t>> _cap;
  size_t _cap_need = 0;
  double _cap_centre = 0;
  std::vector<Hump> _humps;  // this step's candidates (from the first kPsdSpan of the capture)  // hardware centre of the capture (humps are measured from it)
  double _resume_hz = 0;
  // load
  double _busy_s = 0, _load = 0;
  int64_t _load_t0_ms = 0;
  uint64_t _status_ticks = 0;
  // video stats for status
  DecoderStats _vstats;
  uint64_t _frames_at_last = 0;
  int64_t _fps_t0_ms = 0;
  float _fps = 0;
  // levels + spectrum (DSP thread writes, readers take _lm)
  std::mutex _lm;
  std::deque<Level> _levels;
  double _lv_acc_a = 0, _lv_acc_b = 0, _lv_t = 0;
  size_t _lv_n = 0;
  int _lv_banks = 0;
  std::vector<float> _sp_win;
  std::vector<cf> _sp_tmp;
  std::vector<double> _sp_acc;
  int _sp_frames = 0;
  int64_t _psd_t0_ms = 0;
  uint64_t _psd_banks = 0;
  Spectrum _spectrum;
  // timing only (requests under _m; the DSP thread's working copy below)
  std::vector<double> _time_req_hz;
  std::vector<double> _time_hz;
  double _time_centre = 0;
  int _time_rate = 2, _time_settle = 0;
  std::deque<Level> _tlevels[kMaxTimed];  // under _lm
  double _tl_acc_a[kMaxTimed] = {}, _tl_acc_b[kMaxTimed] = {}, _tl_t = 0, _tl_last = 0;
  int _tl_n = 0;
  std::vector<float> _tl_win;
  std::vector<cf> _tl_fa, _tl_fb;
};

int64_t now_ms();

}  // namespace kp
