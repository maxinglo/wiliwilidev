#pragma once

#include <cmath>
#include <algorithm>
#include <limits>
#include <vector>

namespace wiliwili {

enum class HdrOutput { Sdr, Hdr10, DolbyVision };

struct HdmiModeInfo {
    unsigned width, height, bitsPerPixel;
    double refreshRate;
    bool hdr10, dolbyVision;
};

inline bool compatibleXboxHdrStream(int quality, unsigned width, unsigned height) {
    if (quality != 125 && quality != 126) return true;
    if (!width || !height || std::min(width, height) > 2160) return false;
    // A 3996-wide Profile 5 MP4 plays on the system path. Do not infer a
    // narrower decoder limit from a failure in DASH codec negotiation.
    return std::max(width, height) <= 4096u;
}

// Xbox's media pipeline supports display modes up to 60 Hz. A Dolby Vision
// profile 5 stream must never be treated as an HDR10-compatible HEVC stream.
inline int chooseHdrMode(const std::vector<HdmiModeInfo>& modes, HdrOutput output,
                         unsigned width, unsigned height, double fps) {
    int best = -1;
    // Mode selection is independent of stream decoder compatibility.
    if (std::max(width, height) > 4096 || std::min(width, height) > 2160) return -1;
    double bestScore = std::numeric_limits<double>::max();
    for (size_t i = 0; i < modes.size(); ++i) {
        const auto& mode = modes[i];
        if (mode.refreshRate < 23 || mode.refreshRate > 60.1 || mode.width > 3840 || mode.height > 2160) continue;
        // HDR10 can use a 24-bpp YCbCr HDMI transport; the mode's HDR flag is
        // authoritative, rather than the effective transport bit count.
        if (output == HdrOutput::Hdr10 && !mode.hdr10) continue;
        // LLDV can use an 8-bit RGB HDMI transport with tunneled Dolby data.
        if (output == HdrOutput::DolbyVision && !mode.dolbyVision) continue;
        if (fps > 0 && mode.refreshRate + 0.1 < fps) continue;
        const double score = -(static_cast<double>(mode.width) * mode.height) + std::abs(60.0 - mode.refreshRate);
        if (score < bestScore) { best = static_cast<int>(i); bestScore = score; }
    }
    return best;
}

} // namespace wiliwili
