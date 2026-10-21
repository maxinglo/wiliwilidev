#pragma once

#include <cmath>
#include <algorithm>

namespace wiliwili {
inline double canonicalFrameRate(double fps) {
    for (const double rate : {24000.0 / 1001, 30000.0 / 1001, 60000.0 / 1001, 120000.0 / 1001})
        if (std::abs(fps - rate) < 0.01) return rate;
    return fps;
}

// A fixed-refresh compositor needs UI updates on its own refresh grid. Video
// cadence is handled independently by the system media presenter.
inline unsigned displayUiDivisor(double refreshRate, double maximumRate, double minimumRate = 0) {
    if (!std::isfinite(refreshRate) || refreshRate <= 0) return 1;
    if (!std::isfinite(maximumRate) || maximumRate <= 0) return 1;
    auto divisor = static_cast<unsigned>(std::clamp(std::ceil(refreshRate / maximumRate - 0.0001), 1.0, 4.0));
    // Explicit user limits also respect the video floor. Nominal 60 fps media
    // is compatible with a 59.94 Hz output.
    while (divisor > 1 && std::isfinite(minimumRate) && refreshRate / divisor + 0.1 < minimumRate) --divisor;
    return divisor;
}

inline unsigned displayUiSwapInterval(double presentationRate, double targetRate) {
    if (!std::isfinite(presentationRate) || !std::isfinite(targetRate) || presentationRate <= 0 || targetRate <= 0) return 0;
    const auto interval = std::round(presentationRate / targetRate);
    if (interval < 1 || interval > 4 || std::abs(presentationRate - interval * targetRate) > presentationRate * 0.0005) return 0;
    return static_cast<unsigned>(interval);
}

} // namespace wiliwili
