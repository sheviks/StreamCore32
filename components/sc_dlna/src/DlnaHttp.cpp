#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // fopencookie
#endif
#include "DlnaHttp.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dlna {

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kHeadCache = 64 * 1024;  // first bytes of a file kept
constexpr uint64_t kMaxSkip = 128 * 1024; // read over instead of a new request
constexpr const char* kUserAgent = "StreamCore32/1.0 UPnP/1.0 DLNADOC/1.50";

struct Url {
  bool tls = false;
  std::string host;
  uint16_t port = 80;
  std::string path = "/";
};

bool parseUrl(const std::string& u, Url& o) {
  size_t p;
  if (u.compare(0, 7, "http://") == 0) {
    p = 7;
    o.tls = false;
    o.port = 80;
  } else if (u.compare(0, 8, "https://") == 0) {
    p = 8;
    o.tls = true;
    o.port = 443;
  } else {
    return false;
  }
  size_t slash = u.find_first_of("/?#", p);
  std::string hp = u.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
  size_t at = hp.rfind('@');
  if (at != std::string::npos)
    hp = hp.substr(at + 1);
  if (!hp.empty() && hp[0] == '[') {  // [IPv6]:port
    size_t e = hp.find(']');
    if (e == std::string::npos)
      return false;
    o.host = hp.substr(1, e - 1);
    if (e + 1 < hp.size() && hp[e + 1] == ':')
      o.port = (uint16_t)atoi(hp.c_str() + e + 2);
  } else {
    size_t c = hp.rfind(':');
    if (c != std::string::npos) {
      o.host = hp.substr(0, c);
      o.port = (uint16_t)atoi(hp.c_str() + c + 1);
    } else {
      o.host = hp;
    }
  }
  if (o.host.empty() || !o.port)
    return false;
  o.path = slash == std::string::npos ? "/" : u.substr(slash);
  if (o.path[0] != '/')
    o.path = "/" + o.path;
  auto hash = o.path.find('#');
  if (hash != std::string::npos)
    o.path.resize(hash);
  return true;
}

std::string resolve(const std::string& base, const std::string& loc) {
  if (loc.find("://") != std::string::npos)
    return loc;
  Url b;
  if (!parseUrl(base, b))
    return loc;
  std::string origin = std::string(b.tls ? "https://" : "http://") + b.host;
  if (b.port != (b.tls ? 443 : 80))
    origin += ":" + std::to_string(b.port);
  if (loc.compare(0, 2, "//") == 0)
    return std::string(b.tls ? "https:" : "http:") + loc;
  if (!loc.empty() && loc[0] == '/')
    return origin + loc;
  std::string dir = b.path.substr(0, b.path.find('?'));
  dir = dir.substr(0, dir.rfind('/') + 1);
  return origin + dir + loc;
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a]))
    a++;
  while (b > a && std::isspace((unsigned char)s[b - 1]))
    b--;
  return s.substr(a, b - a);
}

}  // namespace

// ================================================================ HttpConn ==
HttpConn::~HttpConn() {
  close();
}

void HttpConn::close() {
#ifdef ESP_PLATFORM
  if (h_)
    esp_tls_conn_destroy(static_cast<esp_tls_t*>(h_));
#else
  if (fd_ >= 0)
    ::close(fd_);
#endif
  h_ = nullptr;
  fd_ = -1;
  buf_.clear();
  bufPos_ = 0;
}

int HttpConn::rawRead(uint8_t* dst, size_t n) {
  if (bufPos_ < buf_.size()) {
    size_t k = std::min(n, buf_.size() - bufPos_);
    memcpy(dst, buf_.data() + bufPos_, k);
    bufPos_ += k;
    if (bufPos_ >= buf_.size()) {
      buf_.clear();
      bufPos_ = 0;
    }
    return (int)k;
  }
#ifdef ESP_PLATFORM
  if (!h_)
    return -1;
  ssize_t r = esp_tls_conn_read(static_cast<esp_tls_t*>(h_), dst, n);
#else
  if (fd_ < 0)
    return -1;
  ssize_t r = recv(fd_, dst, n, 0);
#endif
  return r > 0 ? (int)r : r == 0 ? 0 : -1;
}

