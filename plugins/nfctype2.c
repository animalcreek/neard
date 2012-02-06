/*
 *
 *  neard - Near Field Communication manager
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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
#include <linux/nfc.h>

#include <near/plugin.h>
#include <near/log.h>
#include <near/types.h>
#include <near/adapter.h>
#include <near/target.h>
#include <near/tag.h>
#include <near/ndef.h>
#include <near/tlv.h>

#define CMD_READ         0x30
#define CMD_WRITE        0xA2

#define READ_SIZE  16
#define BLOCK_SIZE 4

#define META_BLOCK_START 0
#define DATA_BLOCK_START 4
#define TYPE2_MAGIC 0xe1

#define TAG_DATA_CC(data) ((data) + 12)
#define TAG_DATA_LENGTH(cc) ((cc)[2] * 8)
#define TAG_DATA_NFC(cc) ((cc)[0] & TYPE2_MAGIC)

#define TYPE2_NOWRITE_ACCESS	0x0F
#define TAG_T2_WRITE_FLAG(cc) ((cc)[3] & TYPE2_NOWRITE_ACCESS)

#define NDEF_MAX_SIZE	0x30

struct type2_cmd {
	uint8_t cmd;
	uint8_t block;
	uint8_t data[BLOCK_SIZE];
} __attribute__((packed));

struct type2_tag {
	uint32_t adapter_idx;
	uint16_t current_block;

	near_tag_io_cb cb;
	struct near_tag *tag;
};

struct recv_cookie {
	uint32_t adapter_idx;
	uint32_t target_idx;
	uint8_t current_block;
	struct near_ndef_message *ndef;
	near_tag_io_cb cb;
};

static void release_recv_cookie(struct recv_cookie *cookie)
{
	if (cookie == NULL)
		return;

	if (cookie->ndef)
		g_free(cookie->ndef->data);

	g_free(cookie->ndef);
	g_free(cookie);
	cookie = NULL;
}

static int data_recv(uint8_t *resp, int length, void *data)
{
	struct type2_tag *tag = data;
	struct type2_cmd cmd;
	uint8_t *nfc_data;
	uint16_t current_length, length_read, data_length;
	uint32_t adapter_idx;
	int read_blocks;

	DBG("%d", length);

	if (length < 0) {
		g_free(tag);

		return  length;
	}

	nfc_data = near_tag_get_data(tag->tag, (size_t *)&data_length);
	adapter_idx = near_tag_get_adapter_idx(tag->tag);

	length_read = length - NFC_HEADER_SIZE;
	current_length = tag->current_block * BLOCK_SIZE;
	if (current_length + length - NFC_HEADER_SIZE > data_length)
		length_read = data_length - current_length;

	memcpy(nfc_data + current_length, resp + NFC_HEADER_SIZE, length_read);

	if (current_length + length_read == data_length) {
		/* TODO parse tag->data for NDEFS, and notify target.c */
		tag->current_block = 0;

		DBG("Done reading");

		near_tlv_parse(tag->tag, tag->cb);

		g_free(tag);

		return 0;
	}

	read_blocks = length / BLOCK_SIZE;
	tag->current_block += read_blocks;

	cmd.cmd = CMD_READ;
	cmd.block = DATA_BLOCK_START + tag->current_block;

	DBG("adapter %d", adapter_idx);

	return near_adapter_send(adapter_idx,
				(uint8_t *)&cmd, sizeof(cmd),
					data_recv, tag);
}

static int data_read(struct type2_tag *tag)
{
	struct type2_cmd cmd;
	uint32_t adapter_idx;

	DBG("");

	tag->current_block = 0;

	cmd.cmd = CMD_READ;
	cmd.block = DATA_BLOCK_START;

	adapter_idx = near_tag_get_adapter_idx(tag->tag);

	return near_adapter_send(adapter_idx,
				(uint8_t *)&cmd, sizeof(cmd),
					data_recv, tag);
}

