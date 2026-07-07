#include "lzav_lib.h"

#include <climits>

#include "lzav.h"

extern "C" LZAV_API int LZAV_DecompressBuffer(
    uint8_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_len)
{
    if (!src || !dst)
        return -1;

    if (src_len == 0 && dst_len == 0)
        return 0;

    if (src_len > static_cast<size_t>(INT_MAX) || dst_len > static_cast<size_t>(INT_MAX))
        return -1;

    return lzav_decompress(
        src,
        dst,
        static_cast<int>(src_len),
        static_cast<int>(dst_len));
}
