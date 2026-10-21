#ifdef WILIWILI_UWP_HDR
#include "utils/uwp_player.hpp"
#include "utils/dash_manifest.hpp"
#include "utils/video_layout.hpp"
#include "utils/mp4_fragment.hpp"
#include "utils/frame_pacing.hpp"
#include "utils/uwp_range_stream.hpp"
#include "api/bilibili/result/video_detail_result.h"
#include "api/bilibili/util/http.hpp"
#include <borealis/core/logger.hpp>
#include <borealis/platforms/driver/d3d11.hpp>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Streaming.Adaptive.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <map>
#include <unordered_map>
#include <utility>

extern std::unique_ptr<brls::D3D11Context> D3D11_CONTEXT;

namespace wiliwili {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display::Core;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Media::Streaming::Adaptive;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Composition;
using namespace Windows::Web::Http;

struct UwpVideoPlayer::Impl : std::enable_shared_from_this<Impl> {
    MediaPlayer player{nullptr};
    MediaPlayer audioPlayer{nullptr};
    Windows::Media::MediaTimelineController timeline{nullptr};
    MediaPlaybackItem playbackItem{nullptr};
    MediaPlaybackItem audioPlaybackItem{nullptr};
    IRandomAccessStream videoRangeStream{nullptr}, audioRangeStream{nullptr};
    MediaPlayerSurface surface{nullptr};
    SpriteVisual visual{nullptr};
    CompositionSurfaceBrush brush{nullptr};
    ContainerVisual root{nullptr};
    HdmiDisplayInformation hdmi{nullptr};
    AdaptiveMediaSource adaptive{nullptr};
    InMemoryRandomAccessStream manifest{nullptr};
    HttpClient http{nullptr};
    std::atomic<uint64_t> generation{0};
    std::mutex eventMutex;
    bool isActive = false, loaded = false, ended = false, failed = false;
    bool hdr10 = false, dolby = false, displayReady = false, visible = false;
    bool autoplay = true, suspended = false;
    int start = 0, end = 0;
    double sourceDuration = 0;
    unsigned videoWidth = 0, videoHeight = 0;
    unsigned surfaceWidth = 0, surfaceHeight = 0;
    unsigned displayWidth = 0, displayHeight = 0;
    VideoRectangle lastBounds{-1, -1, -1, -1};
    bool lastMirror = false, lastCrop = false;
    bool mediaOpened = false;
    bool bufferingStatsWarning = false;
    UwpMediaInfo info;
    std::shared_ptr<const std::vector<bilibili::DashMedia>> tracks;
    bool useDirectSource = false;
    bool videoOpened = false, audioOpened = false;
    bool videoBuffering = false, audioBuffering = false;
    bool videoSeeking = false, audioSeeking = false;
    bool transportRunning = false;
    std::optional<double> pendingSeek, seekingPosition;
    struct PendingData {
        double time;
        std::chrono::steady_clock::time_point since;
    };
    std::map<uint64_t, PendingData> videoReads, audioReads;
    bool dataBlocked = false;
    std::optional<std::chrono::steady_clock::time_point> readySince;
    std::atomic<bool> looping{false};
    std::mutex routeMutex;
    std::unordered_map<std::string, std::string> preferredCdn;
    std::atomic<uint64_t> lastDownloadBitrate{0};
    double fps = 0;
    std::atomic<double> measuredFrameRate{0};
    std::atomic<uint32_t> videoTimescale{0};
    double displayRate = 0;
    HdrOutput output = HdrOutput::Sdr;
    std::string error, outputName;
    IAsyncAction displayTask{nullptr};

