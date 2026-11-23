// SPDX-License-Identifier: GPL-2.0-only
/*
 * btmtk_stp.c -- Bluetooth HCI driver for the MT6630/MT6631 ("consys") combo
 * chip's BT function, switched on through the WMT core and framed over its
 * STP transport.
 *
 * The chip, its WMT power-sequencing and firmware-download core and the BTIF
 * transport are shared with the connectivity subsystem's WiFi driver
 * (CONFIG_MTK_COMBO, CONFIG_MTK_BTIF). What WiFi has and Bluetooth lacked is
 * an adaptor that turns a powered function into something the rest of the
 * kernel can use: WiFi's is wlan/adaptor/wmt_cdev_wifi.c. The Bluetooth
 * counterpart in the vendor tree, /dev/stpbt (stp_chrdev_bt.c), was never
 * part of this tree; this driver does its job, but registers an hci_dev with
 * the kernel's Bluetooth core instead of handing the byte stream to a
 * userspace HCI stack.
 *
 * What the vendor's stp_chrdev_bt.c does, and this driver does the same:
 *
 *  - open: mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) (wmt_func_bt_ops: the chip is
 *    powered and its ROM patch downloaded by the first function that is
 *    switched on, then the BT function is enabled with a WMT command), check
 *    mtk_wcn_stp_is_ready(), mtk_wcn_stp_set_bluez(0) to keep BT frames in
 *    the STP RX queue, and register an RX event callback for BT_TASK_INDX.
 *  - close: unregister the callback, mtk_wcn_wmt_func_off().
 *  - write: the bytes go to mtk_wcn_stp_send_data(..., BT_TASK_INDX) as they
 *    are, retried when STP has no window space (it returns 0).
 *  - read: mtk_wcn_stp_receive_data(..., BT_TASK_INDX), woken by the callback.
 *  - whole chip reset (a WMT message): the stack is told by a Hardware Error
 *    event and reopens the device.
 *
 * Framing: STP is a multiplexer with its own header and CRC per frame (the
 * task type is in the header; BT is task 0); what is inside a BT frame is
 * plain H:4 HCI, with the packet indicator byte, exactly as stp_chrdev_bt.c's
 * users (Android's libbt-vendor, Gemian's hci_stp) exchange it. TX therefore
 * prepends the indicator, and RX is reassembled with the generic H:4 helper.
 *
 * Chip initialization: the vendor's libbt-vendor runs a script of HCI
 * commands before it exposes the controller (BT_InitDevice, the "consys"
 * script): Set BD_ADDR (0xFC1A), Set Radio (0xFC79), Set TX Power Offset
 * (0xFC93), Set Sleep Timeout (0xFC7A), HCI Reset. hci_dev's setup() does the
 * same here; the core sends the HCI Reset itself after it. The parameters are
 * the vendor's NVRAM record when there is one; there is no NVRAM here, so
 * they are its compiled-in defaults for consys chips (CFG_BT_Default.h,
 * stBtDefault_consys), which is what the vendor stack runs on when its
 * NVRAM is empty. The BD address is Amazon's, from the IDME /idme/bt_mac_addr
 * node the bootloader puts in the device tree.
 *
 * Not implemented: SCO/eSCO (voice goes through the CVSD SRAM path on
 * MediaTek combo chips, not HCI SCO, and the vendor sends no SCO setup
 * command), the vendor's debug-only commands (FW syslog, SSP debug mode),
 * PSM control and FW assert ioctls (they are chardev ioctls).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/hex.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/suspend.h>
#include <linux/notifier.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_h4.h>

#include "wmt_exp.h"
#include "stp_exp.h"

#define IDME_OF_BT_MAC_ADDR	"/idme/bt_mac_addr"

/* One STP frame carries at most 12 bits of length */
#define BTMTK_STP_MAX_FRAME	0xfff

/* Scratch size of one mtk_wcn_stp_receive_data() call; the H:4 reassembler
 * is happy with any chunking.
 */
#define BTMTK_STP_RX_CHUNK	2048

/* STP answers 0 when its TX window is full; the caller is to retry when an
 * ACK has freed some (the TX event callback). stp_chrdev_bt.c's
 * send_hci_frame() just waits 30 ms and tries up to three times; here the
 * wait ends with the callback, and is bounded so that a dead chip can't hold
 * the HCI TX work for ever.
 */
