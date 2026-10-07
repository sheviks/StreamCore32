#include "DlnaXml.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dlna {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

bool isNameChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' ||
         c == ':';
}

// local part of a tag name ("u:Play" -> "Play")
std::string_view localName(std::string_view n) {
  auto c = n.rfind(':');
  return c == std::string_view::npos ? n : n.substr(c + 1);
}

// Find the start tag of element `name` at or after `pos`.  Sets the qualified
// name, where the attributes start/end and whether it is empty (<x/>).
bool findStartTag(std::string_view xml, std::string_view name, size_t pos,
                  size_t& tagStart, size_t& attrStart, size_t& tagEnd,
                  std::string_view& qname, bool& empty) {
  while (pos < xml.size()) {
    size_t lt = xml.find('<', pos);
    if (lt == std::string_view::npos || lt + 1 >= xml.size())
      return false;
    char c = xml[lt + 1];
    if (c == '/' || c == '?' || c == '!') {
      if (xml.compare(lt, 9, "<![CDATA[") == 0) {
        size_t e = xml.find("]]>", lt);
        pos = e == std::string_view::npos ? xml.size() : e + 3;
      } else if (xml.compare(lt, 4, "<!--") == 0) {
        size_t e = xml.find("-->", lt);
        pos = e == std::string_view::npos ? xml.size() : e + 3;
      } else {
        pos = lt + 1;
      }
      continue;
    }
    size_t n = lt + 1;
    while (n < xml.size() && isNameChar(xml[n]))
      n++;
    std::string_view q = xml.substr(lt + 1, n - lt - 1);
    // the end of the tag: '>' outside of quotes
    size_t e = n;
    char quote = 0;
    for (; e < xml.size(); e++) {
      char ch = xml[e];
      if (quote) {
        if (ch == quote)
          quote = 0;
      } else if (ch == '"' || ch == '\'') {
        quote = ch;
      } else if (ch == '>') {
        break;
      }
    }
    if (e >= xml.size())
      return false;
    if (ieq(localName(q), name) || ieq(q, name)) {
      tagStart = lt;
      attrStart = n;
      tagEnd = e + 1;
      qname = q;
      empty = e > n && xml[e - 1] == '/';
      return true;
    }
    pos = e + 1;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- escaping --
std::string xmlEscape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + s.size() / 8);
  for (char c : s) {
    switch (c) {
      case '&':
        o += "&amp;";
        break;
      case '<':
        o += "&lt;";
        break;
      case '>':
        o += "&gt;";
        break;
      case '"':
        o += "&quot;";
        break;
      case '\'':
        o += "&apos;";
        break;
      default:
        o += c;
    }
  }
  return o;
}

std::string xmlUnescape(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c != '&') {
      o += c;
      continue;
    }
    size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      o += c;
      continue;
    }
    std::string_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp")
      o += '&';
    else if (ent == "lt")
      o += '<';
    else if (ent == "gt")
      o += '>';
    else if (ent == "quot")
      o += '"';
    else if (ent == "apos")
      o += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      std::string num(ent.substr(1));
      unsigned long cp = (!num.empty() && (num[0] == 'x' || num[0] == 'X'))
                             ? strtoul(num.c_str() + 1, nullptr, 16)
                             : strtoul(num.c_str(), nullptr, 10);
      appendUtf8(o, (uint32_t)cp);
    } else {
      o += c;  // unknown entity: keep as it is
      continue;
    }
    i = semi;
  }
  return o;
}

// -------------------------------------------------------------- elements --
bool xmlElement(std::string_view xml, std::string_view name, std::string& out,
                size_t* endPos) {
  size_t ts, as, te;
  std::string_view q;
  bool empty;
  if (!findStartTag(xml, name, 0, ts, as, te, q, empty))
    return false;
  if (empty) {
    out.clear();
    if (endPos)
      *endPos = te;
    return true;
  }
  // matching end tag (same qualified name); nested elements of the same
  // name are not used in UPnP messages
  std::string close = "</" + std::string(q);
  size_t ce = xml.find(close, te);
  while (ce != std::string_view::npos) {
    size_t k = ce + close.size();
    while (k < xml.size() && std::isspace((unsigned char)xml[k]))
      k++;
    if (k < xml.size() && xml[k] == '>')
      break;
    ce = xml.find(close, ce + 1);
  }
  if (ce == std::string_view::npos)
    return false;
  std::string_view body = xml.substr(te, ce - te);
  // CDATA section: take its content as it is
  if (body.size() >= 12 && body.compare(0, 9, "<![CDATA[") == 0) {
    size_t e = body.find("]]>");
    if (e != std::string_view::npos)
      body = body.substr(9, e - 9);
  }
  out.assign(body.data(), body.size());
  if (endPos)
    *endPos = ce;
  return true;
}

std::string xmlText(std::string_view xml, std::string_view name) {
  std::string v;
  if (!xmlElement(xml, name, v))
    return "";
  return xmlUnescape(v);
}

