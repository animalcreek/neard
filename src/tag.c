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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <glib.h>

#include <gdbus.h>

#include "near.h"

#define TAG_UID_MAX_LEN 8

struct near_tag {
	uint32_t adapter_idx;
	uint32_t target_idx;

	uint8_t uid[TAG_UID_MAX_LEN];

	size_t data_length;
	uint8_t *data;

	uint32_t n_records;
	GList *records;
};

static GList *driver_list = NULL;

void __near_tag_append_records(struct near_tag *tag, DBusMessageIter *iter)
{
	GList *list;

	for (list = tag->records; list; list = list->next) {
		struct near_ndef_record *record = list->data;
		char *path;

		path = __near_ndef_record_get_path(record);
		if (path == NULL)
			continue;

		dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH,
							&path);
	}
}

static int tag_initialize(struct near_tag *tag,
			uint32_t adapter_idx, uint32_t target_idx,
				size_t data_length)
{
	DBG("data length %zu", data_length);

	tag->adapter_idx = adapter_idx;
	tag->target_idx = target_idx;
	tag->n_records = 0;

	if (data_length > 0) {
		tag->data_length = data_length;
		tag->data = g_try_malloc0(data_length);
		if (tag->data == NULL)
			return -ENOMEM;
	}

	return 0;
}

struct near_tag *__near_tag_new(uint32_t adapter_idx, uint32_t target_idx, size_t data_length)
{
	struct near_tag *tag;

	tag = g_try_malloc0(sizeof(struct near_tag));
	if (tag == NULL)
		return NULL;

	if (tag_initialize(tag, adapter_idx, target_idx, data_length) < 0) {
		g_free(tag);
		return NULL;
	}

	return tag;
}

void __near_tag_free(struct near_tag *tag)
{
	GList *list;

	for (list = tag->records; list; list = list->next) {
		struct near_ndef_record *record = list->data;

		__near_ndef_record_free(record);
	}

	g_list_free(tag->records);
	g_free(tag->data);
	g_free(tag);
}

uint32_t __near_tag_n_records(struct near_tag *tag)
{
	return tag->n_records;
}

int __near_tag_add_record(struct near_tag *tag,
				struct near_ndef_record *record)
{
	DBG("");

	tag->n_records++;
	tag->records = g_list_append(tag->records, record);

	return 0;
}

int near_tag_set_uid(struct near_tag *tag, uint8_t *uid, size_t uid_length)
{
	if (uid_length > TAG_UID_MAX_LEN)
		return -EINVAL;

	memset(tag->uid, 0, TAG_UID_MAX_LEN);
	memcpy(tag->uid, uid, uid_length);

	return 0;
}

uint8_t *near_tag_get_data(struct near_tag *tag, size_t *data_length)
{
	if (data_length == NULL)
		return NULL;

	*data_length = tag->data_length;

	return tag->data;
}

uint32_t near_tag_get_adapter_idx(struct near_tag *tag)
{
	return tag->adapter_idx;
}

uint32_t near_tag_get_target_idx(struct near_tag *tag)
{
	return tag->target_idx;
}

int near_tag_driver_register(struct near_tag_driver *driver)
{
	DBG("");

	if (driver->read_tag == NULL)
		return -EINVAL;

	driver_list = g_list_append(driver_list, driver);

	return 0;
}

void near_tag_driver_unregister(struct near_tag_driver *driver)
{
	DBG("");

	driver_list = g_list_remove(driver_list, driver);
}

int __near_tag_read(struct near_target *target,near_tag_read_cb cb)
{
	GList *list;
	uint16_t type;

	DBG("");

	type = __near_target_get_tag_type(target);
	if (type == NEAR_TAG_NFC_UNKNOWN)
		return -ENODEV;

	DBG("type 0x%x", type);

	for (list = driver_list; list; list = list->next) {
		struct near_tag_driver *driver = list->data;

		DBG("driver type 0x%x", driver->type);

		if (driver->type & type) {
			uint32_t adapter_idx, target_idx;		

			target_idx = __near_target_get_idx(target);
			adapter_idx = __near_target_get_adapter_idx(target);

			return driver->read_tag(adapter_idx, target_idx, cb);
		}
	}

	return 0;
}