bool HttpConn::readLine(std::string& line) {
  line.clear();
  uint8_t c;
  while (line.size() < 1024) {
    if (rawRead(&c, 1) != 1)
      return false;
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      return true;
    }
    line += char(c);
  }
  return false;
}

bool HttpConn::request(const std::string& url, uint64_t from, bool range,
                       int timeoutMs) {
  close();
  info_ = Info();
  left_ = kUnknown;
  chunkLeft_ = 0;
  chunkEnd_ = false;
  Url u;
  if (!parseUrl(url, u))
    return false;

#ifdef ESP_PLATFORM
  esp_tls_t* tls = esp_tls_init();
  if (!tls)
    return false;
  esp_tls_cfg_t cfg = {};
  cfg.timeout_ms = timeoutMs;
  cfg.is_plain_tcp = !u.tls;
  if (u.tls)
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
  if (esp_tls_conn_new_sync(u.host.c_str(), (int)u.host.size(), u.port, &cfg, tls) != 1) {
    esp_tls_conn_destroy(tls);
    return false;
  }
  h_ = tls;
#else
  if (u.tls)
    return false;  // PC tests: plain http only
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res)
    return false;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0 || ::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    if (fd >= 0)
      ::close(fd);
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  fd_ = fd;
#endif

  std::string rq = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
  if (u.port != (u.tls ? 443 : 80))
    rq += ":" + std::to_string(u.port);
  rq += "\r\nUser-Agent: " + std::string(kUserAgent) + "\r\n";
  rq += "Accept: */*\r\n";
  if (range || from)
    rq += "Range: bytes=" + std::to_string(from) + "-\r\n";
  rq += "transferMode.dlna.org: Streaming\r\n";
  rq += "Connection: close\r\n\r\n";
  size_t off = 0;
  while (off < rq.size()) {
#ifdef ESP_PLATFORM
    ssize_t w = esp_tls_conn_write(static_cast<esp_tls_t*>(h_), rq.data() + off, rq.size() - off);
#else
    ssize_t w = send(fd_, rq.data() + off, rq.size() - off, 0);
#endif
    if (w <= 0) {
      close();
      return false;
    }
    off += size_t(w);
  }

  // response header
  std::string hdr;
  uint8_t tmp[512];
  size_t end = std::string::npos;
  while (end == std::string::npos) {
    int r = rawRead(tmp, sizeof tmp);
    if (r <= 0 || hdr.size() > kMaxHeader) {
      close();
      return false;
    }
    hdr.append((const char*)tmp, size_t(r));
    end = hdr.find("\r\n\r\n");
    if (end == std::string::npos) {
      // "ICY 200 OK\n\n" style servers
      size_t lf = hdr.find("\n\n");
      if (lf != std::string::npos) {
        end = lf;
        buf_.assign(hdr.begin() + lf + 2, hdr.end());
        hdr.resize(lf);
        break;
      }
    } else {
      buf_.assign(hdr.begin() + end + 4, hdr.end());
      hdr.resize(end);
    }
  }
  bufPos_ = 0;

  size_t le = hdr.find('\n');
  std::string first = trim(hdr.substr(0, le));
  size_t sp = first.find(' ');
  if (sp == std::string::npos) {
    close();
    return false;
  }
  info_.status = atoi(first.c_str() + sp + 1);
  std::string location, te;
  size_t p = le;
  while (p != std::string::npos && p < hdr.size()) {
    size_t e = hdr.find('\n', p + 1);
    std::string line = trim(hdr.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1));
    p = e;
    auto c = line.find(':');
    if (c == std::string::npos)
      continue;
    std::string k = lower(trim(line.substr(0, c)));
    std::string v = trim(line.substr(c + 1));
    if (k == "content-length")
      info_.contentLength = strtoull(v.c_str(), nullptr, 10);
    else if (k == "content-type")
      info_.contentType = v;
    else if (k == "location")
      location = v;
    else if (k == "transfer-encoding")
      te = lower(v);
    else if (k == "content-range") {
      // "bytes 100-999/1000" or "bytes 100-999/*"
      size_t s = v.find(' ');
      size_t dash = v.find('-', s);
      size_t slash = v.find('/', dash);
      if (s != std::string::npos && dash != std::string::npos) {
        info_.start = strtoull(v.c_str() + s + 1, nullptr, 10);
        if (slash != std::string::npos && v[slash + 1] != '*')
          info_.total = strtoull(v.c_str() + slash + 1, nullptr, 10);
      }
    }
  }
  if (!location.empty() && info_.status >= 300 && info_.status < 400)
    info_.url = resolve(url, location);
  else
    info_.url = url;
  info_.partial = info_.status == 206;
  if (info_.status == 200) {
    info_.start = 0;
    if (info_.contentLength != kUnknown && te.find("chunked") == std::string::npos)
      info_.total = info_.contentLength;
  }
  info_.chunked = te.find("chunked") != std::string::npos;
  if (!info_.chunked && info_.contentLength != kUnknown)
    left_ = info_.contentLength;
  return true;
}