static int meta_recv(uint8_t *resp, int length, void *data)
{
        struct recv_cookie *cookie = data;
	struct near_tag *tag;
	struct type2_tag *t2_tag;
	uint8_t *cc;
	int err;

	DBG("%d", length);

	if (length < 0) {
		err = length;
		goto out;
	}

	if (resp[0] != 0) {
		err = -EIO;
		goto out;
	}

	cc = TAG_DATA_CC(resp + NFC_HEADER_SIZE);

	if (TAG_DATA_NFC(cc) == 0) {
		err = -EINVAL;
		goto out;
	}

	tag = near_target_add_tag(cookie->adapter_idx, cookie->target_idx,
					TAG_DATA_LENGTH(cc));
	if (tag == NULL) {
		err = -ENOMEM;
		goto out;
	}

	t2_tag = g_try_malloc0(sizeof(struct type2_tag));
	if (t2_tag == NULL) {
		err = -ENOMEM;
		goto out;
	}

	t2_tag->adapter_idx = cookie->adapter_idx;
	t2_tag->cb = cookie->cb;
	t2_tag->tag = tag;

	near_tag_set_uid(tag, resp + NFC_HEADER_SIZE, 8);

	/* Set the ReadWrite flag */
	if (TAG_T2_WRITE_FLAG(cc) == TYPE2_NOWRITE_ACCESS)
		near_tag_set_ro(tag, TRUE);
	else
		near_tag_set_ro(tag, FALSE);

	near_tag_set_memory_layout(tag, NEAR_TAG_MEMORY_STATIC);

	err = data_read(t2_tag);
	if (err < 0)
		goto out;

	release_recv_cookie(cookie);

	return 0;

out:
	if (err < 0 && cookie->cb)
		cookie->cb(cookie->adapter_idx, cookie->target_idx, err);

	release_recv_cookie(cookie);

	return err;
}

static int nfctype2_read_meta(uint32_t adapter_idx, uint32_t target_idx,
							near_tag_io_cb cb)
{
	struct type2_cmd cmd;
	struct recv_cookie *cookie;
	
	DBG("");

	cmd.cmd = CMD_READ;
	cmd.block = META_BLOCK_START;

	cookie = g_try_malloc0(sizeof(struct recv_cookie));
	cookie->adapter_idx = adapter_idx;
	cookie->target_idx = target_idx;
	cookie->cb = cb;

	return near_adapter_send(adapter_idx, (uint8_t *)&cmd, sizeof(cmd),
							meta_recv, cookie);
}

static int nfctype2_read_tag(uint32_t adapter_idx,
				uint32_t target_idx, near_tag_io_cb cb)
{
	int err;
	enum near_target_sub_type tgt_subtype;

	DBG("");

	tgt_subtype = near_target_get_subtype(adapter_idx, target_idx);

	err = near_adapter_connect(adapter_idx, target_idx, NFC_PROTO_MIFARE);
	if (err < 0) {
		near_error("Could not connect %d", err);

		return err;
	}

	if (tgt_subtype == NEAR_TAG_NFC_T2_MIFARE_ULTRALIGHT)
		err = nfctype2_read_meta(adapter_idx, target_idx, cb);
	else {
		DBG("Unknown TAG Type 2 subtype (%d)", tgt_subtype);
		err = -1;
	}

	if (err < 0)
		near_adapter_disconnect(adapter_idx);

	return err;
}

static int data_write_resp(uint8_t *resp, int length, void *data)
{
	int err;
	struct recv_cookie *cookie = data;
	struct type2_cmd cmd;

	DBG("");

	if (length < 0 || resp[0] != 0) {
		err = -EIO;
		goto out;
	}

	if (cookie->ndef->offset > cookie->ndef->length) {
		DBG("Done writing");

		if (cookie->cb)
			cookie->cb(cookie->adapter_idx, cookie->target_idx, 0);

		release_recv_cookie(cookie);

		return 0;
	}

	cmd.cmd = CMD_WRITE;
	cmd.block = cookie->current_block;
	cookie->current_block++;

	if ((cookie->ndef->offset + BLOCK_SIZE) <
			cookie->ndef->length) {
		memcpy(cmd.data, cookie->ndef->data +
					cookie->ndef->offset, BLOCK_SIZE);
		cookie->ndef->offset += BLOCK_SIZE;
	} else {
		memcpy(cmd.data, cookie->ndef->data + cookie->ndef->offset,
				cookie->ndef->length - cookie->ndef->offset);
		cookie->ndef->offset = cookie->ndef->length + 1;
	}

	err = near_adapter_send(cookie->adapter_idx, (uint8_t *)&cmd,
					sizeof(cmd), data_write_resp, cookie);


	if (err < 0)
		goto out;

	return 0;

out:
	if (err < 0 && cookie->cb)
		cookie->cb(cookie->adapter_idx, cookie->target_idx, err);

	release_recv_cookie(cookie);

	return err;
}

