// SPDX-License-Identifier: GPL-2.0-only
/*
 * The binary wire format. One datagram is a 4 byte header followed by
 * count records of WIRE_REC_LEN bytes, at most WIRE_MAX_RECORDS of them.
 * A datagram without records is a heartbeat, or with WIRE_F_RESYNC a request
 * for a round now:
 *
 *   header:  version(1)  count(1)  flags(1)  reserved(1)
 *   record:  proto(1)  flags(1)  cport(2)  sport(2)  reserved(2)
 *            client(16)  server(16)
 *
 * Ports in network byte order, reserved and flags are zero. Everything is
 * copied byte-wise, nothing is cast to a struct.
 */

#include <arpa/inet.h>
#include <string.h>

#include "flowsync.h"

void wire_put_hdr(uint8_t *dst, unsigned int count, uint8_t flags)
{
	dst[0] = WIRE_VERSION;
	dst[1] = count;
	dst[2] = flags;
	dst[3] = 0;
}

void wire_put(uint8_t *dst, const struct flow *f)
{
	uint16_t be;

	dst[0] = f->proto;
	dst[1] = 0;
	be = htons(f->cport);
	memcpy(dst + 2, &be, 2);
	be = htons(f->sport);
	memcpy(dst + 4, &be, 2);
	dst[6] = 0;
	dst[7] = 0;
	memcpy(dst + 8, &f->c, 16);
	memcpy(dst + 24, &f->s, 16);
}

/* header and length check of a datagram; count and flags are valid on
 * PARSE_OK */
int wire_check(const uint8_t *buf, size_t len, unsigned int *count, uint8_t *flags)
{
	if (len < WIRE_HDR_LEN)
		return PARSE_ERR;
	if (buf[0] != WIRE_VERSION)
		return PARSE_VERSION;
	*count = buf[1];
	*flags = buf[2];
	if (*count > WIRE_MAX_RECORDS ||
	    len != WIRE_HDR_LEN + (size_t)*count * WIRE_REC_LEN)
		return PARSE_ERR;
	return PARSE_OK;
}

int wire_get(const uint8_t *src, struct flow *f)
{
	uint16_t be;

	memset(f, 0, sizeof(*f));
	f->proto = src[0];
	if (!proto_name(f->proto))
		return PARSE_ERR;
	memcpy(&be, src + 2, 2);
	f->cport = ntohs(be);
	memcpy(&be, src + 4, 2);
	f->sport = ntohs(be);
	memcpy(&f->c, src + 8, 16);
	memcpy(&f->s, src + 24, 16);
	return PARSE_OK;
}
