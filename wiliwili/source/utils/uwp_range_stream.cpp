#ifdef WILIWILI_UWP_HDR
#include "utils/uwp_range_stream.hpp"
#include <winrt/Windows.Foundation.h>
#include <robuffer.h>
#include <atomic>
#include <cstring>

namespace wiliwili {
using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

struct RangeReadAhead : std::enable_shared_from_this<RangeReadAhead> {
    std::shared_ptr<RangeBlockCache> cache;
    uint64_t window;
    std::mutex mutex;
    uint64_t position = UINT64_MAX, revision = 0;
    bool active = false, complete = false, closed = false;
    RangeReadAhead(std::shared_ptr<RangeBlockCache> value, uint64_t bytes) : cache(std::move(value)), window(bytes) {}

    void request(uint64_t next) {
        if (!window || next >= cache->size()) return;
        next -= next % cache->blockLength();
        {
            std::lock_guard lock(mutex);
            if (closed || (next == position && (active || complete))) return;
            if (next != position) { position = next; ++revision; complete = false; }
            if (active) return;
            active = true;
        }
        warm(shared_from_this());
    }
    void close() { std::lock_guard lock(mutex); closed = true; }
    static fire_and_forget warm(std::shared_ptr<RangeReadAhead> self) {
        co_await resume_background();
        try {
            for (;;) {
                uint64_t first, token;
                {
                    std::lock_guard lock(self->mutex);
                    if (self->closed) { self->active = false; co_return; }
                    first = self->position;
                    token = self->revision;
                }
                const auto last = first + std::min(self->window, self->cache->size() - first);
                for (auto offset = first; offset < last; offset += self->cache->blockLength()) {
                    {
                        std::lock_guard lock(self->mutex);
                        if (self->closed || token != self->revision) break;
                    }
                    // Each track has at most one read-ahead worker. Cache
                    // hits and decoder demand remain independent of its I/O.
                    self->cache->read(offset, std::min(self->cache->blockLength(), last - offset));
                }
                {
                    std::lock_guard lock(self->mutex);
                    if (self->closed || token == self->revision) {
                        self->complete = !self->closed;
                        self->active = false;
                        co_return;
                    }
                }
            }
        } catch (...) {
            // Speculative fetch failure must not fail an otherwise playable
            // source. A decoder demand will retry through the normal path.
            std::lock_guard lock(self->mutex);
            self->active = false;
            self->complete = false;
        }
    }
};

struct RangeStream : implements<RangeStream, IRandomAccessStream, IInputStream, IOutputStream, IClosable> {
    std::shared_ptr<RangeBlockCache> cache;
    std::shared_ptr<RangeReadAhead> readAhead;
    bool ownsReadAhead;
    std::mutex cursorMutex;
    std::atomic<uint64_t> cursor{0};
    std::atomic<bool> closed{false};
    RangeStream(std::shared_ptr<RangeBlockCache> value, std::shared_ptr<RangeReadAhead> ahead, uint64_t position, bool owner = false)
        : cache(std::move(value)), readAhead(std::move(ahead)), ownsReadAhead(owner), cursor(position) {}
    uint64_t Size() const { return cache->size(); }
    void Size(uint64_t) { throw hresult_access_denied(); }
    bool CanRead() const { return !closed; }
    bool CanWrite() const { return false; }
    uint64_t Position() const { return cursor; }
    void Seek(uint64_t position) {
        if (position > Size()) throw hresult_out_of_bounds();
        std::lock_guard lock(cursorMutex);
        cursor = position;
    }
    IRandomAccessStream CloneStream() { return make<RangeStream>(cache, readAhead, cursor.load()); }
    IInputStream GetInputStreamAt(uint64_t position) {
        if (position > Size()) throw hresult_out_of_bounds();
        return make<RangeStream>(cache, readAhead, position).as<IInputStream>();
    }
    IOutputStream GetOutputStreamAt(uint64_t) { throw hresult_access_denied(); }
    void Close() { closed = true; if (ownsReadAhead) readAhead->close(); }
    IAsyncOperationWithProgress<IBuffer, uint32_t> ReadAsync(IBuffer buffer, uint32_t count, InputStreamOptions options) {
        auto lifetime = get_strong();
        co_await resume_background();
        if (closed) throw hresult_canceled();
        std::lock_guard lock(cursorMutex);
        try {
            const auto wanted = std::min(count, buffer.Capacity());
            // A Partial read must not wait for a large multi-block request
            // when the decoder can already consume the first samples.
            const auto bytes = cache->read(cursor, wanted,
                (options & InputStreamOptions::Partial) == InputStreamOptions::Partial);
            if (closed) throw hresult_canceled();
            uint8_t* destination = nullptr;
            check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&destination));
            if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
            buffer.Length(static_cast<uint32_t>(bytes.size()));
            cursor += bytes.size();
            if (bytes.size() >= 16 * 1024) readAhead->request(cursor);
            co_return buffer;
        } catch (const std::exception&) { throw hresult_error(E_FAIL, L"MP4 byte-range read failed"); }
    }
    IAsyncOperationWithProgress<uint32_t, uint32_t> WriteAsync(IBuffer const&) { throw hresult_access_denied(); }
    IAsyncOperation<bool> FlushAsync() { co_return false; }
};

IRandomAccessStream makeUwpRangeStream(std::shared_ptr<RangeBlockCache> cache, uint64_t readAheadBytes) {
    auto readAhead = std::make_shared<RangeReadAhead>(cache, readAheadBytes);
    return make<RangeStream>(std::move(cache), std::move(readAhead), 0, true);
}
} // namespace wiliwili
#endif
