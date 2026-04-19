#ifndef _BSWAP_H
#define _BSWAP_H

#include <stdint.h>
#include "../host_endian.h"

#if defined(_MSC_VER)
#define HOST_BSWAP_INLINE static __forceinline
#else
#define HOST_BSWAP_INLINE static inline
#endif

#define HOST_WORDS_BIGENDIAN HOST_BIG_ENDIAN

HOST_BSWAP_INLINE uint16_t bswap16(uint16_t x)
{
    return (uint16_t)(((x & 0x00ffU) << 8) | ((x & 0xff00U) >> 8));
}

HOST_BSWAP_INLINE uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffUL) << 24) |
           ((x & 0x0000ff00UL) << 8) |
           ((x & 0x00ff0000UL) >> 8) |
           ((x & 0xff000000UL) >> 24);
}

HOST_BSWAP_INLINE uint64_t bswap64(uint64_t x)
{
    return ((x & 0x00000000000000ffULL) << 56) |
           ((x & 0x000000000000ff00ULL) << 40) |
           ((x & 0x0000000000ff0000ULL) << 24) |
           ((x & 0x00000000ff000000ULL) << 8) |
           ((x & 0x000000ff00000000ULL) >> 8) |
           ((x & 0x0000ff0000000000ULL) >> 24) |
           ((x & 0x00ff000000000000ULL) >> 40) |
           ((x & 0xff00000000000000ULL) >> 56);
}

HOST_BSWAP_INLINE void bswap16s(uint16_t* s)
{
    *s = bswap16(*s);
}

HOST_BSWAP_INLINE void bswap32s(uint32_t* s)
{
    *s = bswap32(*s);
}

HOST_BSWAP_INLINE void bswap64s(uint64_t* s)
{
    *s = bswap64(*s);
}

#define bswap_16(x) bswap16((uint16_t)(x))
#define bswap_32(x) bswap32((uint32_t)(x))
#define bswap_64(x) bswap64((uint64_t)(x))

#if HOST_WORDS_BIGENDIAN
#define HOST_BE16(v) (v)
#define HOST_BE32(v) (v)
#define HOST_BE64(v) (v)
#define HOST_LE16(v) bswap16(v)
#define HOST_LE32(v) bswap32(v)
#define HOST_LE64(v) bswap64(v)
#else
#define HOST_BE16(v) bswap16(v)
#define HOST_BE32(v) bswap32(v)
#define HOST_BE64(v) bswap64(v)
#define HOST_LE16(v) (v)
#define HOST_LE32(v) (v)
#define HOST_LE64(v) (v)
#endif

HOST_BSWAP_INLINE uint16_t be16_to_cpu(uint16_t v) { return HOST_BE16(v); }
HOST_BSWAP_INLINE uint32_t be32_to_cpu(uint32_t v) { return HOST_BE32(v); }
HOST_BSWAP_INLINE uint64_t be64_to_cpu(uint64_t v) { return HOST_BE64(v); }

HOST_BSWAP_INLINE uint16_t cpu_to_be16(uint16_t v) { return HOST_BE16(v); }
HOST_BSWAP_INLINE uint32_t cpu_to_be32(uint32_t v) { return HOST_BE32(v); }
HOST_BSWAP_INLINE uint64_t cpu_to_be64(uint64_t v) { return HOST_BE64(v); }

HOST_BSWAP_INLINE void be16_to_cpus(uint16_t* p) { *p = be16_to_cpu(*p); }
HOST_BSWAP_INLINE void be32_to_cpus(uint32_t* p) { *p = be32_to_cpu(*p); }
HOST_BSWAP_INLINE void be64_to_cpus(uint64_t* p) { *p = be64_to_cpu(*p); }

HOST_BSWAP_INLINE void cpu_to_be16s(uint16_t* p) { *p = cpu_to_be16(*p); }
HOST_BSWAP_INLINE void cpu_to_be32s(uint32_t* p) { *p = cpu_to_be32(*p); }
HOST_BSWAP_INLINE void cpu_to_be64s(uint64_t* p) { *p = cpu_to_be64(*p); }

HOST_BSWAP_INLINE uint16_t be16_to_cpup(const uint16_t* p) { return be16_to_cpu(*p); }
HOST_BSWAP_INLINE uint32_t be32_to_cpup(const uint32_t* p) { return be32_to_cpu(*p); }
HOST_BSWAP_INLINE uint64_t be64_to_cpup(const uint64_t* p) { return be64_to_cpu(*p); }

HOST_BSWAP_INLINE void cpu_to_be16w(uint16_t* p, uint16_t v) { *p = cpu_to_be16(v); }
HOST_BSWAP_INLINE void cpu_to_be32w(uint32_t* p, uint32_t v) { *p = cpu_to_be32(v); }
HOST_BSWAP_INLINE void cpu_to_be64w(uint64_t* p, uint64_t v) { *p = cpu_to_be64(v); }