#define BTMTK_STP_TX_TRIES	20
#define BTMTK_STP_TX_WAIT_MS	50

/* Vendor opcodes of libbt-vendor's init script (radiomod.c) */
#define BTMTK_OP_SET_BDADDR	0xfc1a
#define BTMTK_OP_SET_RADIO	0xfc79
#define BTMTK_OP_SET_TX_PWR_OFS	0xfc93
#define BTMTK_OP_SET_SLEEP	0xfc7a

/* stBtDefault_consys, CFG_BT_Default.h of libbt-vendor: Radio, Sleep and
 * TxPWOffset; the other fields of the NVRAM record are "not used" for the
 * commands of the consys script.
 */
static const u8 btmtk_radio_default[] = { 0x07, 0x80, 0x00, 0x06, 0x07, 0x07 };
static const u8 btmtk_sleep_default[] = { 0x03, 0x40, 0x1f, 0x40, 0x1f, 0x00, 0x04 };
static const u8 btmtk_txpwr_ofs_default[] = { 0xff, 0xff, 0xff };

struct btmtk_stp_dev {
	struct hci_dev *hdev;

	/* Protects powered, suspended and reopen; held across the (slow)
	 * power on and off.
	 */
	struct mutex pwr_lock;
	bool powered;
	bool suspended;
	bool reopen_after_resume;

	/* Whole chip reset between its RESET_START and RESET_END messages */
	bool resetting;

	struct mutex tx_lock;
	wait_queue_head_t tx_wq;
	bool tx_resumed;

	struct work_struct rx_work;
	struct sk_buff *rx_skb;
	u8 *rx_buf;

	struct notifier_block pm_nb;
};

/* There is one combo chip, one BT_TASK_INDX STP channel, and neither the
 * STP event callback nor the WMT message callback carries a context pointer
 * (wmt_cdev_wifi.c makes the same single-instance assumption for WiFi).
 */
static struct btmtk_stp_dev *g_bt;

static const struct h4_recv_pkt btmtk_stp_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};

/* STP's RX thread calls this for a new BT frame; it also demuxes WiFi
 * control traffic, FM, GPS and WMT's own channel, so it only queues the work.
 */
static void btmtk_stp_rx_event_cb(void)
{
	struct btmtk_stp_dev *bt = READ_ONCE(g_bt);

	if (bt)
		queue_work(system_highpri_wq, &bt->rx_work);
}

/* STP calls this, for every registered type, when an ACK has freed TX window */
static void btmtk_stp_tx_event_cb(void)
{
	struct btmtk_stp_dev *bt = READ_ONCE(g_bt);

	if (bt) {
		WRITE_ONCE(bt->tx_resumed, true);
		wake_up(&bt->tx_wq);
	}
}

static void btmtk_stp_rx_work(struct work_struct *work)
{
	struct btmtk_stp_dev *bt = container_of(work, struct btmtk_stp_dev, rx_work);
	INT32 n;

	while ((n = mtk_wcn_stp_receive_data(bt->rx_buf, BTMTK_STP_RX_CHUNK,
					     BT_TASK_INDX)) > 0) {
		bt->rx_skb = h4_recv_skb(bt->hdev, NULL, NULL, bt->rx_skb,
					 bt->rx_buf, n, btmtk_stp_recv_pkts,
					 ARRAY_SIZE(btmtk_stp_recv_pkts));
		if (IS_ERR(bt->rx_skb)) {
			bt_dev_err(bt->hdev, "H:4 reassembly failed (%ld)",
				   PTR_ERR(bt->rx_skb));
			bt->rx_skb = NULL;
		}
	}
}

/* The WMT core's whole chip reset message; the BT function and the BTIF it
 * talks through are gone, so, like stp_chrdev_bt.c's reader, tell the stack
 * with a Hardware Error event, on which it closes and reopens the device.
 */
