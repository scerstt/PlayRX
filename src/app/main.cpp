// AnalogRX: analog 5.8 GHz FPV receivers on Windows, one RTL8812BU adapter per pilot, each pilot's video
// served to OBS, lap timing from the adapters' second antenna input.
//
// The program is an engine plus a web UI:
//   - the engine (this file, on the main thread): adapters, one kepyur radio per pilot slot, the scanner,
//     lap timing, settings. A message-only window gives it timers and lets the HTTP threads call into it.
//   - the HTTP server (src/feeds): the UI (web\ folder: one design system for the app, the gate tuning, the
//     race board and the OBS pages), the JSON API below, and every pilot's video as MJPEG.
//   - the window: a WebView2 view of http://localhost:PORT/. The same UI works from any browser on the
//     network with --lan (a tablet for the race director).
//
// analogrx.exe uses the USB adapters. analogrx_sim.exe is the same app with simulated adapters and
// transmitters (--sim-adapters N, --sim FILE.cs16 ...; default: every rec\sim_*.cs16).
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <io.h>  // _commit
#else
// macOS / Linux: the engine loop under Win32's names, Unix process / path calls; macOS also the Cocoa bits
// (mac_window.mm). Linux (a Raspberry Pi) is headless: no window, the UI only through the browser.
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <csignal>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "engine_loop.h"
#ifdef __APPLE__
#include "mac_window.h"
#endif
extern char **environ;
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef __linux__
#include "webview/webview.h"  // the app window (WebView2 / WKWebView); Linux runs headless
#endif

#include "adapters.h"
#include "receiver.h"  // wsdr::kPid8812bu
#include "usbmode.h"   // wsdr::request_usb3
#include "live_out.h"
#ifdef _WIN32
#include "vcam_writer.h"
#include "lowlat.h"
#endif
#include "feed_server.h"
#include "log.h"
#include "radio.h"
#include "spectrum.h"
#include "capture.h"
#include "net_feed.h"
#include "timing.h"
#include "usb_info.h"  // USB check: host controller, power source and plan
#ifdef ANALOGRX_SIM
#include "sim.h"
#endif

