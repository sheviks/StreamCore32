#include "SdFiles.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "BellHTTPServer.h"
#include "Logger.h"
#include "SDFileStream.h"
#include "SD_Master.h"
#include "civetweb.h"
#include "esp_vfs_fat.h"

namespace sdfiles {

namespace {

constexpr size_t kMaxTextFile = 64 * 1024;
constexpr size_t kChunk = 8 * 1024;
std::function<void(const std::string&)> g_beforeChange;
SD_Master* g_sd = nullptr;

std::string parentOf(const std::string& p) {
  auto s = p.find_last_of('/');
  return s == std::string::npos || s == 0 ? "/" : p.substr(0, s);
}
std::string baseOf(const std::string& p) {
  auto s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}
std::string lowerExt(const std::string& name) {
  auto d = name.find_last_of('.');
  if (d == std::string::npos)
    return "";
  std::string e = name.substr(d + 1);
  for (auto& c : e)
    c = (char)tolower((unsigned char)c);
  return e;
}
bool isTextName(const std::string& name) {
  static const char* t[] = {"txt", "m3u", "m3u8", "pls", "cfg", "conf", "json",
                            "ini", "csv", "log", "md", "xml", "html", "htm",
                            "js",  "css", "nfo", "cue", "lrc", "yaml", "yml"};
  std::string e = lowerExt(name);
  for (auto* x : t)
    if (e == x)
      return true;
  return false;
}
bool isDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

nlohmann::json result(bool ok, const std::string& op, const std::string& msg = "") {
  nlohmann::json j;
  j["type"] = "fs.result";
  j["op"] = op;
  j["ok"] = ok;
  if (!msg.empty())
    j["message"] = msg;
  return j;
}
std::string errText(const char* what) {
  return std::string(what) + ": " + strerror(errno);
}

bool mounted() {
  return g_sd && g_sd->isMounted();
}

// remove a file or a folder with all its content (depth limited)
bool removeTree(const std::string& p, int depth = 0) {
  if (depth > 16)
    return false;
  if (!isDir(p))
    return unlink(p.c_str()) == 0;
  DIR* d = opendir(p.c_str());
  if (!d)
    return false;
  std::vector<std::string> names;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr)
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
      names.push_back(e->d_name);
  closedir(d);
  for (auto& n : names)
    if (!removeTree(p + "/" + n, depth + 1))
      return false;
  return rmdir(p.c_str()) == 0;
}

void beforeChange(const std::string& p) {
  if (g_beforeChange)
    g_beforeChange(p);
}

// ---- operations --------------------------------------------------------------
nlohmann::json list(const std::string& path) {
  nlohmann::json out;
  out["type"] = "fs";
  out["root"] = SD_Master::kMountPoint;
  out["path"] = path;
  out["entries"] = nlohmann::json::array();
  if (!mounted()) {
    out["error"] = "No SD card";
    return out;
  }
  uint64_t total = 0, freeB = 0;
  if (esp_vfs_fat_info(SD_Master::kMountPoint, &total, &freeB) == ESP_OK) {
    out["total"] = total;
    out["free"] = freeB;
  }
  DIR* d = opendir(path.c_str());
  if (!d) {
    out["error"] = errText("cannot open folder");
    return out;
  }
  struct Ent {
    std::string name;
    bool dir;
    uint32_t size;
    long mtime;
  };
  std::vector<Ent> ents;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr && ents.size() < 1000) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
        !strncmp(e->d_name, "._", 2))
      continue;
    std::string full = path + "/" + e->d_name;
    struct stat st;
    bool haveStat = stat(full.c_str(), &st) == 0;
    bool dir = e->d_type == DT_DIR ||
               (e->d_type == DT_UNKNOWN && haveStat && S_ISDIR(st.st_mode));
    ents.push_back({e->d_name, dir, (!dir && haveStat) ? (uint32_t)st.st_size : 0u,
                    haveStat ? (long)st.st_mtime : 0L});
  }
  closedir(d);
  std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
    if (a.dir != b.dir)
      return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  for (auto& x : ents) {
    nlohmann::json j = {{"name", x.name}, {"dir", x.dir}, {"size", x.size},
                        {"mtime", x.mtime}};
    if (!x.dir) {
      j["audio"] = SDFileStream::isAudioFile(x.name);
      j["playlist"] = SDFileStream::isPlaylistFile(x.name);
      j["text"] = isTextName(x.name);
    }
    out["entries"].push_back(std::move(j));
  }
  return out;
}

