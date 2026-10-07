#pragma once
#include <chrono>

#include <iostream>  // for streamsize, basic_streambuf<>::int_type, ios...
#include <memory>    // for unique_ptr, operator!=
#include <string>    // for char_traits, string

#include "BellSocket.h"  // for Socket
#include "BellUtils.h"

namespace bell {
class SocketBuffer : public std::streambuf {
 private:
  std::unique_ptr<bell::Socket> internalSocket;

  static const int bufLen = 1024;
  char ibuf[bufLen], obuf[bufLen];

 public:
  SocketBuffer() { internalSocket = nullptr; }

  SocketBuffer(const std::string& hostname, int port, bool isSSL = false) {
    open(hostname, port);
  }

  int open(const std::string& hostname, int port, bool isSSL = false);

  int close();

  bool isOpen() {
    return internalSocket != nullptr && internalSocket->isOpen();
  }
  ssize_t readSome(char* dst, size_t len);
  ssize_t writeSome(const char* src, size_t len);
  size_t available();
  ~SocketBuffer() { close(); }
  virtual std::streamsize showmanyc() override;

  virtual std::streamsize xsgetn(char_type* __s, std::streamsize __n);

  virtual std::streamsize xsputn(const char_type* __s, std::streamsize __n);

 protected:
  virtual int sync();

  virtual int_type underflow();

  virtual int_type overflow(int_type c = traits_type::eof());
};

class SocketStream : public std::iostream {
 private:
  SocketBuffer socketBuf;

 public:
  SocketStream() : std::iostream(&socketBuf) {}

  SocketStream(const std::string& hostname, int port, bool isSSL = false)
      : std::iostream(&socketBuf) {
    open(hostname, port, isSSL);
  }

  SocketBuffer* rdbuf() { return &socketBuf; }

  int open(const std::string& hostname, int port, bool isSSL = false) {
    int err = socketBuf.open(hostname, port, isSSL);
    if (err){
      setstate(std::ios::failbit);
    }
    return err;
  }

  int close() { return socketBuf.close(); }
  ssize_t readSome(char* dst, size_t len);
  ssize_t writeSome(const char* src, size_t len);
  size_t available();
  size_t readExact(char* dst, size_t n, uint32_t idle_timeout_ms = 5000) {
    // idle time is measured on the clock: a read may itself block for the
    // socket timeout before it returns 0
    size_t total = 0;
    auto idleSince = std::chrono::steady_clock::now();
    while (total < n) {
      ssize_t got = socketBuf.xsgetn(dst + total, n - total);
      if (got <= 0) {
        if (!isOpen())
          break;  // bubble up closed socket
        auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - idleSince)
                        .count();
        if (idle >= (long long)idle_timeout_ms)
          break;
        BELL_SLEEP_MS(5);
        continue;
      }
      idleSince = std::chrono::steady_clock::now();
      total += size_t(got);
    }
    return total;  // == n on success
  }
  bool isOpen() { return socketBuf.isOpen(); }
};
}  // namespace bell