std::string xmlAttr(std::string_view xml, std::string_view name,
                    std::string_view attr) {
  size_t ts, as, te;
  std::string_view q;
  bool empty;
  if (!findStartTag(xml, name, 0, ts, as, te, q, empty))
    return "";
  std::string_view a = xml.substr(as, te - as);
  size_t p = 0;
  while (p < a.size()) {
    while (p < a.size() && !isNameChar(a[p]))
      p++;
    size_t ns = p;
    while (p < a.size() && isNameChar(a[p]))
      p++;
    std::string_view an = a.substr(ns, p - ns);
    while (p < a.size() && std::isspace((unsigned char)a[p]))
      p++;
    if (p >= a.size() || a[p] != '=')
      continue;
    p++;
    while (p < a.size() && std::isspace((unsigned char)a[p]))
      p++;
    if (p >= a.size())
      break;
    char quote = a[p];
    if (quote != '"' && quote != '\'')
      continue;
    size_t ve = a.find(quote, p + 1);
    if (ve == std::string_view::npos)
      break;
    if (ieq(localName(an), attr) || ieq(an, attr))
      return xmlUnescape(a.substr(p + 1, ve - p - 1));
    p = ve + 1;
  }
  return "";
}

// ------------------------------------------------------------------- time --
bool parseTime(std::string_view s, uint32_t& ms) {
  while (!s.empty() && std::isspace((unsigned char)s.front()))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace((unsigned char)s.back()))
    s.remove_suffix(1);
  if (s.empty())
    return false;
  if (s[0] == '+')
    s.remove_prefix(1);
  // fraction: ".500" or ".1/3"
  uint32_t frac = 0;
  auto dot = s.find('.');
  if (dot != std::string_view::npos) {
    std::string f(s.substr(dot + 1));
    auto slash = f.find('/');
    if (slash != std::string::npos) {
      unsigned long a = strtoul(f.c_str(), nullptr, 10);
      unsigned long b = strtoul(f.c_str() + slash + 1, nullptr, 10);
      frac = b ? uint32_t(a * 1000 / b) : 0;
    } else {
      // ".5" = 500 ms, ".05" = 50 ms, ".500" = 500 ms
      uint32_t scale = 100;
      for (char c : f) {
        if (!std::isdigit((unsigned char)c))
          return false;
        frac += uint32_t(c - '0') * scale;
        scale /= 10;
      }
    }
    s = s.substr(0, dot);
  }
  uint64_t parts[3] = {0, 0, 0};
  int n = 0;
  size_t i = 0;
  while (i <= s.size()) {
    size_t c = s.find(':', i);
    if (c == std::string_view::npos)
      c = s.size();
    if (n >= 3 || c == i)
      return false;
    uint64_t v = 0;
    for (size_t k = i; k < c; k++) {
      if (!std::isdigit((unsigned char)s[k]))
        return false;
      v = v * 10 + uint64_t(s[k] - '0');
      if (v > 100000000ull)
        return false;
    }
    parts[n++] = v;
    i = c + 1;
  }
  uint64_t secs = 0;
  for (int k = 0; k < n; k++)
    secs = secs * 60 + parts[k];
  uint64_t r = secs * 1000 + frac;
  if (r > 0xFFFFFFFFull)
    return false;
  ms = uint32_t(r);
  return true;
}

std::string formatTime(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[24];
  snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600), unsigned(s / 60 % 60),
           unsigned(s % 60));
  return b;
}

// ------------------------------------------------------------------- DIDL --
DidlInfo parseDidl(std::string_view didl) {
  DidlInfo d;
  if (didl.empty())
    return d;
  d.title = xmlText(didl, "title");
  d.artist = xmlText(didl, "artist");
  if (d.artist.empty())
    d.artist = xmlText(didl, "creator");
  if (d.artist.empty())
    d.artist = xmlText(didl, "albumArtist");
  d.album = xmlText(didl, "album");
  d.artUri = xmlText(didl, "albumArtURI");
  d.protocolInfo = xmlAttr(didl, "res", "protocolInfo");
  std::string dur = xmlAttr(didl, "res", "duration");
  if (!dur.empty())
    parseTime(dur, d.durationMs);
  std::string size = xmlAttr(didl, "res", "size");
  if (!size.empty())
    d.size = strtoull(size.c_str(), nullptr, 10);
  // protocolInfo = "http-get:*:audio/flac:DLNA.ORG_..."
  size_t c1 = d.protocolInfo.find(':');
  size_t c2 = c1 == std::string::npos ? c1 : d.protocolInfo.find(':', c1 + 1);
  size_t c3 = c2 == std::string::npos ? c2 : d.protocolInfo.find(':', c2 + 1);
  if (c2 != std::string::npos)
    d.mime = d.protocolInfo.substr(c2 + 1, (c3 == std::string::npos
                                                 ? d.protocolInfo.size()
                                                 : c3) -
                                               c2 - 1);
  return d;
}

std::string makeDidl(const DidlInfo& d, const std::string& uri) {
  std::string s =
      "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
      "<item id=\"0\" parentID=\"-1\" restricted=\"1\">";
  s += "<dc:title>" + xmlEscape(d.title) + "</dc:title>";
  if (!d.artist.empty())
    s += "<upnp:artist>" + xmlEscape(d.artist) + "</upnp:artist>";
  if (!d.album.empty())
    s += "<upnp:album>" + xmlEscape(d.album) + "</upnp:album>";
  s += "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
  s += "<res protocolInfo=\"" +
       xmlEscape(d.protocolInfo.empty() ? "http-get:*:*:*" : d.protocolInfo) +
       "\"";
  if (d.durationMs)
    s += " duration=\"" + formatTime(d.durationMs) + "\"";
  s += ">" + xmlEscape(uri) + "</res></item></DIDL-Lite>";
  return s;
}

}  // namespace dlna
