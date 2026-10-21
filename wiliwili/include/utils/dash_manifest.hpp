#pragma once

#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <locale>
#include "utils/dash_index.hpp"

namespace wiliwili {

inline std::string escapeDashXml(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

inline bool validDashRange(const std::string& value) {
    const auto dash = value.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == value.size()) return false;
    uint64_t values[2] = {};
    size_t part = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == dash) { part = 1; continue; }
        if (value[i] < '0' || value[i] > '9') return false;
        const auto digit = static_cast<unsigned>(value[i] - '0');
        if (values[part] > (UINT64_MAX - digit) / 10) return false;
        values[part] = values[part] * 10 + digit;
    }
    return values[0] <= values[1];
}

inline bool validDashResponseRange(const std::string& header, uint64_t offset, uint64_t length, uint64_t bodyLength) {
    if (!length || bodyLength != length || length - 1 > UINT64_MAX - offset) return false;
    const auto last = offset + length - 1;
    const auto prefix = "bytes " + std::to_string(offset) + "-" + std::to_string(last) + "/";
    if (header.rfind(prefix, 0) != 0) return false;
    const auto total = header.substr(prefix.size());
    if (total == "*") return true;
    if (!validDashRange("0-" + total)) return false;
    return std::stoull(total) > last;
}

// Track is a DashMedia (kept generic so this serializer can be checked without
// linking the player or Windows Runtime). Byte ranges must come from the API;
// guessing them can discard dvcC/dvvC and produce incorrect Dolby Vision colors.
template <class Track>
std::string makeDashManifest(const Track& video, const Track* audio, double duration,
                             const DashIndex* videoIndex = nullptr, const DashIndex* audioIndex = nullptr) {
    if (!std::isfinite(duration) || duration <= 0) throw std::invalid_argument("Invalid DASH duration");
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        << "profiles=\"urn:mpeg:dash:profile:" << (videoIndex ? "isoff-main" : "isoff-on-demand") << ":2011\" minBufferTime=\"PT2S\" "
        << "mediaPresentationDuration=\"PT" << duration << "S\"><Period>";
    auto track = [&](const Track& media, bool isVideo, const DashIndex* index) {
        if (media.base_url.empty() || media.codecs.empty() || !media.bandwidth ||
            !validDashRange(media.initialization) || !validDashRange(media.index_range))
            throw std::invalid_argument("Incomplete DASH track description");
        xml << "<AdaptationSet contentType=\"" << (isVideo ? "video" : "audio")
            << "\" mimeType=\"" << (isVideo ? "video/mp4" : "audio/mp4")
            << "\" segmentAlignment=\"true\"><Representation id=\"" << (isVideo ? "v" : "a")
            << "\" bandwidth=\"" << media.bandwidth << "\" codecs=\"" << escapeDashXml(media.codecs) << '"';
        if (isVideo) xml << " width=\"" << media.width << "\" height=\"" << media.height << '"';
        // Bilibili can report a decimal/estimated frame rate. DASH requires an
        // integer or rational; the MP4 decoder can derive the actual rate.
        xml << "><BaseURL>" << escapeDashXml(media.base_url) << "</BaseURL>";
        for (const auto& backup : media.backup_url)
            if (!backup.empty()) xml << "<BaseURL>" << escapeDashXml(backup) << "</BaseURL>";
        if (index) {
            if (!index->timescale || index->segments.empty()) throw std::invalid_argument("Empty DASH segment index");
            xml << "<SegmentList timescale=\"" << index->timescale << "\"><Initialization range=\"" << media.initialization << "\"/><SegmentTimeline>";
            for (const auto& segment : index->segments)
                xml << "<S t=\"" << segment.time << "\" d=\"" << segment.duration << "\"/>";
            xml << "</SegmentTimeline>";
            for (const auto& segment : index->segments)
                xml << "<SegmentURL mediaRange=\"" << segment.offset << '-' << segment.offset + segment.length - 1 << "\"/>";
            xml << "</SegmentList>";
        } else {
            xml << "<SegmentBase indexRange=\"" << media.index_range << "\"><Initialization range=\""
                << media.initialization << "\"/></SegmentBase>";
        }
        xml << "</Representation></AdaptationSet>";
    };
    track(video, true, videoIndex);
    if (audio) track(*audio, false, audioIndex);
    xml << "</Period></MPD>";
    return xml.str();
}

} // namespace wiliwili
