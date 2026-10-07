#pragma once
#include "BellHTTPServer.h"
#include "EspRandomEngine.h"
#include "QobuzConfig.h"
#include "StreamBase.h"
#include "ZeroConf.h"

#include "QobuzPlayer.h"
#include "QobuzQueue.h"
#include "QobuzTrack.h"
#include "StreamCoreFile.h"
#include "TimeSync.h"
#include "WebSocketClient.h"
#include "WsManager.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include "NanoPBHelper.h"
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#include "protobuf/qconnect_envelope.pb.h"
#include "protobuf/qconnect_payload.pb.h"

#include "Logger.h"
/**
 * Qobuz Connect renderer.
 *
 * StreamBase control API: pause/resume/seek act on the player directly and
 * the new state is reported to Qobuz (like a remote command would).
 * next/previous/playQueueItem move inside the Qobuz queue (shuffle aware).
 * Repeat and shuffle are sent to the Qobuz server as controller commands, so
 * all connected apps stay in sync.
 */
class QobuzStream : public StreamBase {
 public:
  using reportStatusFunc = std::function<void(const std::string& status)>;
  using OnUiMessageFunc = std::function<void(const std::string& msg)>;
  struct SessionId {
    size_t browserId = 0;
    uint8_t raw[16];       // use this in protobuf (len = 16)
    std::string hex32;     // for logs / hex style
    std::string b64url22;  // for logs / 22-char style
  };
  struct Token {
    std::string token;
    uint64_t expiresAt = 0;
  };
  struct Config {
    std::string name = "StreamCore32";  // shown in the Qobuz apps / mDNS
    uint8_t maxFormat = 7;  // 5 MP3, 6 CD, 7 Hi-Res <= 96k, 27 Hi-Res <= 192k
    std::string appId;      // request from Qobuz
    std::string appSecret;  // request from Qobuz
    std::string email;
    std::string password;
    std::string userAuthToken;
    std::string userId;
    std::string infos;
    SessionId session_id;
    Token XsessionId;
    uint8_t XsessionId_raw[16];
    std::string queueSnapshot;
    std::string api_base = "https://www.qobuz.com/api.json/0.2";
    char queue_uuid[37];
    uint8_t volume = 0;
    uint32_t expiresAt = 0;
    uint64_t rendererId = 0;
    WSToken ws_token;
    Token api_token;
    bool empty() const {
      return appId.empty() && appSecret.empty() && email.empty() &&
             password.empty();
    }
  };