static void btmtk_stp_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			     ENUM_WMTMSG_TYPE_T type, PVOID buf, UINT32 sz)
{
	struct btmtk_stp_dev *bt = READ_ONCE(g_bt);
	ENUM_WMTRSTMSG_TYPE_T msg;
	bool report;

	if (!bt || src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT ||
	    type != WMTMSG_TYPE_RESET || sz != sizeof(msg))
		return;

	memcpy(&msg, buf, sizeof(msg));

	switch (msg) {
	case WMTRSTMSG_RESET_START:
		bt_dev_warn(bt->hdev, "whole chip reset started");
		WRITE_ONCE(bt->resetting, true);
		break;
	case WMTRSTMSG_RESET_END:
	case WMTRSTMSG_RESET_END_FAIL:
		report = READ_ONCE(bt->powered) && msg == WMTRSTMSG_RESET_END;
		bt_dev_warn(bt->hdev, "whole chip reset %s",
			    msg == WMTRSTMSG_RESET_END ? "done" : "failed");
		WRITE_ONCE(bt->resetting, false);
		if (report)
			hci_reset_dev(bt->hdev);
		break;
	default:
		break;
	}
}

/* The caller holds pwr_lock */
static int btmtk_stp_power_on(struct btmtk_stp_dev *bt)
{
	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(bt->hdev, "WMT turn on BT failed");
		return -EIO;
	}

	if (mtk_wcn_stp_is_ready() == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(bt->hdev, "STP is not ready");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		return -EIO;
	}

	/* BT frames stay in STP's RX queue for us, not pushed to a callback */
	mtk_wcn_stp_set_bluez(MTK_WCN_BOOL_FALSE);

	/* This also flushes what is left in the BT RX queue */
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, btmtk_stp_rx_event_cb);
	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, btmtk_stp_tx_event_cb);

	bt->powered = true;
	return 0;
}

/* The caller holds pwr_lock */
static void btmtk_stp_power_off(struct btmtk_stp_dev *bt)
{
	if (!bt->powered)
		return;

	mtk_wcn_stp_register_tx_event_cb(BT_TASK_INDX, NULL);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	cancel_work_sync(&bt->rx_work);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE)
		bt_dev_err(bt->hdev, "WMT turn off BT failed");

	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->powered = false;
}

static int btmtk_stp_hci_open(struct hci_dev *hdev)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&bt->pwr_lock);

	if (bt->suspended) {
		/* The chip is off for the time of the suspend, see
		 * btmtk_stp_pm_notify()
		 */
		ret = -EBUSY;
	} else if (!bt->powered) {
		WRITE_ONCE(bt->resetting, false);
		ret = btmtk_stp_power_on(bt);
	}

	mutex_unlock(&bt->pwr_lock);
	return ret;
}

static int btmtk_stp_hci_close(struct hci_dev *hdev)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->pwr_lock);
	bt->reopen_after_resume = false;
	btmtk_stp_power_off(bt);
	mutex_unlock(&bt->pwr_lock);

	return 0;
}

static int btmtk_stp_tx(struct btmtk_stp_dev *bt, u8 *buf, unsigned int len)
{
	int tries, ret = 0;

	mutex_lock(&bt->tx_lock);

	for (tries = 0; tries < BTMTK_STP_TX_TRIES; tries++) {
		if (READ_ONCE(bt->resetting) || !READ_ONCE(bt->powered)) {
			ret = -EIO;
			break;
		}

		WRITE_ONCE(bt->tx_resumed, false);
		ret = mtk_wcn_stp_send_data(buf, len, BT_TASK_INDX);
		if (ret == len) {
			ret = 0;
			break;
		}
		if (ret != 0) {
			ret = -EIO;
			break;
		}

		/* No window space: wait for an ACK to open some, then retry */
		wait_event_timeout(bt->tx_wq, READ_ONCE(bt->tx_resumed),
				   msecs_to_jiffies(BTMTK_STP_TX_WAIT_MS));
		ret = -EAGAIN;
	}

	mutex_unlock(&bt->tx_lock);
	return ret;
}

static int btmtk_stp_hci_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btmtk_stp_dev *bt = hci_get_drvdata(hdev);
	int ret;

	if (skb->len + 1 > BTMTK_STP_MAX_FRAME)
		return -EMSGSIZE;

	/* STP carries H:4: the packet indicator is part of the BT payload. */
	ret = skb_cow_head(skb, 1);
	if (ret)
		return ret;
	*(u8 *)skb_push(skb, 1) = hci_skb_pkt_type(skb);

	ret = btmtk_stp_tx(bt, skb->data, skb->len);
	if (ret) {
		bt_dev_err(hdev, "STP send failed (%d)", ret);
		/* hci_send_frame() frees the skb on a failed send */
		return ret;
	}

	hdev->stat.byte_tx += skb->len - 1;
	kfree_skb(skb);
	return 0;
}

