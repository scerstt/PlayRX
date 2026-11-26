#include "feed_server.h"

#include "net.h"
#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <turbojpeg.h>
#else
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace arx {
namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string html_escape(const std::string &s) {
  std::string o;
  for (char c : s) {
    switch (c) {
    case '&': o += "&amp;"; break;
    case '<': o += "&lt;"; break;
    case '>': o += "&gt;"; break;
    case '"': o += "&quot;"; break;
    default: o += c;
    }
  }
  return o;
}

std::string json_escape(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o;
}

bool send_all(SOCKET s, const void *data, size_t n) {
  const char *p = static_cast<const char *>(data);
  while (n > 0) {
    const int k = ::send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
    if (k <= 0) return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool send_str(SOCKET s, const std::string &t) { return send_all(s, t.data(), t.size()); }

void send_response(SOCKET s, const char *status, const char *type, const std::string &body) {
  std::ostringstream h;
  h << "HTTP/1.1 " << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
    << "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
  send_str(s, h.str());
  send_all(s, body.data(), body.size());
}

// "/pilot/3?name=0" -> slot 3 (0 if the prefix does not match or the number is out of range)
int slot_from(const std::string &path, const char *prefix) {
  const size_t n = std::strlen(prefix);
  if (path.compare(0, n, prefix) != 0) return 0;
  const int v = std::atoi(path.c_str() + n);
  return v >= 1 && v <= kMaxSlots ? v : 0;
}

}  // namespace

#ifndef _WIN32
// macOS: libjpeg-turbo straight from the decoder's BGRX (one compressor per thread)
bool encode_jpeg(const uint8_t *bgrx, int w, int h, float quality, std::vector<uint8_t> &out) {
  if (w <= 0 || h <= 0) return false;
  thread_local tjhandle tj = tjInitCompress();
  if (!tj) return false;
  unsigned char *buf = nullptr;
  unsigned long n = 0;
  const int q = std::clamp(static_cast<int>(quality * 100 + 0.5f), 1, 100);
  if (tjCompress2(tj, bgrx, w, 0, h, TJPF_BGRX, &buf, &n, TJSAMP_420, q, TJFLAG_FASTDCT) != 0) {
    tjFree(buf);
    return false;
  }
  out.assign(buf, buf + n);
  tjFree(buf);
  return true;
}
#else
bool encode_jpeg(const uint8_t *bgrx, int w, int h, float quality, std::vector<uint8_t> &out) {
  // one COM apartment and WIC factory per thread
  thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  thread_local IWICImagingFactory *factory = [] {
    IWICImagingFactory *f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
  }();
  (void)com;
  if (!factory || w <= 0 || h <= 0) return false;

  // WIC's JPEG encoder takes 24-bit BGR
  thread_local std::vector<uint8_t> bgr;
  bgr.resize(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; i++) {
    bgr[3 * i] = bgrx[4 * i];
    bgr[3 * i + 1] = bgrx[4 * i + 1];
    bgr[3 * i + 2] = bgrx[4 * i + 2];
  }

  bool ok = false;
  IStream *stream = nullptr;
  IWICBitmapEncoder *enc = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  IPropertyBag2 *props = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
  do {
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) break;
    if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) break;
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality;
    props->Write(1, &opt, &v);
    if (FAILED(frame->Initialize(props))) break;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) break;
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 3), static_cast<UINT>(bgr.size()),
                                  bgr.data())))
      break;
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) break;
    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream, &hg))) break;
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    const size_t n = static_cast<size_t>(st.cbSize.QuadPart);
    const void *p = GlobalLock(hg);
    if (!p) break;
    out.assign(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
    GlobalUnlock(hg);
    ok = true;
  } while (false);
  if (props) props->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  stream->Release();
  return ok;
}
#endif

// Not in the constructor: encode_jpeg puts the calling thread in COM's multi-threaded apartment, and the
// main thread must stay single-threaded for WebView2.
FeedServer::FeedServer() = default;