bool HttpConn::open(const std::string& url, uint64_t from, bool range, int timeoutMs) {
  std::string u = url;
  for (int hop = 0; hop < 6; hop++) {
    if (!request(u, from, range, timeoutMs))
      return false;
    const int st = info_.status;
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) &&
        info_.url != u) {
      u = info_.url;
      close();
      continue;
    }
    if (st == 200 || st == 206)
      return true;
    close();
    return false;
  }
  close();
  return false;
}

int HttpConn::read(uint8_t* dst, size_t n) {
  if (!isOpen())
    return -1;
  if (info_.chunked) {
    if (chunkEnd_)
      return 0;
    if (chunkLeft_ == 0) {
      std::string line;
      do {  // (the CRLF after the previous chunk is an empty line)
        if (!readLine(line))
          return -1;
      } while (trim(line).empty());
      chunkLeft_ = strtoull(line.c_str(), nullptr, 16);
      if (chunkLeft_ == 0) {
        chunkEnd_ = true;
        return 0;
      }
    }
    int r = rawRead(dst, (size_t)std::min<uint64_t>(n, chunkLeft_));
    if (r <= 0)
      return -1;  // closed inside a chunk
    chunkLeft_ -= uint64_t(r);
    return r;
  }
  if (left_ != kUnknown) {
    if (left_ == 0)
      return 0;
    int r = rawRead(dst, (size_t)std::min<uint64_t>(n, left_));
    if (r <= 0)
      return -1;  // closed before Content-Length bytes
    left_ -= uint64_t(r);
    return r;
  }
  return rawRead(dst, n);  // until the connection closes (live streams)
}

// ================================================================ HttpFile ==
HttpFile::HttpFile(std::function<bool()> abort) : abort_(std::move(abort)) {}

HttpFile::~HttpFile() {
  close();
}

void HttpFile::close() {
  conn_.close();
  connPos_ = 0;
}

bool HttpFile::open(const std::string& url) {
  close();
  head_.clear();
  size_ = 0;
  rangeOk_ = false;
  url_ = url;
  if (!conn_.open(url, 0, true))
    return false;
  const auto& i = conn_.info();
  url_ = i.url;  // (no redirects on every reconnect)
  contentType_ = i.contentType;
  rangeOk_ = i.partial;
  if (i.total != HttpConn::kUnknown)
    size_ = i.total;
  else if (!i.chunked && i.contentLength != HttpConn::kUnknown && i.status == 200)
    size_ = i.contentLength;
  connPos_ = 0;
  return true;
}