    // MediaTimelineController does not necessarily stall when a seekable MP4
    // range stream is waiting for data. Own the run/hold decision on the UI
    // thread; callbacks only publish readiness under eventMutex.
    void watchSession(const MediaPlayer& target, bool video, uint64_t token) {
        const auto session = target.PlaybackSession();
        std::weak_ptr<Impl> weak = shared_from_this();
        session.BufferingStarted([weak, token, video](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && token == self->generation) {
                std::lock_guard lock(self->eventMutex);
                if (token != self->generation) return;
                (video ? self->videoBuffering : self->audioBuffering) = true;
            }
        });
        session.BufferingEnded([weak, token, video](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && token == self->generation) {
                std::lock_guard lock(self->eventMutex);
                if (token != self->generation) return;
                (video ? self->videoBuffering : self->audioBuffering) = false;
            }
        });
        session.SeekCompleted([weak, token, video](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && token == self->generation) {
                std::lock_guard lock(self->eventMutex);
                if (token != self->generation) return;
                (video ? self->videoSeeking : self->audioSeeking) = false;
            }
        });
    }

    void holdTransport() {
        if (timeline) timeline.Pause();
        else if (player && transportRunning) player.Pause();
        transportRunning = false;
    }

    bool updateTransport() {
        const auto now = std::chrono::steady_clock::now();
        bool opened, seeking;
        {
            std::lock_guard lock(eventMutex);
            opened = videoOpened && (!audioPlayer || audioOpened);
            seeking = videoSeeking || audioSeeking;
        }
        // Finish the in-flight seek before applying the latest requested one,
        // so an older SeekCompleted cannot release a newer seek's barrier.
        if (opened && pendingSeek && !seeking) {
            holdTransport();
            const auto seconds = *pendingSeek;
            const auto position = std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(seconds));
            const bool videoMove = std::abs(std::chrono::duration<double>(player.PlaybackSession().Position()).count() - seconds) > 0.001;
            const bool audioMove = audioPlayer && std::abs(std::chrono::duration<double>(audioPlayer.PlaybackSession().Position()).count() - seconds) > 0.001;
            {
                std::lock_guard lock(eventMutex);
                videoSeeking = videoMove;
                audioSeeking = audioMove;
            }
            seekingPosition = seconds;
            pendingSeek.reset();
            if (timeline) timeline.Position(position);
            else player.PlaybackSession().Position(position);
        }
        bool ready;
        const auto position = seekingPosition ? *seekingPosition :
            std::chrono::duration<double>(timeline ? timeline.Position() : player.PlaybackSession().Position()).count();
        {
            std::lock_guard lock(eventMutex);
            // Xbox can keep reporting Playing while a seekable MP4 stream
            // has no video samples. Stop before the affected SIDX segment,
            // independently of the runtime's late BufferingStarted event.
            const auto missing = [&](const auto& reads) {
                return std::any_of(reads.begin(), reads.end(), [&](const auto& read) {
                    return position + 0.05 >= read.second.time && now - read.second.since >= std::chrono::milliseconds(200);
                });
            };
            dataBlocked = missing(videoReads) || missing(audioReads);
            ready = opened && !videoBuffering && !videoSeeking &&
                (!audioPlayer || (!audioBuffering && !audioSeeking)) && !dataBlocked;
            if (!videoSeeking && !audioSeeking) seekingPosition.reset();
        }
        const auto available = [](const MediaPlayer& media) {
            const auto state = media.PlaybackSession().PlaybackState();
            return state == MediaPlaybackState::Paused || state == MediaPlaybackState::Playing;
        };
        ready = ready && available(player) && (!audioPlayer || available(audioPlayer));
        if (!ready || !displayReady) readySince.reset();
        else if (!readySince) readySince = now;
        // Let queued buffering/seek notifications and the decoder's preroll
        // settle before releasing both tracks together.
        const bool settled = readySince && now - *readySince >= std::chrono::milliseconds(200);
        const bool run = ready && settled && autoplay && displayReady && !suspended;
        if (transportRunning && !run) holdTransport();
        else if (!transportRunning && run) {
            if (timeline) timeline.Resume();
            else player.Play();
            transportRunning = true;
        }
        return !ready || !displayReady || (autoplay && !suspended && !settled);
    }

    static std::string readRange(const std::shared_ptr<Impl>& self, uint64_t token, const bilibili::DashMedia& track,
                                 uint64_t offset, uint64_t length, bool video, const std::string& requested,
                                 uint64_t* resourceSize = nullptr) {
            if (!length || length > 64 * 1024 * 1024 || length - 1 > UINT64_MAX - offset)
                throw hresult_error(E_INVALIDARG, L"Invalid or oversized DASH segment range");
            std::vector<std::string> candidates{requested};
            for (const auto& url : [&]() { auto urls = track.backup_url; urls.insert(urls.begin(), track.base_url); return urls; }())
                if (!url.empty() && std::find(candidates.begin(), candidates.end(), url) == candidates.end()) candidates.push_back(url);
            {
                std::lock_guard lock(self->routeMutex);
                if (const auto found = self->preferredCdn.find(track.base_url); found != self->preferredCdn.end()) {
                    candidates.erase(std::remove(candidates.begin(), candidates.end(), found->second), candidates.end());
                    candidates.insert(candidates.begin(), found->second);
                }
            }
            for (const auto& url : candidates) {
                if (token != self->generation) break;
                auto session = bilibili::HTTP::createSession();
                session->SetUrl(cpr::Url{url});
                session->SetTimeout(cpr::Timeout{30000});
                session->SetConnectTimeout(cpr::ConnectTimeout{5000});
                const auto range = std::to_string(offset) + "-" + std::to_string(offset + length - 1);
                curl_easy_setopt(session->GetCurlHolder()->handle, CURLOPT_RANGE, range.c_str());
                session->SetProgressCallback(cpr::ProgressCallback([self, token](...) { return token == self->generation; }));
                std::string bytes;
                const auto started = std::chrono::steady_clock::now();
                auto response = session->Download(cpr::WriteCallback([&bytes, length, self, token](const auto& data, intptr_t) {
                    if (token != self->generation) return false;
                    if (data.size() > length - bytes.size()) return false;
                    bytes.append(data.data(), data.size());
                    return true;
                }));
                if (token != self->generation) break;
                if (response.error || response.status_code != 206 || !validDashResponseRange(response.header["Content-Range"], offset, length, bytes.size())) {
                    brls::Logger::warning("Xbox DASH range retry: track={}, host={}, HTTP={}, curl={}, requested={}+{}, received={}",
                        video ? "video" : "audio", to_string(Uri(to_hstring(url)).Host()), response.status_code,
                        static_cast<int>(response.error.code), offset, length, bytes.size());
                    continue;
                }
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (resourceSize) {
                    const auto& range = response.header["Content-Range"];
                    const auto slash = range.find('/');
                    if (slash == std::string::npos || range.substr(slash + 1) == "*")
                        throw hresult_error(E_FAIL, L"MP4 source did not report its length");
                    *resourceSize = std::stoull(range.substr(slash + 1));
                }
                self->lastDownloadBitrate = static_cast<uint64_t>(bytes.size() * 8 / std::max(0.001, seconds));
                {
                    std::lock_guard lock(self->routeMutex);
                    auto& preferred = self->preferredCdn[track.base_url];
                    if (preferred != url && url != requested) brls::Logger::info("Xbox DASH selected backup CDN: {}", to_string(Uri(to_hstring(url)).Host()));
                    preferred = url;
                }
                return bytes;
            }
            if (token != self->generation) throw hresult_canceled();
            throw hresult_error(E_FAIL, L"DASH CDN range download failed on all available URLs");
    }

    static IAsyncOperation<hstring> indexedManifest(std::shared_ptr<Impl> self, uint64_t token, double duration) {
        const auto tracks = self->tracks;
        co_await resume_background();
        std::vector<DashIndex> indexes;
        for (size_t i = 0; i < tracks->size(); ++i) {
            const auto& track = tracks->at(i);
            const auto split = track.index_range.find('-');
            const auto offset = std::stoull(track.index_range.substr(0, split));
            const auto last = std::stoull(track.index_range.substr(split + 1));
            if (last < offset || last - offset >= 1024 * 1024) throw hresult_error(E_INVALIDARG, L"Invalid DASH index range");
            const auto bytes = readRange(self, token, track, offset, last - offset + 1, i == 0, track.base_url);
            indexes.push_back(parseDashIndex(bytes, offset));
            if (i == 0) self->videoTimescale = indexes.back().timescale;
            brls::Logger::info("Xbox DASH index: {} segments={}, timescale={}", i == 0 ? "video" : "audio", indexes.back().segments.size(), indexes.back().timescale);
        }
        co_return to_hstring(makeDashManifest(tracks->at(0), tracks->size() > 1 ? &tracks->at(1) : nullptr, duration,
            &indexes[0], indexes.size() > 1 ? &indexes[1] : nullptr));
    }

    static fire_and_forget serveRange(std::shared_ptr<Impl> self, uint64_t token,
            AdaptiveMediaSourceDownloadRequestedEventArgs args, AdaptiveMediaSourceDownloadRequestedDeferral deferral,
            bilibili::DashMedia track, bool video) {
        co_await resume_background();
        try {
            const uint64_t offset = args.ResourceByteRangeOffset() ? args.ResourceByteRangeOffset().Value() : 0;
            const uint64_t length = args.ResourceByteRangeLength() ? args.ResourceByteRangeLength().Value() : 0;
            const auto bytes = readRange(self, token, track, offset, length, video, to_string(args.ResourceUri().AbsoluteUri()));
            if (video && self->measuredFrameRate == 0) {
                const auto rate = fragmentFrameRate(bytes, self->videoTimescale.load());
                if (rate > 0 && rate <= 240) {
                    self->measuredFrameRate = canonicalFrameRate(rate);
                    brls::Logger::info("Xbox MP4 sample cadence: {:.6f} fps", self->measuredFrameRate.load());
                }
            }
            if (token == self->generation) {
                DataWriter writer;
                writer.WriteBytes(array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size())));
                args.Result().Buffer(writer.DetachBuffer());
                args.Result().ContentType(video ? L"video/mp4" : L"audio/mp4");
                args.Result().ResourceByteRangeOffset(box_value(offset).as<IReference<uint64_t>>());
                args.Result().ResourceByteRangeLength(box_value(length).as<IReference<uint64_t>>());
                brls::Logger::info("Xbox DASH range delivered: {} {}+{}", video ? "video" : "audio", offset, length);
            }
        } catch (const hresult_error& e) {
            if (token == self->generation) self->fail(to_string(e.message()));
        } catch (const std::exception& e) {
            if (token == self->generation) self->fail(e.what());
        }
        try { deferral.Complete(); } catch (...) {}
    }

    void fail(const std::string& message) {
        std::lock_guard lock(eventMutex);
        failed = true;
        error = message;
        brls::Logger::error("Xbox native video: {}", message);
    }

    void initialize() {
        if (Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() != L"Windows.Xbox") return;
        try {
            hdmi = HdmiDisplayInformation::GetForCurrentView();
            if (!hdmi) return;
            try { displayRate = hdmi.GetCurrentDisplayMode().RefreshRate(); }
            catch (const hresult_error&) { /* Capability enumeration can still succeed. */ }
            for (const auto& mode : hdmi.GetSupportedDisplayModes()) {
                brls::Logger::info("Xbox HDMI mode: {}x{}@{}, bpp={}, HDR10={}, LLDV={}",
                    mode.ResolutionWidthInRawPixels(), mode.ResolutionHeightInRawPixels(), mode.RefreshRate(),
                    mode.BitsPerPixel(), mode.IsSmpte2084Supported(), mode.IsDolbyVisionLowLatencySupported());
                if (mode.RefreshRate() < 23 || mode.RefreshRate() > 60.1) continue;
                hdr10 |= mode.IsSmpte2084Supported();
                dolby |= mode.IsDolbyVisionLowLatencySupported();
            }
            brls::Logger::info("Xbox HDMI capabilities: HDR10={}, Dolby Vision={}", hdr10, dolby);
        } catch (const hresult_error& e) {
            brls::Logger::warning("Xbox HDR capability check failed: {}", to_string(e.message()));
        }
    }

    // All HDMI requests are serialized, including requests made obsolete by a
    // quality change. Otherwise an old completion could reset the new HDR mode.
    static IAsyncAction changeDisplay(std::shared_ptr<Impl> self, uint64_t token, bool restore, IAsyncAction previous) {
        try {
            if (previous) co_await previous;
            if (token != self->generation) co_return;
            if (restore) {
                if (self->hdmi) co_await self->hdmi.SetDefaultDisplayModeAsync();
                try { if (self->hdmi) self->displayRate = self->hdmi.GetCurrentDisplayMode().RefreshRate(); }
                catch (const hresult_error&) { /* Keep the last known rate. */ }
                self->displayReady = false;
                brls::Logger::info("Xbox default display mode restored");
                co_return;
            }
            auto supported = self->hdmi.GetSupportedDisplayModes();
            std::vector<HdmiModeInfo> modes;
            for (const auto& mode : supported)
                modes.push_back({mode.ResolutionWidthInRawPixels(), mode.ResolutionHeightInRawPixels(),
                    mode.BitsPerPixel(), mode.RefreshRate(), mode.IsSmpte2084Supported(), mode.IsDolbyVisionLowLatencySupported()});
            const int index = chooseHdrMode(modes, self->output, self->videoWidth, self->videoHeight, self->fps);
            if (index < 0) throw hresult_error(E_NOTIMPL, L"No compatible HDMI HDR mode (maximum 4K/60 Hz)");
            const auto option = self->output == HdrOutput::DolbyVision
                ? HdmiDisplayHdrOption::DolbyVisionLowLatency : HdmiDisplayHdrOption::Eotf2084;
            const bool changed = co_await self->hdmi.RequestSetCurrentDisplayModeAsync(supported.GetAt(index), option);
            if (token != self->generation) co_return;
            if (!changed) throw hresult_error(E_FAIL, L"The Xbox/TV rejected the HDR display mode");
            self->displayReady = true;
            self->displayRate = modes[index].refreshRate;
            self->outputName = self->output == HdrOutput::DolbyVision ? "Dolby Vision (HDMI LLDV)" : "HDR10 (BT.2020/PQ)";
            brls::Logger::info("Xbox HDMI mode selected: {} {}x{}@{}", self->outputName,
                modes[index].width, modes[index].height, modes[index].refreshRate);
            // The render-loop readiness barrier starts playback once the
            // display, video and audio are all ready.
        } catch (const hresult_error& e) {
            if (token == self->generation && !restore) self->fail(to_string(e.message()));
            else brls::Logger::warning("Xbox display restore: {}", to_string(e.message()));
        }
    }

    static IAsyncAction open(std::shared_ptr<Impl> self, uint64_t token, std::string xml, double duration, IAsyncAction previous) {
        // The packaged MPD preserves the original dvcC/dvvC, RPU NAL units and
        // HDR10 SEI boxes: no FFmpeg decode, RGB copy, or codec relabeling.
        try {
            if (previous) co_await previous;
            if (token != self->generation) co_return;
            co_await changeDisplay(self, token, false, nullptr);
            if (token != self->generation) co_return;
            if (!self->displayReady && !self->suspended) co_return;
            if (self->useDirectSource) {
                const apartment_context ui;
                const auto tracks = self->tracks;
                std::vector<IRandomAccessStream> streams;
                co_await resume_background();
                for (size_t i = 0; i < tracks->size(); ++i) {
                    const auto track = tracks->at(i);
                    uint64_t size = 0;
                    const auto initLast = std::stoull(track.initialization.substr(track.initialization.find('-') + 1));
                    const auto split = track.index_range.find('-');
                    const auto indexFirst = std::stoull(track.index_range.substr(0, split));
                    const auto indexLast = std::stoull(track.index_range.substr(split + 1));
                    if (indexLast < indexFirst || std::max(initLast, indexLast) >= 1024 * 1024)
                        throw hresult_error(E_INVALIDARG, L"Invalid MP4 initialization/index range");
                    const auto header = readRange(self, token, track, 0, std::max(initLast, indexLast) + 1, i == 0, track.base_url, &size);
                    const auto index = std::make_shared<const DashIndex>(parseDashIndex(
                        std::string_view(header).substr(indexFirst, indexLast - indexFirst + 1), indexFirst));
                    if (i == 0) self->videoTimescale = index->timescale;
                    std::weak_ptr<Impl> weak = self;
                    auto cache = std::make_shared<RangeBlockCache>(size,
                        [weak, token, track, index, video = i == 0](uint64_t offset, uint64_t length) {
                            const auto state = weak.lock();
                            if (!state || token != state->generation) throw hresult_canceled();
                            const auto time = dashRangeStartTime(*index, offset, length);
                            if (time) {
                                std::lock_guard lock(state->eventMutex);
                                if (token != state->generation) throw hresult_canceled();
                                (video ? state->videoReads : state->audioReads).insert_or_assign(offset, PendingData{*time, std::chrono::steady_clock::now()});
                            }
                            const auto complete = [&] {
                                std::lock_guard lock(state->eventMutex);
                                if (token == state->generation) (video ? state->videoReads : state->audioReads).erase(offset);
                            };
                            try {
                                auto bytes = readRange(state, token, track, offset, length, video, track.base_url);
                                complete();
                                return bytes;
                            } catch (...) { complete(); throw; }
                        }, 1024 * 1024, i == 0 ? 32 : 4);
                    streams.push_back(makeUwpRangeStream(std::move(cache), (i == 0 ? 16ull : 1ull) * 1024 * 1024));
                }
                co_await ui;
                if (token != self->generation) co_return;
                self->videoRangeStream = streams[0];
                if (streams.size() > 1) {
                    self->timeline = Windows::Media::MediaTimelineController();
                    self->timeline.Pause();
                    self->audioPlayer = MediaPlayer();
                    self->audioPlayer.AutoPlay(false);
                    self->audioPlayer.CommandManager().IsEnabled(false);
                    self->audioPlayer.Volume(self->player.Volume());
                    self->watchSession(self->audioPlayer, false, token);
                    self->player.TimelineController(self->timeline);
                    self->audioPlayer.TimelineController(self->timeline);
                    self->timeline.Duration(box_value(std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(duration))).as<IReference<TimeSpan>>());
                    self->timeline.IsLoopingEnabled(self->looping);
                    std::weak_ptr<Impl> weak = self;
                    self->timeline.Ended([weak, token](const auto&, const auto&) {
                        if (const auto state = weak.lock(); state && token == state->generation && !state->looping) {
                            std::lock_guard lock(state->eventMutex);
                            if (token != state->generation) return;
                            state->ended = true;
                            brls::Logger::info("Xbox synchronized timeline ended");
                        }
                    });
                    self->timeline.Failed([weak, token](const auto&, const Windows::Media::MediaTimelineControllerFailedEventArgs& args) {
                        if (const auto state = weak.lock(); state && token == state->generation)
                            state->fail(fmt::format("系统播放时间轴失败 ({:#010x})", static_cast<uint32_t>(args.ExtendedError().value)));
                    });
                    self->audioPlayer.MediaOpened([weak, token](const auto&, const auto&) {
                        if (const auto state = weak.lock(); state && token == state->generation) {
                            std::lock_guard lock(state->eventMutex);
                            if (token != state->generation) return;
                            state->audioOpened = true;
                            state->loaded = state->videoOpened;
                            brls::Logger::info("Xbox synchronized MP4 audio opened");
                        }
                    });
                    self->audioPlayer.MediaFailed([weak, token](const auto&, const MediaPlayerFailedEventArgs& args) {
                        if (const auto state = weak.lock(); state && token == state->generation)
                            state->fail(fmt::format("系统音轨失败 ({:#010x})", static_cast<uint32_t>(args.ExtendedErrorCode().value)));
                    });
                    self->audioRangeStream = streams[1];
                    self->audioPlaybackItem = MediaPlaybackItem(MediaSource::CreateFromStream(streams[1], L"audio/mp4"));
                    self->audioPlayer.Source(self->audioPlaybackItem);
                }
                self->playbackItem = MediaPlaybackItem(MediaSource::CreateFromStream(streams[0], L"video/mp4"));
                self->player.Source(self->playbackItem);
                brls::Logger::info("Xbox Profile 5 original MP4 range streams: audio={}, timeline={}", streams.size() > 1, self->timeline != nullptr);
                co_return;
            }
            xml = to_string(co_await indexedManifest(self, token, duration));
            if (token != self->generation) co_return;
            auto stream = InMemoryRandomAccessStream();
            DataWriter writer(stream);
            writer.WriteString(to_hstring(xml));
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            auto result = co_await AdaptiveMediaSource::CreateFromStreamAsync(stream, Uri(L"https://www.bilibili.com/"),
                L"application/dash+xml", self->http);
            if (token != self->generation) co_return;
            if (result.Status() != AdaptiveMediaSourceCreationStatus::Success)
                throw hresult_error(E_FAIL, L"The system could not open the DASH manifest");
            self->manifest = stream;
            self->adaptive = result.MediaSource();
            self->adaptive.DownloadFailed([](const auto&, const AdaptiveMediaSourceDownloadFailedEventArgs& args) {
                const auto response = args.HttpResponseMessage();
                brls::Logger::error("Xbox DASH download failed: HTTP={}, error={:#010x}",
                    response ? static_cast<int>(response.StatusCode()) : 0, static_cast<uint32_t>(args.ExtendedError().value));
            });
            std::weak_ptr<Impl> weak = self;
            const auto tracks = self->tracks;
            self->adaptive.DownloadRequested([weak, token, tracks](const auto&, const AdaptiveMediaSourceDownloadRequestedEventArgs& args) {
                const auto requested = to_string(args.ResourceUri().AbsoluteUri());
                for (size_t i = 0; i < tracks->size(); ++i) {
                    const auto& track = tracks->at(i);
                    if (requested != track.base_url && std::find(track.backup_url.begin(), track.backup_url.end(), requested) == track.backup_url.end()) continue;
                    auto deferral = args.GetDeferral();
                    if (auto state = weak.lock()) serveRange(state, token, args, deferral, track, i == 0);
                    else deferral.Complete();
                    break;
                }
            });
            self->playbackItem = MediaPlaybackItem(MediaSource::CreateFromAdaptiveMediaSource(self->adaptive));
            self->player.Source(self->playbackItem);
        } catch (const hresult_error& e) {
            if (token == self->generation) self->fail(fmt::format("{} ({:#010x})", to_string(e.message()), static_cast<uint32_t>(e.code().value)));
        } catch (const std::exception& e) {
            if (token == self->generation) self->fail(e.what());
        }
    }

    void releasePlayer() {
        if (visual && root) root.Children().Remove(visual);
        visual = nullptr;
        root = nullptr;
        brush = nullptr;
        if (timeline) timeline.Pause();
        if (audioPlayer) { audioPlayer.Source(nullptr); audioPlayer.TimelineController(nullptr); audioPlayer.Close(); }
        audioPlayer = nullptr;
        audioPlaybackItem = nullptr;
        if (player) {
            if (!timeline) player.Pause();
            player.Source(nullptr);
            if (timeline) player.TimelineController(nullptr);
        }
        timeline = nullptr;
        playbackItem = nullptr;
        if (surface) surface.Close();
        surface = nullptr;
        if (player) player.Close();
        player = nullptr;
        if (videoRangeStream) videoRangeStream.Close();
        if (audioRangeStream) audioRangeStream.Close();
        videoRangeStream = audioRangeStream = nullptr;
        if (adaptive) adaptive.Close();
        adaptive = nullptr;
        manifest = nullptr;
        http = nullptr;
        root = nullptr;
    }
};

