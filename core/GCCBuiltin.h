#pragma once

#ifdef _MSC_VER
#include <intrin.h>

#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#define __builtin_bswap64(x) _byteswap_uint64(x)

inline int __builtin_clz(unsigned int x)
{
    unsigned long index;
    _BitScanReverse(&index, x);
    return 31 ^ index;
}

inline int __builtin_ctz(unsigned int x)
{
    unsigned long index;
    _BitScanForward(&index, x);
    return index;
}

// else generic fallbask?
#endif