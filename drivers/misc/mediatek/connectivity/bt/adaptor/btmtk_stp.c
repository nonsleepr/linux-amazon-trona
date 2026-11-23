// SPDX-License-Identifier: GPL-2.0-only
/*
 * btmtk_stp.c -- Bluetooth HCI driver for the MT6630/MT6631 combo chip's
 * BT function, switched on through the WMT core and framed over its STP
 * transport.
 *
 * This chip and its WMT ("wireless management task") power-sequencing and
 * firmware-download core, and its BTIF transport, are already shared with
 * the connectivity subsystem's WiFi driver (CONFIG_MTK_COMBO,
 * CONFIG_MTK_BTIF). What WiFi has that Bluetooth didn't, until this driver,
 * is an adaptor that turns a powered function into something the rest of
 * the kernel can use: WiFi's is
 * drivers/misc/mediatek/connectivity/wlan/adaptor/wmt_cdev_wifi.c, which
 * calls mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI) and lets the cfg80211 "gen3"
 * driver take it from there. Bluetooth's equivalent hook,
 * mtk_wcn_stpbt_drv_init() in common_detect/drv_init/bluetooth_drv_init.c,
 * was always a weak stub behind a CONFIG_MTK_COMBO_BT that didn't exist
 * anywhere in this tree's Kconfig (see bluetooth_drv_init.c and
 * research/bluetooth.md for the detail) - nothing ever powered BT on, and
 * nothing ever turned the resulting STP channel into an hci_dev.
 *
 * This driver does both of those things itself, the way wmt_cdev_wifi.c
 * does for WiFi:
 *
 *  - hdev->open()/close() call mtk_wcn_wmt_func_on()/_off()(WMTDRV_TYPE_BT),
 *    the WMT core's existing, fully implemented BT power-sequencing and
 *    ROM-patch-download entry point (wmt_func_bt_ops in wmt_func.c) - the
 *    same one WiFi already proved on this board, just for the BT function
 *    bit instead of the WiFi one.
 *
 *  - hdev->send() and a dedicated RX thread frame HCI packets onto/off of
 *    the STP core's BT_TASK_INDX channel (stp_core.c, stp_exp.h). STP is a
 *    byte-stream multiplexer, not an HCI framer: mtk_wcn_stp_send_data()
 *    and mtk_wcn_stp_receive_data() move raw bytes tagged with a task type
 *    (BT_TASK_INDX among WIFI/FM/GPS/WMT/...) over the single shared BTIF
 *    transport, the same way a UART would carry them for a real external
 *    combo chip - so the bytes inside that channel still need standard H4
 *    framing (type byte, then a command/event/ACL/SCO header with its own
 *    length field) exactly as if this were hci_uart.c talking to a tty.
 *    This driver reuses the kernel's own generic H4 reassembler
 *    (net/bluetooth/hci_h4.c's h4_recv_skb(), exported for this purpose)
 *    instead of reinventing it.
 *
 * Deliberately not implemented (see the "Implementation status" section of
 * research/bluetooth.md for the reasoning): SCO/eSCO audio sample routing,
 * any vendor-specific HCI command sequence (none was found to be needed:
 * the combo chip's firmware patch is already downloaded by the WMT core
 * before the BT function is turned on, unlike standalone UART MTK chips
 * whose driver has to send the patch itself over HCI), and whole-chip
 * reset/suspend recovery (wmt_cdev_wifi.c's wifi_reset_start()/
 * wifi_off_start()/wifi_reset_end() and its PM notifier have no BT
 * equivalent here).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_h4.h>

#include "wmt_exp.h"
#include "stp_exp.h"

#define PFX "[BTMTK-STP] "

/* Scratch size for one mtk_wcn_stp_receive_data() call; unrelated to STP's
 * own per-type ring (MTKSTP_BUFFER_SIZE, 16KB) or to any single HCI frame's
 * length - the H4 reassembler below is happy with any chunking.
 */
#define BTMTK_STP_RX_CHUNK 2048

/* STP's sliding TX window can be full; mtk_wcn_stp_send_data() then returns
 * 0 and the caller is expected to retry once the tx-event callback fires.
 * Bound the wait so a dead chip can't hang the HCI TX work forever.
 */
