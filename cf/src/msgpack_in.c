/*
 * msgpack_in.c
 *
 * Copyright (C) 2019-2020 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "msgpack_in.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aerospike/as_bytes.h"
#include "citrusleaf/cf_byte_order.h"

#include "log.h"


//==========================================================
// Typedefs & constants.
//

#define CMP_EXT_TYPE 0xFF
#define CMP_WILDCARD 0x00
#define CMP_INF      0x01

typedef struct {
	const uint8_t *buf;
	const uint8_t * const end;

	union {
		const uint8_t *data;
		uint64_t i_num;
		double d_num;
	};

	uint32_t remain;
	uint32_t len;
	msgpack_type type;
	bool has_nonstorage;
} parse_meta;


//==========================================================
// Forward declarations.
//

static inline msgpack_type bytes_internal_to_msgpack_type(uint8_t type, uint32_t len);
static inline msgpack_type bytes_internal_to_type(uint8_t type, uint32_t len);

static inline const uint8_t *msgpack_sz_table(const uint8_t *buf, const uint8_t * const end, uint32_t *count, bool *has_nonstorage);
static inline const uint8_t *msgpack_sz_internal(const uint8_t *buf, const uint8_t * const end, uint32_t count, bool *has_nonstorage);

static inline uint64_t extract_uint64(const uint8_t *ptr, uint8_t sz);
static inline uint64_t extract_neg_int64(const uint8_t *ptr, uint8_t sz);
static inline void cmp_parse_container(parse_meta *meta, uint32_t count);
static inline msgpack_cmp_type msgpack_cmp_internal(parse_meta *meta0, parse_meta *meta1);


//==========================================================
// Inlines & macros.
//

#define MSGPACK_CMP_RETURN(__p0, __p1) \
	if ((__p0) > (__p1)) { \
		return MSGPACK_CMP_GREATER; \
	} \
	else if ((__p0) < (__p1)) { \
		return MSGPACK_CMP_LESS; \
	}

#define SZ_PARSE_BUF_CHECK(__buf, __end, __sz) \
	if ((__buf) + (__sz) > (__end)) { \
		return NULL; \
	}

#define CMP_PARSE_BUF_CHECK(__m, __sz) \
	if ((__m)->buf + (__sz) > (__m)->end) { \
		(__m)->buf = NULL; \
		return; \
	}


//==========================================================
// Public API.
//

uint32_t
msgpack_sz_rep(msgpack_in *mp, uint32_t rep_count)
{
	const uint8_t * const start = mp->buf + mp->offset;
	const uint8_t * const buf = msgpack_sz_internal(start, mp->buf + mp->buf_sz,
			rep_count, &mp->has_nonstorage);

	if (buf == NULL) {
		return 0;
	}

	uint32_t sz = buf - start;

	mp->offset += sz;

	return sz;
}

msgpack_cmp_type
msgpack_cmp(msgpack_in *mp0, msgpack_in *mp1)
{
	parse_meta meta0 = {
			.buf = mp0->buf + mp0->offset,
			.end = mp0->buf + mp0->buf_sz,
			.remain = 1
	};

	parse_meta meta1 = {
			.buf = mp1->buf + mp1->offset,
			.end = mp1->buf + mp1->buf_sz,
			.remain = 1
	};

	msgpack_cmp_type ret = msgpack_cmp_internal(&meta0, &meta1);

	meta0.buf = msgpack_sz_internal(meta0.buf, meta0.end, meta0.remain,
			&meta0.has_nonstorage);
	meta1.buf = msgpack_sz_internal(meta1.buf, meta1.end, meta1.remain,
			&meta1.has_nonstorage);

	if (meta0.buf == NULL || meta1.buf == NULL) {
		return MSGPACK_CMP_ERROR;
	}

	mp0->has_nonstorage = meta0.has_nonstorage;
	mp1->has_nonstorage = meta1.has_nonstorage;
	mp0->offset = meta0.buf - mp0->buf;
	mp1->offset = meta1.buf - mp1->buf;

	return ret;
}

msgpack_cmp_type
msgpack_cmp_peek(const msgpack_in *mp0, const msgpack_in *mp1)
{
	parse_meta meta0 = {
			.buf = mp0->buf + mp0->offset,
			.end = mp0->buf + mp0->buf_sz,
			.remain = 1
	};

	parse_meta meta1 = {
			.buf = mp1->buf + mp1->offset,
			.end = mp1->buf + mp1->buf_sz,
			.remain = 1
	};

	return msgpack_cmp_internal(&meta0, &meta1);
}

// Does not check buf_sz.
msgpack_type
msgpack_peek_type(const msgpack_in *mp)
{
	const uint8_t *buf = mp->buf + mp->offset;
	uint8_t b = *buf++;

	switch (b) {
	case 0xc0: // nil
		return MSGPACK_TYPE_NIL;
	case 0xc2: // boolean false
		return MSGPACK_TYPE_FALSE;
	case 0xc3: // boolean true
		return MSGPACK_TYPE_TRUE;

	case 0xd0: // signed 8 bit integer
	case 0xd1: // signed 16 bit integer
	case 0xd2: // signed 32 bit integer
	case 0xd3: // signed 64 bit integer
		return (*buf & 0x80) == 0 ? MSGPACK_TYPE_INT : MSGPACK_TYPE_NEGINT;
	case 0xcc: // unsigned 8 bit integer
	case 0xcd: // unsigned 16 bit integer
	case 0xce: // unsigned 32 bit integer
	case 0xcf: // unsigned 64 bit integer
		return MSGPACK_TYPE_INT;

	case 0xca: // float
	case 0xcb: // double
		return MSGPACK_TYPE_DOUBLE;

	case 0xc4:
	case 0xd9: // string/raw bytes with 8 bit header
		return bytes_internal_to_type(*(buf + 1), *buf);
	case 0xc5:
	case 0xda: // string/raw bytes with 16 bit header
		return bytes_internal_to_type(*(buf + 2),
				cf_swap_from_be16(*(uint16_t *)buf));
	case 0xc6:
	case 0xdb: // string/raw bytes with 32 bit header
		return bytes_internal_to_type(*(buf + 4),
				cf_swap_from_be32(*(uint32_t *)buf));
	case 0xdc: // list with 16 bit header
	case 0xdd: // list with 32 bit header
		return MSGPACK_TYPE_LIST;
	case 0xde: // map with 16 bit header
	case 0xdf: // map with 32 bit header
		return MSGPACK_TYPE_MAP;

	case 0xd4: // fixext 1
		if (*buf++ == CMP_EXT_TYPE) {
			if (*buf == CMP_WILDCARD) {
				return MSGPACK_TYPE_CMP_WILDCARD;
			}

			if (*buf == CMP_INF) {
				return MSGPACK_TYPE_CMP_INF;
			}
		}
		return MSGPACK_TYPE_ERROR;
	case 0xd5: // fixext 2
	case 0xd6: // fixext 4
	case 0xd7: // fixext 8
	case 0xd8: // fixext 16
		return MSGPACK_TYPE_EXT;

	case 0xc7: // ext 8
		if (*buf++ == 1) {
			if (*buf++ == CMP_EXT_TYPE) {
				if (*buf == CMP_WILDCARD) {
					return AS_CMP_WILDCARD;
				}

				if (*buf == CMP_INF) {
					return AS_CMP_INF;
				}
			}
		}

		return MSGPACK_TYPE_EXT;
	case 0xc8: // ext 16
	case 0xc9: // ext 32
		return MSGPACK_TYPE_EXT;
	default:
		break;
	}

	if (b < 0x80) { // 8 bit combined integer
		return MSGPACK_TYPE_INT;
	}

	if (b >= 0xe0) {
		return MSGPACK_TYPE_NEGINT;
	}

	if ((b & 0xe0) == 0xa0) { // raw bytes with 8 bit combined header
		return bytes_internal_to_type(*buf, (b & 0x1f));
	}

	if ((b & 0xf0) == 0x80) { // map with 8 bit combined header
		return MSGPACK_TYPE_MAP;
	}

	if ((b & 0xf0) == 0x90) { // list with 8 bit combined header
		return MSGPACK_TYPE_LIST;
	}

	return MSGPACK_TYPE_ERROR;
}

bool
msgpack_peek_is_ext(const msgpack_in *mp)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	uint8_t type = mp->buf[mp->offset];

	switch (type) {
	case 0xc7:
	case 0xc8:
	case 0xc9:
	case 0xd4:
	case 0xd5:
	case 0xd6:
	case 0xd7:
	case 0xd8:
		return true;
	default:
		break;
	}

	return false;
}

const uint8_t *
msgpack_skip(msgpack_in *mp, uint32_t *sz_r)
{
	const uint8_t *buf = mp->buf + mp->offset;
	uint32_t sz = msgpack_sz(mp);

	if (sz == 0) {
		return NULL;
	}

	*sz_r = sz;

	return buf;
}

bool
msgpack_get_bool(msgpack_in *mp, bool *value)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	uint8_t type = mp->buf[mp->offset];

	if (type == 0xc3 || type == 0xc2) {
		*value = type & 0x01;
		mp->offset++;
		return true;
	}

	return false;
}

bool
msgpack_get_uint64(msgpack_in *mp, uint64_t *i)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	const uint8_t *buf = mp->buf + mp->offset;
	uint8_t b = *buf++;

	switch (b) {
	case 0xcc: // unsigned 8 bit integer
		mp->offset += 2;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*i = (uint64_t)*buf;
		return true;
	case 0xcd: // unsigned 16 bit integer
	case 0xce: // unsigned 32 bit integer
	case 0xcf: // unsigned 64 bit integer
		b = 1U << (b - 0xcc);
		mp->offset += 1 + b;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*i = extract_uint64(buf, b);
		return true;
	case 0xd0: // signed 8 bit integer
		mp->offset += 2;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*i = (uint64_t)(int8_t)*buf;
		return true;
	case 0xd1: // signed 16 bit integer
	case 0xd2: // signed 32 bit integer
	case 0xd3: // signed 64 bit integer
		b = 1U << (b & 0x0f);
		mp->offset += 1 + b;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		if ((*buf & 0x80) != 0) {
			*i = extract_neg_int64(buf, b);
			return true;
		}

		*i = extract_uint64(buf, b);
		return true;
	default:
		break;
	}

	if (b < 0x80) { // 8 bit combined unsigned integer
		if (++mp->offset > mp->buf_sz) {
			return false;
		}

		*i = (uint64_t)b;
		return true;
	}

	if (b >= 0xe0) { // 8 bit combined negative integer
		if (++mp->offset > mp->buf_sz) {
			return false;
		}

		*i = (uint64_t)(int8_t)b;
		return true ;
	}

	return false;
}

bool
msgpack_get_double(msgpack_in *mp, double *x)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	const uint8_t *buf = mp->buf + mp->offset;

	switch (*buf++) {
	case 0xca: { // float
		mp->offset += 5;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		uint32_t i = cf_swap_from_be32(*(uint32_t *)buf);

		*x = (double)*(float *)&i;
		return true;
	}
	case 0xcb: { // double
		mp->offset += 9;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		uint64_t i = cf_swap_from_be64(*(uint64_t *)buf);

		*x = *(double *)&i;
		return true;
	}
	default:
		break;
	}

	return false;
}

const uint8_t *
msgpack_get_bin(msgpack_in *mp, uint32_t *sz_r)
{
	if (mp->offset >= mp->buf_sz) {
		return NULL;
	}

	const uint8_t *buf = mp->buf + mp->offset;
	uint8_t b = *buf++;

	switch (b) {
	case 0xc4:
	case 0xd9: // str/bin with 8 bit header
		mp->offset += 2;

		if (mp->offset > mp->buf_sz) {
			return NULL;
		}

		*sz_r = (uint32_t)*buf;
		break;
	case 0xc5:
	case 0xda: // str/bin with 16 bit header
		mp->offset += 3;

		if (mp->offset > mp->buf_sz) {
			return NULL;
		}

		*sz_r = (uint32_t)cf_swap_from_be16(*(uint16_t *)buf);
		break;
	case 0xc6:
	case 0xdb: // str/bin with 32 bit header
		mp->offset += 5;

		if (mp->offset > mp->buf_sz) {
			return NULL;
		}

		*sz_r = cf_swap_from_be32(*(uint32_t *)buf);
		break;
	default:
		if ((b & 0xe0) == 0xa0) { // str bytes with 8 bit combined header
			mp->offset++;
			*sz_r = (uint32_t)(b & 0x1f);
			break;
		}

		return NULL;
	}

	buf = mp->buf + mp->offset;
	mp->offset += *sz_r;

	if (mp->offset > mp->buf_sz) {
		return NULL;
	}

	return buf;
}

bool
msgpack_get_ext(msgpack_in *mp, msgpack_ext *ext)
{
	// Need at least 3 bytes.
	if (mp->buf_sz - mp->offset < 3) {
		return false;
	}

	const uint8_t *buf = mp->buf + mp->offset++;

	switch (*buf) {
	case 0xd4: // fixext 1
		ext->size = 1;
		break;
	case 0xd5: // fixext 2
		ext->size = 2;
		break;
	case 0xd6: // fixext 4
		ext->size = 4;
		break;
	case 0xd7: // fixext 8
		ext->size = 8;
		break;
	case 0xd8: // fixext 16
		ext->size = 16;
		break;
	case 0xc7: // ext 8
		mp->offset++;
		ext->size = (uint32_t)*(buf + 1);
		break;
	case 0xc8: // ext 16
		mp->offset += 2;

		if (mp->offset >= mp->buf_sz) {
			return false;
		}

		ext->size = (uint32_t)cf_swap_from_be16(*(uint16_t *)(buf + 1));
		break;
	case 0xc9: // ext 32
		mp->offset += 4;

		if (mp->offset >= mp->buf_sz) {
			return false;
		}

		ext->size = cf_swap_from_be32(*(uint32_t *)(buf + 1));
		break;
	default:
		return false;
	}

	ext->type_offset = mp->offset;
	ext->type = mp->buf[mp->offset++];
	ext->data = mp->buf + mp->offset;
	mp->offset += ext->size;

	if (mp->offset > mp->buf_sz) {
		return false;
	}

	return true;
}

bool
msgpack_get_list_ele_count(msgpack_in *mp, uint32_t *count_r)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	const uint8_t *buf = mp->buf + mp->offset;

	switch (*buf) {
	case 0xdc: // list with 16 bit header
		mp->offset += 3;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*count_r = (uint32_t)cf_swap_from_be16(*(uint16_t *)(buf + 1));
		break;
	case 0xdd: // list with 32 bit header
		mp->offset += 5;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*count_r = cf_swap_from_be32(*(uint32_t *)(buf + 1));
		break;
	default:
		if ((*buf & 0xf0) == 0x90) { // list with 8 bit combined header
			mp->offset++;
			*count_r = (uint32_t)(*buf & 0x0f);
			break;
		}

		return false;
	}

	return true;
}

bool
msgpack_get_map_ele_count(msgpack_in *mp, uint32_t *count_r)
{
	if (mp->offset >= mp->buf_sz) {
		return false;
	}

	const uint8_t *buf = mp->buf + mp->offset;

	switch (*buf) {
	case 0xde: // map with 16 bit header
		mp->offset += 3;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*count_r = (uint32_t)cf_swap_from_be16(*(uint16_t *)(buf + 1));
		break;
	case 0xdf: // map with 32 bit header
		mp->offset += 5;

		if (mp->offset > mp->buf_sz) {
			return false;
		}

		*count_r = cf_swap_from_be32(*(uint32_t *)(buf + 1));
		break;
	default:
		if ((*buf & 0xf0) == 0x80) { // map with 8 bit combined header
			mp->offset++;
			*count_r = (uint32_t)(*buf & 0x0f);
			break;
		}

		return false;
	}

	return true;
}


//==========================================================
// Local helpers.
//

static inline msgpack_type
bytes_internal_to_msgpack_type(uint8_t type, uint32_t len)
{
	if (len == 0) {
		return MSGPACK_TYPE_BYTES;
	}

	if (type == AS_BYTES_STRING) {
		return MSGPACK_TYPE_STRING;
	}

	if (type == AS_BYTES_GEOJSON) {
		return MSGPACK_TYPE_GEOJSON;
	}

	// All other types are considered BYTES.
	return MSGPACK_TYPE_BYTES;
}

static inline msgpack_type
bytes_internal_to_type(uint8_t type, uint32_t len)
{
	if (len == 0) {
		return MSGPACK_TYPE_BYTES;
	}

	if (type == AS_BYTES_STRING) {
		return MSGPACK_TYPE_STRING;
	}

	if (type == AS_BYTES_GEOJSON) {
		return MSGPACK_TYPE_GEOJSON;
	}

	// All other types are considered BYTES.
	return MSGPACK_TYPE_BYTES;
}

static inline const uint8_t *
msgpack_sz_table(const uint8_t *buf, const uint8_t * const end, uint32_t *count,
		bool *has_nonstorage)
{
	SZ_PARSE_BUF_CHECK(buf, end, 1);

	uint8_t b = *buf++;

	switch (b) {
	case 0xc0: // nil
	case 0xc2: // boolean false
	case 0xc3: // boolean true
		return buf;

	case 0xcc: // unsigned 8 bit integer
	case 0xd0: // signed 8 bit integer
		return buf + 1;

	case 0xcd: // unsigned 16 bit integer
	case 0xd1: // signed 16 bit integer
		return buf + 2;

	case 0xca: // float
	case 0xce: // unsigned 32 bit integer
	case 0xd2: // signed 32 bit integer
		return buf + 4;

	case 0xcb: // double
	case 0xcf: // unsigned 64 bit integer
	case 0xd3: // signed 64 bit integer
		return buf + 8;

	case 0xc4:
	case 0xd9: // string/raw bytes with 8 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 1);
		return buf + 1 + *buf;

	case 0xc5:
	case 0xda: // string/raw bytes with 16 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 2);
		return buf + 2 + cf_swap_from_be16(*(uint16_t *)buf);

	case 0xc6:
	case 0xdb: // string/raw bytes with 32 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 4);
		return buf + 4 + cf_swap_from_be32(*(uint32_t *)buf);

	case 0xdc: // list with 16 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 2);
		*count += cf_swap_from_be16(*(uint16_t *)buf);
		return buf + 2;
	case 0xdd: { // list with 32 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 4);
		*count += cf_swap_from_be32(*(uint32_t *)buf);
		return buf + 4;
	}
	case 0xde: // map with 16 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 2);
		*count += 2 * cf_swap_from_be16(*(uint16_t *)buf);
		return buf + 2;
	case 0xdf: // map with 32 bit header
		SZ_PARSE_BUF_CHECK(buf, end, 4);
		*count += 2 * cf_swap_from_be32(*(uint32_t *)buf);
		return buf + 4;

	case 0xd4: // fixext 1
		SZ_PARSE_BUF_CHECK(buf, end, 1);

		if (*buf == CMP_EXT_TYPE) {
			*has_nonstorage = true;
		}

		return buf + 1 + 1;
	case 0xd5: // fixext 2
		SZ_PARSE_BUF_CHECK(buf, end, 1);

		if (*buf == CMP_EXT_TYPE) {
			*has_nonstorage = true;
		}

		return buf + 1 + 2;
	case 0xd6: // fixext 4
		return buf + 1 + 4;
	case 0xd7: // fixext 8
		return buf + 1 + 8;
	case 0xd8: // fixext 16
		return buf + 1 + 16;
	case 0xc7: // ext 8
		SZ_PARSE_BUF_CHECK(buf, end, 2);

		if (*(buf + 1) == CMP_EXT_TYPE && *buf < 4 && *buf != 0) {
			*has_nonstorage = true;
		}

		return buf + 1 + 1 + *buf;
	case 0xc8: { // ext 16
		SZ_PARSE_BUF_CHECK(buf, end, 3);

		uint32_t len = cf_swap_from_be16(*(uint16_t *)buf);

		if (*(buf + 2) == CMP_EXT_TYPE && len < 4 && len != 0) {
			*has_nonstorage = true;
		}

		return buf + 2 + 1 + len;
	}
	case 0xc9: { // ext 32
		SZ_PARSE_BUF_CHECK(buf, end, 5);

		uint32_t len = cf_swap_from_be32(*(uint32_t *)buf);

		if (*(buf + 4) == CMP_EXT_TYPE && len < 4 && len != 0) {
			*has_nonstorage = true;
		}

		return buf + 4 + 1 + len;
	}
	default:
		break;
	}

	if (b < 0x80 || b >= 0xe0) { // 8 bit combined integer
		return buf;
	}

	if ((b & 0xe0) == 0xa0) { // raw bytes with 8 bit combined header
		return buf + (b & 0x1f);
	}

	if ((b & 0xf0) == 0x80) { // map with 8 bit combined header
		*count += 2 * (b & 0x0f);
		return buf;
	}

	if ((b & 0xf0) == 0x90) { // list with 8 bit combined header
		*count += b & 0x0f;
		return buf;
	}

	return NULL;
}

static inline const uint8_t *
msgpack_sz_internal(const uint8_t *buf, const uint8_t * const end,
		uint32_t count, bool *has_nonstorage)
{
	for (uint32_t i = 0; i < count; i++) {
		buf = msgpack_sz_table(buf, end, &count, has_nonstorage);

		if (buf > end || buf == NULL) {
			cf_warning(AS_PARTICLE, "msgpack_sz_internal: invalid at i %u count %u", i, count);
			return NULL;
		}
	}

	return buf;
}

static inline uint64_t
extract_uint64(const uint8_t *ptr, uint8_t sz)
{
	const uint64_t *p64 = (const uint64_t *)(ptr - 8 + sz);
	return cf_swap_from_be64(*p64) & ((~0ULL) >> (64 - 8 * sz)); // little endian mask
}

static inline uint64_t
extract_neg_int64(const uint8_t *ptr, uint8_t sz)
{
	const uint64_t *p64 = (const uint64_t *)(ptr - 8 + sz);
	return cf_swap_from_be64(*p64) | ~((~0ULL) >> (64 - 8 * sz)); // little endian mask
}

static inline void
cmp_parse_container(parse_meta *meta, uint32_t count)
{
	if (meta->len == 0) {
		return;
	}

	const uint8_t *buf = meta->buf;
	uint8_t type;

	CMP_PARSE_BUF_CHECK(meta, 1);

	switch (*buf++) {
	case 0xc7: // ext 8
		CMP_PARSE_BUF_CHECK(meta, 2);
		type = *(buf + 1);
		break;
	case 0xc8: // ext 16
		CMP_PARSE_BUF_CHECK(meta, 3);
		type = *(buf + 2);
		break;
	case 0xc9: // ext 32
		CMP_PARSE_BUF_CHECK(meta, 5);
		type = *(buf + 4);
		break;
	case 0xd4: // fixext 1
	case 0xd5: // fixext 2
	case 0xd6: // fixext 4
	case 0xd7: // fixext 8
	case 0xd8: // fixext 16
		CMP_PARSE_BUF_CHECK(meta, 1);
		type = *buf;
		break;
	default:
		// not an ext type
		return;
	}

	if (type == CMP_EXT_TYPE) {
		// non-storage type
		return;
	}

	// skip meta elements
	meta->buf = msgpack_sz_internal(meta->buf, meta->end, count,
			&meta->has_nonstorage);
	meta->len -= count;
}

static inline void
msgpack_cmp_parse(parse_meta *meta)
{
	CMP_PARSE_BUF_CHECK(meta, 1);

	uint8_t b = *meta->buf++;

	switch (b) {
	case 0xc0: // nil
		meta->type = MSGPACK_TYPE_NIL;
		return;
	case 0xc3: // boolean true
		meta->type = MSGPACK_TYPE_TRUE;
		return;
	case 0xc2: // boolean false
		meta->type = MSGPACK_TYPE_FALSE;
		return;

	case 0xcc: // unsigned 8 bit integer
		CMP_PARSE_BUF_CHECK(meta, 1);
		meta->i_num = (uint64_t)*meta->buf;
		meta->buf++;
		meta->type = MSGPACK_TYPE_INT;
		return;
	case 0xcd: // unsigned 16 bit integer
	case 0xce: // unsigned 32 bit integer
	case 0xcf: // unsigned 64 bit integer
		b = 1U << (b - 0xcc);
		CMP_PARSE_BUF_CHECK(meta, b);
		meta->i_num = extract_uint64(meta->buf, b);
		meta->buf += b;
		meta->type = MSGPACK_TYPE_INT;
		return;

	case 0xd0: // signed 8 bit integer
		CMP_PARSE_BUF_CHECK(meta, 1);

		if ((*meta->buf & 0x80) != 0) {
			meta->i_num = (uint64_t)(int8_t)*meta->buf;
			meta->type = MSGPACK_TYPE_NEGINT;
		}
		else {
			meta->i_num = (uint64_t)*meta->buf;
			meta->type = MSGPACK_TYPE_INT;
		}

		meta->buf++;
		return;
	case 0xd1: // signed 16 bit integer
	case 0xd2: // signed 32 bit integer
	case 0xd3: // signed 64 bit integer
		b = 1U << (b & 0x0f);
		CMP_PARSE_BUF_CHECK(meta, b);

		if ((*meta->buf & 0x80) != 0) {
			meta->i_num = extract_neg_int64(meta->buf, b);
			meta->type = MSGPACK_TYPE_NEGINT;
		}
		else {
			meta->i_num = extract_uint64(meta->buf, b);
			meta->type = MSGPACK_TYPE_INT;
		}

		meta->buf += b;
		return;

	case 0xca: { // float
		CMP_PARSE_BUF_CHECK(meta, 4);

		uint32_t i = cf_swap_from_be32(*(uint32_t *)meta->buf);

		meta->d_num = (double)*(float *)&i;
		meta->buf += 4;
		meta->type = MSGPACK_TYPE_DOUBLE;
		break;
	}
	case 0xcb: { // double
		CMP_PARSE_BUF_CHECK(meta, 8);

		uint64_t i = cf_swap_from_be64(*(uint64_t *)meta->buf);

		meta->d_num = *(double *)&i;
		meta->buf += 8;
		meta->type = MSGPACK_TYPE_DOUBLE;
		return;
	}

	case 0xc4:
	case 0xd9: // string/raw bytes with 8 bit header
		CMP_PARSE_BUF_CHECK(meta, 1);
		meta->data = meta->buf + 1;
		meta->len = *meta->buf;
		meta->buf += 1 + meta->len;
		CMP_PARSE_BUF_CHECK(meta, 0);
		meta->type = bytes_internal_to_msgpack_type(*meta->data, meta->len);
		return;

	case 0xc5:
	case 0xda: // string/raw bytes with 16 bit header
		CMP_PARSE_BUF_CHECK(meta, 2);
		meta->data = meta->buf + 2;
		meta->len = cf_swap_from_be16(*(uint16_t *)meta->buf);
		meta->buf += 2 + meta->len;
		CMP_PARSE_BUF_CHECK(meta, 0);
		meta->type = bytes_internal_to_msgpack_type(*meta->data, meta->len);
		return;

	case 0xc6:
	case 0xdb: // string/raw bytes with 32 bit header
		CMP_PARSE_BUF_CHECK(meta, 4);
		meta->data = meta->buf + 4;
		meta->len = cf_swap_from_be32(*(uint32_t *)meta->buf);
		meta->buf += 4 + meta->len;
		CMP_PARSE_BUF_CHECK(meta, 0);
		meta->type = bytes_internal_to_msgpack_type(*meta->data, meta->len);
		return;

	case 0xdc: { // list with 16 bit header
		CMP_PARSE_BUF_CHECK(meta, 2);
		meta->len = cf_swap_from_be16(*(uint16_t *)meta->buf);
		meta->buf += 2;
		meta->type = MSGPACK_TYPE_LIST;
		cmp_parse_container(meta, 1);
		return;
	}
	case 0xdd: { // list with 32 bit header
		CMP_PARSE_BUF_CHECK(meta, 4);
		meta->len = cf_swap_from_be32(*(uint32_t *)meta->buf);
		meta->buf += 4;
		meta->type = MSGPACK_TYPE_LIST;
		cmp_parse_container(meta, 1);
		return;
	}
	case 0xde: // map with 16 bit header
		CMP_PARSE_BUF_CHECK(meta, 2);
		meta->len = 2 * cf_swap_from_be16(*(uint16_t *)meta->buf);
		meta->buf += 2;
		meta->type = MSGPACK_TYPE_MAP;
		cmp_parse_container(meta, 2);
		return;
	case 0xdf: // map with 32 bit header
		CMP_PARSE_BUF_CHECK(meta, 4);
		meta->len = 2 * cf_swap_from_be32(*(uint32_t *)meta->buf);
		meta->buf += 4;
		meta->type = MSGPACK_TYPE_MAP;
		cmp_parse_container(meta, 2);
		return;

	case 0xd4: // fixext 1
		meta->len = 1;

		if (*meta->buf++ == CMP_EXT_TYPE) {
			meta->has_nonstorage = true;

			if (*meta->buf == CMP_WILDCARD) {
				meta->buf++;
				meta->type = MSGPACK_TYPE_CMP_WILDCARD;
				return;
			}

			if (*meta->buf == CMP_INF) {
				meta->buf++;
				meta->type = MSGPACK_TYPE_CMP_INF;
				return;
			}
		}

		meta->data = meta->buf;
		meta->type = MSGPACK_TYPE_EXT;
		meta->buf++;
		return;
	case 0xd5: // fixext 2
		meta->len = 2;

		if (*meta->buf++ == CMP_EXT_TYPE) {
			meta->has_nonstorage = true;
		}

		meta->data = meta->buf;
		meta->buf += 2;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	case 0xd6: // fixext 4
		meta->len = 4;
		meta->data = ++meta->buf;
		meta->buf += 4;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	case 0xd7: // fixext 8
		meta->len = 8;
		meta->data = ++meta->buf;
		meta->buf += 8;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	case 0xd8: // fixext 16
		meta->len = 16;
		meta->data = ++meta->buf;
		meta->buf += 16;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	case 0xc7: // ext 8
		meta->len = *meta->buf++;

		if (*meta->buf++ == CMP_EXT_TYPE && meta->len < 4 && meta->len != 0) {
			meta->has_nonstorage = true;

			if (meta->len == 1) {
				if (*meta->buf == CMP_WILDCARD) {
					meta->buf++;
					meta->type = MSGPACK_TYPE_CMP_WILDCARD;
					return;
				}

				if (*meta->buf == CMP_INF) {
					meta->buf++;
					meta->type = MSGPACK_TYPE_CMP_INF;
					return;
				}
			}
		}

		meta->data = meta->buf;
		meta->buf += meta->len;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	case 0xc8: { // ext 16
		meta->len = cf_swap_from_be16(*(uint16_t *)meta->buf);
		meta->buf += 2;

		if (*meta->buf++ == CMP_EXT_TYPE && meta->len < 4 && meta->len != 0) {
			meta->has_nonstorage = true;
		}

		meta->buf += meta->len;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	}
	case 0xc9: { // ext 32
		meta->len = cf_swap_from_be32(*(uint32_t *)meta->buf);
		meta->buf += 4;

		if (*meta->buf++ == CMP_EXT_TYPE && meta->len < 4 && meta->len != 0) {
			meta->has_nonstorage = true;
		}

		meta->buf += meta->len;
		meta->type = MSGPACK_TYPE_EXT;
		return;
	}

	default:
		break;
	}

	if (b < 0x80) { // 8 bit combined unsigned integer
		meta->i_num = b;
		meta->type = MSGPACK_TYPE_INT;
		return;
	}
	if (b >= 0xe0) { // 8 bit combined negative integer
		meta->i_num = (uint64_t)(int8_t)b;
		meta->type = MSGPACK_TYPE_NEGINT;
		return;
	}

	if ((b & 0xe0) == 0xa0) { // raw bytes with 8 bit combined header
		meta->data = meta->buf;
		meta->len = b & 0x1f;
		meta->buf += meta->len;
		CMP_PARSE_BUF_CHECK(meta, 0);
		meta->type = bytes_internal_to_msgpack_type(*meta->data, meta->len);
		return;
	}

	if ((b & 0xf0) == 0x80) { // map with 8 bit combined header
		meta->len = 2 * (b & 0x0f);
		meta->type = MSGPACK_TYPE_MAP;
		cmp_parse_container(meta, 2);
		return;
	}

	if ((b & 0xf0) == 0x90) { // list with 8 bit combined header
		meta->len = b & 0x0f;
		meta->type = MSGPACK_TYPE_LIST;
		cmp_parse_container(meta, 1);
		return;
	}

	meta->type = MSGPACK_TYPE_ERROR;
}

static inline msgpack_cmp_type
msgpack_cmp_internal(parse_meta *meta0, parse_meta *meta1)
{
	uint32_t min_count = 1;
	msgpack_cmp_type end_result = MSGPACK_CMP_EQUAL;

	for (uint32_t i = 0; i < min_count; i++) {
		meta0->remain--;
		meta1->remain--;

		msgpack_cmp_parse(meta0);
		msgpack_cmp_parse(meta1);

		if (meta0->buf == NULL || meta0->type == MSGPACK_TYPE_ERROR ||
				meta0->buf > meta0->end ||
				meta1->buf == NULL || meta1->type == MSGPACK_TYPE_ERROR ||
				meta1->buf > meta1->end) {
			return MSGPACK_CMP_ERROR;
		}

		if (meta0->type != meta1->type) {
			if (meta0->type == MSGPACK_TYPE_LIST ||
					meta0->type == MSGPACK_TYPE_MAP) {
				meta0->remain += meta0->len;
			}

			if (meta1->type == MSGPACK_TYPE_LIST ||
					meta1->type == MSGPACK_TYPE_MAP) {
				meta1->remain += meta1->len;
			}
		}

		if (meta0->type == MSGPACK_TYPE_CMP_WILDCARD ||
				meta1->type == MSGPACK_TYPE_CMP_WILDCARD) {
			return MSGPACK_CMP_EQUAL;
		}

		MSGPACK_CMP_RETURN(meta0->type, meta1->type);

		switch (meta0->type) {
		case MSGPACK_TYPE_NIL:
		case MSGPACK_TYPE_FALSE:
		case MSGPACK_TYPE_TRUE:
			break;
		case MSGPACK_TYPE_NEGINT:
		case MSGPACK_TYPE_INT:
			MSGPACK_CMP_RETURN(meta0->i_num, meta1->i_num);
			break;

		case MSGPACK_TYPE_EXT:
		case MSGPACK_TYPE_STRING:
		case MSGPACK_TYPE_BYTES:
		case MSGPACK_TYPE_GEOJSON: {
			size_t len = (meta0->len < meta1->len) ? meta0->len : meta1->len;
			int cmp = memcmp(meta0->data, meta1->data, len);

			MSGPACK_CMP_RETURN(cmp, 0);
			MSGPACK_CMP_RETURN(meta0->len, meta1->len);

			break;
		}

		case MSGPACK_TYPE_LIST:
			if (meta0->len == meta1->len) {
				meta0->remain += meta0->len;
				meta1->remain += meta1->len;
				min_count += meta0->len;
				break;
			}

			min_count = meta0->len;
			end_result = MSGPACK_CMP_LESS;

			if (min_count > meta1->len) {
				min_count = meta1->len;
				end_result = MSGPACK_CMP_GREATER;
			}

			meta0->remain += meta0->len;
			meta1->remain += meta1->len;
			min_count += i + 1;

			break;
		case MSGPACK_TYPE_MAP:
			meta0->remain += meta0->len;
			meta1->remain += meta1->len;
			MSGPACK_CMP_RETURN(meta0->len, meta1->len);
			min_count += meta0->len;
			break;

		case MSGPACK_TYPE_DOUBLE:
			MSGPACK_CMP_RETURN(meta0->d_num, meta1->d_num);
			break;

		default:
			break;
		}
	}

	return end_result;
}