const std::vector<uint8_t> &FeedServer::blank() {
  std::call_once(_blank_once, [this] {
    std::vector<uint8_t> black(720 * 480 * 4, 0);
    encode_jpeg(black.data(), 720, 480, 0.5f, _blank_jpeg);
  });
  return _blank_jpeg;
}

void FeedServer::notify_any() {
  {
    std::lock_guard<std::mutex> l(_any_m);
    _any_seq++;
  }
  _any_cv.notify_all();
}

FeedServer::~FeedServer() { stop(); }

void FeedServer::start(int port, bool lan) {
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) throw std::runtime_error("WSAStartup failed");
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed");
  int excl = 1;
#ifdef _WIN32
  // nobody else may take the port while we have it
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&excl), sizeof excl);
#else
  // restart straight away (the old connections' TIME_WAIT must not hold the port)
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &excl, sizeof excl);
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u_short>(port));
  a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
  if (::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(s, 16) != 0) {
    closesocket(s);
    throw std::runtime_error("port " + std::to_string(port) + " is in use");
  }
  _listen = s;
  _port = port;
  _lan = lan;
  _stop = false;
  _acceptor = std::thread([this] { accept_loop(); });
}

void FeedServer::stop() {
  if (_listen == ~uintptr_t(0)) return;
  _stop = true;
  closesocket(static_cast<SOCKET>(_listen));
  _listen = ~uintptr_t(0);
  if (_acceptor.joinable()) _acceptor.join();
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> l(_cm);
    for (auto s : _socks) shutdown(static_cast<SOCKET>(s), SD_BOTH);
    threads.swap(_threads);
  }
  for (auto &sl : _slots) sl.cv.notify_all();
  for (auto &t : threads)
    if (t.joinable()) t.join();
  WSACleanup();
}

std::string FeedServer::url(int slot) const {
  return "http://localhost:" + std::to_string(_port) + "/pilot/" + std::to_string(slot);
}

void FeedServer::push(int slot, const uint8_t *bgrx, int w, int h) {
  if (slot < 1 || slot > kMaxSlots) return;
  Slot &s = _slots[slot];
  {
    std::lock_guard<std::mutex> l(s.m);
    s.bgrx.assign(bgrx, bgrx + static_cast<size_t>(w) * h * 4);
    s.w = w;
    s.h = h;
    s.seq++;
    s.t_ms = now_ms();
  }
  s.cv.notify_all();
  notify_any();
}

void FeedServer::set_info(int slot, const SlotInfo &info) {
  if (slot < 1 || slot > kMaxSlots) return;
  std::lock_guard<std::mutex> l(_slots[slot].m);
  _slots[slot].info = info;
}

void FeedServer::clear(int slot) {
  if (slot < 1 || slot > kMaxSlots) return;
  Slot &s = _slots[slot];
  {
    std::lock_guard<std::mutex> l(s.m);
    s.info = {};
    s.bgrx.clear();
    s.w = s.h = 0;
    s.seq++;
  }
  s.cv.notify_all();
  notify_any();
}

bool FeedServer::jpeg_of(int slot, uint64_t &seq, std::vector<uint8_t> &out) {
  Slot &s = _slots[slot];
  std::unique_lock<std::mutex> l(s.m);
  if (s.bgrx.empty() || now_ms() - s.t_ms > 1000) return false;  // stale: treat as no signal
  if (s.jpeg_seq != s.seq) {
    // encode outside the lock (the decoder thread must never wait on JPEG work)
    std::vector<uint8_t> px = s.bgrx;
    const int w = s.w, h = s.h;
    const uint64_t sq = s.seq;
    l.unlock();
    std::vector<uint8_t> j;
    if (!encode_jpeg(px.data(), w, h, 0.85f, j)) return false;
    l.lock();
    if (sq > s.jpeg_seq) {
      s.jpeg = std::move(j);
      s.jpeg_seq = sq;
    }
  }
  seq = s.jpeg_seq;
  out = s.jpeg;
  return true;
}

