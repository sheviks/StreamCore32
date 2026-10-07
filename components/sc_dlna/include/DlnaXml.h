#pragma once
// ============================================================================
//  DlnaXml — small XML / UPnP helpers for the DLNA renderer.
//
//  No XML library: UPnP control messages are simple and flat, a tag search
//  is enough (and tolerant of the namespace prefixes control points use).
//  Pure C++, unit tested on the PC (tools/dlna_test).
// ============================================================================
#include <cstdint>
#include <string>
#include <string_view>

namespace dlna {

/** &<>"' -> entities (for element text and attribute values). */
std::string xmlEscape(std::string_view s);
/** Entities (named + &#NN; / &#xNN;) -> text. */
std::string xmlUnescape(std::string_view s);

/**
 * Text of the first element named `name` (namespace prefix ignored, i.e.
 * "Title" matches <Title>, <dc:title> if name is "title", ...) below `from`.
 * Empty elements (<x/>) give "". Returns false if there is no such element.
 * The text is returned as it is (not unescaped).
 */
bool xmlElement(std::string_view xml, std::string_view name, std::string& out,
                size_t* endPos = nullptr);
/** Like xmlElement(), unescaped, "" if missing. */
std::string xmlText(std::string_view xml, std::string_view name);
/** Attribute `attr` of the first element `name` (unescaped), "" if missing. */
std::string xmlAttr(std::string_view xml, std::string_view name,
                    std::string_view attr);

/** "1:02:03.500" / "02:03" / "123" -> ms (false on rubbish). */
bool parseTime(std::string_view s, uint32_t& ms);
/** ms -> "H:MM:SS" (UPnP time format). */
std::string formatTime(uint32_t ms);

/** Track information from a DIDL-Lite document. */
struct DidlInfo {
  std::string title, artist, album, artUri;
  std::string protocolInfo;  // of the <res> element
  std::string mime;          // 3rd field of protocolInfo
  uint32_t durationMs = 0;
  uint64_t size = 0;
};
DidlInfo parseDidl(std::string_view didl);

/** Minimal DIDL-Lite item (when a control point sent no metadata). */
std::string makeDidl(const DidlInfo& d, const std::string& uri);

}  // namespace dlna