namespace {

using arx::kMaxSlots;

// ---------------------------------------------------------------- small helpers
#ifndef _WIN32
// macOS: file names are UTF-8; std::filesystem converts to and from the wide strings the Windows code uses
std::string narrow(const std::wstring &w) {
  const auto u = std::filesystem::path(w).u8string();
  return std::string(u.begin(), u.end());
}
std::wstring widen(const std::string &s) { return std::filesystem::path(std::u8string(s.begin(), s.end())).wstring(); }
// Explorer's "open this folder / URL": Finder or the default browser, through /usr/bin/open (not waited for)
void open_with_system(const std::string &target) {
#ifdef __linux__
  (void)target;  // headless: no desktop to open it on
  return;
#endif
  pid_t pid = 0;
  const char *argv[] = {"/usr/bin/open", target.c_str(), nullptr};
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char *const *>(argv), environ) == 0)
    std::thread([pid] { int st = 0; waitpid(pid, &st, 0); }).detach();
}
constexpr int SW_SHOWNORMAL = 1;
void ShellExecuteW(void *, const wchar_t *, const wchar_t *target, const wchar_t *, const wchar_t *, int) {
  open_with_system(narrow(target));
}
void ShellExecuteA(void *, const char *, const char *target, const char *, const char *, int) { open_with_system(target); }
// Windows' local date and time, for the few places that format one
struct SYSTEMTIME {
  int wYear, wMonth, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
void GetLocalTime(SYSTEMTIME *t) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm lt{};
  localtime_r(&tt, &lt);
  *t = {lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000)};
}
void CreateDirectoryW(const wchar_t *p, void *) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(p), ec);
}
std::string home_dir() {
  const char *h = std::getenv("HOME");
  if (h && *h) return h;
  const passwd *pw = getpwuid(getuid());
  return pw && pw->pw_dir ? pw->pw_dir : "/tmp";
}
#else
std::string narrow(const std::wstring &w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
[[maybe_unused]] std::wstring widen(const std::string &s) {  // (Play RX: unused on Windows)
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
#endif

// Minimal JSON lookups: Radio::status_json() and the UI's request bodies (JSON.stringify: no spaces).
// The first occurrence of a key wins (top level or "video" for every key used here).
double jnum(const std::string &j, const char *key, double def = 0) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t p = j.find(k);
  return p == std::string::npos ? def : std::strtod(j.c_str() + p + k.size(), nullptr);
}
bool jhas(const std::string &j, const char *key) { return j.find(std::string("\"") + key + "\":") != std::string::npos; }
bool jbool(const std::string &j, const char *key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return false;
  p += k.size();
  while (p < j.size() && (j[p] == ' ' || j[p] == ':' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;  // "key": true
  return j.compare(p, 4, "true") == 0;
}
std::string jstr(const std::string &j, const char *key) {
  const std::string k = std::string("\"") + key + "\":\"";
  const size_t p = j.find(k);
  if (p == std::string::npos) return {};
  std::string o;
  for (size_t i = p + k.size(); i < j.size() && j[i] != '"'; i++) {
    if (j[i] == '\\' && i + 1 < j.size()) i++;
    o += j[i];
  }
  return o;
}
// Cut UTF-8 text to at most `max` bytes without splitting a character (an accented letter, an emoji) in two
void utf8_cut(std::string &s, size_t max) {
  if (s.size() <= max) return;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;  // s[n] continues a character: back to its start
  s.resize(n);
}
std::string q(const std::string &s) {  // JSON string literal
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}
std::string num(double v, int prec = 1) {
  char b[48];
  std::snprintf(b, sizeof b, "%.*f", prec, std::isfinite(v) ? v : 0.0);
  return b;
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------- settings (engine thread)
struct Settings {
  std::string bands = "R";  // channel bands offered in the UI (A B E F R L); Raceband only by default
  bool lan = false;         // serve the UI and feeds to other devices on the network (applies at next start)
  bool guided = true;       // guided race start: video check, gate check, review, staging
  int passes = 2;           // gate passes per pilot in the gate check
  // automatic gate tuning: the gate check applies clear-cut suggestions itself (marginal / not picked up, at most
  // twice a pilot, then a recheck) and goes straight to staging once every pilot is good. Off = all by hand.
  bool auto_gate = true;
  bool voice = true;        // spoken prompts on this PC
  int volume = 100;         // 0..100
  int rate = 0;             // speech speed -5..5
  int pitch = 0;            // voice pitch -10..10
  std::string voice_id;     // the Windows voice (SAPI token id); "" = Windows' default
  std::string precheck = "brief";
  // timing only: no video; each adapter times up to 3 pilots on neighbouring channels (lap timing for more
  // pilots than adapters, and a light load: a Raspberry Pi times 4+ pilots)
#if defined(__linux__) && !defined(ARX_EDITION_RX)  // (Play RX shows video on Linux too)
  bool timing_only = true;  // the Pi: timing only and portable by default (easy to turn off from a PC, hard to turn on from a phone)
#else
  bool timing_only = false;
#endif
  // timing only, experimental: let one adapter take 3 neighbouring Raceband pilots (74 MHz from the lowest to the
  // highest: the outer two at the very edges of the adapter's ~77 MHz view, weaker and partly cut off)
  bool timing_wide = false;
  // portable mode: a Raspberry Pi taken outside, run from phones (the field page, /field): lap timing only, the
  // voice spoken by the phones. Turning it on also turns timing only on.
#if defined(__linux__) && !defined(ARX_EDITION_RX)
  bool portable = true;
#else
  bool portable = false;
#endif
  // gate check callouts: full (every pass) | brief (each pilot once) | minimal
  bool call_laps = true;    // speak each lap during the race
  bool comb = true;         // picture: comb filter (no ringing) vs the classic notch (DecoderOpts::luma_filter via ARX_LUMA)
  int msps = 20;            // receiver sample rate while watching: 20 (robust on Windows USB) or 40 (wider, cleaner)
  bool diversity = false;   // video from both of each adapter's antennas (A + B combined), like diversity goggles
  bool repair = false;      // sparkle repair: FM clicks patched over, badly hit lines kept from the last field
  bool offset_tune = true;  // tune each receiver ~4 MHz beside its pilot (off the chip's DC point: no edge stripes)
  bool acomb = true;        // adaptive comb: clean OSD text (no halo, no rainbow edges); NTSC with the comb filter on
  bool smooth_lines = true; // each field's lines in place, the lines between interpolated (no stair-steps, no bobbing)
  bool hd = false;          // HD picture on every feed (Live view's HD button): steady picture, channel filter
  bool sharp_osd = true;    // Sharp OSD (Settings -> Picture): clean colour at sharp edges + still weave (full vertical detail where still)
  bool usb3_recovery = true; // USB 3 recovery (Settings -> Receivers): free a locked-up adapter USB link in place (wsdr::set_usb3_recovery)
  // a race without a heat loaded (Start race on whoever is in the slots): its format
  std::string quick_mode = "laps";  // laps (first to N, the default) | practice | timed | best3
  int quick_laps = 3;
  int quick_time = 120;  // s
  bool quick_cap = false;  // first to N laps: the race also stops at quick_time (whoever is ahead then wins)
  int64_t pin_league = 0;  // the league pinned to the Dashboard (0 none)
  // OBS scene switching (obs-websocket, built into OBS 28+), driven from the app window
  bool obs_on = false;
  std::string obs_url = "ws://localhost:4455";
  // FPV Trackside: every pilot's video as an RTMP stream to rtmp://<host>:<port + slot - 1> (Trackside's RTMP Server
  // sources listen on 19301, 19302, ...)
  bool ts_on = false;
  std::string ts_host = "127.0.0.1";
  int ts_port = 19301, ts_fps = 30, ts_kbps = 2500;
  int vcam_mask = 0;  // virtual cameras other programs list: bit n = "<product> Pilot n" (none until switched on)
  bool ts_bars = false;  // the 4:3 picture with black bars in the 16:9 stream (off: filling it; Trackside's 4:3 views)
  std::string obs_pass;  // handed only to the app window (a bound function), never over HTTP
  std::string scene_pre, scene_race, scene_results;  // "" = don't switch for that phase
  // RotorHazard does the lap timing (Play League hosts the race)
  bool rh_on = false;
  std::string rh_url = "http://localhost:5000";
  std::string rh_user, rh_pass;  // RH's admin login (race control, frequencies); never sent to the UI
  bool rh_sync = true;           // set RH's node frequencies to the slots' channels
  bool rh_control = true;        // stage / stop RH's race with ours (laps mode)
  // feed node: this PC lends its adapters to a race PC on the network (src/capture/net_feed.h)
  bool node_mode = false;
  std::string node_name;  // what race PCs list it as ("" = the computer name)
  std::string nodes;      // race PC: feed nodes added by hand, "host:port" comma separated
  std::string rh_mode = "laps";  // laps: RH's laps | crossings: RH's raw gate crossings, timed here
  bool lowlat_solo = false;      // low latency mode: pause the other feeds while it is on
} g_settings;
constexpr const char *kAllBands = "ABEFRL";

// The channel a frequency is: `prefer` when it names one within 3 MHz (R7 and F8 are both 5880), else the
// nearest in an enabled band, else the nearest in any band. "" when none is within 3 MHz.
std::string channel_name(double mhz, const std::string &prefer = {}) {
  const kp::FpvChannel *best = nullptr;
  auto pick = [&](bool enabled_only) {
    for (const auto &c : kp::fpv_channels()) {
      if (std::fabs(c.mhz - mhz) > 3) continue;
      if (enabled_only && g_settings.bands.find(c.name[0]) == std::string::npos) continue;
      if (!best || std::fabs(c.mhz - mhz) < std::fabs(best->mhz - mhz)) best = &c;
    }
  };
  for (const auto &c : kp::fpv_channels())
    if (prefer == c.name && std::fabs(c.mhz - mhz) <= 3) return c.name;
  pick(true);
  if (!best) pick(false);
  return best ? best->name : "";
}

std::wstring exe_dir() {
#ifdef _WIN32
  wchar_t p[MAX_PATH];
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p;
#else
  char buf[4096];
  uint32_t n = sizeof buf;
#ifdef __APPLE__
  std::wstring s = _NSGetExecutablePath(buf, &n) == 0 ? widen(std::filesystem::canonical(buf).string()) : L"./x";
#else
  (void)n;
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  std::wstring s = ec ? std::wstring(L"./x") : widen(self.string());
  (void)buf;
#endif
#endif
  return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// ---------------------------------------------------------------- log
struct LogRing {
  std::mutex m;
  std::deque<std::string> lines;
  uint64_t seq = 0;  // lines ever logged; lines.back() is number seq
} g_log;

void log_sink(kp::LogLevel l, const char *line) {
  static const char *tag[] = {"D", "I", "W", "E"};
  std::lock_guard<std::mutex> g(g_log.m);
  g_log.lines.push_back(std::string(tag[static_cast<int>(l)]) + " " + line);
#ifdef __linux__
  std::fprintf(stderr, "%s %s\n", tag[static_cast<int>(l)], line);  // the service's journal
#endif
  while (g_log.lines.size() > 500) g_log.lines.pop_front();
  g_log.seq++;
}

// ---------------------------------------------------------------- state
HWND g_engine = nullptr;  // message-only window: timers + calls from the HTTP threads
constexpr UINT WM_APP_OPENED = WM_APP + 2;    // wParam = slot (0 = the scanner): its open() finished
constexpr UINT WM_APP_ADAPTERS = WM_APP + 3;  // the adapter list was refreshed
constexpr UINT WM_APP_CALL = WM_APP + 4;      // lParam = std::function<void()>*: run it on the engine thread
constexpr UINT_PTR kTimerStatus = 1;          // 250 ms: radio status, adapter assignment
constexpr UINT_PTR kTimerTiming = 2;          // 50 ms: levels -> lap detectors
#ifdef __linux__
// Headless Linux waits for SIGINT / SIGTERM with sigwait() in main(): block them before ANY thread starts (the
// globals below start threads, e.g. the voice), so every thread inherits the mask and the signal reaches main.
const bool g_signals_blocked = [] {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  return pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0;
}();
#endif
arx::FeedServer g_feeds;
arx::LiveOut g_live;  // FPV Trackside / RTMP: every pilot's video as a stream
#ifdef _WIN32
arx::VCamWriter g_vcam;  // virtual cameras: every pilot as its own video device ("Play League Pilot 1" ...)
arx::LowLatWindow g_lowlat;  // low latency mode: one pilot in its own Direct3D window, drawn line by line
#endif
arx::Timing g_timing;

// A live event for the UI and the overlays (/ws/events): {"type":"...", ...extra}
[[maybe_unused]] void emit(const char *type, const std::string &extra = {}) {  // (Play RX: nothing to announce)
  g_feeds.publish(std::string("{\"type\":\"") + type + "\"" + extra + "}");
}

// The two products built from this file: Play League (receivers + racing) and Play RX (receivers only:
// feeds to the app window and OBS; no timing, race, race start or voice).
#ifdef ARX_EDITION_RX
constexpr bool kRx = true;
constexpr const char *kProduct = "Play RX";
#else
constexpr bool kRx = false;
constexpr const char *kProduct = "Play League";
#endif

// the release, from the VERSION file (CMake passes it in)
#ifndef ARX_VERSION
#define ARX_VERSION "dev"
#endif
constexpr const char *kVersion = ARX_VERSION;
// A radio being opened on a worker thread (open() blocks ~5 s for chip bring-up).
struct Rx {
  std::unique_ptr<kp::Radio> radio;
  std::thread opener;
  std::atomic<bool> opening{false};
  std::string adapter;  // the adapter id it holds ("" = none)
  std::string error;
  std::string status;   // Radio::status_json(), polled
  int64_t retry_ms = 0; // don't try to (re)open before this
};

struct Slot {
  std::string name;
  double mhz = 5769;
  std::string channel;  // the channel the pilot picked ("R7"), "" = whatever the frequency is
  std::string ch() const { return channel_name(mhz, channel); }
  int64_t pilot_id = 0;  // from the pilot list (a loaded heat); 0 = a name typed into the slot
  // which slot this radio's pictures go to (changes when a warm spare's radio moves to another slot)
  std::shared_ptr<std::atomic<int>> sink = std::make_shared<std::atomic<int>>(0);
  Rx rx;
  double level_t = 0;        // newest level point handed to g_timing
  uint64_t spec_seq = 0;     // newest spectrum handed over
  uint64_t cfg_seen = 0;     // g_timing config version already applied to the radio (its gain)
  uint64_t cfg_saved = 0;    // g_timing config version already in pilots.cfg (any slot, with or without a radio)
  double gain_applied = -1;  // the gain the radio runs with (< 0 = AGC)
  // timing only: the slot whose radio times this pilot (itself when it holds the adapter; 0 = not grouped
  // yet) and this pilot's channel in that radio's list
  int lead = 0, lead_ch = 0;
};

struct App {
  std::unique_ptr<Slot> slots[kMaxSlots + 1];
  Rx scanner;  // a spare adapter scanning for transmitters
  std::vector<kp::Pilot> found;
  bool scan_running = false;

  std::mutex adapters_m;
  std::vector<arx::AdapterInfo> adapters, adapters_new;
  std::string adapters_log;
  std::thread usb_thread;
  std::atomic<bool> usb_busy{false};

  bool sim = false;
  int sim_adapters = 4;
  std::vector<std::string> sim_files;
  bool sim_two = false;  // --sim-two-antennas: path B a second video antenna (fading), not a gate antenna
  double sim_snr = 12;   // --sim-snr DB: its mean SNR
  int port = 8090;
  bool lan = false, devtools = false;
  std::string feed_error;
  std::filesystem::path web_dir;
} g_app;

// ---------------------------------------------------------------- parked slots
// A slot nobody flies in right now (a smaller heat, a removed pilot) is parked, not closed: its radio keeps
// running so the next pilot in it starts at once instead of a fresh adapter bring-up (~5 s, much longer on
// some Windows PCs). Every adapter without a pilot is also opened into a parked spare slot. Parked slots are
// hidden from the UI, the OBS feeds and races; the scanner may borrow a spare's adapter.
std::atomic<bool> g_parked[kMaxSlots + 1];
bool parked(int n) { return g_parked[n].load(); }
bool active(int n) { return g_app.slots[n] && !parked(n); }
// RotorHazard times the races and Timing only is on: no video wanted and the adapters' timing is not used, so
// the adapters stay closed (a race needs no adapter at all)
bool rh_only() { return !kRx && g_settings.rh_on && g_settings.timing_only; }

// The adapter chosen for slot n in Slots ("" = Auto: whichever is free). Kept by slot number, like the slot's OBS
// feed and gate tuning, so it stays when a warm spare's radio is handed over. An adapter is identified by its USB
// port (two identical adapters report the same serial), so a choice means "the adapter in that port".
std::string g_pin[kMaxSlots + 1];
// The slot's video from a capture device (a USB video receiver, a capture card): its id, "" = from the adapter.
// The adapter (if one is free) then only times laps; with RotorHazard timing the slot needs no adapter at all.
std::string g_vsrc[kMaxSlots + 1];
std::atomic<bool> g_ext[kMaxSlots + 1];  // g_vsrc[n] non-empty (read by the decoder threads)
arx::Capture g_cap[kMaxSlots + 1];
int64_t g_cap_retry[kMaxSlots + 1];
std::mutex g_capdevs_m;
std::vector<arx::CaptureDevice> g_capdevs;  // the last list (names for the UI)
bool ext_video(int n) { return g_ext[n].load(); }
std::string capture_name(const std::string &id) {
  {
    std::lock_guard<std::mutex> l(g_capdevs_m);
    for (const auto &d : g_capdevs)
      if (d.id == id) return d.name;
  }
  std::string h;
  int p;
  return arx::parse_net_source(id, h, p) ? h + ":" + std::to_string(p) : "";
}
// Shared cameras (src/capture/net_feed.h). Race PC: a slot whose video source is "net:<host>:<port>" takes its
// pictures and gate levels from a feed node (no adapter of its own). Feed node: the slots handed to race PCs
// (key = which race PC slot asked), and each slot's status for them.
arx::NetFeed g_net[kMaxSlots + 1];
bool net_video(int n) { return n >= 1 && n <= kMaxSlots && ext_video(n) && g_vsrc[n].compare(0, 4, "net:") == 0; }
std::string g_node_key[kMaxSlots + 1];
int64_t g_node_idle[kMaxSlots + 1];
std::mutex g_node_stat_m;
std::string g_node_stat[kMaxSlots + 1];
std::mutex g_nodes_m;
std::string g_nodes_manual;  // g_settings.nodes, for the server threads
std::string host_name() {  // what race PCs list a feed node as, unless named in Settings
#ifdef _WIN32
  wchar_t b[MAX_COMPUTERNAME_LENGTH + 1] = {0};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameW(b, &n) && n) return narrow(b);
#else
  char b[256] = {0};
  if (gethostname(b, sizeof b - 1) == 0 && b[0]) return b;
#endif
  return "Feed node";
}
// names given to adapters (USB port id -> "Gate left"), adapters.cfg in the data folder
std::map<std::string, std::string> g_adapter_names;
bool pinned_by_other(const std::string &id, int n) {
  for (int i = 1; i <= kMaxSlots; i++)
    if (i != n && active(i) && g_pin[i] == id) return true;
  return false;
}
std::string adapter_label(const std::string &id) {
  const auto it = g_adapter_names.find(id);
  return it != g_adapter_names.end() && !it->second.empty() ? it->second : id;
}
void tune_slot(int n);  // (re)tune slot n's radio: video, or timing only (below)

// ---------------------------------------------------------------- persistence
// The apps keep their files in the user's profile, %APPDATA%\Play League (or Play RX), because an installed
// exe in Program Files cannot write next to itself. The simulated builds (development) stay next to the exe.
// Play RX keeps its own feeds and settings, so it never touches Play League's pilots.
std::filesystem::path data_dir() {
  static std::filesystem::path d;
  if (!d.empty()) return d;
  if (g_app.sim) return d = std::filesystem::path(exe_dir());
#ifdef _WIN32
  const wchar_t *ad = _wgetenv(L"APPDATA");
  d = std::filesystem::path(ad && *ad ? std::wstring(ad) : exe_dir()) / (kRx ? L"Play RX" : L"Play League");
#else
#ifdef __APPLE__
  // macOS: ~/Library/Application Support/Play League (or Play RX)
  d = std::filesystem::path(home_dir()) / "Library" / "Application Support" / (kRx ? "Play RX" : "Play League");
#else
  // Linux: $XDG_DATA_HOME/play-league (or play-rx), normally ~/.local/share/play-league
  const char *xdg = std::getenv("XDG_DATA_HOME");
  d = (xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home_dir()) / ".local" / "share") /
      (kRx ? "play-rx" : "play-league");
#endif
#endif
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return d;
}
std::filesystem::path config_path() {
  if (kRx) return data_dir() / (g_app.sim ? L"playrx_feeds_sim.cfg" : L"playrx_feeds.cfg");
  return data_dir() / (g_app.sim ? L"pilots_sim.cfg" : L"pilots.cfg");
}
std::filesystem::path settings_path() {
  if (kRx) return data_dir() / (g_app.sim ? L"playrx_settings_sim.cfg" : L"playrx_settings.cfg");
  return data_dir() / (g_app.sim ? L"settings_sim.cfg" : L"settings.cfg");
}

// A whole file written safely: into a temporary file next to it, flushed to the disk, then swapped in. A power cut
// mid-write (the Pi is switched off by pulling the plug) leaves the old file, never an empty one.
bool write_file_safe(const std::filesystem::path &to, const std::string &text) {
  std::filesystem::path tmp = to;
  tmp += L".tmp";
#ifdef _WIN32
  FILE *f = _wfopen(tmp.wstring().c_str(), L"wb");
#else
  FILE *f = std::fopen(tmp.string().c_str(), "wb");
#endif
  if (!f) return false;
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = std::fclose(f) == 0 && ok;
  if (ok) {
#ifdef _WIN32
    ok = MoveFileExW(tmp.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = std::rename(tmp.string().c_str(), to.string().c_str()) == 0;
#endif
  }
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    KP_LOGW("could not save %s", narrow(to.wstring()).c_str());
  }
  return ok;
}

void save_settings() {
  const Settings &s = g_settings;
  std::ostringstream f;
  f << "# AnalogRX settings\nbands=" << s.bands << "\nlan=" << s.lan << "\nguided=" << s.guided << "\nauto_gate=" << s.auto_gate << "\npasses=" << s.passes
    << "\nvoice=" << s.voice << "\nvolume=" << s.volume << "\nrate=" << s.rate << "\npitch=" << s.pitch << "\nvoice_id=" << s.voice_id << "\nprecheck=" << s.precheck << "\ntiming_only=" << s.timing_only << "\nts_on=" << s.ts_on << "\nts_host=" << s.ts_host << "\nts_port=" << s.ts_port << "\nts_fps=" << s.ts_fps << "\nts_kbps=" << s.ts_kbps << "\nts_bars=" << s.ts_bars << "\nvcam_mask=" << s.vcam_mask << "\ntiming_wide=" << s.timing_wide << "\nportable=" << s.portable << "\ncall_laps=" << s.call_laps << "\nmsps=" << s.msps << "\ndiversity=" << s.diversity << "\nrepair=" << s.repair << "\noffset_tune=" << s.offset_tune << "\nacomb=" << s.acomb << "\nsmooth_lines=" << s.smooth_lines << "\nhd=" << s.hd << "\nsharp_osd=" << s.sharp_osd << "\nusb3_recovery=" << s.usb3_recovery << "\ncomb=" << s.comb << "\nquick_mode=" << s.quick_mode << "\nquick_laps=" << s.quick_laps
    << "\nquick_time=" << s.quick_time << "\nquick_cap=" << s.quick_cap << "\npin_league=" << s.pin_league << "\nobs_on=" << s.obs_on << "\nobs_url=" << s.obs_url << "\nobs_pass=" << s.obs_pass
    << "\nscene_pre=" << s.scene_pre << "\nscene_race=" << s.scene_race << "\nscene_results=" << s.scene_results
    << "\nrh_on=" << s.rh_on << "\nrh_url=" << s.rh_url << "\nrh_user=" << s.rh_user << "\nrh_pass=" << s.rh_pass << "\nrh_sync=" << s.rh_sync
    << "\nrh_control=" << s.rh_control << "\nrh_mode=" << s.rh_mode << "\nnode_mode=" << s.node_mode << "\nnode_name=" << s.node_name
    << "\nnodes=" << s.nodes << "\nlowlat_solo=" << s.lowlat_solo << "\n";
  write_file_safe(settings_path(), f.str());
}

void load_settings() {
  std::ifstream f(settings_path());
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "bands") {
      std::string b;
      for (char c : std::string(kAllBands))
        if (v.find(c) != std::string::npos) b += c;
      if (!b.empty()) g_settings.bands = b;
    } else if (k == "lan") {
      g_settings.lan = v == "1";
    } else if (k == "guided") {
      g_settings.guided = v == "1";
    } else if (k == "auto_gate") {
      g_settings.auto_gate = v == "1";
    } else if (k == "diversity") {
      g_settings.diversity = v == "1";
    } else if (k == "repair") {
      g_settings.repair = v == "1";
    } else if (k == "offset_tune") {
      g_settings.offset_tune = v == "1";
    } else if (k == "acomb") {
      g_settings.acomb = v == "1";
    } else if (k == "smooth_lines") {
      g_settings.smooth_lines = v == "1";
    } else if (k == "hd") {
      g_settings.hd = v == "1";
    } else if (k == "sharp_osd") {
      g_settings.sharp_osd = v == "1";
    } else if (k == "usb3_recovery") {
      g_settings.usb3_recovery = v == "1";
    } else if (k == "passes") {
      g_settings.passes = std::clamp(std::atoi(v.c_str()), 1, 5);
    } else if (k == "voice") {
      g_settings.voice = v == "1";
    } else if (k == "volume") {
      g_settings.volume = std::clamp(std::atoi(v.c_str()), 0, 100);
    } else if (k == "rate") {
      g_settings.rate = std::clamp(std::atoi(v.c_str()), -5, 5);
    } else if (k == "pitch") {
      g_settings.pitch = std::clamp(std::atoi(v.c_str()), -10, 10);
    } else if (k == "voice_id") {
      g_settings.voice_id = v;
    } else if (k == "timing_only") {
      g_settings.timing_only = v == "1";
    } else if (k == "timing_wide") {
      g_settings.timing_wide = v == "1";
    } else if (k == "portable") {
      g_settings.portable = v == "1";
    } else if (k == "precheck") {
      if (v == "full" || v == "brief" || v == "minimal") g_settings.precheck = v;
    } else if (k == "call_laps") {
      g_settings.call_laps = v == "1";
    } else if (k == "msps") {
      g_settings.msps = std::atoi(v.c_str()) == 40 ? 40 : 20;
    } else if (k == "comb") {
      g_settings.comb = v == "1";
    } else if (k == "quick_mode") {
      if (v == "practice" || v == "laps" || v == "timed" || v == "best3") g_settings.quick_mode = v;
    } else if (k == "quick_laps") {
      g_settings.quick_laps = std::clamp(std::atoi(v.c_str()), 1, 20);
    } else if (k == "pin_league") {
      g_settings.pin_league = std::atoll(v.c_str());
    } else if (k == "quick_time") {
      g_settings.quick_time = std::clamp(std::atoi(v.c_str()), 10, 900);
    } else if (k == "quick_cap") {
      g_settings.quick_cap = v == "1";
    } else if (k == "obs_on") {
      g_settings.obs_on = v == "1";
    } else if (k == "ts_on") {
      g_settings.ts_on = v == "1";
    } else if (k == "ts_host") {
      if (!v.empty()) g_settings.ts_host = v;
    } else if (k == "ts_port") {
      g_settings.ts_port = std::clamp(std::atoi(v.c_str()), 1, 65000);
    } else if (k == "ts_fps") {
      g_settings.ts_fps = std::atoi(v.c_str()) >= 50 ? 60 : 30;
    } else if (k == "vcam_mask") {
      g_settings.vcam_mask = std::atoi(v.c_str()) & 0x1FE;
    } else if (k == "ts_bars") {
      g_settings.ts_bars = v == "1";
    } else if (k == "ts_kbps") {
      g_settings.ts_kbps = std::clamp(std::atoi(v.c_str()), 300, 20000);
    } else if (k == "obs_url") {
      if (!v.empty()) g_settings.obs_url = v;
    } else if (k == "obs_pass") {
      g_settings.obs_pass = v;
    } else if (k == "scene_pre") {
      g_settings.scene_pre = v;
    } else if (k == "scene_race") {
      g_settings.scene_race = v;
    } else if (k == "scene_results") {
      g_settings.scene_results = v;
    } else if (k == "node_mode") {
      g_settings.node_mode = v == "1";
    } else if (k == "node_name") {
      g_settings.node_name = v;
    } else if (k == "nodes") {
      g_settings.nodes = v;
    } else if (k == "rh_on") {
      g_settings.rh_on = v == "1";
    } else if (k == "rh_url") {
      if (!v.empty()) g_settings.rh_url = v;
    } else if (k == "rh_user") {
      g_settings.rh_user = v;
    } else if (k == "rh_pass") {
      g_settings.rh_pass = v;
    } else if (k == "rh_sync") {
      g_settings.rh_sync = v == "1";
    } else if (k == "rh_control") {
      g_settings.rh_control = v == "1";
    } else if (k == "rh_mode") {
      g_settings.rh_mode = v == "crossings" ? "crossings" : "laps";
    } else if (k == "lowlat_solo") {
      g_settings.lowlat_solo = v == "1";
    }
  }
}

std::string settings_json() {
  const Settings &s = g_settings;
  auto b = [](bool v) { return v ? "true" : "false"; };
  std::ostringstream o;
  o << "{\"bands\":[";
  for (size_t i = 0; i < s.bands.size(); i++) o << (i ? "," : "") << "\"" << s.bands[i] << "\"";
  o << "],\"lan\":" << b(s.lan) << ",\"lan_active\":" << b(g_app.lan) << ",\"port\":" << g_feeds.port()
    << ",\"guided\":" << b(s.guided) << ",\"auto_gate\":" << b(s.auto_gate)
    << ",\"passes\":" << s.passes << ",\"voice\":" << b(s.voice)
    << ",\"volume\":" << s.volume << ",\"rate\":" << s.rate << ",\"pitch\":" << s.pitch << ",\"voice_id\":" << q(s.voice_id) << ",\"precheck\":" << q(s.precheck) << ",\"timing_only\":" << b(s.timing_only) << ",\"timing_wide\":" << b(s.timing_wide) << ",\"portable\":" << b(s.portable) << ",\"call_laps\":" << b(s.call_laps) << ",\"msps\":" << s.msps << ",\"diversity\":" << b(s.diversity) << ",\"repair\":" << b(s.repair) << ",\"offset_tune\":" << b(s.offset_tune) << ",\"acomb\":" << b(s.acomb) << ",\"smooth_lines\":" << b(s.smooth_lines) << ",\"hd\":" << b(s.hd) << ",\"sharp_osd\":" << b(s.sharp_osd) << ",\"usb3_recovery\":" << b(s.usb3_recovery) << ",\"comb\":" << b(s.comb) << ",\"quick_mode\":" << q(s.quick_mode)
    << ",\"quick_laps\":" << s.quick_laps << ",\"quick_time\":" << s.quick_time << ",\"quick_cap\":" << b(s.quick_cap) << ",\"pin_league\":" << s.pin_league << ",\"obs_on\":" << b(s.obs_on)
    << ",\"ts_on\":" << b(s.ts_on) << ",\"ts_host\":" << q(s.ts_host) << ",\"ts_port\":" << s.ts_port << ",\"ts_fps\":" << s.ts_fps << ",\"ts_kbps\":" << s.ts_kbps << ",\"ts_bars\":" << b(s.ts_bars) << ",\"vcam_mask\":" << s.vcam_mask
    << ",\"obs_url\":" << q(s.obs_url) << ",\"obs_pass_set\":" << b(!s.obs_pass.empty()) << ",\"scene_pre\":" << q(s.scene_pre)
    << ",\"scene_race\":" << q(s.scene_race) << ",\"scene_results\":" << q(s.scene_results)
    << ",\"rh_on\":" << b(s.rh_on)
    << ",\"rh_url\":" << q(s.rh_url) << ",\"rh_user\":" << q(s.rh_user) << ",\"rh_pass_set\":" << b(!s.rh_pass.empty())
    << ",\"rh_sync\":" << b(s.rh_sync) << ",\"rh_control\":" << b(s.rh_control) << ",\"rh_mode\":" << q(s.rh_mode)
    << ",\"node_mode\":" << b(s.node_mode) << ",\"node_name\":" << q(s.node_name) << ",\"nodes\":" << q(s.nodes) << "}";
  return o.str();
}

void publish_info(int n);
void node_apply();
std::string clean_name(std::string s);
void live_apply();
void vcam_apply();
void rh_apply();
void apply_voice_settings();
void apply_sample_rate();

// {"bands":["R","F"],"lan":true}: partial updates, only the keys present
void apply_settings(const std::string &body) {
  const size_t p = body.find("\"bands\":[");
  if (p != std::string::npos) {
    const std::string list = body.substr(p + 9, body.find(']', p) - p - 9);
    std::string b;
    for (char c : std::string(kAllBands))
      if (list.find(std::string("\"") + c + "\"") != std::string::npos) b += c;
    if (!b.empty()) g_settings.bands = b;  // at least one band stays on
  }
  Settings &s = g_settings;
  if (jhas(body, "lan")) s.lan = jbool(body, "lan");
  if (jhas(body, "guided")) s.guided = jbool(body, "guided");
  if (jhas(body, "auto_gate")) {
    s.auto_gate = jbool(body, "auto_gate");
    KP_LOGI("automatic gate tuning %s", s.auto_gate ? "on" : "off");
  }
  if (jhas(body, "smooth_lines")) {
    s.smooth_lines = jbool(body, "smooth_lines");
    kp::set_smooth_lines(s.smooth_lines);
    KP_LOGI("smooth lines %s", s.smooth_lines ? "on" : "off");
  }
  if (jhas(body, "sharp_osd")) {
    s.sharp_osd = jbool(body, "sharp_osd");
    KP_LOGI("sharp OSD %s", s.sharp_osd ? "on" : "off");  // the radios pick it up on the next tick
  }
  if (jhas(body, "usb3_recovery")) {
    s.usb3_recovery = jbool(body, "usb3_recovery");
    KP_LOGI("USB 3 recovery %s", s.usb3_recovery ? "on" : "off");  // each receiver picks it up at its next stream start
  }
  if (jhas(body, "hd")) {
    s.hd = jbool(body, "hd");
    KP_LOGI("HD picture %s", s.hd ? "on" : "off");  // the radios pick it up on the next tick
  }
  if (jhas(body, "acomb")) {
    s.acomb = jbool(body, "acomb");
    kp::set_adaptive_comb(s.acomb);
    KP_LOGI("adaptive comb %s", s.acomb ? "on" : "off");
  }
  if (jhas(body, "offset_tune")) {
    s.offset_tune = jbool(body, "offset_tune");
    KP_LOGI("tuning beside the channel %s", s.offset_tune ? "on" : "off");
  }
  if (jhas(body, "repair")) {
    s.repair = jbool(body, "repair");
    kp::set_repair(s.repair);
    KP_LOGI("sparkle repair %s", s.repair ? "on" : "off");
  }
  if (jhas(body, "diversity")) {
    const bool was = s.diversity;
    s.diversity = jbool(body, "diversity");
    if (s.diversity && !was) {
      // B is now a video antenna, not a gate antenna: time on the stronger of A and B
      for (int i = 1; i <= kMaxSlots; i++) {
        arx::GateConfig c = g_timing.config(i);
        if (c.path == 1) {
          c.path = 2;
          g_timing.set_config(i, c);
        }
      }
    }
    KP_LOGI("diversity %s: video from %s", s.diversity ? "on" : "off", s.diversity ? "both antennas (A + B combined)" : "antenna A");
  }
  if (jhas(body, "passes")) s.passes = std::clamp(static_cast<int>(jnum(body, "passes")), 1, 5);
  if (jhas(body, "voice")) s.voice = jbool(body, "voice");
  if (jhas(body, "volume")) s.volume = std::clamp(static_cast<int>(jnum(body, "volume")), 0, 100);
  if (jhas(body, "rate")) s.rate = std::clamp(static_cast<int>(jnum(body, "rate")), -5, 5);
  if (jhas(body, "pitch")) s.pitch = std::clamp(static_cast<int>(jnum(body, "pitch")), -10, 10);
  if (jhas(body, "voice_id")) s.voice_id = jstr(body, "voice_id");
  if (jhas(body, "timing_only")) s.timing_only = jbool(body, "timing_only");
  if (jhas(body, "timing_wide")) s.timing_wide = jbool(body, "timing_wide");
  if (jhas(body, "portable") && jbool(body, "portable") != s.portable) {
    s.portable = jbool(body, "portable");
    if (s.portable) s.timing_only = true, s.lan = true;  // no video outside; phones must reach it (lan: at the next start on a PC)
    KP_LOGI("portable mode %s", s.portable ? "on: lap timing only, run from phones (/field)" : "off");
  }
  if (jhas(body, "precheck")) {
    const std::string p = jstr(body, "precheck");
    if (p == "full" || p == "brief" || p == "minimal") s.precheck = p;
  }
  if (jhas(body, "call_laps")) s.call_laps = jbool(body, "call_laps");
  if (jhas(body, "quick_mode")) {
    const std::string m = jstr(body, "quick_mode");
    if (m == "practice" || m == "laps" || m == "timed" || m == "best3") s.quick_mode = m;
  }
  if (jhas(body, "pin_league")) s.pin_league = std::max<int64_t>(0, static_cast<int64_t>(jnum(body, "pin_league")));
  if (jhas(body, "quick_laps")) s.quick_laps = std::clamp(static_cast<int>(jnum(body, "quick_laps")), 1, 20);
  if (jhas(body, "quick_time")) s.quick_time = std::clamp(static_cast<int>(jnum(body, "quick_time")), 10, 900);
  if (jhas(body, "quick_cap")) s.quick_cap = jbool(body, "quick_cap");
  auto line = [](std::string v) {  // one line in settings.cfg
    for (char &c : v)
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    utf8_cut(v, 200);
    return v;
  };
  if (jhas(body, "obs_on")) s.obs_on = jbool(body, "obs_on");
  if (jhas(body, "obs_url")) {
    const std::string u = line(jstr(body, "obs_url"));
    if (u.compare(0, 5, "ws://") == 0 || u.compare(0, 6, "wss://") == 0) s.obs_url = u;
  }
  if (jhas(body, "obs_pass")) s.obs_pass = line(jstr(body, "obs_pass"));
  if (jhas(body, "scene_pre")) s.scene_pre = line(jstr(body, "scene_pre"));
  if (jhas(body, "scene_race")) s.scene_race = line(jstr(body, "scene_race"));
  if (jhas(body, "scene_results")) s.scene_results = line(jstr(body, "scene_results"));
  if (jhas(body, "node_mode") && jbool(body, "node_mode") != s.node_mode) {
    s.node_mode = jbool(body, "node_mode");
    if (s.node_mode) s.lan = true;  // a race PC must be able to reach it (applies at the next start if it was off)
    node_apply();
  }
  if (jhas(body, "node_name")) s.node_name = clean_name(jstr(body, "node_name")), node_apply();
  if (jhas(body, "nodes")) {
    s.nodes = line(jstr(body, "nodes"));
    std::lock_guard<std::mutex> l(g_nodes_m);
    g_nodes_manual = s.nodes;
  }
  if (jhas(body, "ts_on") || jhas(body, "ts_host") || jhas(body, "ts_port") || jhas(body, "ts_fps") || jhas(body, "ts_kbps") || jhas(body, "ts_bars")) {
    if (jhas(body, "ts_bars")) s.ts_bars = jbool(body, "ts_bars");
    if (jhas(body, "ts_on")) s.ts_on = jbool(body, "ts_on");
    if (jhas(body, "ts_host") && !line(jstr(body, "ts_host")).empty()) s.ts_host = line(jstr(body, "ts_host"));
    if (jhas(body, "ts_port")) s.ts_port = std::clamp(static_cast<int>(jnum(body, "ts_port")), 1, 65000);
    if (jhas(body, "ts_fps")) s.ts_fps = jnum(body, "ts_fps") >= 50 ? 60 : 30;
    if (jhas(body, "ts_kbps")) s.ts_kbps = std::clamp(static_cast<int>(jnum(body, "ts_kbps")), 300, 20000);
    live_apply();
  }
  if (jhas(body, "vcam_mask")) {
    s.vcam_mask = static_cast<int>(jnum(body, "vcam_mask")) & 0x1FE;
    vcam_apply();
  }
  bool rh_changed = false;
  if (jhas(body, "rh_on")) rh_changed |= s.rh_on != jbool(body, "rh_on"), s.rh_on = jbool(body, "rh_on");
  if (jhas(body, "rh_url") && !jstr(body, "rh_url").empty()) rh_changed = true, s.rh_url = line(jstr(body, "rh_url"));
  if (jhas(body, "rh_user")) rh_changed = true, s.rh_user = line(jstr(body, "rh_user"));
  if (jhas(body, "rh_pass")) rh_changed = true, s.rh_pass = line(jstr(body, "rh_pass"));
  if (jhas(body, "rh_sync")) s.rh_sync = jbool(body, "rh_sync");
  if (jhas(body, "rh_control")) s.rh_control = jbool(body, "rh_control");
  if (jhas(body, "rh_mode")) s.rh_mode = jstr(body, "rh_mode") == "crossings" ? "crossings" : "laps";
  if (rh_changed) rh_apply();
  if (jhas(body, "comb") && jbool(body, "comb") != s.comb) {
    s.comb = jbool(body, "comb");
#ifdef _WIN32
    _putenv_s("ARX_LUMA", s.comb ? "comb" : "classic");  // every decoder built from now on
#else
    setenv("ARX_LUMA", s.comb ? "comb" : "classic", 1);  // every decoder built from now on
#endif
    apply_sample_rate();                              // retunes: each radio builds a new decoder
  }
  if (jhas(body, "msps")) {
    const int m = static_cast<int>(jnum(body, "msps")) == 40 ? 40 : 20;
    if (m != s.msps) {
      s.msps = m;
      apply_sample_rate();
    }
  }
  apply_voice_settings();
  save_settings();
  for (int i = 1; i <= kMaxSlots; i++) publish_info(i);  // channel names may change with the bands
  KP_LOGI("settings: bands %s, network access %s", g_settings.bands.c_str(), g_settings.lan ? "on" : "off");
}

void save_pilots() {
  std::ostringstream f;
  f << "# AnalogRX pilots: slot<TAB>name<TAB>MHz<TAB>timing input (0 A, 1 B, 2 best)<TAB>enter dB<TAB>exit dB<TAB>min lap s"
       "<TAB>smoothing ms<TAB>gain dB (-1 AGC)<TAB>channel<TAB>threshold mode (0 fixed, 1 relative)<TAB>rise dB<TAB>hysteresis dB"
       "<TAB>pilot list id (0 none)<TAB>chosen adapter (USB port, empty = auto)<TAB>video device (empty = the adapter)\n";
  for (int i = 1; i <= kMaxSlots; i++)
    if (active(i)) {
      const arx::GateConfig c = g_timing.config(i);
      f << i << "\t" << g_app.slots[i]->name << "\t" << g_app.slots[i]->mhz << "\t" << c.path << "\t" << c.enter_db
        << "\t" << c.exit_db << "\t" << c.min_lap_s << "\t" << c.smooth_ms << "\t" << c.gain_db << "\t"
        << g_app.slots[i]->channel << "\t" << c.mode << "\t" << c.rise_db << "\t" << c.hyst_db << "\t"
        << g_app.slots[i]->pilot_id << "\t" << g_pin[i] << "\t" << g_vsrc[i] << "\n";
    }
  write_file_safe(config_path(), f.str());
}

std::filesystem::path adapters_path() {
  return data_dir() / (kRx ? (g_app.sim ? "playrx_adapters_sim.cfg" : "playrx_adapters.cfg") : (g_app.sim ? "adapters_sim.cfg" : "adapters.cfg"));
}
void save_adapter_names() {
  std::ostringstream f;
  f << "# adapter names: USB port<TAB>name\n";
  for (const auto &[id, nm] : g_adapter_names)
    if (!nm.empty()) f << id << "\t" << nm << "\n";
  write_file_safe(adapters_path(), f.str());
}
void load_adapter_names() {
  std::ifstream f(adapters_path());
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t t = line.find('\t');
    if (t != std::string::npos && t > 0) g_adapter_names[line.substr(0, t)] = line.substr(t + 1);
  }
}