UwpVideoPlayer::UwpVideoPlayer() : impl(std::make_shared<Impl>()) { impl->initialize(); }
UwpVideoPlayer::~UwpVideoPlayer() { stop(); }
bool UwpVideoPlayer::supports(HdrOutput output) const {
    return output == HdrOutput::DolbyVision ? impl->dolby : output == HdrOutput::Hdr10 && impl->hdr10;
}
bool UwpVideoPlayer::active() const { return impl->isActive; }
double UwpVideoPlayer::frameRate() const { return impl->measuredFrameRate > 0 ? impl->measuredFrameRate.load() : impl->fps; }
double UwpVideoPlayer::displayRefreshRate() const { return impl->displayRate; }



void UwpVideoPlayer::loadDash(const bilibili::DashMedia& video, const bilibili::DashMedia* audio,
                         double duration, int start, int end, bool autoplay) {
    stop();
    auto& p = *impl;
    p.isActive = true;
    p.start = start;
    p.end = end;
    p.sourceDuration = std::isfinite(duration) && duration > 0 ? duration : 0;
    p.autoplay = autoplay;
    p.suspended = false;
    p.videoWidth = video.width;
    p.videoHeight = video.height;
    p.displayWidth = video.width;
    p.displayHeight = video.height;
    p.output = video.id == 126 ? HdrOutput::DolbyVision : HdrOutput::Hdr10;
    p.outputName = "pending";
    p.mediaOpened = false;
    {
        std::lock_guard lock(p.eventMutex);
        p.videoOpened = p.audioOpened = false;
        p.videoBuffering = p.audioBuffering = false;
        p.videoSeeking = p.audioSeeking = false;
        p.videoReads.clear();
        p.audioReads.clear();
    }
    p.transportRunning = false;
    p.dataBlocked = false;
    p.readySince.reset();
    p.pendingSeek = start > 0 ? std::optional<double>(start) : std::nullopt;
    p.seekingPosition.reset();
    p.looping = false;
    p.bufferingStatsWarning = false;
    p.lastDownloadBitrate = 0;
    p.measuredFrameRate = 0;
    p.videoTimescale = 0;
    { std::lock_guard lock(p.routeMutex); p.preferredCdn.clear(); }
    p.lastBounds = {-1, -1, -1, -1};
    p.info = {};
    p.info.width = video.width;
    p.info.height = video.height;
    p.info.videoCodec = video.codecs;
    p.useDirectSource = (video.codecs.rfind("dvh1.05.", 0) == 0 || video.codecs.rfind("dvhe.05.", 0) == 0);
    p.info.format = p.useDirectSource ? "MP4" : "MPEG-DASH";
    p.info.videoBitrate = video.bandwidth;
    if (audio) { p.info.audioCodec = audio->codecs; p.info.audioBitrate = audio->bandwidth; }
    auto tracks = std::make_shared<std::vector<bilibili::DashMedia>>();
    tracks->push_back(video);
    if (audio) tracks->push_back(*audio);
    p.tracks = tracks;
    brls::Logger::info("Xbox source: quality={}, {}x{}@{}, codecs={}, audio={}",
        video.id, video.width, video.height, video.frame_rate, video.codecs, audio ? audio->codecs : "none");
    try {
        if (!supports(p.output)) throw std::runtime_error("Xbox/TV does not support this HDR format; select an SDR quality");
        if (!compatibleXboxHdrStream(video.id, video.width, video.height))
            throw std::runtime_error("此 HDR／杜比码流尺寸超出 Xbox 当前解码路径支持范围，请选择兼容分P或普通画质");
        const auto xml = makeDashManifest(video, audio, duration);
        p.fps = 0;
        if (!video.frame_rate.empty()) {
            const auto slash = video.frame_rate.find('/');
            p.fps = std::stod(video.frame_rate);
            if (slash != std::string::npos) p.fps /= std::stod(video.frame_rate.substr(slash + 1));
        }
        p.player = MediaPlayer();
        p.player.AutoPlay(false);
        p.player.CommandManager().IsEnabled(false);
        p.player.IsLoopingEnabled(false);
        p.player.Volume(1.0);
        auto compositor = D3D11_CONTEXT->getCompositor();
        p.root = D3D11_CONTEXT->getCompositionRoot();
        // Xbox MediaPlayer misplaces non-16:9 composition surfaces. Keep a UHD
        // output canvas and remove its letterbox with a brush transform. The
        // decoder keeps the original stream size and HDR/Dolby metadata.
        p.surfaceWidth = 3840;
        p.surfaceHeight = 2160;
        p.player.SetSurfaceSize({3840, 2160});
        p.surface = p.player.GetSurface(compositor);
        p.brush = compositor.CreateSurfaceBrush(p.surface.CompositionSurface());
        p.brush.Stretch(CompositionStretch::None);
        p.brush.HorizontalAlignmentRatio(0);
        p.brush.VerticalAlignmentRatio(0);
        p.visual = compositor.CreateSpriteVisual();
        p.visual.Brush(p.brush);
        p.visual.IsVisible(false);
        p.root.Children().InsertAtBottom(p.visual);
        std::weak_ptr<Impl> weak = impl;
        const uint64_t token = p.generation;
        p.watchSession(p.player, true, token);
        p.player.MediaOpened([weak, token, synchronizedAudio = p.useDirectSource && audio != nullptr](const auto&, const auto&) {
            if (auto state = weak.lock(); state && token == state->generation) {
                std::lock_guard lock(state->eventMutex);
                if (token != state->generation) return;
                state->videoOpened = true;
                state->loaded = !synchronizedAudio || state->audioOpened;
            }
        });
        p.player.MediaEnded([weak, token](const auto& sender, const auto&) {
            if (auto state = weak.lock(); state && token == state->generation) {
                if (state->looping || sender.IsLoopingEnabled()) return;
                std::lock_guard lock(state->eventMutex);
                if (token != state->generation) return;
                state->ended = true;
            }
        });
        p.player.MediaFailed([weak, token, width = video.width, height = video.height, codec = video.codecs](const auto&, const MediaPlayerFailedEventArgs& args) {
            if (auto state = weak.lock(); state && token == state->generation)
                state->fail(fmt::format("系统解码失败：{}×{} {}，{} ({:#010x})", width,
                    height, codec, to_string(args.ErrorMessage()),
                    static_cast<uint32_t>(args.ExtendedErrorCode().value)));
        });
        Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
        filter.CacheControl().ReadBehavior(Windows::Web::Http::Filters::HttpCacheReadBehavior::NoCache);
        p.http = HttpClient(filter);
        p.http.DefaultRequestHeaders().Referer(Uri(L"https://www.bilibili.com/"));
        p.http.DefaultRequestHeaders().TryAppendWithoutValidation(L"User-Agent", L"Mozilla/5.0 wiliwili");
        p.displayTask = Impl::open(impl, token, xml, duration, p.displayTask);
    } catch (const std::exception& e) { p.fail(e.what()); }
      catch (const hresult_error& e) { p.fail(to_string(e.message())); }
}

