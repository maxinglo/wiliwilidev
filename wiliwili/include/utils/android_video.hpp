#pragma once

#ifdef __ANDROID__
#include <cstdint>

// mpv's mediacodec_embed takes a JNI global reference to android.view.Surface.
void* androidAcquireVideoSurface();
void androidReleaseVideoSurface(void* surface);
bool androidSetVideoRotation(void* surface, int degrees);
bool androidSupportsHdr10();
uint64_t androidVideoSurfaceGeneration();
void androidSetVideoBounds(int x, int y, int width, int height, float aspect, bool crop, bool stretch);
#endif