void load_pilots() {
  std::ifstream f(config_path());
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> col;
    std::istringstream ls(line);
    for (std::string c; std::getline(ls, c, '\t');) col.push_back(c);
    if (col.size() < 3) continue;
    const int n = std::atoi(col[0].c_str());
    if (n < 1 || n > kMaxSlots || g_app.slots[n]) continue;
    auto s = std::make_unique<Slot>();
    s->name = col[1];
    s->mhz = std::atof(col[2].c_str());
    if (col.size() >= 9) {
      arx::GateConfig c;
      c.path = std::atoi(col[3].c_str());
      c.enter_db = static_cast<float>(std::atof(col[4].c_str()));
      c.exit_db = static_cast<float>(std::atof(col[5].c_str()));
      c.min_lap_s = std::atof(col[6].c_str());
      c.smooth_ms = std::atof(col[7].c_str());
      c.gain_db = std::atof(col[8].c_str());
      if (col.size() >= 13) {
        c.mode = std::atoi(col[10].c_str());
        c.rise_db = static_cast<float>(std::atof(col[11].c_str()));
        c.hyst_db = static_cast<float>(std::atof(col[12].c_str()));
      }
      g_timing.set_config(n, c);
    }
    if (col.size() >= 10) s->channel = col[9];
    if (col.size() >= 14) s->pilot_id = std::atoll(col[13].c_str());
    g_pin[n] = col.size() >= 15 ? col[14] : "";
    g_vsrc[n] = col.size() >= 16 ? col[15] : "";
    g_ext[n] = !g_vsrc[n].empty();
    s->cfg_seen = s->cfg_saved = g_timing.config_version(n);
    g_app.slots[n] = std::move(s);
  }
}

// ---------------------------------------------------------------- feeds
void publish_info(int n) {
  const Slot *s = g_app.slots[n].get();
  if (!s || parked(n)) {
    g_feeds.clear(n);
    return;
  }
  arx::SlotInfo in;
  in.used = true;
  in.name = s->name;
  in.mhz = s->mhz;
  in.channel = s->ch();
  if (in.channel.empty()) in.channel = num(s->mhz, 0) + " MHz";
  in.locked = s->rx.radio && s->rx.radio->is_open() && jbool(s->rx.status, "locked");
  if (ext_video(n)) in.locked = net_video(n) ? g_net[n].connected() && now_ms() - g_net[n].last_frame_ms() < 1500  // a feed node: pictures arriving
                                    : g_cap[n].running() && now_ms() - g_cap[n].last_frame_ms() < 1500;  // USB video: pictures arriving
  g_feeds.set_info(n, in);
}

// One picture of slot n (BGRX) to everyone who shows or sends it.
void slot_frame(int n, const uint8_t *bgrx, int w, int h) {
  g_feeds.push(n, bgrx, w, h);
  g_live.frame(n, bgrx, w, h);  // the same for the Trackside stream (only when it is on)
#ifdef _WIN32
  g_vcam.frame(n, bgrx, w, h);  // the virtual camera (a copy into shared memory)
#endif
}

// The decoder threads' frame sink for slot n: RGBA -> BGRX into the feed store.
kp::FrameSink frame_sink(std::shared_ptr<std::atomic<int>> to) {
  return [to](const kp::Frame &f) {
    const int n = to->load();
    if (n < 1 || parked(n) || ext_video(n)) return;  // a warm spare: nobody watches it; or video from a USB device
    thread_local std::vector<uint8_t> bgrx;
    const size_t px = static_cast<size_t>(f.width) * f.height;
    bgrx.resize(px * 4);
    const uint8_t *s = f.data;
    uint8_t *d = bgrx.data();
    for (size_t i = 0; i < px; i++, s += 4, d += 4) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = 0;
    }
    slot_frame(n, bgrx.data(), f.width, f.height);
  };
}

// The decoder threads' line sink for slot n: each line straight to the low latency window when it shows slot n.
kp::LineSink line_sink(std::shared_ptr<std::atomic<int>> to) {
#ifdef _WIN32
  return [to](const kp::Line &l) {
    const int n = to->load();
    if (n < 1 || n != g_lowlat.slot() || parked(n) || ext_video(n)) return;
    g_lowlat.line(n, l);
  };
#else
  (void)to;
  return {};
#endif
}

// Low latency upkeep (every 250 ms): the shown pilot's radio runs in low latency (see Radio::set_low_latency),
// every other radio normally; with "pause other feeds" the other radios idle (no demodulating or decoding: their
// CPU to the low latency pilot) and go back to their channels when it ends.

