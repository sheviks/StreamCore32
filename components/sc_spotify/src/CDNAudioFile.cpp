#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <algorithm>
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for SC32_LOG
#include "Packet.h"            // for spotify
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace spotify;

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->position = newPos;
}

#ifndef CONFIG_BELL_NOCODEC
/**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  */
bool CDNAudioFile::openStream() {
  // Open connection, read first 128 bytes
  auto resp = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)}, false);
  if (!resp->stream().isOpen() || resp->httpCode() != 200) {
    return false;
  }
  size_t got = resp->readExact(header.data(), OPUS_HEADER_SIZE);
  resp->stream().close();
  if (got != OPUS_HEADER_SIZE) {
    return false;
  }
  this->totalFileSize = resp->totalLength() - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);
  if (!resp->get(cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())},
                 false)) {
    return false;
  }

  got = resp->readExact(footer.data(), footer.size());
  resp->stream().close();
  if (got != footer.size()) {
    return false;
  }
  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;

  return true;
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (position + bytes >= this->totalFileSize) {
    return 0;
  }

  // // Opus tries to read header, use prefetched data
  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  // // Opus tries to read footer, use prefetched data
  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      // Tries to read outside of bounds, truncate
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  // Data not in the headers. Make sense of whats going on.
  // Position in bounds :)
  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  } else {
    size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                        ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }

    if (!resp->get(
            cdnUrl,
            {bell::HTTPClient::RangeHeader::range(
                requestPosition, requestPosition + HTTP_BUFFER_SIZE - 1)},
            false)) {
      return 0;
    }
    this->lastRequestPosition = requestPosition;
    this->lastRequestCapacity = resp->contentLength();

    if (lastRequestCapacity > (size_t)HTTP_BUFFER_SIZE)
      lastRequestCapacity = HTTP_BUFFER_SIZE;
    size_t readBytes = resp->readExact(this->httpBuffer.data(), lastRequestCapacity);
    resp->stream().close();
    this->decrypt(this->httpBuffer.data(), readBytes,
                  this->lastRequestPosition);

    return readBytes(dst, bytes);
  }

  return bytes;
}

#else
/**
 * @brief Opens a connection to the CDN URL and fills the first buffer with track header data.
 *
 * @param header_size Reference to a size_t variable where the size of the header is stored.
 * @return Pointer to the beginning of the HTTP buffer where the track header data is stored.
 */
uint8_t* CDNAudioFile::openStream(ssize_t& header_size) {

  // Open one connection for the whole file ("bytes=0-") and keep it open:
  // the following reads continue on it (see readBytes)
  streaming_ = false;
  response = bell::HTTPClient::get(this->cdnUrl, {{"Range", "bytes=0-"}}, false);
  if (!response || !response->stream().isOpen() || response->status() < 200 ||
      response->status() >= 300) {
    return nullptr;
  }

  this->totalFileSize = response->totalLength();
  if (!this->totalFileSize)
    this->totalFileSize = response->contentLength();
  size_t want = std::min<size_t>(HTTP_BUFFER_SIZE, this->totalFileSize);
  size_t got = response->readExact(this->httpBuffer.data(), want, 8000);
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = got;
  this->decrypt(this->httpBuffer.data(), got, 0);
  if (got == want && got % 16 == 0 && got < this->totalFileSize) {
    wirePos_ = got;
    streaming_ = true;  // continue on this connection
  } else {
    response->stream().close();
  }
  this->position = getHeader();
  header_size = getHeader();
  return &httpBuffer[0];
}

/**
 * @brief Finds the position of the first audio frame in the HTTP response.
 *
 * The OGG Vorbis file starts with three headers. They contain valuable information
 * for decoding the audio.
 *
 * @return The position of the first audio frame in the HTTP response.
 */
