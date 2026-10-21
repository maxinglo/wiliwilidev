#pragma once

#include <cstdint>
#include <string_view>
#include <stdexcept>

namespace wiliwili {
// Read only sample timing from a fragmented MP4. Compressed video and Dolby
// metadata are left untouched. A missing or incomplete timing table is unknown.
inline double fragmentFrameRate(std::string_view bytes, uint32_t timescale) {
    if (!timescale) return 0;
    try {
        auto u32 = [&](size_t p) -> uint32_t {
            if (p > bytes.size() || bytes.size() - p < 4) throw std::invalid_argument("Truncated MP4 timing");
            const auto* b = reinterpret_cast<const unsigned char*>(bytes.data() + p);
            return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        };
        auto boxes = [&](size_t begin, size_t end, auto visit) {
            for (size_t p = begin; p < end;) {
                if (end - p < 8) throw std::invalid_argument("Truncated MP4 box");
                uint64_t size = u32(p);
                size_t header = 8;
                if (size == 1) { size = (uint64_t(u32(p + 8)) << 32) | u32(p + 12); header = 16; }
                if (!size) size = end - p;
                if (size < header || size > end - p) throw std::invalid_argument("Invalid MP4 box");
                visit(bytes.substr(p + 4, 4), p + header, p + static_cast<size_t>(size));
                p += static_cast<size_t>(size);
            }
        };
        uint64_t samples = 0, ticks = 0;
        boxes(0, bytes.size(), [&](auto type, size_t begin, size_t end) {
            if (type != "moof") return;
            boxes(begin, end, [&](auto kind, size_t start, size_t limit) {
                if (kind != "traf") return;
                uint32_t defaultDuration = 0;
                boxes(start, limit, [&](auto name, size_t p, size_t last) {
                    if (name != "tfhd") return;
                    if (last - p < 8) throw std::invalid_argument("Truncated tfhd");
                    const auto flags = u32(p) & 0xffffff;
                    p += 8 + ((flags & 1) ? 8 : 0) + ((flags & 2) ? 4 : 0);
                    if (flags & 8) {
                        if (p > last || last - p < 4) throw std::invalid_argument("Truncated default duration");
                        defaultDuration = u32(p);
                    }
                });
                boxes(start, limit, [&](auto name, size_t p, size_t last) {
                    if (name != "trun") return;
                    if (last - p < 8) throw std::invalid_argument("Truncated trun");
                    const auto flags = u32(p) & 0xffffff;
                    const auto count = u32(p + 4);
                    p += 8 + ((flags & 1) ? 4 : 0) + ((flags & 4) ? 4 : 0);
                    const size_t stride = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) +
                        ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
                    if (p > last || (stride && count > (last - p) / stride)) throw std::invalid_argument("Truncated samples");
                    if (!(flags & 0x100)) {
                        if (!defaultDuration) throw std::invalid_argument("Unknown sample duration");
                        const auto duration = uint64_t(count) * defaultDuration;
                        if (duration > UINT64_MAX - ticks) throw std::invalid_argument("MP4 timing overflow");
                        ticks += duration;
                    } else {
                        for (uint32_t i = 0; i < count; ++i, p += stride) {
                            const auto duration = u32(p);
                            if (!duration) throw std::invalid_argument("Zero sample duration");
                            if (duration > UINT64_MAX - ticks) throw std::invalid_argument("MP4 timing overflow");
                            ticks += duration;
                        }
                    }
                    if (count > UINT64_MAX - samples) throw std::invalid_argument("MP4 sample count overflow");
                    samples += count;
                });
            });
        });
        return ticks && samples ? static_cast<double>(samples) * timescale / ticks : 0;
    } catch (const std::invalid_argument&) { return 0; }
}
} // namespace wiliwili