bool g_solo_idled[kMaxSlots + 1] = {};
void lowlat_tick() {
#ifdef _WIN32
  const int L = g_lowlat.slot();
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    kp::Radio *r = s && s->rx.radio && !s->rx.opening && s->rx.radio->is_open() ? s->rx.radio.get() : nullptr;
    if (r) r->set_low_latency(i == L);
    const bool pause = L && g_settings.lowlat_solo && i != L && r;
    if (pause) {
      if (!r->is_idle()) r->idle();
      if (!g_solo_idled[i]) KP_LOGI("low latency: pilot %d paused", i);
      g_solo_idled[i] = true;
    } else if (g_solo_idled[i]) {
      g_solo_idled[i] = false;
      if (r) {
        KP_LOGI("low latency: pilot %d back", i);
        tune_slot(i);
      }
    }
  }
  if (g_app.scanner.radio && !g_app.scanner.opening) g_app.scanner.radio->set_low_latency(false);
#endif
}

// The low latency window's caption: "Pilot 1 · R4 5865 MHz"
std::string lowlat_title(int n) {
  const Slot *s = n >= 1 && n <= kMaxSlots ? g_app.slots[n].get() : nullptr;
  if (!s) return "Low latency";
  const std::string ch = s->ch().empty() ? num(s->mhz, 0) + " MHz" : s->ch() + " " + num(s->mhz, 0) + " MHz";
  return (s->name.empty() ? "Pilot " + std::to_string(n) : s->name) + "  ·  " + ch;
}

// Capture devices: each slot with one runs it (re-opened 3 s after it fails or is unplugged).
void net_tick();
void capture_tick() {
  net_tick();  // slots fed by a feed node
  const int64_t t = now_ms();
  for (int i = 1; i <= kMaxSlots; i++) {
    const std::string want = active(i) && ext_video(i) && !net_video(i) ? g_vsrc[i] : "";
    arx::Capture &c = g_cap[i];
    if (c.id() != want) {
      if (!c.id().empty()) {
        KP_LOGI("pilot %d: USB video closed", i);
        c.stop();
        if (active(i)) g_feeds.clear(i), publish_info(i);
      }
      if (!want.empty()) {
        KP_LOGI("pilot %d: video from USB device %s", i, capture_name(want).c_str());
        c.start(want, [i](const uint8_t *p, int w, int h) {
          if (!parked(i)) slot_frame(i, p, w, h);
        });
        g_cap_retry[i] = 0;
      }
    } else if (!want.empty() && !c.running()) {
      if (!g_cap_retry[i]) {
        g_cap_retry[i] = t + 3000;
        KP_LOGW("pilot %d: USB video: %s", i, c.error().c_str());
      } else if (t >= g_cap_retry[i]) {
        c.start(want, [i](const uint8_t *p, int w, int h) {
          if (!parked(i)) slot_frame(i, p, w, h);
        });
        g_cap_retry[i] = 0;
      }
    }
  }
}

// Race PC: slots fed by a feed node over the network (net_feed.h). The node is told who flies (name, channel),
// tunes one of its adapters, and streams the pictures and gate levels; lap timing then runs here as usual.
bool update_pilot(int n, const std::string &body);
int add_pilot(const std::string &name, double mhz, const std::string &channel, int64_t pilot_id);
bool remove_pilot(int n);
bool video_ok(int n);
std::string slot_state(const Slot &s);
std::string net_key(int n) { return host_name() + ":" + std::to_string(g_feeds.port()) + "/" + std::to_string(n); }
void net_tick() {
  for (int i = 1; i <= kMaxSlots; i++) {
    const std::string want = active(i) && net_video(i) ? g_vsrc[i] : "";
    arx::NetFeed &f = g_net[i];
    const Slot *s = g_app.slots[i].get();
    if (f.id() != want) {
      if (!f.id().empty()) {
        KP_LOGI("pilot %d: feed node %s let go", i, f.id().c_str());
        f.stop(true);
        if (active(i)) g_feeds.clear(i), publish_info(i);
      }
      if (!want.empty() && s) {
        KP_LOGI("pilot %d: video and timing from feed node %s", i, capture_name(want).c_str());
        f.start(want, net_key(i), s->name, s->mhz, s->ch(), g_timing.config(i).gain_db, [i](const uint8_t *p, int w, int h) { if (!parked(i)) slot_frame(i, p, w, h); },
                [i](const std::vector<arx::LevelPoint> &pts) { if (!parked(i)) g_timing.add_levels(i, pts); });
      }
    } else if (!want.empty() && s) {
      f.set_pilot(s->name, s->mhz, s->ch(), g_timing.config(i).gain_db);  // the node retunes when the pilot, channel or gain changes
    }
  }
}
// what the node says about the slot feeding us: "Node slot 2 · locked"
std::string net_format(int n) {
  const std::string st = g_net[n].node_status();
  if (!g_net[n].node_slot()) return "";
  std::string o = "node slot " + std::to_string(g_net[n].node_slot());
  const std::string state = jstr(st, "state");
  if (!state.empty()) o += " · " + state;
  return o;
}

// Feed node: the race PC tunes the gate (it has the Timing page; a node has none), so its gain drives this adapter
void node_gain(int n, const std::string &body) {
  if (!jhas(body, "gain_db")) return;  // no gain sent: it stays as it is
  arx::GateConfig c = g_timing.config(n);
  const double g = jnum(body, "gain_db");
  if (c.gain_db == g) return;
  c.gain_db = g;
  g_timing.set_config(n, c);  // the engine applies it (and retunes) on its next pass
}
// Feed node: hand a slot (and so an adapter) to a race PC's pilot, or the same one again for the same key
int node_feed(const std::string &key, const std::string &body) {
  for (int i = 1; i <= kMaxSlots; i++)
    if (active(i) && g_node_key[i] == key) {
      update_pilot(i, body);
      node_gain(i, body);
      g_node_idle[i] = 0;
      return i;
    }
  // after a restart of the node: the slot it still has for that pilot
  const std::string nm = clean_name(jstr(body, "name"));
  for (int i = 1; i <= kMaxSlots; i++)
    if (active(i) && g_node_key[i].empty() && g_app.slots[i]->name == nm && g_feeds.node_clients(i) == 0) {
      g_node_key[i] = key;
      update_pilot(i, body);
      node_gain(i, body);
      return i;
    }
  const int n = add_pilot(nm, jnum(body, "mhz"), jstr(body, "channel"), 0);
  if (n) {
    g_node_key[n] = key, g_node_idle[n] = 0;
    node_gain(n, body);
    KP_LOGI("feed node: slot %d to %s (%s)", n, key.c_str(), nm.c_str());
  }
  return n;
}
void node_release(const std::string &key) {
  for (int i = 1; i <= kMaxSlots; i++)
    if (!key.empty() && g_node_key[i] == key) {
      KP_LOGI("feed node: slot %d released by %s", i, key.c_str());
      g_node_key[i].clear();
      if (active(i)) remove_pilot(i);
    }
}
// every second: each slot's status for the race PCs, and slots nobody has streamed for 2 minutes go back
void node_tick() {
  const int64_t t = now_ms();
  for (int i = 1; i <= kMaxSlots; i++) {
    const Slot *s = g_app.slots[i].get();
    std::string st = "{}";
    if (s && active(i))
      st = "{\"name\":" + q(s->name) + ",\"channel\":" + q(s->ch()) + ",\"mhz\":" + num(s->mhz, 0) + ",\"live\":" + (video_ok(i) ? "true" : "false") +
           ",\"state\":" + q(slot_state(*s)) + "}";
    {
      std::lock_guard<std::mutex> l(g_node_stat_m);
      g_node_stat[i] = st;
    }
    if (g_node_key[i].empty() || !g_settings.node_mode) continue;
    if (g_feeds.node_clients(i) > 0) g_node_idle[i] = 0;
    else if (!g_node_idle[i]) g_node_idle[i] = t;
    else if (t - g_node_idle[i] > 120000) {
      KP_LOGI("feed node: slot %d unused for 2 minutes, released", i);
      node_release(g_node_key[i]);
      g_node_idle[i] = 0;
    }
  }
}
std::string ips_json() {  // this PC's network addresses (the feed node page shows where to find it)
  static std::string cached;
  static int64_t at = 0;
  if (now_ms() - at > 10000 || !at) {
    cached.clear();
    for (const auto &ip : arx::local_ips()) cached += (cached.empty() ? "" : ",") + q(ip);
    at = now_ms();
  }
  return cached;
}
void node_apply() {
  arx::node_beacon(g_settings.node_mode, g_feeds.port(), g_settings.node_name.empty() ? host_name() : g_settings.node_name);
  arx::node_listen(!g_settings.node_mode);  // a race PC listens for feed nodes
  KP_LOGI("feed node mode %s", g_settings.node_mode ? "on" : "off");
}

// ---------------------------------------------------------------- adapters and radios
std::set<std::string> adapters_in_use() {
  std::set<std::string> u;
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i] && !g_app.slots[i]->rx.adapter.empty()) u.insert(g_app.slots[i]->rx.adapter);
  if (!g_app.scanner.adapter.empty()) u.insert(g_app.scanner.adapter);
  return u;
}

// A free adapter for slot `for_slot` (0: a spare or the scanner): never one another active slot has chosen
const arx::AdapterInfo *free_adapter(int for_slot = 0) {
  const auto used = adapters_in_use();
  for (const auto &a : g_app.adapters)
    if (!used.count(a.id()) && !pinned_by_other(a.id(), for_slot)) return &a;
  return nullptr;
}
const arx::AdapterInfo *adapter_by_id(const std::string &id) {
  for (const auto &a : g_app.adapters)
    if (a.id() == id) return &a;
  return nullptr;
}

// Refresh the adapter list on a worker thread: switch any USB 2 adapters to USB 3 first, then list.
void refresh_adapters() {
  if (g_app.usb_busy.exchange(true)) return;
  if (g_app.usb_thread.joinable()) g_app.usb_thread.join();
  g_app.usb_thread = std::thread([] {
    std::string log;
    // ask each USB 2 adapter to switch to USB 3 once per port (a port without USB 3 lines never will, and
    // this runs every 5 s); `tried` lives on this worker thread's side only
    static std::set<std::string> tried;
    const auto now_list = arx::list_adapters();
    const size_t before = now_list.size();
    // a port whose adapter is gone is asked again next time: a replug brings the adapter back at USB 2
    for (auto it = tried.begin(); it != tried.end();)
      it = std::none_of(now_list.begin(), now_list.end(), [&](const arx::AdapterInfo &a) { return a.id() == *it; })
               ? tried.erase(it) : std::next(it);
    int asked = 0;
    for (const auto &a : now_list) {
      // speed unknown (Apple silicon): ask anyway; the switch checks the chip's own mode and leaves USB 3 alone
      if ((a.superspeed && a.speed_known) || tried.count(a.id())) continue;
      tried.insert(a.id());
      std::string detail;
      auto r = wsdr::request_usb3(wsdr::kPid8812bu, a.bus, a.port, &detail);
      log += a.id() + ": usb3 switch " + wsdr::usb3_switch_name(r) + (detail.empty() ? "" : " - " + detail) + "\n";
      // "already tried" can be how a chip powers up (seen on a Mac, right after a replug): switch anyway, once per
      // port while the app runs (not again on every replug: a link without USB 3 lines would bounce for ever)
      static std::set<std::string> forced;
      if (r == wsdr::Usb3Switch::AlreadyTried && forced.insert(a.id()).second) {
        detail.clear();
        r = wsdr::request_usb3(wsdr::kPid8812bu, a.bus, a.port, &detail, true);
        log += a.id() + ": usb3 switch (forced) " + wsdr::usb3_switch_name(r) + (detail.empty() ? "" : " - " + detail) + "\n";
      }
      if (r == wsdr::Usb3Switch::Requested) asked++;
    }
    std::vector<arx::AdapterInfo> list;
    if (asked > 0) {
      // the switched adapters drop off the bus and come back as SuperSpeed devices on a new port number; on
      // some controllers that takes well over 4 s. Wait until they are all back (or ~12 s), then list.
      for (int i = 0; i < 24; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        list = arx::list_adapters();
        if (i >= 3 && list.size() >= before) break;
      }
      log += "adapters back after the USB 3 switch: " + std::to_string(list.size()) + " of " + std::to_string(before) + "\n";
    } else {
      list = arx::list_adapters();
    }
    {
      std::lock_guard<std::mutex> l(g_app.adapters_m);
      g_app.adapters_new = std::move(list);
      g_app.adapters_log = log;
    }
    PostMessage(g_engine, WM_APP_ADAPTERS, 0, 0);
  });
}

void start_open(Rx &rx, const arx::AdapterInfo &a, kp::FrameSink sink, int which, kp::LineSink lines = {}) {
  if (rx.opener.joinable()) rx.opener.join();
  rx.radio = std::make_unique<kp::Radio>(std::move(sink), std::move(lines));
  rx.adapter = a.id();
  rx.error.clear();
  rx.opening = true;
  const int bus = a.bus;
  const std::string port = a.port;
  rx.opener = std::thread([&rx, bus, port, which] {
    try {
      rx.radio->open(bus, port);
    } catch (const std::exception &e) {
      rx.error = e.what();
    }
    rx.opening = false;
    PostMessage(g_engine, WM_APP_OPENED, static_cast<WPARAM>(which), 0);
  });
}

void release(Rx &rx) {
  if (rx.opener.joinable()) rx.opener.join();
  if (rx.radio) rx.radio->close();
  rx.radio.reset();
  rx.adapter.clear();
  rx.status.clear();
}

// Slots i and j trade places (their radios, pilots and parked state); each radio's pictures follow.
void swap_slots(int i, int j) {
  std::swap(g_app.slots[i], g_app.slots[j]);
  const bool pi = parked(i), pj = parked(j);
  g_parked[i] = pj;
  g_parked[j] = pi;
  for (int k : {i, j}) {
    g_timing.clear(k);
    Slot *s = g_app.slots[k].get();
    if (!s) continue;
    s->sink->store(k);
    s->cfg_seen = ~0ull;  // re-apply slot k's gate settings (its gain) to this radio
    s->gain_applied = -2;
    s->level_t = 0;
  }
}

// Give slot i (a pilot without a radio) the running radio of a parked spare: an instant start instead of an
// adapter bring-up. The pilot stays in slot i (its OBS feed); the spare takes slot i's place. false: no spare.
bool warm_into(int i) {
  Slot *cur = g_app.slots[i].get();
  if (cur && (cur->rx.radio || cur->rx.opening)) return false;
  int j = 0;
  for (int k = 1; k <= kMaxSlots && !j; k++) {
    const Slot *s = g_app.slots[k].get();
    if (k != i && s && parked(k) && s->rx.radio && !s->rx.opening && s->rx.radio->is_open() &&
        (g_pin[i].empty() ? !pinned_by_other(s->rx.adapter, i) : s->rx.adapter == g_pin[i]))
      j = k;
  }
  if (!j) return false;
  const bool pilot = cur && !parked(i);
  const std::string name = cur ? cur->name : "", channel = cur ? cur->channel : "";
  const double mhz = cur ? cur->mhz : 0;
  const int64_t pid = cur ? cur->pilot_id : 0;
  swap_slots(i, j);
  if (pilot) {
    Slot &s = *g_app.slots[i];
    g_parked[i] = false;
    g_parked[j] = true;
    s.name = name;
    s.pilot_id = pid;
    if (mhz > 0 && mhz != s.mhz) {
      s.mhz = mhz;
      tune_slot(i);
    }
    s.channel = channel_name(s.mhz, channel);
    if (g_app.slots[j]) {
      g_app.slots[j]->name.clear();
      g_app.slots[j]->pilot_id = 0;
    }
  }
  KP_LOGI("slot %d: takes the warm adapter %s of spare slot %d", i, g_app.slots[i]->rx.adapter.c_str(), j);
  publish_info(i);
  publish_info(j);
  return true;
}