void FeedServer::accept_loop() {
  while (!_stop) {
#ifndef _WIN32
    // closing the listening socket does not wake accept() on macOS: wait a little at a time instead
    {
      fd_set r;
      FD_ZERO(&r);
      const SOCKET ls = static_cast<SOCKET>(_listen);
      if (_stop || ls == INVALID_SOCKET) break;
      FD_SET(ls, &r);
      timeval tv{0, 250000};
      if (select(ls + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
    }
#endif
    SOCKET c = ::accept(static_cast<SOCKET>(_listen), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
      if (_stop) break;
      continue;
    }
    int nodelay = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof nodelay);
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &nodelay, sizeof nodelay);  // a viewer leaving must not kill the app
#endif
    std::lock_guard<std::mutex> l(_cm);
    _socks.push_back(c);
    _threads.emplace_back([this, c] { serve(c); });
  }
}

void FeedServer::serve(uintptr_t sock) {
  SOCKET s = static_cast<SOCKET>(sock);
  std::string req;
  char buf[2048];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
    const int k = ::recv(s, buf, sizeof buf, 0);
    if (k <= 0) break;
    req.append(buf, static_cast<size_t>(k));
  }
  std::string method, path;
  {
    std::istringstream ls(req.substr(0, req.find("\r\n")));
    ls >> method >> path;
  }
  // a request body (POST): Content-Length bytes after the blank line, at most 64 KiB
  std::string body;
  const size_t hdr_end = req.find("\r\n\r\n");
  if (hdr_end != std::string::npos) {
    std::string lower = req.substr(0, hdr_end);
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t cl = lower.find("\r\ncontent-length:");
    const size_t cap = 65536;
    const size_t want = cl == std::string::npos ? 0 : std::min<size_t>(cap, std::strtoul(lower.c_str() + cl + 17, nullptr, 10));
    body = req.substr(hdr_end + 4);
    while (body.size() < want) {
      const int k = ::recv(s, buf, sizeof buf, 0);
      if (k <= 0) break;
      body.append(buf, static_cast<size_t>(k));
    }
    if (body.size() > want) body.resize(want);
  }
  std::string h_status, h_type, h_out;
  int n;
  std::string ws_key;
  const bool ws_events = path.compare(0, 10, "/ws/events") == 0;
  if (hdr_end != std::string::npos && (path.compare(0, 8, "/ws/feed") == 0 || ws_events)) {
    std::string lower = req.substr(0, hdr_end);
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t k = lower.find("\r\nsec-websocket-key:");
    if (k != std::string::npos) {
      const size_t v = k + 20, e = req.find("\r\n", v);
      ws_key = req.substr(v, e - v);
      ws_key.erase(0, ws_key.find_first_not_of(" \t"));
      ws_key.erase(ws_key.find_last_not_of(" \t") + 1);
    }
  }
  if (!ws_key.empty() && ws_events) {
    serve_events(sock, ws_key);
  } else if (!ws_key.empty()) {
    // ?slots=1,2,3 (default: every slot)
    std::vector<int> want;
    const size_t sp = path.find("slots=");
    if (sp != std::string::npos)
      for (const char *p = path.c_str() + sp + 6; *p;) {
        const int v = std::atoi(p);
        if (v >= 1 && v <= kMaxSlots) want.push_back(v);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        else break;
      }
    if (want.empty())
      for (int i = 1; i <= kMaxSlots; i++) want.push_back(i);
    serve_ws(sock, ws_key, want);
  } else if (method == "GET" && _node_levels && (n = slot_from(path, "/node/stream/"))) {
    serve_node(sock, n);
  } else if (_handler && _handler(method, path, body, h_status, h_type, h_out)) {
    send_response(s, h_status.c_str(), h_type.c_str(), h_out);
  } else if (method != "GET") {
    send_response(s, "405 Method Not Allowed", "text/plain", "GET only\n");
  } else if (path == "/feeds") {
    send_response(s, "200 OK", "text/html; charset=utf-8", index_html());
  } else if (path == "/api/slots") {
    send_response(s, "200 OK", "application/json", slots_json());
  } else if ((n = slot_from(path, "/pilot/"))) {
    send_response(s, "200 OK", "text/html; charset=utf-8", pilot_html(n, path.find("name=0") == std::string::npos));
  } else if ((n = slot_from(path, "/feed/"))) {
    serve_mjpeg(sock, n);
  } else if ((n = slot_from(path, "/snapshot/"))) {
    uint64_t seq = 0;
    std::vector<uint8_t> j;
    if (!jpeg_of(n, seq, j)) j = blank();
    send_response(s, "200 OK", "image/jpeg", std::string(j.begin(), j.end()));
  } else {
    send_response(s, "404 Not Found", "text/plain", "not found\n");
  }
  closesocket(s);
  std::lock_guard<std::mutex> l(_cm);
  for (size_t i = 0; i < _socks.size(); i++)
    if (_socks[i] == sock) {
      _socks.erase(_socks.begin() + static_cast<long>(i));
      break;
    }
  // this thread's std::thread object is reaped by stop(); detach it now so the list does not grow forever
  for (size_t i = 0; i < _threads.size(); i++)
    if (_threads[i].get_id() == std::this_thread::get_id()) {
      _threads[i].detach();
      _threads.erase(_threads.begin() + static_cast<long>(i));
      break;
    }
}


