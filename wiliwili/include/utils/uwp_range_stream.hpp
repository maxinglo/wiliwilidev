#pragma once
#ifdef WILIWILI_UWP_HDR
#include "utils/range_block_cache.hpp"
#include <memory>
#include <winrt/Windows.Storage.Streams.h>
namespace wiliwili {
winrt::Windows::Storage::Streams::IRandomAccessStream makeUwpRangeStream(std::shared_ptr<RangeBlockCache> cache, uint64_t readAheadBytes = 0);
}
#endif
