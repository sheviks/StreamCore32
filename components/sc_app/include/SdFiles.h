#pragma once
// ============================================================================
//  SdFiles — file manager for the SD card (used by the web UI).
//
//  WebSocket (JSON) operations, all paths absolute and below /sdcard:
//    fs.list   {path}                 -> {"type":"fs","path",entries:[..]}
//    fs.mkdir  {path}
//    fs.move   {from, to}             rename or move (file or folder)
//    fs.delete {path}                 file, or folder with its content
//    fs.read   {path}                 text files up to 64 KB
//    fs.write  {path, content}        create / overwrite a text file
//    pl.list                          playlists in /sdcard/Playlists
//    pl.get    {path}                 entries of an M3U playlist
//    pl.save   {path, entries:[abs]}  write an M3U playlist (relative paths)
//    pl.add    {path, files:[abs]}    append (creates the playlist)
//
//  HTTP (large files, streamed, no global server lock):
//    POST /api/fs/upload?path=/sdcard/dir/name.ext[&overwrite=1]  raw body
//    GET  /api/fs/download?path=/sdcard/dir/name.ext[&inline=1]
// ============================================================================
#include <functional>
#include <string>

#include "nlohmann/json.hpp"

class SD_Master;
namespace bell {
class BellHTTPServer;
}

namespace sdfiles {

constexpr const char* kPlaylistDir = "/sdcard/Playlists";

/** Path below the mount point, no ".." or control characters. */
bool isSafePath(const std::string& path);

/** Handle a "fs.*" / "pl.*" message; returns the reply (type "fs.result",
 *  "fs", "fs.file", "pl.list" or "pl"). */
nlohmann::json handle(const nlohmann::json& msg);

/** The card (needed for the mounted state). */
void setSdMaster(SD_Master* sd);

/** Called before a path is deleted / moved (stop the SD player if needed). */
void setBeforeChangeHook(std::function<void(const std::string& path)> fn);

/** Register the upload / download endpoints. */
void registerHttp(bell::BellHTTPServer& server);

}  // namespace sdfiles
