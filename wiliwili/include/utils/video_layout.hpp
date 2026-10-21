#pragma once

#include <algorithm>
#include <cmath>

namespace wiliwili {

struct VideoRectangle { float x, y, width, height; };

inline VideoRectangle centerVideo(float x, float y, float width, float height, float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0) return {x, y, width, height};
    const float fittedWidth = std::min(width, height * aspect);
    const float fittedHeight = fittedWidth / aspect;
    return {x + (width - fittedWidth) / 2, y + (height - fittedHeight) / 2, fittedWidth, fittedHeight};
}

struct VideoSurfaceLayout {
    VideoRectangle visual;
    float scaleX, scaleY, offsetX, offsetY;
};

inline VideoSurfaceLayout layoutVideoSurface(float x, float y, float width, float height,
        float surfaceWidth, float surfaceHeight, float naturalAspect, float overrideAspect,
        bool crop, bool stretch) {
    const auto content = centerVideo(0, 0, surfaceWidth, surfaceHeight, naturalAspect);
    const auto visual = crop || stretch ? VideoRectangle{x, y, width, height}
        : centerVideo(x, y, width, height, overrideAspect > 0 ? overrideAspect : naturalAspect);
    float sx = visual.width / content.width, sy = visual.height / content.height;
    if (crop) sx = sy = std::max(sx, sy);
    return {visual, sx, sy, (visual.width - content.width * sx) / 2 - content.x * sx,
        (visual.height - content.height * sy) / 2 - content.y * sy};
}

} // namespace wiliwili