#define BTMTK_STP_TX_RETRIES	20
#define BTMTK_STP_TX_WAIT_MS	50

struct btmtk_stp_dev {
	struct hci_dev *hdev;
	struct task_struct *rx_task;
	wait_queue_head_t rx_wq;
	bool rx_signalled;
	wait_queue_head_t tx_wq;
	bool tx_resumed;
	struct sk_buff *rx_skb;
	bool powered;
};

/* Singleton: there is exactly one combo chip, exactly one BT_TASK_INDX STP
 * channel, and the STP event callback this driver registers for it
 * (MTK_WCN_STP_EVENT_CB) carries no context pointer of its own - the same
 * single-instance assumption wmt_cdev_wifi.c already makes for WiFi.
 */
static struct btmtk_stp_dev *g_bt;
static DEFINE_MUTEX(btmtk_stp_pwr_lock);

static const struct h4_recv_pkt btmtk_stp_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};

/*
 * Called from STP core's packet-processing context (stp_core.c's
 * mtkstp_process_packet(), running on the BTIF RX thread) whenever a new
 * BT_TASK_INDX frame has been enqueued. That thread also demuxes WiFi's
 * control traffic, FM, GPS and WMT's own channel, so this callback must
 * stay cheap: it only wakes btmtk_stp_rx_thread(), which does the actual
 * mtk_wcn_stp_receive_data()/H4 reassembly work.
 */
static void btmtk_stp_rx_event_cb(void)
{
	struct btmtk_stp_dev *bt = g_bt;

	if (!bt)
		return;

	bt->rx_signalled = true;
	wake_up_interruptible(&bt->rx_wq);
}

/*
 * STP calls this (for every registered type) whenever an ACK frees up TX
 * window space; see mtk_wcn_sys_event_tx_resume() in stp_exp.c.
 */
static void btmtk_stp_tx_event_cb(void)
{
	struct btmtk_stp_dev *bt = g_bt;

	if (!bt)
		return;

	bt->tx_resumed = true;
	wake_up(&bt->tx_wq);
}

static int btmtk_stp_rx_thread(void *data)
{
	struct btmtk_stp_dev *bt = data;
	u8 *buf;
	INT32 n;

	buf = kmalloc(BTMTK_STP_RX_CHUNK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		wait_event_interruptible(bt->rx_wq,
					  kthread_should_stop() || bt->rx_signalled);
		bt->rx_signalled = false;

		if (kthread_should_stop())
			break;

		while ((n = mtk_wcn_stp_receive_data(buf, BTMTK_STP_RX_CHUNK,
						      BT_TASK_INDX)) > 0) {
			bt->rx_skb = h4_recv_skb(bt->hdev, NULL, NULL, bt->rx_skb,
						 buf, n, btmtk_stp_recv_pkts,
						 ARRAY_SIZE(btmtk_stp_recv_pkts));
			if (IS_ERR(bt->rx_skb)) {
				bt_dev_err(bt->hdev, PFX "H4 reassembly failed (%ld)",
					   PTR_ERR(bt->rx_skb));
				bt->rx_skb = NULL;
			}
		}
	}

	kfree(buf);
	return 0;
}

static int btmtk_stp_hci_open(struct hci_dev *hdev)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&btmtk_stp_pwr_lock);

	if (bt->powered)
		goto out;

	/*
	 * Register before powering the function on, so STP is already
	 * routing BT_TASK_INDX frames to us (and has flushed whatever was
	 * left over from a previous run - mtk_wcn_stp_register_event_cb()
	 * does that itself) by the time the chip's BT subsystem comes up.
	 */
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, btmtk_stp_rx_event_cb);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, btmtk_stp_tx_event_cb);

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, PFX "WMT turn on BT failed\n");
		mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
		mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
		ret = -EIO;
		goto out;
	}

	bt->rx_skb = NULL;
	bt->rx_signalled = false;
	bt->rx_task = kthread_run(btmtk_stp_rx_thread, bt, "btmtk_stp_rx");
	if (IS_ERR(bt->rx_task)) {
		ret = PTR_ERR(bt->rx_task);
		bt_dev_err(hdev, PFX "could not start rx thread (%d)\n", ret);
		bt->rx_task = NULL;
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
		mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
		goto out;
	}

	bt->powered = true;