void assign_adapters() {
  const int64_t t = now_ms();
  if (rh_only()) {  // RotorHazard without video: every adapter closed (the scanner may still use one)
    for (int i = 1; i <= kMaxSlots; i++) {
      Slot *s = g_app.slots[i].get();
      if (s && s->rx.radio && !s->rx.opening) {
        KP_LOGI("slot %d: adapter %s closed (RotorHazard timing, no video)", i, s->rx.adapter.c_str());
        release(s->rx);
      }
    }
    return;
  }
  // chosen adapters first: a slot on another adapter than its choice gives it up; whoever holds the chosen one
  // (a pilot on Auto, the scanner) gives it up for it (a warm spare holding it hands it over below)
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (!s || !active(i) || g_pin[i].empty() || s->rx.opening) continue;
    if (g_settings.timing_only && s->lead != 0 && s->lead != i) continue;  // the group's lead holds the adapter
    if (ext_video(i) && (g_settings.rh_on || net_video(i))) continue;  // USB video + RotorHazard, or a feed node: no adapter needed
    if (s->rx.radio && s->rx.adapter != g_pin[i]) {
      KP_LOGI("pilot %d: moving to the chosen adapter %s", i, adapter_label(g_pin[i]).c_str());
      release(s->rx);
    }
    if (s->rx.radio || !adapter_by_id(g_pin[i])) continue;
    for (int k = 1; k <= kMaxSlots; k++) {
      Slot *o = g_app.slots[k].get();
      if (k != i && o && active(k) && !o->rx.opening && o->rx.adapter == g_pin[i]) {
        KP_LOGI("pilot %d: gives adapter %s to pilot %d, who chose it", k, adapter_label(g_pin[i]).c_str(), i);
        release(o->rx);
      }
    }
    if (g_app.scanner.adapter == g_pin[i] && !g_app.scanner.opening) {
      release(g_app.scanner);
      g_app.scan_running = false;
      KP_LOGI("scan: stopped, pilot %d chose its adapter", i);
    }
  }
  // USB video pilots last: they only need an adapter for lap timing (none at all with RotorHazard)
  std::vector<int> order;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 1; i <= kMaxSlots; i++)
      if ((g_settings.timing_only ? 0 : ext_video(i) ? 1 : 0) == pass) order.push_back(i);
  for (int i : order) {
    Slot *s = g_app.slots[i].get();
    if (s && active(i) && ext_video(i) && (g_settings.rh_on || net_video(i)) && !g_settings.timing_only) {
      if (s->rx.radio && !s->rx.opening) {
        KP_LOGI("pilot %d: adapter %s freed (video and timing from elsewhere)", i, s->rx.adapter.c_str());
        release(s->rx);
      }
      continue;
    }
    if (!s || parked(i) || s->rx.opening || s->rx.radio || t < s->rx.retry_ms) continue;
    if (g_settings.timing_only && s->lead != 0 && s->lead != i) continue;  // timed by another slot's adapter
    if (warm_into(i)) continue;  // a running spare first: no bring-up wait
    if (!g_pin[i].empty()) {  // its chosen adapter only: wait while it is not plugged in or still busy
      const arx::AdapterInfo *a = adapter_by_id(g_pin[i]);
      if (!a || adapters_in_use().count(a->id())) continue;
      KP_LOGI("pilot %d (%s): its chosen adapter %s", i, s->name.c_str(), adapter_label(a->id()).c_str());
      s->sink->store(i);
      start_open(s->rx, *a, frame_sink(s->sink), i, line_sink(s->sink));
      continue;
    }
    // a pilot outranks the scanner: take its adapter if nothing else is free
    if (!free_adapter(i) && !g_app.scanner.adapter.empty() && !g_app.scanner.opening && !pinned_by_other(g_app.scanner.adapter, i)) {
      release(g_app.scanner);
      g_app.scan_running = false;
      KP_LOGI("scan: stopped, pilot %d needs the adapter", i);
    }
    const arx::AdapterInfo *a = free_adapter(i);
    if (!a) continue;
    KP_LOGI("pilot %d (%s): adapter %s", i, s->name.c_str(), a->id().c_str());
    s->sink->store(i);
    start_open(s->rx, *a, frame_sink(s->sink), i, line_sink(s->sink));
  }
  // then warm spares: parked slots without a radio, and new parked slots for adapters nobody has, while the
  // scanner is not after one
  if (g_app.scanner.radio || g_app.scanner.opening) return;
  for (int i = 1; i <= kMaxSlots; i++) {
    const arx::AdapterInfo *a = free_adapter();
    if (!a) return;
    if (!g_app.slots[i]) {
      auto s = std::make_unique<Slot>();
      s->mhz = 5658;
      s->cfg_seen = g_timing.config_version(i);
      g_parked[i] = true;
      g_app.slots[i] = std::move(s);
    }
    Slot *s = g_app.slots[i].get();
    if (!parked(i) || s->rx.opening || s->rx.radio || t < s->rx.retry_ms) continue;
    KP_LOGI("slot %d: spare, warming adapter %s", i, a->id().c_str());
    s->sink->store(i);
    start_open(s->rx, *a, frame_sink(s->sink), i, line_sink(s->sink));
  }
}

// Park slot n: hidden, its radio kept running. `reset_gate`: the next pilot starts from default gate settings.
void park(int n, bool reset_gate) {
  if (!g_app.slots[n]) return;
  g_parked[n] = true;
  g_app.slots[n]->pilot_id = 0;
  g_feeds.clear(n);
  g_timing.clear(n);
  if (reset_gate) {
    g_timing.set_config(n, arx::GateConfig{});
    g_app.slots[n]->cfg_seen = g_timing.config_version(n);
  }
}

// A parked slot for a pilot: the one with a running radio first, else an empty slot, else any parked one. 0: full.
int take_slot() {
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i] && parked(i) && g_app.slots[i]->rx.radio && !g_app.slots[i]->rx.opening) return i;
  for (int i = 1; i <= kMaxSlots; i++)
    if (!g_app.slots[i]) return i;
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i] && parked(i)) return i;
  return 0;
}

bool scan_possible() {
  if (free_adapter()) return true;
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i] && parked(i) && !g_app.slots[i]->rx.opening && g_app.slots[i]->rx.radio) return true;
  return false;
}

void start_scan() {
  if (g_app.scanner.opening || g_app.scanner.radio) return;
  if (!free_adapter())  // borrow a warm spare's adapter (it comes back as a spare after the scan)
    for (int i = kMaxSlots; i >= 1; i--) {
      Slot *s = g_app.slots[i].get();
      if (!s || !parked(i) || s->rx.opening) continue;
      KP_LOGI("scan: borrowing spare slot %d's adapter %s", i, s->rx.adapter.c_str());
      release(s->rx);
      g_app.slots[i].reset();
      g_parked[i] = false;
      break;
    }
  const arx::AdapterInfo *a = free_adapter();
  if (!a) {
    KP_LOGW("scan: no spare adapter (every adapter is assigned to a pilot)");
    return;
  }
  g_app.found.clear();
  g_app.scan_running = true;
  start_open(g_app.scanner, *a, [](const kp::Frame &) {}, 0);
}

// ---------------------------------------------------------------- pilots
std::string clean_name(std::string s) {
  for (char &c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  utf8_cut(s, 40);
  return s;
}

bool pilot_from_list(int64_t id, std::string &name, std::string &channel);  // (the league section)

int add_pilot(const std::string &name, double mhz, const std::string &channel, int64_t pilot_id = 0) {
  std::string nm = name, pch;
  if (pilot_id > 0 && !pilot_from_list(pilot_id, nm, pch)) pilot_id = 0;
  const int n = take_slot();  // a warm spare if there is one
  if (!n) return 0;
  if (!g_app.slots[n]) {
    auto s = std::make_unique<Slot>();
    s->cfg_seen = g_timing.config_version(n);
    s->mhz = 0;
    g_app.slots[n] = std::move(s);
  }
  Slot *s = g_app.slots[n].get();
  g_parked[n] = false;
  s->name = clean_name(nm);
  if (s->name.empty()) s->name = "Pilot " + std::to_string(n);
  s->pilot_id = pilot_id;
  const double f = mhz > 0 ? mhz : 5658;
  if (f != s->mhz) {
    s->mhz = f;
    tune_slot(n);
  }
  s->channel = channel_name(s->mhz, channel);
  publish_info(n);
  save_pilots();
  assign_adapters();
  KP_LOGI("pilot %d added: %s on %.0f MHz", n, g_app.slots[n]->name.c_str(), g_app.slots[n]->mhz);
  return n;
}

bool update_pilot(int n, const std::string &body) {
  Slot *s = n >= 1 && n <= kMaxSlots ? g_app.slots[n].get() : nullptr;
  if (!s || parked(n)) return false;
  if (jhas(body, "pilot_id")) {  // a pilot from the list (> 0), or a typed name (0)
    const int64_t id = static_cast<int64_t>(jnum(body, "pilot_id"));
    std::string nm, ch;
    if (id > 0 && pilot_from_list(id, nm, ch)) {
      s->name = clean_name(nm);
      s->pilot_id = id;
    } else {
      s->pilot_id = 0;
    }
  }
  if (jhas(body, "name")) {
    const std::string nm = clean_name(jstr(body, "name"));
    if (!nm.empty()) s->name = nm, s->pilot_id = jhas(body, "pilot_id") ? s->pilot_id : 0;  // typed: a guest
  }
  if (jhas(body, "mhz")) {
    const double mhz = jnum(body, "mhz");
    if (mhz > 5000 && mhz < 6100 && mhz != s->mhz) {
      s->mhz = mhz;
      tune_slot(n);
    }
    s->channel = channel_name(s->mhz, jstr(body, "channel"));
  }
  if (jhas(body, "video")) {  // "" = from the adapter, else a capture device id
    const std::string v = jstr(body, "video");
    if (v != g_vsrc[n]) {
      g_vsrc[n] = v;
      g_ext[n] = !v.empty();
      KP_LOGI("pilot %d: video from %s", n, v.empty() ? "the adapter" : capture_name(v).c_str());
      g_feeds.clear(n);
      publish_info(n);
      tune_slot(n);  // the adapter: video again, or timing only
      capture_tick();
      assign_adapters();
    }
  }
  if (jhas(body, "adapter")) {  // "" = Auto, else a USB port id
    const std::string a = jstr(body, "adapter");
    if (a != g_pin[n]) {
      g_pin[n] = a;
      KP_LOGI("pilot %d: adapter %s", n, a.empty() ? "auto" : adapter_label(a).c_str());
      s->rx.retry_ms = 0;
      assign_adapters();
    }
  }
  publish_info(n);
  save_pilots();
  return true;
}

bool remove_pilot(int n) {
  if (n < 1 || n > kMaxSlots || !active(n)) return false;
  park(n, true);  // the radio keeps running as a spare; the next pilot here starts from default gate settings
  g_pin[n].clear();
  g_vsrc[n].clear();
  g_ext[n] = false;
  g_cap[n].stop();
  g_net[n].stop(true);
  save_pilots();
  KP_LOGI("pilot %d removed (slot %d kept warm as a spare)", n, n);
  return true;
}

// ---------------------------------------------------------------- periodic work
void timing_regroup();
const std::string &pilot_name(int n);
void poll_status() {
  node_tick();
  timing_regroup();
  // streams for pilots with video (not spares, not timing-only mode)
  for (int i = 1; i <= kMaxSlots; i++) g_live.set_active(i, active(i) && (!g_settings.timing_only || ext_video(i)));
#ifdef _WIN32
  for (int i = 1; i <= kMaxSlots; i++) g_vcam.set_slot(i, active(i) && (!g_settings.timing_only || ext_video(i)), pilot_name(i));
  // low latency window: its caption follows the pilot's name and channel; it closes when the pilot goes
  if (const int L = g_lowlat.slot()) {
    if (!active(L)) g_lowlat.show(0, "");
    else g_lowlat.set_title(lowlat_title(L));
  }
  lowlat_tick();
#endif
  capture_tick();
  // look for newly plugged (or unplugged) adapters every 5 s: there is no USB hotplug notice on Windows here
  static int64_t next_scan = 0;
  if (now_ms() >= next_scan) {
    next_scan = now_ms() + 5000;
    refresh_adapters();  // no-op while one is still running
  }
  wsdr::set_usb3_recovery(g_settings.usb3_recovery);  // read by each receiver at its next stream start
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (!s) continue;
    if (s->rx.radio && !s->rx.opening) {
      s->rx.status = s->rx.radio->status_json();
      s->rx.radio->set_hd(g_settings.hd);  // HD picture (a radio can move between slots: set every tick)
      s->rx.radio->set_sharp_osd(g_settings.sharp_osd);
      // the adapter went away (unplugged): give it up and look again
      const std::string st = jstr(s->rx.status, "rx_state");
      if (st == "lost" || st == "error") {
        KP_LOGW("pilot %d: adapter %s %s, releasing it", i, s->rx.adapter.c_str(), st.c_str());
        release(s->rx);
        s->rx.retry_ms = now_ms() + 3000;
        refresh_adapters();
      }
    }
    publish_info(i);
  }
  if (g_app.scanner.radio && !g_app.scanner.opening) {
    g_app.scanner.status = g_app.scanner.radio->status_json();
    g_app.found = g_app.scanner.radio->pilots();
    // kepyur's scan ends by watching the strongest pilot: that means the sweep is done
    if (jstr(g_app.scanner.status, "mode") == "watch") {
      KP_LOGI("scan: done, %zu transmitter(s)", g_app.found.size());
      release(g_app.scanner);
      g_app.scan_running = false;
    }
  }
  assign_adapters();
}

void feed_timing() {
  bool save = false;
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (!s) continue;
    // gate settings changed (the tuning view, the gate check): save them, whether or not this slot has a radio of
    // its own (timed by another slot's adapter, a feed node, RotorHazard, an adapter not plugged in)
    if (const uint64_t v = g_timing.config_version(i); v != s->cfg_saved) {
      s->cfg_saved = v;
      if (!parked(i)) save = true;
    }
    // timing only, a pilot timed by another slot's adapter: that radio's level for this pilot's channel
    if (g_settings.timing_only && !parked(i) && s->lead != 0 && s->lead != i) {
      const Slot *ls = g_app.slots[s->lead].get();
      if (!ls || !ls->rx.radio || ls->rx.opening || !ls->rx.radio->is_open()) continue;
      const auto lv = ls->rx.radio->levels_since(s->level_t, s->lead_ch);
      if (!lv.empty()) {
        std::vector<arx::LevelPoint> pts;
        pts.reserve(lv.size());
        for (const auto &p : lv) pts.push_back({p.t_s, p.a_db, p.b_db});
        s->level_t = lv.back().t_s;
        g_timing.add_levels(i, pts);
      }
      continue;
    }
    if (!s->rx.radio || s->rx.opening || !s->rx.radio->is_open()) continue;
    kp::Radio &r = *s->rx.radio;
    if (parked(i)) {  // a spare: drop its levels, nobody times it
      const auto lv = r.levels_since(s->level_t);
      if (!lv.empty()) s->level_t = lv.back().t_s;
      continue;
    }
    const bool timed = g_settings.timing_only || ext_video(i);  // USB video: the adapter only times
    const auto lv = timed ? r.levels_since(s->level_t, s->lead == i ? s->lead_ch : 0) : r.levels_since(s->level_t);
    if (!lv.empty()) {
      std::vector<arx::LevelPoint> pts;
      pts.reserve(lv.size());
      for (const auto &p : lv) pts.push_back({p.t_s, p.a_db, p.b_db});
      s->level_t = lv.back().t_s;
      g_timing.add_levels(i, pts);
    }
    const auto sp = r.spectrum();
    if (sp.seq != s->spec_seq && !sp.db.empty()) {
      s->spec_seq = sp.seq;
      g_timing.set_spectrum(i, sp.centre_hz, sp.fs, sp.db, s->mhz * 1e6);
    }
    if (!timed) r.set_combine(g_settings.diversity);  // Settings -> Diversity (takes effect at once)
    if (r.set_offset_tuning(g_settings.offset_tune)) tune_slot(i);  // Settings -> Tune beside the channel: retune
    // gate settings changed (on the tuning view): apply the gain
    const uint64_t v = g_timing.config_version(i);
    if (v != s->cfg_seen) {
      s->cfg_seen = v;
      const double g = g_timing.config(i).gain_db;
      if (g != s->gain_applied) {
        s->gain_applied = g;
        r.set_gain(g);
        tune_slot(i);  // the gain takes effect on a retune
        KP_LOGI("pilot %d: gain %s", i, g < 0 ? "AGC" : (std::to_string(static_cast<int>(g)) + " dB").c_str());
      }
    }
  }
  if (save) save_pilots();
}

struct Slot;
std::string slot_state(const Slot &s);

// The channels slot n's radio times (timing only): the slots it leads, in channel order
std::vector<double> timed_channels(int n) {
  std::vector<std::pair<int, double>> v;
  for (int i = 1; i <= kMaxSlots; i++) {
    const Slot *s = g_app.slots[i].get();
    if (s && active(i) && s->lead == n) v.push_back({s->lead_ch, s->mhz * 1e6});
  }
  std::sort(v.begin(), v.end());
  std::vector<double> hz;
  for (auto &p : v) hz.push_back(p.second);
  if (hz.empty() && g_app.slots[n]) hz.push_back(g_app.slots[n]->mhz * 1e6);
  return hz;
}

// (Re)tune slot n's radio for what it does now: its pilot's video, or (timing only) its group's channels.
void tune_slot(int n) {
  Slot *s = g_app.slots[n].get();
  if (!s || !s->rx.radio || s->rx.opening || !s->rx.radio->is_open()) return;
  if ((g_settings.timing_only || ext_video(n)) && !parked(n)) {
    if (s->lead != 0 && s->lead != n) return;  // timed by another slot's radio
    s->rx.radio->time_channels(timed_channels(n));  // (USB video pilot: just its own channel)
  } else {
    s->rx.radio->watch(s->mhz * 1e6);
  }
}

// How far apart (lowest to highest) the pilots on one adapter may be: ~64 MHz, or 76 MHz with the experimental
// 3-Raceband-pilots setting (R1 + R2 + R3 span 74 MHz)
double timing_span_hz() { return g_settings.timing_wide ? 76e6 : kp::Radio::kMaxTimedSpanHz; }

