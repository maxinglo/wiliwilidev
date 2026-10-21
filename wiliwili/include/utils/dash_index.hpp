#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace wiliwili {
struct DashSegment {
    uint64_t offset, length, time, duration;
};
struct DashIndex {
    uint32_t timescale = 0;
    std::vector<DashSegment> segments;
};

// Earliest media time affected by a missing byte range, relative to the first
// segment. Initialization/index bytes alone do not block a media position.
inline std::optional<double> dashRangeStartTime(const DashIndex& index, uint64_t offset, uint64_t length) {
    if (!index.timescale || index.segments.empty() || !length || length > UINT64_MAX - offset) return std::nullopt;
    const auto last = offset + length;
    for (const auto& segment : index.segments) {
        if (segment.length > UINT64_MAX - segment.offset || segment.time < index.segments.front().time) return std::nullopt;
        if (offset < segment.offset + segment.length && last > segment.offset)
            return static_cast<double>(segment.time - index.segments.front().time) / index.timescale;
    }
    return std::nullopt;
}

// Parse ISO BMFF sidx without changing compressed samples, parameter sets,
// Dolby configuration, or HDR metadata. Reject nested indexes and overflow.
inline DashIndex parseDashIndex(std::string_view bytes, uint64_t fileOffset) {
    auto read32 = [&](size_t offset) -> uint32_t {
        if (offset > bytes.size() || bytes.size() - offset < 4) throw std::invalid_argument("Truncated DASH index");
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    auto read64 = [&](size_t offset) -> uint64_t { return (uint64_t(read32(offset)) << 32) | read32(offset + 4); };
    size_t box = 0;
    while (box < bytes.size()) {
        uint64_t size = read32(box);
        size_t header = 8;
        if (size == 1) { size = read64(box + 8); header = 16; }
        if (size < header || size > bytes.size() - box) throw std::invalid_argument("Invalid DASH index box size");
        if (bytes.substr(box + 4, 4) != "sidx") { box += static_cast<size_t>(size); continue; }
        size_t p = box + header;
        const auto version = read32(p) >> 24;
        if (version > 1 || size < header + (version ? 32 : 24)) throw std::invalid_argument("Unsupported DASH index version");
        DashIndex index;
        index.timescale = read32(p + 8);
        if (!index.timescale) throw std::invalid_argument("Invalid DASH index timescale");
        p += 12;
        uint64_t time = version ? read64(p) : read32(p);
        p += version ? 8 : 4;
        const uint64_t firstOffset = version ? read64(p) : read32(p);
        p += version ? 8 : 4;
        const auto count = read32(p) & 0xffff;
        p += 4;
        if (!count || count > (box + size - p) / 12) throw std::invalid_argument("Truncated DASH segment references");
        if (size > UINT64_MAX - fileOffset || box > UINT64_MAX - fileOffset - size || firstOffset > UINT64_MAX - fileOffset - size - box)
            throw std::invalid_argument("DASH index offset overflow");
        uint64_t offset = fileOffset + box + size + firstOffset;
        for (uint32_t i = 0; i < count; ++i, p += 12) {
            const auto reference = read32(p);
            const uint64_t length = reference & 0x7fffffff;
            const uint64_t duration = read32(p + 4);
            if ((reference & 0x80000000) || !length || !duration || length > UINT64_MAX - offset || duration > UINT64_MAX - time)
                throw std::invalid_argument("Unsupported or invalid DASH segment reference");
            index.segments.push_back({offset, length, time, duration});
            offset += length;
            time += duration;
        }
        return index;
    }
    throw std::invalid_argument("DASH sidx box missing");
}
} // namespace wiliwili