static int data_write(uint32_t adapter_idx, uint32_t target_idx,
				struct near_ndef_message *ndef,
				near_tag_io_cb cb)
{
	struct type2_cmd cmd;
	struct recv_cookie *cookie;
	int err;

	DBG("");

	cookie = g_try_malloc0(sizeof(struct recv_cookie));
	cookie->adapter_idx = adapter_idx;
	cookie->target_idx = target_idx;
	cookie->current_block = DATA_BLOCK_START;
	cookie->ndef = ndef;
	cookie->cb = cb;

	cmd.cmd = CMD_WRITE;
	cmd.block = cookie->current_block;
	memcpy(cmd.data, cookie->ndef->data, BLOCK_SIZE);
	cookie->ndef->offset += BLOCK_SIZE;
	cookie->current_block++;

	err = near_adapter_send(cookie->adapter_idx, (uint8_t *)&cmd,
					sizeof(cmd), data_write_resp, cookie);

	if (err < 0)
		goto out;

	return 0;

out:
	release_recv_cookie(cookie);

	return err;
}

static int nfctype2_write_tag(uint32_t adapter_idx, uint32_t target_idx,
				struct near_ndef_message *ndef,
				near_tag_io_cb cb)
{
	int err;
	struct near_tag *tag;
	enum near_target_sub_type tgt_subtype;

	DBG("");

	if (ndef == NULL || cb == NULL)
		return -EINVAL;

	tag = near_target_get_tag(adapter_idx, target_idx);
	if (tag == NULL)
		return -EINVAL;

	if (near_tag_get_ro(tag) == TRUE) {
		DBG("tag is read-only");
		return -EPERM;
	}

	tgt_subtype = near_target_get_subtype(adapter_idx, target_idx);

	if (tgt_subtype != NEAR_TAG_NFC_T2_MIFARE_ULTRALIGHT) {
		DBG("Unknown Tag Type 2 subtype (%d)", tgt_subtype);
		return -1;
	}

	/* This check is valid for only static tags.
	 * Max data length on Type 2 Tag including TLV's is NDEF_MAX_SIZE */
	if (near_tag_get_memory_layout(tag) == NEAR_TAG_MEMORY_STATIC) {
		if ((ndef->length + 3) > NDEF_MAX_SIZE) {
			near_error("not enough space on tag");
			return -ENOSPC;
		}
	}

	err = data_write(adapter_idx, target_idx, ndef, cb);
	if (err < 0)
		goto out;

	return 0;

out:
	near_adapter_disconnect(adapter_idx);

	return err;
}

static struct near_tag_driver type2_driver = {
	.type     = NEAR_TAG_NFC_TYPE2,
	.priority = NEAR_TAG_PRIORITY_DEFAULT,
	.read_tag = nfctype2_read_tag,
	.add_ndef = nfctype2_write_tag,
};

static int nfctype2_init(void)
{
	DBG("");

	return near_tag_driver_register(&type2_driver);
}

static void nfctype2_exit(void)
{
	DBG("");

	near_tag_driver_unregister(&type2_driver);
}

NEAR_PLUGIN_DEFINE(nfctype2, "NFC Forum Type 2 tags support", VERSION,
			NEAR_PLUGIN_PRIORITY_HIGH, nfctype2_init, nfctype2_exit)

