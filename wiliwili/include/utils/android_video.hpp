#pragma once

#ifdef __ANDROID__
#include <cstdint>
#include <mpv/client.h>

// mpv's mediacodec_embed takes a JNI global reference to android.view.Surface.
void* androidAcquireVideoSurface();
void androidReleaseVideoSurface(void* surface);
bool androidSetVideoRotation(void* surface, int degrees);
void androidSetVideoPlayer(mpv_handle* player, bool nativeOutput);
bool androidVideoSuspendPending();
int androidTakeSuspendedPlayback();
bool androidSupportsHdr10();
uint64_t androidVideoSurfaceGeneration();
void androidSetVideoBounds(int x, int y, int width, int height, float aspect, bool crop, bool stretch);
#endif
