#pragma once
// ============================================================================
//  DlnaHttp — reads media over HTTP(S) for the DLNA renderer.
//
//  HttpConn   one GET request on top of esp-tls (plain TCP or TLS with the
//             certificate bundle): redirects, chunked bodies, Content-Length
//             or "until close" bodies (live streams), Range requests,
//             read timeouts.
//  HttpFile   a remote file read at byte positions: keeps one connection and
//             reconnects with "Range: bytes=N-" when the position jumps or
//             the connection drops; caches the first 64 KB (the decoder
//             header is re-sent from there when seeking); asFile() gives a
//             FILE* so the SD card player's probe (AudioFileInfo) works on it.
//
//  On a PC (tests) plain sockets are used instead of esp-tls (http only).
//  (esp_http_client cannot read bodies that end with the connection, which
//   is what most internet radio servers send.)
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace dlna {

class HttpConn {
 public:
  static constexpr uint64_t kUnknown = ~0ull;
  struct Info {
    int status = 0;
    uint64_t contentLength = kUnknown;  // of this response body
    uint64_t total = kUnknown;          // whole resource (Content-Range)
    uint64_t start = 0;                 // first byte of the body
    bool partial = false;               // 206: Range honoured
    bool chunked = false;
    std::string contentType;
    std::string url;  // after redirects
  };

  HttpConn() = default;
  ~HttpConn();
  HttpConn(const HttpConn&) = delete;
  HttpConn& operator=(const HttpConn&) = delete;

  /** GET `url` from byte `from` (Range only if from > 0 or `range`). */
  bool open(const std::string& url, uint64_t from, bool range = true,
            int timeoutMs = 6000);
  /** >0 bytes, 0 = end of the body, <0 = error / timeout. */
  int read(uint8_t* dst, size_t n);
  void close();
  bool isOpen() const { return h_ != nullptr || fd_ >= 0; }
  const Info& info() const { return info_; }

 private:
  void* h_ = nullptr;  // esp_tls* (ESP32)
  int fd_ = -1;        // plain socket (PC tests: http only)
  Info info_;
  std::vector<uint8_t> buf_;  // bytes received after the header
  size_t bufPos_ = 0;
  uint64_t left_ = kUnknown;  // Content-Length body: bytes still to come
  uint64_t chunkLeft_ = 0;    // chunked: bytes left in the current chunk
  bool chunkEnd_ = false;

  bool request(const std::string& url, uint64_t from, bool range, int timeoutMs);
  int rawRead(uint8_t* dst, size_t n);
  bool readLine(std::string& line);
};

class HttpFile {
 public:
  /** `abort` is asked between retries (stop / new track). */
  explicit HttpFile(std::function<bool()> abort = nullptr);
  ~HttpFile();

  bool open(const std::string& url);
  void close();

  uint64_t size() const { return size_; }  // 0 = unknown (live)
  bool rangeOk() const { return rangeOk_; }
  bool seekable() const { return rangeOk_ && size_ > 0; }
  const std::string& contentType() const { return contentType_; }
  const std::string& url() const { return url_; }

  /** Read up to n bytes at pos: >0 bytes, 0 = end, <0 = error. */
  int readAt(uint64_t pos, uint8_t* dst, size_t n);
  /** Fill dst completely unless the end / an error comes first. */
  int readFull(uint64_t pos, uint8_t* dst, size_t n);

  /** FILE* reading this file (for the probe); fclose() it. */
  FILE* asFile();

 private:
  std::function<bool()> abort_;
  HttpConn conn_;
  uint64_t connPos_ = 0;
  std::string url_;
  std::string contentType_;
  uint64_t size_ = 0;
  bool rangeOk_ = false;
  std::vector<uint8_t> head_;

  bool reconnect(uint64_t pos);
};

}  // namespace dlna
