/*
 * Copyright (C) 2007-2012 Free Software Foundation, Inc.
 *
 * Author: Simon Josefsson
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

/* This file contains support functions for 'TLS Handshake Message for
 * Supplemental Data' (RFC 4680).
 *
 * The idea here is simple.  gnutls_handshake() in gnuts_handshake.c
 * will call _gnutls_gen_supplemental and _gnutls_parse_supplemental
 * when some extension requested that supplemental data be sent or
 * received.  Extension request this by setting the flags
 * do_recv_supplemental or do_send_supplemental in the session.
 *
 * The functions in this file iterate through the _gnutls_supplemental
 * array, and calls the send/recv functions for each respective data
 * type.
 *
 * The receive function of each data type is responsible for decoding
 * its own data.  If the extension did not expect to receive
 * supplemental data, it should return GNUTLS_E_UNEXPECTED_PACKET.
 * Otherwise, it just parse the data as normal.
 *
 * The send function needs to append the 2-byte data format type, and
 * append the 2-byte length of its data, and the data.  If it doesn't
 * want to send any data, it is fine to return without doing anything.
 */

#include <gnutls/gnutls.h>
#include "gnutls_int.h"
#include "gnutls_supplemental.h"
#include "gnutls_errors.h"
#include "gnutls_num.h"

typedef struct {
	const char *name;
	gnutls_supplemental_data_format_type_t type;
	supp_recv_func supp_recv_func;
	supp_send_func supp_send_func;
} gnutls_supplemental_entry;

static size_t suppfunc_size = 0;
static gnutls_supplemental_entry *suppfunc = NULL;

/**
 * gnutls_supplemental_get_name:
 * @type: is a supplemental data format type
 *
 * Convert a #gnutls_supplemental_data_format_type_t value to a
 * string.
 *
 * Returns: a string that contains the name of the specified
 *   supplemental data format type, or %NULL for unknown types.
 **/
const char
    *gnutls_supplemental_get_name(gnutls_supplemental_data_format_type_t
				  type)
{
	size_t i;

	for (i = 0; i < suppfunc_size; i++) {
		if (suppfunc[i].type == type)
			return suppfunc[i].name;
	}

	return NULL;
}

static supp_recv_func
get_supp_func_recv(gnutls_supplemental_data_format_type_t type)
{
	size_t i;

	for (i = 0; i < suppfunc_size; i++) {
		if (suppfunc[i].type == type)
			return suppfunc[i].supp_recv_func;
	}

	return NULL;
}

int
_gnutls_gen_supplemental(gnutls_session_t session, gnutls_buffer_st * buf)
{
	size_t i;
	gnutls_supplemental_entry *p;
	int ret;

	/* Make room for 3 byte length field. */
	ret = _gnutls_buffer_append_data(buf, "\0\0\0", 3);
	if (ret < 0) {
		gnutls_assert();
		return ret;
	}

	for (i = 0; i < suppfunc_size; i++) {
		p = &suppfunc[i];
		supp_send_func supp_send = p->supp_send_func;
		size_t sizepos = buf->length;

		/* Make room for supplement type and length byte length field. */
		ret = _gnutls_buffer_append_data(buf, "\0\0\0\0", 4);
		if (ret < 0) {
			gnutls_assert();
			return ret;
		}

		ret = supp_send(session, buf);
		if (ret < 0) {
			gnutls_assert();
			return ret;
		}

		/* If data were added, store type+length, otherwise reset. */
		if (buf->length > sizepos + 4) {
			buf->data[sizepos] = (p->type >> 8) & 0xFF;
			buf->data[sizepos + 1] = p->type & 0xFF;
			buf->data[sizepos + 2] =
			    ((buf->length - sizepos - 4) >> 8) & 0xFF;
			buf->data[sizepos + 3] =
			    (buf->length - sizepos - 4) & 0xFF;
		} else
			buf->length -= 4;
	}

	buf->data[0] = ((buf->length - 3) >> 16) & 0xFF;
	buf->data[1] = ((buf->length - 3) >> 8) & 0xFF;
	buf->data[2] = (buf->length - 3) & 0xFF;

	_gnutls_debug_log
	    ("EXT[%p]: Sending %d bytes of supplemental data\n", session,
	     (int) buf->length);

	return buf->length;
}

int
_gnutls_parse_supplemental(gnutls_session_t session,
			   const uint8_t * data, int datalen)
{
	const uint8_t *p = data;
	ssize_t dsize = datalen;
	size_t total_size;

	DECR_LEN(dsize, 3);
	total_size = _gnutls_read_uint24(p);
	p += 3;

	if (dsize != (ssize_t) total_size) {
		gnutls_assert();
		return GNUTLS_E_RECEIVED_ILLEGAL_PARAMETER;
	}

	do {
		uint16_t supp_data_type;
		uint16_t supp_data_length;
		supp_recv_func recv_func;

		DECR_LEN(dsize, 2);
		supp_data_type = _gnutls_read_uint16(p);
		p += 2;

		DECR_LEN(dsize, 2);
		supp_data_length = _gnutls_read_uint16(p);
		p += 2;

		_gnutls_debug_log
		    ("EXT[%p]: Got supplemental type=%02x length=%d\n",
		     session, supp_data_type, supp_data_length);

		recv_func = get_supp_func_recv(supp_data_type);
		if (recv_func) {
			int ret = recv_func(session, p, supp_data_length);
			if (ret < 0) {
				gnutls_assert();
				return ret;
			}
		} else {
			gnutls_assert();
			return GNUTLS_E_RECEIVED_ILLEGAL_PARAMETER;
		}

		DECR_LEN(dsize, supp_data_length);
		p += supp_data_length;
	}
	while (dsize > 0);

	return 0;
}

static int
_gnutls_supplemental_register(gnutls_supplemental_entry *entry)
{
	gnutls_supplemental_entry *p;

	p = gnutls_realloc_fast(suppfunc,
				sizeof(*suppfunc) * (suppfunc_size + 1));
	if (!p) {
		gnutls_assert();
		return GNUTLS_E_MEMORY_ERROR;
	}

	suppfunc = p;

	memcpy(&suppfunc[suppfunc_size], entry, sizeof(*entry));

	suppfunc_size++;

	return GNUTLS_E_SUCCESS;
}

/**
 * gnutls_supplemental_register:
 * @name: the name of the supplemental data to register
 * @type: the type of the supplemental data format
 * @recv_func: the function to receive the data
 * @send_func: the function to send the data
 *
 * This function will register a new supplemental data type (rfc4680).
 *
 * Returns: %GNUTLS_E_SUCCESS on success, otherwise a negative error code.
 *
 * Since: 3.4.0
 **/
int
gnutls_supplemental_register(const char *name, gnutls_supplemental_data_format_type_t type,
                             supp_recv_func recv_func, supp_send_func send_func)
{
	gnutls_supplemental_entry tmp_entry;

	tmp_entry.name = name;
	tmp_entry.type = type;
	tmp_entry.supp_recv_func = recv_func;
	tmp_entry.supp_send_func = send_func;
	
	return _gnutls_supplemental_register(&tmp_entry);
}

void
gnutls_do_recv_supplemental(gnutls_session_t session, int do_recv_supplemental)
{
	session->security_parameters.do_recv_supplemental = do_recv_supplemental;
}

void
gnutls_do_send_supplemental(gnutls_session_t session, int do_send_supplemental)
{
	session->security_parameters.do_send_supplemental = do_send_supplemental;
}