// Timing only: pilots sorted by channel, grouped greedily onto adapters (channels within ~64 MHz of each other,
// up to 3 per adapter); each group's lead holds the adapter (one that already has a radio if any), the others
// give theirs up. Video mode: every pilot its own adapter again. Runs every 250 ms; acts only on a change.
void timing_regroup() {
  static std::string last;
  if (rh_only()) {  // no adapters in use: nobody leads a group
    if (last == "R") return;
    last = "R";
    for (int i = 1; i <= kMaxSlots; i++)
      if (Slot *s = g_app.slots[i].get()) s->lead = s->lead_ch = 0;
    KP_LOGI("timing only with RotorHazard: adapters closed, RotorHazard times every pilot");
    return;
  }
  std::string sig = g_settings.timing_only ? "T" : "V";
  if (!g_settings.timing_only) {
    if (last == sig) return;
    last = sig;
    for (int i = 1; i <= kMaxSlots; i++)
      if (Slot *s = g_app.slots[i].get()) {
        s->lead = s->lead_ch = 0;
        tune_slot(i);
      }
    KP_LOGI("timing only: off (video for every pilot)");
    return;
  }
  std::vector<int> act;
  for (int i = 1; i <= kMaxSlots; i++)
    if (active(i)) act.push_back(i);
  std::sort(act.begin(), act.end(), [](int a, int b) { return g_app.slots[a]->mhz < g_app.slots[b]->mhz; });
  std::vector<std::vector<int>> groups;
  for (int i : act) {
    const double mhz = g_app.slots[i]->mhz;
    if (!groups.empty() && groups.back().size() < 3 &&
        (mhz - g_app.slots[groups.back().front()]->mhz) * 1e6 <= timing_span_hz())
      groups.back().push_back(i);
    else
      groups.push_back({i});
  }
  for (const auto &g : groups) {
    sig += "|";
    for (int i : g) sig += std::to_string(i) + "@" + num(g_app.slots[i]->mhz, 0) + ",";
  }
  // every tick: a pilot timed by another slot gives up an adapter it still holds (e.g. one that was still
  // opening when the groups were made), so the group that needs it gets it
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (s && !parked(i) && s->lead > 0 && s->lead != i && s->rx.radio && !s->rx.opening) {
      KP_LOGI("pilot %d: adapter %s freed (timed by pilot %d's adapter)", i, s->rx.adapter.c_str(), s->lead);
      release(s->rx);
    }
  }
  if (sig == last) return;
  last = sig;
  std::string log;
  for (const auto &g : groups) {
    int lead = 0;
    for (int i : g)  // a member who chose an adapter leads the group (the choice applies to the whole group)
      if (!g_pin[i].empty()) {
        lead = i;
        break;
      }
    if (!lead)
      for (int i : g)
        if (g_app.slots[i]->rx.radio || g_app.slots[i]->rx.opening) {
          lead = i;
          break;
        }
    if (!lead) lead = g.front();
    for (size_t k = 0; k < g.size(); k++) {
      Slot &s = *g_app.slots[g[k]];
      s.lead = lead;
      s.lead_ch = static_cast<int>(k);
    }
    log += " [";
    for (int i : g) log += (i == g.front() ? "" : " + ") + g_app.slots[i]->ch();
    log += "]";
    tune_slot(lead);
  }
  KP_LOGI("timing only: %zu adapter(s) for %zu pilot(s):%s", groups.size(), act.size(), log.c_str());
}

// The receiver sample rate changed in Settings: every open radio retunes at the new rate.
void apply_sample_rate() {
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (!s || !s->rx.radio || s->rx.opening || !s->rx.radio->is_open()) continue;
    s->rx.radio->set_watch_rate(g_settings.msps == 40 ? 1 : 2);
    tune_slot(i);  // takes effect on the retune
  }
  KP_LOGI("receiver sample rate: %d MS/s", g_settings.msps);
}


const std::string &pilot_name(int n) {
  static const std::string none;
  return g_app.slots[n] ? g_app.slots[n]->name : none;
}

bool video_ok(int n) {
  const Slot *s = g_app.slots[n].get();
  if (s && net_video(n)) return g_net[n].connected() && now_ms() - g_net[n].last_frame_ms() < 1500;
  if (s && ext_video(n)) return g_cap[n].running() && now_ms() - g_cap[n].last_frame_ms() < 1500;
  return s && s->rx.radio && !s->rx.opening && jbool(s->rx.status, "locked");
}
bool pilot_from_list(int64_t, std::string &, std::string &) { return false; }  // no pilot list in Play RX

void rh_apply() {}              // no RotorHazard in Play RX
void apply_voice_settings() {}  // no voice in Play RX

// ---------------------------------------------------------------- JSON for the UI (engine thread)
int slot_no(const Slot &s) {
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i].get() == &s) return i;
  return 0;
}
std::string slot_state(const Slot &s) {
  if (const int n = slot_no(s); n && net_video(n)) {  // video and timing from a feed node
    if (!g_net[n].connected()) return "net_error";
    return now_ms() - g_net[n].last_frame_ms() < 1500 ? "net_live" : "net_wait";
  }
  if (const int n = slot_no(s); n && ext_video(n)) {  // video from a USB device
    const arx::Capture &c = g_cap[n];
    if (!c.running()) return "usb_error";
    return now_ms() - c.last_frame_ms() < 1500 ? "usb_live" : "usb_wait";
  }
  if (const int n = slot_no(s); n && g_solo_idled[n]) return "paused";  // low latency: "pause other feeds"
  if (rh_only()) return "rotorhazard";
  if (g_settings.timing_only && s.lead > 0 && g_app.slots[s.lead] && g_app.slots[s.lead].get() != &s) {
    const Slot &l = *g_app.slots[s.lead];  // timed by another slot's adapter
    if (l.rx.opening) return "opening";
    if (!l.rx.radio) return "no_adapter";
    return jbool(l.rx.status, "open") && jstr(l.rx.status, "rx_state") == "streaming" ? "timing" : "closed";
  }
  const std::string &j = s.rx.status;
  if (s.rx.opening) return "opening";
  if (g_settings.timing_only && s.rx.radio && jbool(j, "open") && jstr(j, "rx_state") == "streaming") return "timing";
  if (!s.rx.radio) return s.rx.error.empty() ? "no_adapter" : "error";
  if (!jbool(j, "open")) return "closed";
  if (jbool(j, "locked")) return "locked";
  return jstr(j, "rx_state") == "streaming" ? "no_signal" : jstr(j, "rx_state");
}

// Timing only: why pilot n has no adapter when it heads a group of its own (its channel is too far from every
// other pilot's to share an adapter). "" when that is not the case.
std::string share_note(int n) {
  const Slot *s = g_app.slots[n].get();
  if (rh_only()) return "";
  if (net_video(n)) return "";
  if (s && active(n) && ext_video(n) && !g_settings.timing_only) {
    if (g_settings.rh_on) return "";
    if (!s->rx.radio && !s->rx.opening && g_pin[n].empty()) return "Video from a USB device. No adapter is free to time this pilot's laps: plug in another adapter, use RotorHazard, or add laps by hand.";
  }
  if (s && active(n) && !g_pin[n].empty() && !s->rx.radio && !s->rx.opening && !(g_settings.timing_only && s->lead != 0 && s->lead != n)) {
    if (!adapter_by_id(g_pin[n]))
      return "Waiting for its chosen adapter \"" + adapter_label(g_pin[n]) + "\" (not plugged in, or in another USB port). Or set Adapter to Auto.";
    return "Waiting for its chosen adapter \"" + adapter_label(g_pin[n]) + "\" to be free.";
  }
  if (!g_settings.timing_only || !s || parked(n) || s->lead <= 0) return "";
  if (s->lead != n) {  // timed with another pilot: that pair's adapter is the missing one
    const std::string w = share_note(s->lead);
    return w.empty() ? "" : "Timed together with " + g_app.slots[s->lead]->ch() + ". " + w;
  }
  if (s->rx.radio || s->rx.opening) return "";
  int best = 0;
  double gap = 1e9;
  for (int i = 1; i <= kMaxSlots; i++)
    if (i != n && active(i) && std::fabs(g_app.slots[i]->mhz - s->mhz) < gap) gap = std::fabs(g_app.slots[i]->mhz - s->mhz), best = i;
  if (!best) return "";
  if (gap * 1e6 > timing_span_hz())
    return s->ch() + " is " + std::to_string(static_cast<int>(gap + 0.5)) + " MHz from the nearest pilot (" + g_app.slots[best]->ch() +
           "): too far apart to share an adapter (up to " + std::to_string(static_cast<int>(timing_span_hz() / 1e6)) + " MHz). Use neighbouring channels, or plug in another adapter.";
  // close to another pilot, but that adapter already covers as much of the band as it can
  std::string grp;
  const int ld = g_app.slots[best]->lead;
  for (int i = 1; i <= kMaxSlots; i++)
    if (active(i) && g_app.slots[i]->lead == ld && ld > 0) grp += (grp.empty() ? "" : " + ") + g_app.slots[i]->ch();
  return "The adapter timing " + (grp.empty() ? g_app.slots[best]->ch() : grp) + " already covers as much of the band as it can (" + std::to_string(static_cast<int>(timing_span_hz() / 1e6)) + " MHz). " +
         "Plug in another adapter, or move pilots onto channels closer together.";
}

// ---- USB check (Settings -> Receivers): how well each open adapter's USB stream keeps up on this PC ----
// Start: a snapshot of every streaming adapter's counters; while it runs and after: the change since then, against
// what the chip sent (fs / 8192 banks a second). Nothing is stopped or retuned: it measures the streams as they are.
// The report line is for users to send in (controller, power, rate, recovery switch, per-adapter results).
struct UsbCheckAdapter {
  std::string id, name, controller;
  bool usb3 = false;
  int slot = 0;
  double rx_banks = 0, restarts = 0, unwedged = 0, mid_drops = 0, start_drops = 0;  // at the start
};
struct UsbCheck {
  bool ran = false;
  int64_t t0_ms = 0, t_end_ms = 0;
  std::vector<UsbCheckAdapter> adapters;
  arx::PowerInfo power;
  std::string cpu;
} g_usbcheck;

void usb_check_start(int seconds) {
  UsbCheck c;
  c.ran = true;
  c.t0_ms = now_ms();
  c.t_end_ms = c.t0_ms + seconds * 1000LL;
  c.power = arx::power_info();
  c.cpu = arx::cpu_name();
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot *s = g_app.slots[i].get();
    if (!s || !s->rx.radio || s->rx.opening || s->rx.adapter.empty()) continue;
    UsbCheckAdapter a;
    a.id = s->rx.adapter;
    a.slot = i;
    const std::string &j = s->rx.status;
    a.usb3 = jbool(j, "usb3");
    a.rx_banks = jnum(j, "rx_banks");
    a.restarts = jnum(j, "restarts");
    a.unwedged = jnum(j, "unwedged");
    a.mid_drops = jnum(j, "mid_drops");
    a.start_drops = jnum(j, "start_drops");
    {
      std::lock_guard<std::mutex> l(g_app.adapters_m);
      for (const auto &ad : g_app.adapters)
        if (ad.id() == a.id) {
          a.name = ad.name;
          a.controller = arx::usb_controller(ad.vid, ad.pid, ad.port);
        }
    }
    c.adapters.push_back(a);
  }
  g_usbcheck = std::move(c);
  KP_LOGI("USB check: %d s, %zu adapter(s)", seconds, g_usbcheck.adapters.size());
}

std::string usb_check_json() {
  const UsbCheck &c = g_usbcheck;
  const int64_t now = now_ms();
  const bool running = c.ran && now < c.t_end_ms;
  const double secs = c.ran ? (std::min(now, c.t_end_ms) - c.t0_ms) / 1000.0 : 0;
  std::ostringstream o, line;
  char buf[256];
#ifdef _WIN32
  const char *os = "Windows";
#elif defined(__APPLE__)
  const char *os = "macOS";
#else
  const char *os = "Linux";
#endif
  line << kProduct << " " << kVersion << " | " << os;
  if (!c.cpu.empty()) line << " | " << c.cpu;
  if (!c.power.source.empty()) line << " | " << c.power.source;
  if (!c.power.plan.empty()) line << ", plan " << c.power.plan;
  if (!c.power.mode.empty()) line << ", mode " << c.power.mode;
  line << " | " << g_settings.msps << " MS/s, USB 3 recovery " << (g_settings.usb3_recovery ? "on" : "off") << " | " << std::fixed
       << std::setprecision(0) << secs << " s";
  o << "{\"ran\":" << (c.ran ? "true" : "false") << ",\"running\":" << (running ? "true" : "false")
    << ",\"seconds\":" << secs << ",\"left\":" << (running ? (c.t_end_ms - now) / 1000.0 : 0)
    << ",\"power\":" << q(c.power.source) << ",\"plan\":" << q(c.power.plan) << ",\"mode\":" << q(c.power.mode)
    << ",\"cpu\":" << q(c.cpu) << ",\"adapters\":[";
  bool first = true;
  for (const auto &a : c.adapters) {
    Slot *s = a.slot >= 1 && a.slot <= kMaxSlots ? g_app.slots[a.slot].get() : nullptr;
    const bool same = s && s->rx.radio && !s->rx.opening && s->rx.adapter == a.id;
    const std::string j = same ? s->rx.status : std::string();
    const double fs = jnum(j, "fs");
    const bool gapped = jbool(j, "gapped");
    const double got = same ? jnum(j, "rx_banks") - a.rx_banks : 0;
    const double expect = fs > 0 && !gapped ? fs / 8192.0 * secs : 0;
    const double pct = expect > 0 ? std::min(100.0, 100.0 * got / expect) : -1;
    const double restarts = same ? jnum(j, "restarts") - a.restarts : 0;
    const double unwedged = same ? jnum(j, "unwedged") - a.unwedged : 0;
    const double drops = same ? jnum(j, "mid_drops") - a.mid_drops + jnum(j, "start_drops") - a.start_drops : 0;
    const std::string fault = jstr(j, "last_fault");
    const std::string label = g_adapter_names.count(a.id) ? g_adapter_names[a.id] : std::string();
    o << (first ? "" : ",") << "{\"id\":" << q(a.id) << ",\"name\":" << q(a.name) << ",\"label\":" << q(label)
      << ",\"slot\":" << a.slot << ",\"usb3\":" << (a.usb3 ? "true" : "false") << ",\"controller\":" << q(a.controller)
      << ",\"streaming\":" << (same && jstr(j, "rx_state") == "streaming" ? "true" : "false")
      << ",\"gone\":" << (same ? "false" : "true") << ",\"gapped\":" << (gapped ? "true" : "false")
      << ",\"fs\":" << fs << ",\"pct\":" << pct << ",\"restarts\":" << restarts << ",\"unwedged\":" << unwedged
      << ",\"drops\":" << drops << ",\"last_fault\":" << q(fault) << "}";
    first = false;
    line << "\n  " << a.id << (kRx ? " (feed " : " (pilot ") << a.slot << ") USB " << (a.usb3 ? "3" : "2");
    if (!a.controller.empty()) line << ", " << a.controller;
    if (!same) {
      line << ": adapter released during the check";
      continue;
    }
    if (pct >= 0) {
      std::snprintf(buf, sizeof buf, ": %.1f%% of samples", pct);
      line << buf;
    } else {
      line << (gapped ? ": gapped (timing only)" : ": not streaming");
    }
    line << ", " << static_cast<long long>(restarts) << " restarts, " << static_cast<long long>(drops) << " banks dropped, "
         << static_cast<long long>(unwedged) << " lockups freed";
    if (!fault.empty() && restarts > 0) line << ", last fault: " << fault;
  }
  if (c.ran && c.adapters.empty()) line << "\n  no adapter was open when the check started";
  o << "],\"report\":" << q(c.ran ? line.str() : std::string()) << "}";
  return o.str();
}

