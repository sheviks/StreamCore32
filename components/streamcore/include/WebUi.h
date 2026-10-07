// WebUI.cpp
#include "AudioControl.h"
#include "BellHTTPServer.h"

#include <BellLogger.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include <civetweb.h>  // for mg_websocket_write, MG_WEBSOCKET_OPCODE_TEXT
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace WebUI {

using OnWsMessage = std::function<void(struct mg_connection*, char*, size_t)>;
// Adjust these to wherever you store the files (SPIFFS, FATFS, etc.)
static const char* TEXT_INDEX = R"delim(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8" /><title>StreamCore32</title><meta name="viewport" content="width=device-width, initial-scale=1" /><link rel="stylesheet" href="style.css" /><link href="https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@48,700,1,150" rel="stylesheet" /><style>.player-controls > * { font-size: 3rem }.player-volume > * { font-size: 10px }</style></head><body><header class="topbar"><div class="topbar-left"><span class="brand">StreamCore32</span></div><button class="nav-current" id="nav-current" aria-haspopup="true" aria-expanded="false">Player</button><nav class="topbar-nav" id="topbar-nav"><button class="nav-link active" data-page="player">Player</button><button class="nav-link" data-page="radio" data-feature="radio">Radio</button><button class="nav-link" data-page="sd" data-feature="sd">Files</button><button class="nav-link" data-page="settings">Settings</button><button class="nav-link" data-page="debug">Debug</button></nav></header><main class="page-wrap"><section id="page-player" class="page active"><div class="card"><h2 class="card-title">Now Playing</h2><div class="player-layout"><div class="player-cover"><div class="cover-placeholder">STREAMCORE32</div></div><div class="player-meta"><div class="track-title">Track Title</div><div class="track-artist">Artist Name</div><div class="track-album">Album Name</div><div class="player-progress"><span>01:23</span><input type="range" min="0" max="100" value="30" /><span>04:56</span></div><div class="player-controls"><button class="material-symbols-outlined btn mode" title="Shuffle">shuffle</button><button class="material-symbols-outlined btn">skip_previous</button><button class="material-symbols-outlined btn play">pause</button><button class="material-symbols-outlined btn"> skip_next</button><button class="material-symbols-outlined btn mode" title="Repeat">repeat</button><div class="player-volume"><span class="volume-icon">VOL</span><input type="range" min="0" max="100" value="70" /><span class="volume-label">70%</span></div></div></div></div><div class="player-footer"><span>Source: <strong class = "stream-src">Qobuz</strong></span><span>Quality: <strong class = "stream-qlty">24-bit / 96 kHz</strong></span></div><div class="queue-box empty" id="queue-box"><h3 class="queue-head" id="queue-head"><span>Up next</span><span class="queue-peek" id="queue-peek"></span><span class="queue-arrow">&#9652;</span></h3><div class="queue-list"><table class="radio-table"><tbody id="queue-body"><tr><td>-</td></tr></tbody></table></div></div></div></section><section id="page-sd" class="page"><div class="card"><h2 class="card-title">Files <span class="hint" id="fs-space"></span></h2><div class="fs-bar"><button class="btn small" data-action="fs-up" title="Up">&#8593;</button><div id="fs-crumbs" class="fs-crumbs"></div><button class="btn small" data-action="fs-refresh" title="Refresh">&#8635;</button></div><div class="fs-tools"><button class="btn small" data-action="fs-mkdir">New folder</button><button class="btn small" data-action="fs-newfile">New text file</button><label class="btn small primary fs-upload">Upload<input type="file" id="fs-upload" multiple hidden /></label><button class="btn small" data-action="fs-play-folder">Play folder</button><span class="fs-sel-tools" id="fs-sel-tools" hidden><span id="fs-sel-count"></span><button class="btn small" data-action="fs-sel-playlist">Add to playlist</button><button class="btn small" data-action="fs-sel-move">Move</button><button class="btn small danger" data-action="fs-sel-delete">Delete</button></span></div><div id="fs-progress" class="fs-progress" hidden><div class="fs-progress-bar"></div><span></span></div><div id="fs-msg" class="fs-msg" hidden></div><div id="fs-drop" class="fs-drop"><table class="radio-table fs-table"><thead><tr><th class="fs-c"><input type="checkbox" id="fs-all" /></th><th class="fs-c"></th><th>Name</th><th class="fs-size">Size</th><th class="fs-act">Actions</th></tr></thead><tbody id="sd-table-body"><tr><td colspan="5">-</td></tr></tbody></table><div class="hint fs-drop-hint">Drop files here to upload them into this folder</div></div></div><div class="card"><h2 class="card-title">Playlists</h2><div class="fs-tools"><button class="btn small primary" data-action="pl-new">New playlist</button><span class="hint">stored in /sdcard/Playlists (M3U, works on a PC as well)</span></div><table class="radio-table"><tbody id="pl-body"><tr><td>-</td></tr></tbody></table><div id="pl-editor" class="pl-editor" hidden><h3>Playlist <input type="text" id="pl-name" maxlength="64" /></h3><table class="radio-table"><tbody id="pl-entries"></tbody></table><div class="hint">Add files with the &#43; button in the file list above.</div><div class="form-actions"><button class="btn small primary" data-action="pl-save">Save</button><button class="btn small" data-action="pl-play">Play</button><button class="btn small" data-action="pl-close">Close</button></div></div></div><div id="fs-editor" class="modal" hidden><div class="modal-box card"><h2 class="card-title" id="fs-editor-title">Edit</h2><textarea id="fs-editor-text" spellcheck="false"></textarea><div class="form-actions"><span class="hint" id="fs-editor-info"></span><button class="btn small primary" data-action="fs-editor-save">Save</button><button class="btn small" data-action="fs-editor-close">Close</button></div></div></div></section><section id="page-radio" class="page"><div class="card"><h2 class="card-title">Radio</h2><div class="radio-filters"><input type="text" placeholder="Search stations…" /><input type="text" placeholder="Taglist <…,…,…>" /><select><option>All countries</option></select><button class="btn small" data-action="radio-search">Search</button></div><table class="radio-table"><thead><tr><th>★</th><th>Name</th><th>Info</th><th class="radio-actions-col">Actions</th></tr></thead><tbody id="radio-table-body"></tbody></table><div class="radio-add"><h3>Add Station</h3><div class="form-grid"><label> Name <input type="text" placeholder="My Station" /></label><label> URL <input type="text" placeholder="http://stream.example.com" /></label></div><div class="form-actions"><button class="btn primary">Play</button><button class="btn primary">Save</button></div></div></section><section id="page-settings" class="page"><div class="banner" id="restart-banner" hidden>Some changes are applied after a restart. <button class="btn small primary" data-action="restart">Restart now</button></div><div class="card"><h2 class="card-title">Device</h2><div class="set-grid"><label>Device name <span class="hint" id="set-hostname"></span><span class="row"><input type="text" id="set-name" maxlength="48" /><button class="btn small primary" data-action="save-name">Save</button></span></label><label class="chk" data-feature="display"><input type="checkbox" data-set="dark_mode" data-kind="bool" /> Display dark mode</label><span class="row"><button class="btn small" data-action="refresh-display" data-feature="display">Refresh display</button><button class="btn small danger" data-action="restart">Restart device</button></span></div></div><div class="card" data-feature="led"><h2 class="card-title">Status LED</h2><div class="set-grid"><label>Mode <select data-set="led_mode" data-kind="num" data-options="led_modes"></select></label><label>Brightness <span class="val" data-val="led_brightness" data-unit=" %"></span><input type="range" min="1" max="100" step="1" data-set="led_brightness" data-kind="num" /></label></div><table class="radio-table led-table"><thead><tr><th>State</th><th>Color</th><th>Effect</th><th class="fs-act"></th></tr></thead><tbody id="led-body"></tbody></table><div class="hint">Black (#000000) switches the LED off in that state. Mode &quot;Off&quot;: the LED is not written at all. &quot;WiFi status only&quot; shows &quot;WiFi connected&quot; while a source plays.</div></div><div class="card"><h2 class="card-title">WiFi</h2><div class="set-grid"><div class="hint" id="set-wifi-status">-</div><label>Network (SSID) <input type="text" id="set-ssid" /></label><label>Password <input type="password" id="set-pass" placeholder="unchanged" /></label><span class="row"><button class="btn small primary" data-action="wifi-connect">Save &amp; connect</button></span></div></div><div class="card"><h2 class="card-title">Audio</h2><div class="set-grid"><label>Volume <span class="val" data-val="volume"></span><input type="range" min="0" max="100" step="1" data-set="volume" data-kind="num" /></label><label class="chk"><input type="checkbox" data-set="restore_volume" data-kind="bool" /> Keep the volume after a restart</label><label class="chk"><input type="checkbox" data-set="spectrum" data-kind="bool" /> Spectrum broadcast (UDP 6969) <span class="hint">reads the decoder 20x/s, can cause dropouts at high bitrates</span></label><label>Bass boost <span class="val" data-val="bass_db" data-unit=" dB"></span><input type="range" min="0" max="15" step="1" data-set="bass_db" data-kind="num" /></label><label>Bass below <span class="val" data-val="bass_hz" data-unit=" Hz"></span><input type="range" min="20" max="150" step="10" data-set="bass_hz" data-kind="num" /></label><label>Treble <span class="val" data-val="treble_db" data-unit=" dB"></span><input type="range" min="-12" max="10.5" step="1.5" data-set="treble_db" data-kind="num" /></label><label>Treble above <span class="val" data-val="treble_khz" data-unit=" kHz"></span><input type="range" min="1" max="15" step="1" data-set="treble_khz" data-kind="num" /></label></div></div><div class="card"><h2 class="card-title">Streaming</h2><div class="set-grid"><label class="chk" data-feature="spotify"><input type="checkbox" data-set="spotify_enabled" data-kind="bool" /> Spotify Connect <span class="hint">(restart)</span></label><label data-feature="spotify">Spotify quality <select data-set="spotify_format" data-kind="num" data-options="spotify_formats"></select></label><label class="chk" data-feature="qobuz"><input type="checkbox" data-set="qobuz_enabled" data-kind="bool" /> Qobuz Connect <span class="hint">(restart)</span></label><label data-feature="qobuz">Qobuz quality <select data-set="qobuz_format" data-kind="num" data-options="qobuz_formats"></select></label><label class="chk" data-feature="dlna"><input type="checkbox" data-set="dlna_enabled" data-kind="bool" /> DLNA / UPnP renderer <span class="hint">(restart)</span></label><label data-feature="radio">Radio: reconnect after a pause longer than (s, 0 = never) <input type="number" min="0" max="3600" data-set="radio_resume_s" data-kind="num" /></label></div></div><div class="card"><h2 class="card-title">System</h2><div class="info-list" id="set-info"></div></div></section><section id="page-debug" class="page"><div class="card"><h2 class="card-title">Debug</h2><div class="info-list"><div class="info-row"><span>Wi-Fi signal</span><span>-58 dBm</span></div><div class="info-row"><span>Heap memory</span><span>172 KB</span></div><div class="info-row"><span>Threads</span></div></div><h3>Recent Log</h3><pre class="log-box"></pre></div></section></main><script src="app.js"></script></body></html>
)delim";
static const char* TEXT_CSS = R"delim(
:root{--bg:#e8e8e8;--card-bg:#fafafa;--border:#bcbcbc;--border-soft:#d4d4d4;--text:#2e7da4;--muted:#2e7da4;--accent:#2e7da4;--danger:#c64a4a;--font-mono:"SF Mono", ui-monospace, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;--input-bg:#f2f2f2;--input-border:#c8c8c8;--range-track:#c6c6c6;--range-thumb:#2e7da4}@media (prefers-color-scheme:dark){:root{--bg:#000000;--card-bg:#000000;--border:#1f2933;--border-soft:#374151;--text:#7dd3fc;--muted:#7dd3fc;--accent:#7dd3fc;--danger:#f97373;--input-bg:#020617;--input-border:#1f2933;--range-track:#111827;--range-thumb:#7dd3fc}}*,*::before,*::after{box-sizing:border-box}html,body{margin:0;padding:0;height:100%}body{font-family:var(--font-mono);font-size:13px;background:radial-gradient(circle at top,var(--bg) 0,var(--card-bg) 55%);color:var(--text)}.topbar{display:flex;align-items:baseline;justify-content:space-between;padding:8px 16px;background:var(--card-bg);color:var(--text);border-bottom:1px solid var(--border)}.brand{font-weight:600;letter-spacing:.15em;text-transform:uppercase;font-size:11px}.topbar-nav{display:flex;gap:16px}.nav-link{padding:0;margin:0;border:none;background:#fff0;color:var(--muted);font-family:inherit;font-size:12px;letter-spacing:.08em;text-transform:uppercase;cursor:pointer;position:relative}.nav-link::after{content:"";position:absolute;left:0;bottom:-4px;width:0;height:1px;background:var(--accent);transition:width 0.12s ease-out}.nav-link:hover{color:var(--text)}.nav-link.active{color:var(--accent)}.nav-link.active::after{width:100%}.page-wrap{max-width:960px;margin:16px auto 24px;padding:0 16px}.page{display:none}.page.active{display:block}.card{background:var(--card-bg);border-radius:0;border:1px solid var(--border);padding:16px}.card-title{margin:0 0 12px;font-size:14px;text-transform:uppercase;letter-spacing:.12em;color:var(--accent)}.player-layout{display:grid;grid-template-columns:minmax(400px,400px) minmax(0,1fr);gap:16px}.player-cover{width:100%;display:flex;justify-content:center}.cover-placeholder{width:100%;height:100%;padding:50%;border:1px solid var(--border);background:var(--border);display:flex;align-items:center;justify-content:center;font-size:2em;color:var(--muted)}.player-meta{min-width:0}.track-title{font-size:16px;text-transform:uppercase;letter-spacing:.08em}.track-artist{margin-top:2px;color:var(--muted)}.track-album{margin-top:2px;font-size:11px;color:var(--muted)}.player-progress{display:flex;justify-content:space-between;align-items:center;margin-top:12px;font-size:10px;color:var(--muted)}.player-progress input[type="range"]{width:100%}.player-controls{display:flex;align-items:center;gap:12px;margin-top:12px;font-size:35px;text-transform:uppercase}.player-controls .btn{padding:0;font-weight:700;flex: auto;}.player-volume{font-size:10px;display:flex;align-items:center;gap:6px;width:100%}.player-volume input[type="range"]{flex:1}.player-footer{margin-top:14px;padding-top:8px;border-top:1px solid var(--border-soft);display:flex;flex-wrap:wrap;gap:12px;font-size:11px;color:var(--muted)}.radio-filters{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}.radio-table{width:100%;border-collapse:collapse;margin-bottom:12px;font-size:12px;text-align:left}.radio-table th,.radio-table td{padding:4px 4px;border-bottom:1px solid var(--border-soft);width:0%}.radio-table th{font-weight:500;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}.radio-table tbody tr:hover{background:rgb(100 100 100 / .08)}.radio-actions-col,.radio-actions{text-align:right;gap:8px;}.radio-add{margin-top:12px}.radio-add h3{margin:0 0 6px;font-size:12px;text-transform:uppercase;letter-spacing:.08em}.info-row table{border-collapse: collapse;text-align: right;width:100%;text-transform: uppercase;max-height:30vh;overflow-y:auto;display:block;scrollbar-width: none;-ms-overflow-style: none;}.info-row .td-or{text-align:left}.info-row th{position:sticky;top:0px;background:var(--bg);}.info-row th,td{padding-left:10px;width:10%;border-bottom: 1px solid var(--border-soft);}.tabs{display:inline-flex;gap:10px;margin-bottom:10px;border-bottom:1px solid var(--border)}.tab-link{border:none;background:#fff0;padding:0 0 4px;font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);cursor:pointer;position:relative}.tab-link::after{content:"";position:absolute;left:0;bottom:-1px;width:0;height:1px;background:var(--accent);transition:width 0.12s}.tab-link:hover{color:var(--text)}.tab-link.active{color:var(--accent)}.tab-link.active::after{width:100%}.tab-panel{display:none;margin-top:8px;margin-bottom:10px}.tab-panel.active{display:block}.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px 12px;margin-bottom:8px}label{font-size:11px;color:var(--muted);display:flex;flex-direction:column;gap:3px}input[type="text"],input[type="email"],input[type="password"],input[type="number"],select{padding:3px 5px;border-radius:0;border:1px solid var(--input-border);font-size:12px;background:var(--input-bg);color:var(--text)}input:focus,select:focus{outline:1px solid var(--accent);border-color:var(--accent)}.file-input{display:inline-flex;align-items:center;gap:8px;padding:4px 8px;border-radius:0;border:1px dashed var(--border-soft);font-size:11px;color:var(--muted);cursor:pointer}.file-input input[type="file"]{display:none}.form-actions{display:flex;gap:10px;justify-content:flex-end;font-size:11px}.btn{border:none;background:#fff0;padding:0;font-family:inherit;font-size:inherit;color:var(--muted);cursor:pointer}.btn.primary{color:var(--muted)}.btn.ghost{color:var(--muted)}.btn.danger{color:var(--danger)}.btn.small{font-size:11px}.icon-btn{width:auto;height:auto}.info-list{border:1px solid var(--border-soft);background:var(--card-bg);margin-bottom:10px}.info-row{display:flex;justify-content:space-between;padding:4px 6px;font-size:11px}.info-row:nth-child(odd){background:rgb(0 0 0 / .03)}.info-row span:first-child{color:var(--muted)}.log-box{margin:0;padding:6px;border:1px solid var(--border-soft);background:#000;color:#cbd5f5;font-family:var(--font-mono);font-size:11px;max-height:200px;overflow:auto}input[type="range"]{-webkit-appearance:none;appearance:none;width:100%;height:3px;border-radius:999px;background:var(--range-track)}input[type="range"]::-webkit-slider-thumb{-webkit-appearance:none;width:1px;height:3px;border-radius:50%;background:var(--range-thumb);cursor:pointer;appearance:none}input[type="range"]::-moz-range-thumb{width:3px;height:3px;border-radius:50%;background:var(--range-thumb);border:none;cursor:pointer;display:none}@media (max-width:720px){.player-layout{grid-template-columns:1fr}.player-footer{flex-direction:column;align-items:flex-start}.topbar-nav{gap:10px}}
.btn.mode{font-size:80%;opacity:.35}.btn.mode.on{opacity:1}.btn:disabled{opacity:.15;cursor:default}.queue-box{margin-top:14px}.queue-box h3{margin:0 0 6px;font-size:12px;text-transform:uppercase;letter-spacing:.08em}.queue-box tr,#sd-table-body tr{cursor:pointer}.sd-bar{display:flex;gap:12px;align-items:center;margin-bottom:10px;font-size:12px}#sd-path{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#page-settings .card{margin-bottom:14px}.set-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px 18px;align-items:end}.set-grid .row{display:flex;gap:10px;align-items:center}.set-grid .row input{flex:1}.chk{flex-direction:row;align-items:center;gap:6px}.hint{font-size:10px;color:var(--muted);opacity:.8}.val{float:right}.banner{border:1px solid var(--accent);padding:8px 10px;margin-bottom:12px;display:flex;justify-content:space-between;align-items:center}.banner[hidden]{display:none}
.fs-bar{display:flex;gap:8px;align-items:center;margin-bottom:8px;font-size:12px}.fs-crumbs{flex:1;display:flex;flex-wrap:wrap;gap:2px;align-items:center;overflow:hidden}.fs-crumbs a{cursor:pointer;text-decoration:underline;padding:2px 3px}.fs-crumbs .sep{opacity:.5}.fs-tools{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}.fs-sel-tools{display:inline-flex;gap:8px;align-items:center;padding-left:10px;border-left:1px solid var(--border)}.fs-sel-tools[hidden],.fs-progress[hidden],.fs-msg[hidden],.pl-editor[hidden],.modal[hidden]{display:none}.fs-upload{cursor:pointer}.fs-progress{position:relative;height:18px;border:1px solid var(--border);margin-bottom:8px;font-size:11px}.fs-progress-bar{position:absolute;left:0;top:0;bottom:0;width:0;background:var(--accent);opacity:.35}.fs-progress span{position:relative;padding-left:6px;line-height:18px}.fs-msg{border:1px solid var(--danger);color:var(--danger);padding:6px 8px;margin-bottom:8px;font-size:12px}.fs-msg.ok{border-color:var(--accent);color:var(--accent)}.fs-drop{border:1px dashed transparent}.fs-drop.over{border-color:var(--accent)}.fs-drop-hint{text-align:center;padding:6px}.fs-table td{vertical-align:middle}.fs-table{table-layout:auto;width:100%}.fs-table .fs-c{width:1%;white-space:nowrap;padding-right:6px}.fs-table td.fs-name,.fs-table th:nth-child(3){width:100%}.fs-table td,.fs-table th{width:auto}#fs-sel-count{font-size:12px}.fs-size{width:80px;text-align:right}.fs-table td.fs-size{text-align:right;white-space:nowrap}.fs-act{text-align:right}.fs-table td.fs-act{text-align:right;white-space:nowrap}.fs-act .btn{padding:2px 7px;margin-left:3px;min-width:0}.fs-name{cursor:pointer;word-break:break-all}.fs-name.dim{opacity:.6;cursor:default}.pl-editor{margin-top:12px;border-top:1px solid var(--border);padding-top:10px}.pl-editor h3{display:flex;gap:8px;align-items:center;font-size:13px}.pl-editor h3 input{flex:1}.pl-missing{opacity:.5;text-decoration:line-through}.modal{position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:50;padding:16px}.modal-box{width:min(900px,100%);max-height:calc(100vh - 32px);display:flex;flex-direction:column}.modal-box textarea{flex:1;min-height:50vh;width:100%;font-family:var(--font-mono);font-size:12px;background:var(--input-bg);color:var(--text);border:1px solid var(--input-border);padding:8px;resize:vertical}.modal-box .form-actions{display:flex;gap:8px;align-items:center;justify-content:flex-end;margin-top:8px}.modal-box .form-actions .hint{flex:1}
.led-table td{vertical-align:middle}.led-color{width:44px;height:24px;padding:0;border:1px solid var(--input-border);background:none;cursor:pointer}
[data-feature][hidden]{display:none !important}
/* ---- player controls: the volume moves to its own line when there is no room */
.player-controls{flex-wrap:wrap;row-gap:4px}
.player-volume{width:auto;flex:1 1 140px;min-width:140px}
/* ---- navigation / queue on small screens (phones) */
.nav-current{display:none}
.queue-head .queue-peek,.queue-head .queue-arrow{display:none}
@media (max-width:720px){
body{font-size:14px}
.topbar{position:sticky;top:0;z-index:40;align-items:center;padding:6px 12px}
.nav-current{display:block;border:none;background:none;color:var(--accent);font-family:inherit;font-size:14px;letter-spacing:.1em;text-transform:uppercase;padding:10px 0 10px 16px;cursor:pointer;text-decoration:underline;text-underline-offset:5px}
.topbar-nav{display:none;position:absolute;right:8px;top:calc(100% - 2px);flex-direction:column;gap:0;min-width:200px;background:var(--card-bg);border:1px solid var(--border);box-shadow:0 8px 24px rgb(0 0 0 / .18);z-index:41}
.topbar.nav-open .topbar-nav{display:flex}
.topbar-nav .nav-link{padding:14px 18px;text-align:left;font-size:14px;border-bottom:1px solid var(--border-soft)}
.topbar-nav .nav-link:last-child{border-bottom:none}
.topbar-nav .nav-link.active{display:none}
.nav-link::after{display:none}
.page-wrap{margin-top:10px;padding:0 8px}
.card{padding:12px}
.player-cover{max-width:min(300px,38vh);margin:0 auto}
.cover-placeholder{font-size:1.4em}
.track-title{font-size:17px}
.player-progress{font-size:12px;gap:8px}
.player-volume{padding-top:8px}
input[type="range"]{height:6px}
#page-player{padding-bottom:64px}
.queue-box{position:fixed;left:0;right:0;bottom:0;margin:0;z-index:30;background:var(--card-bg);border-top:1px solid var(--border);box-shadow:0 -8px 24px rgb(0 0 0 / .14)}
.queue-box.empty{display:none}
.queue-head{display:flex;gap:10px;align-items:center;margin:0 !important;padding:14px 16px;cursor:pointer;user-select:none}
.queue-head .queue-peek{display:block;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-transform:none;letter-spacing:0;font-weight:400;opacity:.85}
.queue-head .queue-arrow{display:block;transition:transform .15s}
.queue-list{display:none;max-height:60vh;overflow-y:auto;border-top:1px solid var(--border-soft);padding:0 8px 8px}
.queue-box.open .queue-list{display:block}
.queue-box.open .queue-arrow{transform:rotate(180deg)}
.queue-list td{padding:10px 6px}
.set-grid{grid-template-columns:1fr}
.chk{flex-wrap:wrap}
.fs-act .btn{padding:6px 8px}
.radio-filters input,.radio-filters select{flex:1 1 140px}
input[type="text"],input[type="password"],input[type="number"],select{font-size:16px;padding:6px 8px}
}
)delim";
static const char* TEXT_JS = R"delim(
(function(){const radioState={items:[],query:"",genre:"",country:""};const RADIO_SERVERS=["https://de1.api.radio-browser.info","https://de2.api.radio-browser.info","https://fi1.api.radio-browser.info"];const radioApiBase=RADIO_SERVERS[Math.floor(Math.random()*RADIO_SERVERS.length)];function renderRadioStations(){const tbody=document.getElementById("radio-table-body");if(!tbody)
return;if(!radioState.items||radioState.items.length===0){tbody.innerHTML=`<tr><td colspan="4">No stations found. Try another search.</td></tr>`;return}
tbody.innerHTML=radioState.items.map((st)=>{const infoParts=[];if(st.genre)
infoParts.push(st.genre);if(st.country)
infoParts.push(st.country);const info=infoParts.join(" · ")||"";const fav=st.favorite?"★":"☆";return ` <tr data-url="${st.url || ""}" data-name="${st.name || ""}"> <td> <button class="btn small star-btn" data-action="radio-favorite" data-fav="${st.favorite ? "1" : "0"}"> ${fav}</button></td> <td>${st.name || ""}</td> <td>${info}</td> <td class="radio-actions"> <button class="btn small" data-action="radio-play">Play</button> <button class="btn small ghost" data-action="radio-save">Save</button> </td> </tr>`}).join("")}
const rootStyle=getComputedStyle(document.documentElement);const rangeTrackColor=(rootStyle.getPropertyValue("--range-track")||"#c6c6c6").trim();const rangeThumbColor=(rootStyle.getPropertyValue("--range-thumb")||"#2e7da4").trim();function styleRange(input){const min=Number(input.min)||0;const max=Number(input.max)||100;const val=Number(input.value);const span=max-min||1;const pct=((val-min)*100)/span;input.style.background=`linear-gradient(to right, ${rangeThumbColor} 0%, ${rangeThumbColor} ${pct}%, ${rangeTrackColor} ${pct}%, ${rangeTrackColor} 100%)`}
function initAllRanges(){document.querySelectorAll('input[type="range"]').forEach((r)=>{styleRange(r);r.addEventListener("input",()=>styleRange(r))})}
function initNav(){const navLinks=document.querySelectorAll(".nav-link");const pages=document.querySelectorAll(".page");const top=document.querySelector(".topbar");const cur=document.getElementById("nav-current");const setOpen=(o)=>{if(!top)return;top.classList.toggle("nav-open",o);if(cur)cur.setAttribute("aria-expanded",o?"true":"false")};if(cur){const a=document.querySelector(".nav-link.active");if(a)cur.textContent=a.textContent;cur.addEventListener("click",(e)=>{e.stopPropagation();setOpen(!top.classList.contains("nav-open"))})}
document.addEventListener("click",(e)=>{if(top&&!top.contains(e.target))setOpen(!1)});document.addEventListener("keydown",(e)=>{if(e.key==="Escape")setOpen(!1)});navLinks.forEach((btn)=>{btn.addEventListener("click",()=>{const pageId="page-"+btn.dataset.page;wsSend({type:"page",page:pageId});navLinks.forEach((b)=>b.classList.remove("active"));btn.classList.add("active");if(cur)cur.textContent=btn.textContent;setOpen(!1);pages.forEach((p)=>{p.classList.toggle("active",p.id===pageId)})})})
const qh=document.getElementById("queue-head");const qb=document.getElementById("queue-box");if(qh&&qb)qh.addEventListener("click",()=>qb.classList.toggle("open"))}
function initSettingsTabs(){const tabLinks=document.querySelectorAll(".tab-link");const tabPanels=document.querySelectorAll(".tab-panel");tabLinks.forEach((btn)=>{btn.addEventListener("click",()=>{const target="tab-"+btn.dataset.tab;tabLinks.forEach((b)=>b.classList.remove("active"));btn.classList.add("active");tabPanels.forEach((p)=>{p.classList.toggle("active",p.id===target)})})})}
let ws;let reconnectTimer=null;const outbox=[];function wsSend(obj){const payload=JSON.stringify(obj);if(ws&&ws.readyState===WebSocket.OPEN){ws.send(payload)}else{outbox.push(payload)}}
function flushOutbox(){while(ws&&ws.readyState===WebSocket.OPEN&&outbox.length>0){ws.send(outbox.shift())}}
function connectWebSocket(){const proto=location.protocol==="https:"?"wss":"ws";const url=`${proto}://${location.host}/ws`;ws=new WebSocket(url);ws.onopen=()=>{flushOutbox();wsSend({type:"hello",ui:"streamcore32",version:1});wsSend({type:"settings.get"})};ws.onmessage=(ev)=>{try{const msg=JSON.parse(ev.data);handleWsMessage(msg)}catch{const log_box=document.querySelector(".log-box");if(log_box){var text=log_box.textContent;if(text.length>10000){log_box.textContent=text.substring(text.indexOf("\n")+1)}
log_box.textContent+=`${ev.data}\n`;log_box.scrollTop=log_box.scrollHeight}}};ws.onclose=()=>{if(reconnectTimer)
clearTimeout(reconnectTimer);reconnectTimer=setTimeout(connectWebSocket,3000)};ws.onerror=()=>{ws.close()}}
async function wikipediaSearchImageForQuery(query){if(!query)
return null;const params=new URLSearchParams({action:"query",generator:"search",gsrsearch:query,gsrlimit:"3",prop:"pageimages",piprop:"original|thumbnail",pilimit:"3",pithumbsize:"600",format:"json",origin:"*"});const url="https://en.wikipedia.org/w/api.php?"+params.toString();const res=await fetch(url);if(!res.ok){console.warn("Wikipedia search+pageimages failed:",res.status);return null}
const data=await res.json();if(!data.query||!data.query.pages){return null}
const pages=Object.values(data.query.pages);pages.sort((a,b)=>{const ia=typeof a.index==="number"?a.index:9999;const ib=typeof b.index==="number"?b.index:9999;return ia-ib});for(const page of pages){if(page.original&&page.original.source){return page.original.source}
if(page.thumbnail&&page.thumbnail.source){return page.thumbnail.source}}
return null}
async function fetchCoverFromWikipedia(artist,title){try{const queries=[];if(artist&&title){queries.push(`${artist} ${title} album`);queries.push(`${artist} ${title}`)}
if(artist){queries.push(`${artist} album`);queries.push(artist); var seperator="-(/[{,";for(var i=0;i<seperator.length;i++){const seperatorChar=seperator[i];if(artist.indexOf(seperatorChar)>-1){var newArtists=artist.split(seperatorChar);for(const newArtist of newArtists){queries.push(`${newArtist} album`);queries.push(newArtist)}}}}
var seperator="-(/[{,";if(title){queries.push(title);for(var i=0;i<seperator.length;i++){const seperatorChar=seperator[i];if(title.indexOf(seperatorChar)>-1){var newTitle=title.split(seperatorChar)[0];queries.push(newTitle)}}}
for(const q of queries){const img=await wikipediaSearchImageForQuery(q);if(img){return img}}
return null}catch(e){console.warn("Wikipedia cover fetch failed:",e);return null}}
async function fetchCoverFromMusicBrainz(artist,releaseTitle){if(!artist&&!releaseTitle)
return null;const query=[artist?`artist:${artist}`:"",releaseTitle?`recording:${releaseTitle}`:""].filter(Boolean).join(" AND ");if(!query)
return null;const mbSearchUrl="https://musicbrainz.org/ws/2/recording/?"+new URLSearchParams({query,fmt:"json",limit:"1"}).toString();const mbRes=await fetch(mbSearchUrl,{headers:{"User-Agent":"StreamCore32/1.0 (https://example.com)"}});if(!mbRes.ok){console.warn("MusicBrainz search failed:",mbRes.status);return null}
const mbData=await mbRes.json();var recordings=mbData.recordings;var url=null;for(var i=0;i<recordings.length;i++){const recording=recordings[i];if(!recording||!recording.id){console.warn("No release found for query:",query)}else{var mbids=[];for(var j=recording.releases.length-1;j>=0;j--){mbids.push(recording.releases[j].id)}
for(var j=0;j<mbids.length;j++){var caaUrl=`https://coverartarchive.org/release/${mbids[j]}/front-1200`;var caaRes=await fetch(caaUrl,{});if(!caaRes.ok || caaRes.status!==200){console.warn("Cover Art Archive returned:",caaRes.status)}else{return caaRes.url}}}}
if(!url)
url=await fetchCoverFromWikipedia(artist,releaseTitle);return url}
function handleWsMessage(msg){if(!msg||typeof msg!=="object")
return;switch(msg.type){case "playback":updatePlayback(msg);break;case "queue":renderQueue(msg);break;case "fs":case "fs.result":case "fs.file":case "pl.list":case "pl":fsReply(msg);break;case "radio":msg.stations.forEach((item)=>{item.favorite=!0});for(var i=0;i<radioState.items.length;i++){if(radioState.items[i].favorite){radioState.items.splice(i,1)}}
msg.stations.concat(radioState.items);radioState.items=msg.stations;renderRadioStations();break;case "settings":renderSettings(msg);break;case "debug":const log_box=document.getElementById("page-debug");if(log_box){var elem=log_box.querySelectorAll(".info-row");if(msg.heap){elem[1].children[1].textContent=msg.heap+" kB"}
if(msg.rssi){elem[0].children[1].textContent=msg.rssi+" dBm"}
if(msg.tasks){elem[2].removeChild(elem[2].lastChild);var new_table=document.createElement("table");new_table.innerHTML=`<thead><tr><th class="td-or">Thread</th><th>State</th><th>free</th><th>Prio</th></tr></thead>`
var td5=document.createElement("tbody");for(var i=0;i<msg.tasks.length;i++){var tr=document.createElement("tr");var td1=document.createElement("td");td1.classList.add("td-or");var td2=document.createElement("td");td1.textContent=msg.tasks[i].task;td2.textContent=msg.tasks[i].state;var td3=document.createElement("td");td3.textContent=msg.tasks[i].stack;var td4=document.createElement("td");td4.textContent=msg.tasks[i].priority;tr.appendChild(td1);tr.appendChild(td2);tr.appendChild(td3);tr.appendChild(td4);td5.appendChild(tr)}
new_table.appendChild(td5);elem[2].appendChild(new_table)}}
break;default:break}}
function initVolumeControl(){const volSlider=document.querySelector(".player-volume input[type='range']");const volLabel=document.querySelector(".player-volume .volume-label");if(!volSlider)
return;volSlider.addEventListener("input",()=>{const v=Number(volSlider.value)||0;if(volLabel)
volLabel.textContent=`${v}%`;styleRange(volSlider);wsSend({type:"cmd",cmd:"set_volume",value:v})})}
function initProgressControl(){const progSlider=document.querySelector(".player-progress input[type='range']");if(!progSlider)
return;progSlider.addEventListener("change",()=>{const v=Number(progSlider.value)||0;wsSend({type:"cmd",cmd:"seek_percent",value:v})})}
function initSettingsSave(){const saveBtn=document.querySelector('[data-action="save-settings"]');if(!saveBtn)
return;saveBtn.addEventListener("click",()=>{const deviceName=document.querySelector('input[name="device_name"]');const timezone=document.querySelector('input[name="timezone"]');const volumeCurve=document.querySelector('select[name="volume_curve"]');const payload={type:"settings.update",data:{device_name:deviceName?deviceName.value:undefined,timezone:timezone?timezone.value:undefined,volume_curve:volumeCurve?volumeCurve.value:undefined}};wsSend(payload)})}
let lastPlayback={position_ms:0,duration_ms:0,state:0};let lastUpdateTs=Date.now();let progressTimer=null;function updatePlayback(p){const titleEl=document.querySelector(".track-title");const artistEl=document.querySelector(".track-artist");const albumEl=document.querySelector(".track-album");const playBtn=document.querySelector(".btn.play");const artEl=document.querySelector(".cover-placeholder");const volSlider=document.querySelector(".player-volume input[type='range']");const volLabel=document.querySelector(".player-volume .volume-label");const progSlider=document.querySelector(".player-progress input[type='range']");const timeEls=document.querySelectorAll(".player-progress span");const src=document.querySelector(".stream-src");const qlty=document.querySelector(".stream-qlty");if(p.track){if(titleEl&&p.track.title)
titleEl.textContent=p.track.title;if(artistEl&&p.track.artist)
artistEl.textContent=p.track.artist;if(albumEl&&p.track.album)
albumEl.textContent=p.track.album;if(artEl){if(p.track.image){artEl.style.backgroundImage=`url('${p.track.image}')`;artEl.style.backgroundSize="cover";artEl.style.backgroundPosition="center";artEl.style.backgroundRepeat="no-repeat";artEl.textContent=""}else{(async()=>{var arturl=await fetchCoverFromMusicBrainz(p.track.artist,p.track.title);if(arturl){artEl.style.backgroundImage=`url('${arturl}')`;artEl.style.backgroundSize="cover";artEl.style.backgroundPosition="center";artEl.style.backgroundRepeat="no-repeat";artEl.textContent=""}})()}}}
if(typeof p.volume==="number"&&volSlider){volSlider.value=p.volume;styleRange(volSlider);if(volLabel)
volLabel.textContent=`${p.volume}%`}
if(typeof p.position_ms==="number"){lastPlayback.position_ms=p.position_ms}
if(typeof p.duration_ms==="number"){lastPlayback.duration_ms=p.duration_ms}
if(typeof p.state==="number"){lastPlayback.state=p.state;if(playBtn){playBtn.textContent=(p.state===1)?"pause":"play_arrow"}}
lastUpdateTs=Date.now();if(progSlider&&lastPlayback.duration_ms>0){const percent=(lastPlayback.position_ms/lastPlayback.duration_ms)*100;progSlider.value=Math.max(0,Math.min(100,percent));styleRange(progSlider);if(timeEls.length===2){timeEls[0].textContent=formatTime(lastPlayback.position_ms);timeEls[1].textContent=formatTime(lastPlayback.duration_ms)}}
if(p.src){src.textContent=p.src}
if(p.quality){qlty.textContent=p.quality}
updateModes(p)}
function formatTime(ms){const totalSec=Math.floor(ms/1000);const m=Math.floor(totalSec/60);const s=totalSec%60;return `${m.toString().padStart(1, "0")}:${s.toString().padStart(2, "0")}`}
function startProgressTimer(){if(progressTimer!==null)
return;progressTimer=setInterval(()=>{const progSlider=document.querySelector(".player-progress input[type='range']");if(!progSlider)
return;if(lastPlayback.state!==1)
return;if(!lastPlayback.duration_ms||lastPlayback.duration_ms<=0)
return;const now=Date.now();const delta=now-lastUpdateTs;if(delta<=0)
return;lastPlayback.position_ms+=delta;if(lastPlayback.position_ms>lastPlayback.duration_ms){lastPlayback.position_ms=lastPlayback.duration_ms}
lastUpdateTs=now;const percent=(lastPlayback.position_ms/lastPlayback.duration_ms)*100;progSlider.value=Math.max(0,Math.min(100,percent));styleRange(progSlider);const timeEls=document.querySelectorAll(".player-progress span");if(timeEls.length===2){timeEls[0].textContent=formatTime(lastPlayback.position_ms);timeEls[1].textContent=formatTime(lastPlayback.duration_ms)}},1000)}
async function fetchRadioStationsFromWeb(){const queryInput=document.querySelectorAll("#page-radio .radio-filters input[type='text']");const selects=document.querySelectorAll("#page-radio .radio-filters select");const countrySelect=selects[0]||null;radioState.query=queryInput[0]?queryInput[0].value.trim():"";radioState.genre=queryInput[1]?queryInput[1].value.trim():"";radioState.country=countrySelect?countrySelect.value||"":"";const params=new URLSearchParams();params.set("limit","50");params.set("hidebroken","true");if(radioState.query)
params.set("name",radioState.query);if(radioState.genre&&radioState.genre!=="All genres")
params.set("tagList",radioState.genre.toLowerCase());if(radioState.country&&radioState.country!=="All countries")
params.set("countrycode",radioState.country);const url=`${radioApiBase}/json/stations/search?${params.toString()}`;var xhr=new XMLHttpRequest();xhr.timeout=2000;xhr.onreadystatechange=function(){if(xhr.readyState===4){if(xhr.status===200){const results=JSON.parse(xhr.responseText).map((st)=>({name:st.name,url:st.url_resolved||st.url,country:st.countrycode||st.country,genre:(st.tags||"").split(",")[0]||"",homepage:st.homepage,favicon:st.favicon,bitrate:st.bitrate}));var favs=radioState.items.filter((st)=>st.favorite);for(let i=0;i<results.length;i++){favs.forEach((f)=>{if(f.url===results[i].url){results.splice(i,1)}})}
radioState.items=favs.concat(results);renderRadioStations()}else{const tbody=document.getElementById("radio-table-body");if(tbody){tbody.innerHTML=` <tr><td colspan="4">Error while searching radio stations.</td></tr>`}}}}
xhr.open("GET",url,!0);xhr.ontimeout=(e)=>{};xhr.send()}
function initRadioSearch(){const queryInput=document.querySelectorAll("#page-radio .radio-filters input[type='text']");const countrySelect=document.querySelector("#page-radio .radio-filters select");const searchBtn=document.querySelector('[data-action="radio-search"]');const tableBody=document.getElementById("radio-table-body");function triggerSearch(){fetchRadioStationsFromWeb()}
if(searchBtn){searchBtn.addEventListener("click",(e)=>{e.preventDefault();triggerSearch()})}
if(queryInput){queryInput[0].addEventListener("keydown",(e)=>{if(e.key==="Enter"){e.preventDefault();triggerSearch()}});queryInput[1].addEventListener("keydown",(e)=>{if(e.key==="Enter"){e.preventDefault();triggerSearch()}});const url=`${radioApiBase}/json/countrycodes`;var xhr=new XMLHttpRequest();xhr.timeout=2000;xhr.onreadystatechange=function(){if(xhr.readyState===4){if(xhr.status===200){const selects=document.querySelectorAll("#page-radio .radio-filters select");var countries=JSON.parse(xhr.responseText);countries.forEach((country)=>{const option=document.createElement("option");option.textContent=country.name;selects[0].appendChild(option)})}}}
xhr.open("GET",url,!0);xhr.ontimeout=(e)=>{};xhr.send()}
if(countrySelect){countrySelect.addEventListener("change",triggerSearch)}
if(tableBody){tableBody.addEventListener("click",(e)=>{const btn=e.target.closest("button[data-action]");if(!btn)
return;const action=btn.getAttribute("data-action");const row=btn.closest("tr");if(!row)
return;const url=row.getAttribute("data-url")||"";const name=row.getAttribute("data-name")||"";if(!url)
return;if(action==="radio-play"){wsSend({type:"radio.cmd",cmd:"play_station",station:{url,name}})}else if(action==="radio-save"){wsSend({type:"radio.cmd",cmd:"save_station",station:{url,name}})}else if(action==="radio-favorite"){wsSend({type:"radio.cmd",cmd:"remove_station",station:{url,name}})}})}
const addRadio=document.querySelectorAll('.radio-add .btn');const addRadioInputs=document.querySelectorAll(".radio-add input[type='text']");if(addRadio){addRadio[0].addEventListener("click",(e)=>{e.preventDefault();wsSend({type:"radio.cmd",cmd:"play_station",station:{name:addRadioInputs[0].value.trim(),url:addRadioInputs[1].value.trim()}})});addRadio[1].addEventListener("click",(e)=>{e.preventDefault();wsSend({type:"radio.cmd",cmd:"save_station",station:{name:addRadioInputs[0].value.trim(),url:addRadioInputs[1].value.trim()}})})}}
const setState={opts:!1,editing:null};function setValLabel(key,v){document.querySelectorAll('#page-settings [data-val="'+key+'"]').forEach((el)=>{el.textContent=v+(el.dataset.unit||"")})}
function applyFeatures(f){if(!f)return;document.querySelectorAll("[data-feature]").forEach((e)=>{e.hidden=f[e.dataset.feature]===!1})}
function renderSettings(m){applyFeatures(m.features);const v=m.values||{};const pg=document.getElementById("page-settings");if(!pg)
return;if(!setState.opts){pg.querySelectorAll("select[data-options]").forEach((sel)=>{(v[sel.dataset.options]||[]).forEach((o)=>{const op=document.createElement("option");op.value=o.value;op.textContent=o.label;sel.appendChild(op)})});setState.opts=!0}
pg.querySelectorAll("[data-set]").forEach((el)=>{const k=el.dataset.set;if(!(k in v)||el===document.activeElement)
return;if(el.type==="checkbox")el.checked=!!v[k];else el.value=v[k];if(el.type==="range"){styleRange(el);setValLabel(k,v[k])}});const nm=document.getElementById("set-name");if(nm&&nm!==document.activeElement)
nm.value=v.device_name||"";const hn=document.getElementById("set-hostname");if(hn)
hn.textContent=v.hostname?"(http://"+v.hostname+".local/)":"";const b=document.getElementById("restart-banner");if(b)
b.hidden=!m.restart_required;const w=m.wifi||{};const ws=document.getElementById("set-wifi-status");if(ws)
ws.textContent=w.connected?"Connected to "+(w.ssid||"?"):"Not connected";const ss=document.getElementById("set-ssid");if(ss&&!ss.value&&w.ssid)
ss.value=w.ssid;const info=document.getElementById("set-info");if(info){info.textContent="";(m.info||[]).forEach((kv)=>{const r=document.createElement("div");r.className="info-row";const a=document.createElement("span");a.textContent=kv[0];const c=document.createElement("span");c.textContent=kv[1];r.appendChild(a);r.appendChild(c);info.appendChild(r)})}
renderLed(v)}
function renderLed(v){const tb=document.getElementById("led-body");if(!tb||!Array.isArray(v.led))return;const ae=document.activeElement;if(ae&&tb.contains(ae)&&ae.tagName!=="BUTTON")return;tb.textContent="";v.led.forEach((s)=>{const tr=document.createElement("tr");const name=document.createElement("td");name.textContent=s.label;const ctd=document.createElement("td");const col=document.createElement("input");col.type="color";col.value=s.color||"#000000";col.className="led-color";ctd.appendChild(col);const offTxt=document.createElement("span");offTxt.className="hint";offTxt.textContent=col.value==="#000000"?" off":"";ctd.appendChild(offTxt);const etd=document.createElement("td");const fx=document.createElement("select");(v.led_effects||[]).forEach((o)=>{const op=document.createElement("option");op.value=o.value;op.textContent=o.label;fx.appendChild(op)});fx.value=s.effect;etd.appendChild(fx);const atd=document.createElement("td");atd.className="fs-act";const send=(o)=>sendSet({led:{[s.key]:o}});const mk=(t,ti,fn)=>{const b=document.createElement("button");b.className="btn small";b.textContent=t;b.title=ti;b.addEventListener("click",fn);atd.appendChild(b)};mk("Off","LED off in this state",()=>send({color:"#000000"}));mk("Default","Default colour and effect",()=>send({reset:!0}));col.addEventListener("change",()=>send({color:col.value}));fx.addEventListener("change",()=>send({effect:Number(fx.value)}));[name,ctd,etd,atd].forEach((x)=>tr.appendChild(x));tb.appendChild(tr)})}
function sendSet(values){wsSend({type:"settings.set",values})}
function initSettingsPage(){const pg=document.getElementById("page-settings");if(!pg)
return;pg.querySelectorAll("[data-set]").forEach((el)=>{const k=el.dataset.set;const val=()=>el.type==="checkbox"?el.checked:(el.dataset.kind==="num"?Number(el.value):el.value);if(el.type==="range"){el.addEventListener("input",()=>{styleRange(el);setValLabel(k,el.value)})}
el.addEventListener("change",()=>sendSet({[k]:val()}))});pg.addEventListener("click",(e)=>{const btn=e.target.closest("button[data-action]");if(!btn)
return;const a=btn.dataset.action;if(a==="save-name"){const n=document.getElementById("set-name").value.trim();if(n)
sendSet({device_name:n})}else if(a==="restart"){if(confirm("Restart the device now?"))
wsSend({type:"system.restart"})}else if(a==="refresh-display"){wsSend({type:"display.refresh"})}else if(a==="wifi-connect"){const ssid=document.getElementById("set-ssid").value.trim();const password=document.getElementById("set-pass").value;if(ssid)
wsSend({type:"wifi.connect",ssid,password});document.getElementById("set-pass").value=""}})}
function initTransport(){const b=document.querySelectorAll(".player-controls > .btn");if(b.length<5)
return;b[1].addEventListener("click",()=>wsSend({type:"cmd",cmd:"prev"}));b[2].addEventListener("click",()=>wsSend({type:"cmd",cmd:"toggle"}));b[3].addEventListener("click",()=>wsSend({type:"cmd",cmd:"next"}));b[0].addEventListener("click",()=>wsSend({type:"cmd",cmd:"shuffle"}));b[4].addEventListener("click",()=>wsSend({type:"cmd",cmd:"repeat"}))}
function updateModes(p){const b=document.querySelectorAll(".player-controls > .btn");if(b.length<5)
return;const c=p.caps||{};const en=(el,on)=>{el.disabled=(on===!1)};en(b[1],c.previous);en(b[2],c.pause);en(b[3],c.next);en(b[0],c.shuffle);en(b[4],c.repeat);if(typeof p.shuffle==="boolean")
b[0].classList.toggle("on",p.shuffle);if(typeof p.repeat==="string"){b[4].classList.toggle("on",p.repeat!=="off");b[4].textContent=p.repeat==="one"?"repeat_one":"repeat"}
const ps=p.playback_state;if(ps&&b[2])b[2].textContent=(ps==="playing"||ps==="buffering")?"pause":"play_arrow"}
function fmtDur(ms){return ms>0?formatTime(ms):""}
function renderQueue(m){const tb=document.getElementById("queue-body");if(!tb)
return;tb.textContent="";const items=m.items||[];const qb=document.getElementById("queue-box");const pk=document.getElementById("queue-peek");if(qb){qb.classList.toggle("empty",!items.length);if(!items.length)qb.classList.remove("open")}
if(pk)pk.textContent=items.length?(items[0].title||items[0].id||"")+(items.length>1?"  (+"+(items.length-1)+")":""):"";if(!items.length){const tr=document.createElement("tr");const td=document.createElement("td");td.textContent="-";tr.appendChild(td);tb.appendChild(tr);return}
items.forEach((it)=>{const tr=document.createElement("tr");const t1=document.createElement("td");t1.textContent=it.title||it.id||"";const t2=document.createElement("td");t2.textContent=[it.artist,it.album].filter(Boolean).join(" · ");const t3=document.createElement("td");t3.textContent=fmtDur(it.duration_ms);tr.appendChild(t1);tr.appendChild(t2);tr.appendChild(t3);tr.addEventListener("click",()=>wsSend({type:"cmd",cmd:"play_index",value:it.index}));tb.appendChild(tr)})}
const fsState={path:"/sdcard",root:"/sdcard",entries:[],sel:new Set(),busy:0};const fsWait=[];
function fsCall(obj){return new Promise((res)=>{fsWait.push(res);wsSend(obj);setTimeout(()=>{const i=fsWait.indexOf(res);if(i>=0){fsWait.splice(i,1);res({ok:false,message:"no answer from the device"})}},15000)})}
function fsReply(m){const r=fsWait.shift();if(r)r(m)}
function fsJoin(dir,name){return dir.replace(/\/$/,"")+"/"+name}
function fsParent(p){const i=p.lastIndexOf("/");return i>0?p.substring(0,i):"/"}
function fsBase(p){return p.substring(p.lastIndexOf("/")+1)}
function fmtSize(b){return b>=1073741824?(b/1073741824).toFixed(2)+" GB":b>=1048576?(b/1048576).toFixed(1)+" MB":b>=1024?Math.round(b/1024)+" KB":b?b+" B":"0 B"}
function el(tag,props,kids){const e=document.createElement(tag);if(props)Object.keys(props).forEach((k)=>{if(k==="text")e.textContent=props[k];else if(k==="cls")e.className=props[k];else if(k==="on")Object.keys(props.on).forEach((ev)=>e.addEventListener(ev,props.on[ev]));else e.setAttribute(k,props[k])});(kids||[]).forEach((c)=>c&&e.appendChild(c));return e}
function fsMsg(text,ok){const m=document.getElementById("fs-msg");if(!m)return;m.hidden=!text;m.textContent=text||"";m.classList.toggle("ok",!!ok);clearTimeout(fsMsg.t);if(text)fsMsg.t=setTimeout(()=>{m.hidden=!0},ok?3000:8000)}
function fsCheck(r,okText){if(r&&r.ok){if(okText)fsMsg(okText,!0);return!0}
fsMsg((r&&r.message)||"failed");return!1}
function fsBadName(n){return!n||/[\\/:*?"<>|]/.test(n)||n==="."||n===".."}
async function fsList(path){if(path)fsState.path=path;const m=await fsCall({type:"fs.list",path:fsState.path});renderFs(m)}
function renderFs(m){if(m.root)fsState.root=m.root;if(m.path)fsState.path=m.path;fsState.entries=m.entries||[];fsState.sel.clear();const cr=document.getElementById("fs-crumbs");if(cr){cr.textContent="";const parts=fsState.path.substring(fsState.root.length).split("/").filter(Boolean);let acc=fsState.root;cr.appendChild(el("a",{text:"SD card",on:{click:()=>fsList(fsState.root)}}));parts.forEach((p)=>{acc+="/"+p;const target=acc;cr.appendChild(el("span",{cls:"sep",text:"/"}));cr.appendChild(el("a",{text:p,on:{click:()=>fsList(target)}}))})}
const sp=document.getElementById("fs-space");if(sp)sp.textContent=m.total?("· "+fmtSize(m.free)+" free of "+fmtSize(m.total)):"";const tb=document.getElementById("sd-table-body");if(!tb)return;tb.textContent="";if(m.error){tb.appendChild(el("tr",null,[el("td",{colspan:"5",text:m.error})]));fsSelUpdate();return}
if(!fsState.entries.length)tb.appendChild(el("tr",null,[el("td",{colspan:"5",text:"(empty folder)"})]));fsState.entries.forEach((e)=>{const full=fsJoin(fsState.path,e.name);const cb=el("input",{type:"checkbox"});cb.addEventListener("change",()=>{if(cb.checked)fsState.sel.add(full);else fsState.sel.delete(full);fsSelUpdate()});const icon=e.dir?"\u{1F4C1}":e.playlist?"☰":e.audio?"♪":"\u{1F4C4}";const playable=e.dir||e.audio||e.playlist;const nm=el("td",{cls:"fs-name"+(e.dir||e.audio||e.playlist||e.text?"":" dim"),text:e.name,title:e.dir?"Open folder":e.audio||e.playlist?"Play":e.text?"Edit":""});nm.addEventListener("click",()=>{if(e.dir)fsList(full);else if(e.audio)wsSend({type:"sd.cmd",cmd:"play",path:full});else if(e.playlist)wsSend({type:"sd.cmd",cmd:"play_playlist",path:full});else if(e.text)fsEdit(full)});const act=el("td",{cls:"fs-act"});const btn=(label,title,fn,cls)=>act.appendChild(el("button",{cls:"btn small"+(cls?" "+cls:""),text:label,title,on:{click:(ev)=>{ev.stopPropagation();fn()}}}));if(e.audio||e.dir)btn("▶","Play",()=>wsSend(e.dir?{type:"sd.cmd",cmd:"play_folder",path:full}:{type:"sd.cmd",cmd:"play",path:full}));if(e.playlist){btn("▶","Play playlist",()=>wsSend({type:"sd.cmd",cmd:"play_playlist",path:full}));btn("☰","Edit playlist",()=>plOpen(full))}
if(e.audio)btn("+","Add to playlist",()=>plAddFiles([full]));if(e.text)btn("✎","Edit text",()=>fsEdit(full));btn("Aa","Rename",()=>fsRename(full));if(!e.dir)act.appendChild(el("a",{cls:"btn small",text:"↓",title:"Download",href:"/api/fs/download?path="+encodeURIComponent(full),download:e.name}));btn("✕","Delete",()=>fsDelete([full]),"danger");tb.appendChild(el("tr",null,[el("td",{cls:"fs-c"},[cb]),el("td",{cls:"fs-c",text:icon}),nm,el("td",{cls:"fs-size",text:e.dir?"":fmtSize(e.size)}),act]))});const all=document.getElementById("fs-all");if(all)all.checked=!1;fsSelUpdate()}
function fsSelUpdate(){const t=document.getElementById("fs-sel-tools");const n=fsState.sel.size;if(t)t.hidden=!n;const c=document.getElementById("fs-sel-count");if(c)c.textContent=n+" selected"}
async function fsMkdir(){const n=prompt("Name of the new folder:");if(n===null)return;if(fsBadName(n.trim()))return fsMsg("invalid name");fsCheck(await fsCall({type:"fs.mkdir",path:fsJoin(fsState.path,n.trim())}),"folder created");fsList()}
async function fsNewFile(){const n=prompt("Name of the new text file:","notes.txt");if(n===null)return;if(fsBadName(n.trim()))return fsMsg("invalid name");const p=fsJoin(fsState.path,n.trim());if(fsState.entries.some((e)=>e.name===n.trim()))return fsMsg("file exists");if(fsCheck(await fsCall({type:"fs.write",path:p,content:""}))){await fsList();fsEdit(p)}}
async function fsRename(full){const old=fsBase(full);const n=prompt("New name:",old);if(n===null||n.trim()===old)return;if(fsBadName(n.trim()))return fsMsg("invalid name");fsCheck(await fsCall({type:"fs.move",from:full,to:fsJoin(fsParent(full),n.trim())}),"renamed");fsList()}
async function fsDelete(paths){if(!paths.length)return;const what=paths.length===1?'"'+fsBase(paths[0])+'"':paths.length+" items";if(!confirm("Delete "+what+"? Folders are deleted with their content."))return;let ok=0;for(const p of paths){const r=await fsCall({type:"fs.delete",path:p});if(fsCheck(r))ok++;else break}
if(ok===paths.length)fsMsg(ok===1?"deleted":ok+" items deleted",!0);fsList()}
async function fsMove(paths){if(!paths.length)return;const d=prompt("Move "+paths.length+" item(s) to folder:",fsState.path);if(d===null)return;const dest=d.trim().replace(/\/+$/,"");if(!dest.startsWith(fsState.root))return fsMsg("the target must be below "+fsState.root);let ok=0;for(const p of paths){if(fsParent(p)===dest){ok++;continue}
if(fsCheck(await fsCall({type:"fs.move",from:p,to:fsJoin(dest,fsBase(p))})))ok++;else break}
if(ok===paths.length)fsMsg("moved",!0);fsList()}
const fsEd={path:""};async function fsEdit(path){const m=await fsCall({type:"fs.read",path});if(m.error)return fsMsg(m.error);fsEd.path=path;document.getElementById("fs-editor-title").textContent=fsBase(path);const ta=document.getElementById("fs-editor-text");ta.value=m.content||"";document.getElementById("fs-editor-info").textContent=path+" · "+fmtSize(m.size||0);document.getElementById("fs-editor").hidden=!1;ta.focus()}
async function fsEditSave(){const ta=document.getElementById("fs-editor-text");const r=await fsCall({type:"fs.write",path:fsEd.path,content:ta.value});if(fsCheck(r,"saved")){document.getElementById("fs-editor").hidden=!0;fsList();if(plEd.path===fsEd.path)plOpen(plEd.path);plLoad()}}
function fsUploadOne(file,dest,overwrite,onProg){return new Promise((res)=>{const x=new XMLHttpRequest();x.open("POST","/api/fs/upload?path="+encodeURIComponent(dest)+(overwrite?"&overwrite=1":""));x.upload.onprogress=(ev)=>{if(ev.lengthComputable)onProg(ev.loaded)};x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}
res({status:x.status,ok:x.status===200&&j.ok,message:j.message||("HTTP "+x.status)})};x.onerror=()=>res({ok:!1,message:"network error"});x.send(file)})}
async function fsUpload(files){files=Array.from(files||[]);if(!files.length)return;const total=files.reduce((a,f)=>a+f.size,0)||1;let done=0;const pr=document.getElementById("fs-progress");const bar=pr.querySelector(".fs-progress-bar");const lab=pr.querySelector("span");pr.hidden=!1;let failed=0;for(let i=0;i<files.length;i++){const f=files[i];if(fsBadName(f.name)){failed++;continue}
const dest=fsJoin(fsState.path,f.name);const show=(n)=>{bar.style.width=Math.round(((done+n)*100)/total)+"%";lab.textContent=(i+1)+"/"+files.length+" "+f.name+" · "+fmtSize(done+n)+" / "+fmtSize(total)};show(0);let r=await fsUploadOne(f,dest,!1,show);if(r.status===409&&confirm('"'+f.name+'" exists. Overwrite?'))r=await fsUploadOne(f,dest,!0,show);if(!r.ok&&r.status!==409){failed++;fsMsg(f.name+": "+r.message)}
done+=f.size}
pr.hidden=!0;if(!failed)fsMsg(files.length+" file(s) uploaded",!0);fsList()}
const plEd={path:"",entries:[]};async function plLoad(){const m=await fsCall({type:"pl.list"});const tb=document.getElementById("pl-body");if(!tb)return;tb.textContent="";const pls=m.playlists||[];if(!pls.length)tb.appendChild(el("tr",null,[el("td",{text:"No playlists yet."})]));pls.forEach((p)=>{const act=el("td",{cls:"fs-act"});const b=(t,ti,fn,c)=>act.appendChild(el("button",{cls:"btn small"+(c?" "+c:""),text:t,title:ti,on:{click:fn}}));b("▶","Play",()=>wsSend({type:"sd.cmd",cmd:"play_playlist",path:p.path}));b("Edit","Edit",()=>plOpen(p.path));b("✕","Delete",async()=>{if(!confirm('Delete playlist "'+p.name+'"? (the music files stay)'))return;fsCheck(await fsCall({type:"fs.delete",path:p.path}),"deleted");if(plEd.path===p.path)plClose();plLoad()},"danger");tb.appendChild(el("tr",null,[el("td",{cls:"fs-name",text:p.name.replace(/\.m3u8?$/i,""),on:{click:()=>plOpen(p.path)}}),el("td",{text:p.count+" tracks"}),act]))});return pls}
async function plOpen(path){const m=await fsCall({type:"pl.get",path});plEd.path=m.path||path;plEd.entries=(m.entries||[]).map((e)=>({path:e.path,title:e.title,exists:e.exists}));document.getElementById("pl-name").value=fsBase(plEd.path).replace(/\.m3u8?$/i,"");document.getElementById("pl-editor").hidden=!1;plRender();document.getElementById("pl-editor").scrollIntoView({behavior:"smooth",block:"nearest"})}
function plClose(){plEd.path="";plEd.entries=[];document.getElementById("pl-editor").hidden=!0}
function plRender(){const tb=document.getElementById("pl-entries");tb.textContent="";if(!plEd.entries.length)tb.appendChild(el("tr",null,[el("td",{text:"(empty)"})]));plEd.entries.forEach((e,i)=>{const act=el("td",{cls:"fs-act"});const b=(t,ti,fn,dis)=>{const x=el("button",{cls:"btn small",text:t,title:ti,on:{click:fn}});if(dis)x.disabled=!0;act.appendChild(x)};b("▶","Play from here",async()=>{await plSave(!0);wsSend({type:"sd.cmd",cmd:"play_playlist",path:plEd.path,index:i})});b("↑","Up",()=>{[plEd.entries[i-1],plEd.entries[i]]=[plEd.entries[i],plEd.entries[i-1]];plRender()},i===0);b("↓","Down",()=>{[plEd.entries[i+1],plEd.entries[i]]=[plEd.entries[i],plEd.entries[i+1]];plRender()},i===plEd.entries.length-1);b("✕","Remove",()=>{plEd.entries.splice(i,1);plRender()});tb.appendChild(el("tr",null,[el("td",{text:String(i+1)}),el("td",{cls:e.exists===!1?"pl-missing":"",text:e.title||fsBase(e.path),title:e.path+(e.exists===!1?" (missing)":"")}),act]))})}
function plPathFromName(n){return"/sdcard/Playlists/"+n+".m3u"}
async function plSave(quiet){const n=document.getElementById("pl-name").value.trim();if(fsBadName(n)){fsMsg("invalid playlist name");return!1}
let target=plEd.path;const wanted=fsParent(plEd.path||plPathFromName(n))+"/"+n+(/\.m3u8$/i.test(plEd.path)?".m3u8":".m3u");if(plEd.path&&wanted!==plEd.path){if(!fsCheck(await fsCall({type:"fs.move",from:plEd.path,to:wanted})))return!1}
target=wanted;const r=await fsCall({type:"pl.save",path:target,entries:plEd.entries.map((e)=>e.path)});if(!fsCheck(r,quiet?"":"playlist saved"))return!1;plEd.path=r.message||target;plLoad();return!0}
async function plNew(){const n=prompt("Name of the new playlist:");if(n===null)return;if(fsBadName(n.trim()))return fsMsg("invalid name");const p=plPathFromName(n.trim());const pls=await plLoad();if((pls||[]).some((x)=>x.path.toLowerCase()===p.toLowerCase()))return plOpen(p);if(fsCheck(await fsCall({type:"pl.save",path:p,entries:plEd.path?[]:[]}),"playlist created")){await plLoad();plOpen(p)}}
async function plAddFiles(files){files=files.filter((f)=>{const e=fsState.entries.find((x)=>fsJoin(fsState.path,x.name)===f);return!e||e.audio});if(!files.length)return fsMsg("select audio files");if(plEd.path){files.forEach((f)=>plEd.entries.push({path:f,title:fsBase(f).replace(/\.[^.]+$/,""),exists:!0}));plRender();fsMsg(files.length+" added to the open playlist (save it)",!0);return}
const pls=await plLoad();const names=(pls||[]).map((p)=>p.name.replace(/\.m3u8?$/i,""));const n=prompt("Add "+files.length+" file(s) to playlist:"+(names.length?"\n(existing: "+names.join(", ")+")":""),names[0]||"My playlist");if(n===null)return;if(fsBadName(n.trim()))return fsMsg("invalid name");const ex=(pls||[]).find((p)=>p.name.replace(/\.m3u8?$/i,"").toLowerCase()===n.trim().toLowerCase());if(fsCheck(await fsCall({type:"pl.add",path:ex?ex.path:plPathFromName(n.trim()),files}),"added to "+n.trim()))plLoad()}
function initFiles(){const nav=document.querySelector('.nav-link[data-page="sd"]');if(nav)nav.addEventListener("click",()=>{fsList();plLoad()});const on=(a,fn)=>{const b=document.querySelector('[data-action="'+a+'"]');if(b)b.addEventListener("click",fn)};on("fs-up",()=>{if(fsState.path.length>fsState.root.length)fsList(fsParent(fsState.path))});on("fs-refresh",()=>{fsList();plLoad()});on("fs-mkdir",fsMkdir);on("fs-newfile",fsNewFile);on("fs-play-folder",()=>wsSend({type:"sd.cmd",cmd:"play_folder",path:fsState.path}));on("fs-sel-delete",()=>fsDelete([...fsState.sel]));on("fs-sel-move",()=>fsMove([...fsState.sel]));on("fs-sel-playlist",()=>plAddFiles([...fsState.sel]));on("fs-editor-save",fsEditSave);on("fs-editor-close",()=>{document.getElementById("fs-editor").hidden=!0});on("pl-new",plNew);on("pl-save",()=>plSave(!1));on("pl-play",async()=>{if(await plSave(!0))wsSend({type:"sd.cmd",cmd:"play_playlist",path:plEd.path})});on("pl-close",plClose);const up=document.getElementById("fs-upload");if(up)up.addEventListener("change",()=>{fsUpload(up.files);up.value=""});const all=document.getElementById("fs-all");if(all)all.addEventListener("change",()=>{document.querySelectorAll("#sd-table-body input[type=checkbox]").forEach((c)=>{c.checked=all.checked;c.dispatchEvent(new Event("change"))})});const dz=document.getElementById("fs-drop");if(dz){dz.addEventListener("dragover",(e)=>{e.preventDefault();dz.classList.add("over")});dz.addEventListener("dragleave",()=>dz.classList.remove("over"));dz.addEventListener("drop",(e)=>{e.preventDefault();dz.classList.remove("over");if(e.dataTransfer&&e.dataTransfer.files.length)fsUpload(e.dataTransfer.files)})}
document.addEventListener("keydown",(e)=>{if(e.key==="Escape")document.getElementById("fs-editor").hidden=!0;if((e.ctrlKey||e.metaKey)&&e.key==="s"&&!document.getElementById("fs-editor").hidden){e.preventDefault();fsEditSave()}})}
function init(){initNav();initSettingsTabs();initAllRanges();initVolumeControl();initProgressControl();initTransport();initFiles();initSettingsPage();initSettingsSave();initRadioSearch();connectWebSocket();startProgressTimer()}
if(document.readyState==="loading"){document.addEventListener("DOMContentLoaded",init)}else{init()}})()
)delim";