static int btmtk_stp_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	/* The parameter is the address in HCI's little endian order, as the
	 * vendor writes addr[5]..addr[0] of its big endian "addr".
	 */
	return __hci_cmd_sync_status(hdev, BTMTK_OP_SET_BDADDR, sizeof(*bdaddr),
				     bdaddr, HCI_INIT_TIMEOUT);
}

/* Amazon's bootloader adds the factory BT MAC to the device tree as the
 * 12 hex digits of /idme/bt_mac_addr/value, most significant byte first.
 */
static bool btmtk_stp_idme_bdaddr(bdaddr_t *bdaddr)
{
	struct device_node *np;
	const char *val;
	u8 be[6];
	int len = 0;
	bool ok = false;

	np = of_find_node_by_path(IDME_OF_BT_MAC_ADDR);
	if (!np)
		return false;

	val = of_get_property(np, "value", &len);
	if (val && len >= 12 && !hex2bin(be, val, sizeof(be)) &&
	    memchr_inv(be, 0, sizeof(be)))
		ok = true;

	of_node_put(np);

	if (ok)
		baswap(bdaddr, (bdaddr_t *)be);

	return ok;
}

/* The vendor's placeholder addresses are 00:00:46:xx:xx:01 (00:00:46:00:00:01
 * for consys chips); the chip answers one of those when it has no eFUSE one.
 */
static bool btmtk_stp_bdaddr_is_default(const bdaddr_t *bdaddr)
{
	return !bacmp(bdaddr, BDADDR_ANY) ||
	       (bdaddr->b[5] == 0x00 && bdaddr->b[4] == 0x00 &&
		bdaddr->b[3] == 0x46);
}

static int btmtk_stp_hci_setup(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	bdaddr_t bdaddr;
	int err;

	if (btmtk_stp_idme_bdaddr(&bdaddr)) {
		err = btmtk_stp_set_bdaddr(hdev, &bdaddr);
		if (err) {
			bt_dev_err(hdev, "Set BD_ADDR failed (%d)", err);
			return err;
		}
		bt_dev_info(hdev, "BD address %pMR from IDME", &bdaddr);
	} else {
		/* No factory address: what the chip has, unless that is a
		 * placeholder, which has to be set from user space.
		 */
		skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL,
				     HCI_INIT_TIMEOUT);
		if (IS_ERR(skb)) {
			bt_dev_err(hdev, "Read BD_ADDR failed (%ld)", PTR_ERR(skb));
			return PTR_ERR(skb);
		}
		if (skb->len == sizeof(struct hci_rp_read_bd_addr)) {
			bacpy(&bdaddr, &((struct hci_rp_read_bd_addr *)skb->data)->bdaddr);
			if (btmtk_stp_bdaddr_is_default(&bdaddr)) {
				bt_dev_warn(hdev, "BD address %pMR is a placeholder, set one",
					    &bdaddr);
				hci_set_quirk(hdev, HCI_QUIRK_INVALID_BDADDR);
			}
		}
		kfree_skb(skb);
	}

	err = __hci_cmd_sync_status(hdev, BTMTK_OP_SET_RADIO,
				    sizeof(btmtk_radio_default),
				    btmtk_radio_default, HCI_INIT_TIMEOUT);
	if (err) {
		bt_dev_err(hdev, "Set Radio failed (%d)", err);
		return err;
	}

	err = __hci_cmd_sync_status(hdev, BTMTK_OP_SET_TX_PWR_OFS,
				    sizeof(btmtk_txpwr_ofs_default),
				    btmtk_txpwr_ofs_default, HCI_INIT_TIMEOUT);
	if (err) {
		bt_dev_err(hdev, "Set TX Power Offset failed (%d)", err);
		return err;
	}

	err = __hci_cmd_sync_status(hdev, BTMTK_OP_SET_SLEEP,
				    sizeof(btmtk_sleep_default),
				    btmtk_sleep_default, HCI_INIT_TIMEOUT);
	if (err) {
		bt_dev_err(hdev, "Set Sleep Timeout failed (%d)", err);
		return err;
	}

	return 0;
}

