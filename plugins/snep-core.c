/*
 *
 *  neard - Near Field Communication manager
 *
 *  Copyright (C) 2012  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include <linux/socket.h>

#include <near/nfc_copy.h>
#include <near/plugin.h>
#include <near/log.h>
#include <near/types.h>
#include <near/adapter.h>
#include <near/device.h>
#include <near/ndef.h>
#include <near/tlv.h>

#include "p2p.h"
#include "snep-core.h"

struct snep_fragment {
	uint32_t len;
	uint8_t *data;
};

struct p2p_snep_put_req_data {
	uint8_t fd;
	uint32_t adapter_idx;
	uint32_t target_idx;
	near_device_io_cb cb;
	guint watch;

	GSList *fragments;
};

struct p2p_snep_req_frame {
	uint8_t version;
	uint8_t request;
	uint32_t length;
	uint8_t ndef[];
} __attribute__((packed));

struct p2p_snep_resp_frame {
	uint8_t version;
	uint8_t response;
	uint32_t length;
	uint8_t info[];
} __attribute__((packed));

static GHashTable *snep_client_hash;

/* Callback: free snep data */
static void free_snep_core_client(gpointer data)
{
	struct p2p_snep_data *snep_data = data;

	DBG("");

	g_free(snep_data->nfc_data);
	g_free(snep_data);
}

/* Send a short response code */
void snep_core_response_noinfo(int client_fd, uint8_t response)
{
	struct p2p_snep_resp_frame resp;

	DBG("Response 0x%x", response);

	resp.version = SNEP_VERSION;
	resp.response = response;
	resp.length = 0;

	send(client_fd, &resp, sizeof(resp), 0);
}

/*
 * snep_core_parse_handover_record
 *
 * The hr frame should be here BUT:
 *	The first 4 bytes are the Max Allowed Length
 *
 * - Because of an Android's BUGs:
 *	- the Hr frame is not correct; a Hr record
 *	is embedded in a ... Hr record !!! The author
 *	used 'Hr' instead of 'cr'
 *	- The OOB block is badly written:
 *	- the payload ID should be the same in the 'ac' record
 *		and the OOB record.
 *	- The OOB data length bytes must be swapped (Big endian to Little E.)
 *
 * The hack fixes the first issue (bluetooth.c fixes the second) !
 * */
void snep_core_parse_handover_record(int client_fd, uint8_t *ndef,
		uint32_t nfc_data_length)
{
	GList *records;
	struct near_ndef_message *msg = NULL;

	if (ndef == NULL)
		return;

	/*
	 * Bugfix Android: Fix 'cr' instead of 'Hr'
	 * Bug is in Google:HandoverManager.java:645
	 */
	if (nfc_data_length > 9 && strncmp((char *)(ndef + 9), "Hr", 2) == 0) {
		DBG("Android 4.1.1 found !!!");
		*(ndef + 9) = 'c';
	}

	/* Parse the incoming frame */
	records = near_ndef_parse_msg(ndef, nfc_data_length, &msg);
	if (records == NULL)
		return;

	near_ndef_records_free(records);

	if (!msg)
		return;

	near_info("Send SNEP / Hs frame");

	snep_core_response_with_info(client_fd, SNEP_RESP_SUCCESS, msg->data,
								msg->length);

	g_free(msg->data);
	g_free(msg);
}

/*
 * This code will read the ndef message.
 * return	 <0 on error
 *		==0 if not more bytes
 *		>0 if there's still some data to read
 */
static int snep_core_read_ndef(int client_fd,
					struct p2p_snep_data *snep_data)
{
	int bytes_recv, remaining_bytes;

	DBG("");

	remaining_bytes = snep_data->nfc_data_length -
					snep_data->nfc_data_current_length;

	bytes_recv = recv(client_fd, snep_data->nfc_data_ptr, remaining_bytes,
								MSG_DONTWAIT);
	if (bytes_recv < 0) {
		near_error("%d %s", bytes_recv, strerror(errno));

		/* Some more data should show up */
		if (errno == EAGAIN)
			return EAGAIN;	/* Positive !!*/

		goto out;
	}

	snep_data->nfc_data_current_length += bytes_recv;
	snep_data->nfc_data_ptr += bytes_recv;

	/* Is the read complete ? */
	if (snep_data->nfc_data_length == snep_data->nfc_data_current_length)
		return 0;