static std::shared_ptr<AudioControl> g_audio;
static std::unique_ptr<AudioControl::FeedControl> g_feed;
static bell::BellHTTPServer* g_http = nullptr;
static OnWsMessage on_ws_msg_ = nullptr;
static std::vector<mg_connection*> g_ws_conn{};
static std::mutex g_ws_mtx;

// --- Small helper to read a whole file into memory ---
static bool readFile(const char* path, std::string& out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    BELL_LOG(error, "WebUI", "Failed to open file: %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return false;
  }

  out.resize(sz);
  if (sz > 0 && fread(&out[0], 1, sz, f) != (size_t)sz) {
    fclose(f);
    return false;
  }
  fclose(f);
  return true;
}

// --- Helper to build an HTTP response from a string ---
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> makeTextResponse(
    const std::string& body, const std::string& contentType) {
  auto resp = std::make_unique<bell::BellHTTPServer::HTTPResponse>();
  resp->status = 200;
  resp->bodySize = body.size();
  resp->body = (uint8_t*)malloc(body.size());
  if (!resp->body && body.size() > 0) {
    // allocation failed; fall back to empty response
    resp->bodySize = 0;
  } else if (resp->bodySize > 0) {
    memcpy(resp->body, body.data(), body.size());
  }
  resp->headers["Content-Type"] = contentType;
  return resp;
}

