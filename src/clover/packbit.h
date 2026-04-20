#ifndef PACKBIT_HPP
#define PACKBIT_HPP
#include <cstdint>

inline int64_t pack2ints(int32_t a, int32_t b) {
    return (static_cast<int64_t>(a) << 32) | (static_cast<uint32_t>(b));
}

inline int32_t unpack_a(int64_t packed) {
    return static_cast<int32_t>(packed >> 32);
}

inline int32_t unpack_b(int64_t packed) {
    return static_cast<int32_t>(packed & 0xFFFFFFFF);
}

/* compare (a, b) < e2 without packing (a, b) first */
inline bool packed_compare(int32_t a, int32_t b, int64_t e2) {
    int32_t e2_a = static_cast<int32_t>(e2 >> 32);
    if (a != e2_a) return a < e2_a;
    return b < static_cast<int32_t>(e2 & 0xFFFFFFFF);
}

/********************************************/

inline int32_t pack2ints(int16_t a, int16_t b) {
    return (static_cast<int32_t>(a) << 16) | (static_cast<uint16_t>(b));
}

inline int16_t unpack_a(int32_t packed) {
    return static_cast<int16_t>(packed >> 16);
}

inline int16_t unpack_b(int32_t packed) {
    return static_cast<int16_t>(packed & 0xFFFF);
}

/* compare (a, b) < e2 without packing (a, b) first */
inline bool packed_compare(int16_t a, int16_t b, int32_t e2) {
    int16_t e2_a = static_cast<int16_t>(e2 >> 16);
    if (a != e2_a) return a < e2_a;
    return b < static_cast<int16_t>(e2 & 0xFFFF);
}

#endif // PACKBIT_HPP