nlohmann::json readText(const std::string& path) {
  nlohmann::json out;
  out["type"] = "fs.file";
  out["path"] = path;
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
    out["error"] = "not a file";
    return out;
  }
  if ((size_t)st.st_size > kMaxTextFile) {
    out["error"] = "file too large to edit (max 64 KB)";
    return out;
  }
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    out["error"] = errText("cannot open");
    return out;
  }
  std::string s((size_t)st.st_size, '\0');
  size_t n = fread(s.data(), 1, s.size(), f);
  fclose(f);
  s.resize(n);
  out["content"] = s;
  out["size"] = n;
  return out;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  unlink(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string playlistPath(std::string p) {
  if (p.find('/') == std::string::npos)  // just a name
    p = std::string(kPlaylistDir) + "/" + p;
  if (!SDFileStream::isPlaylistFile(p))
    p += ".m3u";
  return p;
}

nlohmann::json playlists() {
  nlohmann::json out;
  out["type"] = "pl.list";
  out["dir"] = kPlaylistDir;
  out["playlists"] = nlohmann::json::array();
  if (!mounted())
    return out;
  DIR* d = opendir(kPlaylistDir);
  if (!d)
    return out;
  std::vector<std::string> names;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr)
    if (e->d_name[0] != '.' && SDFileStream::isPlaylistFile(e->d_name))
      names.push_back(e->d_name);
  closedir(d);
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  for (auto& n : names) {
    std::string p = std::string(kPlaylistDir) + "/" + n;
    out["playlists"].push_back(
        {{"name", n}, {"path", p}, {"count", SDFileStream::readPlaylist(p).size()}});
  }
  return out;
}

nlohmann::json playlist(const std::string& path) {
  nlohmann::json out;
  out["type"] = "pl";
  out["path"] = path;
  out["entries"] = nlohmann::json::array();
  for (auto& p : SDFileStream::readPlaylist(path))
    out["entries"].push_back({{"path", p},
                              {"title", sdfile::titleFromFileName(p)},
                              {"exists", exists(p)}});
  return out;
}

std::vector<std::string> pathList(const nlohmann::json& a) {
  std::vector<std::string> v;
  if (!a.is_array())
    return v;
  for (auto& x : a) {
    std::string p = x.is_string() ? x.get<std::string>()
                    : x.is_object() ? x.value("path", "")
                                    : "";
    if (isSafePath(p))
      v.push_back(p);
  }
  return v;
}