// --- Static file handler (index.html, css, js) ---
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> serveStaticFile(
    const char* path, const char* mime) {
  std::string body;
  if (!readFile(path, body)) {
    // 404
    auto notFound = std::make_unique<bell::BellHTTPServer::HTTPResponse>();
    notFound->status = 404;
    notFound->body = nullptr;
    notFound->bodySize = 0;
    notFound->headers["Content-Type"] = "text/plain; charset=utf-8";
    return notFound;
  }
  return makeTextResponse(body, mime);
}
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> serveStatic(
    const char* body, const char* mime) {
  return makeTextResponse(body, mime);
}
// --- WebSocket output ---------------------------------------------------------
// All messages go through a queue and one sender task.  Callers (stream
// tasks logging, playback updates, ...) never block on a slow or dead
// browser: a websocket write can wait for the TCP send timeout, and doing
// that in the Spotify / Qobuz tasks made the audio stutter and blocked the
// web server for everybody.
enum class WsKind : uint8_t { Normal, Log, Status };
struct WsOut {
  std::string data;
  mg_connection* conn;  // nullptr = all
  WsKind kind;
};
static std::mutex g_q_mtx;
static std::condition_variable g_q_cv;
static std::deque<WsOut> g_q;
static std::string g_status_cache;  // guarded by g_q_mtx
static std::atomic<int> g_ws_count{0};
static std::atomic<bool> g_sender_up{false};
constexpr size_t kWsQueueMax = 48;  // messages
constexpr size_t kWsLogMax = 16;    // log lines waiting (older ones dropped)

