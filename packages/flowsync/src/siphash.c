// SPDX-License-Identifier: GPL-2.0-only
/*
 * SipHash-2-4 (Aumasson, Bernstein 2012), the 64 bit keyed hash that
 * authenticates the sync datagrams (wire.c). Bytes are read one by one, so
 * the result is the same on little and big endian machines.
 */

#include <stddef.h>
#include <stdint.h>

#include "flowsync.h"

#define ROTL(x, b) (((x) << (b)) | ((x) >> (64 - (b))))
#define ROUND(v0, v1, v2, v3) do { \
	v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); \
	v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
	v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; \
	v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); \
} while (0)

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
	       (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
	       (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

uint64_t siphash24(const uint8_t key[SIP_KEY_LEN], const uint8_t *in, size_t len)
{
	uint64_t k0 = le64(key), k1 = le64(key + 8), m;
	uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
	uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
	const uint8_t *end = in + (len & ~(size_t)7);
	unsigned int i;

	for (; in < end; in += 8) {
		m = le64(in);
		v3 ^= m;
		ROUND(v0, v1, v2, v3);
		ROUND(v0, v1, v2, v3);
		v0 ^= m;
	}
	/* the rest, and the length in the top byte */
	m = (uint64_t)len << 56;
	for (i = 0; i < (len & 7); i++)
		m |= (uint64_t)in[i] << (8 * i);
	v3 ^= m;
	ROUND(v0, v1, v2, v3);
	ROUND(v0, v1, v2, v3);
	v0 ^= m;
	v2 ^= 0xff;
	for (i = 0; i < 4; i++)
		ROUND(v0, v1, v2, v3);
	return v0 ^ v1 ^ v2 ^ v3;
}