std::string state_json() {
  std::ostringstream o;
  o << "{\"edition\":" << q(kRx ? "rx" : "league") << ",\"product\":" << q(kProduct)
    << ",\"version\":" << q(kVersion)
#ifdef _WIN32
    << ",\"platform\":\"windows\""
#elif defined(__APPLE__)
    << ",\"platform\":\"mac\""
#else
    << ",\"platform\":\"linux\",\"headless\":true"
#endif
    << ",\"sim\":" << (g_app.sim ? "true" : "false") << ",\"port\":" << g_feeds.port()
    << ",\"lan\":" << (g_app.lan ? "true" : "false") << ",\"feed_error\":" << q(g_app.feed_error)
    << ",\"host\":" << q(g_settings.node_name.empty() ? host_name() : g_settings.node_name) << ",\"ips\":[" << ips_json() << "]"
    << ",\"usb_busy\":" << (g_app.usb_busy ? "true" : "false") << ",\"adapters\":[";
  for (size_t k = 0; k < g_app.adapters.size(); k++) {
    const auto &a = g_app.adapters[k];
    std::string user = "free";
    for (int i = 1; i <= kMaxSlots; i++)
      if (g_app.slots[i] && g_app.slots[i]->rx.adapter == a.id()) user = parked(i) ? "spare" : "pilot " + std::to_string(i);
    if (g_app.scanner.adapter == a.id()) user = "scan";
    for (int i = 1; i <= kMaxSlots; i++)
      if (active(i) && g_pin[i] == a.id() && user != "pilot " + std::to_string(i)) user += " (chosen by pilot " + std::to_string(i) + ")";
    o << (k ? "," : "") << "{\"id\":" << q(a.id()) << ",\"name\":" << q(a.name)
      << ",\"label\":" << q(g_adapter_names.count(a.id()) ? g_adapter_names[a.id()] : std::string())
      << ",\"usb3\":" << (a.superspeed ? "true" : "false") << ",\"user\":" << q(user) << "}";
  }
  o << "],\"slots\":[";
  for (int i = 1; i <= kMaxSlots; i++) {
    const Slot *s = g_app.slots[i].get();
    if (parked(i)) s = nullptr;  // a warm spare: not shown
    o << (i > 1 ? "," : "") << "{\"slot\":" << i << ",\"used\":" << (s ? "true" : "false");
    if (s) {
      const std::string &j = s->rx.status;
      o << ",\"name\":" << q(s->name) << ",\"pilot_id\":" << s->pilot_id << ",\"mhz\":" << num(s->mhz) << ",\"channel\":" << q(s->ch())
        << ",\"state\":" << q(slot_state(*s)) << ",\"error\":" << q(s->rx.error)
        << ",\"adapter\":" << q(g_settings.timing_only && s->lead > 0 && s->lead != i && g_app.slots[s->lead] ? g_app.slots[s->lead]->rx.adapter : s->rx.adapter)
        << ",\"timing_only\":" << (g_settings.timing_only ? "true" : "false") << ",\"timed_by\":" << (g_settings.timing_only ? s->lead : 0)
        << ",\"video_name\":" << q(g_vsrc[i].empty() ? "" : capture_name(g_vsrc[i])) << ",\"video_format\":" << q(g_vsrc[i].empty() ? "" : net_video(i) ? net_format(i) : g_cap[i].format())
        << ",\"video_error\":" << q(net_video(i) ? g_net[i].error() : g_vsrc[i].empty() || g_cap[i].running() ? "" : g_cap[i].error())
        << ",\"node_streams\":" << g_feeds.node_clients(i) << ",\"node_lent\":" << (g_node_key[i].empty() ? "false" : "true")
        << ",\"share_note\":" << q(share_note(i)) << ",\"pin\":" << q(g_pin[i]) << ",\"video\":" << q(g_vsrc[i])
        << ",\"fps\":" << num(jnum(j, "fps"), 0) << ",\"lock\":" << num(jnum(j, "lock"), 2)
        << ",\"std\":" << q(jstr(j, "std")) << ",\"colour\":" << (jbool(j, "colour") ? "true" : "false")
        << ",\"gain_db\":" << num(s->gain_applied, 0) << ",\"url\":" << q(g_feeds.url(i))
        << ",\"ab\":{\"on\":" << (jbool(j, "combine") ? "true" : "false") << ",\"snr_a\":" << num(jnum(j, "snr_a", -99), 1)
        << ",\"snr_b\":" << num(jnum(j, "snr_b", -99), 1) << ",\"snr_c\":" << num(jnum(j, "snr_c", -99), 1)
        << ",\"b_share\":" << num(jnum(j, "b_share"), 2) << ",\"a_fade\":" << num(jnum(j, "a_fade"), 3)
        << ",\"rescued\":" << num(jnum(j, "rescued"), 2) << ",\"coherence\":" << num(jnum(j, "coherence"), 2) << "}";
        o << ",\"glitches\":{\"restarts\":" << num(jnum(j, "restarts"), 0) << ",\"rephases\":" << num(jnum(j, "rephases"), 0)
          << ",\"lost\":" << num(jnum(j, "losses"), 0) << ",\"clicks\":" << num(jnum(j, "clicks_repaired"), 0) << ",\"mended\":" << num(jnum(j, "lines_kept"), 0) << "}";
    }
    o << "}";
  }
  o << "],\"scan\":{\"running\":" << (g_app.scan_running ? "true" : "false")
    << ",\"step\":" << num(jnum(g_app.scanner.status, "scan_step"), 0)
    << ",\"steps\":" << num(jnum(g_app.scanner.status, "scan_steps"), 0)
    << ",\"spare\":" << (scan_possible() ? "true" : "false") << ",\"found\":[";
  for (size_t k = 0; k < g_app.found.size(); k++) {
    const auto &p = g_app.found[k];
    o << (k ? "," : "") << "{\"name\":" << q(p.name) << ",\"mhz\":" << num(p.mhz) << ",\"std\":" << q(p.std_name)
      << ",\"snr\":" << num(p.snr_db) << "}";
  }
  // low latency window (Windows): which slot it shows (0 = closed) and how it is going
  std::string lowlat = "{\"supported\":false,\"slot\":0}";
#ifdef _WIN32
  {
    const auto ls = g_lowlat.stats();
    lowlat = "{\"supported\":true,\"slot\":" + std::to_string(g_lowlat.slot()) + ",\"tearing\":" + (ls.tearing ? "true" : "false") +
             ",\"fullscreen\":" + (ls.direct ? "true" : "false") + ",\"presents\":" + num(ls.presents_s, 0) +
             ",\"lines\":" + num(ls.lines_s, 0) + ",\"age_ms\":" + num(ls.age_ms, 2) + ",\"age_max_ms\":" + num(ls.age_max_ms, 2) +
             ",\"solo\":" + (g_settings.lowlat_solo ? "true" : "false") + ",\"error\":" + q(ls.error) + "}";
  }
#endif
  {
    std::lock_guard<std::mutex> l(g_log.m);
    o << "]},\"log_seq\":" << g_log.seq << ",\"trackside\":" << g_live.status_json()
#ifdef _WIN32
    << ",\"vcam\":{\"supported\":true,\"error\":" << q(g_vcam.error()) << "}"
#else
    << ",\"vcam\":{\"supported\":false,\"error\":\"\"}"
#endif
    << ",\"lowlat\":" << lowlat
    << "}";
  }
  return o.str();
}

std::string log_json(uint64_t since) {
  std::lock_guard<std::mutex> l(g_log.m);
  std::ostringstream o;
  o << "{\"seq\":" << g_log.seq << ",\"lines\":[";
  const uint64_t first = g_log.seq - g_log.lines.size() + 1;  // number of lines.front()
  bool comma = false;
  for (size_t i = 0; i < g_log.lines.size(); i++)
    if (first + i > since) {
      o << (comma ? "," : "") << q(g_log.lines[i]);
      comma = true;
    }
  o << "]}";
  return o.str();
}

std::string channels_json() {
  std::ostringstream o;
  o << "[";
  const auto &ch = kp::fpv_channels();
  for (size_t i = 0; i < ch.size(); i++)
    o << (i ? "," : "") << "{\"name\":" << q(ch[i].name) << ",\"band\":\"" << ch[i].name[0] << "\",\"mhz\":" << num(ch[i].mhz, 0) << "}";
  o << "]";
  return o.str();
}

// ---------------------------------------------------------------- HTTP (server threads)
// Run fn on the engine thread and wait for it (engine state is only touched there). false: engine busy/gone.
bool engine_call(const std::function<void()> &fn) {
  if (!g_engine) return false;
  DWORD_PTR r = 0;
  return SendMessageTimeoutW(g_engine, WM_APP_CALL, 0, reinterpret_cast<LPARAM>(&fn), SMTO_BLOCK | SMTO_ABORTIFHUNG,
                             5000, &r) != 0;
}

const char *content_type(const std::string &path) {
  const std::string ext = path.substr(path.find_last_of('.') + 1);
  if (ext == "html") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js") return "text/javascript; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "ico") return "image/x-icon";
  if (ext == "json") return "application/json";
  if (ext == "webmanifest") return "application/manifest+json";
  return "application/octet-stream";
}

// A file from the web folder (read on every request, so the UI can be edited without a rebuild).
bool serve_file(const std::string &rel, std::string &type, std::string &out) {
  if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos)
    return false;
  std::ifstream f(g_app.web_dir / std::filesystem::path(rel), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  type = content_type(rel);
  return true;
}

void vcam_apply() {  // only the cameras switched on are listed in other programs
#ifdef _WIN32
  const uint32_t m = static_cast<uint32_t>(g_settings.vcam_mask);
  if (g_vcam.cameras(m)) {
    std::string on;
    for (int n = 1; n <= 8; n++)
      if (m >> n & 1) on += (on.empty() ? "" : ",") + std::to_string(n);
    KP_LOGI("virtual cameras: %s", on.empty() ? "none" : (std::string(kProduct) + " Pilot " + on).c_str());
  } else KP_LOGW("virtual cameras: %s", g_vcam.error().c_str());
#endif
}
void live_apply() {
  g_live.configure(g_settings.ts_on, g_settings.ts_host, g_settings.ts_port, g_settings.ts_fps, g_settings.ts_kbps, g_settings.ts_bars);
}



bool api(const std::string &method, const std::string &full, const std::string &body, std::string &status,
         std::string &type, std::string &out) {
  const size_t qm = full.find('?');
  const std::string path = full.substr(0, qm), query = qm == std::string::npos ? "" : full.substr(qm + 1);
  auto qnum = [&](const char *key, double def) {
    const std::string k = std::string(key) + "=";
    size_t p = query.find(k);
    while (p != std::string::npos && p > 0 && query[p - 1] != '&') p = query.find(k, p + 1);
    return p == std::string::npos ? def : std::atof(query.c_str() + p + k.size());
  };
  auto slot_of = [&](const char *prefix) {
    const size_t n = std::strlen(prefix);
    if (path.compare(0, n, prefix) != 0) return 0;
    const int v = std::atoi(path.c_str() + n);
    return v >= 1 && v <= kMaxSlots ? v : 0;
  };
  auto ends = [&](const char *suffix) {
    const size_t n = std::strlen(suffix);
    return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
  };
  status = "200 OK";
  type = "application/json";
  const bool get = method == "GET", post = method == "POST";
  int n;
  [[maybe_unused]] auto starts = [&](const char *p) { return path.compare(0, std::strlen(p), p) == 0; };

  // ---- pages and design system (web\)
  if (get && (path == "/" || path == "/index.html")) return serve_file("index.html", type, out);
  if (get && slot_of("/pilot/")) return serve_file("pilot.html", type, out);  // OBS: one pilot's video
  if (get && path.compare(0, 4, "/ui/") == 0) {
    if (serve_file(path.substr(4), type, out)) return true;
    status = "404 Not Found";
    type = "text/plain";
    out = "not found\n";
    return true;
  }

  // ---- timing (thread-safe, no engine call)
  if (get && (n = slot_of("/api/levels/"))) {
    out = g_timing.levels_json(n, qnum("since", 0), static_cast<int>(qnum("max", 2000)));
    return true;
  }
  if (get && (n = slot_of("/api/spectrum/"))) {
    out = g_timing.spectrum_json(n);
    return true;
  }
  if (post && (n = slot_of("/api/gate/")) && ends("/autogain")) {
    // a fixed gain that puts this pilot's strongest recent signal at about -8 dBFS
    arx::GateConfig c = g_timing.config(n);
    c.gain_db = g_timing.suggest_gain(n);
    g_timing.set_config(n, c);
    KP_LOGI("pilot %d: auto gain -> %.0f dB", n, c.gain_db);
    out = g_timing.config_json(n);
    return true;
  }
  if (post && (n = slot_of("/api/gate/")) && ends("/reset")) {  // the enter / exit lines back to the defaults (min lap etc. kept)
    const arx::GateConfig d;
    std::ostringstream j;
    j << "{\"enter_db\":" << d.enter_db << ",\"exit_db\":" << d.exit_db << ",\"rise_db\":" << d.rise_db << ",\"hyst_db\":" << d.hyst_db << "}";
    g_timing.config_from_json(n, j.str());
    KP_LOGI("slot %d: gate lines reset to the defaults", n);
    out = g_timing.config_json(n);
    return true;
  }
  if ((n = slot_of("/api/gate/"))) {
    if (post) g_timing.config_from_json(n, body);
    out = g_timing.config_json(n);
    return true;
  }
  if (get && path == "/api/uiver") {  // the pages' version: OBS sources reload themselves when it changes (an update)
    long long newest = std::numeric_limits<long long>::min();  // libstdc++'s file clock counts from 2174: times are negative
    std::error_code ec;
    for (const auto &f : std::filesystem::directory_iterator(g_app.web_dir, ec)) {
      const auto t = f.last_write_time(ec);
      if (!ec) newest = std::max<long long>(newest, static_cast<long long>(t.time_since_epoch().count()));
    }
    out = std::string("{\"v\":") + q(std::string(kVersion) + "-" + std::to_string(newest)) + "}";
    return true;
  }
  if (get && path == "/api/log") {
    out = log_json(static_cast<uint64_t>(qnum("since", 0)));
    return true;
  }
  if (get && path == "/api/channels") {
    out = channels_json();
    return true;
  }
  if (get && path == "/api/captures") {  // the video capture devices plugged in (USB video receivers ...)
    auto list = arx::list_capture_devices();
    std::ostringstream o;
    o << "[";
    for (size_t k = 0; k < list.size(); k++) o << (k ? "," : "") << "{\"id\":" << q(list[k].id) << ",\"name\":" << q(list[k].name) << "}";
    // feed nodes: heard on the network, and the ones added by hand (Settings → Feed nodes)
    std::vector<arx::CaptureDevice> nodes;
    for (const auto &n : arx::nodes_heard()) nodes.push_back({n.id, n.name + " (" + n.host + ")"});
    {
      std::lock_guard<std::mutex> l(g_nodes_m);
      std::stringstream ss(g_nodes_manual);
      for (std::string h; std::getline(ss, h, ',');) {
        h.erase(0, h.find_first_not_of(" \t"));
        h.erase(h.find_last_not_of(" \t") + 1);
        if (h.empty()) continue;
        if (h.find(':') == std::string::npos) h += ":8090";
        const std::string id = "net:" + h;
        if (std::none_of(nodes.begin(), nodes.end(), [&](const arx::CaptureDevice &x) { return x.id == id; })) nodes.push_back({id, h});
      }
    }
    std::string js = o.str();  // "[" and the USB devices, still open: the feed nodes follow them
    for (const auto &n : nodes) js += std::string(js.size() > 1 ? "," : "") + "{\"id\":" + q(n.id) + ",\"name\":" + q(n.name) + ",\"net\":true}";
    js += "]";
    list.insert(list.end(), nodes.begin(), nodes.end());
    {
      std::lock_guard<std::mutex> l(g_capdevs_m);
      for (const auto &d : g_capdevs)  // keep names of devices unplugged for now (a slot may still point at one)
        if (std::none_of(list.begin(), list.end(), [&](const arx::CaptureDevice &x) { return x.id == d.id; })) list.push_back(d);
      g_capdevs = list;
    }
    out = js;
    return true;
  }

  // ---- engine (pilots, adapters, scan): on the engine thread
  std::function<void()> fn;
  if (get && path == "/api/state") {
    fn = [&] { out = state_json(); };
  } else if (get && path == "/api/race") {
    // no races in Play RX: every slot with no laps (the OBS pages read this)
    fn = [&] {
      std::string p;
      for (int i = 1; i <= kMaxSlots; i++) p += (i > 1 ? ",{\"slot\":" : "{\"slot\":") + std::to_string(i) + ",\"laps\":0}";
      out = "{\"id\":0,\"running\":false,\"elapsed_s\":0,\"pilots\":[" + p + "]}";
    };
  } else if (post && path == "/api/lowlat") {  // {"slot": n}: show pilot n in the low latency window; 0 closes it
    fn = [&] {
#ifdef _WIN32
      const int n = static_cast<int>(jnum(body, "slot"));
      if (jhas(body, "solo")) {
        g_settings.lowlat_solo = jbool(body, "solo");
        save_settings();
        KP_LOGI("low latency: other feeds %s", g_settings.lowlat_solo ? "paused while it is on" : "keep running");
      }
      if (!jhas(body, "slot")) {  // just the switch
        lowlat_tick();
        out = "{\"slot\":" + std::to_string(g_lowlat.slot()) + "}";
        return;
      }
      if (n >= 1 && n <= kMaxSlots && active(n)) {
        KP_LOGI("low latency: pilot %d (%s)", n, lowlat_title(n).c_str());
        g_lowlat.show(n, lowlat_title(n));
      } else {
        if (g_lowlat.slot()) KP_LOGI("low latency: off");
        g_lowlat.show(0, "");
      }
      out = "{\"slot\":" + std::to_string(g_lowlat.slot()) + "}";
      lowlat_tick();
#else
      status = "404 Not Found";
      out = "{\"error\":\"Low latency mode is Windows only\"}";
#endif
    };
  } else if (path == "/api/settings") {
    fn = [&, post] {
      if (post) apply_settings(body);
      out = settings_json();
    };
  } else if (post && (path == "/api/node/feed" || path == "/api/node/release")) {  // feed node: a race PC asks for a slot / lets it go
    fn = [&] {
      if (!g_settings.node_mode) {
        status = "403 Forbidden";
        out = "{\"error\":\"That PC is not in feed node mode (Feed node in its side menu)\"}";
        return;
      }
      const std::string key = jstr(body, "key");
      if (path == "/api/node/release") {
        node_release(key);
        out = "{}";
        return;
      }
      const int n = key.empty() ? 0 : node_feed(key, body);
      if (!n) {
        status = "409 Conflict";
        out = "{\"error\":\"The feed node has no free slot\"}";
        return;
      }
      out = "{\"slot\":" + std::to_string(n) + "}";
    };
  } else if (post && path == "/api/pilots") {
    fn = [&] {
      const int s = add_pilot(jstr(body, "name"), jnum(body, "mhz"), jstr(body, "channel"), static_cast<int64_t>(jnum(body, "pilot_id")));
      if (!s) status = "409 Conflict";
      out = "{\"slot\":" + std::to_string(s) + "}";
    };
  } else if (post && (n = slot_of("/api/pilots/")) && ends("/remove")) {
    fn = [&, n] {
      if (!remove_pilot(n)) status = "409 Conflict";
      out = "{}";
    };
  } else if (post && (n = slot_of("/api/pilots/"))) {
    fn = [&, n] {
      if (!update_pilot(n, body)) status = "404 Not Found";
      out = "{}";
    };
  } else if (post && path == "/api/adapters/name") {  // {"id": USB port, "name": "Gate left" ("" = none)}
    fn = [&] {
      const std::string id = jstr(body, "id");
      std::string nm = clean_name(jstr(body, "name"));
      if (id.empty()) {
        status = "400 Bad Request";
        out = "{}";
        return;
      }
      if (nm.empty()) g_adapter_names.erase(id);
      else g_adapter_names[id] = nm;
      save_adapter_names();
      out = "{}";
    };
  } else if (path == "/api/usb_check") {  // POST {"seconds": 20} starts it; GET: progress and results so far
    fn = [&, post] {
      if (post) usb_check_start(std::clamp(static_cast<int>(jnum(body, "seconds", 20)), 5, 120));
      out = usb_check_json();
    };
  } else if (post && path == "/api/adapters/rescan") {
    fn = [&] {
      refresh_adapters();
      out = "{}";
    };
  } else if (post && path == "/api/scan") {
    fn = [&] {
      start_scan();
      out = "{}";
    };
  } else {
    return false;
  }
  if (!engine_call(fn)) {
    status = "503 Service Unavailable";
    out = "{\"error\":\"busy\"}";
  }
  return true;
}