static bool isConnected() {
  return g_ws_count.load() > 0;
}

// write to the connected clients (sender task only)
static void wsWriteNow(const WsOut& m) {
  // NOTE: never mg_close_connection() a server connection from here: it
  // belongs to a civetweb worker thread (which may already be serving the
  // next HTTP request with the same struct).  A failed write only drops the
  // connection from the list; civetweb closes the socket itself.
  std::lock_guard<std::mutex> lk(g_ws_mtx);
  for (auto it = g_ws_conn.begin(); it != g_ws_conn.end();) {
    if (m.conn && *it != m.conn) {
      ++it;
      continue;
    }
    int rc = mg_websocket_write(*it, MG_WEBSOCKET_OPCODE_TEXT, m.data.c_str(),
                                m.data.size());
    if (rc <= 0) {
      BELL_LOG(info, "WebUI", "WS write failed, dropping conn %p", *it);
      it = g_ws_conn.erase(it);
      g_ws_count.store((int)g_ws_conn.size());
    } else {
      ++it;
    }
  }
}

static void wsSenderLoop() {
  for (;;) {
    WsOut m;
    {
      std::unique_lock<std::mutex> lk(g_q_mtx);
      g_q_cv.wait(lk, [] { return !g_q.empty(); });
      m = std::move(g_q.front());
      g_q.pop_front();
    }
    if (isConnected())
      wsWriteNow(m);
  }
}