	if (snep_data->respond_continue == FALSE) {
		snep_data->respond_continue = TRUE;
		snep_core_response_noinfo(client_fd, SNEP_RESP_CONTINUE);
	}

	return 1;

out:
	g_hash_table_remove(snep_client_hash, GINT_TO_POINTER(client_fd));

	return -errno;		/* Negative on error */
}

static void free_snep_core_fragment(gpointer data)
{
	struct snep_fragment *fragment = data;

	if (fragment != NULL)
		g_free(fragment->data);

	g_free(fragment);
	fragment = NULL;
}

static void free_snep_core_push_data(gpointer userdata, int status)
{
	struct p2p_snep_put_req_data *data;

	DBG("");

	if (userdata == NULL)
		return;

	data = (struct p2p_snep_put_req_data *) userdata;

	close(data->fd);

	if (data->cb)
		data->cb(data->adapter_idx, data->target_idx, status);

	if (data->watch > 0)
		g_source_remove(data->watch);

	g_slist_free_full(data->fragments, free_snep_core_fragment);
	g_free(data);
}

static int snep_core_send_fragment(struct p2p_snep_put_req_data *req)
{
	struct snep_fragment *fragment;
	int err;

	DBG("");

	if (req == NULL || req->fragments == NULL ||
		g_slist_length(req->fragments) == 0)
		return -EINVAL;

	fragment = req->fragments->data;

	err = send(req->fd, fragment->data, fragment->len, 0);

	req->fragments = g_slist_remove(req->fragments, fragment);
	g_free(fragment->data);
	g_free(fragment);

	return err;
}

static int snep_core_push_response(struct p2p_snep_put_req_data *req)
{
	struct p2p_snep_resp_frame frame;
	uint8_t *ndef;
	uint32_t ndef_len;
	int bytes_recv, err;

	DBG("");

	bytes_recv = recv(req->fd, &frame, sizeof(frame), 0);
	if (bytes_recv < 0) {
		near_error("Read SNEP frame error %d %s", bytes_recv,
							strerror(errno));
		return bytes_recv;
	}

	/* Check frame length */
	frame.length = g_ntohl(frame.length);

	DBG("Response 0x%x %p", frame.response, &frame);
	switch (frame.response) {
	case SNEP_RESP_CONTINUE:
		while (g_slist_length(req->fragments) != 0) {
			err = snep_core_send_fragment(req);
			if (err < 0)
				return err;
		}

		return frame.response;

	case SNEP_RESP_SUCCESS:
		if (frame.length == 0)
			return 0;

		/* Get the incoming data */
		ndef_len = frame.length;
		ndef = g_try_malloc0(ndef_len);
		if (ndef == NULL)
			return -ENOMEM;

		bytes_recv = recv(req->fd, ndef, ndef_len, 0);
		if (bytes_recv < 0) {
			near_error("Read SNEP frame error: %d %s", bytes_recv,
							strerror(errno));
			return bytes_recv;
		}

		/* Not enough bytes */
		if (bytes_recv < 6)
			return -EINVAL;

		if (strncmp((char *)(ndef + 3), "Hs", 2) == 0)
			snep_core_parse_handover_record(req->fd, ndef,
								ndef_len);

		g_free(ndef);

		return 0;
	}

	return -1;
}