void FeedServer::serve_mjpeg(uintptr_t sock, int slot) {
  SOCKET s = static_cast<SOCKET>(sock);
  if (!send_str(s, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=arxframe\r\n"
                   "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"))
    return;
  Slot &sl = _slots[slot];
  uint64_t sent = ~0ull;
  int64_t last_send = 0;
  std::vector<uint8_t> j;
  while (!_stop) {
    {
      // wait for a picture newer than the last one sent (or time out to keep the stream alive)
      std::unique_lock<std::mutex> l(sl.m);
      sl.cv.wait_for(l, std::chrono::milliseconds(250), [&] { return _stop || sl.seq != sent; });
    }
    if (_stop) break;
    uint64_t seq = 0;
    const bool live = jpeg_of(slot, seq, j);
    if (live && seq == sent) continue;
    if (!live) {
      if (now_ms() - last_send < 500) continue;  // no signal: a black frame twice a second
      j = blank();
      seq = ~0ull - 1;
    }
    char head[128];
    const int hn = std::snprintf(head, sizeof head, "--arxframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                                 j.size());
    if (!send_all(s, head, static_cast<size_t>(hn)) || !send_all(s, j.data(), j.size()) || !send_str(s, "\r\n")) break;
    sent = seq;
    last_send = now_ms();
  }
}

namespace {
#if !defined(_WIN32) && !defined(__APPLE__)
// SHA-1 (FIPS 180-4), only for the websocket handshake's Sec-WebSocket-Accept
void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::vector<uint8_t> m(data, data + len);
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 7; i >= 0; i--) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(m[off + 4 * i]) << 24) | (uint32_t(m[off + 4 * i + 1]) << 16) | (uint32_t(m[off + 4 * i + 2]) << 8) |
             uint32_t(m[off + 4 * i + 3]);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  for (int i = 0; i < 5; i++)
    for (int j = 0; j < 4; j++) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
}
#endif
std::string ws_accept(const std::string &key) {
  // base64(SHA-1(key + RFC 6455 GUID)), SHA-1 from Windows' CNG
  const std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t h[20] = {};
#ifdef _WIN32
  BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(in.data())),
             static_cast<ULONG>(in.size()), h, sizeof h);
#elif defined(__APPLE__)
  CC_SHA1(in.data(), static_cast<CC_LONG>(in.size()), h);
#else
  sha1(reinterpret_cast<const uint8_t *>(in.data()), in.size(), h);