HOST_BSWAP_INLINE uint16_t le16_to_cpu(uint16_t v) { return HOST_LE16(v); }
HOST_BSWAP_INLINE uint32_t le32_to_cpu(uint32_t v) { return HOST_LE32(v); }
HOST_BSWAP_INLINE uint64_t le64_to_cpu(uint64_t v) { return HOST_LE64(v); }

HOST_BSWAP_INLINE uint16_t cpu_to_le16(uint16_t v) { return HOST_LE16(v); }
HOST_BSWAP_INLINE uint32_t cpu_to_le32(uint32_t v) { return HOST_LE32(v); }
HOST_BSWAP_INLINE uint64_t cpu_to_le64(uint64_t v) { return HOST_LE64(v); }

HOST_BSWAP_INLINE void le16_to_cpus(uint16_t* p) { *p = le16_to_cpu(*p); }
HOST_BSWAP_INLINE void le32_to_cpus(uint32_t* p) { *p = le32_to_cpu(*p); }
HOST_BSWAP_INLINE void le64_to_cpus(uint64_t* p) { *p = le64_to_cpu(*p); }

HOST_BSWAP_INLINE void cpu_to_le16s(uint16_t* p) { *p = cpu_to_le16(*p); }
HOST_BSWAP_INLINE void cpu_to_le32s(uint32_t* p) { *p = cpu_to_le32(*p); }
HOST_BSWAP_INLINE void cpu_to_le64s(uint64_t* p) { *p = cpu_to_le64(*p); }

HOST_BSWAP_INLINE uint16_t le16_to_cpup(const uint16_t* p) { return le16_to_cpu(*p); }
HOST_BSWAP_INLINE uint32_t le32_to_cpup(const uint32_t* p) { return le32_to_cpu(*p); }
HOST_BSWAP_INLINE uint64_t le64_to_cpup(const uint64_t* p) { return le64_to_cpu(*p); }

HOST_BSWAP_INLINE void cpu_to_le16w(uint16_t* p, uint16_t v) { *p = cpu_to_le16(v); }
HOST_BSWAP_INLINE void cpu_to_le32w(uint32_t* p, uint32_t v) { *p = cpu_to_le32(v); }
HOST_BSWAP_INLINE void cpu_to_le64w(uint64_t* p, uint64_t v) { *p = cpu_to_le64(v); }

HOST_BSWAP_INLINE void cpu_to_le16wu(uint16_t* p, uint16_t v)
{
    uint8_t* p1 = (uint8_t*)p;
    uint16_t t = cpu_to_le16(v);

    p1[0] = (uint8_t)t;
    p1[1] = (uint8_t)(t >> 8);
}

HOST_BSWAP_INLINE void cpu_to_le32wu(uint32_t* p, uint32_t v)
{
    uint8_t* p1 = (uint8_t*)p;
    uint32_t t = cpu_to_le32(v);

    p1[0] = (uint8_t)t;
    p1[1] = (uint8_t)(t >> 8);
    p1[2] = (uint8_t)(t >> 16);
    p1[3] = (uint8_t)(t >> 24);
}

HOST_BSWAP_INLINE uint16_t le16_to_cpupu(const uint16_t* p)
{
    const uint8_t* p1 = (const uint8_t*)p;
    uint16_t raw = (uint16_t)(p1[0] | ((uint16_t)p1[1] << 8));
    return le16_to_cpu(raw);
}

HOST_BSWAP_INLINE uint32_t le32_to_cpupu(const uint32_t* p)
{
    const uint8_t* p1 = (const uint8_t*)p;
    uint32_t raw = (uint32_t)p1[0] |
        ((uint32_t)p1[1] << 8) |
        ((uint32_t)p1[2] << 16) |
        ((uint32_t)p1[3] << 24);
    return le32_to_cpu(raw);
}

HOST_BSWAP_INLINE void cpu_to_be16wu(uint16_t* p, uint16_t v)
{
    uint8_t* p1 = (uint8_t*)p;
    uint16_t t = cpu_to_be16(v);

    p1[0] = (uint8_t)t;
    p1[1] = (uint8_t)(t >> 8);
}

HOST_BSWAP_INLINE void cpu_to_be32wu(uint32_t* p, uint32_t v)
{
    uint8_t* p1 = (uint8_t*)p;
    uint32_t t = cpu_to_be32(v);

    p1[0] = (uint8_t)t;
    p1[1] = (uint8_t)(t >> 8);
    p1[2] = (uint8_t)(t >> 16);
    p1[3] = (uint8_t)(t >> 24);
}

#if HOST_WORDS_BIGENDIAN
#define cpu_to_32wu cpu_to_be32wu
#else
#define cpu_to_32wu cpu_to_le32wu
#endif

#undef HOST_BE16
#undef HOST_BE32
#undef HOST_BE64
#undef HOST_LE16
#undef HOST_LE32
#undef HOST_LE64
#undef HOST_WORDS_BIGENDIAN
#undef HOST_BSWAP_INLINE

#endif /* _BSWAP_H */