static void wsEnqueue(std::string json, mg_connection* conn, WsKind kind) {
  if (!isConnected())
    return;
  {
    std::lock_guard<std::mutex> lk(g_q_mtx);
    if (kind == WsKind::Status && !conn) {
      // only the newest playback state matters
      for (auto& q : g_q)
        if (q.kind == WsKind::Status && !q.conn) {
          q.data = std::move(json);
          return;
        }
    }
    if (kind == WsKind::Log) {
      size_t logs = 0;
      for (auto& q : g_q)
        logs += q.kind == WsKind::Log;
      if (logs >= kWsLogMax)
        return;  // browser too slow: skip log lines
    }
    if (g_q.size() >= kWsQueueMax) {
      // full: drop a queued log line, else this message
      auto it = std::find_if(g_q.begin(), g_q.end(),
                             [](const WsOut& q) { return q.kind == WsKind::Log; });
      if (it == g_q.end())
        return;
      g_q.erase(it);
    }
    g_q.push_back({std::move(json), conn, kind});
  }
  g_q_cv.notify_one();
}

// --- JSON send over websocket (queued, never blocks) ---
static void wsSendJson(const std::string& json, mg_connection* conn = nullptr) {
  wsEnqueue(json, conn, WsKind::Normal);
}
// log lines (dropped when the browser cannot keep up)
static void wsSendLog(const std::string& line) {
  wsEnqueue(line, nullptr, WsKind::Log);
}
// --- JSON send state over websocket and save to cache---
static void wsSendJsonStatus(const std::string& json) {
  {
    std::lock_guard<std::mutex> lk(g_q_mtx);
    if (json == g_status_cache)
      return;  // no change
    g_status_cache = json;
  }
  wsEnqueue(json, nullptr, WsKind::Status);
}

