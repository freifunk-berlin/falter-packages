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
 *   tag(8)   only with WIRE_F_AUTH
 *
 * Ports in network byte order, reserved and record flags are zero. Everything
 * is copied byte-wise, nothing is cast to a struct.
 *
 * Authentication is optional: a daemon with a key (the same on all gateways)
 * sets WIRE_F_AUTH and appends SipHash-2-4 of header and records, and accepts
 * only datagrams that carry the tag of one of its keys. A daemon without a
 * key sends none and checks none. The tag proves that the sender has the
 * key, nothing more: a datagram recorded on the way can be sent again.
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
	/* reserved for later versions: a sender that sets it speaks another one */
	if (buf[3])
		return PARSE_ERR;
	if (*count > WIRE_MAX_RECORDS ||
	    len != WIRE_HDR_LEN + (size_t)*count * WIRE_REC_LEN +
		   (*flags & WIRE_F_AUTH ? WIRE_TAG_LEN : 0))
		return PARSE_ERR;
	return PARSE_OK;
}

static void tag_put(uint8_t *dst, const uint8_t *key, const uint8_t *buf, size_t len)
{
	uint64_t t = siphash24(key, buf, len);
	unsigned int i;

	for (i = 0; i < WIRE_TAG_LEN; i++)
		dst[i] = t >> (8 * i);
}

/* a finished datagram of len bytes, in a buffer with WIRE_TAG_LEN to spare:
 * with a key, flag it and append the tag; returns the length to send */
size_t wire_seal(uint8_t *buf, size_t len)
{
	if (!cfg.n_key)
		return len;
	buf[2] |= WIRE_F_AUTH;
	tag_put(buf + len, cfg.key[0], buf, len);
	return len + WIRE_TAG_LEN;
}

/* a datagram that passed wire_check(): is it one of ours? Without a key
 * every one is, tagged or not (nothing to check a tag with) */
bool wire_auth(const uint8_t *buf, size_t len)
{
	uint8_t tag[WIRE_TAG_LEN], diff;
	unsigned int k, i;

	if (!cfg.n_key)
		return true;
	if (!(buf[2] & WIRE_F_AUTH))
		return false;
	len -= WIRE_TAG_LEN;
	for (k = 0; k < cfg.n_key; k++) {
		tag_put(tag, cfg.key[k], buf, len);
		/* no early exit: the time says nothing about where it differs */
		for (diff = 0, i = 0; i < WIRE_TAG_LEN; i++)
			diff |= tag[i] ^ buf[len + i];
		if (!diff)
			return true;
	}
	return false;
}

int wire_get(const uint8_t *src, struct flow *f)
{
	uint16_t be;

	memset(f, 0, sizeof(*f));
	f->proto = src[0];
	/* record flags and reserved bytes are zero in version 1 */
	if (!f->proto || src[1] || src[6] || src[7])
		return PARSE_ERR;
	memcpy(&be, src + 2, 2);
	f->cport = ntohs(be);
	memcpy(&be, src + 4, 2);
	f->sport = ntohs(be);
	memcpy(&f->c, src + 8, 16);
	memcpy(&f->s, src + 24, 16);
	return PARSE_OK;
}