void UwpVideoPlayer::stop() {
    auto& p = *impl;
    const auto token = ++p.generation;
    p.isActive = false;
    p.displayReady = false;
    p.releasePlayer();
    { std::lock_guard lock(p.eventMutex); p.loaded = p.ended = p.failed = false; p.error.clear(); }
    if (p.hdmi) p.displayTask = Impl::changeDisplay(impl, token, true, p.displayTask);
}
void UwpVideoPlayer::suspend() {
    if (!active()) return;
    impl->suspended = true;
    impl->displayReady = false;
    impl->holdTransport();
    const auto token = impl->generation.load();
    // Only restore the display here; MediaPlayer retains the compressed source.
    impl->displayTask = Impl::changeDisplay(impl, token, true, impl->displayTask);
}
void UwpVideoPlayer::resumeDisplay() {
    if (!active()) return;
    impl->suspended = false;
    // Do not invalidate the source/event handlers during a suspend/resume.
    impl->displayTask = Impl::changeDisplay(impl, impl->generation, false, impl->displayTask);
}
void UwpVideoPlayer::pause(bool paused) {
    if (!impl->player) return;
    impl->autoplay = !paused;
    if (paused) impl->holdTransport();
    else if (impl->suspended) resumeDisplay();
}
void UwpVideoPlayer::seek(double seconds) {
    if (!impl->player || !std::isfinite(seconds)) return;
    impl->holdTransport();
    impl->pendingSeek = std::max(0.0, seconds);
    impl->updateTransport();
}
void UwpVideoPlayer::speed(double rate) {
    if (rate <= 0) return;
    if (impl->timeline) impl->timeline.ClockRate(rate);
    else if (impl->player) impl->player.PlaybackSession().PlaybackRate(rate);
}
void UwpVideoPlayer::setLooping(bool enabled) {
    impl->looping = enabled;
    if (impl->timeline) impl->timeline.IsLoopingEnabled(enabled);
    else if (impl->player && (!impl->useDirectSource || (impl->tracks && impl->tracks->size() == 1)))
        impl->player.IsLoopingEnabled(enabled);
}
void UwpVideoPlayer::setVolume(int value) {
    const auto volume = std::clamp(value, 0, 100) / 100.0;
    if (impl->player) impl->player.Volume(volume);
    if (impl->audioPlayer) impl->audioPlayer.Volume(volume);
}
void UwpVideoPlayer::setVisible(bool visible) { if (impl->visual) impl->visual.IsVisible(visible && impl->displayReady); }
void UwpVideoPlayer::setBounds(float x, float y, float width, float height, float aspect, bool crop, bool stretch, bool mirror) {
    if (!impl->visual || width <= 0 || height <= 0) return;
    const float naturalAspect = impl->displayHeight ? static_cast<float>(impl->displayWidth) / impl->displayHeight : 16.0f / 9;
    const auto layout = layoutVideoSurface(x, y, width, height, impl->surfaceWidth, impl->surfaceHeight,
        naturalAspect, aspect, crop, stretch);
    const auto& bounds = layout.visual;
    const auto& previous = impl->lastBounds;
    if (bounds.x != previous.x || bounds.y != previous.y || bounds.width != previous.width || bounds.height != previous.height ||
        mirror != impl->lastMirror || crop != impl->lastCrop) {
        impl->visual.Offset({bounds.x + (mirror ? bounds.width : 0), bounds.y, 0});
        impl->visual.Size({bounds.width, bounds.height});
        impl->visual.Scale({mirror ? -1.0f : 1.0f, 1, 1});
        impl->brush.Scale({layout.scaleX, layout.scaleY});
        impl->brush.Offset({layout.offsetX, layout.offsetY});
        impl->lastBounds = bounds;
        impl->lastMirror = mirror;
        impl->lastCrop = crop;
    }
    setVisible(true);
}
UwpPlaybackSnapshot UwpVideoPlayer::takeSnapshot() {
    UwpPlaybackSnapshot s;
    s.active = active();
    if (!s.active) return s;
    {
        std::lock_guard lock(impl->eventMutex);
        s.loaded = std::exchange(impl->loaded, false);
        s.ended = std::exchange(impl->ended, false);
        s.failed = std::exchange(impl->failed, false);
        s.error = impl->error;
    }
    s.output = impl->outputName;
    if (impl->player) {
        const auto session = impl->player.PlaybackSession();
        const auto naturalDuration = session.NaturalDuration();
        // Some fragmented MP4 sources report TimeSpan::max() until their
        // duration is known. Keep the DASH duration for the UI and timeline.
        const bool knownDuration = naturalDuration > TimeSpan::zero() && naturalDuration < TimeSpan::max();
        s.duration = knownDuration ? std::chrono::duration<double>(naturalDuration).count() : impl->sourceDuration;
        if (s.loaded && impl->timeline) {
            if (s.duration > 0) impl->timeline.Duration(box_value(
                std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(s.duration))).as<IReference<TimeSpan>>());
        }
        s.buffering = impl->updateTransport();
        const auto state = session.PlaybackState();
        if (impl->timeline) s.buffering |= impl->timeline.State() == Windows::Media::MediaTimelineControllerState::Stalled;
        s.playing = impl->transportRunning && !s.buffering && state == MediaPlaybackState::Playing;
        s.paused = !impl->autoplay;
        s.position = impl->pendingSeek ? *impl->pendingSeek : impl->seekingPosition ? *impl->seekingPosition :
            std::chrono::duration<double>(impl->timeline ? impl->timeline.Position() : session.Position()).count();
        if (s.duration > 0) s.position = std::clamp(s.position, 0.0, s.duration);
        s.speed = impl->timeline ? impl->timeline.ClockRate() : session.PlaybackRate();
        s.volume = static_cast<int>(std::lround(impl->player.Volume() * 100));
        s.width = session.NaturalVideoWidth();
        s.height = session.NaturalVideoHeight();
        if (s.loaded) impl->mediaOpened = true;
        const bool sourceGeometry = (s.width == impl->videoWidth && s.height == impl->videoHeight) ||
            (s.width == impl->videoHeight && s.height == impl->videoWidth);
        if (impl->mediaOpened && sourceGeometry && s.width && s.height && (s.width != impl->displayWidth || s.height != impl->displayHeight)) {
            // NaturalVideo dimensions include the MP4 display rotation. The
            // coded 1280x720 frame may actually be displayed as 720x1280.
            impl->displayWidth = s.width;
            impl->displayHeight = s.height;
            impl->lastBounds = {-1, -1, -1, -1};
            brls::Logger::info("Xbox video display geometry: {}x{}", s.width, s.height);
        }
        if (impl->end > 0 && !s.buffering && s.position >= impl->end) { impl->holdTransport(); s.ended = true; impl->end = 0; }
        if (!knownDuration && s.duration > 0 && !impl->looping && !s.buffering && s.position >= s.duration) {
            impl->holdTransport();
            s.ended = true;
        }
    }
    return s;
}

