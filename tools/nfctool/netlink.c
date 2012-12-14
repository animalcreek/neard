/*
 *
 *  Near Field Communication nfctool
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

#include <errno.h>
#include <sys/socket.h>
#include <linux/nfc.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <glib.h>

#include "nfctool.h"
#include "netlink.h"

struct handler_args {
	const char *group;
	int id;
};

struct nlnfc_state {
	struct nl_sock *cmd_sock;
	struct nl_sock *event_sock;
	int nfc_id;
	int mcid;
};

static struct nlnfc_state *nfc_state = NULL;

static void adapter_add_target(struct nfc_adapter *adapter,
				guint8 type, guint32 idx)
{
	DBG("adapter_idx: %d, target_type: %d, target_idx: %d", adapter->idx,
								type, idx);

	if (type == TARGET_TYPE_TAG)
		adapter->tags = g_slist_append(adapter->tags,
					       GINT_TO_POINTER(idx));
	else
		adapter->devices = g_slist_append(adapter->devices,
						  GINT_TO_POINTER(idx));
}

static int nl_error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = err->error;

	return NL_STOP;
}

static int nl_finish_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = 1;

	return NL_SKIP;
}

static int nl_ack_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;

	DBG("");

	*ret = 1;

	return NL_STOP;
}

static int nl_send_msg(struct nl_sock *sock, struct nl_msg *msg,
		       int (*rx_handler)(struct nl_msg *, void *),
		       void *data)
{
	struct nl_cb *cb;
	int err, done;

	DBG("");

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (cb == NULL)
		return -ENOMEM;

	err = nl_send_auto_complete(sock, msg);
	if (err < 0) {
		nl_cb_put(cb);
		print_error("%s", strerror(err));

		return err;
	}

	err = done = 0;

	nl_cb_err(cb, NL_CB_CUSTOM, nl_error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nl_finish_handler, &done);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, nl_ack_handler, &done);

	if (rx_handler != NULL)
		nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, rx_handler, data);

	while (err == 0 && done == 0)
		nl_recvmsgs(sock, cb);

	nl_cb_put(cb);

	return err;
}

static int nl_family_handler(struct nl_msg *msg, void *arg)
{
	struct handler_args *grp = arg;
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *mcgrp;
	int rem_mcgrp;

	DBG("");

	nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb[CTRL_ATTR_MCAST_GROUPS])
		return NL_SKIP;

	nla_for_each_nested(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], rem_mcgrp) {
		struct nlattr *tb_mcgrp[CTRL_ATTR_MCAST_GRP_MAX + 1];

		nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX,
			  nla_data(mcgrp), nla_len(mcgrp), NULL);

		if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] ||
		    !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID])
			continue;
		if (strncmp(nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]),
			    grp->group,
			    nla_len(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME])))
			continue;
		grp->id = nla_get_u32(tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]);
		break;
	}

	return NL_SKIP;
}

static int nl_get_multicast_id(struct nl_sock *sock, const char *family,
				const char *group)
{
	struct nl_msg *msg;
	int err = 0, ctrlid;
	struct handler_args grp = {
		.group = group,
		.id = -ENOENT,
	};

	DBG("");

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	ctrlid = genl_ctrl_resolve(sock, "nlctrl");

	genlmsg_put(msg, 0, 0, ctrlid, 0,
		    0, CTRL_CMD_GETFAMILY, 0);

	NLA_PUT_STRING(msg, CTRL_ATTR_FAMILY_NAME, family);

	err = nl_send_msg(sock, msg, nl_family_handler, &grp);
	if (err)
		goto nla_put_failure;

	DBG("multicast id %d", grp.id);

	err = grp.id;

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

static int nl_get_targets_handler(struct nl_msg *n, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(n);
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	guint32 target_idx, target_type, protocols;
	struct nfc_adapter *adapter;

	DBG("");

	adapter = (struct nfc_adapter *)arg;

	genlmsg_parse(nlh, 0, attrs, NFC_ATTR_MAX, NULL);

	target_idx = nla_get_u32(attrs[NFC_ATTR_TARGET_INDEX]);
	protocols = nla_get_u32(attrs[NFC_ATTR_PROTOCOLS]);

	if (protocols & NFC_PROTO_NFC_DEP_MASK)
		target_type = TARGET_TYPE_DEVICE;
	else
		target_type = TARGET_TYPE_TAG;

	adapter_add_target(adapter, target_type, target_idx);

	return 0;
}

int nl_get_targets(struct nfc_adapter *adapter)
{
	struct nl_msg *msg;
	void *hdr;
	int err;

	DBG("");

	if (nfc_state == NULL || nfc_state->nfc_id < 0)
		return -ENODEV;

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, nfc_state->nfc_id, 0,
			  NLM_F_DUMP, NFC_CMD_GET_TARGET, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto nla_put_failure;
	}

	err = -EMSGSIZE;

	g_slist_free(adapter->tags);
	adapter->tags = NULL;

	g_slist_free(adapter->devices);
	adapter->devices = NULL;

	NLA_PUT_U32(msg, NFC_ATTR_DEVICE_INDEX, adapter->idx);

	err = nl_send_msg(nfc_state->cmd_sock, msg,
			  nl_get_targets_handler, adapter);

	DBG("nl_send_msg returns %d", err);

nla_put_failure:
	nlmsg_free(msg);

	return err;
}

static int nl_get_devices_handler(struct nl_msg *n, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr(n);
	struct nlattr *attrs[NFC_ATTR_MAX + 1];
	guint32 idx, protocols = 0;
	guint8 powered = 0;
	guint8 rf_mode = NFC_RF_NONE;
	struct nfc_adapter *adapter;

	DBG("");

	genlmsg_parse(nlh, 0, attrs, NFC_ATTR_MAX, NULL);

	if (attrs[NFC_ATTR_DEVICE_INDEX] == NULL) {
		nl_perror(NLE_MISSING_ATTR, "NFC_CMD_GET_DEVICE");
		return NL_STOP;
	}

	idx = nla_get_u32(attrs[NFC_ATTR_DEVICE_INDEX]);

	if (attrs[NFC_ATTR_PROTOCOLS] != NULL)
		protocols = nla_get_u32(attrs[NFC_ATTR_PROTOCOLS]);

	if (attrs[NFC_ATTR_RF_MODE] != NULL)
		rf_mode = nla_get_u8(attrs[NFC_ATTR_RF_MODE]);

	if (attrs[NFC_ATTR_DEVICE_POWERED] != NULL)
		powered = nla_get_u8(attrs[NFC_ATTR_DEVICE_POWERED]);

	adapter = g_malloc0(sizeof(struct nfc_adapter));

	adapter->idx = idx;
	adapter->protocols = protocols;
	adapter->powered = powered;

	if (rf_mode == NFC_RF_TARGET)
		adapter_add_target(adapter, TARGET_TYPE_DEVICE, 0);

	adapters = g_slist_append(adapters, adapter);

	return NL_SKIP;
}

int nl_get_devices(void)
{
	struct nl_msg *msg;
	void *hdr;
	int err;

	DBG("");

	if (nfc_state == NULL || nfc_state->nfc_id < 0)
		return -ENODEV;

	msg = nlmsg_alloc();
	if (msg == NULL)
		return -ENOMEM;

	hdr = genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, nfc_state->nfc_id, 0,
			  NLM_F_DUMP, NFC_CMD_GET_DEVICE, NFC_GENL_VERSION);
	if (hdr == NULL) {
		err = -EINVAL;
		goto out;
	}

	err = nl_send_msg(nfc_state->cmd_sock, msg, nl_get_devices_handler,
			  NULL);

	DBG("nl_send_msg returns %d", err);

out:
	nlmsg_free(msg);

	return err;
}

void nl_cleanup(void)
{
	if (nfc_state) {
		if (nfc_state->cmd_sock)
			nl_socket_free(nfc_state->cmd_sock);

		if (nfc_state->event_sock)
			nl_socket_free(nfc_state->event_sock);

		g_free(nfc_state);
		nfc_state = NULL;
	}
}

int nl_init(void)
{
	int err;

	DBG("");

	nfc_state = g_malloc0(sizeof(struct nlnfc_state));

	nfc_state->cmd_sock = nl_socket_alloc();
	if (nfc_state->cmd_sock == NULL) {
		print_error("Failed to allocate NFC netlink socket");
		err = -ENOMEM;
		goto exit_err;
	}

	nfc_state->event_sock = nl_socket_alloc();
	if (nfc_state->event_sock == NULL) {
		print_error("Failed to allocate NFC netlink socket");
		err = -ENOMEM;
		goto exit_err;
	}

	if (genl_connect(nfc_state->cmd_sock)) {
		print_error("Failed to connect to generic netlink");
		err = -ENOLINK;
		goto exit_err;
	}

	if (genl_connect(nfc_state->event_sock)) {
		print_error("Failed to connect to generic netlink");
		err = -ENOLINK;
		goto exit_err;
	}

	nfc_state->nfc_id = genl_ctrl_resolve(nfc_state->cmd_sock, "nfc");
	if (nfc_state->nfc_id < 0) {
		print_error("Unable to find NFC netlink family");
		err = -ENOENT;
		goto exit_err;
	}

	nfc_state->mcid = nl_get_multicast_id(nfc_state->cmd_sock,
					      NFC_GENL_NAME,
					      NFC_GENL_MCAST_EVENT_NAME);
	if (nfc_state->mcid <= 0) {
		print_error("Wrong mcast id %d", nfc_state->mcid);
		err = nfc_state->mcid;
		goto exit_err;
	}

	err = nl_socket_add_membership(nfc_state->event_sock, nfc_state->mcid);
	if (err) {
		print_error("Error adding nl socket to membership");
		goto exit_err;
	}

	return 0;

exit_err:
	nl_cleanup();

	print_error("netlink init failed");

	return err;
}