bool HttpFile::reconnect(uint64_t pos) {
  conn_.close();
  if (!conn_.open(url_, pos, true))
    return false;
  const auto& i = conn_.info();
  if (pos > 0 && (!i.partial || i.start != pos)) {
    conn_.close();  // the server ignored the Range
    rangeOk_ = false;
    return false;
  }
  connPos_ = pos;
  return true;
}

int HttpFile::readAt(uint64_t pos, uint8_t* dst, size_t n) {
  if (!n)
    return 0;
  if (pos < head_.size()) {
    size_t k = std::min<uint64_t>(n, head_.size() - pos);
    memcpy(dst, head_.data() + pos, k);
    return (int)k;
  }
  if (size_ && pos >= size_)
    return 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (abort_ && abort_())
      return -1;
    if (attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
    // get the connection to `pos`
    if (conn_.isOpen() && pos > connPos_ && pos - connPos_ <= kMaxSkip) {
      uint8_t skip[512];
      while (connPos_ < pos) {
        int r = conn_.read(skip, (size_t)std::min<uint64_t>(sizeof skip, pos - connPos_));
        if (r <= 0)
          break;
        if (connPos_ == head_.size() && head_.size() < kHeadCache)
          head_.insert(head_.end(), skip, skip + std::min<size_t>(size_t(r), kHeadCache - head_.size()));
        connPos_ += uint64_t(r);
      }
      if (connPos_ != pos)
        conn_.close();
    }
    if (!conn_.isOpen() || connPos_ != pos) {
      if (!rangeOk_ && pos != 0)
        return -1;  // cannot jump on this server
      if (!reconnect(pos))
        continue;
    }
    int r = conn_.read(dst, n);
    if (r > 0) {
      if (connPos_ == head_.size() && head_.size() < kHeadCache)
        head_.insert(head_.end(), dst, dst + std::min<size_t>(size_t(r), kHeadCache - head_.size()));
      connPos_ += uint64_t(r);
      return r;
    }
    if (r == 0 && (!size_ || connPos_ >= size_))
      return 0;  // regular end
    // dropped / timed out: try again at the same position (if possible)
    conn_.close();
    if (!rangeOk_)
      return r == 0 ? 0 : -1;
  }
  return -1;
}

int HttpFile::readFull(uint64_t pos, uint8_t* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    int r = readAt(pos + got, dst + got, n - got);
    if (r < 0)
      return got ? (int)got : r;
    if (r == 0)
      break;
    got += size_t(r);
  }
  return (int)got;
}

FILE* HttpFile::asFile() {
  struct Cookie {
    HttpFile* f;
    uint64_t pos;
  };
  cookie_io_functions_t fn = {};
  fn.read = [](void* c, char* buf, size_t n) -> ssize_t {
    auto* k = static_cast<Cookie*>(c);
    int r = k->f->readFull(k->pos, reinterpret_cast<uint8_t*>(buf), n);
    if (r > 0)
      k->pos += uint64_t(r);
    return r < 0 ? -1 : r;
  };
  fn.write = nullptr;
  fn.seek = [](void* c, auto* off, int whence) -> int {
    auto* k = static_cast<Cookie*>(c);
    int64_t p;
    switch (whence) {
      case SEEK_SET:
        p = int64_t(*off);
        break;
      case SEEK_CUR:
        p = int64_t(k->pos) + int64_t(*off);
        break;
      case SEEK_END:
        if (!k->f->size_)
          return -1;
        p = int64_t(k->f->size_) + int64_t(*off);
        break;
      default:
        return -1;
    }
    if (p < 0)
      return -1;
    k->pos = uint64_t(p);
    *off = p;
    return 0;
  };
  fn.close = [](void* c) -> int {
    delete static_cast<Cookie*>(c);
    return 0;
  };
  auto* k = new Cookie{this, 0};
  FILE* f = fopencookie(k, "rb", fn);
  if (!f)
    delete k;
  return f;
}

}  // namespace dlna