/*
 * A system suspend takes the power of the SoC's infra domain, and with it the
 * state of the BTIF and of the DMA channels the chip is talked to through; an
 * open BTIF refuses the suspend for that (see wmt_cdev_wifi.c, which switches
 * WiFi off for the same reason). So the BT function, and with it the chip if
 * WiFi is off too, is switched off for the time of a suspend, and the stack
 * reopens the device after it with the Hardware Error event, as for a whole
 * chip reset. The core's own suspend notifier (scanning off, wake setup) would
 * only talk to a chip that is about to go; HCI_QUIRK_NO_SUSPEND_NOTIFIER is
 * set for that.
 */
static int btmtk_stp_pm_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct btmtk_stp_dev *bt = container_of(nb, struct btmtk_stp_dev, pm_nb);
	bool reopen;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&bt->pwr_lock);
		bt->suspended = true;
		bt->reopen_after_resume = bt->powered;
		btmtk_stp_power_off(bt);
		mutex_unlock(&bt->pwr_lock);
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&bt->pwr_lock);
		bt->suspended = false;
		reopen = bt->reopen_after_resume;
		bt->reopen_after_resume = false;
		mutex_unlock(&bt->pwr_lock);
		if (reopen)
			hci_reset_dev(bt->hdev);
		break;
	}

	return NOTIFY_OK;
}

static int __init btmtk_stp_init(void)
{
	struct btmtk_stp_dev *bt;
	struct hci_dev *hdev;
	int ret;

	bt = kzalloc_obj(*bt);
	if (!bt)
		return -ENOMEM;

	bt->rx_buf = kmalloc(BTMTK_STP_RX_CHUNK, GFP_KERNEL);
	if (!bt->rx_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	mutex_init(&bt->pwr_lock);
	mutex_init(&bt->tx_lock);
	init_waitqueue_head(&bt->tx_wq);
	INIT_WORK(&bt->rx_work, btmtk_stp_rx_work);
	bt->pm_nb.notifier_call = btmtk_stp_pm_notify;

	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto err_free;
	}

	bt->hdev = hdev;
	hci_set_drvdata(hdev, bt);

	/* No tty in this path, but BTIF is the UART-like transport of an
	 * external UART combo chip's integrated twin; the closest honest
	 * classification among hci.h's bus types, as for btmtkuart.
	 */
	hdev->bus = HCI_UART;
	hdev->open = btmtk_stp_hci_open;
	hdev->close = btmtk_stp_hci_close;
	hdev->send = btmtk_stp_hci_send;
	hdev->setup = btmtk_stp_hci_setup;
	hdev->set_bdaddr = btmtk_stp_set_bdaddr;

	/* The function is switched off on close, which resets the chip: its
	 * vendor configuration is gone and has to be sent on each open.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	hci_set_quirk(hdev, HCI_QUIRK_NO_SUSPEND_NOTIFIER);

	WRITE_ONCE(g_bt, bt);

	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, btmtk_stp_rst_cb);

	ret = register_pm_notifier(&bt->pm_nb);
	if (ret)
		goto err_msgcb;

	ret = hci_register_dev(hdev);
	if (ret < 0) {
		pr_err("btmtk_stp: hci_register_dev failed (%d)\n", ret);
		goto err_pm;
	}

	bt_dev_info(hdev, "MediaTek STP Bluetooth registered");
	return 0;

err_pm:
	unregister_pm_notifier(&bt->pm_nb);
err_msgcb:
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	WRITE_ONCE(g_bt, NULL);
	hci_free_dev(hdev);
err_free:
	kfree(bt->rx_buf);
	kfree(bt);
	return ret;
}

static void __exit btmtk_stp_exit(void)
{
	struct btmtk_stp_dev *bt = g_bt;

	unregister_pm_notifier(&bt->pm_nb);

	/* Closes the device, which switches the BT function off if it is on */
	hci_unregister_dev(bt->hdev);

	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	WRITE_ONCE(g_bt, NULL);

	hci_free_dev(bt->hdev);
	kfree(bt->rx_buf);
	kfree(bt);
}

module_init(btmtk_stp_init);
module_exit(btmtk_stp_exit);

MODULE_DESCRIPTION("MediaTek STP Bluetooth HCI driver for the connectivity subsystem");
MODULE_LICENSE("GPL");