// --- WebSocket state handler (connect / ready / closed) ---
static void wsStateHandler(mg_connection* conn,
                           bell::BellHTTPServer::WSState st) {
  switch (st) {
    case bell::BellHTTPServer::WSState::CONNECTED:
      BELL_LOG(info, "WebUI", "WS CONNECTED");
      break;
    case bell::BellHTTPServer::WSState::READY: {
      BELL_LOG(info, "WebUI", "WS READY");
      {
        std::lock_guard<std::mutex> lk(g_ws_mtx);
        // (no limit / no kicking: two browsers kicked each other every
        //  3 s — the page reconnects — and kept the server threads busy)
        g_ws_conn.push_back(conn);
        g_ws_count.store((int)g_ws_conn.size());
      }
      // send initial state
      std::string cached;
      {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        cached = g_status_cache;
      }
      if (!cached.empty())
        wsSendJson(cached, conn);
      on_ws_msg_(conn, nullptr, 0);
      break;
    }
    case bell::BellHTTPServer::WSState::CLOSED: {
      BELL_LOG(info, "WebUI", "WS CLOSED");
      // (called twice: on the close frame and when civetweb ends the
      //  connection; civetweb closes the socket itself)
      std::lock_guard<std::mutex> lk(g_ws_mtx);
      for (auto it = g_ws_conn.begin(); it != g_ws_conn.end(); ++it) {
        if (*it == conn) {
          g_ws_conn.erase(it);
          break;
        }
      }
      g_ws_count.store((int)g_ws_conn.size());
      break;
    }
  }
}