static gboolean snep_core_push_event(GIOChannel *channel,
				GIOCondition condition,	gpointer data)
{
	int err;

	DBG("push_event condition 0x%x", condition);

	if (condition & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {

		near_error("Error with SNEP channel");

		free_snep_core_push_data(data, -1);

		return FALSE;
	}

	err = snep_core_push_response(data);
	if (err <= 0) {
		free_snep_core_push_data(data, err);

		return FALSE;
	}

	return TRUE;
}

static int snep_core_push_prepare_fragments(struct p2p_snep_put_req_data *req,
						struct near_ndef_message *ndef)
{
	struct snep_fragment *fragment;
	uint32_t max_fragment_len;

	DBG("");

	max_fragment_len = SNEP_REQ_MAX_FRAGMENT_LENGTH;

	while (ndef->offset < ndef->length) {

		fragment = g_try_malloc0(sizeof(struct snep_fragment));
		if (fragment == NULL)
			return -ENOMEM;

		if (max_fragment_len <= (ndef->length - ndef->offset))
			fragment->len = max_fragment_len;
		else
			fragment->len = ndef->length - ndef->offset;

		fragment->data = g_try_malloc0(fragment->len);
		if (fragment->data == NULL) {
			g_free(fragment);
			return -ENOMEM;
		}

		memcpy(fragment->data, ndef->data + ndef->offset,
					fragment->len);
		ndef->offset += fragment->len;
		req->fragments = g_slist_append(req->fragments, fragment);
	}

	return 0;
}

static near_bool_t snep_core_process_request(int client_fd,
					struct p2p_snep_data *snep_data,
					struct p2p_snep_req_frame *frame,
					near_server_io req_get,
					near_server_io req_put)
{
	struct snep_fragment *fragment;
	GSList *iter;
	near_bool_t ret;
	int err;

	/* Now, we process the request code */
	switch (frame->request) {
	case SNEP_REQ_PUT:
		DBG("SNEP_REQ_PUT");
		if (req_put != NULL)
			ret = (*req_put)(client_fd, snep_data);
		else {
			snep_core_response_noinfo(client_fd,
							SNEP_RESP_NOT_IMPL);
			ret = TRUE;
		}

		/* free and leave */
		g_hash_table_remove(snep_client_hash,
						GINT_TO_POINTER(client_fd));
		break;

	case SNEP_REQ_GET:
		DBG("SNEP_REQ_GET");
		if (req_get != NULL)
			ret =  (*req_get)(client_fd, snep_data);
		else {
			snep_core_response_noinfo(client_fd,
							SNEP_RESP_NOT_IMPL);
			ret = TRUE;
		}

		/* If there's some fragments, don't delete before the CONT */
		if (snep_data->req == NULL) {
			/* free and leave */
			DBG("Clean Table");
			g_hash_table_remove(snep_client_hash,
						GINT_TO_POINTER(client_fd));
		}
		break;

	case SNEP_REQ_CONTINUE:
		/*
		 * SNEP_REQ_CONTINUE indicates that we have to send the
		 * remaining fragments...
		 */
		DBG("SNEP_REQ_CONTINUE");
		if (snep_data->req->fragments == NULL) {
			near_error("error: SNEP_REQ_CONTINUE but no fragment");
			ret = FALSE;
			goto leave_cont;
		}

		/* Send fragments, one after the other (no ack expected) */
		while (g_slist_length(snep_data->req->fragments) != 0) {
			err = snep_core_send_fragment(snep_data->req);
			if (err < 0) {
				ret = FALSE;
				goto leave_cont;
			}
		}

		ret = TRUE;

leave_cont:
		/* No more fragment to send, clean memory */
		if (g_slist_length(snep_data->req->fragments) == 0) {
			for (iter = snep_data->req->fragments; iter;
							iter = iter->next) {
				fragment = iter->data;
				g_free(fragment->data);
				g_free(fragment);
			}
			g_slist_free(snep_data->req->fragments);
		}

		g_hash_table_remove(snep_client_hash,
						GINT_TO_POINTER(client_fd));

		break;

	default:
		near_error("Unsupported SNEP request code");
		ret = FALSE;
		break;
	}

	return ret;
}

/*
 * SNEP Core: read function
 *	This function handles SNEP REQUEST codes:
 *	GET, PUT and CONTINUE (REJECT is not handled).
 *
 *	We read the first 6 bytes (the header) and check
 *	- the read size ( should be 6 )
 *	- the version (on MAJOR)
 *
 *	Then, we check snep_data. If it exists, it means that we are in
 *	a fragment/continue situation (a 1st fragment was sent, and we
 *	expect a CONTINUE for the remaining bytes).
 *	If there's no existing snep_data, we create a new one and read the
 *	missing bytes (llcp removes fragmentation issues)
 *
 */
near_bool_t snep_core_read(int client_fd,
				uint32_t adapter_idx, uint32_t target_idx,
				near_tag_io_cb cb,
				near_server_io req_get,
				near_server_io req_put)
{
	struct p2p_snep_data *snep_data;
	struct p2p_snep_req_frame frame;
	int bytes_recv;
	uint32_t ndef_length;

	DBG("");

	/*
	 * We already got something from this client, we should try
	 * to continue reading.
	 */
	/* TODO Try with PEEK */
	bytes_recv = recv(client_fd, &frame, sizeof(frame), 0);
	if (bytes_recv < 0) {
		near_error("Read error SNEP %d %s", bytes_recv,
							strerror(errno));
		return FALSE;
	}

	/* Check frame size */
	if (bytes_recv != sizeof(frame)) {
		near_error("Bad frame size: %d", bytes_recv);
		return FALSE;
	}

	/* If major is different, send UNSUPPORTED VERSION */
	if (SNEP_MAJOR(frame.version) != SNEP_MAJOR(SNEP_VERSION)) {
		near_error("Unsupported version (%d)", frame.version);
		snep_core_response_noinfo(client_fd, SNEP_RESP_VERSION);
		return TRUE;
	}

	/* Check previous/pending snep_data */
	snep_data = g_hash_table_lookup(snep_client_hash,
					GINT_TO_POINTER(client_fd));

	/* Is snep data already there, should be a CONTINUE */
	if (snep_data != NULL)
		goto jumptocontinue;

	/* This is a new request from the client */
	snep_data = g_try_malloc0(sizeof(struct p2p_snep_data));
	if (snep_data == NULL)
		return FALSE;

	/* the whole frame length */
	ndef_length = GINT_FROM_BE(frame.length);

	snep_data->nfc_data = g_try_malloc0(ndef_length + TLV_SIZE);
	if (snep_data->nfc_data == NULL) {
		g_free(snep_data);
		return FALSE;
	}

	/* fill the struct */
	snep_data->nfc_data_length = ndef_length;
	snep_data->nfc_data_ptr = snep_data->nfc_data;
	snep_data->adapter_idx = adapter_idx;
	snep_data->target_idx = target_idx;
	snep_data->respond_continue = FALSE;
	snep_data->cb = cb;

	/* Add to the client hash table */
	g_hash_table_insert(snep_client_hash,
					GINT_TO_POINTER(client_fd), snep_data);

	if (ndef_length > 0) {
		if ((frame.request == SNEP_REQ_GET) ||
				(frame.request == SNEP_REQ_PUT)) {
			/* We should read the missing bytes */
			snep_core_read_ndef(client_fd, snep_data);
		}
	}

jumptocontinue:

	return snep_core_process_request(client_fd, snep_data, &frame,
							req_get, req_put);

}

/*
 * send a response frame with some datas. If the frame is too long, we
 * have to fragment the frame, using snep fragmentation protocol.
 * Return:
 * < 0 if error
 * 0 if no fragment;
 * > 0 if there's still some fragments
 *
 */
static int snep_core_response(int fd, struct p2p_snep_put_req_data *req,
		uint8_t resp_code, struct near_ndef_message *ndef)
{
	struct p2p_snep_req_frame header;
	struct snep_fragment *fragment;
	uint32_t max_fragment_len;
	gboolean fragmenting;
	int err;
	int snep_req_header_length, snep_additional_length;

	DBG("resp: 0x%02X", resp_code);

	max_fragment_len = SNEP_REQ_MAX_FRAGMENT_LENGTH;
	header.version = SNEP_VERSION;

	if (resp_code == SNEP_REQ_GET) {	/* Get for android */
		snep_req_header_length = SNEP_REQ_GET_HEADER_LENGTH;
		snep_additional_length = 4;	/* 4 Acceptable Length */
	} else {
		snep_req_header_length = SNEP_REQ_PUT_HEADER_LENGTH;
		snep_additional_length = 0;
	}

	header.length = GUINT32_TO_BE(ndef->length + snep_additional_length);
	header.request = resp_code;

	fragment = g_try_malloc0(sizeof(struct snep_fragment));

	if (fragment == NULL) {
		err = -ENOMEM;
		goto error;
	}

	if (max_fragment_len >= (ndef->length + snep_req_header_length)) {
		fragment->len = ndef->length + snep_req_header_length;
		fragmenting = FALSE;
	} else {
		fragment->len = max_fragment_len;
		fragmenting = TRUE;
	}

	fragment->data = g_try_malloc0(fragment->len);
	if (fragment->data == NULL) {
		g_free(fragment);
		err = ENOMEM;
		goto error;
	}

	/* Header to data - common header */
	memcpy(fragment->data, (uint8_t *)&header, SNEP_REQ_PUT_HEADER_LENGTH);

	/* if GET, we add the Acceptable length */
	if (header.request == SNEP_REQ_GET)
		near_put_be32(snep_req_header_length,
				fragment->data + SNEP_REQ_PUT_HEADER_LENGTH);

	if (fragmenting == TRUE) {
		memcpy(fragment->data + snep_req_header_length, ndef->data,
				max_fragment_len - snep_req_header_length);
		ndef->offset = max_fragment_len - snep_req_header_length;

		err = snep_core_push_prepare_fragments(req, ndef);
		if (err < 0) {
			g_free(fragment->data);
			g_free(fragment);
			goto error;
		}
	} else {
		memcpy(fragment->data + snep_req_header_length,
						ndef->data, ndef->length);
	}

	err = send(fd, fragment->data, fragment->len, 0);
	if (err < 0) {
		near_error("Sending failed %d", err);
		g_free(fragment->data);
		g_free(fragment);
		goto error;
	}

	g_free(fragment->data);
	g_free(fragment);

	return 0;

error:
	if (req != NULL)
		free_snep_core_push_data(req, err);

	return err;
}

void snep_core_response_with_info(int client_fd, uint8_t response,
				uint8_t *data, int length)
{
	struct p2p_snep_data *snep_data;
	struct p2p_snep_put_req_data *req;
	struct near_ndef_message *ndef;

	DBG("Response with info 0x%x (len:%d)", response, length);

	req = NULL;
	ndef = NULL;

	/* get the snep data */
	snep_data = g_hash_table_lookup(snep_client_hash,
						GINT_TO_POINTER(client_fd));
	if (snep_data == NULL) {
		DBG("snep_data not found");
		goto done;
	}

	/* Prepare the ndef struct */
	ndef = g_try_malloc0(sizeof(struct near_ndef_message));
	if (ndef == NULL)
		goto done;

	ndef->data = g_try_malloc0(length);
	if (ndef->data == NULL) {
		g_free(ndef);
		ndef = NULL;
		goto done;
	}

	/* Fill the ndef */
	ndef->length = length;
	ndef->offset = 0;
	memcpy(ndef->data, data, length);

	ndef->offset = 0;

	/* Now prepare req struct */
	req = g_try_malloc0(sizeof(struct p2p_snep_put_req_data));
	if (req == NULL)
		goto done;

	/* Prepare the callback */
	snep_data->req = req;

	req->fd = client_fd;
	req->adapter_idx = snep_data->adapter_idx;
	req->target_idx = snep_data->target_idx;
	req->cb = snep_data->cb;

	/* send it !*/
	snep_core_response(client_fd, req, response, ndef);

done:
	/* If no fragment, free mem */
	if (req != NULL) {
		if (req->fragments == 0) {
			g_free(req);
			snep_data->req = NULL;
		}
	}

	if (ndef != NULL)
		g_free(ndef->data);
	g_free(ndef);
}

/* SNEP Core: on P2P push */
int snep_core_push(int fd, uint32_t adapter_idx, uint32_t target_idx,
			struct near_ndef_message *ndef,
			near_device_io_cb cb)
{
	struct p2p_snep_put_req_data *req;
	GIOChannel *channel;
	uint8_t resp_code;
	int err;

	DBG("");

	req = g_try_malloc0(sizeof(struct p2p_snep_put_req_data));
	if (req == NULL) {
		err = -ENOMEM;
		goto error;
	}

	channel = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(channel, TRUE);

	req->fd = fd;
	req->adapter_idx = adapter_idx;
	req->target_idx = target_idx;
	req->cb = cb;
	ndef->offset = 0;
	req->watch = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_NVAL |
					G_IO_ERR, snep_core_push_event,
					(gpointer) req);

	/* Check if Hr or Hs for Handover over SNEP */
	if (*(char *)(ndef->data + 3) == 'H')
		resp_code = SNEP_REQ_GET;		/* Get for android */
	else
		resp_code = SNEP_REQ_PUT;

	return snep_core_response(fd, req, resp_code, ndef);

error:
	free_snep_core_push_data(req, err);

	return err;

}

/* SNEP core functions: close */
void snep_core_close(int client_fd, int err)
{
	struct p2p_snep_data *snep_data;

	DBG("");

	snep_data = g_hash_table_lookup(snep_client_hash,
					GINT_TO_POINTER(client_fd));
	if (snep_data == NULL)
		return;

	snep_data->cb(snep_data->adapter_idx, snep_data->target_idx, err);

	g_hash_table_remove(snep_client_hash, GINT_TO_POINTER(client_fd));
}

int snep_core_init(void)
{
	snep_client_hash = g_hash_table_new_full(g_direct_hash,
							g_direct_equal, NULL,
							free_snep_core_client);
	snep_init();
	snep_validation_init();

	return 0;
}

void snep_core_exit(void)
{
	snep_validation_exit();

	g_hash_table_destroy(snep_client_hash);
	snep_client_hash = NULL;
}