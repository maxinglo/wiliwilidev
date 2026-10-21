#pragma once

#ifdef WILIWILI_UWP_HDR
#include <memory>
#include <optional>
#include <string>
#include "utils/hdr_output.hpp"

namespace bilibili { class DashMedia; }

namespace wiliwili {

struct UwpMediaInfo {
    unsigned width = 0, height = 0, audioChannels = 0, audioSampleRate = 0;
    uint64_t videoBitrate = 0, audioBitrate = 0, downloadBitrate = 0;
    double fps = 0, bufferingProgress = 0;
    std::string videoCodec, audioCodec, output;
    std::string format;
};

struct UwpPlaybackSnapshot {
    bool active = false, loaded = false, ended = false, failed = false;
    bool playing = false, paused = true, buffering = false;
    double position = 0, duration = 0, speed = 1;
    int volume = 100;
    unsigned width = 0, height = 0;
    std::string error, output;
};

class UwpVideoPlayer {
public:
    UwpVideoPlayer();
    ~UwpVideoPlayer();
    bool supports(HdrOutput output) const;
    bool active() const;
    double frameRate() const;
    double displayRefreshRate() const;
    void loadDash(const bilibili::DashMedia& video, const bilibili::DashMedia* audio,
                  double duration, int start, int end, bool autoplay);
    void stop();
    void suspend();
    void resumeDisplay();
    void pause(bool paused);
    void seek(double seconds);
    void speed(double rate);
    void setLooping(bool enabled);
    void setVolume(int value);
    void setBounds(float x, float y, float width, float height, float aspect, bool crop, bool stretch, bool mirror);
    void setVisible(bool visible);
    UwpPlaybackSnapshot takeSnapshot();
    std::optional<UwpMediaInfo> mediaInfo() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace wiliwili
#endif
