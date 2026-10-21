#ifdef WILIWILI_UWP_HDR
#include "utils/uwp_media_controls.hpp"
#include <borealis/core/logger.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace wiliwili {
using namespace winrt;
using namespace Windows::Media;

struct UwpMediaControls::Impl {
    SystemMediaTransportControls controls{nullptr};
    event_token button{}, seek{}, rate{};
    std::mutex mutex;
    std::vector<std::pair<UwpMediaCommand, double>> pending;
    std::function<void(UwpMediaCommand, double)> callback;
    std::string title, artist, artwork;
    int64_t lastSecond = -1;
    double lastDuration = -1;
    MediaPlaybackStatus lastStatus = MediaPlaybackStatus::Closed;
    void enqueue(UwpMediaCommand command, double value = 0) {
        std::lock_guard lock(mutex);
        pending.emplace_back(command, value);
    }
};

UwpMediaControls::UwpMediaControls(std::function<void(UwpMediaCommand, double)> callback)
    : impl(std::make_shared<Impl>()) {
    impl->callback = std::move(callback);
    try {
        auto& p = *impl;
        p.controls = SystemMediaTransportControls::GetForCurrentView();
        p.controls.IsEnabled(false);
        p.controls.IsPlayEnabled(true);
        p.controls.IsPauseEnabled(true);
        p.controls.IsStopEnabled(true);
        p.controls.IsFastForwardEnabled(true);
        p.controls.IsRewindEnabled(true);
        p.controls.IsNextEnabled(false);
        p.controls.IsPreviousEnabled(false);
        p.controls.PlaybackRate(1);
        std::weak_ptr<Impl> weak = impl;
        p.button = p.controls.ButtonPressed([weak](const auto&, const SystemMediaTransportControlsButtonPressedEventArgs& args) {
            if (auto state = weak.lock()) {
                switch (args.Button()) {
                    case SystemMediaTransportControlsButton::Play: state->enqueue(UwpMediaCommand::Play); break;
                    case SystemMediaTransportControlsButton::Pause: state->enqueue(UwpMediaCommand::Pause); break;
                    case SystemMediaTransportControlsButton::Stop: state->enqueue(UwpMediaCommand::Stop); break;
                    case SystemMediaTransportControlsButton::FastForward: state->enqueue(UwpMediaCommand::Forward); break;
                    case SystemMediaTransportControlsButton::Rewind: state->enqueue(UwpMediaCommand::Rewind); break;
                    default: break;
                }
            }
        });
        p.seek = p.controls.PlaybackPositionChangeRequested([weak](const auto&, const PlaybackPositionChangeRequestedEventArgs& args) {
            if (auto state = weak.lock()) state->enqueue(UwpMediaCommand::Seek,
                std::chrono::duration<double>(args.RequestedPlaybackPosition()).count());
        });
        p.rate = p.controls.PlaybackRateChangeRequested([weak](const auto&, const PlaybackRateChangeRequestedEventArgs& args) {
            if (auto state = weak.lock()) state->enqueue(UwpMediaCommand::Rate, args.RequestedPlaybackRate());
        });
        brls::Logger::info("Xbox system media controls registered");
    } catch (const hresult_error& e) {
        brls::Logger::warning("Xbox system media controls: {}", to_string(e.message()));
    }
}

UwpMediaControls::~UwpMediaControls() {
    if (!impl->controls) return;
    impl->controls.ButtonPressed(impl->button);
    impl->controls.PlaybackPositionChangeRequested(impl->seek);
    impl->controls.PlaybackRateChangeRequested(impl->rate);
    try { impl->controls.IsEnabled(false); } catch (...) {}
}

void UwpMediaControls::setMetadata(const std::string& title, const std::string& artist, const std::string& artwork) {
    auto& p = *impl;
    if (!p.controls || (p.title == title && p.artist == artist && p.artwork == artwork)) return;
    p.title = title;
    p.artist = artist;
    p.artwork = artwork;
    try {
        auto updater = p.controls.DisplayUpdater();
        updater.ClearAll();
        updater.Type(MediaPlaybackType::Video);
        updater.VideoProperties().Title(to_hstring(title));
        updater.VideoProperties().Subtitle(to_hstring(artist));
        if (artwork.rfind("http:", 0) == 0 || artwork.rfind("https:", 0) == 0 || artwork.rfind("//", 0) == 0) {
            std::string url = artwork;
            if (url.rfind("//", 0) == 0) url = "https:" + url;
            if (url.rfind("http:", 0) == 0) url.replace(0, 5, "https:");
            updater.Thumbnail(Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(
                Windows::Foundation::Uri(to_hstring(url))));
        }
        updater.Update();
    } catch (const hresult_error& e) { brls::Logger::warning("Xbox media metadata: {}", to_string(e.message())); }
}

void UwpMediaControls::update(bool stopped, bool playing, bool paused, double position, double duration, double rate) {
    auto& p = *impl;
    std::vector<std::pair<UwpMediaCommand, double>> commands;
    { std::lock_guard lock(p.mutex); commands.swap(p.pending); }
    for (const auto& [command, value] : commands) {
        brls::Logger::info("Xbox system media command: {}", static_cast<int>(command));
        p.callback(command, value);
    }
    if (!p.controls) return;
    try {
        const auto status = stopped ? MediaPlaybackStatus::Stopped : playing ? MediaPlaybackStatus::Playing
            : paused ? MediaPlaybackStatus::Paused : MediaPlaybackStatus::Changing;
        const bool changed = status != p.lastStatus;
        if (changed) {
            p.controls.PlaybackStatus(status);
            p.controls.IsEnabled(!stopped);
            p.lastStatus = status;
        }
        if (std::isfinite(rate) && rate > 0 && p.controls.PlaybackRate() != rate) p.controls.PlaybackRate(rate);
        if (!stopped && std::isfinite(position) && std::isfinite(duration) && duration > 0 &&
            (changed || static_cast<int64_t>(position) != p.lastSecond || duration != p.lastDuration)) {
            auto span = [](double value) { return std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
                std::chrono::duration<double>(std::max(0.0, value))); };
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(span(0));
            timeline.MinSeekTime(span(0));
            timeline.MaxSeekTime(span(duration));
            timeline.EndTime(span(duration));
            timeline.Position(span(std::clamp(position, 0.0, duration)));
            p.controls.UpdateTimelineProperties(timeline);
            p.lastSecond = static_cast<int64_t>(position);
            p.lastDuration = duration;
        }
    } catch (const hresult_error& e) { brls::Logger::warning("Xbox media state: {}", to_string(e.message())); }
}

} // namespace wiliwili
#endif