#endif
  static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (int i = 0; i < 20; i += 3) {
    const uint32_t v = (h[i] << 16) | ((i + 1 < 20 ? h[i + 1] : 0) << 8) | (i + 2 < 20 ? h[i + 2] : 0);
    o += b64[(v >> 18) & 63];
    o += b64[(v >> 12) & 63];
    o += i + 1 < 20 ? b64[(v >> 6) & 63] : '=';
    o += i + 2 < 20 ? b64[v & 63] : '=';
  }
  return o;
}

bool ws_send(SOCKET s, uint8_t opcode, const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
  // one unmasked frame (server -> client), payload = a + b
  const uint64_t n = an + bn;
  uint8_t h[10];
  size_t hn = 0;
  h[hn++] = static_cast<uint8_t>(0x80 | opcode);
  if (n < 126) {
    h[hn++] = static_cast<uint8_t>(n);
  } else if (n < 65536) {
    h[hn++] = 126;
    h[hn++] = static_cast<uint8_t>(n >> 8);
    h[hn++] = static_cast<uint8_t>(n);
  } else {
    h[hn++] = 127;
    for (int i = 7; i >= 0; i--) h[hn++] = static_cast<uint8_t>(n >> (8 * i));
  }
  return send_all(s, h, hn) && (an == 0 || send_all(s, a, an)) && (bn == 0 || send_all(s, b, bn));
}
}  // namespace

void FeedServer::serve_ws(uintptr_t sock, const std::string &key, const std::vector<int> &slots) {
  SOCKET s = static_cast<SOCKET>(sock);
  if (!send_str(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + ws_accept(key) + "\r\n\r\n"))
    return;
  std::vector<uint64_t> sent(kMaxSlots + 1, ~0ull);
  std::vector<uint8_t> j;
  while (!_stop) {
    uint64_t any;
    {
      std::unique_lock<std::mutex> l(_any_m);
      any = _any_seq;
    }
    // the client only sends pings / close: a readable socket with nothing (or a close frame) ends us
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{0, 0};
    if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
      uint8_t in[512];
      const int k = ::recv(s, reinterpret_cast<char *>(in), sizeof in, 0);
      if (k <= 0 || (in[0] & 0x0F) == 0x8) break;
    }
    for (int n : slots) {
      uint64_t seq = 0;
      if (!jpeg_of(n, seq, j) || seq == sent[n]) continue;
      const uint8_t tag = static_cast<uint8_t>(n);
      if (!ws_send(s, 0x2, &tag, 1, j.data(), j.size())) return;
      sent[n] = seq;
    }
    std::unique_lock<std::mutex> l(_any_m);
    _any_cv.wait_for(l, std::chrono::milliseconds(200), [&] { return _stop || _any_seq != any; });
  }
  const uint8_t bye[2] = {0x03, 0xE8};  // close 1000
  ws_send(s, 0x8, bye, 2, nullptr, 0);
}

