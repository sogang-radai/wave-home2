#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #ifdef LZAV_LIB_EXPORTS
    #define LZAV_API __declspec(dllexport)
  #else
    #define LZAV_API __declspec(dllimport)
  #endif
#else
  #define LZAV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decompress an LZAV buffer.
 *
 * @param src      Compressed data.
 * @param src_len  Compressed length in bytes.
 * @param dst      Output buffer (must hold dst_len bytes).
 * @param dst_len  Expected uncompressed length in bytes.
 * @return Bytes written on success, negative LZAV error code on failure.
 */
LZAV_API int LZAV_DecompressBuffer(
    uint8_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_len);

#ifdef __cplusplus
}
#endif
