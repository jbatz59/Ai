#pragma once
// Private to src/core: non-blocking logger access for the crash reporter, which must never wait on
// a lock that the crashed thread may be holding.
#include <cstddef>
#include <string_view>
#include <vector>

#include "core/log.h"

namespace cg::log::internal {

// Like Write() + immediate file flush, but gives up (returns false) if the log lock is busy.
bool TryWriteNow(Level lvl, std::string_view channel, std::string_view text);
// Copies up to maxEntries of the newest ring entries (oldest first). False if the lock is busy.
bool TryCopyTail(std::vector<Entry>& out, size_t maxEntries);

}  // namespace cg::log::internal
