#ifndef _HOST_ENDIAN_H_
#define _HOST_ENDIAN_H_

#include <stdint.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define HOST_BIG_ENDIAN 1
#elif defined(BYTE_ORDER) && defined(BIG_ENDIAN) && (BYTE_ORDER == BIG_ENDIAN)
#define HOST_BIG_ENDIAN 1
#elif defined(_BYTE_ORDER) && defined(_BIG_ENDIAN) && (_BYTE_ORDER == _BIG_ENDIAN)
#define HOST_BIG_ENDIAN 1
#elif defined(WORDS_BIGENDIAN) && WORDS_BIGENDIAN
#define HOST_BIG_ENDIAN 1
#elif defined(__BIG_ENDIAN__)
#define HOST_BIG_ENDIAN 1
#elif defined(__ARMEB__) || defined(__AARCH64EB__) || defined(__MIPSEB__) || defined(__MIPSEB)
#define HOST_BIG_ENDIAN 1
#else
#define HOST_BIG_ENDIAN 0
#endif

#if defined(_MSC_VER)
#define HOST_ENDIAN_INLINE static __forceinline
#else
#define HOST_ENDIAN_INLINE static inline __attribute__((always_inline))
#endif

HOST_ENDIAN_INLINE uint16_t host_read_le16_unaligned(const void* src)
{
	const uint8_t* p = (const uint8_t*)src;
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

HOST_ENDIAN_INLINE uint32_t host_read_le32_unaligned(const void* src)
{
	const uint8_t* p = (const uint8_t*)src;
	return (uint32_t)p[0] |
		((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
}

HOST_ENDIAN_INLINE uint16_t host_read_be16_unaligned(const void* src)
{
	const uint8_t* p = (const uint8_t*)src;
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

HOST_ENDIAN_INLINE void host_write_le16_unaligned(void* dst, uint16_t value)
{
	uint8_t* p = (uint8_t*)dst;
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

HOST_ENDIAN_INLINE void host_write_le32_unaligned(void* dst, uint32_t value)
{
	uint8_t* p = (uint8_t*)dst;
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

#endif
