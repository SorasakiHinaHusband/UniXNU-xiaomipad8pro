// SPDX-License-Identifier: BSD-2-Clause-Patent
// Portable row-band partitioning shared by NativeGopDisplay and its host tests.
#pragma once
#include "GopCompose.h"

namespace gopcopy {

struct RowSlice {
    int32_t y0 = 0, y1 = 0;
};

inline unsigned sliceCount(int32_t y0, int32_t y1, unsigned workers)
{
    if (y1 <= y0) return 0;
    if (!workers) workers = 1;
    const unsigned rows = static_cast<unsigned>(y1 - y0);
    return workers < rows ? workers : rows;
}

inline RowSlice sliceAt(int32_t y0, int32_t y1, unsigned workers, unsigned index)
{
    const unsigned count = sliceCount(y0, y1, workers);
    if (!count || index >= count) return {y1, y1};
    const unsigned rows = static_cast<unsigned>(y1 - y0);
    const unsigned base = rows / count, extra = rows % count;
    const int32_t start = y0 + static_cast<int32_t>(index * base + (index < extra ? index : extra));
    const int32_t height = static_cast<int32_t>(base + (index < extra ? 1 : 0));
    return {start, start + height};
}

} // namespace gopcopy
