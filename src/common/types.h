#pragma once

#include <cstdint>

namespace baton {

// Log sequence number. The first record is LSN 1; 0 means "nothing yet".
using Lsn = uint64_t;

}  // namespace baton
