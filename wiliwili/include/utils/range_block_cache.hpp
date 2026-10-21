#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace wiliwili {
class RangeBlockCache {
public:
    using Fetch = std::function<std::string(uint64_t, uint64_t)>;
    RangeBlockCache(uint64_t size, Fetch fetch, uint64_t blockSize = 1024 * 1024, size_t blocks = 8)
        : length(size), blockSize(blockSize), capacity(blocks), fetch(std::move(fetch)) {
        if (!size || !blockSize || !blocks || !this->fetch) throw std::invalid_argument("Invalid range cache");
    }
    uint64_t size() const { return length; }
    uint64_t blockLength() const { return blockSize; }
    std::string read(uint64_t offset, uint64_t count, bool partial = false) {
        if (offset > length || count > 64 * 1024 * 1024) throw std::out_of_range("Invalid MP4 stream read");
        count = std::min(count, length - offset);
        if (partial) count = std::min(count, blockSize - offset % blockSize);
        std::string result;
        result.reserve(static_cast<size_t>(count));
        while (count) {
            const auto block = offset / blockSize;
            const auto bytes = getBlock(block);
            const auto inside = static_cast<size_t>(offset % blockSize);
            const auto take = std::min<uint64_t>(count, bytes->size() - inside);
            result.append(*bytes, inside, static_cast<size_t>(take));
            offset += take;
            count -= take;
        }
        return result;
    }
private:
    using Block = std::shared_ptr<const std::string>;
    struct Pending {
        std::promise<Block> promise;
        std::shared_future<Block> result = promise.get_future().share();
    };
    Block getBlock(uint64_t block) {
        std::shared_ptr<Pending> pending;
        bool owner = false;
        {
            std::lock_guard lock(mutex);
            const auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& entry) { return entry.first == block; });
            if (found != cache.end()) {
                auto bytes = found->second;
                cache.splice(cache.begin(), cache, found);
                return bytes;
            }
            const auto it = downloads.find(block);
            if (it != downloads.end()) pending = it->second;
            else {
                pending = std::make_shared<Pending>();
                downloads.emplace(block, pending);
                owner = true;
            }
        }
        // Share a miss for the same block without holding the LRU mutex during
        // network I/O. Other streams can still consume cached video samples.
        if (!owner) return pending->result.get();
        try {
            const auto first = block * blockSize;
            const auto wanted = std::min(blockSize, length - first);
            auto bytes = std::make_shared<const std::string>(fetch(first, wanted));
            if (bytes->size() != wanted) throw std::runtime_error("Incomplete MP4 byte range");
            {
                std::lock_guard lock(mutex);
                cache.emplace_front(block, bytes);
                if (cache.size() > capacity) cache.pop_back();
                downloads.erase(block);
            }
            pending->promise.set_value(bytes);
            return bytes;
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                downloads.erase(block);
            }
            pending->promise.set_exception(std::current_exception());
            throw;
        }
    }
    uint64_t length, blockSize;
    size_t capacity;
    Fetch fetch;
    std::mutex mutex;
    std::list<std::pair<uint64_t, Block>> cache;
    std::unordered_map<uint64_t, std::shared_ptr<Pending>> downloads;
};
} // namespace wiliwili