// ---- HTTP helpers --------------------------------------------------------------
// NOTE: never use mg_printf() here: it formats into an 8 KB buffer on the
// stack (MG_BUF_LEN) and the civetweb worker stacks are only 12 KB — deep in
// a request handler (after FATFS / SDMMC calls) that overflows the stack.
void writeHead(mg_connection* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void writeHead(mg_connection* c, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char* out = nullptr;
  int n = vasprintf(&out, fmt, ap);  // heap
  va_end(ap);
  if (n > 0 && out)
    mg_write(c, out, (size_t)n);
  free(out);
}

void sendJson(mg_connection* c, int status, const nlohmann::json& j) {
  std::string body = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  writeHead(c,
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %u\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            status, status == 200 ? "OK" : "Error", (unsigned)body.size());
  mg_write(c, body.data(), body.size());
}

std::string queryVar(mg_connection* c, const char* name) {
  const mg_request_info* ri = mg_get_request_info(c);
  if (!ri->query_string)
    return "";
  std::vector<char> buf(1024);
  int n = mg_get_var(ri->query_string, strlen(ri->query_string), name, buf.data(),
                     buf.size());
  return n > 0 ? std::string(buf.data(), (size_t)n) : "";
}

const char* mimeOf(const std::string& name) {
  std::string e = lowerExt(name);
  if (e == "mp3") return "audio/mpeg";
  if (e == "flac") return "audio/flac";
  if (e == "wav") return "audio/wav";
  if (e == "ogg" || e == "oga") return "audio/ogg";
  if (e == "m4a" || e == "mp4") return "audio/mp4";
  if (e == "aac") return "audio/aac";
  if (e == "jpg" || e == "jpeg") return "image/jpeg";
  if (e == "png") return "image/png";
  if (e == "json") return "application/json";
  if (e == "html" || e == "htm") return "text/html; charset=utf-8";
  if (isTextName(name)) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::string urlEncode(const std::string& s) {
  static const char* H = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      o += (char)c;
    else {
      o += '%';
      o += H[c >> 4];
      o += H[c & 15];
    }
  }
  return o;
}

bool httpUpload(mg_connection* c) {
  const std::string path = queryVar(c, "path");
  const bool overwrite = queryVar(c, "overwrite") == "1";
  if (!isSafePath(path) || path == SD_Master::kMountPoint) {
    sendJson(c, 400, result(false, "upload", "invalid path"));
    return true;
  }
  if (!mounted()) {
    sendJson(c, 503, result(false, "upload", "no SD card"));
    return true;
  }
  if (!isDir(parentOf(path))) {
    sendJson(c, 404, result(false, "upload", "folder does not exist"));
    return true;
  }
  if (exists(path) && !overwrite) {
    sendJson(c, 409, result(false, "upload", "file exists"));
    return true;
  }
  const mg_request_info* ri = mg_get_request_info(c);
  if (ri->content_length < 0) {
    sendJson(c, 411, result(false, "upload", "Content-Length required"));
    return true;
  }
  const std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) {
    sendJson(c, 500, result(false, "upload", errText("cannot create")));
    return true;
  }
  std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
  int64_t left = ri->content_length;
  bool ok = buf != nullptr;
  while (ok && left > 0) {
    int n = mg_read(c, buf.get(), (size_t)std::min<int64_t>(left, kChunk));
    if (n <= 0) {
      ok = false;
      break;
    }
    ok = fwrite(buf.get(), 1, (size_t)n, f) == (size_t)n;
    left -= n;
  }
  ok = (fclose(f) == 0) && ok;
  if (ok && overwrite) {
    beforeChange(path);
    unlink(path.c_str());
  }
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    sendJson(c, 500, result(false, "upload", left > 0 ? "upload interrupted" : "write failed"));
    return true;
  }
  SC32_LOG(info, "SD: uploaded %s (%lld bytes)", path.c_str(),
           (long long)ri->content_length);
  sendJson(c, 200, result(true, "upload", path));
  return true;
}

bool httpDownload(mg_connection* c) {
  const std::string path = queryVar(c, "path");
  const bool inl = queryVar(c, "inline") == "1";
  struct stat st;
  if (!isSafePath(path) || !mounted() || stat(path.c_str(), &st) != 0 ||
      S_ISDIR(st.st_mode)) {
    sendJson(c, 404, result(false, "download", "not found"));
    return true;
  }
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    sendJson(c, 500, result(false, "download", errText("cannot open")));
    return true;
  }
  const std::string name = baseOf(path);
  writeHead(c,
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
            "Content-Disposition: %s; filename*=UTF-8''%s\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
            mimeOf(name), (unsigned long long)st.st_size, inl ? "inline" : "attachment",
            urlEncode(name).c_str());
  std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
  size_t n;
  while (buf && (n = fread(buf.get(), 1, kChunk, f)) > 0)
    if (mg_write(c, buf.get(), n) <= 0)
      break;  // client went away
  fclose(f);
  return true;
}

}  // namespace