long CDNAudioFile::getHeader() {
  uint32_t offset = SPOTIFY_OPUS_HEADER;

  for (int i = 0; i < 3; ++i) {
    offset += 26;
    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    uint8_t segmentCount = httpBuffer[offset];
    uint32_t segmentEnd = segmentCount + offset + 1;
    ++offset;

    for (uint32_t j = offset; j < segmentEnd; ++j) {
      if (offset >= HTTP_BUFFER_SIZE) {
        return HTTP_BUFFER_SIZE;
      }
      offset += httpBuffer[j];
    }

    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    offset += segmentCount;
  }

  return offset;
}
long CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  if (position + bytes >= this->totalFileSize) {
    if (position >= this->totalFileSize - 1) {
      return 0;
    } else {
      SC32_LOG(info, "Truncating read to %d bytes",
               this->totalFileSize - position);
      bytes = this->totalFileSize - position;
    }
  }

  // Serve from the buffer if position is within the last fetched window
  if (this->lastRequestCapacity > 0 && position >= this->lastRequestPosition &&
      position < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + position) >
        this->lastRequestPosition + this->lastRequestCapacity) {
      toRead = this->lastRequestPosition + this->lastRequestCapacity - position;
    }

    memcpy(dst, this->httpBuffer.data() + (position - this->lastRequestPosition),
           toRead);
    position += toRead;
    return (long)toRead;
  }

  // Not buffered: read the next window from the open connection, or open
  // a new one at the (16-byte aligned) position after a seek / an error.
  int failures = 0;
  for (int guard = 0; guard < 64 && failures < 4; guard++) {
    bool sequential = streaming_ && response && response->stream().isOpen() &&
                      position >= wirePos_ &&
                      position - wirePos_ < (size_t)HTTP_BUFFER_SIZE;
    if (!sequential) {
      size_t from = position - (position % 16);
      if (this->enableRequestMargin && from > (size_t)SEEK_MARGIN_SIZE) {
        from = (position - SEEK_MARGIN_SIZE) - ((position - SEEK_MARGIN_SIZE) % 16);
        this->enableRequestMargin = false;
      }
      if (failures)
        BELL_SLEEP_MS(200 * failures);
      if (!openWire(from)) {
        failures++;
        continue;
      }
    }

    size_t want = std::min<size_t>(HTTP_BUFFER_SIZE, this->totalFileSize - wirePos_);
    size_t got = response->readExact(this->httpBuffer.data(), want, 8000);
    if (got == 0) {
      SC32_LOG(error, "CDN: no data at %u, reconnecting", (unsigned)wirePos_);
      response->stream().close();
      streaming_ = false;
      failures++;
      continue;
    }
    // wirePos_ is 16-byte aligned (windows are multiples of 16), so the
    // AES-CTR counter can be derived from it
    this->decrypt(this->httpBuffer.data(), got, wirePos_);
    this->lastRequestPosition = wirePos_;
    this->lastRequestCapacity = got;
    wirePos_ += got;
    if (got < want || got % 16) {
      // short read: use what we have, reopen at the next read
      SC32_LOG(error, "CDN: short read %u/%u", (unsigned)got, (unsigned)want);
      response->stream().close();
      streaming_ = false;
    }
    if (wirePos_ >= this->totalFileSize) {
      response->stream().close();
      streaming_ = false;
    }

    if (position >= this->lastRequestPosition &&
        position < this->lastRequestPosition + this->lastRequestCapacity) {
      size_t toRead = bytes;
      if (toRead + position > this->lastRequestPosition + this->lastRequestCapacity)
        toRead = this->lastRequestPosition + this->lastRequestCapacity - position;
      memcpy(dst, this->httpBuffer.data() + (position - this->lastRequestPosition),
             toRead);
      position += toRead;
      return (long)toRead;
    }
    // (position is further ahead on the same connection: read on)
  }
  SC32_LOG(error, "CDN: giving up at %u of %u", (unsigned)position,
           (unsigned)this->totalFileSize);
  this->lastRequestCapacity = 0;
  streaming_ = false;
  return -1;
}

bool CDNAudioFile::openWire(size_t from) {
  streaming_ = false;
  response.reset();  // closes the previous connection
  size_t to = this->totalFileSize ? this->totalFileSize - 1 : from + HTTP_BUFFER_SIZE - 1;
  response = bell::HTTPClient::get(
      cdnUrl, {bell::HTTPClient::RangeHeader::range(from, to)}, false);
  if (!response || !response->stream().isOpen() || response->status() < 200 ||
      response->status() >= 300) {
    SC32_LOG(error, "CDN: request at %u failed (status %d)", (unsigned)from,
             response ? response->status() : -1);
    response.reset();
    return false;
  }
  wirePos_ = from;
  streaming_ = true;
  return true;
}
#endif

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  if (audioKey.size() != 16 && audioKey.size() != 24 && audioKey.size() != 32) {
    throw std::runtime_error("Invalid AES key length");
  }
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}