// ---------------------------------------------------------------- the engine window
LRESULT CALLBACK engine_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_APP_CALL:
    (*reinterpret_cast<const std::function<void()> *>(lp))();
    return 1;
  case WM_TIMER:
    if (wp == kTimerTiming) {
      feed_timing();
      if (!kRx) {  // Play RX: no race start
      }
    }
    else poll_status();
    return 0;
  case WM_APP_ADAPTERS: {
    if (g_app.usb_thread.joinable()) g_app.usb_thread.join();
    g_app.usb_busy = false;
    std::string log, ids_before, ids_after;
    for (const auto &a : g_app.adapters) ids_before += a.id() + (a.superspeed ? "/3 " : "/2 ");
    {
      std::lock_guard<std::mutex> l(g_app.adapters_m);
      g_app.adapters = g_app.adapters_new;
      log.swap(g_app.adapters_log);
    }
    for (const auto &a : g_app.adapters) ids_after += a.id() + (a.superspeed ? "/3 " : "/2 ");
    std::istringstream ls(log);
    for (std::string line; std::getline(ls, line);) KP_LOGI("%s", line.c_str());
    if (ids_after != ids_before)  // the 5 s rescan only speaks up when something changed
      KP_LOGI("adapters: %zu found (%s)", g_app.adapters.size(), ids_after.empty() ? "none" : ids_after.c_str());
    assign_adapters();
    return 0;
  }
  case WM_APP_OPENED: {
    const int n = static_cast<int>(wp);
    Rx *rx = n == 0 ? &g_app.scanner : (g_app.slots[n] ? &g_app.slots[n]->rx : nullptr);
    if (!rx) return 0;
    if (rx->opener.joinable()) rx->opener.join();
    if (!rx->radio || !rx->radio->is_open()) {
      KP_LOGE("%s: could not open adapter %s: %s", n ? ("pilot " + std::to_string(n)).c_str() : "scan",
              rx->adapter.c_str(), rx->error.c_str());
      const std::string err = rx->error;
      release(*rx);
      rx->error = err;
      rx->retry_ms = now_ms() + 5000;
      if (n == 0) g_app.scan_running = false;
    } else if (n == 0) {
      rx->radio->scan();
    } else {
      Slot &s = *g_app.slots[n];
      s.gain_applied = g_timing.config(n).gain_db;
      s.cfg_seen = g_timing.config_version(n);
      s.level_t = 0;
      rx->radio->set_gain(s.gain_applied);
      rx->radio->set_combine(g_settings.diversity);
      rx->radio->set_offset_tuning(g_settings.offset_tune);
      rx->radio->set_watch_rate(g_settings.msps == 40 ? 1 : 2);
      tune_slot(n);
    }
    return 0;
  }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- full screen (bound into the page)
#ifdef _WIN32
struct Fullscreen {
  bool on = false;
  WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
  LONG style = 0;
} g_fs;

void toggle_fullscreen(HWND hwnd) {
  if (!g_fs.on) {
    GetWindowPlacement(hwnd, &g_fs.wp);
    g_fs.style = GetWindowLongW(hwnd, GWL_STYLE);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(hwnd, GWL_STYLE, g_fs.style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    g_fs.on = true;
  } else {
    SetWindowLongW(hwnd, GWL_STYLE, g_fs.style);
    SetWindowPlacement(hwnd, &g_fs.wp);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    g_fs.on = false;
  }
}
#endif

std::filesystem::path find_web_dir() {
  // next to the exe (a packaged install), else the project's web\ (two up from build\win)
#ifndef _WIN32
  // the .app bundle: Contents/MacOS/<exe>, web pages in Contents/Resources/web
  if (std::filesystem::exists(std::filesystem::path(exe_dir()) / ".." / "Resources" / "web" / "index.html"))
    return (std::filesystem::path(exe_dir()) / ".." / "Resources" / "web").lexically_normal();
  for (const std::wstring &d : {exe_dir() + L"web", exe_dir() + L"../../web", exe_dir() + L"../web"})
    if (std::filesystem::exists(std::filesystem::path(d) / "index.html")) return std::filesystem::path(d).lexically_normal();
#endif
  for (const std::wstring &d : {exe_dir() + L"web", exe_dir() + L"..\\..\\web", exe_dir() + L"..\\web"})
    if (std::filesystem::exists(std::filesystem::path(d) / "index.html")) return std::filesystem::path(d);
  return std::filesystem::path(exe_dir() + L"web");
}

}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#else
int main(int argc_in, char **argv_in) {
  // the same argument handling as Windows' wide command line
  std::vector<std::wstring> wargs;
  for (int i = 0; i < argc_in; i++) wargs.push_back(widen(argv_in[i]));
  std::vector<const wchar_t *> wptrs;
  for (auto &a : wargs) wptrs.push_back(a.c_str());
  auto _wtoi = [](const wchar_t *w) { return std::atoi(narrow(w).c_str()); };
  auto _wtof = [](const wchar_t *w) { return std::atof(narrow(w).c_str()); };
#endif
#ifdef ANALOGRX_SIM
  g_app.sim = true;
#endif
#ifdef _WIN32
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
  const int argc = static_cast<int>(wptrs.size());
  const wchar_t *const *argv = wptrs.data();
#endif
  for (int i = 1; i < argc; i++) {
    const std::wstring a = argv[i];
    if (a == L"--sim" && i + 1 < argc) g_app.sim_files.push_back(narrow(argv[++i]));
    else if (a == L"--sim-adapters" && i + 1 < argc) g_app.sim_adapters = _wtoi(argv[++i]);
    else if (a == L"--sim-two-antennas") g_app.sim_two = true;
    else if (a == L"--sim-snr" && i + 1 < argc) g_app.sim_snr = _wtof(argv[++i]);
    else if (a == L"--port" && i + 1 < argc) g_app.port = _wtoi(argv[++i]);
    else if (a == L"--lan") g_app.lan = true;
    else if (a == L"--devtools") g_app.devtools = true;
    else if (a == L"-v") kp::set_log_debug(true);
  }
#ifdef _WIN32
  LocalFree(argv);
#endif
  kp::set_log_sink(log_sink);
  g_app.web_dir = find_web_dir();
  load_settings();
  KP_LOGI("%s %s %s: data in %s", kProduct, kVersion, g_app.sim ? "(simulated)" : "", narrow(data_dir().wstring()).c_str());
#ifdef _WIN32
  g_vcam.start(kProduct);
  vcam_apply();
  if (!g_app.sim) SetCurrentDirectoryW(data_dir().wstring().c_str());  // diagnostics (rec\) land there, not in Program Files
  _putenv_s("ARX_LUMA", g_settings.comb ? "comb" : "classic");
#else
  {  // diagnostics (rec/) land in the data folder, never inside the .app
    std::error_code ec;
    if (!g_app.sim) std::filesystem::current_path(data_dir(), ec);
  }
  setenv("ARX_LUMA", g_settings.comb ? "comb" : "classic", 1);
#endif
  kp::set_repair(g_settings.repair);
  kp::set_adaptive_comb(g_settings.acomb);
  kp::set_smooth_lines(g_settings.smooth_lines);
  apply_voice_settings();
  if (g_settings.lan || g_settings.node_mode) g_app.lan = true;  // the setting (a feed node always), or --lan for this run
  {
    std::lock_guard<std::mutex> l(g_nodes_m);
    g_nodes_manual = g_settings.nodes;
  }
#ifdef __linux__
  g_app.lan = true;  // headless: the browser on another device is the only way in
#endif
  if (!kRx) {
    // the pilot list, events and results (Play League)
    const std::string db = narrow((data_dir() / (g_app.sim ? L"league_sim.db" : L"league.db")).wstring());
  }

#ifdef ANALOGRX_SIM
  // the test transmitters: rec\sim_*.cs16 in the current folder, else in the project (two up from build\win)
#ifndef _WIN32
  for (const std::filesystem::path &dir : {std::filesystem::path("rec"), std::filesystem::path(exe_dir()) / ".." / ".." / "rec"}) {
    if (!g_app.sim_files.empty()) break;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = it->path().filename().string();
      if (n.rfind("sim_", 0) == 0 && it->path().extension() == ".cs16") g_app.sim_files.push_back(it->path().string());
    }
  }
#endif
  for (const std::wstring &dir : {std::wstring(L"rec\\"), exe_dir() + L"..\\..\\rec\\", exe_dir() + L"..\\rec\\"}) {
#ifndef _WIN32
    (void)dir;
    break;
#endif
    if (!g_app.sim_files.empty()) break;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"sim_*.cs16").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do g_app.sim_files.push_back(narrow(dir + fd.cFileName));
      while (FindNextFileW(h, &fd));
      FindClose(h);
    }
#endif
  }
  try {
    arx::sim::configure(g_app.sim_adapters, g_app.sim_files);
    arx::sim::set_two_antennas(g_app.sim_two, g_app.sim_snr);
    KP_LOGI("sim: %d adapters, %zu transmitter recording(s)", g_app.sim_adapters, g_app.sim_files.size());
  } catch (const std::exception &e) {
    KP_LOGE("sim: %s", e.what());
  }
#endif

  // the engine's message-only window (timers, results from worker threads, calls from the HTTP threads)
#ifdef _WIN32
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = engine_proc;
  wc.hInstance = inst;
  wc.lpszClassName = L"AnalogRXEngine";
  RegisterClassExW(&wc);
  g_engine = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
#else
  // macOS: the engine on a thread of its own (the window owns the main thread); everything below that touches
  // engine state runs before the loop starts or through it
  static EngineLoop engine_loop(engine_proc);
  g_engine = &engine_loop;
#endif
  load_adapter_names();
  load_pilots();
  for (int i = 1; i <= kMaxSlots; i++) publish_info(i);
  std::thread([] {  // the capture devices' names (slots that take their video from one)
    auto list = arx::list_capture_devices();
    std::lock_guard<std::mutex> l(g_capdevs_m);
    g_capdevs = list;
  }).detach();
  SetTimer(g_engine, kTimerStatus, 250, nullptr);
  SetTimer(g_engine, kTimerTiming, 50, nullptr);
  refresh_adapters();
#ifndef _WIN32
  engine_loop.start();
#endif

  live_apply();
  g_feeds.set_handler(api);
  // feed node: each slot's gate levels and status for a race PC streaming it (/node/stream/N)
  g_feeds.set_node(
      [](int slot, double &cursor, std::vector<uint8_t> &out) {
        if (cursor < 0) cursor = arx::now_s() - 0.2;
        const auto v = g_timing.levels_since(slot, cursor);
        for (const auto &p : v) {
          uint8_t b[16];
          std::memcpy(b, &p.t, 8);
          std::memcpy(b + 8, &p.a, 4);
          std::memcpy(b + 12, &p.b, 4);
          out.insert(out.end(), b, b + 16);
        }
        if (!v.empty()) cursor = v.back().t;
      },
      [](int slot) {
        std::lock_guard<std::mutex> l(g_node_stat_m);
        return g_node_stat[slot];
      },
      [] { return arx::now_s(); });
  try {
    g_feeds.start(g_app.port, g_app.lan);
    KP_LOGI("UI and OBS feeds: http://%s:%d/  (web files: %s)", g_app.lan ? "<this-pc>" : "localhost", g_app.port,
            g_app.web_dir.string().c_str());
  } catch (const std::exception &e) {
    g_app.feed_error = e.what();
    KP_LOGE("HTTP server: %s", e.what());
  }
  node_apply();  // feed node mode: announce this PC; race PC: listen for nodes

  // the window: the UI in WebView2 (its message loop also runs the engine window)
  int rc = 0;
#ifdef __linux__
  // headless: no window. Run until the service is stopped (SIGTERM) or Ctrl+C (SIGINT), then shut down in order.
  {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    (void)g_signals_blocked;
    KP_LOGI("%s %s running headless: open http://<this-device>:%d/ in a browser (Ctrl+C or systemctl stop to quit)",
            kProduct, kVersion, g_app.port);
    int sig = 0;
    sigwait(&set, &sig);
    KP_LOGI("signal %d: shutting down", sig);
  }
#else
  try {
    webview::webview w(g_app.devtools, nullptr);
    w.set_title(std::string(kProduct) + " " + kVersion + (g_app.sim ? " (simulated adapters)" : ""));
    w.set_size(1440, 900, WEBVIEW_HINT_NONE);
#ifdef _WIN32
    HWND top = static_cast<HWND>(w.window().value());
    w.bind("arxFullscreen", [top](const std::string &) -> std::string {
      toggle_fullscreen(top);
      return g_fs.on ? "true" : "false";
    });
    // the OBS password for scene switching: only the app window can read it (not other devices on the network)
    w.bind("arxObsPassword", [](const std::string &) -> std::string { return q(g_settings.obs_pass); });
#else
    void *top = w.window().value();  // the NSWindow
    mac_window_setup(top, kProduct);  // menus (Edit: copy / paste in the page), a sensible minimum size
    if (auto wv = w.widget(); wv.ok()) mac_webview_dialogs(wv.value());  // the page's confirm() / prompt() / alert()
    w.bind("arxFullscreen", [top](const std::string &) -> std::string { return mac_toggle_fullscreen(top) ? "true" : "false"; });
    // the OBS password for scene switching: only the app window can read it; settings live on the engine thread
    w.bind("arxObsPassword", [](const std::string &) -> std::string {
      std::string p;
      engine_call([&] { p = g_settings.obs_pass; });
      return q(p);
    });
#endif
    w.bind("arxOpenExternal", [](const std::string &req) -> std::string {
      // ["http(s)://..."]: open in the system browser (e.g. to copy an OBS URL from there, the Discord link). Web links only.
      const size_t a = req.find('"'), b = req.find('"', a + 1);
      if (a != std::string::npos && b != std::string::npos) {
        const std::string url = req.substr(a + 1, b - a - 1);
        const bool web = url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
        if (web && url.find_first_of("\"\\ ") == std::string::npos) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      return "null";
    });
    w.navigate("http://localhost:" + std::to_string(g_app.port) + "/");
    w.run();
  } catch (const std::exception &e) {
    KP_LOGE("window: %s", e.what());
#ifdef _WIN32
    const std::string msg = std::string("The ") + kProduct + " window could not start (" + e.what() +
                            ").\n\nIt needs the Microsoft Edge WebView2 Runtime, which Windows 11 includes. "
                            "The UI is also at http://localhost:" + std::to_string(g_app.port) + "/ in any browser.";
    MessageBoxA(nullptr, msg.c_str(), kProduct, MB_ICONERROR);
#else
    mac_alert(kProduct, std::string("The window could not start (") + e.what() + "). The UI is also at http://localhost:" +
                            std::to_string(g_app.port) + "/ in any browser.");
#endif
    rc = 1;
  }
#endif  // the window

  // shut down: radios first (their decoder threads push into the feed store), then the server
  KillTimer(g_engine, kTimerStatus);
  KillTimer(g_engine, kTimerTiming);
  HWND e = g_engine;
  g_engine = nullptr;  // HTTP threads stop calling in
#ifdef _WIN32
  DestroyWindow(e);
#else
  e->stop();
#endif
  if (g_app.usb_thread.joinable()) g_app.usb_thread.join();
#ifdef _WIN32
  g_lowlat.show(0, "");  // the low latency window
#endif
  for (int i = 1; i <= kMaxSlots; i++)
    if (g_app.slots[i]) release(g_app.slots[i]->rx);
  release(g_app.scanner);
  g_live.stop();  // the Trackside streams
#ifdef _WIN32
  g_vcam.stop();
#endif
  g_feeds.stop();
  return rc;
}