// --- HTTP handlers ---------------------------------------------------------

// GET /  → index.html
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> handleRoot(
    struct mg_connection* conn) {
  (void)conn;
  return serveStatic(TEXT_INDEX, "text/html; charset=utf-8");
}

// GET /style.css
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> handleCSS(
    struct mg_connection* conn) {
  (void)conn;
  return serveStatic(TEXT_CSS, "text/css; charset=utf-8");
}

// GET /app.js
static std::unique_ptr<bell::BellHTTPServer::HTTPResponse> handleJS(
    struct mg_connection* conn) {
  (void)conn;
  return serveStatic(TEXT_JS, "application/javascript; charset=utf-8");
}

/** The HTTP server (nullptr before WebUI_start). */
inline bell::BellHTTPServer* server() { return g_http; }

// --- Public init function you can call from app_main -----------------------

void WebUI_start(int port, OnWsMessage on_ws_msg__) {
  static bell::BellHTTPServer server(port, {
                                               {"thread_stack_size", "16384"},  // PSRAM (see civetweb.c)
                                               {"num_threads", "3"},
                                               {"prespawn_threads", "3"},
                                               {"connection_queue", "5"},
                                               // a dead browser must not hold a
                                               // server thread for 30 s
                                               {"request_timeout_ms", "8000"},
                                               // detect browsers that went away
                                               // (sleeping phone): ping every 15 s,
                                               // the thread is freed after a few
                                               // unanswered pings
                                               {"enable_websocket_ping_pong", "yes"},
                                               {"websocket_timeout_ms", "15000"},
                                           });
  g_http = &server;
  {
    // civetweb left its thread config (PSRAM stack, name) in this task:
    // reset it so later pthreads created here get the defaults
    esp_pthread_cfg_t def = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&def);
  }
  if (!g_sender_up.exchange(true))
    xTaskCreatePinnedToCoreWithCaps([](void*) { wsSenderLoop(); }, "ws_send", 6144,
                                    nullptr, 1, nullptr, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  on_ws_msg_ = on_ws_msg__;
  // HTTP routes
  server.registerGet("/", handleRoot);
  server.registerGet("/index.html", handleRoot);
  server.registerGet("/style.css", handleCSS);
  server.registerGet("/app.js", handleJS);
  // unknown URLs (favicon.ico, ...): a tiny 404 written here.  civetweb's own
  // error page formats into big stack buffers.
  server.registerNotFound([](struct mg_connection* c)
                              -> std::unique_ptr<bell::BellHTTPServer::HTTPResponse> {
    static const char k404[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    mg_write(c, k404, sizeof(k404) - 1);
    return nullptr;
  });

  // WebSocket endpoint
  server.registerWS("/ws", on_ws_msg_, wsStateHandler);

  BELL_LOG(info, "WebUI", "HTTP/WebSocket UI started on port %d", port);
}
}  // namespace WebUI