  static bool rand16(uint8_t out[16]) {
    static mbedtls_entropy_context ent;
    static mbedtls_ctr_drbg_context drbg;
    static bool inited = false;
    if (!inited) {
      mbedtls_entropy_init(&ent);
      mbedtls_ctr_drbg_init(&drbg);
      const char* pers = "qcloud";
      if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                (const unsigned char*)pers, strlen(pers)) != 0)
        return false;
      inited = true;
    }
    return mbedtls_ctr_drbg_random(&drbg, out, 16) == 0;
  }

  static std::string hex32_from16(const uint8_t b[16]) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.resize(32);
    for (int i = 0; i < 16; i++) {
      s[2 * i] = H[b[i] >> 4];
      s[2 * i + 1] = H[b[i] & 0xF];
    }
    return s;
  }
  static inline std::string makeOpenEndedRangeHeader(int64_t start) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "bytes=%lld-", (long long)start);
    return tmp;
  }

  static std::string b64url(const uint8_t* s, size_t n) {
    static const char* B =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    while (i < n) {
      uint32_t v = uint32_t(s[i++]) << 16;
      bool have2 = (i < n);
      if (have2)
        v |= uint32_t(s[i++]) << 8;
      bool have3 = (i < n);
      if (have3)
        v |= uint32_t(s[i++]);

      out.push_back(B[(v >> 18) & 63]);
      out.push_back(B[(v >> 12) & 63]);
      out.push_back(have2 ? B[(v >> 6) & 63] : '\0');
      out.push_back(have3 ? B[(v) & 63] : '\0');

      if (!have2) {
        out.pop_back();
        out.pop_back();
      }  // 1 byte => 2 chars
      else if (!have3) {
        out.pop_back();
      }  // 2 bytes => 3 chars
    }
    return out;
  }
  static void uuid_v4(char out[37]) {
    uint8_t b[16];
    // use your existing DRBG init (you already have mbedTLS set up)
    mbedtls_entropy_context ent;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ctr_drbg_init(&drbg);
    const char* pers = "qobuz-context";
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                          (const unsigned char*)pers, strlen(pers));
    mbedtls_ctr_drbg_random(&drbg, b, 16);
    // UUID v4 bits
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(
        out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
        b[11], b[12], b[13], b[14], b[15]);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
  }
  static SessionId createSessionId() {
    SessionId id{};
    if (!rand16(id.raw)) { /* handle error */
    }
    id.hex32 = hex32_from16(id.raw);
    id.b64url22 = b64url(id.raw, 16);
    return id;
  }
  static std::string formatSessionId(const SessionId& id) {
    char out[37];  // 36 chars + '\0'
    const uint8_t* b = id.raw;

    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-"
                  "%02x%02x-"
                  "%02x%02x-"
                  "%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);

    return std::string(out);
  }

  // --- parse "7c8183a6-9857-4647-85ec-d299f45a39cc" into SessionId ----
  static int hexNibble(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
      return 10 + (c - 'A');
    return -1;
  }

  static bool parseSessionId(const std::string& s, SessionId& out) {
    // strictly enforce UUID-like layout
    if (s.size() != 36)
      return false;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
      return false;

    int bi = 0;
    for (size_t i = 0; i < s.size();) {
      if (s[i] == '-') {
        ++i;
        continue;
      }

      if (i + 1 >= s.size() || bi >= 16)
        return false;

      int hi = hexNibble(s[i++]);
      int lo = hexNibble(s[i++]);
      if (hi < 0 || lo < 0)
        return false;

      out.raw[bi++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (bi != 16)
      return false;

    // keep the other representations in sync
    out.hex32 = hex32_from16(out.raw);
    out.b64url22 = b64url(out.raw, 16);
    return true;
  }
#include <algorithm>
#include <string>
#include <string_view>

  // assumes your enum is already defined exactly as you posted:
  // enum QConnectMessageType { ... };

  inline std::string_view messageTypeToText(qconnect_QConnectMessageType t) {
    using sv = std::string_view;
    switch (t) {
      case qconnect_QConnectMessageType_MESSAGE_TYPE_UNKNOWN:
        return sv("MESSAGE_TYPE_UNKNOWN");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_ERROR:
        return sv("MESSAGE_TYPE_ERROR");

        // Renderer ↔ Server
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_JOIN_SESSION:
        return sv("MESSAGE_TYPE_RNDR_SRVR_JOIN_SESSION");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_DEVICE_INFO_UPDATED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_DEVICE_INFO_UPDATED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_STATE_UPDATED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_STATE_UPDATED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_RENDERER_ACTION:
        return sv("MESSAGE_TYPE_RNDR_SRVR_RENDERER_ACTION");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_VOLUME_CHANGED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_VOLUME_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_FILE_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_FILE_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_DEVICE_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_DEVICE_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_MAX_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_MAX_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_RNDR_SRVR_VOLUME_MUTED:
        return sv("MESSAGE_TYPE_RNDR_SRVR_VOLUME_MUTED");

        // Server → Renderer
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_STATE:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_VOLUME:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_VOLUME");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_ACTIVE:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_ACTIVE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_MAX_AUDIO_QUALITY:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_MAX_AUDIO_QUALITY");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_LOOP_MODE:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_LOOP_MODE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_SHUFFLE_MODE:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_SHUFFLE_MODE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_RNDR_SET_AUTOPLAY_MODE:
        return sv("MESSAGE_TYPE_SRVR_RNDR_SET_AUTOPLAY_MODE");

        // Controller → Server
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_JOIN_SESSION:
        return sv("MESSAGE_TYPE_CTRL_SRVR_JOIN_SESSION");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_PLAYER_STATE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_PLAYER_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_ACTIVE_RENDERER:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_ACTIVE_RENDERER");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_VOLUME:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_VOLUME");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_CLEAR_QUEUE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_CLEAR_QUEUE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_QUEUE_LOAD_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_QUEUE_LOAD_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_QUEUE_INSERT_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_QUEUE_INSERT_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_QUEUE_ADD_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_QUEUE_ADD_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_QUEUE_REMOVE_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_QUEUE_REMOVE_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_QUEUE_REORDER_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_QUEUE_REORDER_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_SHUFFLE_MODE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_SHUFFLE_MODE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_LOOP_MODE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_LOOP_MODE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_MUTE_VOLUME:
        return sv("MESSAGE_TYPE_CTRL_SRVR_MUTE_VOLUME");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_MAX_AUDIO_QUALITY:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_MAX_AUDIO_QUALITY");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_QUEUE_STATE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_QUEUE_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_QUEUE_STATE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_QUEUE_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_RENDERER_STATE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_ASK_FOR_RENDERER_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_SET_AUTOPLAY_MODE:
        return sv("MESSAGE_TYPE_CTRL_SRVR_SET_AUTOPLAY_MODE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_AUTOPLAY_ADD_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_AUTOPLAY_LOAD_TRACKS");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_CTRL_SRVR_AUTOPLAY_REMOVE_TRACKS:
        return sv("MESSAGE_TYPE_CTRL_SRVR_AUTOPLAY_REMOVE_TRACKS");

        // Server → Controllers
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_SESSION_STATE:
        return sv("MESSAGE_TYPE_SRVR_CTRL_SESSION_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_RENDERER_STATE_UPDATED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_RENDERER_STATE_UPDATED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_ADD_RENDERER:
        return sv("MESSAGE_TYPE_SRVR_CTRL_ADD_RENDERER");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_UPDATE_RENDERER:
        return sv("MESSAGE_TYPE_SRVR_CTRL_UPDATE_RENDERER");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_REMOVE_RENDERER:
        return sv("MESSAGE_TYPE_SRVR_CTRL_REMOVE_RENDERER");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_ACTIVE_RENDERER_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_ACTIVE_RENDERER_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_VOLUME_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_VOLUME_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_ERROR_MESSAGE:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_ERROR_MESSAGE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_CLEARED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_CLEARED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_STATE:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_STATE");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_LOADED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_LOADED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_INSERTED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_INSERTED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_ADDED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_ADDED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_REMOVED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_REMOVED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_REORDERED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_TRACKS_REORDERED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_SHUFFLE_MODE_SET:
        return sv("MESSAGE_TYPE_SRVR_CTRL_SHUFFLE_MODE_SET");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_LOOP_MODE_SET:
        return sv("MESSAGE_TYPE_SRVR_CTRL_LOOP_MODE_SET");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_VOLUME_MUTED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_VOLUME_MUTED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_MAX_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_MAX_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_FILE_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_FILE_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_DEVICE_AUDIO_QUALITY_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_DEVICE_AUDIO_QUALITY_CHANGED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_MODE_SET:
        return sv("MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_MODE_SET");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_LOADED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_LOADED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_REMOVED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_AUTOPLAY_TRACKS_REMOVED");
      case qconnect_QConnectMessageType_MESSAGE_TYPE_SRVR_CTRL_QUEUE_VERSION_CHANGED:
        return sv("MESSAGE_TYPE_SRVR_CTRL_QUEUE_VERION_CHANGED");  //setIndex
    }
    return sv("MESSAGE_TYPE_UNKNOWN");
  }

  // overload for raw numeric codes (safe-casts out-of-range to UNKNOWN)
  inline std::string_view messageTypeToText(int code) {
    return messageTypeToText(static_cast<qconnect_QConnectMessageType>(code));
  }

  // optional: human-friendly text like "RNDR SRVR JOIN SESSION"
  inline std::string messageTypeToFriendlyText(qconnect_QConnectMessageType t) {
    std::string s(messageTypeToText(t));
    constexpr std::string_view prefix = "MESSAGE_TYPE_";
    if (s.rfind(prefix.data(), 0) == 0)
      s.erase(0, prefix.size());
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
  }

  QobuzStream(std::shared_ptr<AudioControl> audio, const Config& cfg,
              std::unique_ptr<StreamCoreFile> creds_,
              std::function<void(bool)> onConnect)
      : StreamBase("qobuz_ctrl", audio, 1024 * 8, 4, 1, true),
        onConnect_(onConnect) {
    creds = std::move(creds_);
    audioControl_ = audio;
    cfg_ = cfg;
    cfg_.session_id = createSessionId();
    if (!cfg_.XsessionId.token.empty()) {
      SessionId sid;
      parseSessionId(cfg_.XsessionId.token, sid);
      cfg_.XsessionId.token = sid.hex32;
      memcpy(cfg_.XsessionId_raw, sid.raw, 16);
    }
    if (cfg_.appId.empty() || cfg_.appSecret.empty()) {
      Record appInfo;
      if (creds->load("appInfo", &appInfo) == 0) {
        for (auto& f : appInfo.fields) {
          if (f.name == "appId")
            cfg_.appId = std::string(f.value.begin(), f.value.end());
          if (f.name == "appSecret")
            cfg_.appSecret = std::string(f.value.begin(), f.value.end());
        }
      }
      if (cfg_.appId.empty()) {
        QobuzConfig::ClientAppSecrets secrets;
        config_ = std::make_unique<QobuzConfig>(&secrets);
        config_->loadedSemaphore->wait();
        cfg_.appId = secrets.id;
        cfg_.appSecret = secrets.secrets.begin()->second;
        config_.reset();
        Record appInfo;
        appInfo.userkey = "appInfo";
        appInfo.fields.push_back({"appId", cfg_.appId});
        appInfo.fields.push_back({"appSecret", cfg_.appSecret});
        creds->save(appInfo, true);
      }
    }
    const std::string uuidHex = QobuzStream::formatSessionId(cfg_.session_id);

    ZeroconfServiceManager::ServiceSpec spec;
    spec.key = "qobuz";
    spec.serviceType = "_qobuz-connect";  // same as before
    spec.proto = "_tcp";
    spec.instanceName = cfg_.name;  // how it appears in mDNS
    spec.txt = {
        {"path", "/streamcore"},       {"type", "SPEAKER"},
        {"sdk_version", "sc32-1.0.0"}, {"Name", cfg_.name},
        {"device_uuid", uuidHex},
    };
    // ------------------- /streamcore/get-display-info -------------------
    spec.endpoints.push_back(
        {ZeroconfServiceManager::HttpMethod::GET,
         "/streamcore/get-display-info",
         [uuidHex, name = cfg_.name](mg_connection* c) -> std::string {
           nlohmann::json j = {{"type", "SPEAKER"},
                               {"friendly_name", name},
                               {"model_display_name", "StreamCore32 ESP32"},
                               {"brand_display_name", "StreamCore"},
                               {"serial_number", uuidHex},
                               {"max_audio_quality", "HIRES_L3"}};
           auto s = j.dump();
           return s;
         }});
    // ------------------- /streamcore/get-connect-info -------------------
    spec.endpoints.push_back(
        {ZeroconfServiceManager::HttpMethod::GET,
         "/streamcore/get-connect-info",
         [appId = cfg_.appId](mg_connection* c) -> std::string {
           nlohmann::json j = {{"current_session_id", ""}, {"app_id", appId}};
           auto s = j.dump();
           return s;
         }});
    // ------------------- /streamcore/connect-to-qconnect ----------------
    spec.endpoints.push_back(
        {ZeroconfServiceManager::HttpMethod::POST,
         "/streamcore/connect-to-qconnect",
         [this](mg_connection* c) -> std::string {
           const mg_request_info* ri = mg_get_request_info(c);
           std::string body;

           if (ri && ri->content_length > 0) {
             body.resize(ri->content_length);
             mg_read(c, body.data(), ri->content_length);

             try {
               onConnect_(true);
               nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
               if (!j.is_discarded()) {
                 SessionId sid;
                 parseSessionId(j.at("session_id").get<std::string>(), sid);
                 this->cfg_.XsessionId.token = sid.hex32;
                 memcpy(this->cfg_.XsessionId_raw, sid.raw, 16);

                 this->cfg_.ws_token.endpoint =
                     j.at("jwt_qconnect").at("endpoint").get<std::string>();
                 this->cfg_.ws_token.jwt =
                     j.at("jwt_qconnect").at("jwt").get<std::string>();
                 this->cfg_.ws_token.exp_s =
                     j.at("jwt_qconnect").at("exp").get<uint64_t>();

                 this->cfg_.api_token.token =
                     j.at("jwt_api").at("jwt").get<std::string>();
                 this->cfg_.api_token.expiresAt =
                     j.at("jwt_api").at("exp").get<uint64_t>() * 1000;
                 this->isActive_.store(true);
                 this->startTask();
               }
             } catch (const std::exception& e) {
               SC32_LOG(error, "connect-to-qconnect parse error: %s", e.what());
             }
           }
           return "{}";
         }});
    zeroconf.addService(spec);
  }
  ~QobuzStream() {}

  bool init();
  bool login();
  bool startSession();
  WSToken getWSToken();
  void WSRegisterController();
  void WSDecodePayload(std::vector<uint8_t> data);
  void WSDecodeMessage(_qconnect_QConnectMessage* data);
  void WSSetRendererActive();
  void WSAskForQueueState();
  void WSAskForRendererState();
  void WSSetRendererVolume();
  bool refreshApiToken();
  using StreamBase::open;  // (keep the base overload visible: -Woverloaded-virtual)
  bool open(const std::string& trackId);
  void stop() override {
    if (auto p = getPlayer()) {
      p->stopTask();
      while (p->isRunning())
        BELL_SLEEP_MS(10);
    }
    {
      std::scoped_lock lock(objMutex_);
      this->player_.reset();
      this->queue_.reset();
    }
    setPlaybackState(PlaybackState::Stopped);
    if (this->wsManager) {
      this->wsManager->stop();
      this->wsManager.reset();
    }
    if (token_hb_)
      token_hb_.reset();
    isActive_.store(false);
    onConnect_(false);
  }
  void encodeBatches(qconnect_QConnectMessage* args, size_t count) {
    uint64_t ts = timesync::now_ms();
    qconnect_QConnectBatch batch = {1, ts, 1, ++message_id, count, NULL};
    batch.messages = (qconnect_QConnectMessage*)calloc(
        count, sizeof(qconnect_QConnectMessage));
    for (int i = 0; i < count; i++) {
      batch.messages[i] = args[i];
    }
    batch.messages_count = count;
    auto payload = pbEncode(qconnect_QConnectBatch_fields, &batch);
    wsManager->send(qconnect_QCloudMessageType_PAYLOAD, payload, {{0x2}}, ts);
    pb_release(qconnect_QConnectBatch_fields, &batch);
  }

  static std::string uuid_from_16(const uint8_t b[16]) {
    auto hex2 = [](uint8_t v) {
      char s[3];
      snprintf(s, sizeof(s), "%02x", v);
      return std::string(s);
    };
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; i++) {
      s += hex2(b[i]);
      if (i == 3 || i == 5 || i == 7 || i == 9)
        s.push_back('-');
    }
    return s;
  }

  // onUiMessage_ (OnUiMessageFunc) is inherited from StreamBase

  void runTask() override;

  // ======================================================= StreamBase API
  const char* sourceName() const override { return "Qobuz"; }
  /** Quality for the next tracks (see Config::maxFormat). */
  void setMaxFormat(uint8_t fmt) {
    cfg_.maxFormat = fmt;
    std::scoped_lock lock(objMutex_);
    if (queue_)
      queue_->setAudioFormat((qobuz::AudioFormat)fmt);
  }
  bool isActive() override { return isActive_.load(); }
  Capabilities capabilities() override;
  /** Qobuz is driven by Qobuz Connect: play() only resumes. */
  void play(const std::string& uri,
            const std::string& displayName = std::string()) override {
    (void)uri;
    (void)displayName;
    resume();
  }
  void pause() override;
  void resume() override;
  bool seek(uint32_t positionMs) override;
  bool next() override;
  bool previous() override;
  bool setShuffle(bool on) override;
  bool setRepeat(RepeatMode mode) override;
  void setVolume(uint8_t percent) override;
  std::vector<TrackInfo> getQueue(size_t maxItems = 20) override;
  bool playQueueItem(size_t index) override;
  NowPlaying nowPlaying() override;

  void WSSetLoopMode(qconnect_LoopMode mode);
  void WSSetShuffleMode(bool on);

 private:
  using MetaCb = std::function<void(const std::string&, const std::string&)>;
  using ErrorCb = std::function<void(const std::string&)>;
  using StateCb = std::function<void(bool)>;
  std::function<void(bool)> onConnect_ = nullptr;

  std::unique_ptr<bell::HTTPClient::Response> qobuzGet(
      const std::string& url_base,
      const std::string& object,  //session, user, file
      const std::string& action,  // start, url...
      const std::vector<std::pair<std::string, std::string>>& headers = {},
      const std::vector<std::pair<std::string, std::string>>& params = {},
      const std::string& request_ts = "", const std::string& app_secret = "");
  std::unique_ptr<bell::HTTPClient::Response> qobuzPost(
      const std::string& url_base, const std::string& object,
      const std::string& action,
      const std::vector<std::pair<std::string, std::string>>& headers = {},
      const std::string& body = "",
      const std::vector<std::pair<std::string, std::string>>& params = {},
      const std::string& request_ts = "", const std::string& app_secret = "");
  //void reportStreamingEnd();
  //void reportStreamingStart();
  Config cfg_;
  std::unique_ptr<Heartbeat> token_hb_;

  std::unique_ptr<StreamCoreFile> creds;
  std::unique_ptr<QobuzConfig> config_;
  std::shared_ptr<QobuzPlayer> player_;
  std::shared_ptr<AudioControl> audioControl_;
  std::shared_ptr<qobuz::QobuzQueue> queue_;
  std::mutex objMutex_;  // guards player_/queue_ pointers for UI threads
  std::shared_ptr<QobuzPlayer> getPlayer() {
    std::scoped_lock lock(objMutex_);
    return player_;
  }
  std::shared_ptr<qobuz::QobuzQueue> getQueueObj() {
    std::scoped_lock lock(objMutex_);
    return queue_;
  }
  /** Logical queue position of the audible track. */
  bool playingPosition(size_t& pos);
  /** Jump to a logical queue position (local next/prev/play index). */
  bool jumpTo(size_t logicalPos);
  std::atomic<uint8_t> loopMode_{qconnect_LoopMode_LOOP_MODE_OFF};
  std::atomic<bool> shuffle_{false};
  std::unique_ptr<WsManager> wsManager;

  std::atomic<bool> isActive_{false};
  std::atomic<bool> resetPlayer_{false};
  std::atomic<bool> sentLoadedTracks_{false};
  int32_t message_id = 0;
};