std::optional<UwpMediaInfo> UwpVideoPlayer::mediaInfo() const {
    if (!active() || !impl->player) return std::nullopt;
    auto info = impl->info;
    info.output = impl->outputName;
    info.fps = frameRate();
    info.width = impl->displayWidth;
    info.height = impl->displayHeight;
    info.bufferingProgress = -1;
    try { info.bufferingProgress = impl->player.PlaybackSession().BufferingProgress(); }
    catch (const hresult_error& e) {
        if (!impl->bufferingStatsWarning) {
            impl->bufferingStatsWarning = true;
            brls::Logger::info("Xbox buffering statistics unavailable: {:#010x}", static_cast<uint32_t>(e.code().value));
        }
    }
    if (impl->mediaOpened) {
        if (impl->playbackItem) {
            try {
                const auto tracks = impl->playbackItem.VideoTracks();
                if (tracks && tracks.Size()) {
                    const auto track = tracks.GetAt(0);
                    const auto properties = track ? track.GetEncodingProperties() : nullptr;
                    if (properties) {
                        const auto rate = properties.FrameRate();
                        // Xbox can return a default 30 fps here for a 24 fps
                        // Dolby stream. Prefer the stream's declared rate.
                        if (info.fps <= 0 && rate && rate.Numerator() && rate.Denominator())
                            info.fps = static_cast<double>(rate.Numerator()) / rate.Denominator();
                    }
                }
            } catch (const hresult_error&) { /* The Xbox runtime can omit track encoding properties. */ }
            try {
                const auto tracks = (impl->audioPlaybackItem ? impl->audioPlaybackItem : impl->playbackItem).AudioTracks();
                if (tracks && tracks.Size()) {
                    const auto track = tracks.GetAt(0);
                    const auto properties = track ? track.GetEncodingProperties() : nullptr;
                    if (properties) {
                        info.audioChannels = properties.ChannelCount();
                        info.audioSampleRate = properties.SampleRate();
                    }
                }
            } catch (const hresult_error&) { /* Retain explicitly unknown audio fields. */ }
        }
    }
    // These downloads are supplied by the app, so use the measured transfer
    // rate rather than the system HTTP downloader's optional statistics.
    info.downloadBitrate = impl->lastDownloadBitrate;
    return info;
}
} // namespace wiliwili
#endif
