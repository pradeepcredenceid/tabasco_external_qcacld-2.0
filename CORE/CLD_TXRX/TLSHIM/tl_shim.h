/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#ifndef TXRX_TL_SHIM_H
#define TXRX_TL_SHIM_H

struct tlshim_buf {
	struct list_head list;
	adf_nbuf_t buf;
};

#define TLSHIM_FLUSH_CACHE_IN_PROGRESS 0
struct tlshim_sta_info {
	bool registered;
	bool suspend_flush;
	WLANTL_STARxCBType data_rx;
	struct list_head cached_bufq;
	unsigned long flags;
};

struct txrx_tl_shim_ctx {
	void *cfg_ctx;
	ol_txrx_tx_fp tx;
	WLANTL_MgmtFrmRxCBType mgmt_rx;
	struct tlshim_sta_info sta_info[WLAN_MAX_STA_COUNT];
	adf_os_spinlock_t bufq_lock;
	struct work_struct cache_flush_work;
};

/*
 * APIs used by CLD specific components, as of now these are used only
 * in WMA.
 */
void WLANTL_RegisterVdev(void *vos_ctx, void *vdev);
VOS_STATUS tl_shim_get_vdevid(struct ol_txrx_peer_t *peer, u_int8_t *vdev_id);

#endif