// ============================================================================
bool isSafePath(const std::string& p) {
  const std::string root = SD_Master::kMountPoint;
  if (p.compare(0, root.size(), root) != 0)
    return false;
  if (p.size() > root.size() && p[root.size()] != '/')
    return false;
  if (p.find("/..") != std::string::npos || p.find("//") != std::string::npos)
    return false;
  for (unsigned char c : p)
    if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      return false;
  return true;
}

void setSdMaster(SD_Master* sd) {
  g_sd = sd;
}

void setBeforeChangeHook(std::function<void(const std::string&)> fn) {
  g_beforeChange = std::move(fn);
}

nlohmann::json handle(const nlohmann::json& m) {
  const std::string type = m.value("type", "");
  const std::string path = m.value("path", "");
  if (type == "pl.list")
    return playlists();
  if (!mounted())
    return result(false, type, "no SD card");

  if (type == "fs.list") {
    const std::string p = path.empty() ? SD_Master::kMountPoint : path;
    if (!isSafePath(p))
      return result(false, type, "invalid path");
    return list(p);
  }
  // fs.* need a full path; pl.* also accept a plain playlist name
  if (type.rfind("fs.", 0) == 0 && type != "fs.move" && !isSafePath(path))
    return result(false, type, "invalid path");

  if (type == "fs.mkdir") {
    if (exists(path))
      return result(false, type, "already exists");
    return mkdir(path.c_str(), 0775) == 0 ? result(true, type, path)
                                          : result(false, type, errText("mkdir"));
  }
  if (type == "fs.delete") {
    if (path == SD_Master::kMountPoint)
      return result(false, type, "cannot delete the card");
    beforeChange(path);
    return removeTree(path) ? result(true, type, path)
                            : result(false, type, errText("delete"));
  }
  if (type == "fs.move") {
    const std::string from = m.value("from", ""), to = m.value("to", "");
    if (!isSafePath(from) || !isSafePath(to) || from == SD_Master::kMountPoint)
      return result(false, type, "invalid path");
    if (to.compare(0, from.size() + 1, from + "/") == 0)
      return result(false, type, "cannot move a folder into itself");
    if (exists(to))
      return result(false, type, "target exists");
    if (!isDir(parentOf(to)))
      return result(false, type, "target folder does not exist");
    beforeChange(from);
    return rename(from.c_str(), to.c_str()) == 0 ? result(true, type, to)
                                                 : result(false, type, errText("move"));
  }
  if (type == "fs.read")
    return readText(path);
  if (type == "fs.write") {
    if (!m.contains("content") || !m["content"].is_string())
      return result(false, type, "no content");
    const std::string& c = m["content"].get_ref<const std::string&>();
    if (c.size() > kMaxTextFile)
      return result(false, type, "too large (max 64 KB)");
    beforeChange(path);
    return writeFileAtomic(path, c) ? result(true, type, path)
                                    : result(false, type, errText("write"));
  }
  if (type == "pl.get") {
    const std::string pl = playlistPath(path);
    if (!isSafePath(pl))
      return result(false, type, "invalid playlist name");
    return playlist(pl);
  }
  if (type == "pl.save" || type == "pl.add") {
    const std::string pl = playlistPath(path);
    if (!isSafePath(pl))
      return result(false, type, "invalid playlist name");
    if (!isDir(parentOf(pl)) && mkdir(parentOf(pl).c_str(), 0775) != 0)
      return result(false, type, errText("mkdir"));
    std::vector<std::string> entries;
    if (type == "pl.add")
      entries = SDFileStream::readPlaylist(pl);
    for (auto& p : pathList(m.contains("entries") ? m["entries"] : m.value("files", nlohmann::json())))
      entries.push_back(p);
    return SDFileStream::writePlaylist(pl, entries) ? result(true, type, pl)
                                                    : result(false, type, errText("save"));
  }
  return result(false, type, "unknown command");
}

void registerHttp(bell::BellHTTPServer& server) {
  server.registerPostStream("/api/fs/upload", httpUpload);
  server.registerGetStream("/api/fs/download", httpDownload);
}

}  // namespace sdfiles