// Feed node: one slot to a race PC (see feed_server.h / net_feed.h): [type][len u32 LE][payload] messages
void FeedServer::serve_node(uintptr_t sock, int slot) {
  SOCKET s = static_cast<SOCKET>(sock);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
  if (!send_str(s, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n")) return;
  _node_streams[slot]++;
  auto msg = [&](uint8_t type, const uint8_t *p, size_t n) {
    uint8_t h[5] = {type, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24)};
    return send_all(s, h, 5) && (n == 0 || send_all(s, p, n));
  };
  uint64_t sent = ~0ull;
  double cursor = -1;
  int64_t last_status = 0;
  std::vector<uint8_t> j, lv;
  while (!_stop) {
    uint64_t any;
    {
      std::unique_lock<std::mutex> l(_any_m);
      any = _any_seq;
    }
    fd_set rd;  // the race PC sends nothing: readable = it went away
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{0, 0};
    if (select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) > 0) {
      char in[256];
      if (::recv(s, in, sizeof in, 0) <= 0) break;
    }
    uint64_t seq = 0;
    if (jpeg_of(slot, seq, j) && seq != sent) {
      if (!msg(1, j.data(), j.size())) break;
      sent = seq;
    }
    lv.resize(8);
    const double now = _node_now ? _node_now() : 0;
    std::memcpy(lv.data(), &now, 8);
    _node_levels(slot, cursor, lv);
    if (lv.size() > 8 && !msg(2, lv.data(), lv.size())) break;
    if (now_ms() - last_status > 500) {
      last_status = now_ms();
      const std::string st = _node_status ? _node_status(slot) : "{}";
      if (!msg(3, reinterpret_cast<const uint8_t *>(st.data()), st.size())) break;
      if (lv.size() <= 8 && !msg(2, lv.data(), 8)) break;  // the clock, even with no levels
    }
    std::unique_lock<std::mutex> l(_any_m);
    _any_cv.wait_for(l, std::chrono::milliseconds(20), [&] { return _stop || _any_seq != any; });
  }
  _node_streams[slot]--;
}

void FeedServer::publish(const std::string &json) {
  {
    std::lock_guard<std::mutex> l(_ev_m);
    _ev.push_back({++_ev_seq, json});
    while (_ev.size() > 64) _ev.pop_front();
  }
  _ev_cv.notify_all();
}

void FeedServer::serve_events(uintptr_t sock, const std::string &key) {
  SOCKET s = static_cast<SOCKET>(sock);
  if (!send_str(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + ws_accept(key) + "\r\n\r\n"))
    return;
  uint64_t seen;
  {
    std::lock_guard<std::mutex> l(_ev_m);
    seen = _ev_seq;
  }
  const std::string hello = "{\"type\":\"hello\"}";
  if (!ws_send(s, 0x1, reinterpret_cast<const uint8_t *>(hello.data()), hello.size(), nullptr, 0)) return;
  int64_t last = now_ms();
  while (!_stop) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{0, 0};
    if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
      uint8_t in[512];
      const int k = ::recv(s, reinterpret_cast<char *>(in), sizeof in, 0);
      if (k <= 0 || (in[0] & 0x0F) == 0x8) break;
    }
    std::vector<std::string> out;
    {
      std::unique_lock<std::mutex> l(_ev_m);
      _ev_cv.wait_for(l, std::chrono::milliseconds(250), [&] { return _stop || _ev_seq != seen; });
      for (const auto &[n, m] : _ev)
        if (n > seen) out.push_back(m);
      seen = _ev_seq;
    }
    for (const std::string &m : out)
      if (!ws_send(s, 0x1, reinterpret_cast<const uint8_t *>(m.data()), m.size(), nullptr, 0)) return;
    if (out.empty() && now_ms() - last > 15000) {  // keep idle connections alive through proxies
      if (!ws_send(s, 0x9, nullptr, 0, nullptr, 0)) return;
      last = now_ms();
    } else if (!out.empty()) {
      last = now_ms();
    }
  }
  const uint8_t bye[2] = {0x03, 0xE8};
  ws_send(s, 0x8, bye, 2, nullptr, 0);
}

std::string FeedServer::slots_json() {
  std::ostringstream o;
  o << "[";
  for (int i = 1; i <= kMaxSlots; i++) {
    Slot &s = _slots[i];
    std::lock_guard<std::mutex> l(s.m);
    const bool live = !s.bgrx.empty() && now_ms() - s.t_ms < 1000;
    o << (i > 1 ? "," : "") << "{\"slot\":" << i << ",\"used\":" << (s.info.used ? "true" : "false") << ",\"name\":\""
      << json_escape(s.info.name) << "\",\"channel\":\"" << json_escape(s.info.channel) << "\",\"mhz\":" << s.info.mhz
      << ",\"live\":" << (live ? "true" : "false") << ",\"locked\":" << (s.info.locked ? "true" : "false") << "}";
  }
  o << "]";
  return o.str();
}