out:
	mutex_unlock(&btmtk_stp_pwr_lock);
	return ret;
}

static int btmtk_stp_hci_close(struct hci_dev *hdev)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);

	mutex_lock(&btmtk_stp_pwr_lock);

	if (!bt->powered)
		goto out;

	if (bt->rx_task) {
		kthread_stop(bt->rx_task);
		bt->rx_task = NULL;
	}

	/* Stop STP routing BT_TASK_INDX frames to us before the chip's BT
	 * function is turned off, mirroring the open() ordering.
	 */
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE)
		bt_dev_err(hdev, PFX "WMT turn off BT failed\n");

	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->powered = false;
out:
	mutex_unlock(&btmtk_stp_pwr_lock);
	return 0;
}

static int btmtk_stp_hci_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);
	INT32 written = 0;
	int i;

	if (!bt->powered)
		return -ENETDOWN;

	/* Prepend the H4 packet type byte; STP/BTIF underneath is a raw,
	 * per-task byte stream (see the file header comment), not something
	 * that frames HCI packets itself. Same idiom as hci_h4.c's
	 * h4_enqueue() and virtio_bt.c's virtbt_send_frame().
	 */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	for (i = 0; i < BTMTK_STP_TX_RETRIES; i++) {
		bt->tx_resumed = false;
		written = mtk_wcn_stp_send_data(skb->data, skb->len, BT_TASK_INDX);
		if (written == skb->len)
			break;
		if (written != 0) {
			bt_dev_err(hdev, PFX "stp send failed (%d of %u)\n",
				   written, skb->len);
			return -EIO;
		}
		/* TX window full: wait for an ACK to open it, then retry */
		wait_event_timeout(bt->tx_wq, bt->tx_resumed,
				   msecs_to_jiffies(BTMTK_STP_TX_WAIT_MS));
	}
	if (written != skb->len) {
		bt_dev_err(hdev, PFX "stp send gave up after %d tries\n", i);
		/* Negative return: hci_send_frame() frees skb */
		return -EIO;
	}

	kfree_skb(skb);
	return 0;
}

static int __init btmtk_stp_init(void)
{
	struct btmtk_stp_dev *bt;
	struct hci_dev *hdev;
	int ret;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	init_waitqueue_head(&bt->rx_wq);
	init_waitqueue_head(&bt->tx_wq);

	hdev = hci_alloc_dev();
	if (!hdev) {
		kfree(bt);
		return -ENOMEM;
	}

	bt->hdev = hdev;
	hci_set_drvdata(hdev, bt);

	/* There is no tty in this path (see the file header comment and
	 * research/bluetooth.md section 3), but the BTIF transport
	 * underneath is register-compatible UART-like hardware, and HCI_UART
	 * is the closest honest classification among hci.h's bus types.
	 */
	hdev->bus = HCI_UART;
	hdev->open = btmtk_stp_hci_open;
	hdev->close = btmtk_stp_hci_close;
	hdev->send = btmtk_stp_hci_send;

	ret = hci_register_dev(hdev);
	if (ret < 0) {
		pr_err(PFX "hci_register_dev failed (%d)\n", ret);
		hci_free_dev(hdev);
		kfree(bt);
		return ret;
	}

	g_bt = bt;
	pr_info(PFX "registered %s\n", hdev->name);
	return 0;
}

static void __exit btmtk_stp_exit(void)
{
	struct btmtk_stp_dev *bt = g_bt;

	if (!bt)
		return;

	/* Powers the chip off and stops the rx thread first, if still up. */
	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
	g_bt = NULL;
	kfree(bt);
}

module_init(btmtk_stp_init);
module_exit(btmtk_stp_exit);

MODULE_DESCRIPTION("MediaTek STP Bluetooth HCI driver for the connectivity subsystem");
MODULE_LICENSE("GPL");
