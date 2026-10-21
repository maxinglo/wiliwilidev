#pragma once

#ifdef WILIWILI_UWP_HDR
#include <functional>
#include <memory>
#include <string>

namespace wiliwili {
enum class UwpMediaCommand { Play, Pause, Stop, Seek, Forward, Rewind, Rate };

class UwpMediaControls {
public:
    explicit UwpMediaControls(std::function<void(UwpMediaCommand, double)> callback);
    ~UwpMediaControls();
    void update(bool stopped, bool playing, bool paused, double position, double duration, double rate);
    void setMetadata(const std::string& title, const std::string& artist, const std::string& artwork);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
} // namespace wiliwili
#endif