std::string FeedServer::pilot_html(int slot, bool show_name) {
  std::ostringstream o;
  o << R"(<!doctype html><html><head><meta charset="utf-8"><title>AnalogRX pilot )" << slot << R"(</title>
<style>
html,body{margin:0;height:100%;background:#000;overflow:hidden}
img{position:absolute;inset:0;width:100%;height:100%;object-fit:contain}
#tag{position:absolute;left:3%;bottom:4%;font:700 7vh system-ui,Segoe UI,sans-serif;color:#fff;
  text-shadow:0 0 .2em #000,0 0 .4em #000;white-space:nowrap}
#ch{font-weight:500;opacity:.8;margin-left:.4em;font-size:.7em}
#ns{position:absolute;inset:0;display:none;align-items:center;justify-content:center;
  font:700 9vh system-ui,Segoe UI,sans-serif;color:#ffc400;text-shadow:0 0 .3em #000}
</style></head><body>
<img id="v" src="/feed/)" << slot << R"(.mjpg" alt="">
<div id="tag" )" << (show_name ? "" : "hidden") << R"(><span id="nm"></span><span id="ch"></span></div>
<div id="ns">NO SIGNAL</div>
<script>
const slot = )" << slot << R"(, v = document.getElementById('v');
// reconnect the stream if it ever ends (app restarted, network blip)
v.onerror = () => setTimeout(() => { v.src = '/feed/' + slot + '.mjpg?t=' + Date.now(); }, 1000);
async function poll() {
  try {
    const s = (await (await fetch('/api/slots', {cache: 'no-store'})).json())[slot - 1];
    document.getElementById('nm').textContent = s.used ? s.name : '';
    document.getElementById('ch').textContent = s.used ? s.channel : '';
    document.getElementById('ns').style.display = s.used && !s.live ? 'flex' : 'none';
  } catch (e) {}
  setTimeout(poll, 1000);
}
poll();
</script></body></html>
)";
  return o.str();
}

std::string FeedServer::index_html() {
  std::ostringstream o;
  o << R"(<!doctype html><html><head><meta charset="utf-8"><title>AnalogRX feeds</title>
<style>
body{font:15px system-ui,Segoe UI,sans-serif;margin:24px;background:#111;color:#eee}
table{border-collapse:collapse}td,th{padding:6px 14px;text-align:left;border-bottom:1px solid #333}
a{color:#7cc4ff}img{width:160px;height:120px;object-fit:contain;background:#000;display:block}
code{background:#222;padding:2px 6px;border-radius:4px}
</style></head><body><h2>AnalogRX feeds</h2>
<p>In OBS: <b>Sources &rarr; + &rarr; Browser</b>, URL = a pilot URL below, width 1280, height 960.
Each slot keeps its URL when the pilot in it changes.</p>
<table><tr><th>Slot</th><th>Pilot</th><th>Channel</th><th>OBS Browser Source URL</th><th></th></tr>)";
  for (int i = 1; i <= kMaxSlots; i++) {
    SlotInfo in;
    {
      std::lock_guard<std::mutex> l(_slots[i].m);
      in = _slots[i].info;
    }
    const std::string u = url(i);
    o << "<tr><td>" << i << "</td><td>" << (in.used ? html_escape(in.name) : "<i>empty</i>") << "</td><td>"
      << html_escape(in.channel) << "</td><td><a href=\"/pilot/" << i << "\"><code>" << u << "</code></a></td><td>"
      << (in.used ? "<img src=\"/feed/" + std::to_string(i) + ".mjpg\">" : "") << "</td></tr>";
  }
  o << "</table><p>Raw MJPEG: <code>/feed/N.mjpg</code> &middot; one frame: <code>/snapshot/N.jpg</code> &middot; "
       "add <code>?name=0</code> to a pilot URL to hide the name tag.</p></body></html>\n";
  return o.str();
}

}  // namespace arx
